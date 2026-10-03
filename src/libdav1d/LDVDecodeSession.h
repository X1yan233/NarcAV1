/*
 * LDVDecodeSession.h — 解码会话：后台解码线程 + 解码缓存
 *
 * 这是需求"暂停也要继续解码，直到触及缓存上限"的落点。
 *
 * 核心设计：**会话里没有"暂停"这个概念**
 * ------------------------------------
 * 会话只做一件事：尽可能快地把 MP4 解成帧、塞进缓存，塞满了就等（或丢）。
 * 它**不知道**显示端是否在暂停。
 *
 * "暂停"完全属于显示端：显示端暂停时不再从缓存取帧，于是缓存不释放、
 * 很快被填满，解码线程自然停在缓存上限上。等恢复播放时缓存又被消费掉，
 * 解码线程自动继续。
 *
 * 这样做的好处：
 *   - 只有一处状态（缓存水位），不会出现"暂停标志和解码线程不同步"
 *   - 暂停时的行为（解到缓存满）是免费得到的，不需要额外代码
 *   - 内存上限由缓存预算保证，与暂停时长无关
 *
 * 线程模型
 * --------
 *   解码线程（本会话创建）: 读 MP4 → 喂 dav1d → 取帧 → Push 进缓存
 *   显示线程（App 主线程）: Peek → 渲染 → Pop
 * 两者通过 LDVFrameCache 的互斥锁 + 条件变量同步。
 *
 * 缓存满时的两种策略
 * ------------------
 *   wait（默认）: 等显示端腾出空间。这是正常播放行为 ——
 *                 解码进度被显示进度约束，内存恒定。
 *   drop（压测）: 丢掉最旧的帧继续解。CPU 可以跑满，
 *                 内存依然被预算封顶。用于观察解码吞吐上限。
 */

#ifndef LDV_DECODE_SESSION_H
#define LDV_DECODE_SESSION_H

#include "LDVTypes.h"
#include "LDVDsp.h"
#include "LDVFrameCache.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVDecodeSession LDVDecodeSession;

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 打开 MP4 并准备解码会话（此时还没起线程）。
 *
 *   mp4Path      文件路径（建议传绝对路径；App 里从 bundle 取）
 *   cacheBytes   解码缓存字节预算（0 表示不按字节限制）
 *   cacheFrames  解码缓存帧数上限（<=0 表示不按帧数限制）
 *
 * 失败返回 NULL。
 */
LDVDecodeSession *LDVDecodeSessionCreate(const char *mp4Path,
                                         size_t cacheBytes,
                                         int cacheFrames);
/*
 * 带grain开关覆盖的创建入口（v8.15 决策链的落点）。
 *
 *   applyGrainOverride  <0 = 按设备 tuning 默认；
 *                        0/1 = 强制关/开 dav1d 的 apply_grain。
 *
 * 为什么存在：grain 合成走消费路径逐帧串行，不参与解码线程并行；
 * "视频会被缩小显示"时（判定在显示端 LDVPlayerViewController 的
 * decodeTargetForVideoSize:，判定只写一处），全分辨率上的 grain 是
 * 白算的 —— app 据此传 0 关掉，换回实打实的解码帧率。
 * 老的 LDVDecodeSessionCreate 等价于 override=-1。
 */
LDVDecodeSession *LDVDecodeSessionCreateWithGrainOverride(
        const char *mp4Path,
        size_t cacheBytes,
        int cacheFrames,
        int applyGrainOverride);

/*
 * 上一次 LDVDecodeSessionCreate 返回 NULL 的原因，NULL 表示"没有失败记录"。
 *
 * 分三类（打不开 MP4 / 缓存分配失败 / dav1d 初始化失败），因为这三类的
 * 排查方向完全不同 —— UI 只显示一句"打开失败"等于什么都没说。
 * 更细的原因（哪个 box 不对）由 LDVMP4Source 打到 stderr，前缀 "LDVMP4:"。
 *
 * 返回的字符串由库持有，调用方不要 free；下次 Create 会覆盖。
 */
const char *LDVDecodeSessionCreateError(void);

void LDVDecodeSessionDestroy(LDVDecodeSession **session);

/* ------------------------------------------------------------------------ */
/* DSP 实现切换（汇编 / 纯 C）                                               */
/* ------------------------------------------------------------------------ */

