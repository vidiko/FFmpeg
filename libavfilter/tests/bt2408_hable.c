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
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

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

static const struct {
    const char *name;
    unsigned max_cll;
    unsigned mastering_max;
    enum AVColorTransferCharacteristic trc;
    enum AVColorSpace colorspace;
    enum AVColorPrimaries primaries;
} graph_cases[] = {
    { "cll1000",        1000,    0, AVCOL_TRC_LINEAR,    AVCOL_SPC_RGB,   AVCOL_PRI_BT2020 },
    { "cll1000_pq",     1000,    0, AVCOL_TRC_SMPTE2084, AVCOL_SPC_RGB,   AVCOL_PRI_BT2020 },
    { "mastering1000",     0, 1000, AVCOL_TRC_LINEAR,    AVCOL_SPC_RGB,   AVCOL_PRI_BT2020 },
    { "cll4000",        4000,    0, AVCOL_TRC_LINEAR,    AVCOL_SPC_RGB,   AVCOL_PRI_BT2020 },
    { "no_luma_coeffs", 1000,    0, AVCOL_TRC_LINEAR,    AVCOL_SPC_ICTCP, AVCOL_PRI_BT2020 },
    /* 13 is a primaries value without a name. */
    { "bad_primaries",  1000,    0, AVCOL_TRC_LINEAR,    AVCOL_SPC_RGB,   (enum AVColorPrimaries)13 },
};

static char log_line[1024];
static size_t log_len;

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

/* Capture warning and error log messages into log=<line> lines. */
static void log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char buf[1024];
    int n;

    if (level > AV_LOG_WARNING)
        return;
    n = vsnprintf(buf, sizeof(buf), fmt, vl);
    if (n < 0)
        return;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == '\n') {
            log_line[log_len] = '\0';
            printf("log=%s\n", log_line);
            log_len = 0;
        } else if (log_len + 1 < sizeof(log_line)) {
            log_line[log_len++] = buf[i];
        }
    }
}

/* Allocate and configure a buffer -> tonemap=bt2408_hable -> buffersink graph. */
static int open_graph(const AVFrame *in, AVFilterGraph **pg,
                      AVFilterContext **psrc, AVFilterContext **psink)
{
    AVFilterGraph *g = avfilter_graph_alloc();
    AVFilterContext *src = NULL, *tm = NULL, *sink = NULL;
    char args[128];
    int ret;

    if (!g)
        return AVERROR(ENOMEM);
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/25",
             in->width, in->height, in->format);
    ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in",
                                       args, NULL, g);
    if (ret >= 0)
        ret = avfilter_graph_create_filter(&tm, avfilter_get_by_name("tonemap"), "tm",
                                           "bt2408_hable", NULL, g);
    if (ret >= 0)
        ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out",
                                           NULL, NULL, g);
    if (ret >= 0)
        ret = avfilter_link(src, 0, tm, 0);
    if (ret >= 0)
        ret = avfilter_link(tm, 0, sink, 0);
    if (ret >= 0)
        ret = avfilter_graph_config(g, NULL);
    if (ret < 0) {
        avfilter_graph_free(&g);
        return ret;
    }
    *pg = g;
    *psrc = src;
    *psink = sink;
    return 0;
}

/* Push one frame into the source filter and fetch the result from the sink. */
static int push_frame(AVFilterContext *src, AVFilterContext *sink,
                      AVFrame *in, AVFrame *out)
{
    int ret = av_buffersrc_add_frame_flags(src, in, AV_BUFFERSRC_FLAG_KEEP_REF |
                                                    AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT);
    if (ret >= 0)
        ret = av_buffersink_get_frame(sink, out);
    return ret;
}

