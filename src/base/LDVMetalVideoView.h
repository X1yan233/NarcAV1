//
//  LDVMetalVideoView.h
//  av1decoder — libdav1d 压测 app
//
//  Metal 渲染画布（第 7 代架构，arm64 专用，v8.16）。
//
//  ★ 整个类只在 __arm64__ 下有实体：armv7/i386 编译单元为空，
//    链接器不会引用任何 Metal 符号。framework 用 -weak_framework Metal
//    弱链接（gen-app-project 的 arm64 OTHER_LDFLAGS），iOS 7 的 A7 设备
//    上由 [LDVMetalCanvas isSupported] 运行时门控，永远走 GLES3 路径。
//
//  引擎选择（LDVVideoView init 里定，一次）：
//      arm64 + iOS 8+ + 默认 device 可用 → Metal（本类）
//      arm64（iOS 7+）                   → GLES3 context（LDVGLCanvas）
//      armv7 全系                        → GLES2 context（LDVGLCanvas）
//
//  本阶段的 Metal 管线与 GLES2 直通**功能等价**（YUV 三纹理 →
//  YUV→RGB 着色器 → letterbox quad → present），额外拿到的是：
//    - 平面数据进 MTLBuffer（Shared + WriteCombined），纹理从 buffer
//      按偏移创建 —— bytesPerRow 直接吃 dav1d stride，三套轮转；
//    - 后续 Phase 的 compute 后处理（CDEF/LR/grain GPU 卸载）都在
//      这条管线上加，不用再动渲染路径。
//  grain 合成的 GPU 卸载、CDEF/LR 近似是 Phase 2，本文件不实现。
//

#import <UIKit/UIKit.h>

#if defined(__arm64__) || defined(__aarch64__)

#include "LDVTypes.h"

@interface LDVMetalCanvas : UIView

/*
 * 运行时门控（在实例化**之前**调用）：
 * iOS 8+ 且 Metal 默认 device 创建成功才返回 YES。
 * iOS 7 的 arm64 设备上 CAMetalLayer 类不存在，恒为 NO。
 */
+ (BOOL)isSupported;

/* 建 device/queue/pipeline/几何。失败返回 NO（调用方降级 GLES）。 */
- (BOOL)bootstrap;

/* 本帧能不能走 Metal（bpc/布局/尺寸检查，纯查询）。 */
- (BOOL)canRenderFrame:(const LDVFrameView *)view;

/* 渲染一帧。NO 且 metalBroken=YES → 调用方永久降级 GLES。 */
- (BOOL)drawFrame:(const LDVFrameView *)view;

/* 清成黑屏。 */
- (void)clearToBlack;

/* 真失败标志（device/pipeline/drawable 级不可恢复错误）。 */
- (BOOL)isBroken;

@property (nonatomic, readonly) double lastUploadMilliseconds;
@property (nonatomic, readonly) double lastDrawMilliseconds;

@end

#endif /* __arm64__ */
