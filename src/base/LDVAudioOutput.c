/*
 * LDVAudioOutput.c — 音频输出（AudioToolbox AudioQueue 封装）
 *
 * 设计说明见 LDVAudioOutput.h。三个实现要点：
 *
 * 1) 缓冲所有权
 *    缓冲块由本模块持有，AudioQueue 只借用。AudioQueue 播完一块会在
 *    回调里把它还回来 —— 回调跑在 AudioQueue 自己的线程上，所以
 *    "空闲标记 + 条件变量"必须加锁。回调里**不做任何重活**（只标位
 *    和广播），避免拖慢音频线程。
 *
 * 2) 时钟用 AudioQueueGetCurrentTime
 *    取的是"真正播出去的采样数"，而不是入队量。差别是 N 块缓冲的
 *    提前量（默认约 93 ms）—— 拿入队量当主时钟会让画面稳定领先
 *    声音 0.1 秒。这个量级人耳一听就知道，但看代码看不出来。
 *
 * 3) 欠载不抛错
 *    写不进去就返回 LDV_ERR_NEED_INPUT，由上层决定丢帧还是补静音。
 *    本模块永远不阻塞式死等 —— 需求要求允许音频卡顿，
 *    但绝不能因此拖住视频渲染。
 */

#include "LDVAudioOutput.h"

#include <AudioToolbox/AudioToolbox.h>

/*
 * ★ 头文件必须显式写全，不能指望 AudioToolbox 间接带进来。
 *
 * 这里踩过一次真实的坑：armv7 能编过、**arm64 编不过**，原因是
 * 两个架构的 `-miphoneos-version-min` 不同（4.0 vs 7.0），
 * 间接包含的链条随 SDK 的 availability 分支走，落在哪个头上并不稳定。
 * 显式包含就没有这个不确定性。
 *
 *   <errno.h>     ETIMEDOUT（pthread_cond_timedwait 的返回值比较）
 *   <sys/time.h>  gettimeofday
 *   <time.h>      struct timespec
 *   <stdbool.h>   true / false（C99 里它们不是关键字）
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define LDV_AQ_DEFAULT_BUFFERS 4
#define LDV_AQ_DEFAULT_FRAMES  1024

/*
 * 设置音频会话类别（进程级，只需一次）。
 *
 * 为什么必须设
 * ------------
 * iOS 应用的默认类别是 SoloAmbient，它的行为是**跟随静音开关**：
 * 开关拨到静音就没有声音。对视频播放器来说这是错的 —— 用户拨的是
 * "静音铃声"，不是"静音媒体"。所以要显式设成 MediaPlayback。
 *
 * 为什么用 AudioToolbox 的 C API 而不是 AVAudioSession
 * ---------------------------------------------------
 *   AVAudioSession 是 iOS 6+ 才有的 ObjC 类，而本库要兼容 iOS 4/5。
 *   AudioSessionInitialize / AudioSessionSetProperty 是 iOS 2.0+ 的
 *   C 接口，在 iOS 7 被标记废弃但**仍然可用**（而且这是老系统上唯一
 *   的选择）。所以这里用后者，并把废弃告警就地屏蔽掉。
 *
 * 线程：AudioSessionInitialize 要求在主线程或已有 run loop 的线程调用。
 * 本函数由 LDVAudioSessionCreate 触发，而它是在 viewDidLoad 里调的
 * —— 保证在主线程。用 done 标志兜住重复调用。
 */
/*
 * ★ 音频会话中断监听（v8.24 新增）
 *
 * 为什么要监听：AudioSessionInitialize 的第三参数是中断回调，
 * 传 NULL 意味着**系统中断（电话/闹钟/Siri/tweak 触发的音频会话
 * 抢占）到来时，AudioQueue 被**系统单方面停住**，而本模块的
 * started 标记还是 1 —— 之后每一块 PCM 都照常入队，队列却永远
 * 不会播出，表现为"一切正常但没有声音"，且任何错误计数都不涨。
 * 越狱机上 respring / activator / 各类音频 tweak 都会触发中断，
 * 这正是多台越狱真机"同一构建有的有声有的没声"的头号候选。
 *
 * 处理：Begin → 队列在播就 Pause 并记住；End → 重新 SetActive
 * 并 Start 恢复。音频会话是进程资源，监听器只注册一次，
 * 通过 s_activeOutput 指到当前活着的输出实例。
 */
