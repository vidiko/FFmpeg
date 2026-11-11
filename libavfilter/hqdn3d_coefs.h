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

#ifndef AVFILTER_HQDN3D_COEFS_H
#define AVFILTER_HQDN3D_COEFS_H

#include <stdint.h>

#define HQDN3D_PARAM1_DEFAULT 4.0
#define HQDN3D_PARAM2_DEFAULT 3.0
#define HQDN3D_PARAM3_DEFAULT 6.0

#define HQDN3D_LUT_BITS(depth) ((depth) == 16 ? 8 : 4)

/**
 * Fill unset (zero) denoise strength entries derived from luma spatial strength.
 *
 * Fills zero entries in strength using default ratios based on luma spatial.
 *
 * @param strength array of 4 denoising strengths in [0, DBL_MAX], indexed in order
 *                 LUMA_SPATIAL (0), LUMA_TMP (1), CHROMA_SPATIAL (2), and CHROMA_TMP (3)
 */
void ff_hqdn3d_default_strengths(double strength[4]);

/**
 * Precalculate coefficient lookup table for spatial or temporal filtering.
 *
 * Builds a coefficient lookup table of (512 << HQDN3D_LUT_BITS(depth)) entries.
 * Upon return, ct[0] holds !!dist25 (1 if enabled/non-zero, 0 if disabled).
 *
 * @param dist25 denoising strength parameter, internally clamped to 252.0
 * @param depth  pixel bit depth determining LUT precision via HQDN3D_LUT_BITS(depth)
 * @param ct     lookup table destination buffer of size (512 << HQDN3D_LUT_BITS(depth)) int16_t
 */
void ff_hqdn3d_precalc_coefs(double dist25, int depth, int16_t *ct);

#endif /* AVFILTER_HQDN3D_COEFS_H */
