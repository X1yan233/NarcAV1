/*
 * LDVFrameCache.c — 有字节预算上限的帧缓存（Core 层内部模块）
 *
 * 线程模型
 * --------
 * 生产者（解码线程）和消费者（显示线程）是两个线程，所以缓存自带
 * pthread 互斥锁 + 条件变量，并对外提供两个"等待"原语：
 *   LDVFrameCacheWaitForSpace()  生产者：缓存满时等空间
 *   LDVFrameCacheWaitForFrame()  消费者：缓存空时等帧
 * 这样调用方不需要自己再套一层锁。
 *
 * 借用语义
 * --------
 * Peek 借出队首槽位（最旧的一帧 = 下一个该显示的），Pop 才真正释放。
 * 借出期间：
 *   - 不能重复 Peek（返回 0，调用方应先用完再 Pop）
 *   - DropOldest 不会动这一帧（它正在被显示）
 * 这把"用完了没释放"从纪律变成约束：忘了 Pop 就拿不到下一帧。
 */

#include "LDVFrameCache.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* 单帧内存估算                                                              */
/* ------------------------------------------------------------------------ */

/*
 * 估算一帧占多少内存（像素载荷 × 1.2 的策略系数）。
 *
 * 1.2 是为了让预算数字更接近真实 RSS 影响（分配器对齐与边界 padding）。
 * 它只是策略数字，不要求精确。
 */
#define LDV_FRAME_BYTES_NUM 6
#define LDV_FRAME_BYTES_DEN 5      /* 6/5 = 1.2 */

/* ------------------------------------------------------------------------ */
/* 结构                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * 一个槽位。
 *
 * ★ 为什么存 `view` 而不是"现算"
 * -----------------------------
 * Peek 是消费者线程的热路径。改成**入队时就把 view 填好**：
 * Peek 只是一次结构体拷贝，没有分支，而且"这一帧是什么"只有一处定义
 * （Push 的时候），不会两边漂移。
 *
 * v8.8 起缓存只有自有缓冲一种帧形态（与具体解码器解耦，
 * 见 LDVFrameCache.h 头注释），释放一律 free(own)。
 */
typedef struct {
    LDVFrameView view;       /* 规范描述；Peek 直接返回它 */
    uint8_t     *own;        /* 自有缓冲基址（一次 malloc 的整块） */
    size_t       bytes;
    int          used;
    int          borrowed;    /* 已被 Peek 借出、尚未 Pop */
    int64_t      index;       /* 解码输出序号 */
} LDVFrameSlot;

#define LDV_CACHE_DEFAULT_SLOTS 512

struct LDVFrameCache {
    LDVFrameSlot  *slots;
    int            capacity;
    int            head;      /* 最旧帧的槽位下标 */
    int            count;

    size_t         byteBudget;
    size_t         bytes;
    int            maxFrames;

    /* 统计 */
    int64_t        totalPushed;
    int64_t        totalPopped;
    int64_t        totalDropped;
    int64_t        fullHits;
    int64_t        nextIndex;

    /* 同步 */
    pthread_mutex_t lock;
    pthread_cond_t  condSpace;   /* 生产者等空间 */
    pthread_cond_t  condFrame;   /* 消费者等帧 */
    int             aborted;     /* 置 1 后所有等待立即返回 */
};

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

