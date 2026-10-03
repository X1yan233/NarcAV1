/*
 * LDVDecodeSession.c — 解码会话：后台解码线程 + 解码缓存
 *
 * 设计说明见 LDVDecodeSession.h。这里只强调两点实现上的取舍：
 *
 * 1) 会话里没有"暂停"
 *    解码线程只有一个循环：读样本 → 喂 → 取帧 → 进缓存。缓存满了就等
 *    （或丢）。显示端暂停时不再 Pop，缓存迅速填满，解码线程自然停在上限。
 *    不需要任何暂停标志，也就不会出现标志与线程状态不同步的问题。
 *
 * 2) 所有等待都带超时
 *    退出时不能被 pthread_cond_wait 卡死，所以一律用 timedwait + 重查
 *    stop 标志。100 ms 的粒度对压测没有影响，但能让 Stop 立刻生效。
 */

#include "LDVDecodeSession.h"

#include "LDVCoreDecoder.h"
#include "LDVDownscale.h"
#include "LDVFrameCache.h"
#include "LDVMP4Source.h"
#include "LDVPlatformTuning.h"
#include "LDVDsp.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

/* 等待超时粒度（毫秒）。只影响退出响应速度，不影响解码。 */
#define LDV_WAIT_SLICE_MS 100

/*
 * 最近一次 LDVDecodeSessionCreate 失败在**哪一步**。
 *
 * 为什么要有它
 * ------------
 * Create 的失败原因分三类，但对外都只是返回 NULL：
 *   1. 打不开 MP4（文件缺失 / 不是可解析的 ISOBMFF / 没有 av01 视频轨）
 *   2. 解码缓存分配失败（内存不足）
 *   3. dav1d 初始化失败（线程数或内存）
 * 这三类的排查方向完全不同，UI 只显示一句"打开失败"等于什么都没说。
 * 给一个阶段字符串，就能直接定位到该看哪一段日志。
 *
 * 具体原因（哪个 box 不对）由 LDVMP4Source 打到 stderr，前缀 "LDVMP4:"。
 * Create 只在启动时单线程调用，不存在竞争，所以用静态变量即可。
 */
static char ldv_createError[192];

const char *LDVDecodeSessionCreateError(void) {
    return ldv_createError[0] ? ldv_createError : NULL;
}

struct LDVDecodeSession {
    LDVMP4Source   *source;
    LDVCoreDecoder *core;
    LDVFrameCache  *cache;

    pthread_t       thread;
    int             threadValid;
    volatile int    stop;
    volatile int    running;
    volatile int    eof;

    int64_t         nextSample;
    int64_t         fedSamples;
    int64_t         dropped;
    int64_t         loopCount;

    volatile int    dropWhenFull;
    volatile int    looping;

    char            lastError[256];

    /*
     * 解码缓存的缩放（可选，见 LDVDecodeSessionSetOutputSize）。
     *
     * scaler 只在 Start 之前由 SetOutputSize 建好，Start 之后只读 ——
     * 所以解码线程读它不需要加锁。这一条很重要：如果要支持运行中改，
     * 就必须在解码线程和显示线程之间做同步，而缩放器的权重表是
     * 几百 KB 的结构，加锁会让热路径变复杂且容易漏。
     */
    LDVDownscaler  *scaler;
    int             outW, outH;
    int             scaleActive;

    /*
     * 帧出口分流（v8.20）：非空时解码帧不进缓存，改为复制后回调。
     * 与 scaler 互斥（SetFrameSink 设置时会清掉 scaler）—— 一次只能
     * 走一条输出管线。只在 Start 前设置，Start 后解码线程只读。
     */
    LDVFrameSinkFn  sink;
    void           *sinkUD;

    /* 解码吞吐窗口统计（仅压测诊断用，不参与播放决策） */
    double          decodeFPS;
    int64_t         windowStartUs;
    int64_t         windowFrames;
};

/* ------------------------------------------------------------------------ */
/* 时间工具                                                                  */
/* ------------------------------------------------------------------------ */

static int64_t ldv_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

/*
 * 记录一条错误。只保留**第一条** —— 后面的多半是同一原因的重复噪音，
 * 覆盖掉第一条反而丢失了根因。
 */
static void ldv_set_error(LDVDecodeSession *s, const char *msg) {
    if (!s || !msg) return;
    if (s->lastError[0] == '\0') {
        snprintf(s->lastError, sizeof(s->lastError), "%s", msg);
    }
}

