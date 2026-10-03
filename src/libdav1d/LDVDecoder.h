/*
 * LDVDecoder.h — libdav1d 最小公开 API（Objective-C / ARC）
 *
 * 这一层只做三件事：
 *   1. 把 C 层的 LDVResult 错误码翻译成 NSError；
 *   2. 管理 LDVCoreDecoder 的生命周期（ARC + dealloc 收口）；
 *   3. 按当前设备选择调参档。
 *
 * 它刻意不做的（属于后续阶段）：
 *   - 预缓冲 / 渐进式缓冲调度（LDVProgressiveBufferScheduler）
 *   - GPU 缩放与格式转换（LDVGLPipeline）
 *   - AAC 解码与音画同步（LDVAudioDecoder / LDVAudioClock）
 *   - 输出编码（LDVVideoEncoder）
 *
 * iOS 4.0 兼容性注意：
 *   - 不使用 __weak（改用 __unsafe_unretained，本文件暂无 delegate 所以不涉及）
 *   - 不使用 @autoreleasepool（走 LDVPoolPush/Pop）
 *   - 不使用 NSArray/NSDictionary 下标语法（那需要 iOS 6+）
 *   - 不使用 NSURLSession / NSJSONSerialization 等 iOS 5+ API
 */

#import <Foundation/Foundation.h>

#include "LDVTypes.h"
#include "LDVPlatformTuning.h"

/* 错误域与错误码（错误码数值与 LDVResult 的负值对应） */
extern NSString *const LDVErrorDomain;

typedef enum LDVErrorCode {
    LDVErrorCodeNeedInput  = 1,
    LDVErrorCodeEOF        = 2,
    LDVErrorCodeOutOfMemory = 3,
    LDVErrorCodeInvalidArgument = 4,
    LDVErrorCodeBitstream  = 5,
    LDVErrorCodeUnsupported = 6,
    LDVErrorCodeNotOpen    = 7,
    LDVErrorCodeOpenFailed = 8
} LDVErrorCode;

@interface LDVDecoder : NSObject

/* 按当前设备自动选择调参档（A4 / 单核 / 多核 / 宿主）。 */
+ (instancetype)decoderForCurrentDevice;

/* 用指定调参档创建。 */
- (instancetype)initWithTuning:(LDVTuningProfile)tuning;

/* 打开解码器。失败时 *error 为具体原因。 */
- (BOOL)openWithError:(NSError **)error;

/* 关闭解码器并释放全部资源。允许重复调用；dealloc 里会自动调用。 */
- (void)close;

/*
 * 探测序列头（类方法，不需要实例）。
 * 用于在打开解码器之前决定起播档位、判断能否走 SVC 选层。
 */
+ (LDVResult)probeSequenceHeader:(const void *)bytes
                          length:(NSUInteger)length
                        outWidth:(int *)outWidth
                       outHeight:(int *)outHeight
                           outBpc:(int *)outBpc
                    outColorRange:(int *)outColorRange
            outOperatingPointCount:(int *)outOperatingPointCount;

/*
 * 送入一个 AV1 数据包（一个或多个 OBU）。
 * 内部会复制数据，返回后调用方可以立即复用缓冲区。
 *
 * 返回 LDV_ERR_NEED_INPUT 时表示解码器内部还有数据没消化，
 * 应当先调用 pullFrame:error: 取帧，然后再送。
 */
- (LDVResult)feedPacket:(const void *)bytes
                 length:(NSUInteger)length
              timestamp:(int64_t)timestamp
                  error:(NSError **)error;

/*
 * 拉取一帧。
 *   LDV_OK             成功，*outView 已填充
 *   LDV_ERR_NEED_INPUT 暂无可输出帧（不是错误，*error 为 nil）
 *   其它               失败，*error 有值
 *
 * 取到帧后必须调用 releaseFrame: 才能再次拉取。
 * 喂完全部数据后，反复调用本方法直到返回 LDV_ERR_NEED_INPUT，
 * 即可取出所有内部延迟帧。
 */
- (LDVResult)pullFrame:(LDVFrameView *)outView error:(NSError **)error;

/* 释放当前借出的帧。幂等。 */
- (void)releaseFrame:(LDVFrameView *)view;

/* —— 只读状态 —— */
@property (nonatomic, readonly) LDVTuningProfile tuning;
@property (nonatomic, readonly, getter=isOpen) BOOL open;
@property (nonatomic, readonly) int64_t decodedFrameCount;
@property (nonatomic, readonly) int     decodeErrorCount;

/*
 * dav1d 实际采用的帧级并行深度（n_fc）。
 * A4 单核档上这个值应当是 1 —— 意味着 dav1d 完全没有创建线程。
 * 这是验证单核优化是否生效的最直接指标。
 */
@property (nonatomic, readonly) int effectiveFrameDelay;

@end
