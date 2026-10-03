/*
 * LDVAudioDecoder.c — AAC 解码（AudioToolbox AudioConverter 封装）
 *
 * 设计说明见 LDVAudioDecoder.h。这里只强调三个实现上的取舍：
 *
 * 1) 一次只喂一个访问单元
 *    AudioConverter 支持一次喂多包，但我们的输入是 MP4 样本表里的
 *    独立帧，而且**每帧的 PTS 都要能对上**。一次喂一帧，出问题
 *    只影响这一帧，也便于上层"跳一帧补静音"。
 *
 * 2) 变码率必须给包描述
 *    AAC 是 VBR，解码器不知道一包的边界在哪。输入回调必须通过
 *    outDataPacketDescription 交回 {mStartOffset, mDataByteSize}，
 *    否则多帧连喂时会解错。单帧喂也要给 —— 这是 AudioConverter 的
 *    约定，不是可选项。
 *
 * 3) 输出格式要**回读**
 *    AudioConverterNew 会按自己的判断调整输出格式（HE-AAC 的 SBR
 *    会把采样率翻倍）。所以创建后必须
 *    AudioConverterGetProperty(kAudioConverterCurrentOutputStreamDescription)
 *    把实际格式读回来。音频主时钟用错采样率会让同步整体偏 ——
 *    而且偏得很隐蔽（声音听着正常，只是和画面对不上）。
 */

#include "LDVAudioDecoder.h"

#include <AudioToolbox/AudioToolbox.h>

#include <TargetConditionals.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* AAC-LC 一帧 1024 采样；HE-AAC 的核心层也是 1024 */
#define LDV_AAC_FRAMES_PER_PACKET 1024

struct LDVAudioDecoder {
    AudioConverterRef conv;
    int               valid;

    /* 源格式（创建时传入） */
    uint32_t          srcSampleRate;
    int               srcChannels;

    /* 魔数（重建转换器时要用 —— iOS5 兜底会整个换一个转换器） */
    uint8_t           cookie[8];
    size_t            cookieSize;
    int               cookieIsLC;       /* AOT == 2（截短才有意义） */
    int               usedTruncated;    /* 当前转换器用的是 2 字节魔数 */

    /* 实际输出格式（创建后回读） */
    uint32_t          outSampleRate;
    int               outChannels;
    size_t            bytesPerFrame;      /* 交错，所以 = 声道数 x 2 */
    size_t            maxOutputBytes;

    int64_t           decodedPackets;
    int64_t           decodedFrames;
    int64_t           errorCount;

    char              lastError[192];
};

/* ------------------------------------------------------------------------ */
/* 错误记录                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * OSStatus 转可读 FourCC（大端字节序拼装，与 'prop'/'!dat' 的书写约定一致）。
 * 返回静态缓冲 —— 只用于 fprintf，不可重入但这里都是单线程路径。
 * 十六进制错误码信息量太低；'prop'/'!dat' 一眼就能指向"属性不支持/
 * 数据被拒"。
 */
static const char *ldv_fourcc(OSStatus st) {
    static char buf[5];
    uint32_t v = (uint32_t)st;
    buf[0] = (char)(v >> 24);
    buf[1] = (char)(v >> 16);
    buf[2] = (char)(v >> 8);
    buf[3] = (char)v;
    buf[4] = '\0';
    return buf;
}

static void ldv_audio_set_error(LDVAudioDecoder *d, const char *what,
                                OSStatus st) {
    if (!d) return;
    d->errorCount++;
    /* 只留第一条 —— 后面的多半是同一原因的重复噪音 */
    if (d->lastError[0] == '\0') {
        snprintf(d->lastError, sizeof(d->lastError), "%s（OSStatus %d）",
                 what, (int)st);
    }
}

/* ------------------------------------------------------------------------ */
/* 输入回调                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t         size;
    int            consumed;
    int            channels;
    AudioStreamPacketDescription pkt;
} LDVAudioInputCtx;

/*
 * AudioConverter 向我们要输入。
 *
 * 约定：
 *   - 没有更多数据时把 *ioNumberDataPackets 置 0 并返回 noErr
 *     （不要返回错误，那是"真出错"的意思）
 *   - ioData 里的缓冲区指针指向**我们自己的内存**，解码器不会改它，
 *     但必须在 FillComplexBuffer 返回前保持有效
 *   - 变码率必须填 outDataPacketDescription
 */
