/*
 * LDVCpuUsage.c — 全核 CPU 占用采样（设计说明见 LDVCpuUsage.h）
 *
 * 实现注意（每个都踩过别人的坑，别再踩）：
 *
 * 1) host_processor_info 返回的内存是**内核分配的**（MIG out-of-line），
 *    必须用 vm_deallocate 归还 —— 漏了就是每次采样漏
 *    核数×CPU_STATE_MAX×4 字节，4Hz 采样一天也不少。
 *
 * 2) mach_host_self() 每次调用返回一个 +1 的新引用，
 *    用完要 mach_port_deallocate。不复用端口是为了无状态、无清理路径
 *    （端口的获取成本远低于每 0.25s 一次的可接受开销）。
 *
 * 3) tick 差分用 uint64：natural_t 是 32 位，但整机累计 tick 在
 *    100Hz tick 率下要 5 年才溢出，两次采样间隔只有 0.25s，
 *    即使发生回绕（设备休眠修正？）差分会得到一个巨大的无符号数 ——
 *    占用率会被钳到 100%，错一帧，可接受，不做更复杂的回绕检测。
 */

#include "LDVCpuUsage.h"

#include <stddef.h>   /* size_t（vm_size_t 相关推断的显式兜底） */

/*
 * ★ 头文件显式写全，不赌 <mach/mach.h> 的间接包含链
 * （本项目的教训：间接包含随 SDK 与 deployment target 漂移）。
 */
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <mach/vm_map.h>

void LDVCpuUsageTrackerInit(LDVCpuUsageTracker *t) {
    if (!t) return;
    t->havePrev = 0;
    t->prevBusy = 0;
    t->prevTotal = 0;
}

double LDVCpuUsageTrackerSample(LDVCpuUsageTracker *t) {
    processor_info_array_t  info = NULL;
    mach_msg_type_number_t  infoCount = 0;
    natural_t               numCpu = 0;
    kern_return_t           kr;
    uint64_t                busy = 0;
    uint64_t                total = 0;
    int                     i;

    if (!t) return -1.0;

    kr = host_processor_info(mach_host_self(),
                             PROCESSOR_CPU_LOAD_INFO,
                             &numCpu, &info, &infoCount);
    if (kr != KERN_SUCCESS || info == NULL) return -1.0;

    for (i = 0; i < (int)numCpu; i++) {
        const integer_t *c = &info[(size_t)i * CPU_STATE_MAX];
        busy  += (uint64_t)(c[CPU_STATE_USER] + c[CPU_STATE_SYSTEM] +
                            c[CPU_STATE_NICE]);
        total += (uint64_t)(c[CPU_STATE_USER] + c[CPU_STATE_SYSTEM] +
                            c[CPU_STATE_NICE] + c[CPU_STATE_IDLE]);
    }

    /* 归还内核内存 + 主机端口引用（见文件头 1) 2)） */
    vm_deallocate(mach_task_self(), (vm_address_t)info,
                  (vm_size_t)infoCount * sizeof(integer_t));
    mach_port_deallocate(mach_task_self(), mach_host_self());

    if (total == 0) return -1.0;

    if (!t->havePrev || t->prevTotal == 0) {
        t->havePrev = 1;
        t->prevBusy = busy;
        t->prevTotal = total;
        return -1.0;          /* 首次采样：只有基准，还没有差分 */
    }

    {
        const uint64_t dBusy  = busy  - t->prevBusy;
        const uint64_t dTotal = total - t->prevTotal;
        double pct;

        t->prevBusy = busy;
        t->prevTotal = total;

        if (dTotal == 0) return -1.0;   /* tick 无推进（罕见） */

        pct = (double)dBusy / (double)dTotal * 100.0;
        if (pct < 0.0)   pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        return pct;
    }
}