/* ------------------------------------------------------------------------ */
/* 解码线程                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * sink 模式的取帧（v8.20）：PullFrame 借用解码器内存 → 复制交 sink →
 * 归还引用。复制在解码线程同步完成 —— sink 的实现约定是"立刻返回"，
 * 慢操作（GL 转换）必须发生在它自己的线程里。
 *
 * 复用 PullFrame 而不是自己 get_picture：Dav1dPicture → LDVFrameView
 * 的映射、错误映射、frameCount 自增都已做好，重写一份必然漂移
 * （与 PullIntoCacheScaled 同样的理由）。
 */
static LDVResult ldv_pull_into_sink(LDVDecodeSession *s) {
    LDVFrameView view;
    int          r;

    r = LDVCoreDecoderPullFrame(s->core, &view);
    if (r != LDV_OK) return (LDVResult)r;

    s->sink(s->sinkUD, &view);
    LDVCoreDecoderReleaseFrame(s->core, &view);
    return LDV_OK;
}

/*
 * 把解码器里所有就绪的帧取进缓存。
 *
 * 缓存满时会**等待**（除非是丢帧模式 —— 那种情况下调用方已经先丢过了）。
 * 返回后 s->stop 可能已被置位，调用方必须重查。
 */
static void ldv_drain_to_cache(LDVDecodeSession *s) {
    for (;;) {
        LDVResult r;

        if (s->stop) return;

        if (LDVFrameCacheIsFull(s->cache)) {
            if (s->dropWhenFull) {
                /* 丢最旧的一帧腾地方，保持 CPU 跑满 */
                if (LDVFrameCacheDropOldest(s->cache) == 0) {
                    /* 唯一的帧正被显示端借出，只能等一会儿 */
                    LDVFrameCacheWaitForSpace(s->cache, LDV_WAIT_SLICE_MS);
                }
                continue;
            }
            /* 正常播放：等显示端腾空间。超时后重查 stop。 */
            LDVFrameCacheWaitForSpace(s->cache, LDV_WAIT_SLICE_MS);
            continue;
        }

        /*
         * 帧出口分流（v8.20）：
         *   - sink 模式（GPU 异步转换）：帧不进缓存，复制后交给 sink，
         *     由转换器把缩小产物推进缓存。背压仍由显示缓存水位驱动
         *     （ldv_process_one_sample 的满判定），所以暂停语义不变。
         *   - 常规模式：走 Scaled 变体，s->scaler 为 NULL 时它内部直接
         *     转调 LDVCoreDecoderPullIntoCache，与原来完全等价。
         */
        if (s->sink) {
            r = ldv_pull_into_sink(s);
        } else {
            r = LDVCoreDecoderPullIntoCacheScaled(s->core, s->cache, s->scaler);
        }
        if (r == LDV_OK) {
            s->windowFrames++;
            continue;
        }
        if (r == LDV_ERR_NEED_INPUT) return;      /* 解码器暂时没有帧 */
        if (r == LDV_ERR_EOF)       return;
        /* 其它：解码错误，记一条并继续（不因为一帧坏掉就整体失败） */
        ldv_set_error(s, "解码器取帧失败");
        return;
    }
}

/*
 * 处理一个样本。返回 1 = 继续，0 = 结束（EOF 或致命错误）。
 */
