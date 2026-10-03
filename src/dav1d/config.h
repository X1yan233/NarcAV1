/*
 * config.h —— dav1d 的构建期配置
 *
 * ============================================================================
 * 这个文件本来是 meson 用 configure_file() 从编译器/汇编器探测结果生成的
 * （见 dav1d-master/meson.build 里所有 cdata.set* 调用，以及第 600 行的
 *  config_h_target = configure_file(output: 'config.h', configuration: cdata)）。
 *
 * 我们把 dav1d 源码直接复制进工程，没有 meson，所以这个文件改为**手写**，
 * 并且用编译期宏按架构自动取值 —— 这样同一份文件能同时服务
 * armv7 / armv7s / arm64 三个切片，以及 macOS 宿主的逻辑验证构建。
 *
 * 它被**每一个** dav1d .c 和 .S 文件 include（.S 也走 C 预处理器），
 * 所以这里只能出现 #define / #if / 注释，不能有 C 声明。
 *
 * 取值依据（逐条对应 meson.build 的探测）：
 *   架构宏      ← 编译器预定义宏（__arm__ / __aarch64__ / ...）
 *   HAVE_ASM    ← 是否编译汇编（见下面的 LIBDAV1D_GENERIC_BUILD）
 *   汇编能力    ← Xcode 6 的 Apple 集成汇编器只支持 ARMv8.0 基线，
 *                 不支持 .arch / .func / .arch_extension，所以全部关掉。
 *                 dotprod / i8mm / SVE / SVE2 是 ARMv8.2+ 特性，Xcode 6
 *                 的汇编器完全不认，必须为 0；对应的 mc_dotprod.S /
 *                 mc16_sve.S 有 #if HAVE_DOTPROD / #if HAVE_SVE2 守卫，
 *                 会编译成空文件，这是预期行为。
 *   平台能力    ← iOS 的 libSystem / SDK 实际情况
 * ============================================================================
 */

#ifndef LIBDAV1D_CONFIG_H
#define LIBDAV1D_CONFIG_H

/* ---------------------------------------------------------------------------
 * 通用 C 构建开关
 *
 * 定义 LIBDAV1D_GENERIC_BUILD 后：所有架构特性关闭、不编译任何汇编，
 * dav1d 退化为纯 C 实现。用于在 macOS 宿主上跑 demo 做逻辑验证
 * （宿主是 x86_64，而 dav1d 的 x86 汇编需要 NASM，Xcode 集成汇编器不认）。
 *
 * 注意：HAVE_ASM=0 时 dav1d_init_cpu() 整体被编译掉
 * （src/cpu.c:63 的 #if HAVE_ASM），因此不会引用任何架构的
 * dav1d_get_cpu_flags_*()，也就不需要 src/x86/cpu.c。
 * ------------------------------------------------------------------------- */
#if defined(LIBDAV1D_GENERIC_BUILD)

#define ARCH_AARCH64    0
#define ARCH_ARM        0
#define ARCH_X86        0
#define ARCH_X86_32     0
#define ARCH_X86_64     0
#define ARCH_PPC64LE    0
#define ARCH_RISCV      0
#define ARCH_RV32       0
#define ARCH_RV64       0
#define ARCH_LOONGARCH  0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define HAVE_ASM        0
#define TRIM_DSP_FUNCTIONS 0

#else /* ---------------- iOS 真机构建 ---------------- */

/* ---- 架构 ---- */
#if defined(__aarch64__) || defined(__arm64__)
#define ARCH_AARCH64    1
#define ARCH_ARM        0
#elif defined(__arm__)
#define ARCH_AARCH64    0
#define ARCH_ARM        1
#else
#define ARCH_AARCH64    0
#define ARCH_ARM        0
#endif

#define ARCH_X86        0
#define ARCH_X86_32     0
#define ARCH_X86_64     0
#define ARCH_PPC64LE    0
#define ARCH_RISCV      0
#define ARCH_RV32       0
#define ARCH_RV64       0
#define ARCH_LOONGARCH  0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0

