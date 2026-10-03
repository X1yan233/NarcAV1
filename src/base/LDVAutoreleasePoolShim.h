/*
 * LDVAutoreleasePoolShim.h — iOS 4 的自动释放池兼容层
 *
 * 背景：ARC 下不能用 NSAutoreleasePool，而 @autoreleasepool 会被编译成
 * objc_autoreleasePoolPush/Pop，这两个符号在 iOS 5 之前的运行时里不可靠。
 * 所以按部署目标切换实现：
 *   部署目标 < iOS 5.0 → 走 NSAutoreleasePool（实现文件必须 -fno-objc-arc）
 *   其它情况          → 走 objc_autoreleasePoolPush/Pop
 *
 * 用法（必须成对，中间不能 return / goto）：
 *     void *pool = LDVPoolPush();
 *     ... 可能产生 autorelease 对象的代码 ...
 *     LDVPoolPop(pool);
 */

#ifndef LDV_AUTORELEASE_POOL_SHIM_H
#define LDV_AUTORELEASE_POOL_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

void *LDVPoolPush(void);
void  LDVPoolPop(void *pool);

#ifdef __cplusplus
}
#endif

#endif /* LDV_AUTORELEASE_POOL_SHIM_H */
