/*
 * LDVMP4Source.c — 最小 MP4 / ISOBMFF 解封装器（只取 AV1 视频轨）
 *
 * C99，无外部依赖（libc + stdio）。设计取舍见 LDVMP4Source.h 的注释。
 *
 * 解析策略
 * --------
 *   1. 顺序走顶层 box，定位 moov（允许它在文件末尾 —— 实测素材就是
 *      ftyp + mdat + moov 的布局，moov 在最后）
 *   2. 把 moov 整个读进内存（几十 KB），之后所有解析都在内存里做，
 *      避免反复 seek
 *   3. 从 stbl 里把 样本偏移/大小/时间戳/同步标志 展开成一张扁平表
 *   4. 取帧时按需 fseek + fread 单个样本
 *
 * 为什么要展开成扁平表而不是每次查 stsc：
 *   样本数近万，播放时每帧都要取偏移，展开一次省掉每帧的分段查找；
 *   代价是约 240 KB 常驻内存（每样本 24 字节），可接受。
 */

#include "LDVMP4Source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* 小工具                                                                    */
/* ------------------------------------------------------------------------ */

static uint16_t ldv_rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t ldv_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint64_t ldv_rd64(const uint8_t *p) {
    return ((uint64_t)ldv_rd32(p) << 32) | (uint64_t)ldv_rd32(p + 4);
}