/* Run buffer -> tonemap=bt2408_hable -> buffersink on one frame. */
static int run_filter(AVFrame *in, AVFrame *out)
{
    AVFilterGraph *g = NULL;
    AVFilterContext *src = NULL, *sink = NULL;
    int ret = open_graph(in, &g, &src, &sink);

    if (ret < 0)
        return ret;
    ret = push_frame(src, sink, in, out);
    avfilter_graph_free(&g);
    return ret;
}

/* Return 1 when planes 0, 1 and 2 hold the same row-0 floats, else 0. */
static int same_planes(const AVFrame *a, const AVFrame *b)
{
    const size_t bytes = a->width * sizeof(float);

    if (a->width != b->width)
        return 0;
    for (int p = 0; p < 3; p++) {
        if (!a->data[p] || !b->data[p] || memcmp(a->data[p], b->data[p], bytes))
            return 0;
    }
    return 1;
}

/* Attach MaxCLL and mastering-display peak side data to a frame. */
static int add_light_levels(AVFrame *f, unsigned max_cll, unsigned mastering_max)
{
    if (max_cll) {
        AVContentLightMetadata *cll = av_content_light_metadata_create_side_data(f);
        if (!cll)
            return AVERROR(ENOMEM);
        cll->MaxCLL = max_cll;
    }
    if (mastering_max) {
        AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(f);
        if (!md)
            return AVERROR(ENOMEM);
        md->max_luminance = av_make_q(mastering_max, 1);
        md->min_luminance = av_make_q(1, 10000);
        md->has_luminance = 1;
    }
    return 0;
}

/* Allocate and populate one row of test colours with metadata and side data. */
static AVFrame *make_frame(unsigned max_cll, unsigned mastering_max,
                           enum AVColorTransferCharacteristic trc,
                           enum AVColorSpace colorspace,
                           enum AVColorPrimaries primaries)
{
    const int w = FF_ARRAY_ELEMS(cases);
    AVFrame *f = av_frame_alloc();
    AVFrameSideData *icc;

    if (!f)
        return NULL;
    f->format          = AV_PIX_FMT_GBRPF32;
    f->width           = w;
    f->height          = 1;
    f->color_primaries = primaries;
    f->color_trc       = trc;
    f->colorspace      = colorspace;
    if (av_frame_get_buffer(f, 0) < 0 || add_light_levels(f, max_cll, mastering_max) < 0) {
        av_frame_free(&f);
        return NULL;
    }
    icc = av_frame_new_side_data(f, AV_FRAME_DATA_ICC_PROFILE, 4);
    if (!icc) {
        av_frame_free(&f);
        return NULL;
    }
    memset(icc->data, 0, 4);
    for (int x = 0; x < w; x++) {
        ((float *)f->data[2])[x] = cases[x].in[0];
        ((float *)f->data[0])[x] = cases[x].in[1];
        ((float *)f->data[1])[x] = cases[x].in[2];
    }
    return f;
}

/* Run each graph case and print its actual result and output metadata. */
static int check_filter(void)
{
    av_log_set_callback(log_callback);

    for (size_t i = 0; i < FF_ARRAY_ELEMS(graph_cases); i++) {
        const char *name = graph_cases[i].name;
        AVFrame *in = make_frame(graph_cases[i].max_cll, graph_cases[i].mastering_max,
                                 graph_cases[i].trc, graph_cases[i].colorspace,
                                 graph_cases[i].primaries);
        AVFrame *out = av_frame_alloc();
        int ret;

        if (!in || !out) {
            av_frame_free(&in);
            av_frame_free(&out);
            printf("alloc=ENOMEM\n");
            return 1;
        }

        ret = run_filter(in, out);
        printf("%s.ret=%s\n", name, ret_name(ret));

        if (ret >= 0) {
            const char *pname = av_color_primaries_name(out->color_primaries);
            const char *tname = av_color_transfer_name(out->color_trc);

            for (size_t n = 0; n < FF_ARRAY_ELEMS(cases); n++) {
                char key[64];
                float rgb[3];

                rgb[0] = ((const float *)out->data[2])[n];
                rgb[1] = ((const float *)out->data[0])[n];
                rgb[2] = ((const float *)out->data[1])[n];
                snprintf(key, sizeof(key), "%s.%s", name, cases[n].name);
                print_rgb(key, rgb);
            }
            printf("%s.primaries=%s\n", name, pname ? pname : "unknown");
            printf("%s.trc=%s\n", name, tname ? tname : "unknown");
            printf("%s.cll=%d\n", name,
                   !!av_frame_get_side_data(out, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL));
            printf("%s.mastering=%d\n", name,
                   !!av_frame_get_side_data(out, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA));
            printf("%s.icc=%d\n", name,
                   !!av_frame_get_side_data(out, AV_FRAME_DATA_ICC_PROFILE));
        }

        av_frame_free(&in);
        av_frame_free(&out);
    }

    return 0;
}

