/*
 * LDVDownscale.c — 解码帧的双立方（Keys cubic）缩小
 *
 * 算法：**可分离两趟 + 展宽支撑域的 Keys 立方核（a = -0.5）**
 * ------------------------------------------
 * 缩小是线性可分离的，所以拆成水平一趟、垂直一趟，复杂度从
 * O(W·H·taps²) 降到 O(W·H·taps)。每趟只是"加权求和"：
 *
 *     dst[j] = ( Σ w[j][k] · src[idx[j][k]] ) >> 16
 *
 * 权重是 16.16 定点。核：**Keys cubic（a = -0.5）**，负瓣带来轻锐化
 * （"较锐利"的来源）。
 *
 * 抽头排布（全支撑域积分 —— 与 GPU 着色器的 8/12-tap 跳采不同）
 * ----------------------------------
 * 核按缩放比 scale 缩放后支撑域为 ±2·scale。CPU 侧没有采样预算压力，
 * 用**单位间距把支撑域采满**：taps = 2·⌈2r⌉+1（r = min(scale, 4)）。
 * GPU 跳采版（8/12-tap）在 4x 下对核自身欠采样（check-downscale 实测
 * 条纹振幅 41 vs 全支撑 <20），别把 GPU 档位照搬到 CPU。
 *
 * 边界处理
 * --------
 * 抽头下标越界时**钳位到边界**（"边界像素向外复制"），权重照算。
 * 负瓣可能让累加越出 [0, 255·65536]（黑点邻白点），两趟里都钳位 ——
 * 旧三角形核权重全非负、和恒 65536，不需要这一步。
 *
 * 性能
 * ----
 * taps 从旧三角形核的 2r+3 升到 2·⌈2r⌉+1（r=2: 7→9，r=4: 11→17），
 * 512x384 → 256x192 约 13 万次乘加，A5 上 1 ms 上下，相对解码可忽略。
 */

#include "LDVDownscale.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 分档：r<3 → 4-tap（GPU 同源）；r≥3 → 单位间距全支撑（≤ 2·⌈2·4⌉+1 = 17） */
#define LDV_DS_MAX_TAPS   17

/* 4-tap / 全支撑的分界：达到 3x 起换全支撑（跳采在大 r 下欠采样） */
#define LDV_DS_WIDE_SCALE 3.0

/* 缩放比上限：CPU 缩小只服务 A6/A7/A8（输出上限 1280/1920），r>4 的
 * 场景（4K→极小窗）不存在于此档；钳住防 spacing 过大欠采样 */
#define LDV_DS_MAX_RADIUS 4.0

/* 行距对齐（字节）。标量代码不需要 SIMD 对齐，给 16 只是为了整齐。 */
#define LDV_DS_ALIGN 16

/* 16.16 归一化后的权重和 —— 必须恰好是这个值，否则整帧偏色 */
#define LDV_DS_ONE 65536

/* ------------------------------------------------------------------------ */
/* 一维重采样权重表                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      taps;      /* 每个目标样本的抽头数（不足的用 0 权重补齐） */
    int32_t *weight;    /* [dstLen * taps] 16.16，每行和为 LDV_DS_ONE */
    int32_t *index;     /* [dstLen * taps] 源下标（已钳位到 [0, srcLen-1]） */
} LDVResampleAxis;

static void axis_free(LDVResampleAxis *a) {
    if (!a) return;
    free(a->weight); a->weight = NULL;
    free(a->index);  a->index  = NULL;
    a->taps = 0;
}

/*
 * Keys cubic（a = -0.5）。|s| ≥ 2 时核值为 0（支撑域外）。
 */
static double ldv_keys_kernel(double s) {
    const double a = fabs(s);
    if (a < 1.0) return 1.5 * a * a * a - 2.5 * a * a + 1.0;
    if (a < 2.0) return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
    return 0.0;
}

