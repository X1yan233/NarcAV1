/*
 * LDVAudioSession.c — 音频会话：后台解码线程 + AudioQueue 输出 + 主时钟
 *
 * 设计说明见 LDVAudioSession.h。三个实现要点：
 *
 * 1) 自己开一个 LDVMP4Source
 *    视频会话已经开了一个。两边共用同一个 LDVMP4Source 会有**文件读竞争**：
 *    LDVMP4SourceReadSample 用的是 fseeko + fread，两条线程各自 seek
 *    会把对方的位置冲掉，读到错的样本 —— 而且不会报错，只是画面/声音
 *    偶尔错乱，极难查。
 *    所以音频会话开**自己的一份**（独立的 FILE* + 独立的样本表）。
 *    代价是多一份样本表内存（实测音频 9441 样本 × 24 B ≈ 226 KB），
 *    换来的是两条线程零共享、零锁。
 *
 * 2) 解码线程不会死等
 *    输出队列满时用带超时的等待，醒来重查 stop 标志。
 *    这样 Stop() 能立刻生效，不会被"等空闲缓冲"卡住。
 *
 * 3) 单帧失败只跳一帧
 *    解码/读取失败都补一段等长静音继续。**不中断音频线程**，
 *    也不向上抛"会话级"错误 —— 需求允许轻微卡顿，但要求稳定。
 */

#include "LDVAudioSession.h"

#include "LDVAudioDecoder.h"
#include "LDVAudioOutput.h"
#include "LDVMP4Source.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* 写输出时单次等待的粒度（毫秒）。只影响停止响应速度。 */
#define LDV_AUDIO_WRITE_SLICE_MS 60

struct LDVAudioSession {
    LDVMP4Source   *source;      /* ★ 本会话私有，不与视频会话共享 */
    LDVAudioDecoder *decoder;
    LDVAudioOutput *output;

    pthread_t       thread;
    int             threadValid;
    volatile int    stop;
    volatile int    running;
    volatile int    paused;
    volatile int    eof;
    volatile int    looping;

    int64_t         nextSample;
    int64_t         droppedPackets;
    int64_t         loopCount;

    /* 音轨首样本 PTS（timescale 单位）与它的秒值 */
    int64_t         firstPts;
    int             firstPtsValid;
    double          firstPtsSeconds;

    /* 解码输出缓冲（同时用于补静音） */
    uint8_t        *pcm;
    size_t          pcmCapacity;

    /*
     * SyncToMediaTime → Start 的接力标志。
     * Start 里有一段"从头起播"的状态复位；seek 之后重启时这段复位
     * 会把同步位置洗回 0，所以用这个标志让 Start 知道"别复位"。
     */
    volatile int    pendingSync;

    char            lastError[192];
};

/* ------------------------------------------------------------------------ */
/* 错误记录                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 最近一次 LDVAudioSessionCreate 失败在**哪一步**。
 *
 * 和 LDVDecodeSessionCreateError 同样的理由：Create 有 6 条失败路径
 * （打不开 MP4 / 没有音轨 / 编码不支持 / 没有 AudioSpecificConfig /
 * 解码器建不起来 / 输出建不起来），但对外都只返回 NULL。
 * 排查方向完全不同 —— UI 上只显示"音频 无"等于什么都没说。
 *
 * Create 只在启动时单线程调用，用静态变量即可。
 */
static char ldv_audioCreateError[192];

const char *LDVAudioSessionCreateError(void) {
    return ldv_audioCreateError[0] ? ldv_audioCreateError : NULL;
}

static void ldv_audio_set_create_error(const char *msg) {
    if (!msg) return;
    snprintf(ldv_audioCreateError, sizeof(ldv_audioCreateError), "%s", msg);
}

static void ldv_audio_session_error(LDVAudioSession *s, const char *msg) {
    if (!s || !msg) return;
    /* 只留第一条 —— 后面的多半是同一原因的重复噪音 */
    if (s->lastError[0] == '\0') {
        snprintf(s->lastError, sizeof(s->lastError), "%s", msg);
    }
}

