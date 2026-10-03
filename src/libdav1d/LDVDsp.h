/*
 * LDVDsp.h — 运行时 DSP 实现选择（汇编 / 纯 C）
 *
 * 这是什么
 * --------
 * dav1d 的每个 DSP 函数都有两份实现：一份 NEON 汇编、一份可移植 C。
 * 平时由 dav1d 在 dav1d_open 里按 CPU 特性自动挑（有 NEON 就用汇编）。
 * 本模块把"挑哪一份"变成**可以由上层显式指定**，用于：
 *   - 对比两种实现的性能与行为（压测 app 的两个按钮）
 *   - 汇编路径出问题时一键退回纯 C 定位
 *
 * 三种目标架构下的可用性
 * ----------------------
 *   armv7 / armv7s   asm32 + C   在编进 asm32 的切片上运行时可切
 *   arm64            C           Xcode 6 下 asm64 未启用（旧工具链会卡死）
 *   i386 / x86_64    C           模拟器一行 ARM 汇编都不编
 *
 * ★ "两者都编进去"是靠 config.h 里按 __aarch64__ 条件化的 HAVE_ASM 实现的，
 *   所以一个 config.h 同时服务全部切片。
 *
 * 切换机制
 * --------
 * dav1d 内部有：
 *     unsigned dav1d_cpu_flags;         // 由 dav1d_init_cpu() 按 CPU 填
 *     unsigned dav1d_cpu_flags_mask;    // 默认 ~0
 *     dav1d_get_cpu_flags() = dav1d_cpu_flags & dav1d_cpu_flags_mask
 * 所以 `dav1d_set_cpu_flags_mask(0)` 就能让 dav1d 认为"这台机器没有
 * 任何 SIMD"，从而走 C 实现。
 *
 * ★ 但 DSP 表是在 dav1d_open 里一次性绑定的，所以**切换必须重建
 *   Dav1dContext**。本模块只负责设置掩码，重建由 LDVDecodeSession
 *   的 SetDspMode 负责（顺带清空解码缓存）。
 *
 * ★ 另一个前提：config.h 里 TRIM_DSP_FUNCTIONS 必须是 0。
 *   为 1 时 dav1d 会把编译期已知的 CPU 特性 OR 回 flags
 *   （src/cpu.h:127），掩码就压不住了 —— 切到纯 C 会**静默失效**。
 *   生成器会校验这一点（scripts/vendor-dav1d.py 的 _verify_pbxproj）。
 */

#ifndef LDV_DSP_H
#define LDV_DSP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LDVDspMode {
    LDV_DSP_MODE_C   = 0,   /* 纯 C 可移植实现 */
    LDV_DSP_MODE_ASM = 1    /* 汇编实现（取决于本架构启用的 asm32/asm64） */
} LDVDspMode;

/* ------------------------------------------------------------------------ */
/* 能力查询（编译期决定，运行时恒定）                                        */
/* ------------------------------------------------------------------------ */

/*
 * 当前架构是否编进了汇编实现。
 *   armv7 / armv7s → 编入 asm32 时为 1，否则 0
 *   arm64          → 编入 asm64 时为 1，否则 0
 *   i386 / x86_64  → 0
 */
int LDVDspAsmAvailable(void);

/*
 * 汇编实现的短名：按切片返回 "asm32" 或 "asm64"。
 * 没有汇编时返回 NULL —— 调用方据此把按钮置灰并说明原因。
 */
const char *LDVDspAsmName(void);

/* 模式名，用于 UI 显示："C" / 当前架构的汇编短名 */
const char *LDVDspModeName(LDVDspMode mode);

/* ------------------------------------------------------------------------ */
/* 模式选择                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 设置全局偏好。**下一次 LDVCoreDecoderCreate 生效** ——
 * 已经在跑的解码器不受影响（要生效请用
 * LDVDecodeSessionSetDspMode，它会重建解码器）。
 *
 * 请求 LDV_DSP_MODE_ASM 但当前架构没有汇编时，静默降级为 C。
 */
void LDVDspSetPreferredMode(LDVDspMode mode);

/* 当前偏好（已按可用性夹紧：没汇编时恒为 C） */
LDVDspMode LDVDspPreferredMode(void);

/*
 * 把偏好写进 dav1d 的 CPU 标志掩码。
 * LDVCoreDecoderCreate 内部会调，**不要在别处调** ——
 * 这是进程级全局状态，乱调会让已经建好的解码器语义不一致。
 */
void LDVDspApply(void);

/* 最近一次 Apply 之后实际生效的模式（供 UI 显示） */
LDVDspMode LDVDspActiveMode(void);

#ifdef __cplusplus
}
#endif

#endif /* LDV_DSP_H */
