/*
 * LDVPlatformTuning.c — 设备能力探测与调参档
 */

#include "LDVPlatformTuning.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

/* ------------------------------------------------------------------------ */
/* 设备探测                                                                  */
/* ------------------------------------------------------------------------ */

#if defined(__APPLE__)

static int LDVReadSysctlString(const char *name, char *buf, size_t bufSize) {
    size_t len = bufSize;
    if (!buf || bufSize == 0) return 0;
    buf[0] = '\0';
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0) return 0;
    buf[bufSize - 1] = '\0';
    return 1;
}

static int LDVReadSysctlInt(const char *name, int fallback) {
    int value = 0;
    size_t len = sizeof(value);
    if (sysctlbyname(name, &value, &len, NULL, 0) != 0) return fallback;
    return value > 0 ? value : fallback;
}

#else  /* 非 Apple 平台：仅用于把本文件编译进普通 C 测试工程 */

static int LDVReadSysctlString(const char *name, char *buf, size_t bufSize) {
    (void)name;
    if (buf && bufSize) buf[0] = '\0';
    return 0;
}

static int LDVReadSysctlInt(const char *name, int fallback) {
    (void)name;
    return fallback;
}

#endif

/* 逻辑核数。dav1d 自己也是用这个键探测（dav1d-master/src/cpu.c:103-107）。 */
static int LDVLogicalCPUCount(void) {
    int n = LDVReadSysctlInt("hw.logicalcpu", 0);
    if (n <= 0) n = LDVReadSysctlInt("hw.ncpu", 0);
    if (n <= 0) n = 1;
    return n;
}

static int LDVStringHasPrefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/*
 * A4 家族（Cortex-A8 单核）的机器标识前缀。
 * 这一组设备是本库要特别照顾的低端目标：
 *   iPhone2,1  iPhone 3GS   Samsung S5PC100, 256MB RAM, 屏幕 480x320, 最高 iOS 6.1.6
 *   iPhone3,x  iPhone 4     Apple A4,       512MB RAM, 屏幕 960x640, 最高 iOS 7.1.2
 *   iPad1,x    iPad 1       Apple A4,       256MB RAM
 *   iPod3,x    iPod touch 3 Samsung S5PC100, 256MB RAM
 *   iPod4,x    iPod touch 4 Apple A4,       256MB RAM
 *   AppleTV2,x Apple TV 2   Apple A4
 */
static const char *const kLDVA4MachinePrefixes[] = {
    "iPhone2,1",
    "iPhone3,",
    "iPad1,",
    "iPod3,",
    "iPod4,",
    "AppleTV2,",
    NULL
};

/* iPhone 3GS 专属覆盖：内存最小（256MB），屏幕 480x320 */
static const char *const kLDV3GSMachinePrefixes[] = {
    "iPhone2,1",
    "iPod3,",
    NULL
};

/*
 * A5 – A9X 的初始参数表。
 *
 * 注意 n_fc = min(max_frame_delay, n_tc)，所以 maxFrameDelay 必须 <= nThreads，
 * 设更大的值会被 dav1d 静默截断（dav1d-master/src/lib.c:124）。
 *
 * 这张表只是**初始值**，运行时由 LDVThreadTuner 按实测吞吐修正
 * （见 docs/ARCHITECTURE.md §6.3.3）。这里给的保守值在双核设备上
 * 是安全的起点。
 */
typedef struct LDVSoCProfile {
    const char *machinePrefix;
    const char *socLabel;
    int         nThreads;
    int         maxFrameDelay;
    size_t      budgetMB;
    int         maxOutputWidth;
    int         maxOutputHeight;
    int         cacheDownscale;  /* 1 = 每帧 CPU 缩小可负担（见
                                  * LDVTuningCacheDownscaleAffordable） */
} LDVSoCProfile;

static const LDVSoCProfile kLDVSoCProfiles[] = {
    /* 前缀          SoC 标签        nThreads  maxFrameDelay  预算MB  输出上限  缩小 */
    { "iPad2,",     "Apple A5",         2,        2,           8,   854,  480,  0 },
    { "iPhone4,",   "Apple A5",         2,        2,           8,   854,  480,  0 },
    { "iPod5,",     "Apple A5",         2,        2,           8,   854,  480,  0 },
    { "iPad3,",     "Apple A5X",        2,        2,           8,  1280,  720,  0 },
    { "iPhone5,",   "Apple A6",         2,        2,           8,  1280,  720,  1 },
    { "iPad4,",     "Apple A7",         3,        3,          64,  1920, 1080,  1 },
    { "iPhone6,",   "Apple A7",         3,        3,          64,  1920, 1080,  1 },
    { "iPhone7,",   "Apple A8",         3,        3,          64,  1920, 1080,  1 },
    { "iPad5,",     "Apple A8 / A8X",   3,        3,          64,  1920, 1080,  1 },
     /* A9：CPU 缩小实测吃掉 20%（1080p30 30→24fps），而 Metal 双立方
      * 在 r≈3 场景零压力 —— A9 交 GPU，缓存代价（1080p 17 帧）由
      * 2GB RAM 承担。 */
    { "iPhone8,",   "Apple A9",         4,        4,          64,  1920, 1080,  0 },
    { "iPad6,",     "Apple A9 / A9X",   4,        4,          64,  1920, 1080,  0 },
    { NULL,         NULL,               0,        0,           0,     0,    0,  0 }
};

