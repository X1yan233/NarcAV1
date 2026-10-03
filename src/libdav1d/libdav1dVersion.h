//
//  libdav1dVersion.h
//  libdav1d
//

#ifndef LIBDAV1D_VERSION_H
#define LIBDAV1D_VERSION_H

#import <Foundation/Foundation.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本库版本号 */
extern const char *const kLibdav1dVersionString;

/* 形如 "libdav1d 0.1.0-demo (dav1d 1.5.0, API 7.0.0)" */
NSString *Libdav1dVersionString(void);

/*
 * 构建信息：目标架构、部署目标、编译期 C 标准、
 * 以及当前设备的调参档一行摘要。
 * 用于在真机上快速确认"单核优化是否按预期生效"。
 */
NSString *Libdav1dBuildInfo(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBDAV1D_VERSION_H */
