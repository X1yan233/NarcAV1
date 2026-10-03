/*
 * LDVIVFSource.h — 最小 IVF 容器读取器
 *
 * IVF 是 AV1 最简单的封装格式（32 字节文件头 + 每帧 12 字节帧头），
 * 适合做 demo 输入和单元测试的输入。
 *
 * 文件头（32 字节，全部小端）：
 *   0..3   'D' 'K' 'I' 'F'
 *   4..5   version (uint16)，应为 0
 *   6..7   header size (uint16)，应为 32
 *   8..11  fourcc（4 字节），AV1 为 'A' 'V' '0' '1'
 *   12..13 width (uint16)
 *   14..15 height (uint16)
 *   16..19 timebase denominator (uint32)
 *   20..23 timebase numerator (uint32)
 *   24..27 frame count (uint32)
 *   28..31 unused
 *
 * 帧头（12 字节，全部小端）：
 *   0..3   frame size (uint32)
 *   4..11  timestamp (uint64)
 *   之后是 frame size 字节的帧数据
 */

#ifndef LDV_IVF_SOURCE_H
#define LDV_IVF_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "LDVTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVIVFSource LDVIVFSource;

typedef struct LDVIVFFrame {
    const uint8_t *data;      /* 指向读取器内部缓冲，下一次 ReadFrame 即失效 */
    size_t         size;
    int64_t        timestamp;
} LDVIVFFrame;

/* 打开失败返回 NULL。 */
LDVIVFSource *LDVIVFSourceOpen(const char *path);

/* 关闭并释放，允许传入 NULL；返回后 *src 被置为 NULL。 */
void LDVIVFSourceClose(LDVIVFSource **src);

/*
 * 读下一帧。
 *   LDV_OK          成功
 *   LDV_ERR_EOF     正常结束
 *   LDV_ERR_BITSTREAM 容器损坏
 */
int LDVIVFSourceReadFrame(LDVIVFSource *src, LDVIVFFrame *out);

/* 读取全部帧到内存。用于单元测试与需要随机访问的场景。
 * 成功时 *outFrames 与 *outCount 已填充，
 * 调用方用 LDVIVFSourceFreeFrames(&frames, count) 释放。 */
int LDVIVFSourceLoadAll(LDVIVFSource *src,
                        LDVIVFFrame **outFrames, int *outCount);

/* 释放 LoadAll 的产物。count 必须是 LoadAll 输出的数量。 */
void LDVIVFSourceFreeFrames(LDVIVFFrame **frames, int count);

/* 元数据 */
int         LDVIVFSourceWidth(const LDVIVFSource *src);
int         LDVIVFSourceHeight(const LDVIVFSource *src);
int64_t     LDVIVFSourceDeclaredFrameCount(const LDVIVFSource *src);
void        LDVIVFSourceTimebase(const LDVIVFSource *src,
                                 uint32_t *outNumerator, uint32_t *outDenominator);
const char *LDVIVFSourceFourCC(const LDVIVFSource *src);

#ifdef __cplusplus
}
#endif

#endif /* LDV_IVF_SOURCE_H */
