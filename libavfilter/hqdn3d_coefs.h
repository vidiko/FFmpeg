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

void ff_hqdn3d_default_strengths(double strength[4]);
void ff_hqdn3d_precalc_coefs(double dist25, int depth, int16_t *ct);

#endif /* AVFILTER_HQDN3D_COEFS_H */
