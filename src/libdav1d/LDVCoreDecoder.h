/*
 * LDVCoreDecoder.h — dav1d 的 C99 封装（最小可用子集）
 *
 * 这一层是唯一接触 dav1d API 的地方。上层（ObjC 门面、GPU 层、输出层）
 * 只通过 LDVFrameView 拿到解码结果，永远不直接持有 Dav1dPicture /
 * Dav1dData / Dav1dContext 指针。
 *
 * 与 dav1d 交互的三条硬性约定（见 docs/ARCHITECTURE.md §4.5）：
 *   a) 送数据必须用 dav1d_data_create / dav1d_data_wrap，绝不能手工填
 *      Dav1dData —— dav1d_data_ref 在 src->ref == NULL 时不做引用计数
 *      （dav1d-master/src/data.c:87-98），手工填会 use-after-free。
 *      本 demo 用 dav1d_data_create（复制一份），后续换成 dav1d_data_wrap
 *      即可实现零拷贝。
 *   b) send_data 返回 EAGAIN 时必须先 get_picture 取干净再送。
 *   c) get_picture 成功后调用方拥有引用，必须 dav1d_picture_unref，
 *      一条路径都不能漏。
 */

#ifndef LDV_CORE_DECODER_H
#define LDV_CORE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "LDVTypes.h"
#include "LDVPlatformTuning.h"
/* 缓存模式的 API 需要 LDVFrameCache 这个类型。注意这里用 include 而不是
 * 前向 typedef：C99 不允许重复 typedef 同一个名字（C11 才放开），
 * 而 LDVFrameCache.h 里已经 typedef 过一次了。 */
#include "LDVFrameCache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVCoreDecoder LDVCoreDecoder;

/* ------------------------------------------------------------------------ */
/* 序列头探测                                                                */
/* ------------------------------------------------------------------------ */

/*
 * 在打开解码器之前探测序列头，用于决定起播档位、是否可走 SVC 选层。
 *
 * 返回：
 *   LDV_OK            探测成功，输出参数已填充
 *   LDV_ERR_NEED_INPUT 缓冲区里没有序列头 OBU（需要更多数据）
 *   LDV_ERR_BITSTREAM  数据非法
 *
 * 任何 out 参数都可以传 NULL。
 */
int LDVCoreProbeSequenceHeader(const uint8_t *buf, size_t size,
                               int *outWidth, int *outHeight,
                               int *outBpc, int *outLayout,
                               int *outColorRange,
                               int *outOperatingPointCount);

/* ------------------------------------------------------------------------ */
/* 解码器生命周期                                                            */
/* ------------------------------------------------------------------------ */

/*
 * 用给定调参档打开解码器。tuning 传 NULL 时按当前设备自动选择。
 * 失败返回 NULL（内存不足或 dav1d_open 失败）。
 */
LDVCoreDecoder *LDVCoreDecoderCreate(const LDVTuningProfile *tuning);

/* 关闭并释放。允许传入 NULL。返回后 *dec 被置为 NULL。 */
void LDVCoreDecoderDestroy(LDVCoreDecoder **dec);

/* ------------------------------------------------------------------------ */
/* 送数据 / 取帧                                                             */
/* ------------------------------------------------------------------------ */

/*
 * 送入一个 AV1 数据包（一个或多个 OBU）。
 * 内部会复制数据，所以函数返回后调用方可以立即复用/释放缓冲区。
 *
 * 返回：
 *   LDV_OK             已消费
 *   LDV_ERR_NEED_INPUT 解码器内部还有数据未消化，必须先调用
 *                      LDVCoreDecoderPullFrame() 取帧，然后再送
 *   LDV_ERR_BITSTREAM  码流损坏
 *   LDV_ERR_INVALID_ARG / LDV_ERR_NOMEM
 */
int LDVCoreDecoderFeed(LDVCoreDecoder *dec,
                       const uint8_t *bytes, size_t size,
                       int64_t timestamp);

/*
 * 拉取一帧。
 *
 * 返回：
 *   LDV_OK             成功，*view 已填充
 *   LDV_ERR_NEED_INPUT 暂无可输出帧，需要更多输入
 *   LDV_ERR_BITSTREAM  解码出错
 *   LDV_ERR_INVALID_ARG 上一帧还没调用 ReleaseFrame
 *
 * 注意：喂完全部数据之后，反复调用本函数直到返回 LDV_ERR_NEED_INPUT
 * 即可取出所有内部延迟帧（dav1d 的 dav1d_get_picture 每次调用都会置
 * c->drain = 1，所以 drain 是自动的，不需要额外 API）。
 *
 * 取到帧之后**必须**调用 LDVCoreDecoderReleaseFrame() 才能再次 Pull。
 */
