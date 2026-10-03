/*
 * LDVAutoreleasePoolShim.m — 自动释放池兼容层实现
 *
 * 本文件必须以 -fno-objc-arc 编译（见 Config/libdav1d.xcconfig 里的
 * OTHER_CFLAGS[file=...] 设置）。原因：iOS 4 路径要用 NSAutoreleasePool，
 * 而 ARC 下该类型被禁用。
 */

#import "LDVAutoreleasePoolShim.h"
#import <Foundation/Foundation.h>

/*
 * 只有"部署目标低于 iOS 5.0"时才走旧运行时路径。
 * 用 __IPHONE_OS_VERSION_MIN_REQUIRED 而不是运行时判断，是因为
 * 这决定了必须链接哪个符号——必须在编译期定下来。
 */
#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && (__IPHONE_OS_VERSION_MIN_REQUIRED < 50000)

void *LDVPoolPush(void) {
    /* 非 ARC 文件，可以直接用 NSAutoreleasePool */
    return (void *)[[NSAutoreleasePool alloc] init];
}

void LDVPoolPop(void *pool) {
    if (pool) [(id)pool release];
}

#else

/*
 * iOS 5+ / macOS：直接用运行时函数，语义与 @autoreleasepool 完全一致。
 * 这两个符号在 <objc/runtime.h> 里声明；这里显式 extern 以避免
 * 头文件包含顺序带来的差异。
 */
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *context);

void *LDVPoolPush(void) {
    return objc_autoreleasePoolPush();
}

void LDVPoolPop(void *pool) {
    if (pool) objc_autoreleasePoolPop(pool);
}

#endif
