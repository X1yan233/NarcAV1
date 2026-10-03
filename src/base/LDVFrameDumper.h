/*
 * LDVFrameDumper.h — 解码结果的落盘与校验工具
 *
 * 用途：
 *   1. 把解码帧写成裸 YUV（紧凑排列），用于和参考解码器做逐字节比对；
 *   2. 把单帧写成 PPM，用于肉眼确认解码正确（"跑通了并且画面是对的"）；
 *   3. 计算帧校验和，作为回归测试的稳定基线。
 *
 * 注意：PPM 里的 YUV→RGB 是纯 CPU 的演示实现，只为验证正确性。
 * 正式版的颜色转换会走 GPU（OpenGL ES fragment shader），
 * 见 docs/ARCHITECTURE.md §6.2。
 */

#ifndef LDV_FRAME_DUMPER_H
#define LDV_FRAME_DUMPER_H

#include <stdio.h>
#include <stdint.h>

#include "LDVTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 按紧凑排列写出所有平面（不含 stride padding）。
 * 逐行 fwrite，所以 stride 不紧凑也不会写错。
 * 返回 LDV_OK 或负错误码。
 */
int LDVFrameDumperWriteYUV(FILE *fp, const LDVFrameView *view);

/*
 * 写出 P6 PPM（8bit RGB）。
 * 仅支持 I400 / I420 / I422 / I444；10bit 输入会先降到 8bit。
 * 色度上采样用最近邻（演示够用，正式版走 GPU 的双线性）。
 */
int LDVFrameDumperWritePPM(FILE *fp, const LDVFrameView *view);

/*
 * 计算 Y 平面（按 stride 逐行）的 FNV-1a 32 位校验和。
 * 与分辨率、色深、矩阵系数无关，只反映像素内容，适合做回归基线。
 */
uint32_t LDVFrameChecksum(const LDVFrameView *view);

/* 把 layout / matrix / range 转成可读字符串 */
const char *LDVFrameLayoutName(LDVPixelLayout layout);
const char *LDVFrameMatrixName(LDVMatrixCoefficients matrix);

#ifdef __cplusplus
}
#endif

#endif /* LDV_FRAME_DUMPER_H */