static int ldv_process_one_sample(LDVDecodeSession *s) {
    uint8_t  *data = NULL;
    size_t    size = 0;
    int64_t   pts  = 0;
    LDVResult r;

    /* 缓存满 → 按策略处理 */
    if (LDVFrameCacheIsFull(s->cache)) {
        if (s->dropWhenFull) {
            s->dropped += LDVFrameCacheDropOldest(s->cache);
        } else {
            LDVFrameCacheWaitForSpace(s->cache, LDV_WAIT_SLICE_MS);
            return 1;                       /* 回头再试 */
        }
    }

    r = LDVMP4SourceReadSample(s->source, s->nextSample, &data, &size, &pts);
    if (r == LDV_ERR_EOF) {
        if (s->looping && !s->stop) {
            s->nextSample = 0;
            s->loopCount++;
            return 1;
        }
        s->eof = 1;
        return 0;
    }
    if (r != LDV_OK) {
        ldv_set_error(s, "读取样本失败（码流损坏或文件被截断）");
        s->eof = 1;
        return 0;
    }

    s->nextSample++;
    s->fedSamples++;

    /*
     * 送包。与 dav1d 官方示例一致：EAGAIN 时必须先把帧取干净再继续送。
     * 这里不重试同一个包（取干净后下一轮循环会重新读同一个 index 吗？不会），
     * 所以要在同一轮里把包送出去。
     */
    for (;;) {
        int fr = LDVCoreDecoderFeed(s->core, data, size, pts);
        if (fr == LDV_OK) break;

        if (fr == LDV_ERR_NEED_INPUT) {
            ldv_drain_to_cache(s);
            if (s->stop) { free(data); return 0; }
            continue;                        /* 取干净了再送同一个包 */
        }

        /* 坏流：记一条，跳过这个包，不中断整个会话 */
        ldv_set_error(s, "码流损坏，已跳过该包");
        break;
    }

    free(data);

    /* 每送一个包就把就绪的帧取出来 */
    ldv_drain_to_cache(s);

    /* 吞吐窗口（1 秒） */
    {
        int64_t now = ldv_now_us();
        if (s->windowStartUs == 0) s->windowStartUs = now;
        if (now - s->windowStartUs >= 1000000) {
            s->decodeFPS = (double)s->windowFrames * 1000000.0
                         / (double)(now - s->windowStartUs);
            s->windowStartUs = now;
            s->windowFrames = 0;
        }
    }

    return 1;
}

static void *ldv_session_thread_main(void *arg) {
    LDVDecodeSession *s = (LDVDecodeSession *)arg;

    s->running = 1;
    while (!s->stop) {
        if (!ldv_process_one_sample(s)) break;
    }
    s->running = 0;
    return NULL;
}

