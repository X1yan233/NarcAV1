/*
 * LDVAudioDecoder.h — AAC 解码（AudioToolbox AudioConverter 封装）
 *
 * 为什么用 AudioToolbox 而不是自带解码器
 * --------------------------------------
 * 容器里的音频是 AAC（`mp4a`）。AAC 解码在 iOS 上是**系统自带**的：
 *   AudioToolbox.framework → AudioConverter → kAudioFormatMPEG4AAC
 * 从 iOS 2.0 就有，iPhone 4S / iOS 5.1.1 完全支持。
 *
 * 所以音频这条线**不需要引入任何外部依赖**（不像 Opus / FLAC 那样
 * 必须带一份第三方解码器）。这是"按容器里的实际编码格式处理"最省
 * 的实现路径：容器给什么格式，就用系统对应的解码器。
 *
 * 支持的格式
 * ----------
 *   mp4a / AAC-LC（实测素材就是它）      ✓ 本文件的默认路径
 *   mp4a / HE-AAC（带 SBR 扩展）          ✓ 同一个 kAudioFormatMPEG4AAC
 *   alac                                  ✓ 换 mFormatID 即可（未接）
 *   mp3 / ac-3                            ✓ 同上（未接）
 * 目前只接了 AAC —— 容器里出现别的编码时 Create 会失败并报出来，
 * 不会静默出噪音。
 *
 * 输入/输出格式
 * -------------
 *   输入：**裸 AAC 访问单元**（MP4 的样本本身就是，没有 ADTS 头），
 *         外加 esds 里的 AudioSpecificConfig 当"魔数"。
 *   输出：交错 PCM，16 位有符号，本机字节序，采样率与声道数跟随源。
 *
 * iOS 4/5 兼容要点
 * ----------------
 *   - 用 AudioConverterFillComplexBuffer（iOS 2.0+）。
 *     AudioConverterFillBuffer 虽然也能用，但它是同步拉取的旧接口，
 *     且在老系统上对可变码率输入的边界处理更脆。
 *   - 不用 AudioConverterReset 之外的新 API。
 */

#ifndef LDV_AUDIO_DECODER_H
#define LDV_AUDIO_DECODER_H

#include "LDVTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVAudioDecoder LDVAudioDecoder;

/*
 * 创建解码器。
 *
 *   magicCookie    esds 里的 AudioSpecificConfig（LDVMP4SourceGetAudioConfig）
 *   cookieSize     它的字节数
 *   sampleRate     源采样率（AudioSampleEntry 里的，Hz）
 *   channels       源声道数
 *
 * 失败返回 NULL（会向 stderr 说明原因）。
 *
 * ★ magicCookie 必须来自容器。它描述的是 profile / 采样率 / 声道数 /
 *   帧长等解码必需信息；少了它 AudioConverterNew 会直接失败。
 */
LDVAudioDecoder *LDVAudioDecoderCreate(const uint8_t *magicCookie,
                                       size_t cookieSize,
                                       uint32_t sampleRate,
                                       int channels);

/* 销毁。传 NULL 或 *dec == NULL 都安全。 */
void LDVAudioDecoderDestroy(LDVAudioDecoder **dec);

/*
 * ★ 运行期兜底：换一个全新的转换器、改用 2 字节（AAC-LC）截短魔数。
 *
 * 用在"创建成功、解码却从头到尾全失败"的场合 —— 老系统（iOS 8 及更早）
 * 的候选故障形态：建转换器不报错，解码时逐帧被拒，表现为全程静音但
 * 状态栏显示音频正常。整个换新转换器而不是只改属性：第一次设置失败
 * 可能把编解码器实例内部状态搞脏，新系统能自愈、老系统未必。
 *
 * 已经用截短魔数、或 ASC 不是 AAC-LC 时返回 LDV_ERR_UNSUPPORTED
 * （没有下一步可退）。成功后应重读 OutputSampleRate / OutputChannels
 * （输出队列建好后不变，这里只是让诊断数字一致）。
 */
LDVResult LDVAudioDecoderRetryWithTruncatedCookie(LDVAudioDecoder *dec);

/*
 * 解码一个 AAC 访问单元。
 *
 *   in          裸 AAC 帧
 *   inSize      字节数
 *   out         输出缓冲（交错 PCM）
 *   outCapacity 输出缓冲字节数
 *   outBytes    实际写入的字节数
 *
 * 返回：
 *   LDV_OK             成功（*outBytes 可能为 0 —— AAC 里正常，
 *                      例如某些帧只更新参数不产出采样）
 *   LDV_ERR_INVALID_ARG / LDV_ERR_NOMEM / LDV_ERR_BITSTREAM
 *
 * ★ 解码失败**不是致命错误**。上层应当跳过这一帧、补一段静音继续，
 *   而不是中断音频线程 —— 这是需求里的容错优先级。
 */
LDVResult LDVAudioDecoderDecode(LDVAudioDecoder *dec,
                                const uint8_t *in, size_t inSize,
                                uint8_t *out, size_t outCapacity,
                                size_t *outBytes);

/* 解码器实际的输出格式（可能与源不同，例如 HE-AAC 的 SBR 会把
 * 采样率翻倍）。音频主时钟必须用**这个**采样率换算。 */
int LDVAudioDecoderOutputSampleRate(const LDVAudioDecoder *dec);
int LDVAudioDecoderOutputChannels(const LDVAudioDecoder *dec);

/* 一个 AAC 帧最多解出多少字节的 PCM。用来给输出缓冲定容量。 */
size_t LDVAudioDecoderMaxOutputBytes(const LDVAudioDecoder *dec);

/* 每个 AAC 帧解出的 PCM 帧数（AAC-LC 是 1024）。用于补静音。 */
int LDVAudioDecoderFramesPerPacket(const LDVAudioDecoder *dec);

/* 诊断 */
int64_t LDVAudioDecoderDecodedPackets(const LDVAudioDecoder *dec);
int64_t LDVAudioDecoderDecodedFrames(const LDVAudioDecoder *dec);
int64_t LDVAudioDecoderErrorCount(const LDVAudioDecoder *dec);
const char *LDVAudioDecoderLastError(const LDVAudioDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* LDV_AUDIO_DECODER_H */
