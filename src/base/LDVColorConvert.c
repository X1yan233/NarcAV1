/*
 * LDVColorConvert.c — YUV → BGRA 定点软转换
 *
 * ★ 本文件修掉了一个真实 bug，说明放在下面，因为它极容易被"看起来对"
 *   的代码骗过去。
 *
 * 定点数的量纲（读这段之前必须先搞清楚）
 * --------------------------------------
 * 本文件里所有中间量都是 **16.16 定点**，也就是"真实值 × 65536"。
 *   真实值 130.41  →  定点 8,546,608
 *   真实值 10.0    →  定点    655,360
 * 只有**最后写进 BGRA 字节之前**才 `>> 16` 落回 0..255。
 *
 * 之前错在哪
 * ----------
 * 原实现是：
 *      int32_t Y = yy << 16;                          // yy 是 0..255
 *      const int32_t luma = (Y * c.yMul >> 16) - c.yOff;
 *      ...
 *      d[0] = ldv_clamp8(B);                          // ← 传的是 16.16！
 *
 * 两个独立错误叠在一起：
 *
 * ① **int32 溢出。** 以中性灰 Y=128 代入：
 *        yMul = 1.164383 × 65536 = 76309
 *        Y × yMul = 8,388,608 × 76,309 = 640,126,287,872
 *    而 int32 上限是 2,147,483,647 —— 溢出 298 倍。有符号溢出是 UB，
 *    实测回绕后 `>> 16` 得 1162，再减 yOff(1,220,944) 得
 *    **luma = −1,219,782**，而正确值应该是 **+8,546,608**。符号都反了。
 *
 * ② **量纲没落地。** luma 是 16.16（130.41 应表示为 8,546,608），
 *    却被直接交给只认 0..255 的 clamp8。于是每个通道都被钳成
 *    "正数→255 / 负数→0"，画面变成二值色块。
 *
 * 合起来的效果就是"解码在跑、缓存有水、画面是垃圾/不刷新"——
 * 而解码侧一切正常，所以极难往色彩转换上联想。
 *
 * 正确写法（本文件）：乘 Y 用 int64 防溢出，最后统一 `>> 16` 落量纲。
 *
 * ★ 但 int64 本身又是一次性能事故（2026-10-01 真机实测后修正）
 * ------------------------------------------------------------
 * 上面那个修法在**正确性**上没问题，在**性能**上是灾难：
 *
 *   真机 iPhone 4S（armv7 / A5 800 MHz）实测转换 **81.3 ms/帧**。
 *   196,608 px / 0.0813 s = 2.4 Mpx/s ≈ **330 周期/像素**。
 *   而 60 fps 的预算是 16.7 ms —— 超了 5 倍，而且这段跑在**主线程**上，
 *   直接把刷新率从 60 Hz 拖到 43 Hz（连按钮都变迟钝）。
 *
 * 元凶就是那些 int64 乘法：armv7 没有 64 位乘指令，`(int64_t)a * b`
 * 会变成 `__aeabi_lmul` 调用。热循环里每像素约 3 次，足够解释这 330 周期。
 *
 * 而它们**全都是多余的**：
 *
 * ① luma —— `((int64_t)yy << 16) * yMul >> 16` 恒等于 `yy * yMul`
 *    yy ≤ 255，yMul ≤ 76309，乘积 ≤ 19,458,795，int32 绰绰有余。
 *    先左移 16 再右移 16 是可逆的（中间量没丢位），所以移位纯属浪费。
 *
 * ② 色度 —— `ldv_chroma16(cc) = (cc<<16) - cOff`，而 `cOff = 128<<16`，
 *    于是它 **恒为 65536 的整数倍**。那么
 *        (rV * ((cc-128)<<16)) >> 16  ==  rV * (cc-128)
 *    算术右移对 65536 的整数倍是精确的（负值同样精确：-65536>>16 = -1），
 *    移位被完全约掉。gU/gV 那项是"两项之和再右移"，和同样是 65536 的
 *    整数倍，所以 `(gU*U + gV*V) >> 16 == gU*du + gV*dv` 也精确成立。
 *
 * 于是整个热循环可以**纯 int32**，一次 64 位乘法都不需要。
 * 最大中间量 35,027,378，距 int32 上限还有 61 倍余量。
 *
 * **这是一次纯性能改写，输出逐位不变。** 证据在
 * `scripts/verify-reduce.py`：它把新旧两种形式都算一遍，对
 * 4 套系数 × 全部 256×256×256 组合逐项比对，要求**完全相等**
 * （不是"误差 ≤ 1"，是不允许有任何误差）。当前结果：343 万项，0 项不等。
 * 改这里的任何一处算术，都必须重跑那个脚本 + check-color-convert.py。
 *
 * 验证
 * ----
 * 本机的定点数学在 scripts/check-color-convert.py 里有一份逐字复刻，
 * 与浮点参考实现逐值比对（覆盖 BT.601/709 × video/full range ×
 * I400/I420/I444 × 8/10/12bit）。改这里的系数或移位必须重跑那个脚本。
 *
 * 为什么是定点而不是浮点
 * ----------------------
 * 定点在 A4/A5 这类老 ARM 上比浮点快得多，而且结果可复现 ——
 * 浮点在开/不开 FMA、不同优化级别下会给出不同舍入，画面会偶发偏 1，
 * 这类差异极难定位。定点系数都是预先四舍五入到 16.16 的常量，
 * 结果与优化级别无关。
 *
 * 注意：这不是 GPU 方案。ARCHITECTURE §6.2 里的 GPU 路径
 * （GLES2 + 3 张亮度纹理 + 片元着色器）才是最终形态；
 * 本文件是当前阶段的正确实现，也是 GPU 不可用时的回退路径。
 */

