/*
 * Copyright (c) 2003 Daniel Moreno <comac AT comac DOT darktech DOT org>
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2012 Loren Merritt
 * Copyright (c) 2026 Lusoris
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

#include <math.h>

#include "libavutil/common.h"
#include "hqdn3d_coefs.h"
#include "vf_hqdn3d.h"

void ff_hqdn3d_default_strengths(double strength[4])
{
    if (!strength[LUMA_SPATIAL])
        strength[LUMA_SPATIAL] = HQDN3D_PARAM1_DEFAULT;
    if (!strength[CHROMA_SPATIAL])
        strength[CHROMA_SPATIAL] = HQDN3D_PARAM2_DEFAULT * strength[LUMA_SPATIAL] / HQDN3D_PARAM1_DEFAULT;
    if (!strength[LUMA_TMP])
        strength[LUMA_TMP]   = HQDN3D_PARAM3_DEFAULT * strength[LUMA_SPATIAL] / HQDN3D_PARAM1_DEFAULT;
    if (!strength[CHROMA_TMP])
        strength[CHROMA_TMP] = strength[LUMA_TMP] * strength[CHROMA_SPATIAL] / strength[LUMA_SPATIAL];
}

void ff_hqdn3d_precalc_coefs(double dist25, int depth, int16_t *ct)
{
    int lut_bits = HQDN3D_LUT_BITS(depth);
    int i;
    double gamma, simil, C;

    gamma = log(0.25) / log(1.0 - FFMIN(dist25, 252.0) / 255.0 - 0.00001);

    for (i = -(256 << lut_bits); i < (256 << lut_bits); i++) {
        double f = (i * (1 << (9 - lut_bits)) + (1 << (8 - lut_bits)) - 1) / 512.0;
        simil = FFMAX(0, 1.0 - fabs(f) / 255.0);
        C = pow(simil, gamma) * 256.0 * f;
        ct[(256 << lut_bits) + i] = lrint(C);
    }

    ct[0] = !!dist25;
}