/* ---------------------------------------------------------------------------
 * 汇编总开关：★ 按目标架构条件化
 *
 *   arm64（AArch64）  → HAVE_ASM 1  —— asm64 汇编 + C 兜底**都编进去**，
 *                        运行时用 dav1d_set_cpu_flags_mask() 二选一。
 *                        这就是"asm64 加 C 的混合方式"。
 *   armv7 / armv7s    → HAVE_ASM 0  —— 纯 C
 *   i386 / x86_64     → HAVE_ASM 0  —— 纯 C（模拟器一行汇编都不编）
 *
 * ★ 一个 config.h 同时服务全部三种切片，靠的是 __aarch64__ 这个编译期宏。
 *   不需要为不同架构生成不同的 config.h —— 那样一旦同步出错，
 *   就会出现"arm64 切片编了 arm32 汇编"这种极难定位的问题。
 *
 * ★ 这条判断必须与 libdav1d/LDVDsp.c 里的 LDV_DSP_ASM_COMPILED 保持一致。
 *   生成器会校验（_check_dsp_guard_consistency），不一致直接报错。
 *
 * 为什么 armv7 不给汇编：
 *   clang 3.5 的预处理器在 arm32 汇编上段错误（见 ARCHITECTURE P18），
 *   无法从源码侧规避。arm64 汇编没这个问题。
 * ------------------------------------------------------------------------- */
#if defined(LDV_FORCE_NO_ASM)
#  define HAVE_ASM        0
#elif defined(__aarch64__) || defined(__arm64__)
#  define HAVE_ASM        1
#elif defined(__arm__) && !defined(__aarch64__)
#  define HAVE_ASM        1
#else
#  define HAVE_ASM        0
#endif

/* ---------------------------------------------------------------------------
 * TRIM_DSP_FUNCTIONS 必须是 0
 *
 * =1 时 dav1d 会把**编译期已知的** CPU 特性 OR 进运行时 flags
 * （src/cpu.h:127 的 `flags |= dav1d_get_default_cpu_flags();`），
 * 于是 dav1d_set_cpu_flags_mask(0) 也压不住 NEON ——
 * "切到纯 C"这个功能会**静默失效**（按钮点了没反应，但没有任何报错）。
 *
 * 代价是编译进去的 C 兜底实现不会被死代码消除，体积略大。
 * 这是运行时切换必须付的代价。
 *
 * （meson 在 release 下默认 trim_dsp=if-release，即 =1。我们是刻意关掉。）
 * ------------------------------------------------------------------------- */
#define TRIM_DSP_FUNCTIONS 0

#endif /* LIBDAV1D_GENERIC_BUILD */

/* ---------------------------------------------------------------------------
 * 位深：8bit 与 10/12bit 都启用
 * 对应 meson -Dbitdepths=8,16。GPU 侧会做 10bit→8bit 转换，
 * 所以保留 16 有实际价值（代价是 DSP 编译两遍）。
 * ------------------------------------------------------------------------- */
#define CONFIG_8BPC     1
#define CONFIG_16BPC    1

/* ---------------------------------------------------------------------------
 * 日志：开启。dav1d 的错误日志走 logger 回调，
 * 我们保留默认回调（打 stderr）方便定位码流问题。
 * 需要省体积时可改成 0。
 * ------------------------------------------------------------------------- */
#define CONFIG_LOG      1

/* ---------------------------------------------------------------------------
 * arm64 汇编能力
 *
 * HAVE_AS_ARCH_DIRECTIVE：是否支持 ".arch <level>"
 * HAVE_AS_FUNC：是否支持 ".func" / ".endfunc"
 *   —— Apple 集成汇编器都不支持，置 0（src/arm/asm.S:244/358 有守卫）
 *
 * dotprod / i8mm / sve / sve2 是 ARMv8.2+ 特性，Xcode 6 的汇编器不认，
 * 全部置 0。src/arm/64/mc_dotprod.S 与 mc16_sve.S 因此编译为空文件。
 * 将来换到支持这些扩展的汇编器时，把对应项改成 1 并重新启用那两个 .S 即可。
 * ------------------------------------------------------------------------- */
#define HAVE_AS_FUNC                          0
#define HAVE_AS_ARCH_DIRECTIVE                0
#define AS_ARCH_LEVEL                         armv8-a

#define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE     0
#define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE        0
#define HAVE_AS_ARCHEXT_SVE_DIRECTIVE         0
#define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE        0

#define HAVE_DOTPROD                          0
#define HAVE_I8MM                             0
#define HAVE_SVE                              0
#define HAVE_SVE2                             0