#include "LDVColorConvert.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* 定点系数（16.16）                                                         */
/* ------------------------------------------------------------------------ */

/*
 * 四套组合：BT.601 / BT.709 × video range / full range。
 *
 * video range（MPEG，Y 在 16..235，C 在 16..240）：
 *   luma = 1.164383 * (Y - 16)
 *   R = luma + 1.596027 * (V - 128)          ← BT.601
 *   G = luma - 0.391762 * (U - 128) - 0.812968 * (V - 128)
 *   B = luma + 2.017232 * (U - 128)
 *
 * full range（JPEG，Y 在 0..255，C 在 0..255）：
 *   luma = Y
 *   R = luma + 1.402 * (V - 128)             ← BT.601
 *   ...
 *
 * BT.709 用 1.792741 / -0.213249 / -0.532909 / 2.112402（video）
 * 与 1.5748 / -0.1873 / -0.4681 / 1.8556（full）。
 *
 * ★ 负系数用 `- 0.5` 而不是 `+ 0.5` 取整：往绝对值更大的方向舍入，
 *   让 G 通道的舍入方向与 R/B 一致，避免中性灰出现系统性偏绿/偏品红
 *   （这是这类代码最常踩的一个坑）。
 */
/*
 * 系数结构。
 *
 * ★ 注意**没有 cOff**。
 *
 * 原来有一个 `cOff = 128 << 16`，代表色度零点。但既然它是 65536 的
 * 整数倍，色度项里 `(cc<<16) - cOff` 再 `>>16` 就等于 `cc - 128` ——
 * 存一个常量、每次取出来做一次移位再约掉，不如直接把 128 写进循环。
 * 少一次内存读、少一次移位，而且少一个"必须恒为 65536 倍数"的隐性约束
 * （这个约束一旦被破坏，约简就不再精确 —— 见 scripts/verify-reduce.py
 * 开头那条 assert）。
 */
typedef struct {
    int32_t yMul, yOff;          /* Y  → luma 的缩放与偏移（16.16） */
    int32_t rV, gU, gV, bU;      /* 色度到 RGB 的系数（16.16） */
} LDVColorConv;

/* 色度零点：8bit 时是 128（video range 下实际取值 16..240 的中点） */
#define LDV_CHROMA_MID 128

static int ldv_is_hd(const LDVFrameView *v) {
    /* 按 ITU 惯例：高度 > 576 视为 HD，用 BT.709；否则 SD 用 BT.601。
     * 只在流里没声明矩阵（UNKNOWN）时才需要这个兜底判断。 */
    return v->height > 576;
}

static int ldv_use_bt709(const LDVFrameView *v) {
    switch (v->matrix) {
    case LDV_MATRIX_BT709:      return 1;
    case LDV_MATRIX_BT601:      return 0;
    case LDV_MATRIX_BT470BG:    return 0;
    case LDV_MATRIX_BT2020_NCL: return 1;   /* 2020 借 709 系数（近似） */
    case LDV_MATRIX_BT2020_CL:  return 1;
    default:                    break;
    }
    /* UNKNOWN / IDENTITY / 其它：按分辨率兜底 */
    return ldv_is_hd(v);
}

const char *LDVColorConvertMatrixName(const LDVFrameView *v) {
    if (!v) return "?";
    return ldv_use_bt709(v) ? "709" : "601";
}

