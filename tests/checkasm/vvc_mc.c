/*
 * Copyright (c) 2023-2024 Nuo Mi
 * Copyright (c) 2023-2024 Wu Jianhua
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <string.h>

#include "checkasm.h"
#include "libavcodec/vvc/ctu.h"
#include "libavcodec/vvc/data.h"
#include "libavcodec/vvc/dsp.h"

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"

static const uint32_t pixel_mask[] = { 0xffffffff, 0x03ff03ff, 0x0fff0fff, 0x3fff3fff, 0xffffffff };
static const int sizes[] = { 2, 4, 8, 16, 32, 64, 128 };

#define SIZEOF_PIXEL ((bit_depth + 7) / 8)
#define PIXEL_STRIDE (MAX_CTU_SIZE * 2)
#define EXTRA_BEFORE 3
#define EXTRA_AFTER  4
#define SRC_EXTRA    (EXTRA_BEFORE + EXTRA_AFTER) * 2
#define SRC_BUF_SIZE (PIXEL_STRIDE + SRC_EXTRA) * (PIXEL_STRIDE + SRC_EXTRA)
#define DST_BUF_SIZE (MAX_CTU_SIZE * MAX_CTU_SIZE * 2)
#define SRC_OFFSET   ((PIXEL_STRIDE + EXTRA_BEFORE * 2) * EXTRA_BEFORE)

#define randomize_buffers(buf0, buf1, size, mask)           \
    do {                                                    \
        int k;                                              \
        for (k = 0; k < size; k += 4 / sizeof(*buf0)) {     \
            uint32_t r = rnd() & mask;                      \
            AV_WN32A(buf0 + k, r);                          \
            AV_WN32A(buf1 + k, r);                          \
        }                                                   \
    } while (0)

#define randomize_pixels(buf0, buf1, size)                  \
    do {                                                    \
        uint32_t mask = pixel_mask[(bit_depth - 8) >> 1];   \
        randomize_buffers(buf0, buf1, size, mask);          \
    } while (0)

#define randomize_avg_src(buf0, buf1, size)                 \
    do {                                                    \
        uint32_t mask = 0x3fff3fff;                         \
        randomize_buffers(buf0, buf1, size, mask);          \
    } while (0)

#define randomize_prof_src(buf0, buf1, size)                \
    do {                                                    \
        const int shift   = 14 - bit_depth;                 \
        const int mask16  = 0x3fff >> shift << shift;       \
        uint32_t mask = (mask16 << 16) | mask16;            \
        randomize_buffers(buf0, buf1, size, mask);          \
    } while (0)

static struct {
    int16_t *dst0;
    int16_t *dst1;
    uint8_t *src0;
    uint8_t *src1;
} put_vvc_luma_buf;

static av_noinline void check_put_vvc_luma(void)
{
    if (!put_vvc_luma_buf.dst0)
        put_vvc_luma_buf.dst0 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*put_vvc_luma_buf.dst0));
    if (!put_vvc_luma_buf.dst1)
        put_vvc_luma_buf.dst1 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*put_vvc_luma_buf.dst1));
    if (!put_vvc_luma_buf.src0)
        put_vvc_luma_buf.src0 = av_malloc(SRC_BUF_SIZE);
    if (!put_vvc_luma_buf.src1)
        put_vvc_luma_buf.src1 = av_malloc(SRC_BUF_SIZE);
    int16_t *dst0 = put_vvc_luma_buf.dst0;
    int16_t *dst1 = put_vvc_luma_buf.dst1;
    uint8_t *src0 = put_vvc_luma_buf.src0;
    uint8_t *src1 = put_vvc_luma_buf.src1;
    VVCDSPContext c;

    declare_func(void, int16_t *dst, const uint8_t *src, const ptrdiff_t src_stride,
        const int height, const int8_t *hf, const int8_t *vf, const int width);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        if (src0 && src1)
            randomize_pixels(src0, src1, SRC_BUF_SIZE);
        ff_vvc_dsp_init(&c, bit_depth);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int h = 4; h <= MAX_CTU_SIZE; h *= 2) {
                    for (int w = 4; w <= MAX_CTU_SIZE; w *= 2) {
                        const int idx       = av_log2(w) - 1;
                        const int mx        = rnd() % 16;
                        const int my        = rnd() % 16;
                        const int8_t *hf    = ff_vvc_inter_luma_filters[rnd() % 3][mx];
                        const int8_t *vf    = ff_vvc_inter_luma_filters[rnd() % 3][my];
                        const char *type;
                        switch ((j << 1) | i) {
                            case 0: type = "put_luma_pixels"; break; // 0 0
                            case 1: type = "put_luma_h"; break; // 0 1
                            case 2: type = "put_luma_v"; break; // 1 0
                            case 3: type = "put_luma_hv"; break; // 1 1
                        }
                        if (check_func(c.inter.put[LUMA][idx][j][i], "%s_%d_%dx%d", type, bit_depth, w, h)) {
                            if (!dst0 || !dst1 || !src0 || !src1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, DST_BUF_SIZE);
                            memset(dst1, 0, DST_BUF_SIZE);
                            call_ref(dst0, src0 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            call_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            if (memcmp(dst0, dst1, DST_BUF_SIZE))
                                fail();
                            if (w == h)
                                bench_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                        }
                    }
                }
            }
        }
    }
    report("put_luma");
}

static struct {
    uint8_t *dst0;
    uint8_t *dst1;
    uint8_t *src0;
    uint8_t *src1;
} put_vvc_luma_uni_buf;

static av_noinline void check_put_vvc_luma_uni(void)
{
    if (!put_vvc_luma_uni_buf.dst0)
        put_vvc_luma_uni_buf.dst0 = av_malloc(DST_BUF_SIZE);
    if (!put_vvc_luma_uni_buf.dst1)
        put_vvc_luma_uni_buf.dst1 = av_malloc(DST_BUF_SIZE);
    if (!put_vvc_luma_uni_buf.src0)
        put_vvc_luma_uni_buf.src0 = av_malloc(SRC_BUF_SIZE);
    if (!put_vvc_luma_uni_buf.src1)
        put_vvc_luma_uni_buf.src1 = av_malloc(SRC_BUF_SIZE);
    uint8_t *dst0 = put_vvc_luma_uni_buf.dst0;
    uint8_t *dst1 = put_vvc_luma_uni_buf.dst1;
    uint8_t *src0 = put_vvc_luma_uni_buf.src0;
    uint8_t *src1 = put_vvc_luma_uni_buf.src1;

    VVCDSPContext c;
    declare_func(void, uint8_t *dst, ptrdiff_t dststride,
        const uint8_t *src, ptrdiff_t srcstride,  int height,
        const int8_t *hf, const int8_t *vf, int width);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        ff_vvc_dsp_init(&c, bit_depth);
        if (src0 && src1)
            randomize_pixels(src0, src1, SRC_BUF_SIZE);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int h = 4; h <= MAX_CTU_SIZE; h *= 2) {
                    for (int w = 4; w <= MAX_CTU_SIZE; w *= 2) {
                        const int idx       = av_log2(w) - 1;
                        const int mx        = rnd() % VVC_INTER_LUMA_FACTS;
                        const int my        = rnd() % VVC_INTER_LUMA_FACTS;
                        const int8_t *hf    = ff_vvc_inter_luma_filters[rnd() % VVC_INTER_LUMA_FILTER_TYPES][mx];
                        const int8_t *vf    = ff_vvc_inter_luma_filters[rnd() % VVC_INTER_LUMA_FILTER_TYPES][my];
                        const char *type;

                        switch ((j << 1) | i) {
                            case 0: type = "put_uni_pixels"; break; // 0 0
                            case 1: type = "put_uni_h"; break; // 0 1
                            case 2: type = "put_uni_v"; break; // 1 0
                            case 3: type = "put_uni_hv"; break; // 1 1
                        }

                        if (check_func(c.inter.put_uni[LUMA][idx][j][i], "%s_luma_%d_%dx%d", type, bit_depth, w, h)) {
                            if (!dst0 || !dst1 || !src0 || !src1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, DST_BUF_SIZE);
                            memset(dst1, 0, DST_BUF_SIZE);
                            call_ref(dst0, PIXEL_STRIDE, src0 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            call_new(dst1, PIXEL_STRIDE, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            if (memcmp(dst0, dst1, DST_BUF_SIZE))
                                fail();
                            if (w == h)
                                bench_new(dst1, PIXEL_STRIDE, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                        }
                    }
                }
            }
        }
    }
    report("put_uni_luma");
}

static struct {
    int16_t *dst0;
    int16_t *dst1;
    uint8_t *src0;
    uint8_t *src1;
} put_vvc_chroma_buf;

static av_noinline void check_put_vvc_chroma(void)
{
    if (!put_vvc_chroma_buf.dst0)
        put_vvc_chroma_buf.dst0 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*put_vvc_chroma_buf.dst0));
    if (!put_vvc_chroma_buf.dst1)
        put_vvc_chroma_buf.dst1 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*put_vvc_chroma_buf.dst1));
    if (!put_vvc_chroma_buf.src0)
        put_vvc_chroma_buf.src0 = av_malloc(SRC_BUF_SIZE);
    if (!put_vvc_chroma_buf.src1)
        put_vvc_chroma_buf.src1 = av_malloc(SRC_BUF_SIZE);
    int16_t *dst0 = put_vvc_chroma_buf.dst0;
    int16_t *dst1 = put_vvc_chroma_buf.dst1;
    uint8_t *src0 = put_vvc_chroma_buf.src0;
    uint8_t *src1 = put_vvc_chroma_buf.src1;
    VVCDSPContext c;

    declare_func(void, int16_t *dst, const uint8_t *src, const ptrdiff_t src_stride,
        const int height, const int8_t *hf, const int8_t *vf, const int width);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        if (src0 && src1)
            randomize_pixels(src0, src1, SRC_BUF_SIZE);
        ff_vvc_dsp_init(&c, bit_depth);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int h = 2; h <= MAX_CTU_SIZE; h *= 2) {
                    for (int w = 2; w <= MAX_CTU_SIZE; w *= 2) {
                        const int idx       = av_log2(w) - 1;
                        const int mx        = rnd() % VVC_INTER_CHROMA_FACTS;
                        const int my        = rnd() % VVC_INTER_CHROMA_FACTS;
                        const int8_t *hf    = ff_vvc_inter_chroma_filters[rnd() % VVC_INTER_CHROMA_FILTER_TYPES][mx];
                        const int8_t *vf    = ff_vvc_inter_chroma_filters[rnd() % VVC_INTER_CHROMA_FILTER_TYPES][my];
                        const char *type;
                        switch ((j << 1) | i) {
                            case 0: type = "put_chroma_pixels"; break; // 0 0
                            case 1: type = "put_chroma_h"; break; // 0 1
                            case 2: type = "put_chroma_v"; break; // 1 0
                            case 3: type = "put_chroma_hv"; break; // 1 1
                        }
                        if (check_func(c.inter.put[CHROMA][idx][j][i], "%s_%d_%dx%d", type, bit_depth, w, h)) {
                            if (!dst0 || !dst1 || !src0 || !src1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, DST_BUF_SIZE);
                            memset(dst1, 0, DST_BUF_SIZE);
                            call_ref(dst0, src0 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            call_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            if (memcmp(dst0, dst1, DST_BUF_SIZE))
                                fail();
                            if (w == h)
                                bench_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                        }
                    }
                }
            }
        }
    }
    report("put_chroma");
}

static struct {
    uint8_t *dst0;
    uint8_t *dst1;
    uint8_t *src0;
    uint8_t *src1;
} put_vvc_chroma_uni_buf;

static av_noinline void check_put_vvc_chroma_uni(void)
{
    if (!put_vvc_chroma_uni_buf.dst0)
        put_vvc_chroma_uni_buf.dst0 = av_malloc(DST_BUF_SIZE);
    if (!put_vvc_chroma_uni_buf.dst1)
        put_vvc_chroma_uni_buf.dst1 = av_malloc(DST_BUF_SIZE);
    if (!put_vvc_chroma_uni_buf.src0)
        put_vvc_chroma_uni_buf.src0 = av_malloc(SRC_BUF_SIZE);
    if (!put_vvc_chroma_uni_buf.src1)
        put_vvc_chroma_uni_buf.src1 = av_malloc(SRC_BUF_SIZE);
    uint8_t *dst0 = put_vvc_chroma_uni_buf.dst0;
    uint8_t *dst1 = put_vvc_chroma_uni_buf.dst1;
    uint8_t *src0 = put_vvc_chroma_uni_buf.src0;
    uint8_t *src1 = put_vvc_chroma_uni_buf.src1;

    VVCDSPContext c;
    declare_func(void, uint8_t *dst, ptrdiff_t dststride,
        const uint8_t *src, ptrdiff_t srcstride, int height,
        const int8_t *hf, const int8_t *vf, int width);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        ff_vvc_dsp_init(&c, bit_depth);
        if (src0 && src1)
            randomize_pixels(src0, src1, SRC_BUF_SIZE);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int h = 4; h <= MAX_CTU_SIZE; h *= 2) {
                    for (int w = 4; w <= MAX_CTU_SIZE; w *= 2) {
                        const int idx       = av_log2(w) - 1;
                        const int mx        = rnd() % VVC_INTER_CHROMA_FACTS;
                        const int my        = rnd() % VVC_INTER_CHROMA_FACTS;
                        const int8_t *hf    = ff_vvc_inter_chroma_filters[rnd() % VVC_INTER_CHROMA_FILTER_TYPES][mx];
                        const int8_t *vf    = ff_vvc_inter_chroma_filters[rnd() % VVC_INTER_CHROMA_FILTER_TYPES][my];
                        const char *type;

                        switch ((j << 1) | i) {
                            case 0: type = "put_uni_pixels"; break; // 0 0
                            case 1: type = "put_uni_h"; break; // 0 1
                            case 2: type = "put_uni_v"; break; // 1 0
                            case 3: type = "put_uni_hv"; break; // 1 1
                        }

                        if (check_func(c.inter.put_uni[CHROMA][idx][j][i], "%s_chroma_%d_%dx%d", type, bit_depth, w, h)) {
                            if (!dst0 || !dst1 || !src0 || !src1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, DST_BUF_SIZE);
                            memset(dst1, 0, DST_BUF_SIZE);
                            call_ref(dst0, PIXEL_STRIDE, src0 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            call_new(dst1, PIXEL_STRIDE, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                            if (memcmp(dst0, dst1, DST_BUF_SIZE))
                                fail();
                            if (w == h)
                                bench_new(dst1, PIXEL_STRIDE, src1 + SRC_OFFSET, PIXEL_STRIDE, h, hf, vf, w);
                        }
                    }
                }
            }
        }
    }
    report("put_uni_chroma");
}

#define AVG_SRC_BUF_SIZE (MAX_CTU_SIZE * MAX_CTU_SIZE)
#define AVG_DST_BUF_SIZE (MAX_PB_SIZE * MAX_PB_SIZE * 2)

static struct {
    int16_t *src00;
    int16_t *src01;
    int16_t *src10;
    int16_t *src11;
    uint8_t *dst0;
    uint8_t *dst1;
} avg_buf;

static av_noinline void check_avg(void)
{
    if (!avg_buf.src00)
        avg_buf.src00 = av_malloc_array(AVG_SRC_BUF_SIZE, sizeof(*avg_buf.src00));
    if (!avg_buf.src01)
        avg_buf.src01 = av_malloc_array(AVG_SRC_BUF_SIZE, sizeof(*avg_buf.src01));
    if (!avg_buf.src10)
        avg_buf.src10 = av_malloc_array(AVG_SRC_BUF_SIZE, sizeof(*avg_buf.src10));
    if (!avg_buf.src11)
        avg_buf.src11 = av_malloc_array(AVG_SRC_BUF_SIZE, sizeof(*avg_buf.src11));
    if (!avg_buf.dst0)
        avg_buf.dst0 = av_malloc(AVG_DST_BUF_SIZE);
    if (!avg_buf.dst1)
        avg_buf.dst1 = av_malloc(AVG_DST_BUF_SIZE);
    int16_t *src00 = avg_buf.src00;
    int16_t *src01 = avg_buf.src01;
    int16_t *src10 = avg_buf.src10;
    int16_t *src11 = avg_buf.src11;
    uint8_t *dst0  = avg_buf.dst0;
    uint8_t *dst1  = avg_buf.dst1;
    VVCDSPContext c;

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        if (src00 && src10)
            randomize_avg_src((uint8_t*)src00, (uint8_t*)src10, AVG_SRC_BUF_SIZE * sizeof(int16_t));
        if (src01 && src11)
            randomize_avg_src((uint8_t*)src01, (uint8_t*)src11, AVG_SRC_BUF_SIZE * sizeof(int16_t));
        ff_vvc_dsp_init(&c, bit_depth);
        for (int h = 2; h <= MAX_CTU_SIZE; h *= 2) {
            for (int w = 2; w <= MAX_CTU_SIZE; w *= 2) {
                {
                   declare_func(void, uint8_t *dst, ptrdiff_t dst_stride,
                        const int16_t *src0, const int16_t *src1, int width, int height);
                    if (check_func(c.inter.avg, "avg_%d_%dx%d", bit_depth, w, h)) {
                        if (!src00 || !src01 || !src10 || !src11 || !dst0 || !dst1) {
                            fail();
                            continue;
                        }
                        memset(dst0, 0, AVG_DST_BUF_SIZE);
                        memset(dst1, 0, AVG_DST_BUF_SIZE);
                        call_ref(dst0, MAX_CTU_SIZE * SIZEOF_PIXEL, src00, src01, w, h);
                        call_new(dst1, MAX_CTU_SIZE * SIZEOF_PIXEL, src10, src11, w, h);
                        if (memcmp(dst0, dst1, DST_BUF_SIZE))
                            fail();
                        if (w == h)
                            bench_new(dst0, MAX_CTU_SIZE * SIZEOF_PIXEL, src00, src01, w, h);
                    }
                }
                {
                    declare_func(void, uint8_t *dst, ptrdiff_t dst_stride,
                        const int16_t *src0, const int16_t *src1, int width, int height,
                        int denom, int w0, int w1, int o);
                    {
                        const int denom = rnd() % 8;
                        const int w0    = rnd() % 256 - 128;
                        const int w1    = rnd() % 256 - 128;
                        const int o0    = rnd() % 256 - 128;
                        const int o1    = rnd() % 256 - 128;
                        if (check_func(c.inter.w_avg, "w_avg_%d_%dx%d", bit_depth, w, h)) {
                            if (!src00 || !src01 || !src10 || !src11 || !dst0 || !dst1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, AVG_DST_BUF_SIZE);
                            memset(dst1, 0, AVG_DST_BUF_SIZE);

                            call_ref(dst0, MAX_CTU_SIZE * SIZEOF_PIXEL, src00, src01, w, h, denom, w0, w1, o0 + o1);
                            call_new(dst1, MAX_CTU_SIZE * SIZEOF_PIXEL, src10, src11, w, h, denom, w0, w1, o0 + o1);
                            if (memcmp(dst0, dst1, DST_BUF_SIZE))
                                fail();
                            if (w == h)
                                bench_new(dst0, MAX_CTU_SIZE * SIZEOF_PIXEL, src00, src01, w, h, denom, w0, w1, o0 + o1);
                        }
                    }
                }
            }
        }
    }
    report("avg");
}

#define SR_RANGE 2

static struct {
    uint16_t *dst0;
    uint16_t *dst1;
    uint8_t *src0;
    uint8_t *src1;
} dmvr_buf;

static av_noinline void check_dmvr(void)
{
    if (!dmvr_buf.dst0)
        dmvr_buf.dst0 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*dmvr_buf.dst0));
    if (!dmvr_buf.dst1)
        dmvr_buf.dst1 = av_malloc_array(DST_BUF_SIZE / 2, sizeof(*dmvr_buf.dst1));
    if (!dmvr_buf.src0)
        dmvr_buf.src0 = av_malloc(SRC_BUF_SIZE);
    if (!dmvr_buf.src1)
        dmvr_buf.src1 = av_malloc(SRC_BUF_SIZE);
    uint16_t *dst0 = dmvr_buf.dst0;
    uint16_t *dst1 = dmvr_buf.dst1;
    uint8_t *src0  = dmvr_buf.src0;
    uint8_t *src1  = dmvr_buf.src1;
    const int dst_stride = MAX_PB_SIZE;

    VVCDSPContext c;
    declare_func(void, int16_t *dst, const uint8_t *src, ptrdiff_t src_stride, int height,
        intptr_t mx, intptr_t my, int width);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        ff_vvc_dsp_init(&c, bit_depth);
        if (src0 && src1)
            randomize_pixels(src0, src1, SRC_BUF_SIZE);
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int h = 8; h <= 16; h *= 2) {
                    for (int w = 8; w <= 16; w *= 2) {
                        const int pred_w = w + 2 * SR_RANGE;
                        const int pred_h = h + 2 * SR_RANGE;
                        const int mx     = rnd() % VVC_INTER_LUMA_DMVR_FACTS;
                        const int my     = rnd() % VVC_INTER_LUMA_DMVR_FACTS;
                        const char *type;

                        if (w * h < 128)
                            continue;

                        switch ((j << 1) | i) {
                            case 0: type = "dmvr";    break; // 0 0
                            case 1: type = "dmvr_h";  break; // 0 1
                            case 2: type = "dmvr_v";  break; // 1 0
                            case 3: type = "dmvr_hv"; break; // 1 1
                        }

                        if (check_func(c.inter.dmvr[j][i], "%s_%d_%dx%d", type, bit_depth, pred_w, pred_h)) {
                            if (!dst0 || !dst1 || !src0 || !src1) {
                                fail();
                                continue;
                            }
                            memset(dst0, 0, DST_BUF_SIZE);
                            memset(dst1, 0, DST_BUF_SIZE);
                            call_ref(dst0, src0 + SRC_OFFSET, PIXEL_STRIDE, pred_h, mx, my, pred_w);
                            call_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, pred_h, mx, my, pred_w);
                            for (int k = 0; k < pred_h; k++) {
                                if (memcmp(dst0 + k * dst_stride, dst1 + k * dst_stride, pred_w * sizeof(int16_t))) {
                                    fail();
                                    break;
                                }
                            }

                            bench_new(dst1, src1 + SRC_OFFSET, PIXEL_STRIDE, pred_h, mx, my, pred_w);
                        }
                    }
                }
            }
        }
    }
    report("dmvr");
}

#define BDOF_BLOCK_SIZE 16
#define BDOF_SRC_SIZE   (MAX_PB_SIZE* (BDOF_BLOCK_SIZE + 2))
#define BDOF_SRC_OFFSET (MAX_PB_SIZE + 1)
#define BDOF_DST_SIZE   (BDOF_BLOCK_SIZE * BDOF_BLOCK_SIZE * 2)
static av_noinline void check_bdof(void)
{
    LOCAL_ALIGNED_32(uint8_t, dst0, [BDOF_DST_SIZE]);
    LOCAL_ALIGNED_32(uint8_t, dst1, [BDOF_DST_SIZE]);
    LOCAL_ALIGNED_32(uint16_t, src00, [BDOF_SRC_SIZE]);
    LOCAL_ALIGNED_32(uint16_t, src01, [BDOF_SRC_SIZE]);
    LOCAL_ALIGNED_32(uint16_t, src10, [BDOF_SRC_SIZE]);
    LOCAL_ALIGNED_32(uint16_t, src11, [BDOF_SRC_SIZE]);

    VVCDSPContext c;
    declare_func(void, uint8_t *dst, ptrdiff_t dst_stride, const int16_t *src0, const int16_t *src1, int block_w, int block_h);

    for (int bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        const int dst_stride = BDOF_BLOCK_SIZE * SIZEOF_PIXEL;

        ff_vvc_dsp_init(&c, bit_depth);
        randomize_prof_src(src00, src10, BDOF_SRC_SIZE);
        randomize_prof_src(src01, src11, BDOF_SRC_SIZE);
        for (int h = 8; h <= 16; h *= 2) {
            for (int w = 8; w <= 16; w *= 2) {
                if (w * h < 128)
                    continue;
                if (check_func(c.inter.apply_bdof, "apply_bdof_%d_%dx%d", bit_depth, w, h)) {
                    memset(dst0, 0, BDOF_DST_SIZE);
                    memset(dst1, 0, BDOF_DST_SIZE);
                    call_ref(dst0, dst_stride, src00 + BDOF_SRC_OFFSET, src01 + BDOF_SRC_OFFSET, w, h);
                    call_new(dst1, dst_stride, src10 + BDOF_SRC_OFFSET, src11 + BDOF_SRC_OFFSET, w, h);
                    if (memcmp(dst0, dst1, BDOF_DST_SIZE))
                        fail();
                    bench_new(dst0, dst_stride, src00 + BDOF_SRC_OFFSET, src01 + BDOF_SRC_OFFSET, w, h);
                }
            }
        }
    }
    report("apply_bdof");
}

static struct {
    uint16_t *src0;
    uint16_t *src1;
} vvc_sad_buf;

static av_noinline void check_vvc_sad(void)
{
    const int bit_depth = 10;
    VVCDSPContext c;
    if (!vvc_sad_buf.src0)
        vvc_sad_buf.src0 = av_malloc_array(MAX_CTU_SIZE * MAX_CTU_SIZE * 4, sizeof(*vvc_sad_buf.src0));
    if (!vvc_sad_buf.src1)
        vvc_sad_buf.src1 = av_malloc_array(MAX_CTU_SIZE * MAX_CTU_SIZE * 4, sizeof(*vvc_sad_buf.src1));
    uint16_t *src0 = vvc_sad_buf.src0;
    uint16_t *src1 = vvc_sad_buf.src1;
    declare_func(int, const int16_t *src0, const int16_t *src1, int dx, int dy, int block_w, int block_h);

    ff_vvc_dsp_init(&c, bit_depth);
    if (src0 && src1)
        randomize_pixels(src0, src1, MAX_CTU_SIZE * MAX_CTU_SIZE * 4);
    for (int h = 8; h <= 16; h *= 2) {
        for (int w = 8; w <= 16; w *= 2) {
            for(int offy = 0; offy <= 4; offy++) {
                for(int offx = 0; offx <= 4; offx++) {
                    if (w * h < 128)
                        continue;

                    if (check_func(c.inter.sad, "sad_%dx%d", w, h)) {
                        if (!src0 || !src1) {
                            fail();
                            continue;
                        }
                        int result0;
                        int result1;

                        result0 =  call_ref(src0 + PIXEL_STRIDE * 2 + 2, src1 + PIXEL_STRIDE * 2 + 2, offx, offy, w, h);
                        result1 =  call_new(src0 + PIXEL_STRIDE * 2 + 2, src1 + PIXEL_STRIDE * 2 + 2, offx, offy, w, h);

                        if (result1 != result0)
                            fail();
                        if(offx == 0 && offy == 0)
                            bench_new(src0 + PIXEL_STRIDE * 2 + 2, src1 + PIXEL_STRIDE * 2 + 2, offx, offy, w, h);
                    }
                }
            }
        }
    }

    report("sad");
}

void checkasm_uninit_vvc_mc(void)
{
    av_freep(&put_vvc_luma_buf.dst0);
    av_freep(&put_vvc_luma_buf.dst1);
    av_freep(&put_vvc_luma_buf.src0);
    av_freep(&put_vvc_luma_buf.src1);

    av_freep(&put_vvc_luma_uni_buf.dst0);
    av_freep(&put_vvc_luma_uni_buf.dst1);
    av_freep(&put_vvc_luma_uni_buf.src0);
    av_freep(&put_vvc_luma_uni_buf.src1);

    av_freep(&put_vvc_chroma_buf.dst0);
    av_freep(&put_vvc_chroma_buf.dst1);
    av_freep(&put_vvc_chroma_buf.src0);
    av_freep(&put_vvc_chroma_buf.src1);

    av_freep(&put_vvc_chroma_uni_buf.dst0);
    av_freep(&put_vvc_chroma_uni_buf.dst1);
    av_freep(&put_vvc_chroma_uni_buf.src0);
    av_freep(&put_vvc_chroma_uni_buf.src1);

    av_freep(&avg_buf.src00);
    av_freep(&avg_buf.src01);
    av_freep(&avg_buf.src10);
    av_freep(&avg_buf.src11);
    av_freep(&avg_buf.dst0);
    av_freep(&avg_buf.dst1);

    av_freep(&dmvr_buf.dst0);
    av_freep(&dmvr_buf.dst1);
    av_freep(&dmvr_buf.src0);
    av_freep(&dmvr_buf.src1);

    av_freep(&vvc_sad_buf.src0);
    av_freep(&vvc_sad_buf.src1);
}

void checkasm_check_vvc_mc(void)
{
    check_dmvr();
    check_bdof();
    check_vvc_sad();
    check_put_vvc_luma();
    check_put_vvc_luma_uni();
    check_put_vvc_chroma();
    check_put_vvc_chroma_uni();
    check_avg();
}
