/*
 * LDVCpuUsage.h — 全核 CPU 占用采样器（解码框架通用诊断模块）
 *
 * 定位
 * ----
 * 给播放器的状态栏提供"CPU 占用 %"读数。采样原理：
 *   host_processor_info(PROCESSOR_CPU_LOAD_INFO) 读每个核的 Mach tick
 *   计数（USER/SYSTEM/IDLE/NICE），两次采样做差：
 *     占用% = Δ(user+system+nice) / Δ(全部) × 100
 *
 * 为什么是它而不是别的
 * --------------------
 * - iOS 没有 /proc/stat，sysctl kern.cp_time 也不暴露；
 *   Mach 主机端口 API 是唯一公开途径，iOS 2.0 起全系可用，无版本坑。
 * - 开销：一次 mach trap + 一次小块内核内存拷贝（核数 × 4 个 integer_t），
 *   在 0.25~0.5s 的 GUI 刷新间隔下完全可以忽略 —— 满足
 *   "GUI 数据层不拖累性能"的要求（压测时的 98-125fps 不能被采样拉低）。
 *
 * 设计取舍：值类型（结构体 + 两个函数），不堆分配、不销毁，
 * 调用方直接放 ivar。只允许一个线程调用（播放器里是主线程），
 * 所以没有锁 —— 锁的开销比采样本身还贵。
 */

#ifndef LDV_CPU_USAGE_H
#define LDV_CPU_USAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 采样状态。两次采样之间的差分基准。
 * ObjC ivar 天然零填充（havePrev=0），不调 Init 也能工作；
 * 显式 Init 只是为了语义清楚。
 */
typedef struct LDVCpuUsageTracker {
    int      havePrev;    /* 首次采样前为 0 */
    uint64_t prevBusy;    /* user+system+nice 累计 tick */
    uint64_t prevTotal;   /* 四类状态合计 tick */
} LDVCpuUsageTracker;

/* 归零。传 NULL 安全。 */
void LDVCpuUsageTrackerInit(LDVCpuUsageTracker *t);

/*
 * 采样并返回自上次调用以来的全核占用百分比（0~100，各核平均）。
 *
 * 返回 -1.0 的两种情况：
 *   - 首次调用（还没有差分基准）
 *   - 内核读取失败 / 两次采样间 tick 无推进（极端罕见）
 * 调用方对 -1 直接跳过显示即可，不要当成 0。
 */
double LDVCpuUsageTrackerSample(LDVCpuUsageTracker *t);

#ifdef __cplusplus
}
#endif

#endif /* LDV_CPU_USAGE_H */