/*
 * 切换 DSP 实现，并**重建解码器 + 清空解码缓存**。
 *
 * ★ 为什么必须重建而不是改个标志：
 *   dav1d 的 DSP 函数表是在 dav1d_open 里一次性绑定的，掩码改晚了
 *   对已建好的解码器没有任何影响。所以切换 = 停线程 → 销毁解码器 →
 *   改偏好 → 重建 → 清缓存 → 重启线程。
 *
 * ★ 为什么必须清缓存：
 *   重建后的解码器从序列头重新开始（nextSample 归零），缓存里那些用
 *   旧 DSP 解出来的帧已经没有参考价值；而且新旧实现混在一个缓存里
 *   会让压测数据失去意义（对比的就是两者的差异）。
 *
 * 调用方可见的副作用：播放会从头重新开始（这是重建解码器的必然结果）。
 *
 * 返回：
 *   LDV_OK               切换完成（模式没变时也返回 OK，不做任何事）
 *   LDV_ERR_UNSUPPORTED  该架构没有汇编（mode == LDV_DSP_MODE_ASM 时）
 *   LDV_ERR_NOMEM        重建失败（此时解码器为空，会话不可用）
 */
LDVResult LDVDecodeSessionSetDspMode(LDVDecodeSession *session,
                                     LDVDspMode mode);

/*
 * 设置解码缓存的**目标尺寸**（可选）。必须在 Start 之前调用。
 *
 *   w/h <= 0，或等于视频原始尺寸  → 不缩放（默认）
 *   w/h 小于视频原始尺寸          → 每帧先缩小再进缓存
 *
 * 为什么要缩：缓存是按**字节**封顶的，而帧大小与像素数成正比。
 * 1080p 的素材在 960x640 的屏上没必要存 1080p 的帧 —— 缩到 960 宽
 * 之后每帧只剩 30% 的字节，同样预算能多装 3 倍的帧，抗抖动余量直接翻倍。
 *
 * 缩放用**展宽支撑域的双线性**（保持锐利、不混叠），详见 LDVDownscale.h。
 * 高位深（10/12bit）不支持缩放，会自动退回原尺寸。
 *
 * 返回 LDV_ERR_UNSUPPORTED 表示参数合法但当前帧不满足缩放条件
 * （例如视频尺寸还没解析出来）；LDV_ERR_INVALID_ARG 表示参数非法。
 * 两种情况都不影响正常播放 —— 只是不缩而已。
 */
LDVResult LDVDecodeSessionSetOutputSize(LDVDecodeSession *session,
                                        int w, int h);

/*
 * 运行中换解码输出尺寸（v8.18 动态分辨率）。
 *
 * 与 SetOutputSize（只能 Start 前调）的差别：
 *   - 停解码线程但**不清缓存**；
 *   - keepBelowWidth > 0 时，宽超过它的缓存帧被释放（"高清缓存"），
 *     其余保留 —— 换挡后缓存里会短暂存在两种分辨率的帧，
 *     显示端三条路径（GLES/Metal/CPU）本来就按帧自适应尺寸；
 *   - 从**当前取样位置**继续解码，进度/节拍序号不重置
 *     （Restart 是"从头重播"，这里是"无缝换挡"）。
 *
 *   keepBelowWidth <= 0 表示全部保留（换到更大/原生尺寸时旧帧必然更小）。
 *
 * 失败语义：缩放器建不起来 → 按原尺寸继续播放（返回 LDV_OK）；
 * 只有解码线程重启失败才返回 LDV_ERR_NOMEM。
 */
LDVResult LDVDecodeSessionRescaleOutput(LDVDecodeSession *session,
                                        int w, int h, int keepBelowWidth);

/*
 * 帧出口分流（v8.20 armv7 ES2 异步缩小管线的接入点）。
 *
 *   sink == NULL（默认）：解码帧按常规路径进缓存（可选 CPU 缩放）。
 *   sink != NULL：解码帧**不进缓存**，每帧复制一份后回调
 *                 sink(userdata, &view)，回调返回即视为消费完毕，
 *                 帧内存归还解码器。缓存的推进责任随之转移到 sink 的
 *                 实现方（例如 LDVES2Transcoder 把缩小的产物推进缓存）。
 *
 * 回调在**解码线程**内同步调用 —— 实现方必须复制数据后立刻返回，
 * 不得长时间阻塞（有界队列 + 满丢弃是推荐姿势）。
 *
 * 必须在 Start 之前调用；设置 sink 时会清掉已设的 CPU 缩放器
 * （两条输出管线互斥，一次只能走一条）。
 * 返回 LDV_ERR_INVALID_ARG：session 为空或线程已在跑。
 */
