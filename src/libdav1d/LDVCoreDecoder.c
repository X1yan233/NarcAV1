/*
 * LDVCoreDecoder.c — dav1d 的 C99 封装
 */

#include "LDVCoreDecoder.h"
#include "LDVDsp.h"
#include "LDVDownscale.h"

#include <stdlib.h>
#include <stdio.h>    /* fprintf（流参数探针）—— 不赌间接包含链 */
#include <string.h>
#include <errno.h>

#include "dav1d/dav1d.h"

/* ------------------------------------------------------------------------ */
/* 编译期校验：本地枚举数值必须与 dav1d 完全一致                             */
/*                                                                          */
/* 用 C99 的"负数组长度"技巧做静态断言（_Static_assert 是 C11，本库要求     */
/* C99 所以不能用）。这些断言能在编译期挡住 dav1d 升级时枚举漂移导致的       */
/* 静默错值 —— 这类 bug 运行时极难定位。                                     */
/* ------------------------------------------------------------------------ */

typedef char LDVAssertLayoutI400[(LDV_PIXEL_LAYOUT_I400 == DAV1D_PIXEL_LAYOUT_I400) ? 1 : -1];
typedef char LDVAssertLayoutI420[(LDV_PIXEL_LAYOUT_I420 == DAV1D_PIXEL_LAYOUT_I420) ? 1 : -1];
typedef char LDVAssertLayoutI422[(LDV_PIXEL_LAYOUT_I422 == DAV1D_PIXEL_LAYOUT_I422) ? 1 : -1];
typedef char LDVAssertLayoutI444[(LDV_PIXEL_LAYOUT_I444 == DAV1D_PIXEL_LAYOUT_I444) ? 1 : -1];

typedef char LDVAssertFilterNone[(LDV_INLOOP_FILTER_NONE        == DAV1D_INLOOPFILTER_NONE)        ? 1 : -1];
typedef char LDVAssertFilterDeblock[(LDV_INLOOP_FILTER_DEBLOCK  == DAV1D_INLOOPFILTER_DEBLOCK)     ? 1 : -1];
typedef char LDVAssertFilterCdef[(LDV_INLOOP_FILTER_CDEF        == DAV1D_INLOOPFILTER_CDEF)        ? 1 : -1];
typedef char LDVAssertFilterRest[(LDV_INLOOP_FILTER_RESTORATION == DAV1D_INLOOPFILTER_RESTORATION) ? 1 : -1];

typedef char LDVAssertMatrixIdentity[(LDV_MATRIX_IDENTITY   == DAV1D_MC_IDENTITY)   ? 1 : -1];
typedef char LDVAssertMatrixBt709[(LDV_MATRIX_BT709         == DAV1D_MC_BT709)      ? 1 : -1];
typedef char LDVAssertMatrixUnknown[(LDV_MATRIX_UNKNOWN     == DAV1D_MC_UNKNOWN)    ? 1 : -1];
typedef char LDVAssertMatrixBt470bg[(LDV_MATRIX_BT470BG     == DAV1D_MC_BT470BG)    ? 1 : -1];
typedef char LDVAssertMatrixBt601[(LDV_MATRIX_BT601         == DAV1D_MC_BT601)      ? 1 : -1];
typedef char LDVAssertMatrixBt2020[(LDV_MATRIX_BT2020_NCL   == DAV1D_MC_BT2020_NCL) ? 1 : -1];

/* ------------------------------------------------------------------------ */

struct LDVCoreDecoder {
    Dav1dContext   *ctx;
    LDVTuningProfile tuning;

    /*
     * 单槽借用：当前已借出的帧。
     * 用"不释放就取不到下一帧"把 dav1d_picture_unref 的漏调变成
     * 立即可见的错误，而不是隐蔽的内存泄漏。
     */
    Dav1dPicture current;
    int          hasCurrent;

    int64_t frameCount;
    int     errorCount;
    int     effectiveFrameDelay;   /* dav1d 实际采用的 n_fc，用于自检 */
    int     probeLogged;           /* 流参数探针只打一次（v8.15 排查 720p 用） */
};

/* ------------------------------------------------------------------------ */
/* 序列头探测                                                                */
/* ------------------------------------------------------------------------ */

