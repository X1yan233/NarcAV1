#!/bin/bash

###############################################################################
# NarcAV1 — armv7/arm64 静态库一键构建
#
# 产物:
#   dist/libNarcAV1.a        armv7 + arm64 (lipo 合并)
#   dist/libNarcAV1_armv7.a
#   dist/libNarcAV1_arm64.a
#
# 库内容:
#   - src/dav1d/     改造后的 dav1d 1.5（iOS 4 纪律适配 + armv7/arm64 NEON）
#   - src/libdav1d/  解码会话封装（Session/CoreDecoder/Dsp/IVF）
#   - src/base/      通用层：帧缓存 / 缩放 / 色彩转换 / MP4 解封装 /
#                    音频（解码/输出/会话）/ GPU 显示视图（GLES2/3 + Metal, ObjC）
#
# 默认环境口径:
#   macOS 10.9.5 + Xcode 6.x + iPhoneOS8.0.sdk
#   armv7 -> iOS 4.0   arm64 -> iOS 5.1.1
#
# ★ SDK 路径与工具全部在下面 CONFIG 区，克隆后按你的环境改即可。
#
# NEON 汇编说明:
#   旧 Xcode（LLVM MC 3.5）编不了 dav1d 1.5 的 GNU 语法汇编（.irp 族
#   直接死循环），所以仓库自带预构建汇编归档（Mach-O，llvm-mingw 交叉
#   汇编产物，manifest.txt 有工具链与符号清单）。默认直接链接它。
#   如果你有现代 LLVM（17+，Mac/Linux/Windows 均可），设 ARM_CLANG
#   指向它的 clang 即可现场重编汇编（汇编与 C 完全解耦，见
#   asm_rebuild()）。
###############################################################################

set -u

###############################################################################
# CONFIG —— 按你的环境修改这里
###############################################################################

# iPhoneOS SDK：填名字（走 xcrun 查找）或直接填 SDK 绝对路径
SDK_NAME="iphoneos8.0"
# SDK_PATH="/path/to/iPhoneOS8.0.sdk"     # 显式路径优先于 SDK_NAME

# 部署目标
ARMV7_DEPLOYMENT="4.0"
ARM64_DEPLOYMENT="5.1.1"

# Apple 工具链（默认走 xcrun；也可以全填绝对路径脱离 xcode 环境）
CLANG="xcrun -f clang"
LIBTOOL="xcrun -f libtool"
LIPO="xcrun -f lipo"
NM="xcrun -f nm"

# 现代 LLVM clang（用于现场重编 NEON 汇编；留空 = 用仓库预构建归档）
#   例: ARM_CLANG="/usr/local/bin/clang"
#       Windows: ARM_CLANG="C:/llvm-mingw-*/bin/clang.exe"
ARM_CLANG=""

# 并行度（C 编译用）
JOBS=4

###############################################################################

cd "$(cd "$(dirname "$0")" && pwd)"
ROOT="$(pwd)"
SRC="$ROOT/src"
OBJROOT="$ROOT/build"
DIST="$ROOT/dist"

log()  { echo ""; echo "==== $1 ===="; }
die()  { echo "" >&2; echo "ERROR: $1" >&2; exit 1; }

[ -d "$SRC/dav1d" ]      || die "缺 src/dav1d（改造后的 dav1d）"
[ -d "$SRC/libdav1d" ]   || die "缺 src/libdav1d"
[ -d "$SRC/base" ]       || die "缺 src/base"

# ---- 工具链解析 -------------------------------------------------------------
if [ -n "${SDK_PATH:-}" ]; then
    SDK="$SDK_PATH"
else
    SDK="$(xcrun --sdk "$SDK_NAME" --show-sdk-path 2>/dev/null || true)"
fi
[ -n "$SDK" ] && [ -d "$SDK" ] || die "找不到 iPhoneOS SDK（SDK_PATH=$SDK_PATH, SDK_NAME=$SDK_NAME）"

# xcrun 包装解析成实际工具路径（只 fork 一次，后面直接调用）
for pair in "CLANG:clang" "LIBTOOL:libtool" "LIPO:lipo" "NM:nm"; do
    var="${pair%%:*}"; tool="${pair##*:}"
    if [ "$(eval "echo \$$var")" = "xcrun -f $tool" ]; then
        resolved="$(xcrun -f "$tool" 2>/dev/null || true)"
        [ -n "$resolved" ] && eval "$var=\"\$resolved\""
    fi
done

INC="-I $SRC/dav1d -I $SRC/dav1d/include -I $SRC/dav1d/src -I $SRC/libdav1d -I $SRC/base"

# dav1d 核心（上游源码，gnu99）
DAV1D_BASE_SOURCES="cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c \
getbits.c intra_edge.c itx_1d.c lf_mask.c lib.c log.c mem.c msac.c obu.c \
pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c thread_task.c warpmv.c wedge.c"

