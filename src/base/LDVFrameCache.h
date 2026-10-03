/*
 * LDVFrameCache.h — 有字节预算上限的帧缓存（解码框架通用模块）
 *
 * 定位
 * ----
 * 这是"解码缓存"的实现：解出来的帧先存进这里，显示端按自己的节奏取用。
 * 它是**生产者-消费者缓冲**，不是播放列表：
 *   - 生产者：解码线程（LDVDecodeSession），持续把帧推进来
 *   - 消费者：显示端（App），Peek 当前帧 → 渲染 → Pop 释放
 *
 * 为什么要有上限
 * --------------
 * 需求是"暂停后仍继续解码，直到触及缓存上限"。上限是**内存**的硬约束：
 *   - iOS 4 设备（iPhone 3GS 只有 256 MB RAM）必须严格封顶
 *   - 856x480 I420 一帧约 0.6 MB，1080p 一帧约 3 MB
 *   所以预算用**字节**表示，帧数只是辅助上限。
 *
 * 帧的形态（v8.8 起只有一种）
 * --------------------------
 * 缓存只接收**自有缓冲**：一整块 malloc 的连续内存 + LDVFrameView 描述，
 * 释放就是 free()。这让它与具体解码器完全解耦（dav1d / libhevc 都能用）：
 * 解码器侧负责把解码器自己的帧表示（如 Dav1dPicture）复制进自有缓冲
 * —— 复制成本对老设备的解码耗时来说可以忽略（512x384 I420 288 KB，
 * 远小于单帧解码时间），换来的是缓存层零依赖。
 *
 * 借用语义（与库的其他部分一致）
 * ------------------------------
 * Peek 借出的是**缓存槽位里的帧**，在 Pop 之前一直有效，期间不能再次 Peek。
 * 这是刻意的：把"用完了没释放"从纪律变成类型约束 —— 忘了 Pop 就会
 * 拿不到下一帧，立刻可见，而不是悄悄泄漏内存。
 */

#ifndef LDV_FRAME_CACHE_H
#define LDV_FRAME_CACHE_H

#include "LDVTypes.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LDVFrameCache LDVFrameCache;

/*
 * 创建缓存。
 *   byteBudget  帧数据字节上限（0 表示不按字节限制，只按 maxFrames）
 *   maxFrames   帧数上限（<=0 表示不按帧数限制）
 * 两者取先到者。都 <=0 则缓存无上限（仅用于调试，别在真机上这么用）。
 */
LDVFrameCache *LDVFrameCacheCreate(size_t byteBudget, int maxFrames);

/* 销毁。会释放所有缓存帧。传 NULL 或 *cache == NULL 都安全。 */
void LDVFrameCacheDestroy(LDVFrameCache **cache);

/* ------------------------------------------------------------------------ */
/* 生产者侧                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 把一帧**自有缓冲**的所有权移入缓存（唯一入队路径）。
 *
 * 为什么需要它：解码线程在入缓存前可能要把帧缩小（见 LDVDownscale.h），
 * 缩小后的帧不是解码器分配的，不能用解码器的释放函数释放。
 * 这条路径让缓存直接接管一块调用方 malloc 的内存。
 *
 *   view       帧的规范描述（plane/stride/width/height/bpc/layout/...）
 *              缓存会**整个复制**它，调用方之后可以随意改自己的副本。
 *              注意 view->index 会被缓存覆盖（缓存自己维护序号）。
 *   owner      这块内存的基址，Pop/Drop 时由缓存 free() 掉。
 *              **必须是一次 malloc 得到的整块**，不能是多个平面分别
 *              malloc 再拼起来的（那样 free(owner) 会漏掉其它平面）。
 *              传 NULL 会被拒绝 —— 见实现里的说明（用后释放隐患）。
 *   ownerBytes 计入字节预算的量（传 0 表示按 view 的尺寸估算）
 *
 * 缓存已满时不接收，返回 LDV_ERR_NEED_INPUT，owner 仍归调用方。
 */
LDVResult LDVFrameCachePushOwned(LDVFrameCache *cache,
                                 const LDVFrameView *view,
                                 void *owner, size_t ownerBytes);

/* 缓存是否已满（按字节或帧数任一到达上限） */
int LDVFrameCacheIsFull(const LDVFrameCache *cache);

/* 丢弃最旧的一帧以腾出空间。返回被丢弃的帧数（0 或 1）。 */
int LDVFrameCacheDropOldest(LDVFrameCache *cache);

/*
 * 丢弃所有宽超过 maxWidth 的缓存帧（v8.18 动态分辨率的缓存策略）。
 *
 * 用途：解码分辨率换挡时，"高清缓存"（比新显示目标宽 1.5 倍以上的帧）
 * 释放内存，低清帧保留继续显示 —— 显示端按帧自适应尺寸，
 * 新旧分辨率在缓存里短暂共存是设计内的。
 *
 * maxWidth <= 0 是空操作。借用中的帧（队首正在显示）不会被动。
 * 返回释放的帧数；字节预算同步回收。
 */