int LDVCoreProbeSequenceHeader(const uint8_t *buf, size_t size,
                               int *outWidth, int *outHeight,
                               int *outBpc, int *outLayout,
                               int *outColorRange,
                               int *outOperatingPointCount)
{
    Dav1dSequenceHeader hdr;
    int err;

    if (!buf || size == 0) return LDV_ERR_INVALID_ARG;

    memset(&hdr, 0, sizeof(hdr));
    err = dav1d_parse_sequence_header(&hdr, buf, size);

    if (err == DAV1D_ERR(ENOENT)) return LDV_ERR_NEED_INPUT;
    if (err < 0)                  return LDV_ERR_BITSTREAM;

    if (outWidth)  *outWidth  = hdr.max_width;
    if (outHeight) *outHeight = hdr.max_height;
    if (outBpc)    *outBpc    = (hdr.hbd == 0) ? 8 : (hdr.hbd == 1 ? 10 : 12);
    if (outLayout) *outLayout = (int)hdr.layout;
    if (outColorRange) *outColorRange = hdr.color_range;
    if (outOperatingPointCount) *outOperatingPointCount = hdr.num_operating_points;

    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 生命周期                                                                  */
/* ------------------------------------------------------------------------ */

LDVCoreDecoder *LDVCoreDecoderCreate(const LDVTuningProfile *tuning) {
    LDVCoreDecoder *dec;
    Dav1dSettings   s;
    int             err;

    dec = (LDVCoreDecoder *)calloc(1, sizeof(*dec));
    if (!dec) return NULL;

    dec->tuning = tuning ? *tuning : LDVTuningProfileForCurrentDevice();

    /*
     * dav1d_default_settings 会把 allocator 设成 dav1d_default_picture_alloc /
     * dav1d_default_picture_release（dav1d-master/src/lib.c:75-77），
     * 而 dav1d_open 会校验这两个回调非空（src/lib.c:150-153），
     * 所以 demo 阶段直接用默认分配器即可。
     *
     * 正式版必须换成 LDVPicturePool 的自定义分配器，理由见
     * docs/ARCHITECTURE.md §5.5：内存上界可预测、避免大块 malloc 抖动、
     * 以及让 T2 的帧直接引用池内 slab 实现零拷贝。
     */
    dav1d_default_settings(&s);

    s.n_threads        = dec->tuning.nThreads;
    s.max_frame_delay  = dec->tuning.maxFrameDelay;
    s.apply_grain      = dec->tuning.applyGrain;
    s.inloop_filters   = (enum Dav1dInloopFilterType)dec->tuning.inloopFilters;
    s.frame_size_limit = dec->tuning.frameSizeLimit;
    s.operating_point  = 0;
    s.all_layers       = 1;

    /*
     * ★ 把 DSP 实现偏好写进 dav1d 的 CPU 标志掩码，**必须在 dav1d_open 之前**。
     *
     * 为什么必须在这里：dav1d 的 DSP 函数表是在 dav1d_open 里一次性绑定的
     * （src/lib.c → dav1d_init_dsp）。掩码改晚了不影响已经建好的解码器 ——
     * 所以"切模式"这件事在会话层面必须是"重建解码器"，而不是改个标志。
     *
     * 掩码语义：dav1d_get_cpu_flags() = dav1d_cpu_flags & dav1d_cpu_flags_mask。
     * 传 0 就是"这台机器没有任何 SIMD"，dav1d 会绑 C 实现。
     */
    LDVDspApply();

    err = dav1d_open(&dec->ctx, &s);
    if (err < 0 || !dec->ctx) {
        free(dec);
        return NULL;
    }

    /* dav1d 实际采用的帧级并行深度。A4 上应当是 1（等于完全不建线程）。 */
    dec->effectiveFrameDelay = dav1d_get_frame_delay(&s);
    if (dec->effectiveFrameDelay < 1) dec->effectiveFrameDelay = 1;

    return dec;
}

void LDVCoreDecoderDestroy(LDVCoreDecoder **dec) {
    LDVCoreDecoder *d;
    if (!dec || !*dec) return;
    d = *dec;

    /* 先还掉可能还借在外面的帧，再关解码器。
     * 顺序不能反：dav1d_close 会 join 内部线程并释放所有持有的 picture。 */
    if (d->hasCurrent) {
        dav1d_picture_unref(&d->current);
        memset(&d->current, 0, sizeof(d->current));
        d->hasCurrent = 0;
    }
    dav1d_close(&d->ctx);

    free(d);
    *dec = NULL;
}

/* ------------------------------------------------------------------------ */
/* 送数据 / 取帧                                                             */
/* ------------------------------------------------------------------------ */

int LDVCoreDecoderFeed(LDVCoreDecoder *dec,
                       const uint8_t *bytes, size_t size,
                       int64_t timestamp)
{
    Dav1dData data;
    uint8_t  *dst;
    int       err;

    if (!dec || !dec->ctx || !bytes || size == 0) return LDV_ERR_INVALID_ARG;

    memset(&data, 0, sizeof(data));

    /*
     * 用 dav1d_data_create 而不是手工填 Dav1dData：
     * dav1d_data_ref 在 src->ref == NULL 时不做引用计数
     * （dav1d-master/src/data.c:87-98），手工填会让 dav1d 内部持有裸指针，
     * 而 send_data 成功后又会把调用方的结构体清零 —— 结果就是
     * 调用方以为缓冲区可以复用，实际是 use-after-free。
     *
     * 这里复制一份，代价是每包一次 memcpy。后续接预缓冲时换成
     * dav1d_data_wrap + 环缓冲归还回调即可实现零拷贝。
     */
    dst = dav1d_data_create(&data, size);
    if (!dst) return LDV_ERR_NOMEM;
    memcpy(dst, bytes, size);
    data.m.timestamp = timestamp;

    err = dav1d_send_data(dec->ctx, &data);

    if (err < 0) {
        /*
         * 失败与 EAGAIN 两种情况下，所有权都还在本函数手上，必须自己释放：
         *   EAGAIN  —— dav1d_send_data 在 src/lib.c:447 提前返回，
         *              此时 dav1d_data_ref 还没被调用，引用计数是 1。
         *   其它错误 —— dav1d_data_ref 已调用（计数 2），c->in 持有其中一份；
         *              我们再 unref 一次，计数回到 1，dav1d 保留数据。
         * 两种情况都要 unref，不能漏。
         */
        dav1d_data_unref(&data);

        if (err == DAV1D_ERR(EAGAIN)) return LDV_ERR_NEED_INPUT;

        dec->errorCount++;
        return LDV_ERR_BITSTREAM;
    }

    /* 成功：所有权已转移给 dav1d，data 已被 dav1d_data_unref_internal 清零 */
    return LDV_OK;
}

int LDVCoreDecoderPullFrame(LDVCoreDecoder *dec, LDVFrameView *view) {
    Dav1dPicture pic;
    int          err;

    if (!dec || !dec->ctx || !view) return LDV_ERR_INVALID_ARG;
    if (dec->hasCurrent)            return LDV_ERR_INVALID_ARG;  /* 上一帧未释放 */

    memset(&pic, 0, sizeof(pic));
    err = dav1d_get_picture(dec->ctx, &pic);

    if (err < 0) {
        if (err == DAV1D_ERR(EAGAIN)) return LDV_ERR_NEED_INPUT;
        dec->errorCount++;
        return LDV_ERR_BITSTREAM;
    }

    /* 接管所有权，直到 ReleaseFrame 才还回去 */
    dec->current    = pic;
    dec->hasCurrent = 1;

    memset(view, 0, sizeof(*view));
    view->plane[0] = (const uint8_t *)pic.data[0];
    view->plane[1] = (const uint8_t *)pic.data[1];
    view->plane[2] = (const uint8_t *)pic.data[2];
    view->stride[0] = pic.stride[0];
    view->stride[1] = pic.stride[1];
    view->width     = pic.p.w;
    view->height    = pic.p.h;
    view->bpc       = pic.p.bpc;
    view->layout    = (LDVPixelLayout)pic.p.layout;
    view->colorRange = pic.seq_hdr ? pic.seq_hdr->color_range : 0;
    view->matrix    = pic.seq_hdr
                    ? (LDVMatrixCoefficients)pic.seq_hdr->mtrx
                    : LDV_MATRIX_UNKNOWN;
    view->filmGrain = pic.frame_hdr ? (int)pic.frame_hdr->film_grain.present : 0;
    view->timestamp = pic.m.timestamp;
    view->index     = (int)dec->frameCount;

    dec->frameCount++;
    return LDV_OK;
}

void LDVCoreDecoderReleaseFrame(LDVCoreDecoder *dec, LDVFrameView *view) {
    if (!dec) return;

    if (dec->hasCurrent) {
        dav1d_picture_unref(&dec->current);
        memset(&dec->current, 0, sizeof(dec->current));
        dec->hasCurrent = 0;
    }
    if (view) memset(view, 0, sizeof(*view));
}

/* ------------------------------------------------------------------------ */
/* 诊断                                                                      */
/* ------------------------------------------------------------------------ */

int64_t LDVCoreDecoderFrameCount(const LDVCoreDecoder *dec) {
    return dec ? dec->frameCount : 0;
}
int LDVCoreDecoderErrorCount(const LDVCoreDecoder *dec) {
    return dec ? dec->errorCount : 0;
}

const LDVTuningProfile *LDVCoreDecoderTuning(const LDVCoreDecoder *dec) {
    return dec ? &dec->tuning : NULL;
}

int LDVCoreDecoderEffectiveFrameDelay(const LDVCoreDecoder *dec) {
    return dec ? dec->effectiveFrameDelay : 0;
}

/* ------------------------------------------------------------------------ */
/* 解码缓存模式                                                              */
/* ------------------------------------------------------------------------ */

/*
 * 把 dav1d 帧复制进一块自有缓冲并推入缓存（v8.8）。
 *
 * ★ 缓存（LMCDecoderBase，与解码器解耦）只接收自有缓冲 —— 见
 *   LDVFrameCache.h。这里负责 dav1d 帧表示到通用帧的转换：
 *   三个平面复制进一整块 malloc，view.plane 指向**新缓冲**里的位置
 *   （不能指向 pic 的原始数据 —— 调用方随后的 unref 会让它失效）。
 *
 * 复制成本：512x384 I420 288 KB，在 A5 上远小于单帧解码时间，
 * 换来缓存层零 dav1d 依赖（libhevc 直接复用整套缓存/音频/解封装）。
 *
 * 成功后 pic 的所有权仍在调用方（复制不是转移），必须 unref。
 */
static LDVResult ldv_push_picture_owned(LDVFrameCache *cache,
                                        const Dav1dPicture *pic) {
    LDVFrameView view;
    uint8_t     *buf;
    size_t       yBytes, uvBytes, total;
    int          h, uvH;

    if (!pic || !pic->data[0]) return LDV_ERR_INVALID_ARG;

    h   = pic->p.h;
    uvH = (pic->p.layout == DAV1D_PIXEL_LAYOUT_I400) ? 0
        : (pic->p.layout == DAV1D_PIXEL_LAYOUT_I420) ? (h + 1) / 2
        : h;    /* I422 / I444：色度行数 = 亮度行数 */

    yBytes  = (size_t)pic->stride[0] * (size_t)h;
    uvBytes = (size_t)pic->stride[1] * (size_t)uvH;
    total   = yBytes + 2 * uvBytes;

    buf = (uint8_t *)malloc(total);
    if (!buf) return LDV_ERR_NOMEM;

    memcpy(buf, pic->data[0], yBytes);
    if (uvBytes) {
        memcpy(buf + yBytes, pic->data[1], uvBytes);
        memcpy(buf + yBytes + uvBytes, pic->data[2], uvBytes);
    }

    memset(&view, 0, sizeof(view));
    view.plane[0] = buf;
    view.plane[1] = uvBytes ? (buf + yBytes) : NULL;
    view.plane[2] = uvBytes ? (buf + yBytes + uvBytes) : NULL;
    view.stride[0] = pic->stride[0];
    view.stride[1] = pic->stride[1];
    view.width      = pic->p.w;
    view.height     = pic->p.h;
    view.bpc        = pic->p.bpc;
    view.layout     = (LDVPixelLayout)pic->p.layout;
    view.colorRange = pic->seq_hdr ? pic->seq_hdr->color_range : 0;
    view.matrix     = pic->seq_hdr
                    ? (LDVMatrixCoefficients)pic->seq_hdr->mtrx
                    : LDV_MATRIX_UNKNOWN;
    view.filmGrain  = pic->frame_hdr
                    ? (int)pic->frame_hdr->film_grain.present : 0;
    view.timestamp  = pic->m.timestamp;
    view.index      = 0;              /* 由缓存自己填 */

    {
        LDVResult r = LDVFrameCachePushOwned(cache, &view, buf, total);
        if (r != LDV_OK) free(buf);   /* 拒收：pic 归调用方 unref，缓冲自释放 */
        return r;
    }
}

LDVResult LDVCoreDecoderPullIntoCache(LDVCoreDecoder *dec, LDVFrameCache *cache) {
    Dav1dPicture pic;
    int          r;
    LDVResult    pr;

    if (!dec || !cache) return LDV_ERR_INVALID_ARG;
    if (!dec->ctx)      return LDV_ERR_INVALID_ARG;

    /*
     * 缓存满时直接返回，不在这里等。
     * 理由：等待是调用方（解码线程）的策略 —— 它需要在等待期间检查
     * 停止标志，否则退出时会卡住。所以这里保持"非阻塞"的纯粹语义。
     */
    if (LDVFrameCacheIsFull(cache)) return LDV_ERR_NEED_INPUT;

    memset(&pic, 0, sizeof(pic));
    r = dav1d_get_picture(dec->ctx, &pic);
    if (r < 0) {
        if (DAV1D_ERR(r) == EAGAIN) return LDV_ERR_NEED_INPUT;   /* 暂时没有 */
        dec->errorCount++;
        /*
         * 不要把 dav1d 的原始负错误码直接当 LDVResult 返回 ——
         * LDVResult 是只有 0 与 -1..-6 的枚举，塞个 -22 进去会变成
         * 枚举范围外的值，调用方判断起来很别扭。统一映射成码流错误。
         */
        return LDV_ERR_BITSTREAM;
    }

    /*
     * 首帧流参数探针（只打一次）。
     *
     * 背景：iPhone5 解 720p30 只有 18fps 而 CPU 只有 50-70% ——
     * 解码 worker 没吃满双核，怀疑瓶颈在串行的 film grain 合成上
     * （A6 档 applyGrain=1；grain 合成走消费路径，不参加
     * n_threads=2 的解码并行）。grain=1 即实锤，下一步按这个方向治。
     */
    if (!dec->probeLogged) {
        dec->probeLogged = 1;
        fprintf(stderr,
                "LDVCore: 流首帧 %dx%d bpc=%d layout=%d grain=%d"
                "（grain=1：流带胶片颗粒合成，其合成不参与解码并行）\n",
                pic.p.w, pic.p.h, pic.p.bpc, (int)pic.p.layout,
                pic.frame_hdr ? (int)pic.frame_hdr->film_grain.present : -1);
    }

    /* 复制进自有缓冲后入缓存（v8.8：缓存只收自有缓冲） */
    pr = ldv_push_picture_owned(cache, &pic);
    /*
     * ★ 无论 push 成败，pic 都必须 unref —— v8.8 改成"复制"语义后，
     *   缓存拿到的是自拷缓冲，**不会**替我们 unref dav1d 侧的引用
     *   （旧版 PushPicture 是所有权转移，缓存释放时会 unref ——
     *    那个约定随解耦一起没了）。
     *
     *   漏掉成功路径的 unref 就是 400MB 内存的直接原因：每帧一个
     *   Dav1dPicture 引用永不归还，dav1d 的内部池无法复用，之后每帧
     *   都向系统新分配缓冲（实测 ~8MB/s，RSS 从 80MB 涨到 400MB 后
     *   被系统杀掉）。功能完全正常 —— 池只增不还，解码照跑 ——
     *   所以只有 RSS 暴露它。
     */
    dav1d_picture_unref(&pic);
    memset(&pic, 0, sizeof(pic));
    if (pr != LDV_OK) return pr;

    dec->frameCount++;
    return LDV_OK;
}

/*
 * 把 dec->current 复制进缓存，然后释放 dav1d 侧的引用。
 *
 * ★ 成功也要 unref：v8.8 改"复制"语义后，缓存不接管 dav1d 引用
 *   （旧版 PushPicture 是所有权转移，由缓存在释放槽位时 unref ——
 *    那个约定随解耦一起没了）。只清 hasCurrent 不 unref 的话，
 *    dec->current 里那个引用就悬空了：下一次 PullFrame 用新的
 *    get_picture 覆盖这个结构体，旧帧的引用计数永远不归零 ——
 *    dav1d 池又是每帧泄漏一块缓冲（与 PullIntoCache 同款）。
 *   hasCurrent 两处都清：成功时帧已进缓存，失败时帧在这里 unref。
 */
static LDVResult ldv_hand_off_current(LDVCoreDecoder *dec,
                                      LDVFrameCache *cache) {
    LDVResult pr = ldv_push_picture_owned(cache, &dec->current);
    dav1d_picture_unref(&dec->current);
    memset(&dec->current, 0, sizeof(dec->current));
    dec->hasCurrent = 0;
    return pr;
}

LDVResult LDVCoreDecoderPullIntoCacheScaled(LDVCoreDecoder *dec,
                                            LDVFrameCache *cache,
                                            struct LDVDownscaler *scaler) {
    LDVFrameView view;
    int          r;

    if (!dec || !cache) return LDV_ERR_INVALID_ARG;
    if (!dec->ctx)      return LDV_ERR_INVALID_ARG;

    /* 没配缩放器：走原路，一行都不多花 */
    if (!scaler) return LDVCoreDecoderPullIntoCache(dec, cache);

    if (LDVFrameCacheIsFull(cache)) return LDV_ERR_NEED_INPUT;

    /*
     * 复用 PullFrame 而不是自己 dav1d_get_picture：
     * 它已经把 Dav1dPicture → LDVFrameView 的映射、错误映射、
     * frameCount 自增都做好了，复制一份必然漂移。
     * ★ 注意它已经 frameCount++，所以本函数后面**不能**再自增。
     */
    r = LDVCoreDecoderPullFrame(dec, &view);
    if (r != LDV_OK) return (LDVResult)r;

    {
        const size_t need  = LDVDownscalerFrameBytes(scaler);
        uint8_t     *buf   = (uint8_t *)malloc(need);
        uint8_t     *plane[3];
        LDVFrameView small;
        LDVResult    dr;

        if (!buf) {
            /* 内存不够 → 退回原尺寸。宁可多占缓存，也不要丢帧。 */
            return ldv_hand_off_current(dec, cache);
        }

        plane[0] = buf + LDVDownscalerPlaneOffset(scaler, 0);
        plane[1] = buf + LDVDownscalerPlaneOffset(scaler, 1);
        plane[2] = buf + LDVDownscalerPlaneOffset(scaler, 2);

        dr = LDVDownscalerRun(scaler, &view, plane);
        if (dr != LDV_OK) {
            /* 缩放不支持（如高位深）→ 退回原尺寸 */
            free(buf);
            return ldv_hand_off_current(dec, cache);
        }

        /* 构造缩小后帧的描述。元信息（layout/色域/矩阵/时间戳）沿用原帧。 */
        small            = view;
        small.width      = LDVDownscalerDstWidth(scaler);
        small.height     = LDVDownscalerDstHeight(scaler);
        small.plane[0]   = plane[0];
        small.plane[1]   = (view.layout == LDV_PIXEL_LAYOUT_I400) ? NULL : plane[1];
        small.plane[2]   = (view.layout == LDV_PIXEL_LAYOUT_I400) ? NULL : plane[2];
        small.stride[0]  = (ptrdiff_t)LDVDownscalerPlaneStride(scaler, 0);
        small.stride[1]  = (ptrdiff_t)LDVDownscalerPlaneStride(scaler, 1);

        {
            LDVResult pr = LDVFrameCachePushOwned(cache, &small, buf, need);
            if (pr != LDV_OK) {
                free(buf);
                LDVCoreDecoderReleaseFrame(dec, &view);
                return pr;
            }
        }

        /* 缩小后的帧已经入缓存，原帧可以还回 dav1d 了 */
        LDVCoreDecoderReleaseFrame(dec, &view);
        return LDV_OK;
    }
}

void LDVCoreDecoderClearCache(LDVCoreDecoder *dec, LDVFrameCache *cache) {
    (void)dec;
    if (cache) LDVFrameCacheClear(cache);
}