static OSStatus ldv_audio_input_proc(AudioConverterRef converter,
                                     UInt32 *ioNumberDataPackets,
                                     AudioBufferList *ioData,
                                     AudioStreamPacketDescription **outPktDesc,
                                     void *userData) {
    LDVAudioInputCtx *ctx = (LDVAudioInputCtx *)userData;
    (void)converter;

    if (!ctx || !ioNumberDataPackets || !ioData) return noErr;

    if (ctx->consumed || *ioNumberDataPackets == 0) {
        *ioNumberDataPackets = 0;
        return noErr;
    }

    if (*ioNumberDataPackets > 1) *ioNumberDataPackets = 1;   /* 一次一帧 */

    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = (UInt32)ctx->channels;
    ioData->mBuffers[0].mDataByteSize   = (UInt32)ctx->size;
    ioData->mBuffers[0].mData           = (void *)ctx->data;

    ctx->consumed = 1;

    if (outPktDesc) {
        ctx->pkt.mStartOffset            = 0;
        ctx->pkt.mDataByteSize           = (UInt32)ctx->size;
        ctx->pkt.mVariableFramesInPacket = 0;
        *outPktDesc = &ctx->pkt;
    }

    return noErr;
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 老系统判定：Darwin 主版本 < 15 即 iOS 8 及更早。
 *
 * 为什么要分：实测 iOS 9 对 5 字节 ASC（带 GASpecificConfig 扩展）
 * 来者不拒，iOS 5 的 AAC 编解码器则会拒收（模拟器 '陌生' 码、真机各异的
 * 错误码都有报告）。AAC-LC 的全部必要信息（对象类型/采样率/声道）就在
 * 前 2 字节，老系统**优先**用截短魔数可以少走一次失败路径 ——
 * 而且避免"第一次设置失败把转换器内部状态搞脏"这类说不清的问题。
 */
static int ldv_audio_is_legacy_os(void) {
#if defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
    /*
     * 模拟器的 kern.osrelease 是**宿主 Mac 的内核版本**（10.9.5 宿主 =
     * Darwin 13），按 iOS 映射会误判成"iOS 8 及更早"。模拟器一律按
     * 现代序走 —— 反正下面的尝试序对两种顺序都有兜底。
     */
    return 0;
#else
    char rel[32];
    size_t sz = sizeof(rel);
    int major = 0, i;

    memset(rel, 0, sizeof(rel));
    if (sysctlbyname("kern.osrelease", rel, &sz, NULL, 0) != 0) return 0;
    for (i = 0; rel[i] >= '0' && rel[i] <= '9'; i++) {
        major = major * 10 + (rel[i] - '0');
    }
    /* Darwin 11=iOS5 12=6 13=7 14=8 15=9 ... */
    return (major > 0 && major < 15) ? 1 : 0;
#endif
}

/*
 * 建一个转换器并喂魔数。cookieBytes 传 0 表示用完整魔数，
 * 传 2 表示截短为前 2 字节（仅 AAC-LC 合法）。
 * inFlags 写进输入 ASBD 的 mFormatFlags —— 对 kAudioFormatMPEG4AAC，
 * 这个字段的本义就是 **MPEG-4 object type**（AudioFormat.h 的
 * kMPEG4Object_AAC_LC=2 等）。传 0 = "object type 0"，iOS 设备的
 * 编解码器一直容忍，模拟器/Mac 侧 CoreAudio 严格校验则回
 * '!dat'（kAudioCodecUnsupportedFormatError）—— v8.24 无声根因。
 * 返回 noErr = 成功（d->conv 有效），否则失败（调用方负责 Dispose）。
 */
static OSStatus ldv_audio_build_converter(LDVAudioDecoder *d, int cookieBytes,
                                          UInt32 inFlags) {
    AudioStreamBasicDescription inFmt, outFmt;
    OSStatus st;
    const uint8_t *cookie = d->cookie;
    size_t         useLen = d->cookieSize;

    if (cookieBytes == 2) {
        if (!d->cookieIsLC || d->cookieSize <= 2) return -1;
        useLen = 2;
    }

    /* ---- 源格式：AAC ---- */
    memset(&inFmt, 0, sizeof(inFmt));
    inFmt.mSampleRate       = (Float64)d->srcSampleRate;
    inFmt.mFormatID         = kAudioFormatMPEG4AAC;
    inFmt.mFormatFlags      = inFlags;              /* MPEG-4 object type */
    inFmt.mFramesPerPacket  = LDV_AAC_FRAMES_PER_PACKET;
    inFmt.mChannelsPerFrame = (UInt32)d->srcChannels;
    inFmt.mBitsPerChannel   = 0;
    inFmt.mBytesPerFrame    = 0;                    /* VBR：由包描述给出 */
    inFmt.mBytesPerPacket   = 0;

    /* ---- 目标格式：交错 PCM，16 位有符号 ---- */
    memset(&outFmt, 0, sizeof(outFmt));
    outFmt.mSampleRate       = (Float64)d->srcSampleRate;
    outFmt.mFormatID         = kAudioFormatLinearPCM;
    outFmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger |
                               kAudioFormatFlagIsPacked;   /* 不带 NonInterleaved
                                                             * → 交错 */
    outFmt.mFramesPerPacket  = 1;
    outFmt.mChannelsPerFrame = (UInt32)d->srcChannels;
    outFmt.mBitsPerChannel   = 16;
    outFmt.mBytesPerFrame    = (UInt32)d->srcChannels * 2;
    outFmt.mBytesPerPacket   = outFmt.mBytesPerFrame;

    st = AudioConverterNew(&inFmt, &outFmt, &d->conv);
    if (st != noErr || !d->conv) {
        fprintf(stderr, "LDVAudio: AudioConverterNew 失败（OSStatus %d = %.4s）"
                        " —— 该编码可能不被系统支持\n",
                (int)st, ldv_fourcc(st));
        d->conv = NULL;
        return st;
    }

    /*
     * ★ 必须用 DecompressionMagicCookie（解码），不是 CompressionMagicCookie。
     *   Compression 是给**编码器**描述输出用的；对解码器设它会得到
     *   'prop'（kAudioConverterErr_PropertyNotSupported）—— 真机中招过。
     *   Apple QA1318 与架构文档 §音视频同步 都是这个结论。
     */
    st = AudioConverterSetProperty(d->conv,
                                   kAudioConverterDecompressionMagicCookie,
                                   (UInt32)useLen, cookie);
    if (st != noErr) {
        fprintf(stderr, "LDVAudio: 设置魔数失败（%zu B，OSStatus %d = %.4s）\n",
                useLen, (int)st, ldv_fourcc(st));
        AudioConverterDispose(d->conv);
        d->conv = NULL;
        return st;
    }

    d->usedTruncated = (cookieBytes == 2);
    return noErr;
}

/* 成功建好转换器之后的公共收尾：回读输出格式 + 单帧上限 */
static void ldv_audio_readback_format(LDVAudioDecoder *d) {
    AudioStreamBasicDescription actual;
    OSStatus st;
    UInt32 sz;

    memset(&actual, 0, sizeof(actual));
    sz = (UInt32)sizeof(actual);
    st = AudioConverterGetProperty(d->conv,
                                   kAudioConverterCurrentOutputStreamDescription,
                                   &sz, &actual);
    if (st == noErr && actual.mSampleRate > 0 && actual.mChannelsPerFrame > 0) {
        d->outSampleRate = (uint32_t)actual.mSampleRate;
        d->outChannels   = (int)actual.mChannelsPerFrame;
        d->bytesPerFrame = actual.mBytesPerFrame
                         ? actual.mBytesPerFrame
                         : (size_t)actual.mChannelsPerFrame * 2;
    } else {
        /* 回读失败就按请求值兜底 */
        d->outSampleRate = d->srcSampleRate;
        d->outChannels   = d->srcChannels;
        d->bytesPerFrame = (size_t)d->srcChannels * 2;
    }

    {
        UInt32 maxOut = 0;
        sz = (UInt32)sizeof(maxOut);
        if (AudioConverterGetProperty(d->conv,
                                      kAudioConverterPropertyMaximumOutputPacketSize,
                                      &sz, &maxOut) == noErr && maxOut > 0) {
            d->maxOutputBytes = (size_t)maxOut;
        } else {
            d->maxOutputBytes = d->bytesPerFrame * LDV_AAC_FRAMES_PER_PACKET * 2;
        }
        /* 留一倍余量：HE-AAC 的 SBR 输出帧数可能翻倍 */
        d->maxOutputBytes *= 2;
    }
}

LDVAudioDecoder *LDVAudioDecoderCreate(const uint8_t *magicCookie,
                                       size_t cookieSize,
                                       uint32_t sampleRate,
                                       int channels) {
    LDVAudioDecoder *d;
    OSStatus st;
    int legacy;

    if (!magicCookie || cookieSize == 0 || cookieSize > sizeof(d->cookie) ||
        sampleRate == 0 || channels <= 0) {
        fprintf(stderr, "LDVAudio: 创建参数不完整"
                        "（魔数 %p/%zu, 采样率 %u, 声道 %d）\n",
                (const void *)magicCookie, cookieSize, sampleRate, channels);
        return NULL;
    }

    d = (LDVAudioDecoder *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->srcSampleRate = sampleRate;
    d->srcChannels   = channels;
    memcpy(d->cookie, magicCookie, cookieSize);
    d->cookieSize  = cookieSize;
    /* magicCookie[0] 高 5 位 = AudioObjectType；2 = AAC-LC */
    d->cookieIsLC  = ((magicCookie[0] >> 3) == 2) ? 1 : 0;
    legacy         = ldv_audio_is_legacy_os();

    fprintf(stderr, "LDVAudio: 创建解码器  %u Hz / %d ch  魔数 %zu B（LC=%d）"
                    "  系统 Darwin%s\n",
            sampleRate, channels, cookieSize, d->cookieIsLC,
            legacy ? "（iOS 8 及更早，走老系统优先序）" : "");

    /*
     * ★ v8.24 尝试序（'!dat' 根因修复）：flags=AOT 为正解
     * （mFormatFlags = MPEG-4 object type，见 build_converter 注释），
     * 旧 flags=0 路径全保留垫底 —— 在老设备上已验证能响的路径不会被砍。
     * 每次尝试 = 重建转换器 + 设魔数；首个 noErr 即用。
     */
    {
        struct { int trunc; UInt32 flags; } tries[4];
        int n = 0, i;
        const UInt32 aot   = (UInt32)(d->cookie[0] >> 3);   /* ASC 高 5 位 */
        const int     lcExt = (d->cookieIsLC && cookieSize > 2);

        if (!legacy) {
            /* 现代序：正解优先，旧路径兜底 */
            tries[n].trunc = 0; tries[n].flags = aot; n++;
            if (lcExt) { tries[n].trunc = 1; tries[n].flags = aot; n++; }
            tries[n].trunc = 0; tries[n].flags = 0;   n++;
            if (lcExt) { tries[n].trunc = 1; tries[n].flags = 0;   n++; }
        } else {
            /* 老系统：截短优先（iOS5 对 GASpecific 扩展拒收的实测），
             * 但 flags 同样取正解在前 */
            if (lcExt) { tries[n].trunc = 1; tries[n].flags = aot; n++; }
            tries[n].trunc = 0; tries[n].flags = aot; n++;
            if (lcExt) { tries[n].trunc = 1; tries[n].flags = 0;   n++; }
            tries[n].trunc = 0; tries[n].flags = 0;   n++;
        }

        for (i = 0; i < n; i++) {
            st = ldv_audio_build_converter(d, tries[i].trunc ? 2 : 0,
                                           tries[i].flags);
            if (st == noErr) {
                if (i > 0) {
                    fprintf(stderr,
                            "LDVAudio: 第 %d 次尝试成功（%s魔数，flags=%u）\n",
                            i + 1, tries[i].trunc ? "截短" : "完整",
                            tries[i].flags);
                }
                break;
            }
        }
    }

    if (st != noErr || !d->conv) {
        size_t i;
        fprintf(stderr, "LDVAudio: 魔数全部尝试都被拒（%zu 字节:",
                d->cookieSize);
        for (i = 0; i < d->cookieSize && i < 8; i++) {
            fprintf(stderr, " %02X", d->cookie[i]);
        }
        fprintf(stderr, "）—— 系统编解码器不支持该 ASC\n");
        if (d->conv) { AudioConverterDispose(d->conv); d->conv = NULL; }
        free(d);
        return NULL;
    }

    ldv_audio_readback_format(d);
    d->valid = 1;

    fprintf(stderr,
            "LDVAudio: 解码器就绪  AAC %u Hz / %d ch  →  PCM %u Hz / %d ch  "
            "魔数 %zu B%s  单帧上限 %zu B\n",
            sampleRate, channels, d->outSampleRate, d->outChannels,
            d->usedTruncated ? 2 : d->cookieSize,
            d->usedTruncated ? "（截短）" : "",
            d->maxOutputBytes);

    return d;
}

/*
 * ★ 运行期兜底：换一个全新的转换器、改用 2 字节截短魔数。
 *
 * 用在"创建成功了、但解码从头到尾全失败"的场合（iOS 5 真机的候选
 * 故障形态：建转换器不报错，解码时逐帧 '陌生' 码失败 → 表现为全程
 * 静音但状态栏显示音频正常）。整个换新转换器而不是只改属性 ——
 * 第一次设置失败可能把编解码器实例内部状态搞脏，iOS 9 能自愈、
 * 老系统未必。已经用截短魔数的直接拒绝（没有下一步可退）。
 */
LDVResult LDVAudioDecoderRetryWithTruncatedCookie(LDVAudioDecoder *d) {
    OSStatus st;

    if (!d || !d->valid) return LDV_ERR_INVALID_ARG;
    if (d->usedTruncated || !d->cookieIsLC || d->cookieSize <= 2)
        return LDV_ERR_UNSUPPORTED;

    fprintf(stderr, "LDVAudio: 解码持续失败 —— 换新转换器 + 截短魔数重试\n");
    if (d->conv) { AudioConverterDispose(d->conv); d->conv = NULL; }

    st = ldv_audio_build_converter(d, 2, (UInt32)(d->cookie[0] >> 3));
    if (st != noErr) {
        d->valid = 0;
        return LDV_ERR_UNSUPPORTED;
    }
    ldv_audio_readback_format(d);
    /* 换了转换器，错误史清零 —— 否则状态栏一直挂着旧错误误导人 */
    d->lastError[0] = '\0';
    d->errorCount   = 0;
    fprintf(stderr, "LDVAudio: 截短魔数转换器就绪  →  PCM %u Hz / %d ch\n",
            d->outSampleRate, d->outChannels);
    return LDV_OK;
}

void LDVAudioDecoderDestroy(LDVAudioDecoder **dec) {
    LDVAudioDecoder *d;
    if (!dec || !*dec) return;
    d = *dec;

    if (d->conv) {
        AudioConverterDispose(d->conv);
        d->conv = NULL;
    }
    free(d);
    *dec = NULL;
}

/* ------------------------------------------------------------------------ */
/* 解码                                                                      */
/* ------------------------------------------------------------------------ */

LDVResult LDVAudioDecoderDecode(LDVAudioDecoder *d,
                                const uint8_t *in, size_t inSize,
                                uint8_t *out, size_t outCapacity,
                                size_t *outBytes) {
    LDVAudioInputCtx ctx;
    AudioBufferList  abl;
    UInt32           outPackets;
    OSStatus         st;

    if (outBytes) *outBytes = 0;
    if (!d || !d->valid || !d->conv) return LDV_ERR_INVALID_ARG;
    if (!in || inSize == 0 || !out || outCapacity == 0) return LDV_ERR_INVALID_ARG;

    memset(&ctx, 0, sizeof(ctx));
    ctx.data     = in;
    ctx.size     = inSize;
    ctx.consumed = 0;
    ctx.channels = d->srcChannels;

    /*
     * 输出容量能装下多少个"输出包"。
     * 输出格式是 PCM，1 包 = 1 帧，所以直接按 bytesPerFrame 算。
     */
    outPackets = (UInt32)(outCapacity / (d->bytesPerFrame ? d->bytesPerFrame : 2));
    if (outPackets == 0) return LDV_ERR_NOMEM;

    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = (UInt32)d->outChannels;
    abl.mBuffers[0].mDataByteSize   = (UInt32)outCapacity;
    abl.mBuffers[0].mData           = out;

    st = AudioConverterFillComplexBuffer(d->conv, ldv_audio_input_proc, &ctx,
                                         &outPackets, &abl, NULL);
    if (st != noErr) {
        /*
         * 解码失败**不是致命错误**：上层会跳过这一帧、补静音继续。
         * 所以这里只记一条，不往上抛"会话级"错误。
         */
        ldv_audio_set_error(d, "AAC 解码失败", st);
        return LDV_ERR_BITSTREAM;
    }

    d->decodedPackets++;
    d->decodedFrames += (int64_t)outPackets;

    if (outBytes) *outBytes = (size_t)abl.mBuffers[0].mDataByteSize;
    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 查询                                                                      */
/* ------------------------------------------------------------------------ */

int LDVAudioDecoderOutputSampleRate(const LDVAudioDecoder *d) {
    return d ? (int)d->outSampleRate : 0;
}

int LDVAudioDecoderOutputChannels(const LDVAudioDecoder *d) {
    return d ? d->outChannels : 0;
}

size_t LDVAudioDecoderMaxOutputBytes(const LDVAudioDecoder *d) {
    return d ? d->maxOutputBytes : 0;
}

int LDVAudioDecoderFramesPerPacket(const LDVAudioDecoder *d) {
    return d ? LDV_AAC_FRAMES_PER_PACKET : 0;
}

int64_t LDVAudioDecoderDecodedPackets(const LDVAudioDecoder *d) {
    return d ? d->decodedPackets : 0;
}

int64_t LDVAudioDecoderDecodedFrames(const LDVAudioDecoder *d) {
    return d ? d->decodedFrames : 0;
}

int64_t LDVAudioDecoderErrorCount(const LDVAudioDecoder *d) {
    return d ? d->errorCount : 0;
}

const char *LDVAudioDecoderLastError(const LDVAudioDecoder *d) {
    if (!d || d->lastError[0] == '\0') return NULL;
    return d->lastError;
}