/* ------------------------------------------------------------------------ */
/* 写输出 / 补静音                                                           */
/* ------------------------------------------------------------------------ */

/*
 * 把一段 PCM 写进输出队列。队列满就等一小段再试，期间重查 stop。
 * 返回 1 = 写成功，0 = 会话已停（调用方应当退出）。
 */
static int ldv_audio_push(LDVAudioSession *s, const void *pcm, size_t bytes) {
    for (;;) {
        LDVResult r;

        if (s->stop) return 0;

        r = LDVAudioOutputWrite(s->output, pcm, bytes, LDV_AUDIO_WRITE_SLICE_MS);
        if (r == LDV_OK)         return 1;
        if (r == LDV_ERR_EOF)    return 0;    /* 输出已停 */
        if (r == LDV_ERR_NEED_INPUT) continue; /* 队列满：等过一轮再试 */

        /* 其它错误（极少）：记一条，丢掉这段，不中断线程 */
        ldv_audio_session_error(s, "音频输出写入失败");
        return 1;
    }
}

/*
 * 补一段静音。
 *
 * 用在两种地方：
 *   - 某个 AAC 帧解码失败 → 用等长静音占住时间轴，
 *     否则后续音频会整体提前，和画面越差越多
 *   - 样本读取失败 → 同上
 *
 * frames 按"输出采样率"算（不是源采样率 —— HE-AAC 下两者不同）。
 */
static void ldv_audio_push_silence(LDVAudioSession *s, int frames) {
    size_t bytes;
    const int ch = LDVAudioOutputChannels(s->output);

    if (frames <= 0 || ch <= 0) return;

    bytes = (size_t)frames * (size_t)ch * 2;
    if (bytes > s->pcmCapacity) bytes = s->pcmCapacity;
    if (bytes == 0) return;

    memset(s->pcm, 0, bytes);
    ldv_audio_push(s, s->pcm, bytes);
}

/* ------------------------------------------------------------------------ */
/* 解码线程                                                                  */
/* ------------------------------------------------------------------------ */

