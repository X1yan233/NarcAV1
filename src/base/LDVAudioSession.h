/*
 * LDVAudioSession.h — 音频会话：后台解码线程 + AudioQueue 输出 + 主时钟
 *
 * 与 LDVDecodeSession（视频）的分工
 * --------------------------------
 *   视频会话：解出来的帧进**大缓存**（几十帧 / 几十 MB），显示端按节拍取用。
 *             暂停时缓存能继续填到上限 —— 这是需求里那条暂停语义。
 *   音频会话：解出来的 PCM 直接进 AudioQueue，**只有约 93 ms 的队列深度**
 *             （4 块 × 1024 帧 @44.1kHz）。音频是实时流，缓冲再多也没意义 ——
 *             多了只会让"暂停/跳转"的响应变迟钝，而且声音一旦入队就改不了。
 *
 *   ★ 所以两个会话的暂停行为**天然不对称**，这是对的：
 *     暂停时视频解码会继续跑（填满帧缓存），音频解码会停在队列深度上。
 *
 * 主时钟：为什么是音频
 * --------------------
 *   音画同步只能选一边当基准。这里选**音频**，理由：
 *     - 音频不能随便重采样/变速，改它的时间基会直接听出变调
 *     - 视频丢一帧或多显示一帧，人眼几乎察觉不到
 *     - 这是所有播放器的通行做法（QuickTime / ffplay / ExoPlayer 都是音频主）
 *   于是：视频渲染端读音频时钟，落后就丢帧、超前就等。
 *
 *   时钟值 = 音轨首样本 PTS(秒) + 已播放采样数 / 输出采样率
 *   两项都换算成**秒**，因为音视频两条轨的 timescale 不同
 *   （实测视频 15360、音频 44100），只有秒是共同语言。
 *
 * 容错优先级（需求第 4 条）
 * ------------------------
 *   - 单个 AAC 帧解码失败 → 跳过它，补一段等长静音，继续下一个
 *   - 样本读失败          → 同上
 *   - 输出队列满          → 等一小段再试，**绝不阻塞视频线程**
 *   - 音频会话创建失败    → 整个 app 退回纯视频 + 墙钟节拍，不报错崩溃
 *   音频线程与视频线程**没有共享锁**：视频只读一个时钟值，
 *   读的是 AudioQueue 内部的原子状态，不会被音频卡住。
 */

#ifndef LDV_AUDIO_SESSION_H
#define LDV_AUDIO_SESSION_H

#include "LDVTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVAudioSession LDVAudioSession;

/*
 * 打开 MP4 的音频轨并准备会话（此时还没起线程、还没出声）。
 *
 *   mp4Path          文件路径（和视频会话可以传同一个文件）
 *   bufferCount      AudioQueue 缓冲块数（<2 取默认 4）
 *   framesPerBuffer  每块帧数（<=0 取默认 1024）
 *
 * 返回 NULL 的常见原因（都会向 stderr 说明）：
 *   - 容器里没有音轨
 *   - 音频编码不是系统支持的（本版只接 AAC / mp4a）
 *   - AudioQueue 建不起来（模拟器上偶发，此时应当退回纯视频）
 */
LDVAudioSession *LDVAudioSessionCreate(const char *mp4Path,
                                       int bufferCount,
                                       int framesPerBuffer);

/*
 * 上一次 LDVAudioSessionCreate 返回 NULL 的原因，NULL 表示没有失败记录。
 *
 * 分六类（打不开 MP4 / 没有音轨 / 编码不支持 / 没有 AudioSpecificConfig /
 * 解码器建不起来 / AudioQueue 建不起来）—— 排查方向完全不同，
 * UI 上只显示"音频 无"等于什么都没说。
 * 更细的原因由本模块打到 stderr，前缀 "LDVAudio:"。
 *
 * 返回的字符串由库持有，调用方不要 free；下次 Create 会覆盖。
 */
const char *LDVAudioSessionCreateError(void);

void LDVAudioSessionDestroy(LDVAudioSession **session);

/* 起播（起解码线程 + 启动输出）。可重复调用。 */
LDVResult LDVAudioSessionStart(LDVAudioSession *session);

/* 暂停：输出暂停，时钟停住，已入队的缓冲保留。解码线程会停在队列深度上。 */
void LDVAudioSessionPause(LDVAudioSession *session);

/* 恢复：从暂停位置继续。 */
LDVResult LDVAudioSessionResume(LDVAudioSession *session);

