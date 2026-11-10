/*
 * HDR to SDR tone mapping with the Hable curve on the intensity of IPT
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

#ifndef AVFILTER_BT2408_HABLE_H
#define AVFILTER_BT2408_HABLE_H

#include "libavutil/pixfmt.h"

#define FF_BT2408_HABLE_LUT_SIZE 256

/**
 * Everything needed to tone map one frame. All members are 32-bit scalars
 * or arrays of them, so the struct can be uploaded unchanged as an OpenCL
 * or std430 Vulkan buffer.
 */
typedef struct FFBT2408Hable {
    float rgb_to_lms[3][3];  ///< linear input RGB, 1.0 = 100 cd/m^2, to LMS
    float lms_to_rgb[3][3];  ///< LMS to linear BT.709 RGB
    float lut_min;           ///< PQ intensity of the first LUT entry
    float lut_max;           ///< PQ intensity of the last LUT entry
    float lut[FF_BT2408_HABLE_LUT_SIZE]; ///< tone curve on PQ intensity
    float black_scale;       ///< BT.1886 encoding with a 1000:1 contrast
    float black_offset;
} FFBT2408Hable;

/**
 * Fill out for one input colour space and signal peak.
 *
 * @param prim primaries of the input, with a D65 white point
 * @param peak signal peak in cd/m^2
 * @return 0 on success, AVERROR(ENOTSUP) for other primaries,
 *         AVERROR(EINVAL) for a peak that is not a positive number
 */
int ff_bt2408_hable_init(FFBT2408Hable *s, enum AVColorPrimaries prim, double peak);

/**
 * Tone map one pixel.
 *
 * @param in  linear RGB in the input primaries, 1.0 = 100 cd/m^2
 * @param out linear BT.709 RGB in [0, 1] for a pure power 2.4 display,
 *            1.0 = reference white (203 cd/m^2, ITU-R BT.2408)
 */
void ff_bt2408_hable_pixel(const FFBT2408Hable *s, const float in[3], float out[3]);

#endif /* AVFILTER_BT2408_HABLE_H */