int LDVCoreDecoderPullFrame(LDVCoreDecoder *dec, LDVFrameView *view);
/* 释放当前借出的帧。传入的 view 会被清零。允许重复调用（幂等）。 */
void LDVCoreDecoderReleaseFrame(LDVCoreDecoder *dec, LDVFrameView *view);

/* ------------------------------------------------------------------------ */
/* 解码缓存模式                                                              */
/*                                                                          */
/* 与上面的"单槽借出"是两条互斥的取帧路径：                                  */
/*   - 借出模式：取一帧给调用方看，看完 Release                              */
/*   - 缓存模式：取一帧塞进 LDVFrameCache，由缓存统一持有与释放              */
/* 之所以要有缓存模式：需求是"暂停后仍继续解码，直到触及缓存上限"，           */
/* 那就不可能只留一个槽位 —— 必须能攒住多帧。                               */
/* ------------------------------------------------------------------------ */

/*
 * 取一帧并放进缓存（所有权转移给缓存）。
 *
 * 返回：
 *   LDV_OK              成功，已存入缓存
 *   LDV_ERR_NEED_INPUT  解码器暂时没有可输出的帧（应继续喂数据）
 *   LDV_ERR_NEED_INPUT  缓存已满（调用方应等 LDVFrameCacheWaitForSpace）
 * 注意两种 NEED_INPUT 的区分方式：调用前先用 LDVFrameCacheIsFull() 判断。
 *
 * 本函数**不阻塞**：缓存满时立刻返回，不会自己等。等待由调用方决定，
 * 这样解码线程才能在等待期间检查停止标志。
 */
LDVResult LDVCoreDecoderPullIntoCache(LDVCoreDecoder *dec, LDVFrameCache *cache);

/*
 * 取一帧进缓存，**可选先缩小**。
 *
 *   scaler == NULL  行为与 LDVCoreDecoderPullIntoCache 完全相同。
 *   scaler != NULL  把帧缩小到 scaler 的目标尺寸后再入缓存；入缓存的是
 *                   一块**自有的连续缓冲**（LDVFrameCachePushOwned），
 *                   不是 dav1d 的帧。
 *
 * 为什么缩放放在这里而不是调用方
 * ------------------------------
 * 缩放需要"原始帧"（Dav1dPicture），而它的所有权在本模块内部
 * （PullFrame 之后由 dec->current 持有）。放在调用方就得把
 * Dav1dPicture 暴露出去，破坏"单槽借用"的封装。
 *
 * 任何一步失败（分配失败 / 缩放不支持）都**退回原尺寸入缓存**，
 * 而不是丢帧 —— 内存换画质可以商量，丢帧不行。
 */
/*
 * 前置声明 tag —— ★ 不能省。
 *
 * 如果不先声明，下面原型参数表里的 "struct LDVDownscaler" 就成了这个
 * tag 的**首次声明**，而 clang 3.5 会把它限制在**原型作用域**里
 * （警告 "Declaration of 'struct LDVDownscaler' will not be visible
 * outside of this function"）。于是原型里的参数类型与 LDVCoreDecoder.c
 * 定义处（已 include LDVDownscale.h、tag 是文件作用域）不是同一个类型，
 * 报 "Conflicting types for 'LDVCoreDecoderPullIntoCacheScaled'"。
 * 与 LDVDownscale.h 的 "typedef struct LDVDownscaler LDVDownscaler;"
 * 用的是同一个 tag，二者不会冲突（C99 允许 tag 重复声明）。
 * 本库要求 C99，不能像 C11 那样靠重复 typedef 兜底，所以用 tag 前置声明。
 */
struct LDVDownscaler;

LDVResult LDVCoreDecoderPullIntoCacheScaled(LDVCoreDecoder *dec,
                                            LDVFrameCache *cache,
                                            struct LDVDownscaler *scaler);

/* 清空缓存里所有帧并归还内存。 */
void LDVCoreDecoderClearCache(LDVCoreDecoder *dec, LDVFrameCache *cache);

/* ------------------------------------------------------------------------ */
/* 诊断（注意：本库不实时，不提供也不显示帧率）                              */
/* ------------------------------------------------------------------------ */

int64_t LDVCoreDecoderFrameCount(const LDVCoreDecoder *dec);
int     LDVCoreDecoderErrorCount(const LDVCoreDecoder *dec);
const LDVTuningProfile *LDVCoreDecoderTuning(const LDVCoreDecoder *dec);

/*
 * dav1d 实际采用的帧级并行深度（n_fc，由 dav1d_get_frame_delay 回读）。
 * 单核档上应当为 1 —— 意味着 dav1d 完全没有创建线程，
 * 这是验证 A4 单核优化是否生效的最直接指标。
 */
int     LDVCoreDecoderEffectiveFrameDelay(const LDVCoreDecoder *dec);

#ifdef __cplusplus
}
#endif

#endif /* LDV_CORE_DECODER_H */
