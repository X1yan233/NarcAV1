/*
 * LDVFrameDumper.c — 解码结果落盘与校验实现
 */

#include "LDVFrameDumper.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* 平面尺寸与采样                                                            */
/* ------------------------------------------------------------------------ */

/*
 * 计算某个平面的可见尺寸。
 * I400 只有 Y；I420 色度水平垂直各减半；I422 只水平减半；I444 与亮度相同。
 * 尺寸用 ceil 处理奇数分辨率。
 */
static int LDVPlaneDims(const LDVFrameView *v, int plane,
                        int *outWidth, int *outHeight)
{
    int ssHor = 0;
    int ssVer = 0;

    if (plane == 0) {
        *outWidth  = v->width;
        *outHeight = v->height;
        return 1;
    }
    if (v->layout == LDV_PIXEL_LAYOUT_I400) return 0;

    if (v->layout == LDV_PIXEL_LAYOUT_I420 || v->layout == LDV_PIXEL_LAYOUT_I422) {
        ssHor = 1;
    }
    if (v->layout == LDV_PIXEL_LAYOUT_I420) {
        ssVer = 1;
    }

    *outWidth  = (v->width  + (ssHor ? 1 : 0)) >> ssHor;
    *outHeight = (v->height + (ssVer ? 1 : 0)) >> ssVer;
    return 1;
}

/* 按字节 stride 读取一个采样。dav1d 保证 plane 至少 64 字节对齐。 */
static int LDVReadSample(const uint8_t *plane, ptrdiff_t stride, int bpc,
                         int x, int y)
{
    const uint8_t *row = plane + (ptrdiff_t)y * stride;
    if (bpc > 8) {
        const uint16_t *p16 = (const uint16_t *)(const void *)row;
        return (int)p16[x];
    }
    return (int)row[x];
}

/* ------------------------------------------------------------------------ */
/* 裸 YUV 输出                                                               */
/* ------------------------------------------------------------------------ */

