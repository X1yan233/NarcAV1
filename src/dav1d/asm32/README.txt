本目录是 dav1d src/arm/32/ 的副本（由 scripts/vendor-dav1d.py 生成）。

为什么要分目录 + 重命名：
  arm32 与 arm64 的汇编文件**同名**（src/arm/32/cdef.S 与 src/arm/64/cdef.S），
  但一个 Xcode target 里必须按架构排除其中一个。Xcode 的
  EXCLUDED_SOURCE_FILE_NAMES 是按**文件名**匹配的，同名文件无法区分，
  所以把编译单元重命名为 *_arm32.S。

为什么可以重命名：
  .S 之间只通过 util.S / cdef_tmpl.S / looprestoration_tmpl.S 互相 include
  （相对路径），这三个辅助文件保持原名不动，因此重命名编译单元不影响
  任何 include 解析。asm.S 通过 -I 头文件搜索路径从
  Vendor/dav1d/src/arm/asm.S 引入，也不受影响。

排除规则见 project.pbxproj 里的 EXCLUDED_SOURCE_FILE_NAMES[arch=...]。
