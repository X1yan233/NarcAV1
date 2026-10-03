//
//  libdav1d.m
//  libdav1d
//
//  Created by xiyan233 on 26-9-24.
//  Copyright (c) 2026年 Narcira. All rights reserved.
//

#import "libdav1d.h"

#include <stdio.h>
#include "dav1d/dav1d.h"

/* 非 clang 编译器没有 __has_feature，先兜底再使用 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/*
 * ★ 版本号是“Mac 侧源码是否同步到位”的可见信号：
 *   启动日志里这串数字和 Windows 侧对不上，就说明同步漏了文件
 *   （v8.10 实测：旧音频头没同步时编译只有 implicit declaration 警告、
 *   链接照样成功 —— 静默漂移，只有版本号能暴露它）。
 */
const char *const kLibdav1dVersionString = "0.1.9-tier-scale";

/* 编译期架构名。用来确认 armv7 / armv7s / arm64 三个切片各自编对了。 */
static const char *Libdav1dArchName(void) {
#if defined(__arm64__) || defined(__aarch64__)
    return "arm64";
#elif defined(__ARM_ARCH_7S__)
    return "armv7s";
#elif defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_7__)
    return "armv7";
#elif defined(__i386__)
    return "i386";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

/* 编译期部署目标，形如 "4.0" / "7.0"。 */
static NSString *Libdav1dDeploymentTarget(void) {
#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED)
    int v = __IPHONE_OS_VERSION_MIN_REQUIRED;
    return [NSString stringWithFormat:@"iOS %d.%d",
            (v / 10000) % 100, (v / 100) % 100];
#elif defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
    return @"macOS";
#else
    return @"unknown";
#endif
}

NSString *Libdav1dVersionString(void) {
    return [NSString stringWithFormat:@"libdav1d %s (dav1d %s, API %u.%u.%u)",
            kLibdav1dVersionString,
            dav1d_version(),
            DAV1D_API_MAJOR(dav1d_version_api()),
            DAV1D_API_MINOR(dav1d_version_api()),
            DAV1D_API_PATCH(dav1d_version_api())];
}

NSString *Libdav1dBuildInfo(void) {
    LDVTuningProfile tuning = LDVTuningProfileForCurrentDevice();
    char             desc[512];
    const char      *cStandard;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
    cStandard = "C99";
#else
    cStandard = "pre-C99";
#endif

    LDVTuningDescribe(&tuning, desc, sizeof(desc));

    /* iOS 4 兼容：不用 NSDictionary 下标语法 */
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"arch=%s, min=%@, C=%s, ARC=%s\n",
        Libdav1dArchName(),
        Libdav1dDeploymentTarget(),
        cStandard,
#if __has_feature(objc_arc)
        "on"
#else
        "off"
#endif
    ];
    [s appendFormat:@"tuning: %s", desc];
    return s;
}
