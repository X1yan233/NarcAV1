/* 自动生成，请勿手工修改 —— 见 scripts/vendor-dav1d.py
 *
 * dav1d 上游用 meson 把 src/lf_apply_tmpl.c 按位深各编译一遍，
 * 命令行传 -DBITDEPTH=16（src/meson.build:337-346）。
 *
 * Xcode 无法方便地把同一个文件用两组不同参数编译两遍，
 * 所以这里生成一层薄封装：先 #define BITDEPTH，再 #include 模板本体。
 * 模板本体（src/lf_apply_tmpl.c）本身不含 BITDEPTH 定义，
 * 由 include/common/bitdepth.h:36 检查必须已定义。
 *
 * 文件名带 _16bpc 后缀是为了避免与 src/ 下的同名基础源文件
 * 产生目标文件重名（例如 src/cdef.c 与 cdef_tmpl.c 的封装）。 */
#define BITDEPTH 16
#include "src/lf_apply_tmpl.c"
