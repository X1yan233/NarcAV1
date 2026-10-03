/*
 * LDVAudioOutput.h — 音频输出（AudioToolbox AudioQueue 封装）
 *
 * 为什么用 AudioQueue
 * ------------------
 *   可用性：iOS 2.0+。iPhone 4S / iOS 5.1.1、以及 iOS 4 设备都支持。
 *   依赖：AudioToolbox.framework —— **系统框架，不是外部依赖**。
 *   对比：AudioUnit(RemoteIO) 延迟更低，但要多写一百多行
 *         AudioUnit 生命周期与 AudioSession 类别管理，而本库**不是
 *         实时解码器**（需求原话：按在线视频缓冲的方式设计），
 *         AudioQueue 自带的缓冲队列正好对上这个模型。
 *
 * 缓冲模型
 * --------
 *   固定 N 块（默认 4）环形缓冲，每块 framesPerBuffer 帧。
 *   解码线程 Write() 一块、AudioQueue 播完一块回调归还一块。
 *   生产者等空闲块用 pthread 条件变量 + 超时 —— 超时是为了让
 *   "停止"能立刻生效，不会卡在等缓冲上。
 *
 * 时钟（这是音画同步的基准）
 * --------------------------
 *   ClockSeconds() = 已播放帧数 / 采样率，用 AudioQueueGetCurrentTime()
 *   的 mSampleTime 取"真正播出去的采样数"，而不是"入队了多少"。
 *
 *   ★ 为什么不用入队量：入队量领先真实播放最多 N 块缓冲
 *     （4 块 × 1024 帧 ≈ 93 ms @44.1kHz）。拿它当主时钟会让画面
 *     稳定地领先声音约 0.1 秒 —— 这种偏移量人耳一听就出来，
 *     但看代码很难发现（数字都是"对"的，只是量的不是同一个东西）。
 *
 * 容错
 * ----
 *   写不进去（超时）返回 LDV_ERR_NEED_INPUT，由调用方决定是重试、
 *   丢帧还是补静音。本模块**不会**阻塞式死等，也不会因为欠载而
 *   抛错 —— 需求要求允许轻微卡顿，但绝不能拖住视频渲染。
 */

#ifndef LDV_AUDIO_OUTPUT_H
#define LDV_AUDIO_OUTPUT_H

#include "LDVTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVAudioOutput LDVAudioOutput;

/*
 * 创建输出。
 *   sampleRate        PCM 采样率（用解码器回读的那个）
 *   channels          声道数
 *   bufferCount       缓冲块数（<2 时取默认 4）
 *   framesPerBuffer   每块帧数（<=0 时按 23ms 估算，约 1024 @44.1kHz）
 * 失败返回 NULL。
 */
LDVAudioOutput *LDVAudioOutputCreate(uint32_t sampleRate, int channels,
                                     int bufferCount, int framesPerBuffer);

void LDVAudioOutputDestroy(LDVAudioOutput **out);

/* 起播。可重复调用。 */
LDVResult LDVAudioOutputStart(LDVAudioOutput *o);

/*
 * 暂停 / 恢复。
 * 暂停时已入队的缓冲**保留**，时钟停住；恢复后从原位置继续。
 * 这正是播放器"暂停"该有的行为。
 */
void LDVAudioOutputPause(LDVAudioOutput *o);
LDVResult LDVAudioOutputResume(LDVAudioOutput *o);

/* 停播并清空队列。允许重复调用。 */
void LDVAudioOutputStop(LDVAudioOutput *o);

/*
 * 写入一段 PCM（交错，16 位）。
 *
 *   bytes 必须是"每帧字节数"的整数倍（多余部分会被截掉）。
 *   内部最多等 timeoutMs 毫秒拿空闲缓冲。
 *
 * 返回：
 *   LDV_OK              已入队
 *   LDV_ERR_NEED_INPUT  超时（缓冲全满，调用方应稍后重试或丢帧）
 *   LDV_ERR_EOF         已停止
 *   LDV_ERR_INVALID_ARG 参数问题
 */
LDVResult LDVAudioOutputWrite(LDVAudioOutput *o, const void *pcm,
                              size_t bytes, int timeoutMs);

/* 已播放的帧数（暂停期间不增长） */
int64_t LDVAudioOutputFramesPlayed(LDVAudioOutput *o);

/* 主时钟：秒。已播放帧数 / 采样率。 */
double LDVAudioOutputClockSeconds(LDVAudioOutput *o);

/* 当前空闲缓冲块数（诊断用） */
int LDVAudioOutputFreeBuffers(const LDVAudioOutput *o);

/* 诊断 */
uint32_t LDVAudioOutputSampleRate(const LDVAudioOutput *o);
int      LDVAudioOutputChannels(const LDVAudioOutput *o);
int64_t  LDVAudioOutputEnqueuedFrames(const LDVAudioOutput *o);
int64_t  LDVAudioOutputUnderruns(const LDVAudioOutput *o);
int      LDVAudioOutputIsRunning(const LDVAudioOutput *o);

/*
 * 输出队列是否已排空（全部缓冲都空闲）。
 * "音频真的放完了"的判据：线程退出（EOF）只说明解完了，
 * 队列里还压着最后一段（默认 4 块 ≈ 93 ms）；播放结束提示
 * 要等这一段也走完，否则提示框弹出时声音还剩一小截。
 */
int LDVAudioOutputIsIdle(const LDVAudioOutput *o);

#ifdef __cplusplus
}
#endif

#endif /* LDV_AUDIO_OUTPUT_H */