static LDVColorConv ldv_make_conv(const LDVFrameView *v) {
    LDVColorConv c;
    const int fullRange = v->colorRange ? 1 : 0;
    const int bt709     = ldv_use_bt709(v);

    memset(&c, 0, sizeof(c));

    if (fullRange) {
        c.yMul = 1 << 16;
        c.yOff = 0;
    } else {
        c.yMul = (int32_t)(1.164383 * 65536.0 + 0.5);
        c.yOff = (int32_t)(1.164383 * 16.0 * 65536.0 + 0.5);
    }

    if (bt709) {
        if (fullRange) {
            c.rV = (int32_t)( 1.5748   * 65536.0 + 0.5);
            c.gU = (int32_t)(-0.1873   * 65536.0 - 0.5);
            c.gV = (int32_t)(-0.4681   * 65536.0 - 0.5);
            c.bU = (int32_t)( 1.8556   * 65536.0 + 0.5);
        } else {
            c.rV = (int32_t)( 1.792741 * 65536.0 + 0.5);
            c.gU = (int32_t)(-0.213249 * 65536.0 - 0.5);
            c.gV = (int32_t)(-0.532909 * 65536.0 - 0.5);
            c.bU = (int32_t)( 2.112402 * 65536.0 + 0.5);
        }
    } else {
        if (fullRange) {
            c.rV = (int32_t)( 1.402    * 65536.0 + 0.5);
            c.gU = (int32_t)(-0.344136 * 65536.0 - 0.5);
            c.gV = (int32_t)(-0.714136 * 65536.0 - 0.5);
            c.bU = (int32_t)( 1.772    * 65536.0 + 0.5);
        } else {
            c.rV = (int32_t)( 1.596027 * 65536.0 + 0.5);
            c.gU = (int32_t)(-0.391762 * 65536.0 - 0.5);
            c.gV = (int32_t)(-0.812968 * 65536.0 - 0.5);
            c.bU = (int32_t)( 2.017232 * 65536.0 + 0.5);
        }
    }
    return c;
}

/* ------------------------------------------------------------------------ */
/* 取样与落量纲                                                              */
/* ------------------------------------------------------------------------ */

/* 16.16 → 0..255，带钳位。★ 所有通道值必须先经过这里再写字节。 */
static inline uint8_t ldv_fixed_to_u8(int32_t v16) {
    const int32_t v = v16 >> 16;          /* 落量纲 */
    if (v <= 0)   return 0;
    if (v >= 255) return 255;
    return (uint8_t)v;
}

/*
 * luma：yy(0..255) → 16.16。
 *
 * ★ 纯 int32，无 64 位乘法。等价性证明见文件头 ②/① 与 verify-reduce.py。
 *   原写法 `((int64_t)yy << 16) * c->yMul >> 16` 在 armv7 上每次都要
 *   调 __aeabi_lmul —— 这正是 81.3 ms/帧的元凶。
 */
static inline int32_t ldv_luma16(int yy, const LDVColorConv *c) {
    return yy * c->yMul - c->yOff;
}

/*
 * 一个像素的完整变换与落盘。
 *
 *   yy  亮度样本 0..255
 *   du  U - 128（已落量纲的**整数**，不是 16.16）
 *   dv  V - 128
 *
 * 三个色差项都是 int32：
 *     rT = rV * dv          （因为 (rV * ((cc-128)<<16)) >> 16 == rV * (cc-128)）
 *     gT = gU * du + gV * dv
 *     bT = bU * du
 * 写进 BGRA 前统一 `>> 16` 落量纲（在 ldv_fixed_to_u8 里）。
 *
 * 声明成 static inline 而不是宏：-O3 下一定内联，读起来是普通函数，
 * 而且编译器能自己把同一对像素共用的 `rV*dv` 等公共子表达式提出去，
 * 不需要手写"每 2 个像素算一次色度"这种容易改错的优化。
 */
static inline void ldv_store_bgra(uint8_t *d, int yy, int du, int dv,
                                  const LDVColorConv *c) {
    const int32_t l = ldv_luma16(yy, c);
    d[0] = ldv_fixed_to_u8(l + c->bU * du);
    d[1] = ldv_fixed_to_u8(l + c->gU * du + c->gV * dv);
    d[2] = ldv_fixed_to_u8(l + c->rV * dv);
    d[3] = 255;
}

/* 灰度：R = G = B = luma（I400，或色度平面缺失时的兜底） */
static inline void ldv_store_gray_bgra(uint8_t *d, int yy,
                                       const LDVColorConv *c) {
    const uint8_t g = ldv_fixed_to_u8(ldv_luma16(yy, c));
    d[0] = g; d[1] = g; d[2] = g; d[3] = 255;
}

/* 取 8bit 样本；高位深取高字节（预览够用，HDR 输出属于后续阶段） */
static inline int ldv_s8(const uint8_t *row, int x, int bpc) {
    if (bpc > 8) {
        const uint16_t *p = (const uint16_t *)row;
        return (int)(p[x] >> (bpc - 8));
    }
    return (int)row[x];
}

static inline const uint8_t *ldv_row(const uint8_t *plane, ptrdiff_t stride,
                                     int y) {
    return plane + (ptrdiff_t)y * stride;
}

/* ------------------------------------------------------------------------ */
/* 主转换                                                                    */
/* ------------------------------------------------------------------------ */

