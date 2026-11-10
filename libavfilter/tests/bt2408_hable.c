/*
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
#include <stdio.h>

#include "libavutil/error.h"

#include "libavfilter/bt2408_hable.c"

#define PEAK 1000.0

/*
 * inputs are BT.2020, 1.0 = 100 cd/m^2, peak 1000 cd/m^2; the expected output is
 * tests/ref/fate/filter-bt2408-hable, which agrees within 2e-4 with a double precision
 * model of the libplacebo v7.360.1 colour map, written separately from this port.
 */
static const struct {
    const char *name;
    float in[3];
} cases[] = {
    { "grey",         { 0.5f,  0.5f,  0.5f  } },
    { "white",        { 2.03f, 2.03f, 2.03f } },
    { "warm",         { 3.0f,  2.0f,  1.5f  } },
    { "sky",          { 0.8f,  1.2f,  2.5f  } },
    { "clipped_blue", { 0.05f, 0.3f,  4.0f  } },
    { "near_peak",    { 6.0f,  5.5f,  5.8f  } },
    { "black",        { 0.0f,  0.0f,  0.0f  } },
};

/* Print an RGB triplet as three key=value lines. */
static void print_rgb(const char *name, const float rgb[3])
{
    printf("%s.r=%.7f\n", name, rgb[0]);
    printf("%s.g=%.7f\n", name, rgb[1]);
    printf("%s.b=%.7f\n", name, rgb[2]);
}

/* Return the cross-platform name for an FFmpeg error return code. */
static const char *ret_name(int ret)
{
    if (ret >= 0)
        return "ok";
    if (ret == AVERROR(EINVAL))
        return "EINVAL";
    if (ret == AVERROR(ENOTSUP))
        return "ENOTSUP";
    return "other";
}

/* Run the BT.2408 Hable tone mapping unit test and print actual values. */
int main(void)
{
    const float low_in[3] = { 0.6f, 0.4f, 0.3f };
    FFBT2408Hable s;
    float out[3];
    int ret;

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, PEAK);
    if (ret < 0) {
        printf("init=%s\n", ret_name(ret));
        return 1;
    }
    for (size_t n = 0; n < FF_ARRAY_ELEMS(cases); n++) {
        ff_bt2408_hable_pixel(&s, cases[n].in, out);
        print_rgb(cases[n].name, out);
    }

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, 4 * PEAK);
    if (ret < 0) {
        printf("init=%s\n", ret_name(ret));
        return 1;
    }
    ff_bt2408_hable_pixel(&s, cases[2].in, out);
    print_rgb("warm.peak4000", out);

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, 50.0);
    if (ret < 0) {
        printf("init=%s\n", ret_name(ret));
        return 1;
    }
    ff_bt2408_hable_pixel(&s, low_in, out);
    print_rgb("low.peak50", out);

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, 100.0);
    if (ret < 0) {
        printf("init=%s\n", ret_name(ret));
        return 1;
    }
    ff_bt2408_hable_pixel(&s, low_in, out);
    print_rgb("low.peak100", out);

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, 0.0);
    printf("zero_peak=%s\n", ret_name(ret));

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_BT2020, NAN);
    printf("nan_peak=%s\n", ret_name(ret));

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_UNSPECIFIED, PEAK);
    printf("unspecified_primaries=%s\n", ret_name(ret));

    ret = ff_bt2408_hable_init(&s, AVCOL_PRI_SMPTE431, PEAK);
    printf("dci_white_point=%s\n", ret_name(ret));

    return 0;
}