/* ★ 本结构体必须排在所有解引用 o->... 的回调之前 —— C 语言不允许
   通过不完整类型访问成员（2026-10-03 armv7 实锤：中断回调被插到了
   结构体定义之前，于是 LDVAudioOutput.c:94 起一片「Incomplete
   definition of type 'struct LDVAudioOutput'」）。 */
struct LDVAudioOutput {
    AudioQueueRef   queue;
    int             valid;

    uint32_t        sampleRate;
    int             channels;
    size_t          bytesPerFrame;

    int             bufferCount;
    int             framesPerBuffer;

    AudioQueueBufferRef buffers[16];
    int             inUse[16];       /* 1 = 已交给 AudioQueue */
    int             freeCount;

    int64_t         playedFrames;    /* 回调累计（兜底用） */
    int64_t         enqueuedFrames;
    int64_t         underruns;

    int             started;
    int             paused;
    int             stopping;
    int             interrupted;     /* 中断开始时在播 → End 时要恢复 */

    pthread_mutex_t lock;
    pthread_cond_t  cond;
};

static LDVAudioOutput *s_activeOutput = NULL;

static void ldv_audio_interruption_cb(void *userData, UInt32 type) {
    LDVAudioOutput *o = s_activeOutput;
    (void)userData;
    if (!o) return;

    pthread_mutex_lock(&o->lock);
    if (!o->valid || !o->queue) {
        pthread_mutex_unlock(&o->lock);
        return;
    }
    if (type == kAudioSessionBeginInterruption) {
        if (o->started && !o->paused) {
            AudioQueuePause(o->queue);
            o->interrupted = 1;
        }
    } else if (type == kAudioSessionEndInterruption) {
        UInt32 cat = kAudioSessionCategory_MediaPlayback;
        /* End 后会话默认是失活的，老系统还可能丢类别：不重新
         * SetActive + 设类别，Start 会失败或按 SoloAmbient 静音 */
        AudioSessionSetProperty(kAudioSessionProperty_AudioCategory,
                                (UInt32)sizeof(cat), &cat);
        AudioSessionSetActive(1);
        if (o->interrupted) {
            o->interrupted = 0;
            AudioQueueStart(o->queue, NULL);
        }
    }
    pthread_mutex_unlock(&o->lock);
}

static void ldv_audio_setup_session(void) {
    static int done = 0;
    UInt32 cat;

    if (done) return;
    done = 1;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AudioSessionInitialize(NULL, NULL, ldv_audio_interruption_cb, NULL);
    cat = kAudioSessionCategory_MediaPlayback;
    AudioSessionSetProperty(kAudioSessionProperty_AudioCategory,
                            (UInt32)sizeof(cat), &cat);
    AudioSessionSetActive(1);   /* Boolean：用 1 而不是 true，少一个头文件依赖 */
#pragma clang diagnostic pop
}

/* ------------------------------------------------------------------------ */
/* AudioQueue 回调（跑在 AudioQueue 自己的线程上）                            */
/* ------------------------------------------------------------------------ */