int LDVFrameDumperWriteYUV(FILE *fp, const LDVFrameView *view) {
    int plane;

    if (!fp || !view) return LDV_ERR_INVALID_ARG;

    for (plane = 0; plane < 3; plane++) {
        int         w = 0, h = 0;
        int         bps;
        ptrdiff_t   rowBytes;
        int         y;

        if (!LDVPlaneDims(view, plane, &w, &h)) continue;
        if (!view->plane[plane]) continue;

        bps      = (view->bpc > 8) ? 2 : 1;
        rowBytes = (ptrdiff_t)w * bps;

        for (y = 0; y < h; y++) {
            const uint8_t *row = view->plane[plane]
                               + (ptrdiff_t)y * view->stride[plane ? 1 : 0];
            if (fwrite(row, 1, (size_t)rowBytes, fp) != (size_t)rowBytes) {
                return LDV_ERR_INVALID_ARG;
            }
        }
    }
    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* PPM 输出                                                                  */
/* ------------------------------------------------------------------------ */

static uint8_t LDVClamp8(double v) {
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

static void LDVYUVToRGB(int Y, int U, int V, int fullRange,
                        LDVMatrixCoefficients m,
                        uint8_t *r, uint8_t *g, uint8_t *b)
{
    double y = (double)Y;
    double u = (double)U - 128.0;
    double v = (double)V - 128.0;
    double kr, kb, kg;

    if (!fullRange) {
        /* MPEG / video range → full range */
        y = (y - 16.0) * 255.0 / 219.0;
        u = u * 255.0 / 224.0;
        v = v * 255.0 / 224.0;
    }

    switch (m) {
    case LDV_MATRIX_BT709:      kr = 0.2126; kb = 0.0722; break;
    case LDV_MATRIX_BT2020_NCL: kr = 0.2627; kb = 0.0593; break;
    case LDV_MATRIX_SMPTE240:   kr = 0.212;  kb = 0.087;  break;
    default:                    kr = 0.299;  kb = 0.114;  break; /* BT.601 / FCC / 未知 */
    }
    kg = 1.0 - kr - kb;

    *r = LDVClamp8(y + 2.0 * (1.0 - kr) * v);
    *b = LDVClamp8(y + 2.0 * (1.0 - kb) * u);
    *g = LDVClamp8(y
                   - (2.0 * kb * (1.0 - kb) / kg) * u
                   - (2.0 * kr * (1.0 - kr) / kg) * v);
}

int LDVFrameDumperWritePPM(FILE *fp, const LDVFrameView *view) {
    int      yW = 0, yH = 0, cW = 0, cH = 0;
    int      hasChroma;
    int      shift;
    uint8_t *rowBuf;
    int      y;

    if (!fp || !view) return LDV_ERR_INVALID_ARG;

    LDVPlaneDims(view, 0, &yW, &yH);
    hasChroma = LDVPlaneDims(view, 1, &cW, &cH);

    /* 10/12bit 降到 8bit。dav1d 保证 10bit 数据在 16 位字的低 10 位。 */
    shift = (view->bpc > 8) ? (view->bpc - 8) : 0;

    rowBuf = (uint8_t *)malloc((size_t)yW * 3u);
    if (!rowBuf) return LDV_ERR_NOMEM;

    if (fprintf(fp, "P6\n%d %d\n255\n", yW, yH) < 0) {
        free(rowBuf);
        return LDV_ERR_INVALID_ARG;
    }

    for (y = 0; y < yH; y++) {
        int x;
        for (x = 0; x < yW; x++) {
            int Y = LDVReadSample(view->plane[0], view->stride[0],
                                  view->bpc, x, y) >> shift;
            int U = 128;
            int V = 128;

            if (hasChroma) {
                /* 最近邻上采样 */
                int cx = (cW > 0) ? (x * cW / (yW > 0 ? yW : 1)) : 0;
                int cy = (cH > 0) ? (y * cH / (yH > 0 ? yH : 1)) : 0;
                if (cx >= cW) cx = cW - 1;
                if (cy >= cH) cy = cH - 1;
                U = LDVReadSample(view->plane[1], view->stride[1],
                                  view->bpc, cx, cy) >> shift;
                V = LDVReadSample(view->plane[2], view->stride[1],
                                  view->bpc, cx, cy) >> shift;
            }

            LDVYUVToRGB(Y, U, V, view->colorRange, view->matrix,
                        &rowBuf[x * 3 + 0], &rowBuf[x * 3 + 1], &rowBuf[x * 3 + 2]);
        }
        if (fwrite(rowBuf, 1, (size_t)yW * 3u, fp) != (size_t)yW * 3u) {
            free(rowBuf);
            return LDV_ERR_INVALID_ARG;
        }
    }

    free(rowBuf);
    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 校验和                                                                    */
/* ------------------------------------------------------------------------ */

uint32_t LDVFrameChecksum(const LDVFrameView *view) {
    uint32_t hash = 2166136261u;   /* FNV-1a 32 位 offset basis */
    int      yW = 0, yH = 0;
    int      bps;
    int      y;

    if (!view || !view->plane[0]) return 0;
    LDVPlaneDims(view, 0, &yW, &yH);

    bps = (view->bpc > 8) ? 2 : 1;

    for (y = 0; y < yH; y++) {
        const uint8_t *row = view->plane[0] + (ptrdiff_t)y * view->stride[0];
        size_t         n   = (size_t)yW * (size_t)bps;
        size_t         i;
        for (i = 0; i < n; i++) {
            hash ^= (uint32_t)row[i];
            hash *= 16777619u;     /* FNV-1a 32 位 prime */
        }
    }
    return hash;
}

/* ------------------------------------------------------------------------ */
/* 名称                                                                      */
/* ------------------------------------------------------------------------ */

const char *LDVFrameLayoutName(LDVPixelLayout layout) {
    switch (layout) {
    case LDV_PIXEL_LAYOUT_I400: return "I400";
    case LDV_PIXEL_LAYOUT_I420: return "I420";
    case LDV_PIXEL_LAYOUT_I422: return "I422";
    case LDV_PIXEL_LAYOUT_I444: return "I444";
    default:                    return "unknown";
    }
}

const char *LDVFrameMatrixName(LDVMatrixCoefficients matrix) {
    switch (matrix) {
    case LDV_MATRIX_IDENTITY:   return "identity";
    case LDV_MATRIX_BT709:      return "BT.709";
    case LDV_MATRIX_UNKNOWN:    return "unknown";
    case LDV_MATRIX_FCC:        return "FCC";
    case LDV_MATRIX_BT470BG:    return "BT.470BG";
    case LDV_MATRIX_BT601:      return "BT.601";
    case LDV_MATRIX_SMPTE240:   return "SMPTE 240M";
    case LDV_MATRIX_YCGCO:      return "YCgCo";
    case LDV_MATRIX_BT2020_NCL: return "BT.2020 NCL";
    case LDV_MATRIX_BT2020_CL:  return "BT.2020 CL";
    case LDV_MATRIX_ICTCP:      return "ICtCp";
    default:                    return "other";
    }
}
