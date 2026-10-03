/*
 * LDVDecoder.m — libdav1d 最小公开 API 实现（ARC）
 */

#import "LDVDecoder.h"
#import "LDVCoreDecoder.h"

NSString *const LDVErrorDomain = @"LDVErrorDomain";

/* 把 C 层错误码翻成 NSError。LDV_ERR_NEED_INPUT 不是错误，返回 nil。 */
static NSError *LDVErrorFromResult(LDVResult result, NSString *context) {
    LDVErrorCode code;
    NSString    *desc;

    if (result == LDV_OK || result == LDV_ERR_NEED_INPUT) return nil;

    switch (result) {
    case LDV_ERR_EOF:         code = LDVErrorCodeEOF;             desc = @"end of stream"; break;
    case LDV_ERR_NOMEM:       code = LDVErrorCodeOutOfMemory;     desc = @"out of memory"; break;
    case LDV_ERR_INVALID_ARG: code = LDVErrorCodeInvalidArgument; desc = @"invalid argument"; break;
    case LDV_ERR_BITSTREAM:   code = LDVErrorCodeBitstream;       desc = @"corrupt bitstream"; break;
    case LDV_ERR_UNSUPPORTED: code = LDVErrorCodeUnsupported;     desc = @"unsupported feature"; break;
    default:                  code = LDVErrorCodeBitstream;       desc = @"unknown error"; break;
    }

    /* iOS 4 兼容：不用 NSDictionary 下标语法 */
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    NSString *full = context ? [NSString stringWithFormat:@"%@: %@", context, desc] : desc;
    [info setObject:full forKey:NSLocalizedDescriptionKey];

    return [NSError errorWithDomain:LDVErrorDomain code:(NSInteger)code userInfo:info];
}

@implementation LDVDecoder {
    LDVCoreDecoder  *_core;
    LDVTuningProfile _tuning;
}

#pragma mark - 创建与销毁

+ (instancetype)decoderForCurrentDevice {
    return [[self alloc] initWithTuning:LDVTuningProfileForCurrentDevice()];
}

- (instancetype)initWithTuning:(LDVTuningProfile)tuning {
    self = [super init];
    if (self) {
        _tuning = tuning;
        _core   = NULL;
    }
    return self;
}

- (instancetype)init {
    return [self initWithTuning:LDVTuningProfileForCurrentDevice()];
}

- (void)dealloc {
    /* ARC 下禁止 [super dealloc]。close 负责把 _core 置 NULL。 */
    [self close];
}

#pragma mark - 生命周期

- (BOOL)openWithError:(NSError **)error {
    if (_core) return YES;

    _core = LDVCoreDecoderCreate(&_tuning);
    if (!_core) {
        if (error) {
            NSMutableDictionary *info = [NSMutableDictionary dictionary];
            [info setObject:@"dav1d_open failed (out of memory or invalid settings)"
                     forKey:NSLocalizedDescriptionKey];
            *error = [NSError errorWithDomain:LDVErrorDomain
                                         code:(NSInteger)LDVErrorCodeOpenFailed
                                     userInfo:info];
        }
        return NO;
    }

    /* 把 dav1d 实际采用的参数回读出来，供上层校验单核优化是否生效 */
    const LDVTuningProfile *actual = LDVCoreDecoderTuning(_core);
    if (actual) _tuning = *actual;

    return YES;
}

- (void)close {
    if (_core) {
        LDVCoreDecoderDestroy(&_core);   /* 内部会置 _core = NULL */
        _core = NULL;
    }
}

#pragma mark - 探测

+ (LDVResult)probeSequenceHeader:(const void *)bytes
                          length:(NSUInteger)length
                        outWidth:(int *)outWidth
                       outHeight:(int *)outHeight
                           outBpc:(int *)outBpc
                    outColorRange:(int *)outColorRange
            outOperatingPointCount:(int *)outOperatingPointCount
{
    if (!bytes || length == 0) return LDV_ERR_INVALID_ARG;
    return (LDVResult)LDVCoreProbeSequenceHeader((const uint8_t *)bytes,
                                                 (size_t)length,
                                                 outWidth, outHeight,
                                                 outBpc, NULL,
                                                 outColorRange,
                                                 outOperatingPointCount);
}

#pragma mark - 送数据 / 取帧

- (LDVResult)feedPacket:(const void *)bytes
                 length:(NSUInteger)length
              timestamp:(int64_t)timestamp
                  error:(NSError **)error
{
    if (!_core) {
        if (error) {
            NSMutableDictionary *info = [NSMutableDictionary dictionary];
            [info setObject:@"decoder is not open" forKey:NSLocalizedDescriptionKey];
            *error = [NSError errorWithDomain:LDVErrorDomain
                                         code:(NSInteger)LDVErrorCodeNotOpen
                                     userInfo:info];
        }
        return LDV_ERR_INVALID_ARG;
    }

    LDVResult r = (LDVResult)LDVCoreDecoderFeed(_core,
                                                (const uint8_t *)bytes,
                                                (size_t)length,
                                                timestamp);
    if (error) *error = LDVErrorFromResult(r, @"feedPacket");
    return r;
}

- (LDVResult)pullFrame:(LDVFrameView *)outView error:(NSError **)error {
    if (!_core) {
        if (error) {
            NSMutableDictionary *info = [NSMutableDictionary dictionary];
            [info setObject:@"decoder is not open" forKey:NSLocalizedDescriptionKey];
            *error = [NSError errorWithDomain:LDVErrorDomain
                                         code:(NSInteger)LDVErrorCodeNotOpen
                                     userInfo:info];
        }
        return LDV_ERR_INVALID_ARG;
    }

    LDVResult r = (LDVResult)LDVCoreDecoderPullFrame(_core, outView);
    if (error) *error = LDVErrorFromResult(r, @"pullFrame");
    return r;
}

- (void)releaseFrame:(LDVFrameView *)view {
    if (_core) LDVCoreDecoderReleaseFrame(_core, view);
}

#pragma mark - 状态

- (LDVTuningProfile)tuning { return _tuning; }
- (BOOL)isOpen             { return _core != NULL; }

- (int64_t)decodedFrameCount {
    return _core ? LDVCoreDecoderFrameCount(_core) : 0;
}

- (int)decodeErrorCount {
    return _core ? LDVCoreDecoderErrorCount(_core) : 0;
}

- (int)effectiveFrameDelay {
    return _core ? LDVCoreDecoderEffectiveFrameDelay(_core) : 0;
}

@end
