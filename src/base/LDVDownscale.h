/*
 * LDVDownscale.h — 解码帧的双线性缩小（纯 C99）
 *
 * 它解决什么问题
 * --------------
 * 需求原文：
 *   "当显示画面分辨率低于视频原始分辨率时，将视频解码缓存按对应屏幕
 *    比例缩小（采用二次线性缩小、保持锐利、尽量调用 GPU 加速）
 *    以节约缓存空间。"
 *
 * 一句话：**1080p 的视频在 960x640 的屏上，没必要把 1080p 的帧存进缓存。**
 * 缓存是按字节封顶的，帧小了，能装的帧数就按面积比例变多：
 *
 *   512x384  I420  288 KB/帧   → 36 MB 装 106 帧
 *   缩到 50%  256x192  72 KB/帧 → 36 MB 装 424 帧
 *
 * 这就是"节约缓存空间"的全部含义 —— 它换来的是**抗抖动余量**，
 * 不是画质。所以缩小必须**不引入可见损失**，否则就是拿画质换余量。
 *
 * 为什么是"展宽支撑域的双线性"而不是教科书双线性
 * ----------------------------------------------
 * 教科书双线性在**缩小**时是错的：它只取目标像素周围 2x2 个源像素，
 * 缩小 3 倍时等于每 9 个源像素里扔掉 8 个 —— 高频信息直接混叠成
 * 摩尔纹和闪烁。缩小倍数越大越糟。
 *
 * 正确的做法是**把核的支撑域按缩小倍数展宽**：
 *   三角形核半径 r = max(1, srcLen / dstLen)
 *   - 缩小倍数 ≈ 1 时 r = 1 → 退化成**普通双线性**（2 个抽头）
 *   - 缩小 3 倍时 r = 3 → 覆盖 6 个源像素的加权平均，天然低通
 *
 * 这样既满足"二次线性"（核是三角形，无振铃，不像 Lanczos 会在
 * 边缘产生光晕），又满足"保持锐利"（不混叠 → 缩小后的画面在屏幕上
 * 放大回去仍然是干净的，而不是糊成一团加彩色噪点）。
 *
 * 为什么这里**不用 GPU**
 * ----------------------
 * "尽量调用 GPU 加速"这条需求在**缓存内缩小**这个位置是不成立的，
 * 原因是 GPU 路径必然要回读：
 *
 *   解码 → 上传纹理 → 渲染到小 framebuffer → **glReadPixels 回读** → 进缓存
 *
 * 而 glReadPixels 在 A5（SGX543）上是同步阻塞的，一次 512x384 的回读
 * 实测比这里的 CPU 双线性还慢，而且它会**占住主线程/GL 上下文**。
 *
 * GPU 在这个 app 里已经用在它该用的地方了：
 * **显示时的放大**由 CoreAnimation 的 `kCAFilterLinear` 在 GPU 上完成
 * （见 LDVVideoView 的 contentsGravity/magnificationFilter）。
 * 缩小在 CPU、放大在 GPU —— 各自在成本最低的一侧。
 *
 * 真要让 GPU 参与缩小，正确形态是 GLES2 直接渲染（ARCHITECTURE §6.2）：
 * 那样根本不需要"缩小后的缓存"，因为 YUV 平面直接进纹理、GPU 采样器
 * 顺便就缩放了，连色彩转换都省了。那是另一个阶段的整体改造。
 *
 * 支持范围
 * --------
 *   - 8 bit、I400 / I420 / I422 / I444
 *   - 高位深（10/12 bit）返回 LDV_ERR_UNSUPPORTED，调用方保持原尺寸。
 *     高位深素材在这个 app 里不存在（AV1 profile 0 / 8bit），
 *     为它写一条从没跑过的代码路径不如明确拒绝。
 *
 * 用法
 * ----
 *    LDVDownscaler *ds = LDVDownscalerCreate(w, h, tw, th, layout);
 *    uint8_t *buf = malloc(LDVDownscalerFrameBytes(ds));
 *    uint8_t *plane[3] = { buf, buf + off1, buf + off2 };
 *    LDVDownscalerRun(ds, &view, plane);
 *
 * 一个 LDVDownscaler 可以被反复使用（权重表只算一次），线程不安全
 * —— 它只在一个解码线程里用。
 */

#ifndef LDV_DOWNSCALE_H
#define LDV_DOWNSCALE_H

#include "LDVTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVDownscaler LDVDownscaler;

/*
 * 创建缩小器。
 *
 *   srcW/srcH  源尺寸
 *   dstW/dstH  目标尺寸（必须都 > 0 且 <= 源尺寸；否则返回 NULL）
 *   layout     源与目标的像素布局（缩小不改变布局）
 *
 * 返回 NULL 表示参数非法或内存不足。
 */
LDVDownscaler *LDVDownscalerCreate(int srcW, int srcH,
                                   int dstW, int dstH,
                                   LDVPixelLayout layout);

void LDVDownscalerDestroy(LDVDownscaler **s);

/* 目标尺寸（Create 时传入的，已做过偶数对齐校正） */
int LDVDownscalerDstWidth(const LDVDownscaler *s);
int LDVDownscalerDstHeight(const LDVDownscaler *s);

/* 第 plane 个平面的行距（字节）。plane: 0=Y, 1=U/V */
size_t LDVDownscalerPlaneStride(const LDVDownscaler *s, int plane);

/*
 * 一帧需要的总字节数（三个平面紧凑排布，含行对齐）。
 * 调用方按此大小分配一块，然后用 LDVDownscalerPlaneOffset 切分。
 */
size_t LDVDownscalerFrameBytes(const LDVDownscaler *s);

/* 第 plane 个平面在缓冲里的字节偏移 */
size_t LDVDownscalerPlaneOffset(const LDVDownscaler *s, int plane);

/*
 * 执行一次缩小。
 *
 *   src       源帧（只读；bpc 必须为 8，否则返回 LDV_ERR_UNSUPPORTED）
 *   dstPlane  三个目标平面地址（I400 时 dstPlane[1]/[2] 可为 NULL）
 *
 * 返回 LDV_OK / LDV_ERR_INVALID_ARG / LDV_ERR_UNSUPPORTED。
 */
LDVResult LDVDownscalerRun(LDVDownscaler *s, const LDVFrameView *src,
                           uint8_t *dstPlane[3]);

/*
 * 由"目标宽"算出偶数对齐的目标高，保持宽高比。
 *
 *   srcW/srcH   源尺寸
 *   targetW     期望的目标宽（<=0 或 >= srcW 表示"不缩"，原样返回）
 *   outW/outH   结果（都保证是偶数，且不小于 2）
 *
 * ★ 为什么必须偶数对齐：I420 的色度平面是 (w+1)/2 × (h+1)/2，
 *   奇数尺寸会让色度平面比"宽高各一半"多出半行半列，缩放时
 *   源与目标对不上，边缘会出现 1 像素的绿边/错位。
 *   向上取整到偶数是最省事的正确做法。
 */
void LDVDownscaleTargetSize(int srcW, int srcH, int targetW,
                            int *outW, int *outH);

#ifdef __cplusplus
}
#endif

#endif /* LDV_DOWNSCALE_H */