typedef void (*LDVFrameSinkFn)(void *userdata, const LDVFrameView *view);

LDVResult LDVDecodeSessionSetFrameSink(LDVDecodeSession *session,
                                       LDVFrameSinkFn sink, void *userdata);

/*
 * 显示缓存（只读借用）。缓存归 session 所有、随 session 销毁 ——
 * 借用方（如转换器）必须在 Destroy 之前停止对它的写入。
 * v8.20 GPU 异步转换模式用它做"转换产物推进 + 1.5x 宽度过滤"。
 */
LDVFrameCache *LDVDecodeSessionFrameCache(const LDVDecodeSession *session);

/* 当前的目标尺寸。未设置或未生效时返回 0。 */
int LDVDecodeSessionOutputWidth(const LDVDecodeSession *session);
int LDVDecodeSessionOutputHeight(const LDVDecodeSession *session);

/*
 * 缩放是否**真的生效了**（1/0）。SetOutputSize 成功不等于生效 ——
 * 高位深素材、或目标尺寸等于源尺寸，都会让它退回不缩。
 * UI 上显示的是这个，不是 SetOutputSize 的返回值。
 */
int LDVDecodeSessionScalingActive(const LDVDecodeSession *session);

/* 起解码线程。可重复调用（已在跑则直接返回 LDV_OK）。 */
LDVResult LDVDecodeSessionStart(LDVDecodeSession *session);

/*
 * 停解码线程并等它退出（join）。会清空缓存、释放全部帧内存。
 * 允许重复调用（幂等）。Destroy 内部会调用它。
 */
void LDVDecodeSessionStop(LDVDecodeSession *session);

/*
 * ★ 从头重启：停线程（顺带清空缓存）→ 解码进度归零 → 重新起线程。
 *
 * "视频播完后从头再播"的落点。为什么不用"清个缓存"就完事：
 *   播完时线程已经退出（eof 置位后 break），并且 nextSample 停在末尾 ——
 *   只清缓存的话线程不会重启、进度也不会归零。必须走一遍完整的
 *   停-复位-启动。与 SetDspMode 的差别：不重建解码器（DSP 表不变）。
 *
 * 返回 Start 的结果。幂等：线程没在跑也能正确重启。
 */
