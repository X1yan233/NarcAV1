/*
 * LDVIVFSource.c — 最小 IVF 容器读取器实现
 */

#include "LDVIVFSource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LDV_IVF_HEADER_SIZE      32
#define LDV_IVF_FRAME_HEADER_SIZE 12
#define LDV_IVF_INITIAL_CAPACITY (1024u * 1024u)

struct LDVIVFSource {
    FILE    *fp;
    char     fourcc[5];
    int      width;
    int      height;
    uint32_t tbNumerator;
    uint32_t tbDenominator;
    int64_t  declaredFrameCount;
    int64_t  framesRead;

    uint8_t *buffer;
    size_t   capacity;
};

static uint16_t LDVReadU16LE(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t LDVReadU32LE(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t LDVReadU64LE(const uint8_t *p) {
    return (uint64_t)LDVReadU32LE(p) | ((uint64_t)LDVReadU32LE(p + 4) << 32);
}

LDVIVFSource *LDVIVFSourceOpen(const char *path) {
    LDVIVFSource *src;
    uint8_t       header[LDV_IVF_HEADER_SIZE];
    size_t        got;

    if (!path) return NULL;

    src = (LDVIVFSource *)calloc(1, sizeof(*src));
    if (!src) return NULL;

    src->fp = fopen(path, "rb");
    if (!src->fp) { free(src); return NULL; }

    got = fread(header, 1, sizeof(header), src->fp);
    if (got != sizeof(header)) {
        LDVIVFSourceClose(&src);
        return NULL;
    }

    if (memcmp(header, "DKIF", 4) != 0) {
        LDVIVFSourceClose(&src);
        return NULL;
    }
    if (LDVReadU16LE(header + 6) != LDV_IVF_HEADER_SIZE) {
        LDVIVFSourceClose(&src);
        return NULL;
    }

    memcpy(src->fourcc, header + 8, 4);
    src->fourcc[4] = '\0';

    src->width               = (int)LDVReadU16LE(header + 12);
    src->height              = (int)LDVReadU16LE(header + 14);
    src->tbDenominator       = LDVReadU32LE(header + 16);
    src->tbNumerator         = LDVReadU32LE(header + 20);
    src->declaredFrameCount  = (int64_t)LDVReadU32LE(header + 24);

    src->capacity = LDV_IVF_INITIAL_CAPACITY;
    src->buffer   = (uint8_t *)malloc(src->capacity);
    if (!src->buffer) {
        LDVIVFSourceClose(&src);
        return NULL;
    }

    return src;
}

void LDVIVFSourceClose(LDVIVFSource **src) {
    LDVIVFSource *s;
    if (!src || !*src) return;
    s = *src;
    if (s->fp)     fclose(s->fp);
    if (s->buffer) free(s->buffer);
    free(s);
    *src = NULL;
}

int LDVIVFSourceReadFrame(LDVIVFSource *src, LDVIVFFrame *out) {
    uint8_t  fh[LDV_IVF_FRAME_HEADER_SIZE];
    uint32_t frameSize;
    size_t   got;

    if (!src || !src->fp || !out) return LDV_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    got = fread(fh, 1, sizeof(fh), src->fp);
    if (got == 0)                        return LDV_ERR_EOF;
    if (got != sizeof(fh))               return LDV_ERR_BITSTREAM;

    frameSize = LDVReadU32LE(fh);
    if (frameSize == 0 || frameSize > (64u * 1024u * 1024u)) {
        return LDV_ERR_BITSTREAM;
    }

    if (frameSize > src->capacity) {
        size_t newCapacity = src->capacity;
        uint8_t *grown;
        while (newCapacity < frameSize) newCapacity *= 2;
        grown = (uint8_t *)realloc(src->buffer, newCapacity);
        if (!grown) return LDV_ERR_NOMEM;
        src->buffer   = grown;
        src->capacity = newCapacity;
    }

    got = fread(src->buffer, 1, frameSize, src->fp);
    if (got != frameSize) return LDV_ERR_BITSTREAM;

    out->data      = src->buffer;
    out->size      = frameSize;
    out->timestamp = (int64_t)LDVReadU64LE(fh + 4);

    src->framesRead++;
    return LDV_OK;
}

/* 释放一个部分填充的数组（内部使用）。 */
static void LDVIVFFreeFramesPartial(LDVIVFFrame *frames, int count) {
    int i;
    if (!frames) return;
    for (i = 0; i < count; i++) free((void *)frames[i].data);
    free(frames);
}

int LDVIVFSourceLoadAll(LDVIVFSource *src,
                        LDVIVFFrame **outFrames, int *outCount)
{
    LDVIVFFrame *frames = NULL;
    int          count  = 0;
    int          cap    = 0;

    if (!src || !outFrames || !outCount) return LDV_ERR_INVALID_ARG;

    for (;;) {
        LDVIVFFrame  f;
        int          r = LDVIVFSourceReadFrame(src, &f);
        LDVIVFFrame *grown;
        uint8_t     *copy;

        if (r == LDV_ERR_EOF) break;
        if (r != LDV_OK) { LDVIVFFreeFramesPartial(frames, count); return r; }

        if (count == cap) {
            cap = cap ? cap * 2 : 256;
            grown = (LDVIVFFrame *)realloc(frames, (size_t)cap * sizeof(*frames));
            if (!grown) { LDVIVFFreeFramesPartial(frames, count); return LDV_ERR_NOMEM; }
            frames = grown;
        }

        copy = (uint8_t *)malloc(f.size ? f.size : 1);
        if (!copy) { LDVIVFFreeFramesPartial(frames, count); return LDV_ERR_NOMEM; }
        memcpy(copy, f.data, f.size);

        frames[count].data      = copy;
        frames[count].size      = f.size;
        frames[count].timestamp = f.timestamp;
        count++;
    }

    *outFrames = frames;
    *outCount  = count;
    return LDV_OK;
}

void LDVIVFSourceFreeFrames(LDVIVFFrame **frames, int count) {
    if (!frames || !*frames) return;
    LDVIVFFreeFramesPartial(*frames, count);
    *frames = NULL;
}

int      LDVIVFSourceWidth(const LDVIVFSource *src)  { return src ? src->width  : 0; }
int      LDVIVFSourceHeight(const LDVIVFSource *src) { return src ? src->height : 0; }

int64_t LDVIVFSourceDeclaredFrameCount(const LDVIVFSource *src) {
    return src ? src->declaredFrameCount : 0;
}

void LDVIVFSourceTimebase(const LDVIVFSource *src,
                          uint32_t *outNumerator, uint32_t *outDenominator)
{
    if (outNumerator)   *outNumerator   = src ? src->tbNumerator   : 0;
    if (outDenominator) *outDenominator = src ? src->tbDenominator : 0;
}

const char *LDVIVFSourceFourCC(const LDVIVFSource *src) {
    return src ? src->fourcc : "";
}