# 位深模板（meson 的两遍编译 → 仓库里已是现成封装文件，直接编）
DAV1D_TMPL="cdef cdef_apply fg_apply filmgrain ipred ipred_prepare itx \
lf_apply loopfilter looprestoration lr_apply mc recon"

# 本库 C 源（库相对路径）
OWN_C_SOURCES="libdav1d/LDVCoreDecoder.c libdav1d/LDVIVFSource.c \
libdav1d/LDVDecodeSession.c libdav1d/LDVDsp.c \
base/LDVPlatformTuning.c base/LDVMP4Source.c base/LDVFrameCache.c \
base/LDVDownscale.c base/LDVFrameDumper.c base/LDVColorConvert.c \
base/LDVAudioSession.c base/LDVAudioDecoder.c base/LDVAudioOutput.c \
base/LDVCpuUsage.c"

# 本库 ObjC 源（GPU 显示视图与 ObjC 封装；ARC，shim 除外）
OWN_OBJC_SOURCES="base/LDVVideoView.m base/LDVMetalVideoView.m \
base/LDVES2Transcoder.m libdav1d/libdav1d.m libdav1d/LDVDecoder.m"
OWN_OBJC_NOARC="base/LDVAutoreleasePoolShim.m"

###############################################################################
# 汇编
###############################################################################

# 现场重编 NEON 汇编（仅当 ARM_CLANG 提供了现代 LLVM clang）。
# 参数与上游 dav1d CI / llvm-mingw 交叉汇编流水线一致：
#   armv7: cortex-a9 + -mimplicit-it=always（统一语法下游离 IT 块）
#   arm64: 基线 armv8-a；SVE/dotprod 单元不在 asm64/ 内（config 关闭）
asm_rebuild()
{
    local outdir="$1"; mkdir -p "$outdir"
    local units_ok=0

    # armv7（17 个单元）
    for f in "$SRC/dav1d/asm32/"*_arm32.S; do
        $ARM_CLANG --target=armv7-apple-ios4.0.0 -mcpu=cortex-a9 \
            -mimplicit-it=always -fPIC -x assembler-with-cpp \
            -I "$SRC/dav1d" -I "$SRC/dav1d/asm32" -I "$SRC/dav1d/src/arm" \
            -I "$SRC/dav1d/src" -c "$f" -o "$outdir/$(basename "$f" .S).o" \
            || die "汇编失败: $f"
        units_ok=$((units_ok+1))
    done
    # arm64（17 个单元）
    for f in "$SRC/dav1d/asm64/"*_arm64.S; do
        $ARM_CLANG --target=aarch64-apple-ios7.0.0 -fPIC \
            -x assembler-with-cpp \
            -I "$SRC/dav1d" -I "$SRC/dav1d/asm64" -I "$SRC/dav1d/src/arm" \
            -I "$SRC/dav1d/src" -c "$f" -o "$outdir/$(basename "$f" .S).o" \
            || die "汇编失败: $f"
        units_ok=$((units_ok+1))
    done
    echo "NEON 汇编重编完成（$units_ok 单元）→ $outdir"
}

###############################################################################
# C / ObjC 编译
###############################################################################

compile_arch()
{
    local arch="$1"; local minos="$2"
    local objdir="$OBJROOT/$arch/obj"
    mkdir -p "$objdir"
    local t0=$SECONDS

    # ---- dav1d 核心：基础源（gnu99）----
    for s in $DAV1D_BASE_SOURCES; do
        $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
            -isysroot "$SDK" -std=gnu99 -O2 -DNDEBUG $INC \
            -c "$SRC/dav1d/src/$s" -o "$objdir/dav1d_${s%.c}.o" || die "编译失败: dav1d/src/$s ($arch)"
    done

    # ---- dav1d 核心：位深模板封装（8bpc / 16bpc 各一遍）----
    for bd in 8 16; do
        for t in $DAV1D_TMPL; do
            $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
                -isysroot "$SDK" -std=gnu99 -O2 -DNDEBUG $INC \
                -c "$SRC/dav1d/bitdepth_$bd/${t}_${bd}bpc.c" \
                -o "$objdir/dav1d_${t}_${bd}bpc.o" \
                || die "编译失败: bitdepth_$bd/$t ($arch)"
        done
    done

    # ---- dav1d 核心：arm 架构探测（src/arm/cpu.c，改名避基名冲突）----
    $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
        -isysroot "$SDK" -std=gnu99 -O2 -DNDEBUG $INC \
        -c "$SRC/dav1d/src/arm/cpu.c" -o "$objdir/dav1d_arch_cpu.o" \
        || die "编译失败: src/arm/cpu.c ($arch)"

    # ---- 本库 C（c99 + DARWIN 扩展；AudioOutput 需压废弃告警，见 vendor 笔记）----
    for s in $OWN_C_SOURCES; do
        local extra=""
        case "$(basename "$s")" in
            LDVAudioOutput.c) extra="-Wno-deprecated-declarations" ;;
        esac
        $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
            -isysroot "$SDK" -std=c99 -D_DARWIN_C_SOURCE -O2 -DNDEBUG \
            $extra $INC -c "$SRC/$s" -o "$objdir/$(basename "$s" .c).o" \
            || die "编译失败: $s ($arch)"
    done

    # ---- 本库 ObjC（ARC；shim 手动管理内存）----
    for s in $OWN_OBJC_SOURCES; do
        $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
            -isysroot "$SDK" -fobjc-arc -O2 -DNDEBUG $INC \
            -c "$SRC/$s" -o "$objdir/$(basename "$s" .m).o" \
            || die "编译失败: $s ($arch)"
    done
    for s in $OWN_OBJC_NOARC; do
        $CLANG -arch "$arch" -miphoneos-version-min="$minos" \
            -isysroot "$SDK" -fno-objc-arc -O2 -DNDEBUG $INC \
            -c "$SRC/$s" -o "$objdir/$(basename "$s" .m).o" \
            || die "编译失败: $s ($arch)"
    done

    echo "$arch 完成（$((SECONDS-t0))s，$(ls "$objdir" | wc -l | tr -d ' ') 个目标）"
}

