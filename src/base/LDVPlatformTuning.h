/*
 * LDVPlatformTuning.h — 按设备能力选择解码参数
 *
 * 这是"针对 iPhone 3GS / iPhone 4（Apple A4）的特殊优化"的落点。
 *
 * 设计前提（见 docs/ARCHITECTURE.md §6.3）：
 *   1. dav1d 的线程数不是随便设的。n_tc = n_threads，n_fc = min(max_frame_delay, n_tc)
 *      （dav1d-master/src/lib.c:122-125），且 n_tc == 1 时 dav1d 完全不创建线程
 *      （src/lib.c:226 的 if (c->n_tc > 1)），n_fc == 1 时也不分配 out_delayed
 *      （src/lib.c:244 的 if (c->n_fc > 1)）。
 *   2. 所以单核设备（A4 / Cortex-A8）上的正确做法是 n_threads = 1 且
 *      max_frame_delay = 1 —— 不是"少开几个线程"，而是**一个都不开**。
 *      省掉每线程 1MB 栈（src/lib.c:161-163）和全部互斥/条件变量开销。
 *   3. 多核设备（A5 – A9X）上，因为 n_fc <= n_tc，要提高帧级并行深度
 *      就必须把 n_threads 设大（AV1 的 tile 数由码流决定，单 tile 流上
 *      提高 n_threads 的收益全部来自帧级并行）。
 */

#ifndef LDV_PLATFORM_TUNING_H
#define LDV_PLATFORM_TUNING_H

#include <stddef.h>
#include "LDVTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* inloop filter 位掩码。数值必须与 dav1d 的 enum Dav1dInloopFilterType 一致
 * （dav1d-master/include/dav1d/dav1d.h:61-69）。 */
#define LDV_INLOOP_FILTER_NONE        0
#define LDV_INLOOP_FILTER_DEBLOCK     (1 << 0)
#define LDV_INLOOP_FILTER_CDEF        (1 << 1)
#define LDV_INLOOP_FILTER_RESTORATION (1 << 2)
#define LDV_INLOOP_FILTER_ALL         (LDV_INLOOP_FILTER_DEBLOCK | \
                                       LDV_INLOOP_FILTER_CDEF    | \
                                       LDV_INLOOP_FILTER_RESTORATION)

typedef enum LDVDeviceClass {
    LDV_DEVICE_CLASS_UNKNOWN = 0,
    LDV_DEVICE_CLASS_A4,       /* Apple A4 / Samsung S5PC100，Cortex-A8 单核：
                                * iPhone 3GS(iPhone2,1)、iPhone 4(iPhone3,x)、
                                * iPad 1(iPad1,x)、iPod touch 3/4、Apple TV 2 */
    LDV_DEVICE_CLASS_SINGLE,   /* 其它单核设备 */
    LDV_DEVICE_CLASS_MULTI,    /* 双核及以上：A5 – A9X */
    LDV_DEVICE_CLASS_HOST      /* 非 iOS 宿主（macOS），仅用于逻辑验证 */
} LDVDeviceClass;

typedef struct LDVTuningProfile {
    LDVDeviceClass deviceClass;
    const char    *deviceClassLabel;   /* 静态字符串，生命周期同进程 */
    const char    *socLabel;           /* 例如 "Apple A4"；未知时为 "unknown" */

    /* —— dav1d 线程 —— */
    int nThreads;        /* → Dav1dSettings.n_threads（= n_tc） */
    int maxFrameDelay;   /* → Dav1dSettings.max_frame_delay（n_fc = min(它, n_tc)） */

    /* —— 解码成本开关 —— */
    int      applyGrain;       /* 0 = 关闭胶片颗粒（省一个完整 pass） */
    int      inloopFilters;    /* LDV_INLOOP_FILTER_* 位掩码 */
    unsigned frameSizeLimit;   /* 像素数上限，早拒绝超规格流，避免大分配直接 OOM */

    /* —— 内存与渐进式缓冲 —— */
    size_t bufferBudgetBytes;  /* 预缓冲总预算（T1 压缩包 + T2 解码帧） */
    int    maxAheadFrames;     /* 解码前瞻上限 */
    int    picturePoolFrames;  /* 建议的图片池 slab 数 */

    /* —— 输出档位上限 —— */
    int maxOutputWidth;
    int maxOutputHeight;

    /* —— GPU —— */
    int gpuAccelerationEnabled; /* A4 的 PowerVR SGX535 只用于最终显示，
                                 * 不参与 YUV→RGB / 缩放 / 格式转换链路 */
} LDVTuningProfile;

/* 运行时探测当前设备类别（sysctlbyname("hw.machine") + "hw.logicalcpu"）。 */
LDVDeviceClass LDVDeviceClassDetect(void);

/* 取指定类别的调参档。 */
LDVTuningProfile LDVTuningProfileForClass(LDVDeviceClass deviceClass);

/* 取当前设备的调参档（等价于 LDVTuningProfileForClass(LDVDeviceClassDetect())）。 */
LDVTuningProfile LDVTuningProfileForCurrentDevice(void);

/* 判断是否属于 A4 家族（iPhone 3GS / iPhone 4 / iPad 1 等）。 */
int LDVTuningIsA4Family(void);

/*
 * 每帧 CPU 缩小（缓存窗口贴合）在这台机器上是否可负担。
 * 0 = 不可负担：解码线程与 dav1d 串行，A4/单核/A5 上任何每帧重采样
 * 都是帧率净损失（4S 480p60 实测 34→15fps）—— 这些设备应解码原生
 * 尺寸，缩小交给 GPU（双线性，零 CPU 成本）。
 */
int LDVTuningCacheDownscaleAffordable(void);

/* 把 profile 写成一行可读文本，返回 buf。用于日志与自检。 */
const char *LDVTuningDescribe(const LDVTuningProfile *profile,
                              char *buf, size_t bufSize);

#ifdef __cplusplus
}
#endif

#endif /* LDV_PLATFORM_TUNING_H */
