/*
 * LDVColorConvert.h — YUV → BGRA 定点软转换（纯 C99）
 *
 * 为什么单独一个文件
 * ------------------
 * 1) **要单独开 -O2。**
 *    这是逐像素的热循环：856x480 = 410,880 个像素，每个像素 3 次取样、
 *    几次定点乘加、4 字节写出。Debug 配置整包 -O0 时，每像素都是真实的
 *    函数调用，实测 20–40 ms/帧；而 60 fps 的预算是 16.7 ms。
 *    主线程一旦跟不上就会滚雪球（落后 → 追帧 → 更慢 → 主线程被占满，
 *    连按钮点击都收不到）。所以本文件由工程单独指定 -O2，
 *    见 scripts/gen-app-project.py 的 APP_FILE_FLAGS。
 *
 * 2) **它是纯计算，没有调试价值。**
 *    正确性由定点系数表保证，不靠单步调试；把它从视图代码里分出来，
 *    LDVVideoView 就只剩下"缓冲管理 + 提交图层"。
 *
 * 3) 纯 C99、不依赖 Objective-C，可以在任何宿主上单独跑单元测试
 *    （本机没有 C 编译器，但结构上留好了这个口子）。
 *
 * 为什么是定点而不是浮点
 * ----------------------
 * 定点（16.16）在 A4/A5 这类老 ARM 上比浮点快得多，而且结果可复现 ——
 * 浮点在不同优化级别/不同 FMA 使用下可能给出不同的舍入，画面会有
 * 极难查的"偶尔偏 1"。定点系数都是预先四舍五入到 16.16 的常量，
 * 结果与优化级别无关。
 *
 * 注意：这不是 GPU 方案。ARCHITECTURE §6.2 里的 GPU 路径
 * （GLES2 + 3 张亮度纹理 + 片元着色器）才是最终形态；
 * 本文件是当前阶段的正确实现，也是 GPU 不可用时的回退路径。
 */

#ifndef LDV_COLOR_CONVERT_H
#define LDV_COLOR_CONVERT_H

#include "LDVTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把一帧转换成 BGRA8888（内存序 B, G, R, A）。
 *
 *   view        帧的只读视图（plane[] / stride[] / width / height / bpc /
 *               layout / colorRange / matrix）
 *   dst         目标缓冲，必须至少 dstStride * view->height 字节
 *   dstStride   目标行距（字节）。传 0 表示取 width * 4（紧凑排列）
 *
 * 支持 I400 / I420 / I422 / I444，8/10/12 bit（高位深取高字节）。
 * 色域按 view->colorRange 选 video/full range，矩阵按 view->matrix 选
 * BT.601 / BT.709（UNKNOWN 时按分辨率兜底）。
 *
 * 线程安全：只读 view，只写 dst，无全局状态。
 */
void LDVConvertYUVToBGRA(const LDVFrameView *view,
                         uint8_t *dst, size_t dstStride);

/*
 * 选中的矩阵系数名，仅用于诊断显示（"601" / "709"）。
 * 传 NULL 时按分辨率兜底。
 */
const char *LDVColorConvertMatrixName(const LDVFrameView *view);

#ifdef __cplusplus
}
#endif

#endif /* LDV_COLOR_CONVERT_H */