/* ---------------------------------------------------------------------------
 * 符号前缀
 *
 * meson 对 darwin/ios/tvos 会设 PREFIX=1（meson.build:527）。
 * src/arm/asm.S:259 用它在 EXTERN 前加下划线：
 *   #ifdef PREFIX  →  EXTERN = _dav1d_   （Mach-O 的 C 符号也带下划线）
 *   #else          →  EXTERN = dav1d_
 * 所以 iOS 上必须为 1，否则汇编定义的符号名与 C 侧引用对不上，链接失败。
 * ------------------------------------------------------------------------- */
#define PREFIX                                1

/* ---------------------------------------------------------------------------
 * 平台能力（对应 meson.build 里的 cc.has_function / cc.check_header 探测）
 * ------------------------------------------------------------------------- */

/* 内存对齐分配：src/mem.h:89-111 的分支顺序是
 *   _WIN32 → HAVE_POSIX_MEMALIGN → HAVE_MEMALIGN → HAVE_ALIGNED_ALLOC → 手动对齐
 * iOS 上 posix_memalign 命中第一个分支。
 * HAVE_ALIGNED_ALLOC 显式置 0：aligned_alloc 是 C11 且 iOS 13+ 才可用，
 * 即使探测到了也不要用，避免将来分支顺序变化踩坑。 */
#define HAVE_POSIX_MEMALIGN    1
#define HAVE_MEMALIGN          0
#define HAVE_ALIGNED_ALLOC     0

#define HAVE_SYS_TYPES_H       1
#define HAVE_UNISTD_H          1
#define HAVE_IO_H              0
#define HAVE_DLSYM             1

/* 无 <sys/auxv.h>：src/cpu.c:116 的 dav1d_getauxval 会走
 * "return 0" 兜底，arm 的 CPU 探测改走 src/arm/cpu.c:75 的
 * #elif defined(__APPLE__) 分支（sysctlbyname）。逻辑自洽。 */
#define HAVE_GETAUXVAL         0
#define HAVE_ELF_AUX_INFO      0

#define HAVE_PTHREAD_NP_H              0
#define HAVE_PTHREAD_GETAFFINITY_NP    0
#define HAVE_PTHREAD_SETAFFINITY_NP    0

/* ★ 关键：必须为 0。
 *
 * meson 用 cc.has_function('pthread_setname_np') 对着 **SDK** 探测
 * （meson.build:268），现代 SDK 里有这个符号所以会判定为 1。
 * 但 iOS 4/5 的运行时**不导出**它 —— 一旦置 1，
 * src/thread.h:156 的 Apple 分支就会直接调用它，
 * 结果是 dyld 在加载阶段因缺符号崩溃。
 *
 * 置 0 后 src/thread.h 会走最后的空实现兜底分支（thread.h:188-190），
 * 线程名设置变成 no-op，对功能无影响。
 *
 * 另外 src/thread.h 的副本上还额外打了 iOS 版本守卫补丁（见本脚本
 * patch_thread_h()），双保险。 */
#define HAVE_PTHREAD_SETNAME_NP        0
#define HAVE_PTHREAD_SET_NAME_NP       0

/* clang 支持 _Generic（C11 特性，在 C99 模式下也可用） */
#define HAVE_C11_GENERIC       1

/* clock_gettime 是 iOS 10+ 才有的 API。dav1d 库本体不用它
 * （只有 tools/ 和 tests/ 用），但为保险起见置 0。 */
#define HAVE_CLOCK_GETTIME     0
#define HAVE_SIGACTION         1

/* 小端 */
#define ENDIANNESS_BIG         0

/* 调试用堆分配追踪（src/lib.c:604）。正式构建关闭。 */
#define TRACK_HEAP_ALLOCATIONS 0

/* 注意：以下宏**故意不定义**，让 dav1d 自己推导，定义反而会冲突或改变语义
 *
 *   PIC           src/arm/asm.S:244 在未定义时会从 __PIC__ / __pic__ 推导，
 *                 iOS 上 __PIC__ 已定义，走 Apple 专用的
 *                 .non_lazy_symbol_pointer / .indirect_symbol 路径
 *                 （src/arm/32/util.S:68），这才是正确行为。
 *   CONFIG_THUMB  src/arm/asm.S:229 自己按 __thumb__ 定义。
 *   PRIVATE_PREFIX src/arm/asm.S:252 默认 dav1d_。
 *   DEBUG_*       src/obu.c 里的调试开关，未定义即为关闭。
 */

#endif /* LIBDAV1D_CONFIG_H */
