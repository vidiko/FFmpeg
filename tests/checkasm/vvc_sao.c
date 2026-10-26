/*
 * Copyright (c) 2018 Yingming Fan <yingmingfan@gmail.com>
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
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"

#include "libavcodec/vvc/dsp.h"
#include "libavcodec/vvc/ctu.h"

static const uint32_t pixel_mask[3] = { 0xffffffff, 0x03ff03ff, 0x0fff0fff };
static const uint32_t sao_size[] = {8, 16, 32, 48, 64, 80, 96, 112, 128};

#define SIZEOF_PIXEL ((bit_depth + 7) / 8)
#define PIXEL_STRIDE (2*MAX_CTU_SIZE + AV_INPUT_BUFFER_PADDING_SIZE) //same with sao_edge src_stride
#define BUF_SIZE (PIXEL_STRIDE * (MAX_CTU_SIZE+2) * 2) //+2 for top and bottom row, *2 for high bit depth
#define DST_BUF_W (CHECKASM_ROUND(MAX_CTU_SIZE, 64) + 64)
#define DST_BUF_H (MAX_CTU_SIZE + 32)
#define DST_RECT_BUF_SIZE (DST_BUF_H * DST_BUF_W + 64)
#define DST_RECT_OFFSET (DST_BUF_W * 16 + 64)
#define OFFSET_THRESH (1 << (bit_depth - 5))
#define OFFSET_LENGTH 5

#define randomize_buffers(buf0, buf1, size)                 \
    do {                                                    \
        uint32_t mask = pixel_mask[(bit_depth - 8) >> 1];   \
        int k;                                              \
        for (k = 0; k < size; k += 4) {                     \
            uint32_t r = rnd() & mask;                      \
            AV_WN32A(buf0 + k, r);                          \
            AV_WN32A(buf1 + k, r);                          \
        }                                                   \
    } while (0)

#define randomize_buffers2(buf, size)                       \
    do {                                                    \
        uint32_t max_offset = OFFSET_THRESH;                \
        int k;                                              \
        if (bit_depth == 8) {                               \
            for (k = 0; k < size; k++) {                    \
                uint8_t r = rnd() % max_offset;             \
                buf[k] = r;                                 \
            }                                               \
        } else {                                            \
            for (k = 0; k < size; k++) {                    \
                uint16_t r = rnd() % max_offset;            \
                buf[k] = r;                                 \
            }                                               \
        }                                                   \
    } while (0)

static void check_sao_band(VVCDSPContext *h, int bit_depth)
{
    uint16_t *dst0_buf = av_malloc_array(DST_RECT_BUF_SIZE, sizeof(*dst0_buf));
    uint16_t *dst1_buf = av_malloc_array(DST_RECT_BUF_SIZE, sizeof(*dst1_buf));
    uint8_t *dst0 = dst0_buf ? (uint8_t *)(dst0_buf + DST_RECT_OFFSET) : NULL;
    uint8_t *dst1 = dst1_buf ? (uint8_t *)(dst1_buf + DST_RECT_OFFSET) : NULL;
    const ptrdiff_t dst_stride = sizeof(*dst0_buf) * DST_BUF_W;
    uint8_t *src0 = av_malloc(BUF_SIZE);
    uint8_t *src1 = av_malloc(BUF_SIZE);
    int16_t offset_val[OFFSET_LENGTH];
    const int left_class = rnd()%32;
    const int walign = 16;

    if (!dst0 || !dst1 || !src0 || !src1)
        fail();

    for (int i = 0; i < FF_ARRAY_ELEMS(sao_size); i++) {
        const int block_size = sao_size[i];
        const int prev_size = i > 0 ? sao_size[i - 1] : 0;
        ptrdiff_t stride = PIXEL_STRIDE*SIZEOF_PIXEL;
        declare_func(void, uint8_t *dst, const uint8_t *src, ptrdiff_t dst_stride, ptrdiff_t src_stride,
                     const int16_t *sao_offset_val, int sao_left_class, int width, int height);

        if (check_func(h->sao.band_filter[i], "vvc_sao_band_%d_%d", block_size, bit_depth)) {

            for (int w = prev_size + 4; w <= block_size; w += 4) {
                randomize_buffers(src0, src1, BUF_SIZE);
                randomize_buffers2(offset_val, OFFSET_LENGTH);
                checkasm_clear(dst0_buf, DST_RECT_BUF_SIZE * sizeof(*dst0_buf));
                checkasm_clear(dst1_buf, DST_RECT_BUF_SIZE * sizeof(*dst1_buf));

                call_ref(dst0, src0, dst_stride, stride, offset_val, left_class, w, block_size);
                call_new(dst1, src1, dst_stride, stride, offset_val, left_class, w, block_size);
                checkasm_check_pixel_padded_align(dst0, dst_stride, dst1, dst_stride, w, block_size, "dst", walign, 1);
            }
            bench_new(dst1, src1, dst_stride, stride, offset_val, left_class, block_size, block_size);
        }
    }

    av_free(dst0_buf);
    av_free(dst1_buf);
    av_free(src0);
    av_free(src1);
}

static void check_sao_edge(VVCDSPContext *h, int bit_depth)
{
    uint16_t *dst0_buf = av_malloc_array(DST_RECT_BUF_SIZE, sizeof(*dst0_buf));
    uint16_t *dst1_buf = av_malloc_array(DST_RECT_BUF_SIZE, sizeof(*dst1_buf));
    uint8_t *dst0 = dst0_buf ? (uint8_t *)(dst0_buf + DST_RECT_OFFSET) : NULL;
    uint8_t *dst1 = dst1_buf ? (uint8_t *)(dst1_buf + DST_RECT_OFFSET) : NULL;
    const ptrdiff_t dst_stride = sizeof(*dst0_buf) * DST_BUF_W;
    uint8_t *src0 = av_malloc(BUF_SIZE);
    uint8_t *src1 = av_malloc(BUF_SIZE);
    int16_t offset_val[OFFSET_LENGTH];
    const int eo = rnd()%4;
    const int walign = 16;

    if (!dst0 || !dst1 || !src0 || !src1)
        fail();

    for (int i = 0; i < FF_ARRAY_ELEMS(sao_size); i++) {
        int block_size = sao_size[i];
        int prev_size = i > 0 ? sao_size[i - 1] : 0;
        int offset = (AV_INPUT_BUFFER_PADDING_SIZE + PIXEL_STRIDE)*SIZEOF_PIXEL;
        declare_func(void, uint8_t *dst, const uint8_t *src, ptrdiff_t stride_dst,
                     const int16_t *sao_offset_val, int eo, int width, int height);

        if (check_func(h->sao.edge_filter[i], "vvc_sao_edge_%d_%d", block_size, bit_depth)) {
            for (int w = prev_size + 4; w <= block_size; w += 4) {
                randomize_buffers(src0, src1, BUF_SIZE);
                randomize_buffers2(offset_val, OFFSET_LENGTH);
                checkasm_clear(dst0_buf, DST_RECT_BUF_SIZE * sizeof(*dst0_buf));
                checkasm_clear(dst1_buf, DST_RECT_BUF_SIZE * sizeof(*dst1_buf));

                call_ref(dst0, src0 + offset, dst_stride, offset_val, eo, w, block_size);
                call_new(dst1, src1 + offset, dst_stride, offset_val, eo, w, block_size);
                checkasm_check_pixel_padded_align(dst0, dst_stride, dst1, dst_stride, w, block_size, "dst", walign, 1);
            }
            bench_new(dst1, src1 + offset, dst_stride, offset_val, eo, block_size, block_size);
        }
    }

    av_free(dst0_buf);
    av_free(dst1_buf);
    av_free(src0);
    av_free(src1);
}

void checkasm_check_vvc_sao(void)
{
    int bit_depth;

    for (bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        VVCDSPContext h;

        ff_vvc_dsp_init(&h, bit_depth);
        check_sao_band(&h, bit_depth);
    }
    report("sao_band");

    for (bit_depth = 8; bit_depth <= 12; bit_depth += 2) {
        VVCDSPContext h;

        ff_vvc_dsp_init(&h, bit_depth);
        check_sao_edge(&h, bit_depth);
    }
    report("sao_edge");
}