/* 起解码线程（Start / RescaleOutput 共用）。失败时 threadValid 保持 0。 */
static LDVResult ldv_session_spawn_thread(LDVDecodeSession *s) {
    int rc;

    rc = pthread_create(&s->thread, NULL, ldv_session_thread_main, s);
    if (rc != 0) {
        ldv_set_error(s, "无法创建解码线程");
        return LDV_ERR_NOMEM;
    }
    s->threadValid = 1;
    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

LDVDecodeSession *LDVDecodeSessionCreateWithGrainOverride(
        const char *mp4Path,
        size_t cacheBytes,
        int cacheFrames,
        int applyGrainOverride) {
    LDVDecodeSession *s;

    if (!mp4Path) return NULL;

    ldv_createError[0] = '\0';

    s = (LDVDecodeSession *)calloc(1, sizeof(LDVDecodeSession));
    if (!s) {
        snprintf(ldv_createError, sizeof(ldv_createError),
                 "会话结构分配失败（内存不足）");
        return NULL;
    }

    s->source = LDVMP4SourceOpen(mp4Path);
    if (!s->source) {
        fprintf(stderr, "LDVSession: 打不开 %s\n", mp4Path);
        snprintf(ldv_createError, sizeof(ldv_createError),
                 "打不开 MP4（文件缺失 / 不是可解析的 ISOBMFF / "
                 "没有 av01 视频轨）");
        free(s);
        return NULL;
    }

    s->cache = LDVFrameCacheCreate(cacheBytes, cacheFrames);
    if (!s->cache) {
        snprintf(ldv_createError, sizeof(ldv_createError),
                 "解码缓存分配失败（预算 %lu 字节 / %d 帧）",
                 (unsigned long)cacheBytes, cacheFrames);
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    /*
     * 解码器按"当前设备"选调参档。注意 LDVCoreDecoderCreate 收的是
     * **指针**，而 LDVTuningProfileForCurrentDevice() 返回结构体本身，
     * 所以要先落到局部变量再取地址。
     * tuning 提到外层作用域 —— 失败分支里要拿它的 n_threads 说明原因。
     */
    {
        LDVTuningProfile tuning = LDVTuningProfileForCurrentDevice();

        /*
         * grain 合成开关的外部覆盖（v8.15 决策）。
         *
         * 背景：grain 合成走消费路径**逐帧串行**做，不参与 n_threads=2
         * 的帧级并行；视频会被缩小显示时（窗口内容尺寸 < 视频原生
         * 尺寸），全分辨率上的 grain 是白算的 —— 缩小后肉眼也分不出。
         * 所以"会缩放 → 关 grain 换帧率；不缩放（视频 ≤ 窗口）→ 保留"。
         *
         * 判定只写一处：显示端（LDVPlayerViewController 的
         * decodeTargetForVideoSize:）已经算过"会不会缩小"，这里只收
         * 结论（applyGrainOverride）。override < 0 表示按 tuning 默认
         * （Demo / 老调用方）。
         */
        if (applyGrainOverride >= 0 && applyGrainOverride != tuning.applyGrain) {
            fprintf(stderr,
                    "LDVSession: apply_grain 由外部覆盖 %d -> %d"
                    "（缩小显示的素材，全分辨率 grain 合成白算）\n",
                    tuning.applyGrain, applyGrainOverride);
            tuning.applyGrain = applyGrainOverride;
        }

        s->core = LDVCoreDecoderCreate(&tuning);
        if (!s->core) {
            snprintf(ldv_createError, sizeof(ldv_createError),
                     "dav1d 初始化失败（n_threads=%d，见控制台）",
                     tuning.nThreads);
            LDVFrameCacheDestroy(&s->cache);
            LDVMP4SourceClose(&s->source);
            free(s);
            return NULL;
        }
    }

    return s;
}

LDVDecodeSession *LDVDecodeSessionCreate(const char *mp4Path,
                                         size_t cacheBytes,
                                         int cacheFrames) {
    /* 老入口：grain 按设备 tuning 默认（override = -1） */
    return LDVDecodeSessionCreateWithGrainOverride(mp4Path, cacheBytes,
                                                   cacheFrames, -1);
}

LDVResult LDVDecodeSessionStart(LDVDecodeSession *s) {
    if (!s) return LDV_ERR_INVALID_ARG;
    if (s->threadValid) return LDV_OK;

    s->stop = 0;
    s->eof  = 0;
    s->running = 0;
    s->windowStartUs = ldv_now_us();
    s->windowFrames = 0;

    return ldv_session_spawn_thread(s);
}

void LDVDecodeSessionStop(LDVDecodeSession *s) {
    if (!s) return;

    if (s->threadValid) {
        s->stop = 1;
        /* 广播一次，让可能正在等待条件变量的解码线程立刻醒来 */
        LDVFrameCacheBroadcast(s->cache);
        pthread_join(s->thread, NULL);
        s->threadValid = 0;
    }

    s->running = 0;

    /* 释放缓存里的全部帧 —— 这是"内存释放"最直接的体现 */
    if (s->cache) LDVFrameCacheClear(s->cache);
}

LDVResult LDVDecodeSessionRestart(LDVDecodeSession *s) {
    if (!s) return LDV_ERR_INVALID_ARG;

    /*
     * 停（join + 清缓存）→ 进度归零 → 重启。
     *
     * 复位项与 SetDspMode 的复位段保持一致，差别只有一点：
     * 这里**不重建解码器** —— DSP 表不变，从头重播不需要。
     * 两处复位清单如果将来要加字段，务必同步改（"同一事实两处写"）。
     */
    LDVDecodeSessionStop(s);

    s->nextSample    = 0;
    s->fedSamples    = 0;
    s->dropped       = 0;
    s->eof           = 0;
    s->loopCount     = 0;
    s->decodeFPS     = 0.0;
    s->windowFrames  = 0;
    s->windowStartUs = ldv_now_us();
    s->lastError[0]  = '\0';

    return LDVDecodeSessionStart(s);
}

void LDVDecodeSessionDestroy(LDVDecodeSession **session) {
    LDVDecodeSession *s;
    if (!session || !*session) return;
    s = *session;

    LDVDecodeSessionStop(s);

    /* 缩放器必须在解码线程 join 之后才能销毁 —— 它持有解码线程在用的权重表 */
    LDVDownscalerDestroy(&s->scaler);

    if (s->core)   LDVCoreDecoderDestroy(&s->core);
    if (s->cache)  LDVFrameCacheDestroy(&s->cache);
    if (s->source) LDVMP4SourceClose(&s->source);

    free(s);
    *session = NULL;
}

/* ------------------------------------------------------------------------ */
/* DSP 实现切换                                                              */
/* ------------------------------------------------------------------------ */

LDVResult LDVDecodeSessionSetDspMode(LDVDecodeSession *s, LDVDspMode mode) {
    int              wasRunning;
    LDVTuningProfile tuning;

    if (!s) return LDV_ERR_INVALID_ARG;

    /* 请求汇编但当前架构没有 —— 明确拒绝，不要静默降级（调用方要据此禁用按钮） */
    if (mode == LDV_DSP_MODE_ASM && !LDVDspAsmAvailable())
        return LDV_ERR_UNSUPPORTED;

    /* 模式没变：不做任何事。**尤其不要重建** —— 那会让播放无故从头开始 */
    if (mode == LDVDspPreferredMode()) return LDV_OK;

    /*
     * 顺序不能变：
     *   1. 停线程（join）—— 绝不能在解码线程还在 Pull 的时候销毁解码器
     *   2. 销毁旧解码器（Stop 里已顺带清空缓存）
     *   3. 设新偏好 —— 下一次 Create 会把它 Apply 到 dav1d 的 CPU 掩码
     *   4. 重建解码器
     *   5. 复位会话进度（新解码器要从序列头重新开始，nextSample 必须归零）
     *   6. 按原来的运行状态重启线程
     */
    wasRunning = s->threadValid;

    LDVDecodeSessionStop(s);

    LDVCoreDecoderDestroy(&s->core);
    LDVDspSetPreferredMode(mode);

    tuning = LDVTuningProfileForCurrentDevice();
    s->core = LDVCoreDecoderCreate(&tuning);
    if (!s->core) {
        ldv_set_error(s, "切换 DSP 模式后重建解码器失败");
        return LDV_ERR_NOMEM;
    }

    /* 重建后从序列头开始 —— 这一步是正确性要求，不只是"重置缓存" */
    LDVFrameCacheClear(s->cache);
    s->nextSample   = 0;
    s->fedSamples   = 0;
    s->dropped      = 0;
    s->eof          = 0;
    s->loopCount    = 0;
    s->decodeFPS    = 0.0;
    s->windowFrames = 0;
    s->windowStartUs = ldv_now_us();
    s->lastError[0] = '\0';

    if (wasRunning) LDVDecodeSessionStart(s);

    fprintf(stderr, "LDVSession: DSP 模式已切换为 %s（解码器已重建，缓存已清空）\n",
            LDVDspModeName(mode));

    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 缓存缩放                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 为 (w, h) 建缩放器 —— SetOutputSize / RescaleOutput 共用的核心。
 * 调用方约定：线程已停、旧的 s->scaler 已销毁、scaleActive/outW/outH 已清零。
 * 失败时播放退回原始尺寸（不算致命错误，scaleActive 保持 0）。
 */
static LDVResult ldv_session_build_scaler(LDVDecodeSession *s, int w, int h) {
    const uint8_t *obus = NULL;
    size_t         obusLen = 0;
    int            srcW, srcH, bpc = 0, layout = 0;

    /*
     * 视频尺寸走 LDVDecodeSessionVideoWidth/Height —— 和状态栏显示的
     * 是**同一个来源**。另开一条路读尺寸，就会出现"状态栏说 512x384、
     * 缩放器按别的数字建表"这种最难查的漂移。
     */
    srcW = LDVDecodeSessionVideoWidth(s);
    srcH = LDVDecodeSessionVideoHeight(s);
    if (srcW <= 0 || srcH <= 0) return LDV_ERR_UNSUPPORTED;

    /* 目标不比源小 → 没什么可缩的。这是正常结果，不是错误。 */
    if (w >= srcW || h >= srcH) return LDV_OK;

    /*
     * 位深与布局**从 AV1 配置 OBU 里探**，不猜。
     *
     * 这里必须探而不是假设 I420：缩放器要按布局算色度平面尺寸，
     * 猜错会让色度平面越界读写 —— 那是内存破坏，不是画面花屏。
     */
    if (LDVMP4SourceGetConfigOBUs(s->source, &obus, &obusLen) == LDV_OK &&
        obus && obusLen > 0) {
        (void)LDVCoreProbeSequenceHeader(obus, obusLen,
                                         NULL, NULL, &bpc, &layout, NULL, NULL);
    }

    if (bpc > 8) {
        /*
         * 高位深不支持缩放。明确打一行说明原因，而不是静默退回 ——
         * "为什么没缩"必须能从日志里直接读出来。
         */
        fprintf(stderr, "LDVSession: 素材 %d bit，暂不支持缓存缩放，"
                        "退回原始尺寸\n", bpc);
        return LDV_ERR_UNSUPPORTED;
    }
    if (layout <= (int)LDV_PIXEL_LAYOUT_I400 ||
        layout >  (int)LDV_PIXEL_LAYOUT_I444) {
        layout = (int)LDV_PIXEL_LAYOUT_I420;   /* AV1 8bit 的常规取值 */
    }

    s->scaler = LDVDownscalerCreate(srcW, srcH, w, h, (LDVPixelLayout)layout);
    if (!s->scaler) {
        /* 多半是内存不足。不该让播放失败：退回不缩即可。 */
        ldv_set_error(s, "缩小器创建失败，退回原始尺寸");
        return LDV_ERR_UNSUPPORTED;
    }

    s->outW        = LDVDownscalerDstWidth(s->scaler);
    s->outH        = LDVDownscalerDstHeight(s->scaler);
    s->scaleActive = 1;

    fprintf(stderr,
            "LDVSession: 缓存帧缩小 %dx%d → %dx%d"
            "（每帧 %.0f%% 内存，同预算可多装 %.1f 倍帧数）\n",
            srcW, srcH, s->outW, s->outH,
            100.0 * (double)(s->outW * s->outH) / (double)(srcW * srcH),
            (double)(srcW * srcH) / (double)(s->outW * s->outH));
    return LDV_OK;
}

LDVResult LDVDecodeSessionSetOutputSize(LDVDecodeSession *s, int w, int h) {
    if (!s) return LDV_ERR_INVALID_ARG;

    /*
     * ★ 必须在 Start 之前。解码线程一旦跑起来就会用 s->scaler，
     *   而换缩放器要重建权重表 —— 那是个几百 KB 的结构，
     *   在运行中换必须和生产者同步，很容易漏。
     *   与其做一个"小心使用"的接口，不如直接拒绝。
     *   （运行中换挡走 LDVDecodeSessionRescaleOutput。）
     */
    if (s->threadValid) return LDV_ERR_INVALID_ARG;

    /* 清掉旧的（重复调用时） */
    LDVDownscalerDestroy(&s->scaler);
    s->scaleActive = 0;
    s->outW = s->outH = 0;

    if (w <= 0 || h <= 0) return LDV_OK;    /* 没要求缩 */

    return ldv_session_build_scaler(s, w, h);
}

/*
 * 运行中换解码输出尺寸（v8.18 动态分辨率的核心）。
 *
 * 与 SetOutputSize（只能 Start 前调）的三点差别：
 *   1. 停解码线程但**不清缓存** —— LDVDecodeSessionStop 会清空缓存，
 *      与"低清缓存保留"的策略冲突，所以这里手动 join；
 *   2. 按 keepBelowWidth 过滤缓存：宽超过它的帧（"高清缓存"）释放，
 *      其余保留 —— 换挡后缓存里会短暂存在两种分辨率的帧，
 *      显示端（GL/Metal/CPU 三条路径）本来就按帧自适应尺寸；
 *   3. 从**当前取样位置**继续解码 —— nextSample/loopCount/节拍序号
 *      全部不重置，与 Restart 的"从头重播"是两种语义。
 *
 *   keepBelowWidth > 0：宽超过它的缓存帧被释放；
 *   keepBelowWidth <= 0：全部保留（换到更大/原生尺寸时旧帧必然更小）。
 *
 * 失败语义：缩放器建不起来（高位深/内存不足）→ 按原尺寸继续播放，
 * 不算失败；只有线程重启失败才返回错误（极端情况，状态保持一致）。
 */
LDVResult LDVDecodeSessionRescaleOutput(LDVDecodeSession *s, int w, int h,
                                        int keepBelowWidth) {
    int wasRunning;
    LDVResult r;

    if (!s) return LDV_ERR_INVALID_ARG;

    wasRunning = s->threadValid;
    if (wasRunning) {
        s->stop = 1;
        LDVFrameCacheBroadcast(s->cache);
        pthread_join(s->thread, NULL);
        s->threadValid = 0;
        s->running = 0;
    }

    if (keepBelowWidth > 0 && s->cache) {
        int dropped = LDVFrameCacheDropWiderThan(s->cache, keepBelowWidth);
        if (dropped > 0) {
            fprintf(stderr, "LDVSession: 换挡释放宽 > %d 的缓存帧 %d 个\n",
                    keepBelowWidth, dropped);
        }
    }

    LDVDownscalerDestroy(&s->scaler);
    s->scaleActive = 0;
    s->outW = s->outH = 0;

    r = LDV_OK;
    if (w > 0 && h > 0) {
        r = ldv_session_build_scaler(s, w, h);
        if (r != LDV_OK) {
            /* 原尺寸继续，不让播放失败 */
            s->scaleActive = 0;
            s->outW = s->outH = 0;
            r = LDV_OK;
        }
    }

    if (wasRunning) {
        s->stop = 0;
        if (ldv_session_spawn_thread(s) != LDV_OK) {
            return LDV_ERR_NOMEM;   /* 线程起不来：播放停摆但状态一致 */
        }
    }
    return LDV_OK;
}

LDVResult LDVDecodeSessionSetFrameSink(LDVDecodeSession *s,
                                       LDVFrameSinkFn sink, void *userdata) {
    if (!s) return LDV_ERR_INVALID_ARG;
    if (s->threadValid) return LDV_ERR_INVALID_ARG;   /* 必须 Start 之前 */

    /*
     * 两条输出管线互斥：sink 负责把帧（转换后）推进缓存时，
     * CPU 缩放器没有存在意义；留着只会在某条未走过的路径上被误用。
     */
    if (sink) {
        LDVDownscalerDestroy(&s->scaler);
        s->scaleActive = 0;
        s->outW = s->outH = 0;
    }

    s->sink = sink;
    s->sinkUD = userdata;
    return LDV_OK;
}

LDVFrameCache *LDVDecodeSessionFrameCache(const LDVDecodeSession *s) {
    return s ? s->cache : NULL;
}

int LDVDecodeSessionOutputWidth(const LDVDecodeSession *s) {
    return s ? s->outW : 0;
}

int LDVDecodeSessionOutputHeight(const LDVDecodeSession *s) {
    return s ? s->outH : 0;
}

int LDVDecodeSessionScalingActive(const LDVDecodeSession *s) {
    return s ? s->scaleActive : 0;
}

/* ------------------------------------------------------------------------ */
/* 显示端                                                                    */
/* ------------------------------------------------------------------------ */

int LDVDecodeSessionPeekFrame(LDVDecodeSession *s, LDVFrameView *out) {
    if (!s || !s->cache) return 0;
    return LDVFrameCachePeek(s->cache, out);
}

int LDVDecodeSessionNextFrameTimestamp(LDVDecodeSession *s,
                                       int64_t *outTimestamp) {
    if (!s || !s->cache) return 0;
    return LDVFrameCachePeekTimestamp(s->cache, outTimestamp);
}

int LDVDecodeSessionNextFrameIndex(LDVDecodeSession *s, int64_t *outIndex) {
    if (!s || !s->cache) return 0;
    return LDVFrameCachePeekIndex(s->cache, outIndex);
}

uint32_t LDVDecodeSessionTimescale(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceTimescale(s->source) : 0;
}

void LDVDecodeSessionPopFrame(LDVDecodeSession *s) {
    if (!s || !s->cache) return;
    LDVFrameCachePop(s->cache);
}

int LDVDecodeSessionDropOldestFrame(LDVDecodeSession *s) {
    int n;
    if (!s || !s->cache) return 0;
    n = LDVFrameCacheDropOldest(s->cache);
    s->dropped += n;
    return n;
}

/* ------------------------------------------------------------------------ */
/* 缓存状态                                                                  */
/* ------------------------------------------------------------------------ */

float LDVDecodeSessionCacheProgress(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheProgress(s->cache) : 0.0f;
}

size_t LDVDecodeSessionCacheBytes(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheBytes(s->cache) : 0;
}

size_t LDVDecodeSessionCacheBudget(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheByteBudget(s->cache) : 0;
}

int LDVDecodeSessionCacheCount(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheCount(s->cache) : 0;
}

int LDVDecodeSessionCacheFull(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheIsFull(s->cache) : 0;
}

/* ------------------------------------------------------------------------ */
/* 策略开关                                                                  */
/* ------------------------------------------------------------------------ */

void LDVDecodeSessionSetDropWhenFull(LDVDecodeSession *s, int drop) {
    if (s) s->dropWhenFull = drop ? 1 : 0;
}

int LDVDecodeSessionDropWhenFull(const LDVDecodeSession *s) {
    return s ? s->dropWhenFull : 0;
}

void LDVDecodeSessionSetLooping(LDVDecodeSession *s, int loop) {
    if (s) s->looping = loop ? 1 : 0;
}

int LDVDecodeSessionLooping(const LDVDecodeSession *s) {
    return s ? s->looping : 0;
}

/* ------------------------------------------------------------------------ */
/* 统计                                                                      */
/* ------------------------------------------------------------------------ */

int64_t LDVDecodeSessionDecodedFrames(const LDVDecodeSession *s) {
    return (s && s->core) ? LDVCoreDecoderFrameCount(s->core) : 0;
}

int64_t LDVDecodeSessionFedSamples(const LDVDecodeSession *s) {
    return s ? s->fedSamples : 0;
}

int64_t LDVDecodeSessionTotalFrames(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceFrameCount(s->source) : 0;
}

/*
 * 缓存「拒收」次数。
 *
 * ★ 这个计数器是专门为定位一类故障加的：缓存满时生产者本该去等空间，
 *   如果它没等（或者两处「满」的判定不一致），就会出现
 *   「生产者一直在解、但帧全被拒收丢掉」—— 表现是界面上的解码计数不动、
 *   缓存却持续下降。
 *
 *   正常播放时它应该几乎不涨（只在恰好卡在边界时偶尔 +1）。
 *   持续快速上涨 = 生产者在空转，帧在被丢。
 */
int64_t LDVDecodeSessionCacheFullHits(const LDVDecodeSession *s) {
    return (s && s->cache) ? LDVFrameCacheFullHits(s->cache) : 0;
}

int64_t LDVDecodeSessionDroppedFrames(const LDVDecodeSession *s) {
    return s ? s->dropped : 0;
}

int64_t LDVDecodeSessionErrorCount(const LDVDecodeSession *s) {
    return (s && s->core) ? LDVCoreDecoderErrorCount(s->core) : 0;
}

double LDVDecodeSessionDecodeFPS(const LDVDecodeSession *s) {
    return s ? s->decodeFPS : 0.0;
}

int LDVDecodeSessionIsRunning(const LDVDecodeSession *s) {
    return s ? s->running : 0;
}

int LDVDecodeSessionIsEOF(const LDVDecodeSession *s) {
    return s ? s->eof : 0;
}

int LDVDecodeSessionLoopCount(const LDVDecodeSession *s) {
    return s ? (int)s->loopCount : 0;
}

const char *LDVDecodeSessionLastError(const LDVDecodeSession *s) {
    if (!s || s->lastError[0] == '\0') return NULL;
    return s->lastError;
}

/* ------------------------------------------------------------------------ */
/* 轨道信息                                                                  */
/* ------------------------------------------------------------------------ */

int LDVDecodeSessionVideoWidth(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceWidth(s->source) : 0;
}

int LDVDecodeSessionVideoHeight(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceHeight(s->source) : 0;
}

double LDVDecodeSessionVideoFPS(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceFPS(s->source) : 0.0;
}

/*
 * 视频轨首样本的 PTS（秒）。
 *
 * 音画同步要用：显示节拍算出来的是"第 idx 帧距轨首多少秒"，
 * 要加上这个起点才是**容器时间轴上的媒体时间**，
 * 才能和音频时钟（LDVAudioSessionMediaTime）直接比较。
 */
double LDVDecodeSessionFirstPtsSeconds(const LDVDecodeSession *s) {
    return (s && s->source) ? LDVMP4SourceFirstPtsSeconds(s->source) : 0.0;
}

/*
 * 容器时间戳（视频轨 timescale tick）→ 容器时间轴秒（v8.25 动态帧率）。
 *
 * 原点对齐 FirstPtsSeconds：先减轨首 PTS 的 tick 数再除以 timescale，
 * 加上轨首的秒值 —— 这样 PTS 相同的帧换出来的秒与"序号/帧率 + 轨首"
 * 在同一条时间轴上，音频比较、音画偏差计算都不用改。
 */
double LDVDecodeSessionTimestampSeconds(const LDVDecodeSession *s,
                                        int64_t ts) {
    uint32_t tsc;
    int64_t  first;

    if (!s || !s->source || ts == INT64_MIN) return 0.0;
    tsc = LDVMP4SourceTimescale(s->source);
    if (tsc == 0) return 0.0;
    first = LDVMP4SourceFirstPts(s->source);
    return LDVMP4SourceFirstPtsSeconds(s->source)
         + (double)(ts - first) / (double)tsc;
}

/* ------------------------------------------------------------------------ */
/* 进程内存                                                                  */
/* ------------------------------------------------------------------------ */

size_t LDVDecodeSessionResidentMemory(void) {
#if defined(__APPLE__) && defined(TASK_BASIC_INFO)
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (size_t)info.resident_size;
    }
#endif
    return 0;
}

size_t LDVDecodeSessionVirtualMemory(void) {
#if defined(__APPLE__) && defined(TASK_BASIC_INFO)
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (size_t)info.virtual_size;
    }
#endif
    return 0;
}