LDVResult LDVDecodeSessionRestart(LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 显示端                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * 借出缓存里最旧的一帧（= 下一个该显示的）。缓存空返回 0。
 * 借出的 view 在 LDVDecodeSessionPopFrame() 之前有效，期间不能重复 Peek。
 */
int  LDVDecodeSessionPeekFrame(LDVDecodeSession *session, LDVFrameView *out);

/*
 * 只看下一帧的时间戳（timescale 单位），**不借出**。
 * 显示端用它来判断"这一帧现在该不该显示"，避免过早借出。
 * 缓存空返回 0。
 */
int  LDVDecodeSessionNextFrameTimestamp(LDVDecodeSession *session,
                                        int64_t *outTimestamp);

/* 媒体时间基。把 PTS 换算成秒用：sec = pts / timescale。 */
uint32_t LDVDecodeSessionTimescale(const LDVDecodeSession *session);

/*
 * 只看下一帧的**解码输出序号**（从 0 开始），不借出。缓存空返回 0。
 *
 * ★ v8.25 起显示节拍改为"PTS 底座 + 同 PTS 组内细分"（见
 *   LDVDecodeSessionTimestampSeconds 与显示端注释）：真 VFR 素材
 *   每帧自带独立 PTS，按 PTS 走；按字节切块的 MP4 一个样本含多帧、
 *   共享同一 PTS，组内按 1/fps 细分后退化为原来的序号节拍。
 *   序号/帧率仍是 PTS 未知（INT64_MIN）时的兜底。
 */
int LDVDecodeSessionNextFrameIndex(LDVDecodeSession *session,
                                   int64_t *outIndex);

/*
 * 容器时间戳（视频轨 timescale tick，即 view.timestamp 的单位）
 * → 容器时间轴上的秒。时间轴原点与 LDVDecodeSessionFirstPtsSeconds
 * 一致（先减轨首 PTS 再换算），结果可直接与音频媒体时间比较。
 * ts == INT64_MIN（未知）返回 0 —— 调用方应先判 INT64_MIN。
 */
double LDVDecodeSessionTimestampSeconds(const LDVDecodeSession *session,
                                        int64_t ts);

/* 释放当前借出的帧（内存归还）。没有借出时是空操作。 */
void LDVDecodeSessionPopFrame(LDVDecodeSession *session);

/*
 * 丢弃缓存里最旧的一帧（用于"快进/跳帧"）。
 * 当前帧被借出时不会丢它。返回丢弃的帧数（0/1）。
 */
int  LDVDecodeSessionDropOldestFrame(LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 缓存状态（进度条用）                                                      */
/* ------------------------------------------------------------------------ */

float  LDVDecodeSessionCacheProgress(const LDVDecodeSession *session);
size_t LDVDecodeSessionCacheBytes(const LDVDecodeSession *session);
size_t LDVDecodeSessionCacheBudget(const LDVDecodeSession *session);
int    LDVDecodeSessionCacheCount(const LDVDecodeSession *session);
int    LDVDecodeSessionCacheFull(const LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 策略开关                                                                  */
/* ------------------------------------------------------------------------ */

/* 缓存满时是否丢帧继续解（1 = 压测模式，0 = 等待）。默认 0。 */
void LDVDecodeSessionSetDropWhenFull(LDVDecodeSession *session, int drop);
int  LDVDecodeSessionDropWhenFull(const LDVDecodeSession *session);

/* EOF 后是否从头重来（1 = 循环，压测用）。默认 0。 */
void LDVDecodeSessionSetLooping(LDVDecodeSession *session, int loop);
int  LDVDecodeSessionLooping(const LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 统计与状态                                                                */
/* ------------------------------------------------------------------------ */

int64_t LDVDecodeSessionDecodedFrames(const LDVDecodeSession *session);
int64_t LDVDecodeSessionFedSamples(const LDVDecodeSession *session);
int64_t LDVDecodeSessionTotalFrames(const LDVDecodeSession *session);
int64_t LDVDecodeSessionDroppedFrames(const LDVDecodeSession *session);

/*
 * 缓存「拒收」次数（PushPicture 因满而拒绝的次数）。
 *
 * 正常播放时几乎不涨。持续快速上涨说明生产者在空转 ——
 * 它在不停地解码、但帧全被拒收丢掉，而缓存却在下降。
 * 这是「两处满的判定不一致」这类故障的专属指标。
 */
int64_t LDVDecodeSessionCacheFullHits(const LDVDecodeSession *session);
int64_t LDVDecodeSessionErrorCount(const LDVDecodeSession *session);

/*
 * 解码线程自己的吞吐（帧/秒），用 mach/clock 在**解码线程内**统计。
 *
 * 注意：本库不是实时解码器，这个数字只用于观察压测时的解码能力上限，
 * 不对外承诺、也不参与任何播放决策（需求明确要求不显示帧率）。
 * UI 里只在"压测模式"下作为诊断信息展示。
 */
double LDVDecodeSessionDecodeFPS(const LDVDecodeSession *session);

int LDVDecodeSessionIsRunning(const LDVDecodeSession *session);
int LDVDecodeSessionIsEOF(const LDVDecodeSession *session);
int LDVDecodeSessionLoopCount(const LDVDecodeSession *session);

const char *LDVDecodeSessionLastError(const LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 轨道信息（UI 用）                                                         */
/* ------------------------------------------------------------------------ */

int    LDVDecodeSessionVideoWidth(const LDVDecodeSession *session);
int    LDVDecodeSessionVideoHeight(const LDVDecodeSession *session);
double LDVDecodeSessionVideoFPS(const LDVDecodeSession *session);

/*
 * 视频轨首样本的 PTS（秒）。
 *
 * 音画同步要用：显示节拍算出来的是"第 idx 帧距轨首多少秒"，
 * 加上这个起点才是**容器时间轴上的媒体时间**，才能和
 * LDVAudioSessionMediaTime() 直接比较。少了它，起点不为 0 的文件
 * 会整体偏一个固定量（而且这个量看起来"不大"，容易被忽略）。
 */
double LDVDecodeSessionFirstPtsSeconds(const LDVDecodeSession *session);

/* ------------------------------------------------------------------------ */
/* 进程内存占用（用于观察内存释放行为）                                      */
/* ------------------------------------------------------------------------ */

/* 当前常驻内存（RSS）字节数。取不到返回 0。 */
size_t LDVDecodeSessionResidentMemory(void);
/* 当前虚拟内存（VSZ）字节数。取不到返回 0。 */
size_t LDVDecodeSessionVirtualMemory(void);

#ifdef __cplusplus
}
#endif

#endif /* LDV_DECODE_SESSION_H */
