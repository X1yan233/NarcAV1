/* 自动生成，请勿手工修改 —— 见 scripts/vendor-dav1d.py
 *
 * 为什么要这一层：src/arm/cpu.c 与 dav1d 里的另一个源文件**基名相同**，
 * 而 Xcode 按文件基名生成目标文件（Objects-normal/<arch>/<基名>.o），
 * 同名会互相覆盖，表现为链接期缺符号。
 *
 * 包一层之后基名变成 arm_cpu.c，冲突消失。
 * 内容原样 include，不改变任何编译语义。 */
#include "src/arm/cpu.c"
