/*
 * LDVDsp.c — 运行时 DSP 实现选择
 *
 * 设计说明见 LDVDsp.h。这里只说明两个实现上的决定：
 *
 * 1) 不 include dav1d 的内部头（src/cpu.h）
 *    需要的只有一个符号 dav1d_set_cpu_flags_mask()。自己声明一下就够了，
 *    没必要为此把 config.h + src/arm/cpu.h + common/attributes.h 一整套
 *    内部头拉进本编译单元 —— 那会让本库与 dav1d 的内部结构耦合得更紧，
 *    而且 config.h 里的宏有和系统头打架的风险。
 *    代价是签名要手工跟住上游；我们是 vendored 构建（版本锁定），
 *    而且生成器有 API 一致性检查兜着。
 *
 * 2) 汇编可用性由 __aarch64__ 判断，与 config.h 的 HAVE_ASM 是同一个条件
 *    ★ 这两处必须一致，否则会出现"按钮能点但其实没汇编"这类静默错误。
 *      scripts/vendor-dav1d.py 的 _check_dsp_guard_consistency()
 *      会把两边的架构宏提取出来比对，不一致直接终止构建。
 */

#include "LDVDsp.h"

#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* 汇编可用性                                                                */
/* ------------------------------------------------------------------------ */

/*
 * ★ 这个条件必须与 Vendor/dav1d/config.h 里 HAVE_ASM 的 #if 条件
 *   完全一致（生成器会校验）。改这里就要同步改生成器里的 config.h 模板。
 */
/*
 * ★ 直接读 config.h 的 HAVE_ASM —— 那是「这次构建到底有没有编汇编」的
 *   **唯一权威**，由生成器按 DAV1D_ASM_DIRS 与目标架构共同决定。
 *
 *   刻意不自己判断 __aarch64__：生成器可以在 arm64 上把 HAVE_ASM 置 0
 *   （例如纯 C 构建），那时自己判断会得出相反的结论 ——
 *   表现是"汇编解码"按钮亮着可点，但按下去其实什么都没变。
 *
 *   config.h 在 Vendor/dav1d/ 下，已在本工程的 HEADER_SEARCH_PATHS 里。
 *   它只有 #define，且必须在系统头之前 include（避免宏与系统头冲突）。
 */
#include "config.h"

#if HAVE_ASM && ARCH_ARM
#  define LDV_DSP_ASM_COMPILED 1
#  define LDV_DSP_ASM_NAME     "asm32"
#elif HAVE_ASM && ARCH_AARCH64
#  define LDV_DSP_ASM_COMPILED 1
#  define LDV_DSP_ASM_NAME     "asm64"
#else
#  define LDV_DSP_ASM_COMPILED 0
#  define LDV_DSP_ASM_NAME     NULL
#endif

/* ------------------------------------------------------------------------ */
/* dav1d 的 CPU 标志掩码接口                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 声明见 Vendor/dav1d/src/cpu.h:37（DAV1D_API void dav1d_set_cpu_flags_mask(unsigned)）。
 *
 * 语义：dav1d_get_cpu_flags() 返回 dav1d_cpu_flags & dav1d_cpu_flags_mask。
 * 所以传 0 = 屏蔽全部 SIMD 特性 = 强制走 C 实现；
 * 传 ~0U = 恢复默认（有 NEON 就用汇编）。
 *
 * ★ 前提是 config.h 里 TRIM_DSP_FUNCTIONS = 0。为 1 时 dav1d 会把
 *   编译期已知的 CPU 特性 OR 回 flags，掩码压不住（详见 LDVDsp.h）。
 */
extern void dav1d_set_cpu_flags_mask(unsigned mask);

/* ------------------------------------------------------------------------ */
/* 状态                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * 全局偏好。默认 ASM —— 有汇编就用汇编，这是正常的性能取向；
 * 想要纯 C 得显式切。
 *
 * 线程：只有 app 主线程会改它（按钮回调），而 Apply 在读它。
 * 单字节枚举的读写是原子的，不需要额外加锁。
 */
static LDVDspMode g_preferred = LDV_DSP_MODE_ASM;

/* 最近一次 Apply 实际写下去的模式 */
static LDVDspMode g_applied = LDV_DSP_MODE_ASM;

/* ------------------------------------------------------------------------ */
/* 能力查询                                                                  */
/* ------------------------------------------------------------------------ */

int LDVDspAsmAvailable(void) {
    return LDV_DSP_ASM_COMPILED;
}

const char *LDVDspAsmName(void) {
    return LDV_DSP_ASM_NAME;
}

const char *LDVDspModeName(LDVDspMode mode) {
    if (mode == LDV_DSP_MODE_ASM) {
        return LDV_DSP_ASM_COMPILED ? LDV_DSP_ASM_NAME : "asm(未编译)";
    }
    return "C";
}

/* ------------------------------------------------------------------------ */
/* 模式选择                                                                  */
/* ------------------------------------------------------------------------ */

void LDVDspSetPreferredMode(LDVDspMode mode) {
    if (!LDV_DSP_ASM_COMPILED) {
        /* 这个架构没有汇编，请求 ASM 就静默降级 */
        g_preferred = LDV_DSP_MODE_C;
        return;
    }
    g_preferred = (mode == LDV_DSP_MODE_ASM) ? LDV_DSP_MODE_ASM
                                             : LDV_DSP_MODE_C;
}

LDVDspMode LDVDspPreferredMode(void) {
    if (!LDV_DSP_ASM_COMPILED) return LDV_DSP_MODE_C;
    return g_preferred;
}

void LDVDspApply(void) {
    LDVDspMode mode = LDVDspPreferredMode();

    /*
     * 没有汇编可用时不需要动掩码 —— 那些切片上 dav1d 本来就只有 C 实现，
     * 掩码怎么写都一样。保持默认值可以少一处全局状态。
     */
    if (!LDV_DSP_ASM_COMPILED) {
        g_applied = LDV_DSP_MODE_C;
        return;
    }

    dav1d_set_cpu_flags_mask(mode == LDV_DSP_MODE_ASM ? ~0U : 0U);
    g_applied = mode;
}

LDVDspMode LDVDspActiveMode(void) {
    if (!LDV_DSP_ASM_COMPILED) return LDV_DSP_MODE_C;
    return g_applied;
}
