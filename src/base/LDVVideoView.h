//
//  LDVVideoView.h
//  av1decoder — libdav1d 压测 app
//
//  中间那个 16:9 播放视图。
//
//  渲染路径（v8.14 起：GLES2 直通优先，CPU 转换降为回退）：
//
//  主路径（GPU 卸载，8bit I400/I420/I422/I444）：
//      planar YUV 原样上传三张 GL_LUMINANCE 纹理（纹理宽 = stride，
//      零 CPU 拷贝）→ 片元着色器做 YUV→RGB（BT.601/709 × video/full
//      range，系数与 LDVColorConvert.c 逐值对齐）→ letterbox quad
//      → presentRenderbuffer。CPU 不再碰任何一个像素。
//
//  回退路径（bpc > 8、尺寸超 GL 上限、GL 初始化/运行失败）：
//      I420/I422/I444 → BGRA（LDVColorConvert.c 的定点软转换，-O3）
//      → CGImage（数据源直接指向 3 块轮转缓冲，不拷贝像素）→ layer.contents
//      （3 块轮转的原因：CA 在渲染线程异步读缓冲，单块会撕裂，详见 .m）
//
//  为什么必须上 GPU（iPhone 5 / A6 实测）：
//      压测（纯解码）98-125 fps，接上旧渲染路径后 47-55 fps ——
//      CPU 逐像素转换 + RGBA 纹理上传吃掉大半个核，GPU 全程空闲。
//      GL 路径把逐像素工作全部交给 SGX，上传带宽从 4WH 降到 1.5WH。
//

#import <UIKit/UIKit.h>

#include "LDVTypes.h"

@interface LDVVideoView : UIView

/*
 * 显示一帧。8bit 帧走 GLES2 直通；高位深/超限帧自动走 CPU 回退
 * （逐帧判断，不禁用 GL）。view 里的像素必须在本方法返回前保持有效
 * —— GL 路径上传是同步语义，CPU 路径会同步转换，返回后即可复用。
 * 传 NULL 表示清空画面（黑屏）。
 */
- (void)presentFrame:(const LDVFrameView *)view;

/* 当前显示帧的像素尺寸（0 表示还没显示过）。 */
- (CGSize)currentVideoSize;

/* 累计显示了多少帧（用于观察显示端是否跟上）。 */
- (int64_t)presentedFrameCount;

/*
 * 最近一次"呈现一帧"的耗时（毫秒）。
 *   GL 模式 = 纹理上传 + present；
 *   CPU 模式 = 逐像素转换。
 * 60 fps 的预算是 16.7 ms，读数超了说明渲染端成了瓶颈（会滚雪球：
 * 落后 → 追帧 → 更慢 → 主线程被占满，连按钮点击都收不到）。
 */
- (double)lastConvertMilliseconds;

/*
 * 当前渲染后端名："Metal"（arm64+iOS8+）/ "GLES3"（arm64，iOS7+，
 * SGX543 等 ES3 不支持的 GPU 自动回落）/ "GLES2"（armv7 全系）/ "CPU"。
 * 状态栏显示它，是为了让各后端的表现一眼可辨 —— 量级差只看毫秒数
 * 猜不出瓶颈在哪条路径上。
 */
- (const char *)renderBackendName;

@end