LDVFrameCache *LDVFrameCacheCreate(size_t byteBudget, int maxFrames) {
    LDVFrameCache *c = (LDVFrameCache *)calloc(1, sizeof(LDVFrameCache));
    if (!c) return NULL;

    if (maxFrames > 0) {
        c->capacity = maxFrames;
    } else {
        /*
         * 不按帧数限制（maxFrames <= 0）：容量从**字节预算**倒推，
         * 让环形槽位永远装得下预算内的帧 —— 窗口贴合缩小后单帧变小
         * （960x540 → ~626x352，字节降到 44%），固定槽数会在预算
         * 还没填满时就绕回。按最小合理帧（32KB）估算槽数。
         */
        c->capacity = (int)(byteBudget / 32768) + 16;
        if (c->capacity < LDV_CACHE_DEFAULT_SLOTS)
            c->capacity = LDV_CACHE_DEFAULT_SLOTS;
    }
    if (c->capacity > 4096) c->capacity = 4096;      /* 防御性上限 */
    c->byteBudget = byteBudget;
    c->maxFrames  = (maxFrames > 0) ? maxFrames : 0;
    c->head = 0;

    c->slots = (LDVFrameSlot *)calloc((size_t)c->capacity, sizeof(LDVFrameSlot));
    if (!c->slots) { free(c); return NULL; }

    if (pthread_mutex_init(&c->lock, NULL) != 0) {
        free(c->slots); free(c); return NULL;
    }
    if (pthread_cond_init(&c->condSpace, NULL) != 0) {
        pthread_mutex_destroy(&c->lock); free(c->slots); free(c); return NULL;
    }
    if (pthread_cond_init(&c->condFrame, NULL) != 0) {
        pthread_cond_destroy(&c->condSpace);
        pthread_mutex_destroy(&c->lock); free(c->slots); free(c); return NULL;
    }
    return c;
}

/* 释放单个槽位持有的帧（调用方必须已持锁） */
static void ldv_slot_release(LDVFrameSlot *s) {
    if (s->used) {
        free(s->own);
        s->own = NULL;
        memset(&s->view, 0, sizeof(s->view));
        s->used = 0;
        s->borrowed = 0;
        s->bytes = 0;
    }
}

void LDVFrameCacheDestroy(LDVFrameCache **cache) {
    LDVFrameCache *c;
    int i;
    if (!cache || !*cache) return;
    c = *cache;

    pthread_mutex_lock(&c->lock);
    c->aborted = 1;
    pthread_cond_broadcast(&c->condSpace);
    pthread_cond_broadcast(&c->condFrame);
    for (i = 0; i < c->capacity; i++) ldv_slot_release(&c->slots[i]);
    pthread_mutex_unlock(&c->lock);

    pthread_cond_destroy(&c->condFrame);
    pthread_cond_destroy(&c->condSpace);
    pthread_mutex_destroy(&c->lock);
    free(c->slots);
    free(c);
    *cache = NULL;
}

/* ------------------------------------------------------------------------ */
/* 内部：槽位寻址                                                            */
/* ------------------------------------------------------------------------ */

static int ldv_slot_index(const LDVFrameCache *c, int pos) {
    return (c->head + pos) % c->capacity;
}

/*
 * 「缓存满了」的**唯一**判定。
 *
 * ★★ 这里曾经有两个不一致的判定，导致一个很隐蔽的故障 ★★
 *
 * 原来：
 *     LDVFrameCacheIsFull()          → c->bytes >= byteBudget
 *     ldv_cache_is_full_locked(c, n) → c->bytes + n > byteBudget
 *
 * 生产者用前者决定「要不要等空间」，PushPicture 用后者决定「收不收」。
 * 两者在 bytes 落进 (budget - 帧大小, budget] 这个窗口时**结论相反**。
 *
 * 实测数字（512x384 素材 / 24MB 预算 / 单帧计入 353,894 B）：
 *     71 帧时 bytes = 25,126,474
 *     IsFull()       → 25,126,474 >= 25,165,824 ？ 否 → 认为「没满」
 *     PushPicture()  → +353,894 = 25,480,368 > 预算 ？ 是 → 拒收
 *
 * 后果是连锁的：
 *   生产者以为有空间 → **从不等 WaitForSpace**
 *   → 每轮从 dav1d 取一帧 → PushPicture 拒收 → 当场 unref 丢掉
 *   → 解出来的帧几乎全被扔掉，frameCount 不涨（界面显示「解码暂停」）
 *   → 而显示端仍按 30fps 消耗缓存 → 缓存一路掉到 0 → 冻结
 *
 * 修法：**所有地方用同一个判定** —— 不再看 incoming。
 * 代价是缓存可能超预算「一帧」（byteBudget 本来就是软上限，文档写明了），
 * 换来的是一致性：IsFull / WaitForSpace / PushPicture 三处结论永远相同。
 */