static void ldv_aq_output_cb(void *userData, AudioQueueRef queue,
                             AudioQueueBufferRef buf) {
    LDVAudioOutput *o = (LDVAudioOutput *)userData;
    int i;

    (void)queue;
    if (!o) return;

    pthread_mutex_lock(&o->lock);

    for (i = 0; i < o->bufferCount; i++) {
        if (o->buffers[i] == buf && o->inUse[i]) {
            o->inUse[i] = 0;
            o->freeCount++;
            o->playedFrames += o->framesPerBuffer;
            break;
        }
    }
    pthread_cond_broadcast(&o->cond);

    pthread_mutex_unlock(&o->lock);
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

LDVAudioOutput *LDVAudioOutputCreate(uint32_t sampleRate, int channels,
                                     int bufferCount, int framesPerBuffer) {
    LDVAudioOutput *o;
    AudioStreamBasicDescription fmt;
    OSStatus st;
    int i;

    if (sampleRate == 0 || channels <= 0) return NULL;

    if (bufferCount < 2)      bufferCount = LDV_AQ_DEFAULT_BUFFERS;
    if (bufferCount > 16)     bufferCount = 16;
    if (framesPerBuffer <= 0) framesPerBuffer = LDV_AQ_DEFAULT_FRAMES;

    o = (LDVAudioOutput *)calloc(1, sizeof(*o));
    if (!o) return NULL;

    o->sampleRate      = sampleRate;
    o->channels        = channels;
    o->bytesPerFrame   = (size_t)channels * 2;      /* 16 位交错 */
    o->bufferCount     = bufferCount;
    o->framesPerBuffer = framesPerBuffer;

    if (pthread_mutex_init(&o->lock, NULL) != 0) { free(o); return NULL; }
    if (pthread_cond_init(&o->cond, NULL) != 0) {
        pthread_mutex_destroy(&o->lock); free(o); return NULL;
    }

    /* 先把音频会话类别设好，再建队列 —— 否则第一批数据可能按
     * SoloAmbient 处理（静音开关一拨就没声） */
    ldv_audio_setup_session();

    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate       = (Float64)sampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger |
                            kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = (UInt32)channels;
    fmt.mBitsPerChannel   = 16;
    fmt.mBytesPerFrame    = (UInt32)o->bytesPerFrame;
    fmt.mBytesPerPacket   = fmt.mBytesPerFrame;

    /* 回调跑在 AudioQueue 内部线程：run loop 与 mode 传 NULL */
    st = AudioQueueNewOutput(&fmt, ldv_aq_output_cb, o, NULL, NULL, 0,
                             &o->queue);
    if (st != noErr || !o->queue) {
        fprintf(stderr, "LDVAudio: AudioQueueNewOutput 失败（OSStatus %d）\n",
                (int)st);
        pthread_cond_destroy(&o->cond);
        pthread_mutex_destroy(&o->lock);
        free(o);
        return NULL;
    }

    /* 预分配全部缓冲 */
    {
        const UInt32 bytes = (UInt32)((size_t)framesPerBuffer * o->bytesPerFrame);
        for (i = 0; i < bufferCount; i++) {
            st = AudioQueueAllocateBuffer(o->queue, bytes, &o->buffers[i]);
            if (st != noErr || !o->buffers[i]) {
                fprintf(stderr, "LDVAudio: AudioQueueAllocateBuffer 失败"
                                "（第 %d 块，OSStatus %d）\n", i, (int)st);
                AudioQueueDispose(o->queue, 1);   /* 1 = 同步释放全部缓冲 */
                pthread_cond_destroy(&o->cond);
                pthread_mutex_destroy(&o->lock);
                free(o);
                return NULL;
            }
            o->buffers[i]->mAudioDataByteSize = 0;
            o->inUse[i] = 0;
        }
        o->freeCount = bufferCount;
    }

    o->valid = 1;
    s_activeOutput = o;   /* 中断监听从此指向本实例（进程内单输出） */

    fprintf(stderr,
            "LDVAudio: 输出就绪  %u Hz / %d ch  %d 块 x %d 帧（%.0f ms 缓冲）\n",
            sampleRate, channels, bufferCount, framesPerBuffer,
            1000.0 * framesPerBuffer * bufferCount / (double)sampleRate);

    return o;
}

void LDVAudioOutputDestroy(LDVAudioOutput **out) {
    LDVAudioOutput *o;
    if (!out || !*out) return;
    o = *out;

    LDVAudioOutputStop(o);

    pthread_mutex_lock(&o->lock);
    o->valid = 0;
    if (s_activeOutput == o) s_activeOutput = NULL;   /* 监听不再指向已销毁实例 */
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    if (o->queue) {
        AudioQueueDispose(o->queue, 1);   /* 1 = 同步释放全部缓冲 */
        o->queue = NULL;
    }
    pthread_cond_destroy(&o->cond);
    pthread_mutex_destroy(&o->lock);
    free(o);
    *out = NULL;
}

/* ------------------------------------------------------------------------ */
/* 起停                                                                      */
/* ------------------------------------------------------------------------ */

LDVResult LDVAudioOutputStart(LDVAudioOutput *o) {
    OSStatus st;
    if (!o || !o->valid) return LDV_ERR_INVALID_ARG;

    pthread_mutex_lock(&o->lock);
    if (o->started && !o->paused) { pthread_mutex_unlock(&o->lock); return LDV_OK; }
    o->stopping = 0;
    pthread_mutex_unlock(&o->lock);

    st = AudioQueueStart(o->queue, NULL);
    if (st != noErr) {
        fprintf(stderr, "LDVAudio: AudioQueueStart 失败（OSStatus %d）\n", (int)st);
        return LDV_ERR_UNSUPPORTED;
    }

    pthread_mutex_lock(&o->lock);
    o->started = 1;
    o->paused  = 0;
    pthread_mutex_unlock(&o->lock);
    return LDV_OK;
}

void LDVAudioOutputPause(LDVAudioOutput *o) {
    if (!o || !o->valid) return;
    AudioQueuePause(o->queue);
    pthread_mutex_lock(&o->lock);
    o->paused = 1;
    pthread_mutex_unlock(&o->lock);
}

LDVResult LDVAudioOutputResume(LDVAudioOutput *o) {
    OSStatus st;
    if (!o || !o->valid) return LDV_ERR_INVALID_ARG;

    pthread_mutex_lock(&o->lock);
    if (!o->paused) { pthread_mutex_unlock(&o->lock); return LDV_OK; }
    pthread_mutex_unlock(&o->lock);

    st = AudioQueueStart(o->queue, NULL);
    if (st != noErr) return LDV_ERR_UNSUPPORTED;

    pthread_mutex_lock(&o->lock);
    o->paused = 0;
    pthread_mutex_unlock(&o->lock);
    return LDV_OK;
}

void LDVAudioOutputStop(LDVAudioOutput *o) {
    if (!o || !o->valid || !o->queue) return;

    pthread_mutex_lock(&o->lock);
    o->stopping = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    AudioQueueStop(o->queue, 1);         /* 1 = 立刻停，不等播完 */
    AudioQueueReset(o->queue);           /* 丢弃残余缓冲 */

    pthread_mutex_lock(&o->lock);
    {
        int i;
        for (i = 0; i < o->bufferCount; i++) {
            o->inUse[i] = 0;
            if (o->buffers[i]) o->buffers[i]->mAudioDataByteSize = 0;
        }
        o->freeCount  = o->bufferCount;
        o->started    = 0;
        o->paused     = 0;
        o->stopping   = 0;
        o->playedFrames = 0;             /* 时钟归零：下次起播从头算 */
    }
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
}

/* ------------------------------------------------------------------------ */
/* 写入                                                                      */
/* ------------------------------------------------------------------------ */

LDVResult LDVAudioOutputWrite(LDVAudioOutput *o, const void *pcm,
                              size_t bytes, int timeoutMs) {
    AudioQueueBufferRef buf = NULL;
    size_t useBytes;
    int slot = -1;
    int i;

    if (!o || !o->valid || !o->queue) return LDV_ERR_INVALID_ARG;
    if (!pcm || bytes == 0)             return LDV_ERR_INVALID_ARG;

    /* 截到"每帧字节数"的整数倍 */
    useBytes = (bytes / o->bytesPerFrame) * o->bytesPerFrame;
    if (useBytes == 0) return LDV_ERR_INVALID_ARG;

    /* 超过一块的容量就截断（上层按块写，不该发生） */
    {
        const size_t cap = (size_t)o->framesPerBuffer * o->bytesPerFrame;
        if (useBytes > cap) useBytes = cap;
    }

    pthread_mutex_lock(&o->lock);

    if (o->stopping || !o->started) {
        pthread_mutex_unlock(&o->lock);
        return LDV_ERR_EOF;
    }

    /*
     * 全部缓冲都空闲 = 队列被掏空了 —— 这就是一次欠载（audible glitch）。
     * 统计它，但**不在这里补静音**：补不补是上层的策略。
     */
    if (o->freeCount == o->bufferCount && o->enqueuedFrames > 0) {
        o->underruns++;
    }

    if (o->freeCount == 0) {
        if (timeoutMs > 0) {
            struct timespec ts;
            struct timeval  now;
            gettimeofday(&now, NULL);
            ts.tv_sec  = now.tv_sec + timeoutMs / 1000;
            ts.tv_nsec = (long)(now.tv_usec * 1000) + (long)(timeoutMs % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            while (o->freeCount == 0 && !o->stopping) {
                if (pthread_cond_timedwait(&o->cond, &o->lock, &ts) == ETIMEDOUT)
                    break;
            }
        }
        if (o->freeCount == 0) {
            pthread_mutex_unlock(&o->lock);
            return LDV_ERR_NEED_INPUT;
        }
    }

    if (o->stopping) {
        pthread_mutex_unlock(&o->lock);
        return LDV_ERR_EOF;
    }

    for (i = 0; i < o->bufferCount; i++) {
        if (!o->inUse[i]) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&o->lock);
        return LDV_ERR_NEED_INPUT;
    }

    buf = o->buffers[slot];
    o->inUse[slot] = 1;
    o->freeCount--;
    o->enqueuedFrames += (int64_t)(useBytes / o->bytesPerFrame);

    pthread_mutex_unlock(&o->lock);

    /* 拷贝与入队在锁外做：拷贝可能不小，不该占着锁 */
    memcpy(buf->mAudioData, pcm, useBytes);
    buf->mAudioDataByteSize = (UInt32)useBytes;

    if (AudioQueueEnqueueBuffer(o->queue, buf, 0, NULL) != noErr) {
        pthread_mutex_lock(&o->lock);
        o->inUse[slot] = 0;
        o->freeCount++;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        return LDV_ERR_BITSTREAM;
    }

    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 时钟与状态                                                                */
/* ------------------------------------------------------------------------ */

int64_t LDVAudioOutputFramesPlayed(LDVAudioOutput *o) {
    AudioTimeStamp ts;
    int64_t frames;

    if (!o || !o->valid || !o->queue) return 0;

    pthread_mutex_lock(&o->lock);
    if (!o->started) {
        frames = 0;
        pthread_mutex_unlock(&o->lock);
        return frames;
    }
    pthread_mutex_unlock(&o->lock);

    /*
     * ★ 用 AudioQueueGetCurrentTime 取**真正播出去的**采样数，
     *   而不是入队量。入队量领先真实播放最多 bufferCount 块
     *   （默认 4 x 1024 帧 ≈ 93 ms @44.1kHz），拿它当主时钟会让
     *   画面稳定领先声音约 0.1 秒 —— 人耳一听就出来，
     *   但看代码看不出来（数字都对，只是量的不是同一个东西）。
     */
    memset(&ts, 0, sizeof(ts));
    if (AudioQueueGetCurrentTime(o->queue, NULL, &ts, NULL) == noErr &&
        (ts.mFlags & kAudioTimeStampSampleTimeValid) &&
        ts.mSampleTime > 0.0) {
        return (int64_t)ts.mSampleTime;
    }

    /* 取不到（队列未跑/暂停）就退回回调累计值 */
    pthread_mutex_lock(&o->lock);
    frames = o->playedFrames;
    pthread_mutex_unlock(&o->lock);
    return frames;
}

double LDVAudioOutputClockSeconds(LDVAudioOutput *o) {
    if (!o || o->sampleRate == 0) return 0.0;
    return (double)LDVAudioOutputFramesPlayed(o) / (double)o->sampleRate;
}

int LDVAudioOutputFreeBuffers(const LDVAudioOutput *o) {
    int n;
    if (!o) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&o->lock);
    n = o->freeCount;
    pthread_mutex_unlock((pthread_mutex_t *)&o->lock);
    return n;
}

uint32_t LDVAudioOutputSampleRate(const LDVAudioOutput *o) {
    return o ? o->sampleRate : 0;
}

int LDVAudioOutputChannels(const LDVAudioOutput *o) {
    return o ? o->channels : 0;
}

int64_t LDVAudioOutputEnqueuedFrames(const LDVAudioOutput *o) {
    int64_t n;
    if (!o) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&o->lock);
    n = o->enqueuedFrames;
    pthread_mutex_unlock((pthread_mutex_t *)&o->lock);
    return n;
}

int64_t LDVAudioOutputUnderruns(const LDVAudioOutput *o) {
    int64_t n;
    if (!o) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&o->lock);
    n = o->underruns;
    pthread_mutex_unlock((pthread_mutex_t *)&o->lock);
    return n;
}

int LDVAudioOutputIsRunning(const LDVAudioOutput *o) {
    return (o && o->valid && o->started && !o->paused) ? 1 : 0;
}

int LDVAudioOutputIsIdle(const LDVAudioOutput *o) {
    int idle;
    if (!o) return 1;
    pthread_mutex_lock((pthread_mutex_t *)&o->lock);
    idle = (o->freeCount >= o->bufferCount);
    pthread_mutex_unlock((pthread_mutex_t *)&o->lock);
    return idle;
}
