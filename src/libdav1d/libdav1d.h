//
//  libdav1d.h
//  libdav1d
//
//  Created by xiyan233 on 26-9-24.
//  Copyright (c) 2026年 Narcira. All rights reserved.
//

/*
 * libdav1d — 基于 dav1d 的 AV1 解码静态库（总头文件）
 *
 * 目标平台：iOS 4.0+（armv7 / armv7s）/ iOS 7.0+（arm64）
 * 语言约束：主体 Objective-C（ARC），C 部分严格 C99
 * 底层：dav1d-master（DAV1D_API_VERSION 7.0.0）
 *
 * 使用方式：
 *     #import <libdav1d/libdav1d.h>
 *
 * 最小用法（细节见 Demo/main.m）：
 *     LDVDecoder *d = [LDVDecoder decoderForCurrentDevice];
 *     [d openWithError:&err];
 *     [d feedPacket:obuBytes length:obuSize timestamp:pts error:&err];
 *     while ([d pullFrame:&view error:&err] == LDV_OK) {
 *         ... 使用 view.plane[] ...
 *         [d releaseFrame:&view];
 *     }
 *     [d close];
 *
 * 设计文档：docs/ARCHITECTURE.md
 */

#import <Foundation/Foundation.h>

/* 版本信息 */
#import "libdav1dVersion.h"

/* C 层基础类型（纯 C99，也可被 C 编译单元单独包含） */
#include "LDVTypes.h"

/* 设备调参档：A4 / 单核 / 多核（A5–A9X）/ 宿主 */
#include "LDVPlatformTuning.h"

/* 最小公开 API（Objective-C / ARC） */
#import "LDVDecoder.h"