static void *ldv_audio_thread_main(void *arg) {
    LDVAudioSession *s = (LDVAudioSession *)arg;
    const int64_t total = LDVMP4SourceAudioFrameCount(s->source);

    /*
     * iOS 5 真机的候选故障形态：建转换器时一切正常、解码时逐帧被拒
     * （全程静音，上层只见"解码失败"）。用"开头连续失败计数"识别它：
     *   - 满 LDV_AUDIO_RETRY_AFTER_N 包且还没试过截短魔数 → 换新转换器重试
     *   - 仍失败则继续静音路径，错误详情已在 decoder 的 lastError 里，
     *     状态栏的 ⚠ 会显示 —— 那一行日志就是定位依据
     */
#define LDV_AUDIO_RETRY_AFTER_N 24
    int consecFail = 0;
    int retried    = 0;

    s->running = 1;

    while (!s->stop) {
        uint8_t  *data = NULL;
        size_t    size = 0;
        int64_t   pts  = 0;
        size_t    outBytes = 0;
        LDVResult r;

        /* ---- EOF / 循环 ---- */
        if (s->nextSample >= total) {
            if (s->looping && !s->stop) {
                s->nextSample = 0;
                s->loopCount++;
                continue;
            }
            s->eof = 1;
            break;
        }

        /* ---- 读样本 ---- */
        r = LDVMP4SourceReadAudioSample(s->source, s->nextSample,
                                        &data, &size, &pts);
        if (r != LDV_OK) {
            /*
             * 读不出来（文件被截断 / 样本表与 mdat 不符）：
             * 跳一帧、补静音，继续往下走 —— 不中断整条音频线。
             */
            s->droppedPackets++;
            ldv_audio_session_error(s, "音频样本读取失败，已跳过");
            ldv_audio_push_silence(s, LDVAudioDecoderFramesPerPacket(s->decoder));
            s->nextSample++;
            continue;
        }

        if (!s->firstPtsValid) {
            s->firstPts        = pts;
            s->firstPtsValid   = 1;
            s->firstPtsSeconds = (double)pts
                               / (double)LDVMP4SourceAudioTimescale(s->source);
        }

        /* ---- 解码 ---- */
        r = LDVAudioDecoderDecode(s->decoder, data, size,
                                  s->pcm, s->pcmCapacity, &outBytes);
        free(data);

        if (r != LDV_OK) {
            /*
             * 单帧解码失败：跳过 + 补等长静音。
             * ★ 关键是"等长" —— 不补的话时间轴会往前挤，
             *   累积起来就是明显的音画不同步。
             */
            s->droppedPackets++;
            ldv_audio_push_silence(s, LDVAudioDecoderFramesPerPacket(s->decoder));
            s->nextSample++;
            if (++consecFail == LDV_AUDIO_RETRY_AFTER_N && !retried) {
                retried = 1;
                fprintf(stderr, "LDVAudio: 开头 %d 包连续解码失败，"
                                "最后错误：%s\n",
                        consecFail,
                        LDVAudioDecoderLastError(s->decoder)
                            ? LDVAudioDecoderLastError(s->decoder) : "?");
                if (LDVAudioDecoderRetryWithTruncatedCookie(s->decoder)
                        != LDV_OK) {
                    fprintf(stderr, "LDVAudio: 截短魔数不可用，"
                                    "按静音路径继续（后续错误见状态栏 ⚠）\n");
                }
            }
            continue;
        }
        consecFail = 0;

        /* ---- 写输出（outBytes 可能为 0，某些帧不产出采样，正常）---- */
        if (outBytes > 0) {
            if (!ldv_audio_push(s, s->pcm, outBytes)) break;
        }

        s->nextSample++;
    }

    s->running = 0;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

LDVAudioSession *LDVAudioSessionCreate(const char *mp4Path,
                                       int bufferCount,
                                       int framesPerBuffer) {
    LDVAudioSession *s;
    const uint8_t *cookie = NULL;
    size_t cookieSize = 0;
    int outRate, outChannels;

    if (!mp4Path) return NULL;

    ldv_audioCreateError[0] = '\0';

    s = (LDVAudioSession *)calloc(1, sizeof(*s));
    if (!s) {
        ldv_audio_set_create_error("会话结构分配失败（内存不足）");
        return NULL;
    }

    /* ---- 自己开一份 MP4（不与视频会话共享 FILE*，见文件头说明）---- */
    s->source = LDVMP4SourceOpen(mp4Path);
    if (!s->source) {
        fprintf(stderr, "LDVAudio: 打不开 %s\n", mp4Path);
        ldv_audio_set_create_error("打不开 MP4");
        free(s);
        return NULL;
    }

    if (!LDVMP4SourceHasAudio(s->source)) {
        fprintf(stderr, "LDVAudio: 容器里没有音频轨，退回纯视频\n");
        ldv_audio_set_create_error("容器里没有音频轨");
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    if (strcmp(LDVMP4SourceAudioCodec(s->source), "mp4a") != 0) {
        fprintf(stderr, "LDVAudio: 音频编码 %s 暂不支持（本版只接 AAC/mp4a）\n",
                LDVMP4SourceAudioCodec(s->source));
        snprintf(ldv_audioCreateError, sizeof(ldv_audioCreateError),
                 "音频编码 %s 暂不支持（本版只接 AAC/mp4a）",
                 LDVMP4SourceAudioCodec(s->source));
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    if (LDVMP4SourceGetAudioConfig(s->source, &cookie, &cookieSize) != LDV_OK) {
        fprintf(stderr, "LDVAudio: esds 里没有 AudioSpecificConfig，无法配置解码器\n");
        ldv_audio_set_create_error("esds 里没有 AudioSpecificConfig");
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    /* ---- 解码器 ---- */
    s->decoder = LDVAudioDecoderCreate(cookie, cookieSize,
                                       (uint32_t)LDVMP4SourceAudioSampleRate(s->source),
                                       LDVMP4SourceAudioChannels(s->source));
    if (!s->decoder) {
        ldv_audio_set_create_error(
            "AAC 解码器建不起来（AudioConverterNew/魔数被拒，见控制台 LDVAudio: 行）");
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    /* ---- 输出（用解码器**回读**的格式，HE-AAC 下与源不同）---- */
    outRate     = LDVAudioDecoderOutputSampleRate(s->decoder);
    outChannels = LDVAudioDecoderOutputChannels(s->decoder);

    s->output = LDVAudioOutputCreate((uint32_t)outRate, outChannels,
                                     bufferCount, framesPerBuffer);
    if (!s->output) {
        ldv_audio_set_create_error(
            "AudioQueue 建不起来（模拟器上偶发，见控制台 LDVAudio: 行）");
        LDVAudioDecoderDestroy(&s->decoder);
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    /* ---- 解码输出缓冲 ---- */
    s->pcmCapacity = LDVAudioDecoderMaxOutputBytes(s->decoder);
    if (s->pcmCapacity < (size_t)outChannels * 2 * 1024) {
        s->pcmCapacity = (size_t)outChannels * 2 * 1024;
    }
    s->pcm = (uint8_t *)malloc(s->pcmCapacity);
    if (!s->pcm) {
        LDVAudioOutputDestroy(&s->output);
        LDVAudioDecoderDestroy(&s->decoder);
        LDVMP4SourceClose(&s->source);
        free(s);
        return NULL;
    }

    return s;
}

void LDVAudioSessionDestroy(LDVAudioSession **session) {
    LDVAudioSession *s;
    if (!session || !*session) return;
    s = *session;

    LDVAudioSessionStop(s);

    if (s->output)  LDVAudioOutputDestroy(&s->output);
    if (s->decoder) LDVAudioDecoderDestroy(&s->decoder);
    if (s->source)  LDVMP4SourceClose(&s->source);
    if (s->pcm)     free(s->pcm);

    free(s);
    *session = NULL;
}

LDVResult LDVAudioSessionStart(LDVAudioSession *s) {
    int rc;

    if (!s) return LDV_ERR_INVALID_ARG;
    if (s->threadValid) {
        /* 已在跑：只确保输出在播 */
        return LDVAudioOutputStart(s->output);
    }

    s->stop     = 0;
    s->eof      = 0;
    s->paused   = 0;
    s->running  = 0;
    s->lastError[0] = '\0';

    if (s->pendingSync) {
        /*
         * seek 之后的重启：位置已由 SyncToMediaTime 设好（nextSample /
         * firstPtsSeconds），这里**不能**复位，否则音频永远从头播。
         * 只清接力标志。
         */
        s->pendingSync = 0;
    } else {
        s->nextSample = 0;
        s->loopCount  = 0;
        s->droppedPackets = 0;
        s->firstPtsValid = 0;
        s->firstPtsSeconds = 0.0;
    }

    /* 先起输出，再起解码线程 —— 反过来的话第一批数据会挤在队列里空转 */
    if (LDVAudioOutputStart(s->output) != LDV_OK) {
        ldv_audio_session_error(s, "音频输出启动失败");
        return LDV_ERR_UNSUPPORTED;
    }

    rc = pthread_create(&s->thread, NULL, ldv_audio_thread_main, s);
    if (rc != 0) {
        ldv_audio_session_error(s, "无法创建音频解码线程");
        return LDV_ERR_NOMEM;
    }
    s->threadValid = 1;
    return LDV_OK;
}

void LDVAudioSessionPause(LDVAudioSession *s) {
    if (!s) return;
    LDVAudioOutputPause(s->output);
    s->paused = 1;
}

LDVResult LDVAudioSessionResume(LDVAudioSession *s) {
    if (!s) return LDV_ERR_INVALID_ARG;
    if (!s->paused) return LDV_OK;
    s->paused = 0;
    return LDVAudioOutputResume(s->output);
}

void LDVAudioSessionStop(LDVAudioSession *s) {
    if (!s) return;

    if (s->threadValid) {
        s->stop = 1;
        pthread_join(s->thread, NULL);
        s->threadValid = 0;
    }

    s->running = 0;
    s->paused  = 0;

    /*
     * ★ 接力标志必须在这里清掉：SyncToMediaTime 的顺序是
     *   Stop → 设位置 → 置 pendingSync → （调用方）Start。
     *   而"整停之后再 Start"（从头重播 / 切 DSP）应当从 0 开始 ——
     *   如果 pendingSync 残留，Start 会保留旧同步位置，音频就永远
     *   回不到开头了。Stop 清零 + Sync 在 Stop **之后**置位，两条路都对。
     */
    s->pendingSync = 0;

    if (s->output) LDVAudioOutputStop(s->output);
}

/* ------------------------------------------------------------------------ */
/* 主时钟                                                                    */
/* ------------------------------------------------------------------------ */

double LDVAudioSessionMediaTime(LDVAudioSession *s) {
    if (!s || !s->output) return 0.0;

    /*
     * 时钟 = 音轨首样本 PTS(秒) + 已播放采样数 / 输出采样率
     *
     * 首样本 PTS 不能省：音视频两条轨的起点不一定都是 0
     * （实测素材都是 0，但这是巧合，不能写死）。少了这一项，
     * 音频起点不为 0 的文件会整体偏一个固定量。
     *
     * 用"已播放"而不是"已入队"：入队量领先真实播放一个队列深度
     * （默认约 93 ms），拿它当基准画面会稳定领先声音 0.1 秒。
     */
    return s->firstPtsSeconds + LDVAudioOutputClockSeconds(s->output);
}

double LDVAudioSessionFirstPtsSeconds(const LDVAudioSession *s) {
    return s ? s->firstPtsSeconds : 0.0;
}

/* ------------------------------------------------------------------------ */
/* 查询                                                                      */
/* ------------------------------------------------------------------------ */

int LDVAudioSessionOutputSampleRate(const LDVAudioSession *s) {
    return (s && s->decoder) ? LDVAudioDecoderOutputSampleRate(s->decoder) : 0;
}

int LDVAudioSessionOutputChannels(const LDVAudioSession *s) {
    return (s && s->decoder) ? LDVAudioDecoderOutputChannels(s->decoder) : 0;
}

const char *LDVAudioSessionSourceCodec(const LDVAudioSession *s) {
    return (s && s->source) ? LDVMP4SourceAudioCodec(s->source) : "";
}

int LDVAudioSessionSourceSampleRate(const LDVAudioSession *s) {
    return (s && s->source) ? LDVMP4SourceAudioSampleRate(s->source) : 0;
}

int LDVAudioSessionSourceChannels(const LDVAudioSession *s) {
    return (s && s->source) ? LDVMP4SourceAudioChannels(s->source) : 0;
}

int64_t LDVAudioSessionTotalPackets(const LDVAudioSession *s) {
    return (s && s->source) ? LDVMP4SourceAudioFrameCount(s->source) : 0;
}

int LDVAudioSessionIsRunning(const LDVAudioSession *s) {
    return s ? s->running : 0;
}

int LDVAudioSessionIsEOF(const LDVAudioSession *s) {
    return s ? s->eof : 0;
}

int LDVAudioSessionIsPaused(const LDVAudioSession *s) {
    return s ? s->paused : 0;
}

int LDVAudioSessionLoopCount(const LDVAudioSession *s) {
    return s ? (int)s->loopCount : 0;
}

int64_t LDVAudioSessionDecodedPackets(const LDVAudioSession *s) {
    return (s && s->decoder) ? LDVAudioDecoderDecodedPackets(s->decoder) : 0;
}

int64_t LDVAudioSessionDecodedFrames(const LDVAudioSession *s) {
    return (s && s->decoder) ? LDVAudioDecoderDecodedFrames(s->decoder) : 0;
}

int64_t LDVAudioSessionDroppedPackets(const LDVAudioSession *s) {
    return s ? s->droppedPackets : 0;
}

int64_t LDVAudioSessionUnderruns(const LDVAudioSession *s) {
    return (s && s->output) ? LDVAudioOutputUnderruns(s->output) : 0;
}

int64_t LDVAudioSessionQueuedFrames(const LDVAudioSession *s) {
    if (!s || !s->output) return 0;
    return LDVAudioOutputEnqueuedFrames(s->output);
}

int LDVAudioSessionFreeBuffers(const LDVAudioSession *s) {
    return (s && s->output) ? LDVAudioOutputFreeBuffers(s->output) : 0;
}

int64_t LDVAudioSessionErrorCount(const LDVAudioSession *s) {
    return (s && s->decoder) ? LDVAudioDecoderErrorCount(s->decoder) : 0;
}

const char *LDVAudioSessionLastError(const LDVAudioSession *s) {
    if (!s) return NULL;
    if (s->lastError[0] != '\0') return s->lastError;
    if (s->decoder) return LDVAudioDecoderLastError(s->decoder);
    return NULL;
}

void LDVAudioSessionSetLooping(LDVAudioSession *s, int loop) {
    if (s) s->looping = loop ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
/* 按时间重同步（seek）                                                       */
/* ------------------------------------------------------------------------ */

LDVResult LDVAudioSessionSyncToMediaTime(LDVAudioSession *s,
                                         double mediaSeconds) {
    int64_t total, i;
    double  tscale;

    if (!s || !s->source) return LDV_ERR_INVALID_ARG;
    total = LDVMP4SourceAudioFrameCount(s->source);
    if (total <= 0) return LDV_ERR_UNSUPPORTED;

    tscale = (double)LDVMP4SourceAudioTimescale(s->source);
    if (tscale <= 0.0) return LDV_ERR_UNSUPPORTED;

    /*
     * 先整停：停线程（join）+ 停输出（清队列、时钟归零）。
     * 之后由调用方 Start 重启 —— pendingSync 标志保证 Start 保留
     * 下面设好的位置。Stop 是幂等的，线程没在跑也无害。
     */
    LDVAudioSessionStop(s);

    /*
     * 线性扫描样本表找第一个 PTS >= 目标 的样本。
     *
     * 为什么不建二分：音频样本量级是几千（实测 9441），这个操作只在
     * "压测后恢复 / 长暂停后恢复"时发生，一次全扫 < 10ms，不值得为它
     * 在 MP4Source 里加按 pts 索引的接口。
     * 目标超过末尾 → 找不到 → 落回从头（调用方不必特判）。
     */
    for (i = 0; i < total; i++) {
        uint8_t *data = NULL;
        size_t   size = 0;
        int64_t  pts  = 0;
        LDVResult r = LDVMP4SourceReadAudioSample(s->source, i,
                                                  &data, &size, &pts);
        if (data) free(data);
        if (r != LDV_OK) break;               /* 样本表有问题：落回从头 */

        if ((double)pts / tscale >= mediaSeconds - 0.002) {
            s->nextSample       = i;
            s->firstPts         = pts;
            s->firstPtsValid    = 1;
            s->firstPtsSeconds  = (double)pts / tscale;
            s->eof              = 0;
            s->pendingSync      = 1;
            fprintf(stderr,
                    "LDVAudio: 重同步到 %.2f s（样本 %lld/%lld，pts %.3f s）\n",
                    mediaSeconds, (long long)i, (long long)total,
                    s->firstPtsSeconds);
            return LDV_OK;
        }
    }

    s->nextSample      = 0;
    s->firstPtsValid   = 0;
    s->firstPtsSeconds = 0.0;
    s->eof             = 0;
    s->pendingSync     = 1;
    fprintf(stderr, "LDVAudio: 重同步目标 %.2f s 超出音轨长度，落回从头\n",
            mediaSeconds);
    return LDV_OK;
}

int LDVAudioSessionOutputIdle(const LDVAudioSession *s) {
    return (s && s->output) ? LDVAudioOutputIsIdle(s->output) : 1;
}
