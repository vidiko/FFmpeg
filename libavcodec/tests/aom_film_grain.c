/*
 * Copyright (c) 2026
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>

#include "libavutil/film_grain_params.h"

#include "libavcodec/aom_film_grain.h"
#include "libavcodec/put_bits.h"

enum {
    Y,
    CB,
    CR,
    EMPTY,
};

static void put_coeffs(PutBitContext *bw, int count, int base)
{
    int i;

    put_bits(bw, 2, 0); // 5-bit signed coefficients
    for (i = 0; i < count; i++)
        put_bits(bw, 5, (base + i + 16) & 31);
}

static void put_explicit_points(PutBitContext *bw, int uv, int count)
{
    int i;

    put_bits(bw, 4, count);
    if (!count)
        return;

    put_bits(bw, 3, 3); // 4-bit point increments
    put_bits(bw, 2, 1); // 6-bit scaling values
    if (uv)
        put_bits(bw, 8, 32);
    for (i = 0; i < count; i++) {
        put_bits(bw, 4, 8 + 4 * i);
        put_bits(bw, 6, 12 + 8 * i);
    }
}

static void put_predicted_points(PutBitContext *bw, int bits_res)
{
    int i;

    put_bits(bw, 9, 272); // scale 16
    put_bits(bw, 9, 256); // offset 0
    put_bits(bw, 3, bits_res);
    if (!bits_res)
        return;

    for (i = 0; i < 2; i++)
        put_bits(bw, bits_res, 3);
    put_bits(bw, 3, 2);
}

static void put_set(PutBitContext *bw, int component, int bits_res, int reference)
{
    uint8_t *start = put_bits_ptr(bw);
    int payload_size;
    int predict_y = !reference && (component == Y || component == EMPTY);
    int predict_uv[2] = {
        !reference && (component == CB || component == EMPTY),
        !reference && (component == CR || component == EMPTY),
    };
    int empty = component == EMPTY;
    int y_points = reference && !empty;
    int uv_points[2] = {
        reference && !empty,
        reference && !empty,
    };
    int num_coeffs = 4;
    int uv;

    put_bits(bw, 1, 0); // 8-bit payload size
    put_bits(bw, 8, 0); // patched below
    put_bits(bw, 3, reference ? 0 : 1);
    put_bits(bw, 1, 1); // AV1 film grain
    put_bits(bw, 16, 1);
    put_bits(bw, 1, 1); // update_grain
    put_bits(bw, 4, 0);
    put_bits(bw, 12, 64);
    put_bits(bw, 12, reference ? 32 : 16);
    put_bits(bw, 1, 0); // not luma only
    put_bits(bw, 1, 0); // 4:4:4 permits independently empty chroma lists
    put_bits(bw, 1, 0);
    put_bits(bw, 1, 0); // no video signal characteristics
    put_bits(bw, 1, !reference); // predict_scaling

    if (!reference) {
        put_bits(bw, 1, predict_y);
        if (predict_y)
            put_predicted_points(bw, bits_res);
    }
    if (!predict_y)
        put_explicit_points(bw, 0, y_points ? 2 : 0);

    put_bits(bw, 1, 0); // chroma_scaling_from_luma
    for (uv = 0; uv < 2; uv++) {
        if (!reference)
            put_bits(bw, 1, predict_uv[uv]);
        if (predict_uv[uv])
            put_predicted_points(bw, bits_res);
        else
            put_explicit_points(bw, 1, uv_points[uv] ? 2 : 0);
    }

    put_bits(bw, 2, 1); // scaling_shift = 9
    put_bits(bw, 2, 1); // ar_coeff_lag = 1
    if (y_points || predict_y)
        put_coeffs(bw, num_coeffs, -4);
    for (uv = 0; uv < 2; uv++) {
        if (uv_points[uv] || predict_uv[uv])
            put_coeffs(bw, num_coeffs + !!(y_points || predict_y), 4 + uv * 8);
    }
    put_bits(bw, 2, 1); // ar_coeff_shift = 7
    put_bits(bw, 2, 2); // grain_scale_shift = 2
    for (uv = 0; uv < 2; uv++) {
        if (uv_points[uv] && !predict_uv[uv]) {
            put_bits(bw, 8, 130 + uv);
            put_bits(bw, 8, 127 - uv);
            put_bits(bw, 9, 259 + uv);
        }
    }
    put_bits(bw, 1, 1);
    put_bits(bw, 1, 0);

    flush_put_bits(bw);
    payload_size = put_bits_ptr(bw) - start;
    // The byte-aligned payload stores its size in bits 1 through 8.
    start[0] = payload_size >> 1;
    start[1] |= (payload_size & 1) << 7;
}

static int build_payload(uint8_t *payload, int component, int bits_res)
{
    PutBitContext bw;

    init_put_bits(&bw, payload, 128);

    put_bits(&bw, 1, 1);
    put_bits(&bw, 4, 0);
    put_bits(&bw, 3, 1); // two parameter sets
    flush_put_bits(&bw);
    put_set(&bw, component, bits_res, 1);
    put_set(&bw, component, bits_res, 0);

    return put_bytes_output(&bw);
}

static int build_explicit_payload(uint8_t *payload, int y_points,
                                  int cb_points, int cr_points)
{
    PutBitContext bw;
    int uv_points[2] = { cb_points, cr_points };
    uint8_t *start;
    int payload_size;
    int uv;

    init_put_bits(&bw, payload, 128);

    put_bits(&bw, 1, 1);
    put_bits(&bw, 4, 0);
    put_bits(&bw, 3, 0); // one parameter set
    flush_put_bits(&bw);

    start = put_bits_ptr(&bw);
    put_bits(&bw, 1, 0); // 8-bit payload size
    put_bits(&bw, 8, 0); // patched below
    put_bits(&bw, 3, 0);
    put_bits(&bw, 1, 1); // AV1 film grain
    put_bits(&bw, 16, 1);
    put_bits(&bw, 1, 1); // update_grain
    put_bits(&bw, 4, 0);
    put_bits(&bw, 12, 64);
    put_bits(&bw, 12, 32);
    put_bits(&bw, 1, 0); // not luma only
    put_bits(&bw, 1, 0); // subsampling_x = 0 (4:4:4)
    put_bits(&bw, 1, 0); // subsampling_y = 0
    put_bits(&bw, 1, 0); // no video signal characteristics
    put_bits(&bw, 1, 0); // no predicted scaling
    put_explicit_points(&bw, 0, y_points);
    put_bits(&bw, 1, 0); // chroma_scaling_from_luma
    for (uv = 0; uv < 2; uv++)
        put_explicit_points(&bw, 1, uv_points[uv]);

    put_bits(&bw, 2, 1); // scaling_shift = 9
    put_bits(&bw, 2, 1); // ar_coeff_lag = 1
    if (y_points)
        put_coeffs(&bw, 4, -4);
    for (uv = 0; uv < 2; uv++) {
        if (uv_points[uv])
            put_coeffs(&bw, 4 + !!y_points, 4 + uv * 8);
    }
    put_bits(&bw, 2, 1); // ar_coeff_shift = 7
    put_bits(&bw, 2, 2); // grain_scale_shift = 2
    for (uv = 0; uv < 2; uv++) {
        if (uv_points[uv]) {
            put_bits(&bw, 8, 130 + uv);
            put_bits(&bw, 8, 127 - uv);
            put_bits(&bw, 9, 259 + uv);
        }
    }
    put_bits(&bw, 1, 1);
    put_bits(&bw, 1, 0);

    flush_put_bits(&bw);
    payload_size = put_bits_ptr(&bw) - start;
    start[0] = payload_size >> 1;
    start[1] |= (payload_size & 1) << 7;

    return put_bytes_output(&bw);
}

static void put_skipped_set(PutBitContext *bw, int update_grain)
{
    int start = put_bits_count(bw);
    int payload_size;

    put_bits(bw, 1, !update_grain); // payload_less_than_4byte_flag
    payload_size = update_grain ? 4 : 1;
    put_bits(bw, update_grain ? 8 : 2, payload_size);
    put_bits(bw, 3, 1); // distinct from the following full set
    put_bits(bw, 1, update_grain);
    if (update_grain) {
        put_bits(bw, 16, 1);
        put_bits(bw, 1, 0);
    }
    while (put_bits_count(bw) - start < payload_size * 8)
        put_bits(bw, 1, 0);
    flush_put_bits(bw);
}

static int build_padded_payload(uint8_t *payload, int update_grain)
{
    PutBitContext bw;

    init_put_bits(&bw, payload, 128);

    put_bits(&bw, 1, 1);
    put_bits(&bw, 4, 0);
    put_bits(&bw, 3, 1); // two parameter sets
    flush_put_bits(&bw);
    put_skipped_set(&bw, update_grain);
    put_set(&bw, Y, 0, 1);

    return put_bytes_output(&bw);
}

static void print_points(const char *name, int count, const uint8_t points[][2])
{
    int i;

    printf("  %s[%d]", name, count);
    for (i = 0; i < count; i++)
        printf(" %d/%d", points[i][0], points[i][1]);
    printf("\n");
}

static void print_coeffs(const char *name, const int8_t *coeffs, int count)
{
    int i;

    printf("  %s[%d]", name, count);
    for (i = 0; i < count; i++)
        printf(" %d", coeffs[i]);
    printf("\n");
}

static void run_case(const char *name, int component, int bits_res)
{
    AVFilmGrainAFGS1Params sets = { 0 };
    uint8_t payload[128];
    AVFilmGrainAOMParams *aom;
    int y_coeffs = (component == Y || component == EMPTY) ? 4 : 0;
    int cb_coeffs = (component == CB || component == EMPTY) ? 4 + !!y_coeffs : 0;
    int cr_coeffs = (component == CR || component == EMPTY) ? 4 + !!y_coeffs : 0;
    int ret = ff_aom_parse_film_grain_sets(&sets, payload,
                                            build_payload(payload, component, bits_res));

    printf("%s: ret=%d\n", name, ret);
    if (ret || !sets.sets[1]) {
        printf("  set=missing\n");
        ff_aom_uninit_film_grain_params(&sets);
        return;
    }

    aom = &((AVFilmGrainParams *)sets.sets[1]->data)->codec.aom;
    print_points("y", aom->num_y_points, aom->y_points);
    print_points("cb", aom->num_uv_points[0], aom->uv_points[0]);
    print_points("cr", aom->num_uv_points[1], aom->uv_points[1]);
    printf("  scaling=%d lag=%d ar_shift=%d grain_shift=%d\n",
           aom->scaling_shift, aom->ar_coeff_lag, aom->ar_coeff_shift,
           aom->grain_scale_shift);
    print_coeffs("y_ar", aom->ar_coeffs_y, y_coeffs);
    print_coeffs("cb_ar", aom->ar_coeffs_uv[0], cb_coeffs);
    print_coeffs("cr_ar", aom->ar_coeffs_uv[1], cr_coeffs);
    printf("  cb_uv=%d/%d/%d cr_uv=%d/%d/%d overlap=%d limit=%d\n",
           aom->uv_mult[0], aom->uv_mult_luma[0], aom->uv_offset[0],
           aom->uv_mult[1], aom->uv_mult_luma[1], aom->uv_offset[1],
           aom->overlap_flag, aom->limit_output_range);
    ff_aom_uninit_film_grain_params(&sets);
}

static void run_explicit_case(const char *name, int y_points,
                              int cb_points, int cr_points)
{
    AVFilmGrainAFGS1Params sets = { 0 };
    uint8_t payload[128];
    AVFilmGrainAOMParams *aom;
    int ret = ff_aom_parse_film_grain_sets(&sets, payload,
                                            build_explicit_payload(payload, y_points,
                                                                   cb_points, cr_points));

    printf("%s: ret=%d\n", name, ret);
    if (ret || !sets.sets[0]) {
        printf("  set=missing\n");
        ff_aom_uninit_film_grain_params(&sets);
        return;
    }

    aom = &((AVFilmGrainParams *)sets.sets[0]->data)->codec.aom;
    print_points("y", aom->num_y_points, aom->y_points);
    print_points("cb", aom->num_uv_points[0], aom->uv_points[0]);
    print_points("cr", aom->num_uv_points[1], aom->uv_points[1]);
    ff_aom_uninit_film_grain_params(&sets);
}

static void run_padded_case(const char *name, int update_grain)
{
    AVFilmGrainAFGS1Params sets = { 0 };
    uint8_t payload[128];
    AVFilmGrainAOMParams *aom;
    int ret = ff_aom_parse_film_grain_sets(&sets, payload,
                                            build_padded_payload(payload, update_grain));

    printf("%s: ret=%d\n", name, ret);
    if (ret || !sets.sets[0]) {
        printf("  set=missing\n");
        ff_aom_uninit_film_grain_params(&sets);
        return;
    }

    aom = &((AVFilmGrainParams *)sets.sets[0]->data)->codec.aom;
    print_points("y", aom->num_y_points, aom->y_points);
    ff_aom_uninit_film_grain_params(&sets);
}

int main(void)
{
    run_case("y-zero", Y, 0);
    run_case("y-nonzero", Y, 2);
    run_case("cb-zero", CB, 0);
    run_case("cb-nonzero", CB, 2);
    run_case("cr-zero", CR, 0);
    run_case("cr-nonzero", CR, 2);
    run_case("empty-reference", EMPTY, 0);
    run_explicit_case("explicit-y-cb0-cr", 1, 0, 1);
    run_explicit_case("explicit-y-cb-cr0", 1, 1, 0);
    run_explicit_case("explicit-y-cb0-cr0", 1, 0, 0);
    run_padded_case("padded-apply-grain-zero", 0);
    run_padded_case("padded-update-grain-zero", 1);

    return 0;
}