static const struct {
    unsigned max_cll;
    unsigned mastering_max;
    enum AVColorTransferCharacteristic trc;
    enum AVColorSpace colorspace;
    enum AVColorPrimaries primaries;
} seq_cases[] = {
    { 1000, 0, AVCOL_TRC_LINEAR, AVCOL_SPC_RGB, AVCOL_PRI_BT2020 },
    { 4000, 0, AVCOL_TRC_LINEAR, AVCOL_SPC_RGB, AVCOL_PRI_BT2020 },
    { 4000, 0, AVCOL_TRC_LINEAR, AVCOL_SPC_RGB, AVCOL_PRI_BT709  },
};

/* Push successive frames through one graph; return 1 unless each matches a fresh graph. */
static int check_sequence(void)
{
    AVFilterGraph *g = NULL;
    AVFilterContext *src = NULL, *sink = NULL;
    AVFrame *in[FF_ARRAY_ELEMS(seq_cases)] = { 0 };
    AVFrame *out_shared = NULL, *out_fresh = NULL;
    int ok = 1;
    int ret = 0;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(seq_cases); i++) {
        in[i] = make_frame(seq_cases[i].max_cll, seq_cases[i].mastering_max,
                           seq_cases[i].trc, seq_cases[i].colorspace,
                           seq_cases[i].primaries);
        if (!in[i])
            ok = 0;
    }
    out_shared = av_frame_alloc();
    out_fresh  = av_frame_alloc();
    if (!ok || !out_shared || !out_fresh || open_graph(in[0], &g, &src, &sink) < 0) {
        printf("alloc=ENOMEM\n");
        ret = 1;
    } else {
        for (size_t i = 0; i < FF_ARRAY_ELEMS(seq_cases); i++) {
            int ret_shared = push_frame(src, sink, in[i], out_shared);
            int ret_fresh  = run_filter(in[i], out_fresh);
            int fresh = (ret_shared >= 0 && ret_fresh >= 0 &&
                         same_planes(out_shared, out_fresh));

            printf("seq%zu.ret=%s\n", i, ret_name(ret_shared));
            printf("seq%zu.fresh=%d\n", i, fresh);
            if (ret_shared < 0 || !fresh)
                ret = 1;
            av_frame_unref(out_shared);
            av_frame_unref(out_fresh);
        }
    }

    avfilter_graph_free(&g);
    av_frame_free(&out_shared);
    av_frame_free(&out_fresh);
    for (size_t i = 0; i < FF_ARRAY_ELEMS(seq_cases); i++)
        av_frame_free(&in[i]);
    return ret;
}

/* Run the unit test or, with the "filter" argument, the filter graph check. */
int main(int argc, char **argv)
{
    const float low_in[3] = { 0.6f, 0.4f, 0.3f };
    FFBT2408Hable s;
    float out[3];
    int ret;

    if (argc > 1 && !strcmp(argv[1], "filter")) {
        int ret_filter = check_filter();
        int ret_seq = check_sequence();
        return ret_filter ? ret_filter : ret_seq;
    }

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