static int ldv_cache_is_full_locked(const LDVFrameCache *c) {
    if (c->count >= c->capacity) return 1;
    if (c->maxFrames > 0 && c->count >= c->maxFrames) return 1;
    if (c->byteBudget > 0 && c->bytes >= c->byteBudget) return 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 生产者                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * 占一个槽位。成功返回槽位下标，失败返回 -1（调用方负责解锁）。
 * 三种失败：已中止 / 缓存满 / 槽位被占（防御性）。
 */
static int ldv_reserve_slot(LDVFrameCache *c) {
    int slot;

    if (c->aborted) return -1;

    if (ldv_cache_is_full_locked(c)) {
        c->fullHits++;
        return -1;
    }

    slot = ldv_slot_index(c, c->count);
    if (c->slots[slot].used) {
        c->fullHits++;
        return -1;
    }
    return slot;
}

/*
 * 按帧视图估算字节数（自有缓冲路径的兜底）。
 *
 * ★ 这里踩过一个坑：一开始写成
 *       w * h * ((layout == I420) ? 3u / 2u : ...)
 *   —— 而 `3u / 2u` 在 C 里是**整数除法**，等于 1 不是 1.5。
 *   于是 I420 被低估 33%，字节预算变得过于乐观：
 *   缓存实际能占的内存比预算多一半，而且这个偏差**不会报错**，
 *   只会让"预算 36MB"变成实际 54MB —— 在 256MB 的老设备上很危险。
 *
 *   现在用与 ldv_frame_bytes 完全相同的算法（samples * bps * 1.2），
 *   两条路径的估算口径一致，不会一个说 A 一个说 B。
 */
static size_t ldv_estimate_frame_bytes(const LDVFrameView *v) {
    const size_t w   = (size_t)v->width;
    const size_t h   = (size_t)v->height;
    const size_t bps = (v->bpc > 8) ? 2u : 1u;
    size_t samples;

    switch (v->layout) {
    case LDV_PIXEL_LAYOUT_I400:
        samples = w * h;
        break;
    case LDV_PIXEL_LAYOUT_I422:
        samples = w * h + 2u * ((w + 1u) / 2u) * h;
        break;
    case LDV_PIXEL_LAYOUT_I444:
        samples = w * h * 3u;
        break;
    case LDV_PIXEL_LAYOUT_I420:
    default:
        samples = w * h + 2u * ((w + 1u) / 2u) * ((h + 1u) / 2u);
        break;
    }

    return samples * bps * LDV_FRAME_BYTES_NUM / LDV_FRAME_BYTES_DEN;
}

LDVResult LDVFrameCachePushOwned(LDVFrameCache *c,
                                 const LDVFrameView *view,
                                 void *owner, size_t ownerBytes) {
    size_t bytes;
    int    slot;

    if (!c || !view) return LDV_ERR_INVALID_ARG;
    if (view->width <= 0 || view->height <= 0) return LDV_ERR_INVALID_ARG;

    /*
     * ★ 必须给 owner。
     *
     * 一开始允许传 NULL（语义是"内存不归缓存管，调用方自己保活"），
     * 但那会制造一个**用后释放**的隐患：Peek 把 view 借给消费者之后，
     * 调用方随时可能把这块内存释放掉，而缓存这边完全不知道。
     * 这类 bug 只在特定时序下崩，极难复现。
     *
     * 现在直接拒绝，状态空间小一半：槽位只有自有缓冲一种形态。
     */
    if (!owner) return LDV_ERR_INVALID_ARG;

    /*
     * 字节预算怎么算：调用方给了就用它（自有缓冲一定是"一整块连续内存"，
     * 调用方给的数字最准），否则按平面尺寸估。
     */
    bytes = (ownerBytes > 0) ? ownerBytes : ldv_estimate_frame_bytes(view);

    pthread_mutex_lock(&c->lock);

    slot = ldv_reserve_slot(c);
    if (slot < 0) {
        LDVResult r = c->aborted ? LDV_ERR_EOF : LDV_ERR_NEED_INPUT;
        pthread_mutex_unlock(&c->lock);
        return r;
    }

    c->slots[slot].view     = *view;
    c->slots[slot].own      = (uint8_t *)owner;
    c->slots[slot].used     = 1;
    c->slots[slot].borrowed = 0;
    c->slots[slot].bytes    = bytes;
    c->slots[slot].index    = c->nextIndex++;
    c->slots[slot].view.index = (int)c->slots[slot].index;

    c->bytes += bytes;
    c->count++;
    c->totalPushed++;

    pthread_cond_signal(&c->condFrame);
    pthread_mutex_unlock(&c->lock);
    return LDV_OK;
}

int LDVFrameCacheIsFull(const LDVFrameCache *c) {
    int r;
    if (!c) return 1;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = (c->count >= c->capacity) ||
        (c->maxFrames > 0 && c->count >= c->maxFrames) ||
        (c->byteBudget > 0 && c->bytes >= c->byteBudget);
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

int LDVFrameCacheDropOldest(LDVFrameCache *c) {
    int dropped = 0;
    if (!c) return 0;

    pthread_mutex_lock(&c->lock);
    if (c->count > 0 && !c->slots[c->head].borrowed) {
        size_t bytes = c->slots[c->head].bytes;
        ldv_slot_release(&c->slots[c->head]);
        c->head = (c->head + 1) % c->capacity;
        c->count--;
        c->bytes -= bytes;
        c->totalDropped++;
        dropped = 1;
        pthread_cond_signal(&c->condSpace);
    }
    pthread_mutex_unlock(&c->lock);
    return dropped;
}

int LDVFrameCacheDropWiderThan(LDVFrameCache *c, int maxWidth) {
    int dropped = 0, kept = 0, i;

    if (!c || maxWidth <= 0) return 0;

    pthread_mutex_lock(&c->lock);

    /* 第一遍：释放超宽帧（借用中的队首不動 —— 它正在被显示） */
    for (i = 0; i < c->count; i++) {
        LDVFrameSlot *s = &c->slots[ldv_slot_index(c, i)];
        if (s->used && !s->borrowed && s->view.width > maxWidth) {
            c->bytes -= s->bytes;
            c->totalDropped++;
            dropped++;
            ldv_slot_release(s);
        }
    }

    /*
     * 第二遍：把保留的槽位向 head 压实成连续区段。
     * 环形队列里"抽掉中间元素"必须重排，否则 head/count 语义就断了。
     * 保留槽位的相对顺序不变（FIFO 语义保持），借用中的队首天然留在 0 位。
     */
    for (i = 0; i < c->count; i++) {
        LDVFrameSlot *s = &c->slots[ldv_slot_index(c, i)];
        if (s->used) {
            if (i != kept) {
                LDVFrameSlot *d = &c->slots[ldv_slot_index(c, kept)];
                *d = *s;
                memset(s, 0, sizeof(*s));
            }
            kept++;
        }
    }
    c->count = kept;

    if (dropped > 0) {
        /* 可能正好腾出了生产者等待的空间 */
        pthread_cond_broadcast(&c->condSpace);
    }
    pthread_mutex_unlock(&c->lock);
    return dropped;
}

/* ------------------------------------------------------------------------ */
/* 消费者                                                                    */
/* ------------------------------------------------------------------------ */

int LDVFrameCachePeek(LDVFrameCache *c, LDVFrameView *out) {
    if (!c || !out) return 0;

    pthread_mutex_lock(&c->lock);

    if (c->count <= 0) {
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    if (c->slots[c->head].borrowed) {
        /* 上一帧还没 Pop —— 这是借用语义刻意制造的可见错误 */
        pthread_mutex_unlock(&c->lock);
        return 0;
    }

    /*
     * 只做一次结构体拷贝 —— 描述在 Push 的时候就已经填好了，
     * 所以这里对所有帧一视同仁：只做结构体拷贝，没有分支。
     */
    *out = c->slots[c->head].view;
    c->slots[c->head].borrowed = 1;

    pthread_mutex_unlock(&c->lock);
    return 1;
}

int LDVFrameCachePeekTimestamp(LDVFrameCache *c, int64_t *outTimestamp) {
    int ok = 0;
    if (!c || !outTimestamp) return 0;

    pthread_mutex_lock(&c->lock);
    if (c->count > 0) {
        *outTimestamp = c->slots[c->head].view.timestamp;
        ok = 1;
    }
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int LDVFrameCachePeekIndex(LDVFrameCache *c, int64_t *outIndex) {
    int ok = 0;
    if (!c || !outIndex) return 0;

    pthread_mutex_lock(&c->lock);
    if (c->count > 0) {
        *outIndex = c->slots[c->head].index;
        ok = 1;
    }
    pthread_mutex_unlock(&c->lock);
    return ok;
}

void LDVFrameCachePop(LDVFrameCache *c) {
    if (!c) return;

    pthread_mutex_lock(&c->lock);
    if (c->count > 0 && c->slots[c->head].borrowed) {
        size_t bytes = c->slots[c->head].bytes;
        ldv_slot_release(&c->slots[c->head]);
        c->head = (c->head + 1) % c->capacity;
        c->count--;
        c->bytes -= bytes;
        c->totalPopped++;
        pthread_cond_signal(&c->condSpace);
    }
    pthread_mutex_unlock(&c->lock);
}

/* ------------------------------------------------------------------------ */
/* 等待原语                                                                  */
/* ------------------------------------------------------------------------ */

static void ldv_make_deadline(struct timespec *ts, int timeoutMs) {
    struct timeval now;
    gettimeofday(&now, NULL);
    ts->tv_sec  = now.tv_sec + timeoutMs / 1000;
    ts->tv_nsec = (long)(now.tv_usec * 1000) + (long)(timeoutMs % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec += 1; ts->tv_nsec -= 1000000000L; }
}

int LDVFrameCacheWaitForSpace(LDVFrameCache *c, int timeoutMs) {
    int ok;
    if (!c) return 0;

    pthread_mutex_lock(&c->lock);
    if (c->aborted) { pthread_mutex_unlock(&c->lock); return 0; }

    if (!ldv_cache_is_full_locked(c)) {
        pthread_mutex_unlock(&c->lock);
        return 1;
    }

    if (timeoutMs > 0) {
        struct timespec ts;
        ldv_make_deadline(&ts, timeoutMs);
        while (!c->aborted && ldv_cache_is_full_locked(c)) {
            if (pthread_cond_timedwait(&c->condSpace, &c->lock, &ts) == ETIMEDOUT) break;
        }
    } else {
        while (!c->aborted && ldv_cache_is_full_locked(c)) {
            pthread_cond_wait(&c->condSpace, &c->lock);
        }
    }

    ok = !c->aborted && !ldv_cache_is_full_locked(c);
    pthread_mutex_unlock(&c->lock);
    return ok;
}

int LDVFrameCacheWaitForFrame(LDVFrameCache *c, int timeoutMs) {
    int ok;
    if (!c) return 0;

    pthread_mutex_lock(&c->lock);
    if (c->aborted) { pthread_mutex_unlock(&c->lock); return 0; }

    if (c->count > 0) { pthread_mutex_unlock(&c->lock); return 1; }

    if (timeoutMs > 0) {
        struct timespec ts;
        ldv_make_deadline(&ts, timeoutMs);
        while (!c->aborted && c->count == 0) {
            if (pthread_cond_timedwait(&c->condFrame, &c->lock, &ts) == ETIMEDOUT) break;
        }
    } else {
        while (!c->aborted && c->count == 0) {
            pthread_cond_wait(&c->condFrame, &c->lock);
        }
    }

    ok = !c->aborted && c->count > 0;
    pthread_mutex_unlock(&c->lock);
    return ok;
}

void LDVFrameCacheBroadcast(LDVFrameCache *c) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    pthread_cond_broadcast(&c->condSpace);
    pthread_cond_broadcast(&c->condFrame);
    pthread_mutex_unlock(&c->lock);
}

/* ------------------------------------------------------------------------ */
/* 状态                                                                      */
/* ------------------------------------------------------------------------ */

int LDVFrameCacheCount(const LDVFrameCache *c) {
    int r;
    if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->count;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

size_t LDVFrameCacheBytes(const LDVFrameCache *c) {
    size_t r;
    if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->bytes;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

size_t LDVFrameCacheByteBudget(const LDVFrameCache *c) {
    return c ? c->byteBudget : 0;
}

int LDVFrameCacheMaxFrames(const LDVFrameCache *c) {
    return c ? c->maxFrames : 0;
}

float LDVFrameCacheProgress(const LDVFrameCache *c) {
    float r = 0.0f;
    if (!c) return 0.0f;

    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    {
        float byBytes = 0.0f, byFrames = 0.0f;
        int has = 0;

        if (c->byteBudget > 0) {
            byBytes = (float)c->bytes / (float)c->byteBudget;
            has = 1;
        }
        if (c->maxFrames > 0) {
            byFrames = (float)c->count / (float)c->maxFrames;
            has = 1;
        }
        r = has ? (byBytes > byFrames ? byBytes : byFrames) : 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < 0.0f) r = 0.0f;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

int64_t LDVFrameCacheTotalPushed(const LDVFrameCache *c) {
    int64_t r; if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->totalPushed;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

int64_t LDVFrameCacheTotalPopped(const LDVFrameCache *c) {
    int64_t r; if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->totalPopped;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

int64_t LDVFrameCacheTotalDropped(const LDVFrameCache *c) {
    int64_t r; if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->totalDropped;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

int64_t LDVFrameCacheFullHits(const LDVFrameCache *c) {
    int64_t r; if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&c->lock);
    r = c->fullHits;
    pthread_mutex_unlock((pthread_mutex_t *)&c->lock);
    return r;
}

void LDVFrameCacheClear(LDVFrameCache *c) {
    int i;
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    for (i = 0; i < c->capacity; i++) ldv_slot_release(&c->slots[i]);
    c->head = 0;
    c->count = 0;
    c->bytes = 0;

    /*
     * ★ nextIndex 也要归零。
     *
     * 它表示"这是解码输出的第几帧"，而显示节拍算的是 `帧序号 / 帧率`
     * —— 也就是**媒体时间**。Clear 总是伴随"解码器重建、从序列头重来"
     * （见 LDVDecodeSessionSetDspMode / Stop），此时流的位置回到了 0，
     * 序号必须跟着回到 0。
     *
     * 不归零的后果很隐蔽：切一次 DSP 模式后，新解出来的第 0 帧会拿到
     * 比如 122 号，于是媒体时间凭空跳到 4.07 秒 —— 播放看着正常
     * （节拍是自洽的），但音画同步会整体偏掉一个固定量，
     * 而且偏多少取决于"切之前解了多少帧"，每次都不一样。
     *
     * totalPushed / totalPopped / totalDropped 是**累计诊断计数**，
     * 不清零 —— 它们的语义是"这个缓存对象一生中的总数"。
     */
    c->nextIndex = 0;

    pthread_cond_broadcast(&c->condSpace);
    pthread_mutex_unlock(&c->lock);
}
