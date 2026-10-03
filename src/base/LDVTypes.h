/*
 * LDVTypes.h — libdav1d 公共基础类型
 *
 * 纯 C99，可被 Objective-C 与 C 编译单元共同包含。
 * 不依赖任何系统框架，不依赖 dav1d 头文件（枚举数值与 dav1d 保持一致）。
 */

#ifndef LDV_TYPES_H
#define LDV_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 统一返回值约定：0 为成功，负数为错误。
 * LDV_ERR_NEED_INPUT 不是错误，是"状态未就绪"——
 * 喂数据时表示需要先取帧，取帧时表示需要更多输入。
 */
typedef enum LDVResult {
    LDV_OK              =  0,
    LDV_ERR_NEED_INPUT  = -1,
    LDV_ERR_EOF         = -2,
    LDV_ERR_NOMEM       = -3,
    LDV_ERR_INVALID_ARG = -4,
    LDV_ERR_BITSTREAM   = -5,
    LDV_ERR_UNSUPPORTED = -6
} LDVResult;

/*
 * 像素布局。数值必须与 dav1d 的 enum Dav1dPixelLayout 一致
 * （dav1d-master/include/dav1d/headers.h:108-113）。
 * 在 LDVCoreDecoder.c 里有编译期断言校验。
 */
typedef enum LDVPixelLayout {
    LDV_PIXEL_LAYOUT_I400 = 0,  /* 单色 */
    LDV_PIXEL_LAYOUT_I420 = 1,  /* 4:2:0 平面 */
    LDV_PIXEL_LAYOUT_I422 = 2,  /* 4:2:2 平面 */
    LDV_PIXEL_LAYOUT_I444 = 3   /* 4:4:4 平面 */
} LDVPixelLayout;

/*
 * 矩阵系数（用于 YUV→RGB）。名称与数值严格对应 dav1d 的
 * enum Dav1dMatrixCoefficients（dav1d-master/include/dav1d/headers.h:159-175）。
 * 注意 4 是 FCC，不是 BT.470M；6 是 BT.601（即 SMPTE 170M）。
 * 在 LDVCoreDecoder.c 里有编译期断言校验这些数值。
 */
typedef enum LDVMatrixCoefficients {
    LDV_MATRIX_IDENTITY   = 0,
    LDV_MATRIX_BT709      = 1,
    LDV_MATRIX_UNKNOWN    = 2,
    LDV_MATRIX_FCC        = 4,
    LDV_MATRIX_BT470BG    = 5,
    LDV_MATRIX_BT601      = 6,   /* == SMPTE 170M */
    LDV_MATRIX_SMPTE240   = 7,
    LDV_MATRIX_YCGCO      = 8,
    LDV_MATRIX_BT2020_NCL = 9,
    LDV_MATRIX_BT2020_CL  = 10,
    LDV_MATRIX_ICTCP      = 14
} LDVMatrixCoefficients;

/*
 * 一帧解码结果的只读视图。
 *
 * 生命周期：由 LDVCoreDecoder 拥有。取到视图后必须调用
 * LDVCoreDecoderReleaseFrame() 才能继续拉取下一帧；释放之后本结构内
 * 的所有指针立即失效。这是刻意设计的"单槽借用"语义——它把
 * "dav1d_picture_unref 一条路径都不能漏"这条硬性约定变成类型系统的约束：
 * 不释放就取不到下一帧，漏释放会立刻表现为取帧失败，而不是内存泄漏。
 *
 * 注意 plane[] 是按 stride 排列的，stride 带有 dav1d 的对齐 padding
 * （aligned_w 向上取整到 128，必要时再 +64），
 * 所以逐行访问时**必须**用 stride[]，不能假设 stride == width。
 */
typedef struct LDVFrameView {
    const uint8_t *plane[3];    /* Y / U / V；I400 时 plane[1] 与 plane[2] 为 NULL */
    ptrdiff_t      stride[2];   /* [0] 用于 plane[0]；[1] 同时用于 plane[1] 与 plane[2] */
    int            width;       /* 可见宽度（像素），不含 padding */
    int            height;      /* 可见高度（像素） */
    int            bpc;         /* 8 / 10 / 12 */
    LDVPixelLayout layout;
    int            colorRange;  /* 0 = MPEG（video range），1 = JPEG（full range） */
    LDVMatrixCoefficients matrix;
    /*
     * 流是否带胶片颗粒合成（dav1d frame_hdr->film_grain.present）。
     * 纯诊断用：grain 合成走消费路径逐帧串行做，不参与解码线程的
     * 帧级并行 —— 是高分辨率素材在 A6 上吞吐受限的头号嫌疑（v8.15）。
     */
    int            filmGrain;
    int64_t        timestamp;   /* 容器时间戳；INT64_MIN 表示未知 */
    int            index;       /* 解码输出序号，从 0 开始 */
} LDVFrameView;

#ifdef __cplusplus
}
#endif

#endif /* LDV_TYPES_H */