/*
 * 停播：停线程（join）+ 停输出 + 清队列。幂等。
 * Destroy 内部会调用它。
 */
void LDVAudioSessionStop(LDVAudioSession *session);

/* ------------------------------------------------------------------------ */
/* 主时钟                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * ★ 当前媒体时间（秒）。**这就是音画同步的主时钟。**
 *
 *   = 音轨首样本 PTS(秒) + 已播放采样数 / 输出采样率
 *
 * 用"已播放"而不是"已入队"：入队量领先真实播放约一个队列深度
 * （默认 ~93 ms），拿它当基准会让画面稳定领先声音 0.1 秒 ——
 * 这个量级人耳一听就知道，但看代码看不出来。
 *
 * 音频未起播时返回音轨首样本 PTS（即媒体时间起点）。
 * Stop 之后时钟归零（下次起播从头算）。
 */
double LDVAudioSessionMediaTime(LDVAudioSession *session);

/* 音轨首样本的 PTS（秒）。用于和视频轨对齐时间轴。 */
double LDVAudioSessionFirstPtsSeconds(const LDVAudioSession *session);

/* 输出采样率（解码器回读的实际值；HE-AAC 可能与源不同）。 */
int LDVAudioSessionOutputSampleRate(const LDVAudioSession *session);
int LDVAudioSessionOutputChannels(const LDVAudioSession *session);

/* 源编码信息 */
const char *LDVAudioSessionSourceCodec(const LDVAudioSession *session);
int         LDVAudioSessionSourceSampleRate(const LDVAudioSession *session);
int         LDVAudioSessionSourceChannels(const LDVAudioSession *session);
int64_t     LDVAudioSessionTotalPackets(const LDVAudioSession *session);

/* ------------------------------------------------------------------------ */
/* 状态与诊断                                                                */
/* ------------------------------------------------------------------------ */

int LDVAudioSessionIsRunning(const LDVAudioSession *session);
int LDVAudioSessionIsEOF(const LDVAudioSession *session);
int LDVAudioSessionIsPaused(const LDVAudioSession *session);
int LDVAudioSessionLoopCount(const LDVAudioSession *session);

int64_t LDVAudioSessionDecodedPackets(const LDVAudioSession *session);
int64_t LDVAudioSessionDecodedFrames(const LDVAudioSession *session);
int64_t LDVAudioSessionDroppedPackets(const LDVAudioSession *session);
int64_t LDVAudioSessionUnderruns(const LDVAudioSession *session);
int64_t LDVAudioSessionQueuedFrames(const LDVAudioSession *session);
int     LDVAudioSessionFreeBuffers(const LDVAudioSession *session);
int64_t LDVAudioSessionErrorCount(const LDVAudioSession *session);
const char *LDVAudioSessionLastError(const LDVAudioSession *session);

/* EOF 后是否从头重来（压测用）。默认 0。 */
void LDVAudioSessionSetLooping(LDVAudioSession *session, int loop);

/*
 * ★ 把音频位置重同步到指定媒体时间（秒）—— 一个真正的"seek"。
 *
 * 为什么需要它：音频解码是线性的（nextSample 只进不退），暂停/压测
 * 期间视频端缓存里的帧会被压测消耗掉（队首前进了几千帧），恢复播放
 * 时视频队首的媒体时间远在音频时钟前面 —— 音频追不上，视频就得一路
 * 等下去。唯一的出路是把音频也跳到同一时刻。
 *
 * 实现：停线程 + 停输出（清队列、时钟归零）→ 从头扫描样本表找第一个
 * PTS >= 目标 的样本 → 设为新的起点。之后调用方用 LDVAudioSessionStart
 * 重启，会从**这个位置**开始解码（Start 里的状态复位被 pendingSync
 * 标志挡掉，不会把同步位置洗回 0）。
 *
 * 返回 LDV_ERR_UNSUPPORTED 表示没有音轨。mediaSeconds 超过末尾时
 * 落回从头（返回 LDV_OK），调用方不必特判。
 */
LDVResult LDVAudioSessionSyncToMediaTime(LDVAudioSession *session,
                                         double mediaSeconds);

/*
 * 输出队列是否已排空（全部缓冲都空闲）。
 * "音频真正放完"的判据 —— 播放结束提示要等它，而不是只看 EOF 标志。
 */
int LDVAudioSessionOutputIdle(const LDVAudioSession *session);

#ifdef __cplusplus
}
#endif

#endif /* LDV_AUDIO_SESSION_H */
