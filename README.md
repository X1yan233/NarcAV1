# NarcAV1
NarcAV1 — AV1 decoder forked from dav1d, powered by Narcira GPU offload &amp; armv7/arm64 hand-tuned NEON. Built for legacy iOS (S5L8920/8922,A4–A9,required iOS 4.0+).

## Repository Layout

```
NarcAV1/
├── build.sh          one-shot static-library build (armv7 + arm64)
└── src/
    ├── dav1d/        patched dav1d 1.5 — iOS 4 discipline (no C11 atoms,
    │                 no iOS5+ API), armv7/arm64 NEON assembly, ready-made
    │                 config.h + vcs_version.h + bitdepth wrappers,
    │                 prebuilt Mach-O asm archives (asmNN-macho/)
    ├── libdav1d/     decode session layer: Session / CoreDecoder / Dsp / IVF
    └── base/         codec-agnostic shared layer: frame cache, downscaler,
                      color convert, MP4 demuxer, audio (decoder/output/session),
                      GPU display views (GLES2/3 passthrough + Metal, ObjC),
                      platform tuning, CPU usage tracker
```

`src/base` 的 GPU 显示链与 `src/dav1d` 的解码核心通过 `LDVFrameView`
（I420 planar + stride）解耦：解码端只出帧，缩放/色彩/节拍/缓存全部
在显示端完成；缓存满 → 背压等待，永不丢帧。

## Building

```bash
./build.sh
```

产物：`dist/libNarcAV1.a`（armv7 + arm64 fat 静态库），另附单架构切片。

- **默认口径**：macOS 10.9.5 + Xcode 6.x + `iphoneos8.0` SDK；
  armv7 → iOS 4.0，arm64 → iOS 5.1.1。
- **SDK 可配**：`build.sh` 顶部 CONFIG 区改 `SDK_PATH`（SDK 绝对路径）
  或 `SDK_NAME`（xcrun 名字），`ARMV7_DEPLOYMENT` / `ARM64_DEPLOYMENT` 同理。
- **NEON 汇编**：旧 Xcode（LLVM MC 3.5）编不了 dav1d 1.5 的 GNU 语法
  汇编，仓库自带预构建 Mach-O 归档（`src/dav1d/asmNN-macho/`，
  llvm-mingw 交叉汇编产物，`manifest.txt` 记录工具链与符号清单）。
  若你有现代 LLVM（17+），设 `ARM_CLANG=/path/to/clang` 即可现场重编
  （Mac / Linux / Windows 均可，汇编与 C 完全解耦）。

## Linking Into a Host App

把 `dist/libNarcAV1.a` 与 `src/` 下的头文件加入工程，链接
Metal / OpenGLES / AudioToolbox / AudioUnit / CoreFoundation；
Metal 需 weak link（arm64 部署目标低于 8.0 时运行时判定后端）。
