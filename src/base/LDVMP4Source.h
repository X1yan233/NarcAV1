/*
 * LDVMP4Source.h — 最小 MP4 / ISOBMFF 解封装器（只取 AV1 视频轨）
 *
 * 为什么需要它
 * ------------
 * 测试素材是 MP4 封装的 AV1（`av01` 样本条目）。dav1d 只吃 AV1 OBU 流，
 * 所以必须先解封装。用 ffmpeg 转成 IVF 是最省事的做法，但：
 *   - 真实使用场景拿到的就是 MP4，库迟早需要这一层（ARCHITECTURE §3 的 Demux 模块）
 *   - 不引入外部工具链依赖
 *
 * 本实现只覆盖**播放所需的子集**，不追求完整：
 *   顶层      ftyp / moov（允许 moov 在文件末尾）/ mdat
 *   moov      mvhd(忽略) / trak / mdia / minf / stbl
 *   stbl      stsd→av01→av1C、stsz、stsc、stco/co64、stts、stss
 *
 * 关键事实（实测素材确认）
 * ------------------------
 *   - AV1-in-MP4 的样本是**裸 OBU 流，没有长度前缀**（不同于 H.264/HEVC 的
 *     length-prefixed 格式）。所以样本字节可以直接喂 dav1d_send_data()。
 *   - 序列头 OBU 在 av1C 的 configOBUs 里，且首样本通常也自带一份。
 *   - 样本表里的偏移指向 mdat 内部，mdat 头可能是 16 字节（64 位 size）。
 *
 * 内存策略
 * --------
 * 不把整个文件读进内存（素材 28 MB，iOS 4 设备吃不消）。只把**样本表**
 * （偏移 + 大小 + 时间戳）预先展开到内存里（每样本 16 字节），
 * 取帧时按需 fseek + fread 单帧。9971 个样本约 160 KB。
 */

#ifndef LDV_MP4_SOURCE_H
#define LDV_MP4_SOURCE_H

#include "LDVTypes.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVMP4Source LDVMP4Source;

/* 打开并解析。失败返回 NULL（会向 stderr 说明原因）。 */
LDVMP4Source *LDVMP4SourceOpen(const char *path);

/* 关闭并释放。传 NULL 或 *src == NULL 都安全。 */
void LDVMP4SourceClose(LDVMP4Source **src);

/* ---- 轨道信息 ---- */
int         LDVMP4SourceWidth(const LDVMP4Source *s);
int         LDVMP4SourceHeight(const LDVMP4Source *s);
const char *LDVMP4SourceCodec(const LDVMP4Source *s);     /* "av01" */
uint32_t    LDVMP4SourceTimescale(const LDVMP4Source *s);
int64_t     LDVMP4SourceFrameCount(const LDVMP4Source *s);
double      LDVMP4SourceFPS(const LDVMP4Source *s);       /* 0 表示未知 */

/* 最大的单样本字节数。用于给解码器/缓存预留缓冲。 */
size_t      LDVMP4SourceMaxSampleSize(const LDVMP4Source *s);

/*
 * 首样本的 PTS（timescale 单位）与它的秒值。
 *
 * 音画同步要用：两条轨的时间轴起点不一定相同（也未必都是 0）。
 * 只有各自减去自己的起点、换算成秒之后，才是同一条时间轴上的位置。
 */
int64_t     LDVMP4SourceFirstPts(const LDVMP4Source *s);
double      LDVMP4SourceFirstPtsSeconds(const LDVMP4Source *s);
int64_t     LDVMP4SourceAudioFirstPts(const LDVMP4Source *s);
double      LDVMP4SourceAudioFirstPtsSeconds(const LDVMP4Source *s);

/* ---- 取样本 ----
 *
 * index 从 0 开始。成功时 *outData 指向 malloc 出来的裸 OBU 字节，
 * 由调用方 free()；*outSize 是字节数；*outTimestamp 是 timescale 单位的
 * 显示时间戳（PTS，从 0 开始）。
 *
 * 越界返回 LDV_ERR_EOF。
 */
LDVResult LDVMP4SourceReadSample(LDVMP4Source *s, int64_t index,
                                 uint8_t **outData, size_t *outSize,
                                 int64_t *outTimestamp);

/* 是否随机访问点（stss 里列出的同步样本）。没有 stss 时全部返回 1。 */
int LDVMP4SourceIsSyncSample(const LDVMP4Source *s, int64_t index);

/* av1C 里的 configOBUs（含序列头 OBU）。没有则返回 LDV_ERR_UNSUPPORTED。
 * 指针由 source 持有，不要 free。 */
LDVResult LDVMP4SourceGetConfigOBUs(const LDVMP4Source *s,
                                    const uint8_t **outData, size_t *outSize);

/* ------------------------------------------------------------------------ */
/* 音频轨（AAC / mp4a）                                                      */
/* ------------------------------------------------------------------------ */
/*
 * 音频与视频是**两条独立的轨**，各自有：
 *   - 自己的 mdhd timescale（实测素材视频 15360、音频 44100）
 *   - 自己的样本表（音频的"样本"= 一个 AAC 访问单元，通常 1024 采样）
 *   - 自己的时间轴起点
 * 所以下面每个访问器都成对出现，不要拿视频的 timescale 去换算音频时间戳。
 *
 * 容器里没有音轨时：HasAudio() 返回 0，其余返回 0 / 空串，
 * ReadAudioSample / GetAudioConfig 返回 LDV_ERR_UNSUPPORTED。
 */

int         LDVMP4SourceHasAudio(const LDVMP4Source *s);
const char *LDVMP4SourceAudioCodec(const LDVMP4Source *s);      /* "mp4a" */
uint32_t    LDVMP4SourceAudioTimescale(const LDVMP4Source *s);
int         LDVMP4SourceAudioSampleRate(const LDVMP4Source *s); /* Hz */
int         LDVMP4SourceAudioChannels(const LDVMP4Source *s);
int64_t     LDVMP4SourceAudioFrameCount(const LDVMP4Source *s);
double      LDVMP4SourceAudioDuration(const LDVMP4Source *s);   /* 秒 */
size_t      LDVMP4SourceAudioMaxSampleSize(const LDVMP4Source *s);

/*
 * 读一个音频样本（= 一个 AAC 访问单元，**裸数据、无 ADTS 头**）。
 * 语义与 LDVMP4SourceReadSample 完全一致：*outData 由调用方 free()。
 */
LDVResult LDVMP4SourceReadAudioSample(LDVMP4Source *s, int64_t index,
                                      uint8_t **outData, size_t *outSize,
                                      int64_t *outTimestamp);

/*
 * esds 里的 AudioSpecificConfig（AAC 解码器要的"魔数"）。
 * 用 AudioConverterSetProperty(kAudioConverterDecompressionMagicCookie)
 * 原样喂给 AudioToolbox 即可。指针由 source 持有，不要 free。
 */
LDVResult LDVMP4SourceGetAudioConfig(const LDVMP4Source *s,
                                     const uint8_t **outData, size_t *outSize);

#ifdef __cplusplus
}
#endif

#endif /* LDV_MP4_SOURCE_H */
