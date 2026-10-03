/* 自动生成，请勿手工修改 —— 见 scripts/vendor-dav1d.py
 *
 * 本文件通过 -include 强制注入到每个 dav1d 编译单元（.c 与 .S）的最前面，
 * 用于屏蔽**上游代码固有**、我们不想维护的告警。
 *
 * 为什么用 -include + pragma，而不是在 OTHER_CFLAGS 里加 -Wno-*：
 *   Xcode 拼编译命令时各段 flag 的顺序不保证。-Wno-conditional-uninitialized
 *   如果排在 -Wconditional-uninitialized 之前就完全无效。
 *   #pragma clang diagnostic 在源码里生效，与命令行顺序无关。
 *
 * 只对 Vendor/dav1d/ 下的文件生效（由 per-file COMPILER_FLAGS 指定），
 * 本库自己的代码（libdav1d/）仍然保持全量告警。
 */

#ifndef LIBDAV1D_DAV1D_PREFIX_H
#define LIBDAV1D_DAV1D_PREFIX_H

#if defined(__clang__)

/* ---- -Wconditional-uninitialized：dav1d 上游有一批误报 ----
 *
 * 典型形态是"两个变量成对赋值、其中一个当哨兵"，编译器无法关联该条件：
 *   src/obu.c:966   off_before_idx / off_after_idx
 *                   （off_before 初值 -1，只在赋值 off_before_idx 的分支里变成 >= 0，
 *                     所以 if ((off_before | off_after) >= 0) 里它一定已赋值）
 *   src/obu.c:987   off_before2_idx（同上）
 *   src/thread_task.c:498        off
 *   src/ipred_prepare_tmpl.c:142/194  dst_top（have_top ? dst_top[-1] : dst[-1]）
 *   src/loopfilter_tmpl.c:87-107 p2..p6 / q2..q6 / flat8in / flat8out（同形态）
 *   src/refmvs.c:407/572/627/629 b_top / b_left / refmv_ctx / newmv_ctx
 * dav1d 上游自己的构建也不开这个告警。
 * 如果哪天改用了支持该告警的编译器（clang 3.7+ 的诊断质量更好），
 * 可以删掉这一行重新评估。 */
#pragma clang diagnostic ignored "-Wconditional-uninitialized"

/* ---- -Wunicode：arm32 汇编里的 \\u 与 UCN 转义语法冲突 ----
 *
 * dav1d 的 arm32 汇编用 GAS 宏语法 \uv_layout / \uv_coeff 引用宏参数，
 * 而 C 预处理器会把开头的 \\u 当成"通用字符名"（\\uXXXX）转义，
 * 于是报 "\\u used with no following hex digits; treating as '\\' followed by identifier"。
 * 因为 \\u 后面跟的是 v（不是十六进制数字），clang 会原样保留这两个字符，
 * 所以**预处理结果是对的、汇编仍然正确**，纯粹是噪音。 */
#pragma clang diagnostic ignored "-Wunicode"

#endif /* __clang__ */

#endif /* LIBDAV1D_DAV1D_PREFIX_H */