/*
 * 构造一条轴的重采样表（Keys 立方核，与 GPU 分档同源）。
 * 返回 0 成功，-1 失败（内存不足）。
 *
 * 分档（v8.19e，按真实场景校准）：
 *   r < 3 → 4-tap，位置 (k-1.5)·r，权重 keys(0.5)/keys(1.5) 常数
 *           （= GPU 4-tap 表；CPU 缩小的主力场景：720p→iPhone5 窗口
 *           r≈2.0，成本 ~4M MAC/帧，A6 上 3ms 级）
 *   r ≥ 3 → 单位间距全支撑（taps = 2·⌈2r⌉+1 ≤ 17）：r≈3 的场景
 *           （1080p→SE1 窗口）spacing 已 ≈1 纹素，跳采会欠采样
 *           （12-tap spacing r/3 在 r=4 实测条纹振幅 102/255）。
 *           此档在 A9 上不会走（A9 归 GPU），仅作 A7/A8 大幅缩小兜底。
 */
static int axis_build(LDVResampleAxis *a, int srcLen, int dstLen) {
    const double scale = (double)srcLen / (double)dstLen;
    double       r, spacing;
    int          taps, j;

    memset(a, 0, sizeof(*a));
    if (srcLen <= 0 || dstLen <= 0) return -1;

    r = scale;
    if (r < 1.0) r = 1.0;
    if (r > LDV_DS_MAX_RADIUS) r = LDV_DS_MAX_RADIUS;

    if (r >= LDV_DS_WIDE_SCALE) {
        taps    = 2 * (int)ceil(2.0 * r) + 1;   /* 单位间距全支撑 */
        spacing = 1.0;
    } else {
        taps    = 4;                            /* GPU 同源 4-tap */
        spacing = r;
    }
    if (taps > LDV_DS_MAX_TAPS) taps = LDV_DS_MAX_TAPS;

    a->taps   = taps;
    a->weight = (int32_t *)calloc((size_t)dstLen * (size_t)taps,
                                  sizeof(int32_t));
    a->index  = (int32_t *)calloc((size_t)dstLen * (size_t)taps,
                                  sizeof(int32_t));
    if (!a->weight || !a->index) { axis_free(a); return -1; }

    for (j = 0; j < dstLen; j++) {
        /* 目标样本中心映射回源坐标 */
        const double center = ((double)j + 0.5) * scale - 0.5;
        const double half   = (double)(taps - 1) * 0.5;
        int32_t     *wq     = a->weight + (size_t)j * (size_t)taps;
        int32_t     *iq     = a->index  + (size_t)j * (size_t)taps;

        double  raw[LDV_DS_MAX_TAPS];
        double  wsum = 0.0;
        int     k, maxK = 0;

        for (k = 0; k < taps; k++) {
            const double off = ((double)k - half) * spacing;
            const double w   = ldv_keys_kernel(fabs(off) / r);
            int          ic  = (int)floor(center + off + 0.5);

            if (ic < 0)       ic = 0;
            if (ic >= srcLen) ic = srcLen - 1;

            iq[k]  = ic;
            raw[k] = w;
            wsum  += w;
            if (fabs(w) > fabs(raw[maxK])) maxK = k;
        }

        /*
         * 带符号归一化到恰好 65536：逐抽头四舍五入，残差补在
         * |raw| 最大的抽头上（它的相对误差最小）。
         */
        {
            int32_t acc = 0;
            for (k = 0; k < taps; k++) {
                int32_t q = (int32_t)(raw[k] / wsum * (double)LDV_DS_ONE
                                      + (raw[k] >= 0.0 ? 0.5 : -0.5));
                wq[k] = q;
                acc  += q;
            }
            wq[maxK] += (int32_t)LDV_DS_ONE - acc;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* 两趟重采样                                                                */
/* ------------------------------------------------------------------------ */

/*
 * 水平趟：src(rows × srcLen) → tmp(rows × dstLen)
 * 累加上限 255 × 65536 = 16,711,680，int32 安全。
 */
static void axis_run_h(const LDVResampleAxis *a,
                       const uint8_t *src, ptrdiff_t srcStride,
                       int rows, int dstLen,
                       uint8_t *tmp, ptrdiff_t tmpStride) {
    int y, j, k;

    for (y = 0; y < rows; y++) {
        const uint8_t *s = src + (ptrdiff_t)y * srcStride;
        uint8_t       *d = tmp + (ptrdiff_t)y * tmpStride;

        for (j = 0; j < dstLen; j++) {
            const int32_t *wq = a->weight + (size_t)j * (size_t)a->taps;
            const int32_t *iq = a->index  + (size_t)j * (size_t)a->taps;
            int32_t        acc = 0;

            for (k = 0; k < a->taps; k++) {
                acc += wq[k] * (int32_t)s[iq[k]];
            }
            /*
             * 立方核有负瓣：权重和恒为 65536，但个别累加可能越出
             * [0, 255·65536]（黑点邻白点等）。钳位，防 (uint8_t) 回绕。
             */
            if (acc < 0) acc = 0;
            if (acc > 16711680) acc = 16711680;   /* 255 * 65536 */
            d[j] = (uint8_t)(acc >> 16);
        }
    }
}

/*
 * 垂直趟：tmp(srcRows × dstLen) → dst(dstRows × dstLen)
 *
 * 内层按 x 连续走，读 tmp 时是逐行顺序访问（对 cache 友好）。
 */
static void axis_run_v(const LDVResampleAxis *a,
                       const uint8_t *tmp, ptrdiff_t tmpStride,
                       int dstRows, int dstLen,
                       uint8_t *dst, ptrdiff_t dstStride) {
    int j, x, k;

    for (j = 0; j < dstRows; j++) {
        const int32_t *wq = a->weight + (size_t)j * (size_t)a->taps;
        const int32_t *iq = a->index  + (size_t)j * (size_t)a->taps;
        uint8_t       *d  = dst + (ptrdiff_t)j * dstStride;

        /* 先把这一行的 taps 个源行基址取出来，避免内层重复乘 */
        const uint8_t *rows[LDV_DS_MAX_TAPS];
        int32_t        ws[LDV_DS_MAX_TAPS];

        for (k = 0; k < a->taps; k++) {
            rows[k] = tmp + (ptrdiff_t)iq[k] * tmpStride;
            ws[k]   = wq[k];
        }

        for (x = 0; x < dstLen; x++) {
            int32_t acc = 0;
            for (k = 0; k < a->taps; k++) {
                acc += ws[k] * (int32_t)rows[k][x];
            }
            /* 负瓣钳位，同水平趟：防 (uint8_t) 回绕 */
            if (acc < 0) acc = 0;
            if (acc > 16711680) acc = 16711680;   /* 255 * 65536 */
            d[x] = (uint8_t)(acc >> 16);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* 对象                                                                      */
/* ------------------------------------------------------------------------ */

struct LDVDownscaler {
    int           srcW, srcH;
    int           dstW, dstH;
    LDVPixelLayout layout;

    int           nPlanes;      /* 1（I400）或 3 */
    int           srcCW, srcCH; /* 色度平面尺寸 */
    int           dstCW, dstCH;

    size_t        stride[3];    /* 目标平面行距（字节） */
    size_t        offset[3];    /* 目标平面在帧缓冲里的偏移 */
    size_t        frameBytes;

    LDVResampleAxis hxLuma, vxLuma;
    LDVResampleAxis hxChroma, vxChroma;

    uint8_t      *scratch;      /* 水平趟的中间平面（按最大的 Y 平面开） */
    size_t        scratchBytes;
};

static size_t ldv_ds_align(size_t v) {
    return (v + (LDV_DS_ALIGN - 1)) & ~(size_t)(LDV_DS_ALIGN - 1);
}

static void ldv_ds_plane_dims(const LDVDownscaler *s, int plane,
                              int *pw, int *ph) {
    if (plane == 0) { *pw = s->dstW;  *ph = s->dstH;  return; }
    *pw = s->dstCW; *ph = s->dstCH;
}

static void ldv_ds_src_dims(const LDVDownscaler *s, int plane,
                            int *pw, int *ph) {
    if (plane == 0) { *pw = s->srcW;  *ph = s->srcH;  return; }
    *pw = s->srcCW; *ph = s->srcCH;
}

LDVDownscaler *LDVDownscalerCreate(int srcW, int srcH,
                                   int dstW, int dstH,
                                   LDVPixelLayout layout) {
    LDVDownscaler *s;

    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return NULL;
    if (dstW > srcW || dstH > srcH) return NULL;
    if (layout < LDV_PIXEL_LAYOUT_I400 || layout > LDV_PIXEL_LAYOUT_I444) {
        return NULL;
    }

    s = (LDVDownscaler *)calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->srcW = srcW;  s->srcH = srcH;
    s->dstW = dstW;  s->dstH = dstH;
    s->layout = layout;

    if (layout == LDV_PIXEL_LAYOUT_I400) {
        s->nPlanes = 1;
        s->srcCW = 0; s->srcCH = 0;
        s->dstCW = 0; s->dstCH = 0;
    } else {
        s->nPlanes = 3;
        if (layout == LDV_PIXEL_LAYOUT_I444) {
            s->srcCW = srcW;   s->srcCH = srcH;
            s->dstCW = dstW;   s->dstCH = dstH;
        } else {                            /* I420 / I422：水平都减半 */
            s->srcCW = (srcW + 1) / 2;
            s->dstCW = (dstW + 1) / 2;
            s->srcCH = (layout == LDV_PIXEL_LAYOUT_I420) ? (srcH + 1) / 2 : srcH;
            s->dstCH = (layout == LDV_PIXEL_LAYOUT_I420) ? (dstH + 1) / 2 : dstH;
        }
    }

    /* ---- 目标缓冲布局 ---- */
    s->stride[0] = ldv_ds_align((size_t)dstW);
    s->stride[1] = s->nPlanes > 1 ? ldv_ds_align((size_t)s->dstCW) : 0;
    s->stride[2] = s->stride[1];

    s->offset[0] = 0;
    s->offset[1] = ldv_ds_align(s->stride[0] * (size_t)dstH);
    s->offset[2] = s->offset[1] + s->stride[1] * (size_t)s->dstCH;
    s->frameBytes = (s->nPlanes > 1)
                  ? (s->offset[2] + s->stride[2] * (size_t)s->dstCH)
                  : (s->stride[0] * (size_t)dstH);

    /* ---- 权重表 ---- */
    if (axis_build(&s->hxLuma, srcW, dstW) != 0) goto fail;
    if (axis_build(&s->vxLuma, srcH, dstH) != 0) goto fail;
    if (s->nPlanes > 1) {
        if (axis_build(&s->hxChroma, s->srcCW, s->dstCW) != 0) goto fail;
        if (axis_build(&s->vxChroma, s->srcCH, s->dstCH) != 0) goto fail;
    }

    /* ---- 水平趟的中间平面：Y 那趟最大，色度可以复用它 ---- */
    s->scratchBytes = s->stride[0] * (size_t)srcH;
    s->scratch = (uint8_t *)malloc(s->scratchBytes);
    if (!s->scratch) goto fail;

    return s;

fail:
    LDVDownscalerDestroy(&s);
    return NULL;
}

void LDVDownscalerDestroy(LDVDownscaler **s) {
    LDVDownscaler *p;
    if (!s || !*s) return;
    p = *s;
    axis_free(&p->hxLuma);
    axis_free(&p->vxLuma);
    axis_free(&p->hxChroma);
    axis_free(&p->vxChroma);
    free(p->scratch);
    free(p);
    *s = NULL;
}

int LDVDownscalerDstWidth(const LDVDownscaler *s)  { return s ? s->dstW : 0; }
int LDVDownscalerDstHeight(const LDVDownscaler *s) { return s ? s->dstH : 0; }

size_t LDVDownscalerPlaneStride(const LDVDownscaler *s, int plane) {
    if (!s || plane < 0 || plane > 2) return 0;
    return s->stride[plane];
}

size_t LDVDownscalerPlaneOffset(const LDVDownscaler *s, int plane) {
    if (!s || plane < 0 || plane > 2) return 0;
    return s->offset[plane];
}

size_t LDVDownscalerFrameBytes(const LDVDownscaler *s) {
    return s ? s->frameBytes : 0;
}

/* ------------------------------------------------------------------------ */
/* 执行                                                                      */
/* ------------------------------------------------------------------------ */

static LDVResult ldv_ds_plane(LDVDownscaler *s, int plane,
                              const uint8_t *srcPlane, ptrdiff_t srcStride,
                              uint8_t *dstPlane, size_t dstStride) {
    int              srcLen, srcRows, dstLen, dstRows;
    const LDVResampleAxis *hx, *vx;

    ldv_ds_src_dims(s, plane, &srcLen, &srcRows);
    ldv_ds_plane_dims(s, plane, &dstLen, &dstRows);
    if (srcLen <= 0 || srcRows <= 0 || dstLen <= 0 || dstRows <= 0) return LDV_OK;

    hx = (plane == 0) ? &s->hxLuma : &s->hxChroma;
    vx = (plane == 0) ? &s->vxLuma : &s->vxChroma;

    /* 尺寸没变就直拷（省一趟无意义的乘加） */
    if (srcLen == dstLen && srcRows == dstRows) {
        int y;
        for (y = 0; y < srcRows; y++) {
            memcpy(dstPlane + (ptrdiff_t)y * (ptrdiff_t)dstStride,
                   srcPlane + (ptrdiff_t)y * srcStride,
                   (size_t)dstLen);
        }
        return LDV_OK;
    }

    /* 水平趟 → scratch */
    axis_run_h(hx, srcPlane, srcStride, srcRows, dstLen,
               s->scratch, (ptrdiff_t)s->stride[0]);

    /* 垂直趟 → 目标平面 */
    axis_run_v(vx, s->scratch, (ptrdiff_t)s->stride[0], dstRows, dstLen,
               dstPlane, (ptrdiff_t)dstStride);

    return LDV_OK;
}

LDVResult LDVDownscalerRun(LDVDownscaler *s, const LDVFrameView *src,
                           uint8_t *dstPlane[3]) {
    if (!s || !src || !dstPlane || !dstPlane[0]) return LDV_ERR_INVALID_ARG;

    /*
     * 只支持 8bit。高位深素材在这个 app 里不存在（AV1 profile 0），
     * 明确拒绝比写一条从没跑过的路径好 —— 调用方收到
     * LDV_ERR_UNSUPPORTED 后会保持原尺寸，行为是可预期的。
     */
    if (src->bpc != 8) return LDV_ERR_UNSUPPORTED;
    if (src->layout != s->layout) return LDV_ERR_INVALID_ARG;
    if (src->width != s->srcW || src->height != s->srcH) {
        return LDV_ERR_INVALID_ARG;
    }

    {
        LDVResult r = ldv_ds_plane(s, 0,
                                   src->plane[0], src->stride[0],
                                   dstPlane[0], s->stride[0]);
        if (r != LDV_OK) return r;
    }

    if (s->nPlanes > 1) {
        if (!src->plane[1] || !src->plane[2]) return LDV_ERR_INVALID_ARG;
        if (!dstPlane[1] || !dstPlane[2])     return LDV_ERR_INVALID_ARG;

        {
            LDVResult r = ldv_ds_plane(s, 1,
                                       src->plane[1], src->stride[1],
                                       dstPlane[1], s->stride[1]);
            if (r != LDV_OK) return r;
        }
        {
            LDVResult r = ldv_ds_plane(s, 2,
                                       src->plane[2], src->stride[1],
                                       dstPlane[2], s->stride[2]);
            if (r != LDV_OK) return r;
        }
    }

    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 尺寸换算                                                                  */
/* ------------------------------------------------------------------------ */

void LDVDownscaleTargetSize(int srcW, int srcH, int targetW,
                            int *outW, int *outH) {
    int w, h;

    if (!outW || !outH) return;

    /* 不需要缩：原样返回 */
    if (targetW <= 0 || targetW >= srcW || srcW <= 0 || srcH <= 0) {
        *outW = srcW;
        *outH = srcH;
        return;
    }

    w = targetW;
    h = (int)(((double)srcH * (double)w / (double)srcW) + 0.5);

    /* 偶数对齐（I420 色度平面是半尺寸，奇数会错位；见头文件说明） */
    w &= ~1;
    h &= ~1;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (w > srcW) w = srcW & ~1;
    if (h > srcH) h = srcH & ~1;

    *outW = w;
    *outH = h;
}
