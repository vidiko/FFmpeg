/*
 * HDR to SDR tone mapping with the Hable curve on the intensity of IPT
 *
 * The colour map follows libplacebo v7.360.1 with hable tone mapping and
 * clipping gamut mapping (src/shaders/colorspace.c: pl_shader_color_map_ex;
 * src/tone_mapping.c: pl_tone_map_generate, hable_map), Copyright (C) Niklas
 * Haas and the libplacebo contributors, licensed under the GNU Lesser General
 * Public License version 2.1 or later.
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

#include <math.h>

#include "libavutil/common.h"
#include "libavutil/csp.h"
#include "libavutil/error.h"

#include "bt2408_hable.h"
#include "colorspace.h"

#define SDR_WHITE    203.0  /* cd/m^2, ITU-R BT.2408 */
#define SDR_CONTRAST 1000.0
#define HDR_BLACK    1e-6   /* cd/m^2 */
#define PQ_MAX       10000.0

#define PQ_M1 (2610.0 / 4096 / 4)
#define PQ_M2 (2523.0 / 4096 * 128)
#define PQ_C1 (3424.0 / 4096)
#define PQ_C2 (2413.0 / 4096 * 32)
#define PQ_C3 (2392.0 / 4096 * 32)

/* Hunt-Pointer-Estevez XYZ to LMS, then 4% crosstalk, as for ICtCp. */
static const double hpe_xyz_to_lms[3][3] = {
    {  0.40024, 0.70760, -0.08081 },
    { -0.22630, 1.16532,  0.04570 },
    {  0.0,     0.0,      0.91822 },
};

static const double crosstalk[3][3] = {
    { 0.92, 0.04, 0.04 },
    { 0.04, 0.92, 0.04 },
    { 0.04, 0.04, 0.92 },
};

static const float lms_to_ipt[3][3] = {
    { 0.4000f,  0.4000f,  0.2000f },
    { 4.4550f, -4.8510f,  0.3960f },
    { 0.8056f,  0.3572f, -1.1628f },
};

static const float ipt_to_lms[3][3] = {
    { 1.0f,  0.0975689f,  0.205226f },
    { 1.0f, -0.1138760f,  0.133217f },
    { 1.0f,  0.0326151f, -0.676887f },
};

