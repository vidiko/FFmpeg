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

/* Which colour-dependent side data zscale keeps on a transfer change. */
#include <stdio.h>

#include "libavutil/frame.h"
#include "libavutil/gain_map.h"
#include "libavutil/log.h"
#include "libavutil/mastering_display_metadata.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#define HAS(f, t) (av_frame_get_side_data(f, t) ? "kept" : "dropped")

/* Parses and configures desc in g and passes one frame through it. */
static int run_graph(AVFilterGraph *g, AVFrame *in, AVFrame *out, const char *desc)
{
    AVFilterContext *src, *sink;
    int ret;

    ret = avfilter_graph_parse_ptr(g, desc, NULL, NULL, NULL);
    if (ret < 0)
        return ret;
    ret = avfilter_graph_config(g, NULL);
    if (ret < 0)
        return ret;
    src = avfilter_graph_get_filter(g, "buffer@in");
    sink = avfilter_graph_get_filter(g, "buffersink@out");
    if (!src || !sink)
        return AVERROR_BUG;
    ret = av_buffersrc_add_frame_flags(src, in, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
        return ret;
    ret = av_buffersrc_add_frame(src, NULL);
    if (ret < 0)
        return ret;
    return av_buffersink_get_frame(sink, out);
}

/* Runs one frame through buffer -> <chain> -> buffersink. */
static int run_filter(AVFrame *in, AVFrame *out, const char *chain)
{
    AVFilterGraph *g = avfilter_graph_alloc();
    char desc[512];
    int ret;

    if (!g)
        return AVERROR(ENOMEM);
    snprintf(desc, sizeof(desc),
             "buffer@in=video_size=%dx%d:pix_fmt=%d:time_base=1/25:colorspace=%d,%s,buffersink@out",
             in->width, in->height, in->format, in->colorspace, chain);
    ret = run_graph(g, in, out, desc);
    avfilter_graph_free(&g);
    return ret;
}

/* Attaches light levels, a mastering display and gain map parameters. */
static int add_side_data(AVFrame *f)
{
    AVContentLightMetadata *cll = av_content_light_metadata_create_side_data(f);
    AVMasteringDisplayMetadata *md = av_mastering_display_metadata_create_side_data(f);

    if (!cll || !md ||
        !av_gain_map_params_create_side_data(&f->side_data, &f->nb_side_data))
        return AVERROR(ENOMEM);
    cll->MaxCLL = 10000;
    md->has_luminance = 1;
    md->max_luminance = av_make_q(10000, 1);
    md->min_luminance = av_make_q(1, 10000);
    return 0;
}

/* A PQ BT.2020 frame with light levels and gain map parameters. */
static AVFrame *make_frame(void)
{
    AVFrame *f = av_frame_alloc();

    if (!f)
        return NULL;
    f->format = AV_PIX_FMT_GBRPF32;
    f->width = 16;
    f->height = 8;
    f->colorspace = AVCOL_SPC_RGB;
    f->color_primaries = AVCOL_PRI_BT2020;
    f->color_trc = AVCOL_TRC_SMPTE2084;
    if (av_frame_get_buffer(f, 0) < 0 || add_side_data(f) < 0) {
        av_frame_free(&f);
        return NULL;
    }
    for (int p = 0; p < 3; p++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 16; x++)
                ((float *)(f->data[p] + y * f->linesize[p]))[x] = 1.0f;
    return f;
}

/* Tests retention of light level metadata across filter chains. */
int main(void)
{
    static const char *const chains[] = {
        "zscale=t=linear",
        "zscale=t=linear:p=bt709",
        "zscale=t=linear:p=2020",
        "zscale=t=linear:primariesin=bt709",
        "zscale=t=linear:primariesin=2020",
        "zscale=t=linear:primariesin=bt709:p=bt709",
        "zscale=t=linear:tin=bt709",
        "zscale=t=linear:tin=smpte2084",
        "zscale=t=linear:npl=100",
        "zscale=t=linear:npl=1000",
        "zscale=t=linear:npl=1000,tonemap=linear:desat=0",
        "zscale=t=linear,format=gbrp10",
        "zscale=t=linear,format=gbrpf16",
        "zscale=t=bt709",
        "zscale=w=8:h=4",
        "zscale=w=8:h=4:t=linear",
        "zscale=m=2020_ncl:r=limited,format=yuv444p16,zscale=t=linear,format=gbrpf32",
        "zscale=m=2020_ncl:r=limited,format=yuv444p16,zscale=t=linear:min=2020_ncl:rin=limited,format=gbrpf32",
        "zscale=m=2020_ncl:r=limited,format=yuv444p16,zscale=t=linear:min=709,format=gbrpf32",
        "zscale=m=2020_ncl:r=limited,format=yuv444p16,zscale=t=linear:rin=full,format=gbrpf32"
    };
    int fails = 0;

    av_log_set_level(AV_LOG_QUIET);
    for (int i = 0; i < FF_ARRAY_ELEMS(chains); i++) {
        AVFrame *in = make_frame();
        AVFrame *out = av_frame_alloc();
        int ret = in && out ? run_filter(in, out, chains[i]) : AVERROR(ENOMEM);

        if (ret < 0) {
            printf("%s: failed\n", chains[i]);
        } else {
            printf("%s: light level %s, mastering display %s, gain map %s",
                   chains[i], HAS(out, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL),
                   HAS(out, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA),
                   HAS(out, AV_FRAME_DATA_GAIN_MAP_PARAMS));
            /* zimg builds differ in the low digits of a sample; whole numbers
             * still tell the linear scales apart. */
            if (out->format == AV_PIX_FMT_GBRPF32 &&
                out->color_trc == AVCOL_TRC_LINEAR)
                printf(" value %.0f", ((const float *)out->data[0])[0]);
            printf("\n");
        }
        av_frame_free(&in);
        av_frame_free(&out);
        fails += ret < 0;
    }
    return !!fails;
}