int LDVFrameCacheDropWiderThan(LDVFrameCache *cache, int maxWidth);

/* ------------------------------------------------------------------------ */
/* 消费者侧                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * 借出当前待显示的帧。空缓存返回 0。
 * 借出的 view 在 LDVFrameCachePop() 之前有效。
 */
int LDVFrameCachePeek(LDVFrameCache *cache, LDVFrameView *out);

/*
 * 只看队首帧的时间戳，**不借出**。空返回 0。
 *
 * 为什么需要它：显示端要先知道"下一帧该什么时候显示"再决定取不取，
 * 如果为了看时间戳就先 Peek，那还没到显示时间时帧已经被借出，
 * 只能要么提前显示、要么被迫 Pop 丢掉 —— 两条路都不对。
 * 所以单独给一个只读查询。
 */
int LDVFrameCachePeekTimestamp(LDVFrameCache *cache, int64_t *outTimestamp);

/*
 * 只看队首帧的**解码输出序号**（从 0 开始，单调递增），不借出。
 *
 * 为什么需要它：显示节拍应该按"这是第几帧"算，而不是按容器给的时间戳算。
 * 实测素材（videoplayback.mp4）的 MP4 是**按字节切块**封装的，
 * 样本边界和帧边界并不对齐 —— 一个样本里可能塞了 1~3 帧，也有只有
 * 帧头（show_existing_frame）的样本。这种情况下容器 PTS 与真实帧
 * 没有一一对应关系，拿它做节拍会让播放速度错乱。
 * 帧序号则永远与"第几帧"一致，是最可靠的节拍基准。
 */
int LDVFrameCachePeekIndex(LDVFrameCache *cache, int64_t *outIndex);

/* 丢弃当前借出的帧（释放其内存）。没有借出时是空操作。 */
void LDVFrameCachePop(LDVFrameCache *cache);

/* ------------------------------------------------------------------------ */
/* 同步原语                                                                  */
/*                                                                          */
/* 缓存自带互斥锁 + 条件变量，所以调用方不需要再套一层锁。                   */
/* 等待一律带超时：退出时不能被 pthread_cond_wait 卡死，调用方在超时返回后   */
/* 重查自己的停止标志即可（100 ms 粒度对播放没有影响，但退出能立刻响应）。   */
/* ------------------------------------------------------------------------ */

/*
 * 生产者用：等缓存有空间。
 * timeoutMs <= 0 表示一直等（直到被 Broadcast 或 Destroy 唤醒）。
 * 返回 1 = 有空间了，0 = 超时或被中止。
 */
int LDVFrameCacheWaitForSpace(LDVFrameCache *cache, int timeoutMs);

/*
 * 消费者用：等缓存里有帧。
 * timeoutMs <= 0 表示一直等。
 * 返回 1 = 有帧了，0 = 超时或被中止。
 */
int LDVFrameCacheWaitForFrame(LDVFrameCache *cache, int timeoutMs);

/*
 * 唤醒所有正在等待的线程。用于停止流程 —— 让可能阻塞在
 * WaitForSpace / WaitForFrame 里的线程立刻醒来去检查停止标志。
 */
void LDVFrameCacheBroadcast(LDVFrameCache *cache);

/* ------------------------------------------------------------------------ */
/* 状态                                                                      */
/* ------------------------------------------------------------------------ */

int    LDVFrameCacheCount(const LDVFrameCache *cache);
size_t LDVFrameCacheBytes(const LDVFrameCache *cache);
size_t LDVFrameCacheByteBudget(const LDVFrameCache *cache);
int    LDVFrameCacheMaxFrames(const LDVFrameCache *cache);

/* 填充进度 0.0 ~ 1.0。无上限时恒为 0。 */
float  LDVFrameCacheProgress(const LDVFrameCache *cache);

/* 累计进出帧数（用于观察是否均衡、有没有泄漏） */
int64_t LDVFrameCacheTotalPushed(const LDVFrameCache *cache);
int64_t LDVFrameCacheTotalPopped(const LDVFrameCache *cache);
int64_t LDVFrameCacheTotalDropped(const LDVFrameCache *cache);

/* 因为缓存满而被拒绝的次数（生产者等待的次数） */
int64_t LDVFrameCacheFullHits(const LDVFrameCache *cache);

/* 清空（释放全部缓存帧）。用于 stop / 释放内存。 */
void LDVFrameCacheClear(LDVFrameCache *cache);

#ifdef __cplusplus
}
#endif

#endif /* LDV_FRAME_CACHE_H */