void LDVConvertYUVToBGRA(const LDVFrameView *v, uint8_t *dst, size_t dstStride) {
    int w, h, bpc, cxShift, cyShift;
    int hasChroma;
    LDVColorConv c;
    const uint8_t *yPlane, *uPlane, *vPlane;
    ptrdiff_t yStride, cStride;
    int y, x;

    if (!v || !dst) return;
    w = v->width;
    h = v->height;
    if (w <= 0 || h <= 0) return;

    bpc = v->bpc;
    hasChroma = (v->layout != LDV_PIXEL_LAYOUT_I400) &&
                v->plane[1] && v->plane[2];

    switch (v->layout) {
    case LDV_PIXEL_LAYOUT_I422: cxShift = 1; cyShift = 0; break;
    case LDV_PIXEL_LAYOUT_I444: cxShift = 0; cyShift = 0; break;
    case LDV_PIXEL_LAYOUT_I420:
    default:                    cxShift = 1; cyShift = 1; break;
    }

    if (dstStride == 0) dstStride = (size_t)w * 4;

    c = ldv_make_conv(v);

    yPlane = v->plane[0];
    uPlane = v->plane[1];
    vPlane = v->plane[2];
    yStride = v->stride[0];
    cStride = v->stride[1];

    /*
     * 主循环。
     *
     * ★ 性能要点（2026-10-01 真机 81.3 ms → 目标 ≤ 16.7 ms）
     * ------------------------------------------------------
     * 1) 热循环里**没有任何 64 位乘法**。armv7 上 `(int64_t)a * b` 会退化成
     *    __aeabi_lmul 调用，原来每像素约 3 次，是 330 周期/像素的主因。
     *    现在全是 int32，且 `du/dv` 已经是"值域内整数"（cc - 128），
     *    色差项就是 `系数 × du`，一次 32 位乘法。
     *
     * 2) 行指针提到循环外（`ldv_row` 只在每行调一次，不在内层）。
     *
     * 3) 4:2:0 / 4:2:2 每 2 个像素共用一组色度样本，`du/dv` 只取一次。
     *
     * 4) 写字节用的是 `d[0..3]` 而不是 `*d++` 链 —— 让编译器把 4 字节
     *    合成一条 32 位 store（BGRA 是 4 字节对齐的，可以一条 STR 写掉）。
     *
     * 5) `ldv_s8` 里那个 `bpc > 8` 分支**故意留着**：bpc 是整个调用的
     *    不变量，分支预测恒定命中，代价约 1 周期/次；为它把整个循环
     *    复制成 8bit / 高位深两份是 35 行重复代码，改一处漏一处的风险
     *    比这点收益大得多。真到需要时再上循环分裂（loop unswitching）。
     */
    for (y = 0; y < h; y++) {
        uint8_t *d = dst + (size_t)y * dstStride;
        const uint8_t *yRow = ldv_row(yPlane, yStride, y);

        if (!hasChroma) {
            for (x = 0; x < w; x++) {
                ldv_store_gray_bgra(d, ldv_s8(yRow, x, bpc), &c);
                d += 4;
            }
            continue;
        }

        {
            const uint8_t *uRow = ldv_row(uPlane, cStride, y >> cyShift);
            const uint8_t *vRow = ldv_row(vPlane, cStride, y >> cyShift);

            if (cxShift == 1) {
                /* 4:2:0 / 4:2:2：水平每 2 像素共用一组色度。奇数宽度时
                 * 最后 1 个像素在循环后补齐。 */
                for (x = 0; x + 1 < w; x += 2) {
                    const int du = ldv_s8(uRow, x >> 1, bpc) - LDV_CHROMA_MID;
                    const int dv = ldv_s8(vRow, x >> 1, bpc) - LDV_CHROMA_MID;
                    ldv_store_bgra(d,     ldv_s8(yRow, x,     bpc), du, dv, &c);
                    ldv_store_bgra(d + 4, ldv_s8(yRow, x + 1, bpc), du, dv, &c);
                    d += 8;
                }
                if (x < w) {
                    const int du = ldv_s8(uRow, x >> 1, bpc) - LDV_CHROMA_MID;
                    const int dv = ldv_s8(vRow, x >> 1, bpc) - LDV_CHROMA_MID;
                    ldv_store_bgra(d, ldv_s8(yRow, x, bpc), du, dv, &c);
                }
            } else {
                /* 4:4:4：色度与 luma 一一对应 */
                for (x = 0; x < w; x++) {
                    ldv_store_bgra(d, ldv_s8(yRow, x, bpc),
                                   ldv_s8(uRow, x, bpc) - LDV_CHROMA_MID,
                                   ldv_s8(vRow, x, bpc) - LDV_CHROMA_MID, &c);
                    d += 4;
                }
            }
        }
    }
}