/* SMPTE ST 2084 inverse EOTF: luminance in cd/m^2 to a PQ signal in [0, 1]. */
static double pq_oetf(double nits)
{
    double y = pow(FFMAX(nits, 0.0) / PQ_MAX, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

/* SMPTE ST 2084 EOTF: PQ signal in [0, 1] to luminance in cd/m^2. */
static double pq_eotf(double x)
{
    double p = pow(FFMAX(x, 0.0), 1.0 / PQ_M2);
    return pow(FFMAX(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1) * PQ_MAX;
}

/* Single-precision pq_oetf() for the per-pixel path. */
static float pq_oetf_f(float nits)
{
    float y = powf(FFMAX(nits, 0.0f) * (float)(1.0 / PQ_MAX), (float)PQ_M1);
    return powf(((float)PQ_C1 + (float)PQ_C2 * y) / (1.0f + (float)PQ_C3 * y), (float)PQ_M2);
}

/* Single-precision pq_eotf() for the per-pixel path. */
static float pq_eotf_f(float x)
{
    float p = powf(FFMAX(x, 0.0f), (float)(1.0 / PQ_M2));
    return powf(FFMAX(p - (float)PQ_C1, 0.0f) / ((float)PQ_C2 - (float)PQ_C3 * p),
                (float)(1.0 / PQ_M1)) * (float)PQ_MAX;
}

/* Hable filmic curve, as in libplacebo; the caller normalises it to the peak. */
static double hable(double x)
{
    const double a = 0.15, b = 0.50, c = 0.10, d = 0.20, e = 0.02, f = 0.30;
    return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}

/* ITU-R BT.1886 EOTF: signal in [0, 1] to linear light, for black level min and white level max. */
static double bt1886_eotf(double x, double min, double max)
{
    const double lb = pow(min, 1 / 2.4), lw = pow(max, 1 / 2.4);
    return pow((lw - lb) * x + lb, 2.4);
}

/* Inverse of bt1886_eotf(). */
static double bt1886_oetf(double x, double min, double max)
{
    const double lb = pow(min, 1 / 2.4), lw = pow(max, 1 / 2.4);
    return (pow(FFMAX(x, 0.0), 1 / 2.4) - lb) / (lw - lb);
}

/* RGB of prim to LMS with crosstalk; ENOTSUP unless prim is known and has a D65 white point. */
static int rgb_to_lms(enum AVColorPrimaries prim, double out[3][3])
{
    const AVColorPrimariesDesc *desc = av_csp_primaries_desc_from_id(prim);
    const AVColorPrimariesDesc *d65  = av_csp_primaries_desc_from_id(AVCOL_PRI_BT709);
    double rgb2xyz[3][3], lms[3][3];

    if (!desc || av_cmp_q(desc->wp.x, d65->wp.x) || av_cmp_q(desc->wp.y, d65->wp.y))
        return AVERROR(ENOTSUP);
    /* The table maps white to Y = y_w; PQ needs absolute luminance, Y = 1. */
    ff_fill_rgb2xyz_table(&desc->prim, &desc->wp, rgb2xyz);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            rgb2xyz[i][j] /= av_q2d(desc->wp.y);
    /* ff_matrix_mul_3x3(dst, a, b) computes b * a */
    ff_matrix_mul_3x3(lms, rgb2xyz, hpe_xyz_to_lms);
    ff_matrix_mul_3x3(out, lms, crosstalk);
    return 0;
}

/* libplacebo pl_tone_map_params_infer: no inverse tone mapping. */
static void fill_lut(FFBT2408Hable *s, double peak)
{
    const double out_max = FFMIN(SDR_WHITE, peak) / SDR_WHITE;
    const double out_min = SDR_WHITE / SDR_CONTRAST / SDR_WHITE;
    const double in_max  = peak / SDR_WHITE, in_min = HDR_BLACK / SDR_WHITE;
    const double ratio   = in_max / out_max, scale = 1.0 / hable(ratio);

    s->lut_min = pq_oetf(HDR_BLACK);
    s->lut_max = pq_oetf(peak);
    for (int i = 0; i < FF_BT2408_HABLE_LUT_SIZE; i++) {
        double t = (double)i / (FF_BT2408_HABLE_LUT_SIZE - 1);
        double x = pq_eotf(s->lut_min + t * (s->lut_max - s->lut_min)) / SDR_WHITE;
        x = bt1886_oetf(x, in_min, in_max);
        x = scale * hable(bt1886_eotf(x, 0, ratio));
        x = bt1886_eotf(bt1886_oetf(x, 0, 1), out_min, out_max);
        s->lut[i] = pq_oetf(av_clipd(x, out_min, out_max) * SDR_WHITE);
    }
}

int ff_bt2408_hable_init(FFBT2408Hable *s, enum AVColorPrimaries prim, double peak)
{
    const double lb = pow(1.0 / SDR_CONTRAST, 1 / 2.4), lw = 1.0;
    double in[3][3], bt709[3][3], out[3][3];
    int ret;

    if (!(peak > 0.0) || !isfinite(peak))
        return AVERROR(EINVAL);
    if ((ret = rgb_to_lms(prim, in)) < 0 || (ret = rgb_to_lms(AVCOL_PRI_BT709, bt709)) < 0)
        return ret;
    ff_matrix_invert_3x3(bt709, out);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            s->rgb_to_lms[i][j] = in[i][j] * REFERENCE_WHITE / SDR_WHITE;
            s->lms_to_rgb[i][j] = out[i][j];
        }
    }

    /* Input below the reference display peak is not expanded. */
    fill_lut(s, av_clipd(peak, FFMIN(SDR_WHITE, REFERENCE_WHITE), PQ_MAX));
    s->black_scale  = 1.0 / pow(lw - lb, 2.4);
    s->black_offset = lb / (lw - lb);
    return 0;
}

/* out = m * in for a 3x3 matrix and a column vector. */
static void mul3(const float m[3][3], const float in[3], float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
}

/* Interpolate the tone curve table linearly at the PQ intensity i. */
static float lut_lookup(const FFBT2408Hable *s, float i)
{
    float pos = (i - s->lut_min) / (s->lut_max - s->lut_min) * (FF_BT2408_HABLE_LUT_SIZE - 1);
    int idx;

    pos = av_clipf(pos, 0.0f, FF_BT2408_HABLE_LUT_SIZE - 1);
    idx = FFMIN((int)pos, FF_BT2408_HABLE_LUT_SIZE - 2);
    return s->lut[idx] + (pos - idx) * (s->lut[idx + 1] - s->lut[idx]);
}

/* Desaturate towards the intensity axis as the tone curve compresses it. */
static float hull_scale(float i_orig, float i_mapped)
{
    float h_orig   = ((i_orig - 6.0f) * i_orig + 9.0f) * i_orig;
    float h_mapped = ((i_mapped - 6.0f) * i_mapped + 9.0f) * i_mapped;

    return FFMIN(i_orig / FFMAX(i_mapped, 1e-9f), h_mapped / FFMAX(h_orig, 1e-9f));
}

void ff_bt2408_hable_pixel(const FFBT2408Hable *s, const float in[3], float out[3])
{
    float lms[3], ipt[3], k;

    mul3(s->rgb_to_lms, in, lms);
    for (int c = 0; c < 3; c++)
        lms[c] = pq_oetf_f(lms[c] * (float)SDR_WHITE);
    mul3(lms_to_ipt, lms, ipt);

    k = ipt[0];
    ipt[0] = lut_lookup(s, k);
    k = hull_scale(k, ipt[0]);
    ipt[1] *= k;
    ipt[2] *= k;

    mul3(ipt_to_lms, ipt, lms);
    for (int c = 0; c < 3; c++)
        lms[c] = pq_eotf_f(lms[c]) * (float)(1.0 / SDR_WHITE);
    mul3(s->lms_to_rgb, lms, out);

    /* Encode for BT.1886 with a raised black, then decode as pure power 2.4. */
    for (int c = 0; c < 3; c++) {
        float v = powf(av_clipf(out[c], 0.0f, 1.0f) * s->black_scale, 1.0f / 2.4f) - s->black_offset;
        out[c] = powf(av_clipf(v, 0.0f, 1.0f), 2.4f);
    }
}