/* 把 4 字符 box 类型转成可打印字符串 */
static void ldv_box_type(const uint8_t *p, char out[5]) {
    int i;
    for (i = 0; i < 4; i++) {
        uint8_t c = p[i];
        out[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[4] = '\0';
}

/* ------------------------------------------------------------------------ */
/* box 遍历                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *buf;
    size_t         pos;
    size_t         end;
} LDVBoxIter;

static void ldv_iter_init(LDVBoxIter *it, const uint8_t *buf,
                          size_t off, size_t end) {
    it->buf = buf;
    it->pos = off;
    it->end = end;
}

/*
 * 取下一个 box。
 * 成功时 *type 是 4 字符类型，*body 是内容起始偏移，*bend 是内容结束偏移。
 * 返回 1 表示取到，0 表示遍历结束。
 *
 * 支持 32 位 size、64 位 size（size==1）、以及"到末尾"（size==0）。
 */
static int ldv_iter_next(LDVBoxIter *it, char type[5],
                         size_t *body, size_t *bend) {
    size_t off = it->pos;
    uint64_t size;
    size_t hdr = 8;

    if (off + 8 > it->end) return 0;

    size = ldv_rd32(it->buf + off);
    if (size == 1) {
        if (off + 16 > it->end) return 0;
        size = ldv_rd64(it->buf + off + 8);
        hdr = 16;
    } else if (size == 0) {
        size = (uint64_t)(it->end - off);
    }

    if (size < hdr || (uint64_t)off + size > (uint64_t)it->end) return 0;

    ldv_box_type(it->buf + off + 4, type);
    *body = off + hdr;
    *bend = off + (size_t)size;
    it->pos = off + (size_t)size;
    return 1;
}

/* 在 [off,end) 里找第一个指定类型的 box。
 * 找到返回 1，并给出内容区间。 */
static int ldv_find_box(const uint8_t *buf, size_t off, size_t end,
                        const char *want, size_t *body, size_t *bend) {
    LDVBoxIter it;
    char type[5];

    ldv_iter_init(&it, buf, off, end);
    while (ldv_iter_next(&it, type, body, bend)) {
        if (strcmp(type, want) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 结构                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t offset;   /* 文件内绝对偏移 */
    uint32_t size;     /* 样本字节数 */
    int64_t  pts;      /* 显示时间戳，timescale 单位 */
    uint8_t  sync;     /* 1 = 随机访问点 */
} LDVSampleEntry;

/*
 * 一条轨道解析出来的全部东西。
 *
 * ★ 视频与音频各持一份，**完全独立 —— 包括各自的 mdhd timescale**。
 *   这是硬要求：音视频的 timescale 通常不同（实测素材视频 15360、
 *   音频 44100），共用一个时间基会让其中一边的时间戳换算整体错掉，
 *   而且错得隐蔽 —— 画面和声音各自看着都正常，只是彼此对不上。
 */
typedef struct {
    int             present;        /* 1 = 找到了这条轨 */

    char            codec[8];       /* "av01" / "mp4a" */
    uint32_t        timescale;      /* mdhd 的时间基 */
    int64_t         duration;       /* mdhd 的 duration（timescale 单位） */
    int64_t         sampleCount;
    size_t          maxSampleSize;
    LDVSampleEntry *samples;        /* 扁平样本表 */

    /* ---- 视频轨专用 ---- */
    int             width;
    int             height;
    uint8_t        *configOBUs;     /* av1C 的 configOBUs（含序列头 OBU） */
    size_t          configOBUsSize;

    /* ---- 音频轨专用 ---- */
    int             channels;
    int             bitsPerSample;
    uint32_t        sampleRate;     /* 采样率 Hz */
    uint8_t        *audioConfig;    /* esds 的 AudioSpecificConfig（AAC 魔数） */
    size_t          audioConfigSize;
} LDVMP4Track;

struct LDVMP4Source {
    FILE          *fp;
    int64_t        fileSize;

    LDVMP4Track    video;
    LDVMP4Track    audio;          /* present = 0 表示容器里没有音轨 */
};

/* ------------------------------------------------------------------------ */
/* stbl 解析                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t firstChunk;
    uint32_t samplesPerChunk;
} LDVStscEntry;

typedef struct {
    uint32_t count;
    uint32_t delta;
} LDVSttsEntry;

/*
 * stts 缺失或不足时估算样本间隔。
 *
 * 原实现硬编码 `timescale / 60` —— 那等于假设所有素材都是 60fps。
 * 换成 30fps 的素材（实测新素材就是 30fps）时间戳会整体偏一倍，
 * 于是"帧序号 / 帧率"的显示节拍跟着偏一倍。
 * 改成优先用 mdhd 的 duration 均分；再不行才退回 timescale/30。
 */
static int64_t ldv_estimate_sample_step(const LDVMP4Track *trk,
                                        uint32_t sampleCount) {
    if (trk->duration > 0 && sampleCount > 0) {
        int64_t step = trk->duration / (int64_t)sampleCount;
        if (step > 0) return step;
    }
    if (trk->timescale) {
        int64_t step = (int64_t)(trk->timescale / 30);
        if (step > 0) return step;
    }
    return 1;
}


/*
 * 解析 stbl，填充 trk->samples / trk->sampleCount / trk->maxSampleSize。
 * 返回 LDV_OK / LDV_ERR_BITSTREAM / LDV_ERR_NOMEM。
 */
static LDVResult ldv_parse_stbl(LDVMP4Track *trk,
                                const uint8_t *buf, size_t off, size_t end) {
    size_t b, e;
    uint32_t sampleCount = 0, sampleSizeConst = 0;
    const uint8_t *stszBase = NULL;
    uint64_t *chunkOffsets = NULL;
    LDVStscEntry *stsc = NULL;
    LDVSttsEntry *stts = NULL;
    uint32_t chunkCount = 0, stscCount = 0, sttsCount = 0;
    int is64 = 0;
    uint32_t i, si;
    LDVResult rc = LDV_OK;

    /* ---- stsz：每个样本的字节数 ---- */
    if (!ldv_find_box(buf, off, end, "stsz", &b, &e)) {
        fprintf(stderr, "LDVMP4: stbl 里没有 stsz\n");
        return LDV_ERR_BITSTREAM;
    }
    sampleSizeConst = ldv_rd32(buf + b + 4);
    sampleCount     = ldv_rd32(buf + b + 8);
    stszBase        = buf + b + 12;

    if (sampleCount == 0) {
        fprintf(stderr, "LDVMP4: stsz 样本数为 0\n");
        return LDV_ERR_BITSTREAM;
    }
    if (sampleSizeConst == 0) {
        size_t need = (size_t)sampleCount * 4;
        if (b + 12 + need > e) {
            fprintf(stderr, "LDVMP4: stsz 表长度不足\n");
            return LDV_ERR_BITSTREAM;
        }
    }

    /* ---- stco / co64：每个 chunk 的文件偏移 ---- */
    if (ldv_find_box(buf, off, end, "stco", &b, &e)) {
        is64 = 0;
    } else if (ldv_find_box(buf, off, end, "co64", &b, &e)) {
        is64 = 1;
    } else {
        fprintf(stderr, "LDVMP4: stbl 里既没有 stco 也没有 co64\n");
        return LDV_ERR_BITSTREAM;
    }
    chunkCount = ldv_rd32(buf + b + 4);
    if (chunkCount == 0) {
        fprintf(stderr, "LDVMP4: chunk 数为 0\n");
        return LDV_ERR_BITSTREAM;
    }
    {
        size_t need = (size_t)chunkCount * (is64 ? 8 : 4);
        if (b + 8 + need > e) {
            fprintf(stderr, "LDVMP4: stco/co64 表长度不足\n");
            return LDV_ERR_BITSTREAM;
        }
    }
    chunkOffsets = (uint64_t *)malloc((size_t)chunkCount * sizeof(uint64_t));
    if (!chunkOffsets) return LDV_ERR_NOMEM;
    for (i = 0; i < chunkCount; i++) {
        chunkOffsets[i] = is64 ? ldv_rd64(buf + b + 8 + (size_t)i * 8)
                               : (uint64_t)ldv_rd32(buf + b + 8 + (size_t)i * 4);
    }

    /* ---- stsc：chunk 与样本的对应关系 ---- */
    if (!ldv_find_box(buf, off, end, "stsc", &b, &e)) {
        fprintf(stderr, "LDVMP4: stbl 里没有 stsc\n");
        rc = LDV_ERR_BITSTREAM;
        goto done;
    }
    stscCount = ldv_rd32(buf + b + 4);
    if (stscCount == 0) {
        fprintf(stderr, "LDVMP4: stsc 条目为 0\n");
        rc = LDV_ERR_BITSTREAM;
        goto done;
    }
    if (b + 8 + (size_t)stscCount * 12 > e) {
        fprintf(stderr, "LDVMP4: stsc 表长度不足\n");
        rc = LDV_ERR_BITSTREAM;
        goto done;
    }
    stsc = (LDVStscEntry *)malloc((size_t)stscCount * sizeof(LDVStscEntry));
    if (!stsc) { rc = LDV_ERR_NOMEM; goto done; }
    for (i = 0; i < stscCount; i++) {
        const uint8_t *p = buf + b + 8 + (size_t)i * 12;
        stsc[i].firstChunk      = ldv_rd32(p);
        stsc[i].samplesPerChunk = ldv_rd32(p + 4);
    }

    /* ---- stts：时间戳增量 ---- */
    if (ldv_find_box(buf, off, end, "stts", &b, &e)) {
        sttsCount = ldv_rd32(buf + b + 4);
        if (sttsCount && b + 8 + (size_t)sttsCount * 8 <= e) {
            stts = (LDVSttsEntry *)malloc((size_t)sttsCount * sizeof(LDVSttsEntry));
            if (!stts) { rc = LDV_ERR_NOMEM; goto done; }
            for (i = 0; i < sttsCount; i++) {
                const uint8_t *p = buf + b + 8 + (size_t)i * 8;
                stts[i].count = ldv_rd32(p);
                stts[i].delta = ldv_rd32(p + 4);
            }
        } else {
            sttsCount = 0;
        }
    }

    /* ---- 分配样本表 ---- */
    trk->samples = (LDVSampleEntry *)calloc((size_t)sampleCount,
                                            sizeof(LDVSampleEntry));
    if (!trk->samples) { rc = LDV_ERR_NOMEM; goto done; }

    /* ---- 展开 chunk → 样本偏移 ---- */
    si = 0;
    {
        uint32_t sc = 0;
        for (i = 0; i < chunkCount && si < sampleCount; i++) {
            uint32_t spc;
            uint64_t o;
            uint32_t k;

            while (sc + 1 < stscCount && stsc[sc + 1].firstChunk <= i + 1) sc++;
            spc = stsc[sc].samplesPerChunk;
            if (spc == 0) {
                fprintf(stderr, "LDVMP4: stsc 第 %u 条的 samples_per_chunk 为 0\n", sc);
                rc = LDV_ERR_BITSTREAM;
                goto done;
            }

            o = chunkOffsets[i];
            for (k = 0; k < spc && si < sampleCount; k++) {
                uint32_t sz = sampleSizeConst ? sampleSizeConst
                                              : ldv_rd32(stszBase + (size_t)si * 4);
                if (o > 0xFFFFFFFFull) {
                    fprintf(stderr, "LDVMP4: 样本偏移超过 32 位（%llu）\n",
                            (unsigned long long)o);
                    rc = LDV_ERR_UNSUPPORTED;
                    goto done;
                }
                trk->samples[si].offset = (uint32_t)o;
                trk->samples[si].size   = sz;
                if (sz > trk->maxSampleSize) trk->maxSampleSize = sz;
                o += sz;
                si++;
            }
        }
    }
    if (si != sampleCount) {
        fprintf(stderr, "LDVMP4: 只展开了 %u / %u 个样本，样本表不一致\n",
                si, sampleCount);
        rc = LDV_ERR_BITSTREAM;
        goto done;
    }

    /* ---- 时间戳 ---- */
    {
        int64_t pts = 0;
        si = 0;
        for (i = 0; i < sttsCount && si < sampleCount; i++) {
            uint32_t k;
            for (k = 0; k < stts[i].count && si < sampleCount; k++) {
                trk->samples[si].pts = pts;
                pts += stts[i].delta;
                si++;
            }
        }
        /* stts 缺失或不足时，用 timescale/60 兜底，避免时间戳全 0 */
        if (si < sampleCount && trk->timescale) {
            int64_t step = ldv_estimate_sample_step(trk, sampleCount);
            if (step <= 0) step = 1;
            for (; si < sampleCount; si++) {
                trk->samples[si].pts = pts;
                pts += step;
            }
        }
    }

    /* ---- 同步样本 ---- */
    if (ldv_find_box(buf, off, end, "stss", &b, &e)) {
        uint32_t n = ldv_rd32(buf + b + 4);
        if (b + 8 + (size_t)n * 4 <= e) {
            for (i = 0; i < n; i++) {
                uint32_t idx = ldv_rd32(buf + b + 8 + (size_t)i * 4);
                if (idx >= 1 && idx <= sampleCount) trk->samples[idx - 1].sync = 1;
            }
        }
    } else {
        /* 没有 stss = 全是随机访问点 */
        for (i = 0; i < sampleCount; i++) trk->samples[i].sync = 1;
    }

    trk->sampleCount = (int64_t)sampleCount;

done:
    free(chunkOffsets);
    free(stsc);
    free(stts);
    if (rc != LDV_OK) {
        free(trk->samples);
        trk->samples = NULL;
        trk->sampleCount = 0;
    }
    return rc;
}

/* ------------------------------------------------------------------------ */
/* moov 解析                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * 在一个 trak 里找 av01 视频样本条目，取出宽高、av1C。
 * 返回 1 表示这是我们要的视频轨。
 */
/*
 * 在描述符序列里递归找 DecoderSpecificInfo（tag 0x05）—— 也就是 AAC 解码器
 * 需要的 AudioSpecificConfig（俗称"魔数"）。
 *
 * esds 的 payload 结构（MPEG-4 Part 1 的描述符，长度是 7 位一组的可变长）：
 *
 *   version(1) + flags(3)                    ← 不是描述符，调用方先跳过
 *   ES_Descriptor                   tag=0x03
 *     ES_ID(2) + flags(1) [+ dependsOn_ES_ID(2) / URLlength(1)+URL / OCR_ES_ID(2)]
 *     DecoderConfigDescriptor       tag=0x04
 *       objectTypeIndication(1) streamType(1) bufferSize(3)
 *       maxBitrate(4) avgBitrate(4)          ← 共 13 字节固定头
 *       DecoderSpecificInfo         tag=0x05 ← ★ 就是它
 *
 * 实测素材里它是 2 字节：0x12 0x10 = AAC-LC / 44100Hz / stereo。
 * 用 AudioConverterSetProperty(kAudioConverterDecompressionMagicCookie)
 * 原样喂给 AudioToolbox 即可，不需要我们自己解释这些位域。
 */
static void ldv_walk_descriptors(LDVMP4Track *trk, const uint8_t *p, size_t size) {
    size_t i = 0;

    while (i < size) {
        uint8_t  tag = p[i++];
        uint32_t len = 0;
        int      k;

        if (i >= size) return;

        /* 可变长长度：每字节低 7 位有效，最高位表示"还有后续"，最多 4 字节 */
        for (k = 0; k < 4 && i < size; k++) {
            uint8_t b = p[i++];
            len = (len << 7) | (uint32_t)(b & 0x7F);
            if (!(b & 0x80)) break;
        }
        if (i + len > size) return;

        if (tag == 0x05) {
            if (len > 0) {
                trk->audioConfig = (uint8_t *)malloc(len);
                if (trk->audioConfig) {
                    memcpy(trk->audioConfig, p + i, len);
                    trk->audioConfigSize = len;
                }
            }
            return;
        }

        if (tag == 0x03) {                       /* ES_Descriptor */
            uint8_t flags;
            size_t  skip = 3;                    /* ES_ID(2) + flags(1) */
            if (len < 3) return;
            flags = p[i + 2];
            if (flags & 0x80) skip += 2;         /* streamDependenceFlag */
            if (flags & 0x40) skip += 1;         /* URL_Flag（这里只算长度字节） */
            if (flags & 0x20) skip += 2;         /* OCRstreamFlag */
            if (skip >= len) return;
            ldv_walk_descriptors(trk, p + i + skip, len - skip);
            return;
        }

        if (tag == 0x04) {                       /* DecoderConfigDescriptor */
            if (len < 13) return;
            ldv_walk_descriptors(trk, p + i + 13, len - 13);
            return;
        }

        i += len;
    }
}

static void ldv_parse_esds(LDVMP4Track *trk, const uint8_t *p, size_t size) {
    if (size < 5) return;
    ldv_walk_descriptors(trk, p + 4, size - 4);   /* 跳过 version + flags */
}

/*
 * 解析一条 trak，按样本条目类型分派到视频或音频。
 *
 * 返回 1 表示已填充 trk（调用方看 trk->codec 决定收不收），0 表示这条轨
 * 不是我们要的（其它编码、或结构不认识）。outStblOff/End 交回 stbl 区间，
 * 供调用方随后展开样本表。
 *
 * 为什么 stsd 与 mdhd 都在这里取：音视频轨的 timescale 通常不同
 * （实测视频 15360、音频 44100），在外面统一扫一遍会取到音频的，
 * 导致视频的时间戳换算整体错掉。
 */
static int ldv_parse_trak(LDVMP4Track *trk,
                          const uint8_t *buf, size_t off, size_t end,
                          size_t *outStblOff, size_t *outStblEnd) {
    size_t mdiaOff, mdiaEnd, minfOff, minfEnd;
    size_t stblOff, stblEnd, stsdOff, stsdEnd;
    size_t entryOff, eb, ee;
    char   type[5];
    LDVBoxIter it;

    memset(trk, 0, sizeof(*trk));

    /* mdia -> minf -> stbl -> stsd */
    if (!ldv_find_box(buf, off, end, "mdia", &mdiaOff, &mdiaEnd)) return 0;
    if (!ldv_find_box(buf, mdiaOff, mdiaEnd, "minf", &minfOff, &minfEnd)) return 0;
    if (!ldv_find_box(buf, minfOff, minfEnd, "stbl", &stblOff, &stblEnd)) return 0;
    if (!ldv_find_box(buf, stblOff, stblEnd, "stsd", &stsdOff, &stsdEnd)) return 0;

    /* stsd: version/flags(4) + entry_count(4)，之后才是样本条目 */
    entryOff = stsdOff + 8;
    if (entryOff >= stsdEnd) return 0;

    ldv_iter_init(&it, buf, entryOff, stsdEnd);
    if (!ldv_iter_next(&it, type, &eb, &ee)) return 0;

    /* ---- mdhd：本轨自己的时间基 ---- */
    {
        size_t mb, me;
        if (ldv_find_box(buf, mdiaOff, mdiaEnd, "mdhd", &mb, &me) &&
            me >= mb + 20) {
            uint8_t  ver = buf[mb];
            uint32_t ts  = (ver == 1) ? ldv_rd32(buf + mb + 20)
                                      : ldv_rd32(buf + mb + 12);
            int64_t  du  = (ver == 1) ? (int64_t)ldv_rd64(buf + mb + 24)
                                      : (int64_t)ldv_rd32(buf + mb + 16);
            if (ts) trk->timescale = ts;
            trk->duration = du;
        }
    }

    if (strcmp(type, "av01") == 0) {
        /*
         * VisualSampleEntry 固定头 78 字节（6 reserved + 2 data_reference_index
         * + 16 pre_defined/reserved 等），之后才是 av1C 等子 box。
         */
        if (eb + 78 > ee) return 0;
        trk->width  = ldv_rd16(buf + eb + 24);
        trk->height = ldv_rd16(buf + eb + 26);
        strcpy(trk->codec, "av01");

        if (ldv_find_box(buf, eb + 78, ee, "av1C", &eb, &ee)) {
            /* av1C: marker+version(1) profile/level(1) 标志(1) 保留(1) = 4 字节 */
            if (ee >= eb + 4) {
                size_t n = ee - (eb + 4);
                if (n > 0) {
                    trk->configOBUs = (uint8_t *)malloc(n);
                    if (trk->configOBUs) {
                        memcpy(trk->configOBUs, buf + eb + 4, n);
                        trk->configOBUsSize = n;
                    }
                }
            }
        }
        trk->present = 1;

    } else if (strcmp(type, "mp4a") == 0) {
        /*
         * AudioSampleEntry 固定头 28 字节：
         *   6 reserved + 2 data_reference_index                        (8)
         *   8 reserved + 2 channelcount + 2 samplesize
         *   + 2 pre_defined + 2 reserved + 4 samplerate(16.16 定点)    (20)
         * 之后才是 esds / btrt 等子 box。
         */
        if (eb + 28 > ee) return 0;
        trk->channels      = (int)ldv_rd16(buf + eb + 16);
        trk->bitsPerSample = (int)ldv_rd16(buf + eb + 18);
        trk->sampleRate    = ldv_rd32(buf + eb + 24) >> 16;   /* 16.16 -> Hz */
        strcpy(trk->codec, "mp4a");
        if (trk->sampleRate == 0) trk->sampleRate = trk->timescale;

        if (ldv_find_box(buf, eb + 28, ee, "esds", &eb, &ee)) {
            ldv_parse_esds(trk, buf + eb, ee - eb);
        }
        trk->present = 1;

    } else {
        return 0;      /* 其它编码（含未知轨）一律跳过 */
    }

    if (outStblOff) *outStblOff = stblOff;
    if (outStblEnd) *outStblEnd = stblEnd;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* 公开 API                                                                  */
/* ------------------------------------------------------------------------ */

LDVMP4Source *LDVMP4SourceOpen(const char *path) {
    LDVMP4Source *src;
    uint64_t fileSize;
    uint64_t off;
    uint64_t moovOff = 0, moovSize = 0;
    uint8_t *moov = NULL;
    int found = 0;
    size_t videoStblOff = 0, videoStblEnd = 0;   /* AV1 视频轨的 stbl 区间 */
    size_t audioStblOff = 0, audioStblEnd = 0;   /* AAC 音频轨的 stbl 区间 */
    uint32_t mvhdTimescale = 0;                  /* 容器级时间基（兜底用） */

    if (!path) return NULL;

    src = (LDVMP4Source *)calloc(1, sizeof(LDVMP4Source));
    if (!src) return NULL;

    src->fp = fopen(path, "rb");
    if (!src->fp) {
        fprintf(stderr, "LDVMP4: 打不开 %s\n", path);
        free(src);
        return NULL;
    }

    /* 文件大小 */
    if (fseeko(src->fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "LDVMP4: 无法 seek\n");
        LDVMP4SourceClose(&src);
        return NULL;
    }
    {
        off_t sz = ftello(src->fp);
        if (sz < 0) {
            LDVMP4SourceClose(&src);
            return NULL;
        }
        fileSize = (uint64_t)sz;
    }
    src->fileSize = (int64_t)fileSize;
    if (fileSize < 16) {
        fprintf(stderr, "LDVMP4: 文件太小\n");
        LDVMP4SourceClose(&src);
        return NULL;
    }

    /* ---- 顺序走顶层 box，找 moov（允许在文件末尾）---- */
    off = 0;
    while (off + 8 <= fileSize) {
        uint8_t hdr[16];
        uint64_t size;
        size_t hdrLen = 8;
        char type[5];

        if (fseeko(src->fp, (off_t)off, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 8, src->fp) != 8) break;

        size = ldv_rd32(hdr);
        if (size == 1) {
            if (off + 16 > fileSize) break;
            if (fread(hdr + 8, 1, 8, src->fp) != 8) break;
            size = ldv_rd64(hdr + 8);
            hdrLen = 16;
        } else if (size == 0) {
            size = fileSize - off;
        }
        if (size < hdrLen || off + size > fileSize) break;

        ldv_box_type(hdr + 4, type);

        if (strcmp(type, "moov") == 0) {
            moovOff  = off + hdrLen;
            moovSize = size - hdrLen;
            found = 1;
            break;                      /* moov 只可能有一个 */
        }
        off += size;
    }

    if (!found || moovSize == 0 || moovSize > 64u * 1024u * 1024u) {
        fprintf(stderr, "LDVMP4: 找不到 moov（或大小异常 %llu）\n",
                (unsigned long long)moovSize);
        LDVMP4SourceClose(&src);
        return NULL;
    }

    /* ---- 读入 moov ---- */
    moov = (uint8_t *)malloc((size_t)moovSize);
    if (!moov) {
        LDVMP4SourceClose(&src);
        return NULL;
    }
    if (fseeko(src->fp, (off_t)moovOff, SEEK_SET) != 0 ||
        fread(moov, 1, (size_t)moovSize, src->fp) != (size_t)moovSize) {
        fprintf(stderr, "LDVMP4: 读 moov 失败\n");
        free(moov);
        LDVMP4SourceClose(&src);
        return NULL;
    }

    /*
     * ---- mvhd：容器级 timescale，只作**兜底** ----
     *
     * 正常情况下每条轨的时间基来自它自己的 mdhd（ldv_parse_trak 里读），
     * mvhd 这个值用不到。只有 mdhd 缺失/为 0 的畸形容器才需要它。
     *
     * ★ 必须存到局部变量，**不能直接写进 src->video.timescale** ——
     *   ldv_parse_trak() 一进来就 `memset(trk, 0, ...)`，会把写进去的
     *   兜底值冲掉，等于兜底完全失效（而且不报错，只是 fps 算不出来）。
     *   所以留到两条轨都解析完、确认 mdhd 没给上之后，再补。
     */
    {
        size_t b, e;
        if (ldv_find_box(moov, 0, (size_t)moovSize, "mvhd", &b, &e) &&
            e >= b + 20) {
            uint8_t ver = moov[b];
            mvhdTimescale = (ver == 1) ? ldv_rd32(moov + b + 20)
                                       : ldv_rd32(moov + b + 12);
        }
    }

    /* ---- 遍历 trak：视频轨（av01）与音频轨（mp4a）各收一条 ---- */
    {
        LDVBoxIter it;
        char type[5];
        size_t b, e;

        ldv_iter_init(&it, moov, 0, (size_t)moovSize);
        while (ldv_iter_next(&it, type, &b, &e)) {
            LDVMP4Track tmp;
            size_t sOff = 0, sEnd = 0;

            if (strcmp(type, "trak") != 0) continue;

            /*
             * 每条 trak 都解析一遍，按样本条目类型分派。
             * 不能"取第一个有 stbl 的 trak" —— 有音轨的文件里音频轨
             * 也有完整的 stbl，那样会解析到音频的样本表。
             */
            if (!ldv_parse_trak(&tmp, moov, b, e, &sOff, &sEnd)) continue;

            if (strcmp(tmp.codec, "av01") == 0) {
                if (src->video.present) continue;      /* 只收第一条视频轨 */
                src->video    = tmp;
                videoStblOff  = sOff;
                videoStblEnd  = sEnd;
            } else if (strcmp(tmp.codec, "mp4a") == 0) {
                if (src->audio.present) continue;      /* 只收第一条音频轨 */
                src->audio    = tmp;
                audioStblOff  = sOff;
                audioStblEnd  = sEnd;
            }
        }

        if (!src->video.present) {
            fprintf(stderr, "LDVMP4: 没有找到 av01 视频轨\n");
            free(moov);
            LDVMP4SourceClose(&src);
            return NULL;
        }
    }

    /*
     * 媒体时间基（mdhd）已经在 ldv_parse_trak() 里、确认样本条目类型之后
     * 分别取过了。这里不再统一扫一遍 —— 音视频轨的 timescale 通常不同
     * （实测视频 15360、音频 44100），统一扫会取错。
     * 上面的 mvhd 值只是兜底（容器级时间基，正常情况下会被 mdhd 覆盖）。
     */

    /* ---- mvhd 兜底：mdhd 没给上时间基的轨，用容器级的值补 ---- */
    if (mvhdTimescale) {
        if (src->video.timescale == 0) src->video.timescale = mvhdTimescale;
        if (src->audio.present && src->audio.timescale == 0)
            src->audio.timescale = mvhdTimescale;
    }

    /* ---- 展开两条轨各自的样本表 ---- */
    {
        LDVResult rc = ldv_parse_stbl(&src->video, moov,
                                      videoStblOff, videoStblEnd);
        if (rc != LDV_OK) {
            free(moov);
            LDVMP4SourceClose(&src);
            return NULL;
        }
    }

    if (src->audio.present) {
        LDVResult rc = ldv_parse_stbl(&src->audio, moov,
                                      audioStblOff, audioStblEnd);
        if (rc != LDV_OK) {
            /*
             * 音频样本表坏了**不该让整个文件打不开**。
             * 容错优先级：宁可没有声音，也要能放画面。
             */
            fprintf(stderr, "LDVMP4: 音频轨样本表解析失败，降级为纯视频播放\n");
            free(src->audio.samples);
            free(src->audio.audioConfig);
            memset(&src->audio, 0, sizeof(src->audio));
        }
    }

    free(moov);

    fprintf(stderr,
            "LDVMP4: %s\n"
            "        视频 %s %dx%d %lld 帧 timescale=%u %.2f fps 最大样本 %zu B\n",
            path, src->video.codec, src->video.width, src->video.height,
            (long long)src->video.sampleCount, src->video.timescale,
            LDVMP4SourceFPS(src), src->video.maxSampleSize);

    if (src->audio.present) {
        fprintf(stderr,
                "        音频 %s %d Hz %d ch %d bit %lld 帧 timescale=%u "
                "%.2f s 魔数 %zu B\n",
                src->audio.codec, (int)src->audio.sampleRate, src->audio.channels,
                src->audio.bitsPerSample, (long long)src->audio.sampleCount,
                src->audio.timescale, LDVMP4SourceAudioDuration(src),
                src->audio.audioConfigSize);
    } else {
        fprintf(stderr, "        音频 无（容器里没有音频轨）\n");
    }

    return src;
}

void LDVMP4SourceClose(LDVMP4Source **src) {
    LDVMP4Source *s;
    if (!src || !*src) return;
    s = *src;

    if (s->fp) fclose(s->fp);

    /* 两条轨各自的资源都要放掉 */
    free(s->video.samples);
    free(s->video.configOBUs);
    free(s->audio.samples);
    free(s->audio.audioConfig);

    free(s);
    *src = NULL;
}

/* ------------------------------------------------------------------------ */
/* 视频轨                                                                    */
/* ------------------------------------------------------------------------ */

int LDVMP4SourceWidth(const LDVMP4Source *s)  { return s ? s->video.width  : 0; }
int LDVMP4SourceHeight(const LDVMP4Source *s) { return s ? s->video.height : 0; }

const char *LDVMP4SourceCodec(const LDVMP4Source *s) {
    return s ? s->video.codec : "";
}

uint32_t LDVMP4SourceTimescale(const LDVMP4Source *s) {
    return s ? s->video.timescale : 0;
}

int64_t LDVMP4SourceFrameCount(const LDVMP4Source *s) {
    return s ? s->video.sampleCount : 0;
}

double LDVMP4SourceFPS(const LDVMP4Source *s) {
    if (!s || !s->video.timescale || s->video.sampleCount < 2) return 0.0;
    {
        /*
         * 用整张样本表的**平均**帧间隔，而不是前两个样本的差。
         *
         * 为什么不能只看前两个样本：素材的 MP4 可能是**按字节切块**封装的，
         * 样本间隔并不恒定 —— 一个样本里可能塞了 1~3 帧，也可能只有
         * 一个 show_existing_frame 帧头。只要前两个样本恰好跨了一个
         * "2 帧样本"，算出来的帧率就会差一倍（30 而不是 60），
         * 而显示节拍是 `帧序号 / 帧率`，帧率错一倍等于整个播放速度错一倍。
         *
         * 平均间隔 = (末样本 pts - 首样本 pts) / (样本数 - 1)，
         * 对局部抖动天然免疫。
         */
        const int64_t span = s->video.samples[s->video.sampleCount - 1].pts
                           - s->video.samples[0].pts;
        if (span > 0) {
            return (double)s->video.timescale
                 * (double)(s->video.sampleCount - 1) / (double)span;
        }
        /* 退化：所有样本 pts 相同（异常容器），退回前两样本差 */
        {
            const int64_t d = s->video.samples[1].pts - s->video.samples[0].pts;
            if (d <= 0) return 0.0;
            return (double)s->video.timescale / (double)d;
        }
    }
}

size_t LDVMP4SourceMaxSampleSize(const LDVMP4Source *s) {
    return s ? s->video.maxSampleSize : 0;
}

/*
 * 视频轨首样本的 PTS（timescale 单位）。
 *
 * 音画同步要用：两条轨的**时间轴起点不一定相同**（也未必都是 0）。
 * 只有各自减去自己的起点，换算成秒之后才是同一条时间轴上的位置。
 */
int64_t LDVMP4SourceFirstPts(const LDVMP4Source *s) {
    if (!s || s->video.sampleCount <= 0) return 0;
    return s->video.samples[0].pts;
}

double LDVMP4SourceFirstPtsSeconds(const LDVMP4Source *s) {
    if (!s || !s->video.timescale) return 0.0;
    return (double)LDVMP4SourceFirstPts(s) / (double)s->video.timescale;
}

int64_t LDVMP4SourceAudioFirstPts(const LDVMP4Source *s) {
    if (!s || !s->audio.present || s->audio.sampleCount <= 0) return 0;
    return s->audio.samples[0].pts;
}

double LDVMP4SourceAudioFirstPtsSeconds(const LDVMP4Source *s) {
    if (!s || !s->audio.timescale) return 0.0;
    return (double)LDVMP4SourceAudioFirstPts(s) / (double)s->audio.timescale;
}

/*
 * 从文件里读一个样本。视频与音频共用。
 * 成功时 *outData 是 malloc 出来的字节，由调用方 free()。
 */
static LDVResult ldv_read_track_sample(LDVMP4Source *s, const LDVMP4Track *trk,
                                       int64_t index,
                                       uint8_t **outData, size_t *outSize,
                                       int64_t *outTimestamp) {
    const LDVSampleEntry *se;
    uint8_t *buf;

    *outData = NULL;
    *outSize = 0;
    if (outTimestamp) *outTimestamp = 0;

    if (index < 0 || index >= trk->sampleCount) return LDV_ERR_EOF;

    se = &trk->samples[index];
    if (se->size == 0) return LDV_ERR_BITSTREAM;

    buf = (uint8_t *)malloc(se->size);
    if (!buf) return LDV_ERR_NOMEM;

    if (fseeko(s->fp, (off_t)se->offset, SEEK_SET) != 0 ||
        fread(buf, 1, se->size, s->fp) != se->size) {
        free(buf);
        return LDV_ERR_BITSTREAM;
    }

    *outData = buf;
    *outSize = se->size;
    if (outTimestamp) *outTimestamp = se->pts;
    return LDV_OK;
}

LDVResult LDVMP4SourceReadSample(LDVMP4Source *s, int64_t index,
                                 uint8_t **outData, size_t *outSize,
                                 int64_t *outTimestamp) {
    if (!s || !outData || !outSize) return LDV_ERR_INVALID_ARG;
    return ldv_read_track_sample(s, &s->video, index,
                                 outData, outSize, outTimestamp);
}

int LDVMP4SourceIsSyncSample(const LDVMP4Source *s, int64_t index) {
    if (!s || index < 0 || index >= s->video.sampleCount) return 0;
    return s->video.samples[index].sync ? 1 : 0;
}

LDVResult LDVMP4SourceGetConfigOBUs(const LDVMP4Source *s,
                                    const uint8_t **outData, size_t *outSize) {
    if (!s || !outData || !outSize) return LDV_ERR_INVALID_ARG;
    if (!s->video.configOBUs || s->video.configOBUsSize == 0)
        return LDV_ERR_UNSUPPORTED;
    *outData = s->video.configOBUs;
    *outSize = s->video.configOBUsSize;
    return LDV_OK;
}

/* ------------------------------------------------------------------------ */
/* 音频轨                                                                    */
/* ------------------------------------------------------------------------ */

int LDVMP4SourceHasAudio(const LDVMP4Source *s) {
    return (s && s->audio.present) ? 1 : 0;
}

const char *LDVMP4SourceAudioCodec(const LDVMP4Source *s) {
    return (s && s->audio.present) ? s->audio.codec : "";
}

uint32_t LDVMP4SourceAudioTimescale(const LDVMP4Source *s) {
    return (s && s->audio.present) ? s->audio.timescale : 0;
}

int LDVMP4SourceAudioSampleRate(const LDVMP4Source *s) {
    return (s && s->audio.present) ? (int)s->audio.sampleRate : 0;
}

int LDVMP4SourceAudioChannels(const LDVMP4Source *s) {
    return (s && s->audio.present) ? s->audio.channels : 0;
}

int64_t LDVMP4SourceAudioFrameCount(const LDVMP4Source *s) {
    return (s && s->audio.present) ? s->audio.sampleCount : 0;
}

double LDVMP4SourceAudioDuration(const LDVMP4Source *s) {
    if (!s || !s->audio.present || !s->audio.timescale) return 0.0;
    return (double)s->audio.duration / (double)s->audio.timescale;
}

size_t LDVMP4SourceAudioMaxSampleSize(const LDVMP4Source *s) {
    return (s && s->audio.present) ? s->audio.maxSampleSize : 0;
}

LDVResult LDVMP4SourceReadAudioSample(LDVMP4Source *s, int64_t index,
                                      uint8_t **outData, size_t *outSize,
                                      int64_t *outTimestamp) {
    if (!s || !outData || !outSize) return LDV_ERR_INVALID_ARG;
    if (!s->audio.present) return LDV_ERR_UNSUPPORTED;
    return ldv_read_track_sample(s, &s->audio, index,
                                 outData, outSize, outTimestamp);
}

LDVResult LDVMP4SourceGetAudioConfig(const LDVMP4Source *s,
                                     const uint8_t **outData, size_t *outSize) {
    if (!s || !outData || !outSize) return LDV_ERR_INVALID_ARG;
    if (!s->audio.present || !s->audio.audioConfig ||
        s->audio.audioConfigSize == 0)
        return LDV_ERR_UNSUPPORTED;
    *outData = s->audio.audioConfig;
    *outSize = s->audio.audioConfigSize;
    return LDV_OK;
}