###############################################################################
# 主流程
###############################################################################

log "NarcAV1 静态库构建"
echo "SDK     : $SDK"
echo "部署目标: armv7 -> iOS $ARMV7_DEPLOYMENT / arm64 -> iOS $ARM64_DEPLOYMENT"
echo "汇编    : $([ -n "$ARM_CLANG" ] && echo "现场重编 ($ARM_CLANG)" || echo "仓库预构建归档")"

rm -rf "$OBJROOT"; mkdir -p "$OBJROOT" "$DIST"

# ---- NEON 汇编 ----
ASM32_A="$SRC/dav1d/asm32-macho/libdav1d-asm32-armv7.a"
ASM64_A="$SRC/dav1d/asm64-macho/libdav1d-asm64-arm64.a"
if [ -n "$ARM_CLANG" ]; then
    asm_rebuild "$OBJROOT/asm"
    ASM32_A=""      # 现场汇编的目标文件已在 objdir，直接进归档
    ASM64_A=""
elif [ ! -f "$ASM32_A" ] || [ ! -f "$ASM64_A" ]; then
    die "缺少预构建汇编归档（$ASM32_A / $ASM64_A）—— 或设置 ARM_CLANG 现场重编"
fi

# ---- C / ObjC（两架构）----
compile_arch armv7 "$ARMV7_DEPLOYMENT"
compile_arch arm64 "$ARM64_DEPLOYMENT"

# ---- 归档（libtool 会把预构建汇编归档的成员一起并入）----
log "归档 armv7"
$LIBTOOL -static -o "$DIST/libNarcAV1_armv7.a" \
    "$OBJROOT/armv7/obj/"*.o $([ -n "$ASM32_A" ] && echo "$ASM32_A") \
    || die "libtool 失败 (armv7)"

log "归档 arm64"
$LIBTOOL -static -o "$DIST/libNarcAV1_arm64.a" \
    "$OBJROOT/arm64/obj/"*.o $([ -n "$ASM64_A" ] && echo "$ASM64_A") \
    || die "libtool 失败 (arm64)"

# ---- 合并 fat 库 ----
log "lipo 合并"
$LIPO -create "$DIST/libNarcAV1_armv7.a" "$DIST/libNarcAV1_arm64.a" \
    -output "$DIST/libNarcAV1.a" || die "lipo 合并失败"

$LIPO -info "$DIST/libNarcAV1.a" || die "fat 库校验失败"
echo "$($LIPO -info "$DIST/libNarcAV1.a")" | grep -q armv7 || die "fat 库缺 armv7"
echo "$($LIPO -info "$DIST/libNarcAV1.a")" | grep -q arm64 || die "fat 库缺 arm64"

# ---- 符号冒烟：解码入口 / 会话 API / NEON 汇编入口 ----
log "符号冒烟检查"
for sym in _dav1d_open _LDVDecodeSessionStart _LDVFrameCachePushOwned; do
    $NM -gU "$DIST/libNarcAV1_armv7.a" 2>/dev/null | grep -q " $sym\$" \
        || die "缺符号 $sym（armv7）"
    $NM -gU "$DIST/libNarcAV1_arm64.a" 2>/dev/null | grep -q " $sym\$" \
        || die "缺符号 $sym（arm64）"
    echo "  ok $sym"
done

echo ""
echo "========================================"
echo "构建完成"
echo "========================================"
echo "  $DIST/libNarcAV1.a        (armv7 + arm64)"
echo "  $DIST/libNarcAV1_armv7.a"
echo "  $DIST/libNarcAV1_arm64.a"
echo ""
echo "接入宿主 app：把 libNarcAV1.a 与 src/ 下的头文件加入工程，"
echo "链接 Metal / OpenGLES / AudioToolbox / AudioUnit / CoreFoundation 框架；"
echo "Metal 需 weak link（arm64 部署目标 < 8.0）。"