int LDVTuningIsA4Family(void) {
    char machine[64];
    int i;
    if (!LDVReadSysctlString("hw.machine", machine, sizeof(machine))) return 0;
    for (i = 0; kLDVA4MachinePrefixes[i]; i++) {
        if (LDVStringHasPrefix(machine, kLDVA4MachinePrefixes[i])) return 1;
    }
    return 0;
}

static int LDVIs3GSFamily(void) {
    char machine[64];
    int i;
    if (!LDVReadSysctlString("hw.machine", machine, sizeof(machine))) return 0;
    for (i = 0; kLDV3GSMachinePrefixes[i]; i++) {
        if (LDVStringHasPrefix(machine, kLDV3GSMachinePrefixes[i])) return 1;
    }
    return 0;
}

/*
 * 每帧 CPU 缩小（缓存窗口贴合）在这台机器上是否可负担。
 *
 * ★ 背景（v8.19 真机教训，480p60 / iPhone 4S）：
 *   缩小发生在**解码线程**里，与 dav1d 串行 —— A5 正好顶着算力墙时，
 *   每帧加几毫秒重采样就是帧率腰斩（34fps → 15fps）。而 A4/A5 的
 *   SGX543 做 GPU 双线性缩小是白嫖，缓存用原生帧照样装得下。
 *
 * ★ v8.19c 起此接口只作诊断参考：app 已裁决**所有设备**的缓存缩小
 *   都交还 GPU（SE1 1080p30 实测 A9 上 CPU 缩小同样吃掉 20%：
 *   30fps → 24fps），ES3/Metal 用双立方 shader、ES2 用双线性。
 *   本函数保留给将来的动态换挡策略用。
 */
int LDVTuningCacheDownscaleAffordable(void) {
    char machine[64];
    int  i;

    if (LDVTuningIsA4Family() || LDVIs3GSFamily()) return 0;

    if (LDVReadSysctlString("hw.machine", machine, sizeof(machine))) {
        for (i = 0; kLDVSoCProfiles[i].machinePrefix; i++) {
            if (LDVStringHasPrefix(machine, kLDVSoCProfiles[i].machinePrefix)) {
                return kLDVSoCProfiles[i].cacheDownscale;
            }
        }
    }

    /* 表外未知设备：按核数保守判断 —— 单核不做，多核放开 */
    return LDVLogicalCPUCount() > 1;
}

LDVDeviceClass LDVDeviceClassDetect(void) {
    char machine[64];
    int  i;

    if (!LDVReadSysctlString("hw.machine", machine, sizeof(machine))) {
        return LDV_DEVICE_CLASS_HOST;   /* 拿不到机器标识：非 Apple 平台 */
    }

    /* 非 iOS 的 Apple 平台机器标识形如 "x86_64" / "arm64"，不含设备名 */
    if (!strstr(machine, "iPhone") && !strstr(machine, "iPad") &&
        !strstr(machine, "iPod")   && !strstr(machine, "AppleTV")) {
        return LDV_DEVICE_CLASS_HOST;
    }

    for (i = 0; kLDVA4MachinePrefixes[i]; i++) {
        if (LDVStringHasPrefix(machine, kLDVA4MachinePrefixes[i])) {
            return LDV_DEVICE_CLASS_A4;
        }
    }

    return LDVLogicalCPUCount() <= 1 ? LDV_DEVICE_CLASS_SINGLE
                                     : LDV_DEVICE_CLASS_MULTI;
}

/* ------------------------------------------------------------------------ */
/* 调参档                                                                    */
/* ------------------------------------------------------------------------ */

