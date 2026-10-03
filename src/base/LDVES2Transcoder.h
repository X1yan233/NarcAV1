/*
 * LDVES2Transcoder.h — armv7 专用的 ES2 异步缩小转换器（v8.20）
 *
 * 背景与职责
 * ----------
 * 解码分辨率必须等于播放器显示尺寸，否则缓存字节、上传带宽、显示开销
 * 全都在处理看不见的多余像素（用户实测：4S 720p30 解出 1280x720，窗口
 * 内容才 ~568x320，白白多解 5 倍像素）。但"每帧 CPU 缩小"在解码线程内
 * 与 dav1d 串行，A5 这类设备承担不起（4S 480p60 实测 34→15fps）。
 *
 * 用户定案的方案：**缩小放在"解码完成之后"异步进行，用 GPU 的 ES2 转换**。
 * 解码线程只管解原生尺寸，帧经 LDVDecodeSession 的 sink 分流交到本类的
 * 有界队列；专用线程用独立 EAGLContext（offscreen FBO，无 drawable）做
 *   YUV 三平面上传 → 着色器线性缩小（一次 pass，YUV 打包进 RGBA）
 *   → glReadPixels 回读 → 解包成目标尺寸 I420 → 推进显示缓存。
 *
 * 效果：
 *   - 解码线程零缩放开销（队列满丢最旧，绝不阻塞解码）
 *   - 缓存里是窗口尺寸的小帧（720p→568x320 每帧 ~1.4MB→~0.26MB，
 *     同预算缓存深度翻 5 倍），上传/显示也只剩小帧
 *   - 缓存管理（字节预算、宽 >1.5x 新目标释放）照旧在缓存层生效
 *
 * 失败兜底（不黑屏原则）
 * ---------------------
 * GL 上下文建不起来 / 源平面超过 GL_MAX_TEXTURE_SIZE（SGX535 上限
 * 1024，720p 的 stride 1280 装不下）/ FBO 不完整 → 内部转入直通：
 * 后续帧不再进 GL，按原尺寸复制直推显示缓存（= v8.19f 的"原生解码"
 * 行为，只是没有 CPU 缩放）。播放永远继续。
 *
 * 线程模型
 * --------
 *   解码线程：ingestFrameView:（复制进队列，锁内，非阻塞）
 *   转换线程（本类自建 pthread）：等队列 → GL 转换 → 推缓存
 *   主线程：setOutputWidth:height: 只改标记，FBO 重建在转换线程做
 *   stop：任意线程、幂等；返回后转换线程已结束、GL 资源已释放
 *
 * 与显示端（LDVVideoView 的 ES2 上下文）完全独立：无 sharegroup、
 * 无共享资源，产物是标准 I420 CPU 帧，显示端零改动。
 */

#import <Foundation/Foundation.h>
#import "LDVTypes.h"
#import "LDVFrameCache.h"

#ifdef __OBJC__

@interface LDVES2Transcoder : NSObject

/*
 * cache：显示缓存（转换产物推进去）。
 * 不持有它 —— 调用方必须保证缓存的生命周期覆盖本对象：
 * 先 [transcoder stop]（停掉转换线程）再销毁缓存。
 */
- (id)initWithCache:(LDVFrameCache *)cache;

/*
 * 设定缩小目标尺寸（0,0 = 直通：帧原样进缓存）。
 * 任意线程、可随时调用；实际的 FBO 重建发生在转换线程。
 * 尺寸会做偶数对齐（I420 色度对位要求）。
 */
- (void)setOutputWidth:(int)w height:(int)h;

/*
 * 解码线程调用：把一帧（借用解码器内存，返回后作废）复制进有界队列。
 * 队列满（4 帧）丢最旧 —— 绝不阻塞解码线程。
 * 高位深 / 非 I420 帧不进 GL，按原尺寸直推缓存。
 */
- (void)ingestFrameView:(const LDVFrameView *)view;

/*
 * 丢弃队列里所有还没转换的帧。
 * 用于"清空缓存重新开始"的场景（Restart / DSP 切换）：这些路径会把
 * 显示缓存清空重解，队列里残留的旧内容帧必须一并丢掉，否则它们会被
 * 转换线程推进新缓存，画面短暂错序。
 */
- (void)clearQueuedFrames;

/* 停止并回收（幂等）。返回后转换线程已结束、GL 资源已释放。 */
- (void)stop;

@end

#endif /* __OBJC__ */