LDVTuningProfile LDVTuningProfileForClass(LDVDeviceClass deviceClass) {
    LDVTuningProfile p;
    memset(&p, 0, sizeof(p));

    switch (deviceClass) {

    /* ------------------------------------------------------------------ */
    /* Apple A4 / Cortex-A8：单核，本库最需要照顾的目标                    */
    /* ------------------------------------------------------------------ */
    case LDV_DEVICE_CLASS_A4:
        p.deviceClass      = LDV_DEVICE_CLASS_A4;
        p.deviceClassLabel = "Apple A4 / Cortex-A8 (single core)";
        p.socLabel         = "Apple A4";

        /*
         * 关键：n_threads = 1 且 max_frame_delay = 1。
         * 这样 n_tc = 1、n_fc = min(1, 1) = 1，dav1d 在 dav1d_open 里
         * 不会创建任何线程（src/lib.c:226 的 if (c->n_tc > 1) 为假），
         * 也不会分配 out_delayed（src/lib.c:244 的 if (c->n_fc > 1) 为假）。
         * 单核上这是唯一正确的选择：多开线程只会引入同步开销和
         * 每个线程 1MB 的栈（src/lib.c:161-163）。
         */
        p.nThreads      = 1;
        p.maxFrameDelay = 1;

        /*
         * 关闭胶片颗粒。dav1d 的 film grain 是一个独立的合成 pass，
         * 在 A4 上纯属浪费——SGX535 太弱、CPU 又是单核，
         * 而这个 pass 对主观画质影响很小。
         */
        p.applyGrain = 0;

        /* 环路滤波全开。去块 / CDEF / loop restoration 直接决定画质，
         * 关掉省下的时间远不抵画质损失。 */
        p.inloopFilters = LDV_INLOOP_FILTER_ALL;

        /*
         * 早拒绝超规格流。A4 设备 RAM 只有 256–512MB，
         * 一帧 1080p 参考帧就要 3MB，8 个参考帧 + 延迟帧很容易把进程撑爆。
         * 在 dav1d 分配之前用 frame_size_limit 挡掉，比分配失败更可控。
         */
        p.frameSizeLimit = 1280u * 720u;

        p.bufferBudgetBytes = 8u * 1024u * 1024u;   /* 规格里的在线默认档 */
        p.maxAheadFrames    = 2;   /* 单核上深前瞻会直接拖慢当前帧的解码 */
        p.picturePoolFrames = 4;   /* n_fc = 1，除参考帧外几乎不需要额外 slab */

        p.maxOutputWidth  = 854;
        p.maxOutputHeight = 480;

        /* A4 的 PowerVR SGX535 只用来把最终画面贴到屏幕上，
         * 不参与 YUV→RGB / 缩放 / 格式转换链路。 */
        p.gpuAccelerationEnabled = 0;
        break;

    /* ------------------------------------------------------------------ */
    /* 其它单核设备                                                        */
    /* ------------------------------------------------------------------ */
    case LDV_DEVICE_CLASS_SINGLE:
        p.deviceClass      = LDV_DEVICE_CLASS_SINGLE;
        p.deviceClassLabel = "single-core (generic)";
        p.socLabel         = "unknown (single core)";
        p.nThreads         = 1;
        p.maxFrameDelay    = 1;
        p.applyGrain       = 0;
        p.inloopFilters    = LDV_INLOOP_FILTER_ALL;
        p.frameSizeLimit   = 1920u * 1080u;
        p.bufferBudgetBytes = 8u * 1024u * 1024u;
        p.maxAheadFrames    = 2;
        p.picturePoolFrames = 4;
        p.maxOutputWidth  = 1280;
        p.maxOutputHeight = 720;
        p.gpuAccelerationEnabled = 1;
        break;

    /* ------------------------------------------------------------------ */
    /* 双核及以上（A5 – A9X）：走 SoC 参数表                               */
    /* ------------------------------------------------------------------ */
    case LDV_DEVICE_CLASS_MULTI:
    default: {
        char machine[64];
        int  i;
        int  matched = 0;
        int  cores;

        p.deviceClass      = LDV_DEVICE_CLASS_MULTI;
        p.deviceClassLabel = "multi-core (A5 - A9X)";
        p.socLabel         = "unknown (multi-core)";
        p.inloopFilters    = LDV_INLOOP_FILTER_ALL;
        p.applyGrain       = 1;

        if (LDVReadSysctlString("hw.machine", machine, sizeof(machine))) {
            for (i = 0; kLDVSoCProfiles[i].machinePrefix; i++) {
                if (LDVStringHasPrefix(machine, kLDVSoCProfiles[i].machinePrefix)) {
                    p.socLabel       = kLDVSoCProfiles[i].socLabel;
                    p.nThreads       = kLDVSoCProfiles[i].nThreads;
                    p.maxFrameDelay  = kLDVSoCProfiles[i].maxFrameDelay;
                    p.bufferBudgetBytes =
                        kLDVSoCProfiles[i].budgetMB * 1024u * 1024u;
                    p.maxOutputWidth  = kLDVSoCProfiles[i].maxOutputWidth;
                    p.maxOutputHeight = kLDVSoCProfiles[i].maxOutputHeight;
                    matched = 1;
                    break;
                }
            }
        }

        if (!matched) {
            /* 表里没有的机型：按核数给一个保守起点 */
            cores = LDVLogicalCPUCount();
            if (cores < 2) cores = 2;
            if (cores > 4) cores = 4;   /* 双核设备上超过 4 只会增加调度开销 */
            p.nThreads         = cores;
            p.maxFrameDelay    = cores;
            p.bufferBudgetBytes = 8u * 1024u * 1024u;
            p.maxOutputWidth   = 1280;
            p.maxOutputHeight  = 720;
        }

        /* 帧级并行的内存代价 = n_fc x 帧大小。1080p 一帧 2.97MB，
         * n_fc = 4 就是 12MB，在 8MB 缓冲档下等于把预算翻倍，
         * 所以 1080p 输出必须配 64MB 档。 */
        if (p.maxOutputHeight >= 1080 &&
            p.bufferBudgetBytes < 64u * 1024u * 1024u) {
            p.bufferBudgetBytes = 64u * 1024u * 1024u;
        }

        p.frameSizeLimit    = 3840u * 2160u;
        p.maxAheadFrames    = (p.nThreads >= 4) ? 8 : 3;
        p.picturePoolFrames = 8 + p.nThreads + p.maxAheadFrames + 2;
        p.gpuAccelerationEnabled = 1;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* 非 iOS 宿主（macOS）：仅用于逻辑验证                                */
    /* ------------------------------------------------------------------ */
    case LDV_DEVICE_CLASS_HOST: {
        int cores = LDVLogicalCPUCount();
        if (cores < 1) cores = 1;
        if (cores > 4) cores = 4;
        p.deviceClass      = LDV_DEVICE_CLASS_HOST;
        p.deviceClassLabel = "host (macOS) - logic verification only";
        p.socLabel         = "host";
        p.nThreads         = cores;
        p.maxFrameDelay    = cores;
        p.applyGrain       = 1;
        p.inloopFilters    = LDV_INLOOP_FILTER_ALL;
        p.frameSizeLimit   = 0;   /* 不限 */
        p.bufferBudgetBytes = 8u * 1024u * 1024u;
        p.maxAheadFrames    = 3;
        p.picturePoolFrames = 8 + cores + 3 + 2;
        p.maxOutputWidth  = 3840;
        p.maxOutputHeight = 2160;
        p.gpuAccelerationEnabled = 0;
        break;
    }
    }

    return p;
}

LDVTuningProfile LDVTuningProfileForCurrentDevice(void) {
    LDVTuningProfile p = LDVTuningProfileForClass(LDVDeviceClassDetect());

    /*
     * iPhone 3GS / iPod touch 3 专属覆盖。
     * 它们只有 256MB RAM（iPhone 4 是 512MB），屏幕也只有 480x320，
     * 所以把帧尺寸上限和输出上限进一步压低，避免 OOM 或
     * 让 GPU 去缩一个本来就用不到的分辨率。
     */
    if (LDVTuningIsA4Family() && LDVIs3GSFamily()) {
        p.socLabel        = "Samsung S5PC100 (iPhone 3GS)";
        p.frameSizeLimit  = 960u * 544u;
        p.maxOutputWidth  = 640;
        p.maxOutputHeight = 360;
        p.picturePoolFrames = 3;
    }

    return p;
}

const char *LDVTuningDescribe(const LDVTuningProfile *p, char *buf, size_t bufSize) {
    if (!p || !buf || bufSize == 0) return NULL;
    snprintf(buf, bufSize,
             "%s | soc=%s | n_threads=%d max_frame_delay=%d | grain=%d "
             "filters=0x%X size_limit=%u | budget=%luMB ahead=%d pool=%d | "
             "out<=%dx%d gpu=%d",
             p->deviceClassLabel, p->socLabel ? p->socLabel : "unknown",
             p->nThreads, p->maxFrameDelay,
             p->applyGrain, (unsigned)p->inloopFilters, p->frameSizeLimit,
             (unsigned long)(p->bufferBudgetBytes / (1024u * 1024u)),
             p->maxAheadFrames, p->picturePoolFrames,
             p->maxOutputWidth, p->maxOutputHeight,
             p->gpuAccelerationEnabled);
    return buf;
}
