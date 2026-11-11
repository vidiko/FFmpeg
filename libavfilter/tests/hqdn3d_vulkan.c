/*
 * Copyright (c) 2026 Lusoris
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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vulkan.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

/* Return test frame dimensions for frame index n across three distinct resolution buckets. */
static void get_frame_dims(int n, int *w, int *h)
{
    if (n < 4) {
        *w = 128;
        *h = 72;
    } else if (n < 8) {
        *w = 96;
        *h = 56;
    } else {
        *w = 160;
        *h = 96;
    }
}

/* Generate pseudo-random byte using linear congruential generator and advance 32-bit state. */
static uint8_t lcg_rand(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return (uint8_t)(*state >> 16);
}

/* Allocate YUV420P frame with index-based dimensions and fill planes with deterministic noise. */
static int create_input_frame(int n, uint32_t *seed, AVFrame **out_frame)
{
    int w, h, err;
    AVFrame *f = av_frame_alloc();
    if (!f)
        return AVERROR(ENOMEM);

    get_frame_dims(n, &w, &h);
    f->format = AV_PIX_FMT_YUV420P;
    f->width  = w;
    f->height = h;
    f->pts    = n;

    err = av_frame_get_buffer(f, 0);
    if (err < 0) {
        av_frame_free(&f);
        return err;
    }

    for (int p = 0; p < 3; p++) {
        int pw = (p == 0) ? w : AV_CEIL_RSHIFT(w, 1);
        int ph = (p == 0) ? h : AV_CEIL_RSHIFT(h, 1);
        for (int y = 0; y < ph; y++) {
            uint8_t *dst = f->data[p] + y * f->linesize[p];
            for (int x = 0; x < pw; x++)
                dst[x] = lcg_rand(seed);
        }
    }

    *out_frame = f;
    return 0;
}

/* Create and initialize Vulkan hardware frames context with storage and transfer usage flags. */
static int create_vk_frames_ctx(AVBufferRef *device_ref, int w, int h,
                                enum AVPixelFormat sw_format, AVBufferRef **frames_out)
{
    AVBufferRef *frames_ref = av_hwframe_ctx_alloc(device_ref);
    AVHWFramesContext *hwfc;
    AVVulkanFramesContext *vk_hwfc;
    int err;

    if (!frames_ref)
        return AVERROR(ENOMEM);

    hwfc = (AVHWFramesContext *)frames_ref->data;
    hwfc->format    = AV_PIX_FMT_VULKAN;
    hwfc->sw_format = sw_format;
    hwfc->width     = w;
    hwfc->height    = h;

    vk_hwfc = hwfc->hwctx;
    vk_hwfc->tiling = VK_IMAGE_TILING_OPTIMAL;
    vk_hwfc->usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    err = av_hwframe_ctx_init(frames_ref);
    if (err < 0) {
        av_buffer_unref(&frames_ref);
        return err;
    }

    *frames_out = frames_ref;
    return 0;
}

/* Allocate GPU frame from hardware frames context, upload software frame data, and copy props. */
static int upload_vk_frame(AVBufferRef *frames_ref, AVFrame *sw_in, AVFrame **vk_out)
{
    AVFrame *vk_f = av_frame_alloc();
    int err;

    if (!vk_f)
        return AVERROR(ENOMEM);

    err = av_hwframe_get_buffer(frames_ref, vk_f, 0);
    if (err < 0) {
        av_frame_free(&vk_f);
        return err;
    }

    err = av_hwframe_transfer_data(vk_f, sw_in, 0);
    if (err < 0) {
        av_frame_free(&vk_f);
        return err;
    }

    err = av_frame_copy_props(vk_f, sw_in);
    if (err < 0) {
        av_frame_free(&vk_f);
        return err;
    }

    *vk_out = vk_f;
    return 0;
}

/* Transfer frame pixels from Vulkan hardware surface to a new software frame in the frames context format. */
static int download_vk_frame(AVFrame *vk_in, AVFrame **sw_out)
{
    AVFrame *sw_f = av_frame_alloc();
    int err;

    if (!sw_f)
        return AVERROR(ENOMEM);

    sw_f->format = ((AVHWFramesContext *)vk_in->hw_frames_ctx->data)->sw_format;
    err = av_hwframe_transfer_data(sw_f, vk_in, 0);
    if (err < 0) {
        av_frame_free(&sw_f);
        return err;
    }

    err = av_frame_copy_props(sw_f, vk_in);
    if (err < 0) {
        av_frame_free(&sw_f);
        return err;
    }

    *sw_out = sw_f;
    return 0;
}

/* Compare pixel data plane-by-plane between CPU and GPU frames, returning 1 if identical or 0. */
static int compare_planes(const AVFrame *cpu_f, const AVFrame *gpu_f)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(cpu_f->format);

    if (!desc || cpu_f->format != gpu_f->format ||
        cpu_f->width != gpu_f->width || cpu_f->height != gpu_f->height)
        return 0;

    for (int p = 0; p < 3; p++) {
        int bytes = av_image_get_linesize(cpu_f->format, cpu_f->width, p);
        int ph = (p == 0) ? cpu_f->height : AV_CEIL_RSHIFT(cpu_f->height, desc->log2_chroma_h);

        if (bytes <= 0)
            return 0;

        for (int y = 0; y < ph; y++) {
            const uint8_t *c = cpu_f->data[p] + y * cpu_f->linesize[p];
            const uint8_t *g = gpu_f->data[p] + y * gpu_f->linesize[p];
            if (memcmp(c, g, bytes) != 0)
                return 0;
        }
    }
    return 1;
}

/* Connect source to middle filter, middle to sink, and validate complete filter graph layout. */
static int link_and_config(AVFilterGraph *graph, AVFilterContext *src,
                           AVFilterContext *mid, AVFilterContext *sink)
{
    int err = avfilter_link(src, 0, mid, 0);
    if (err < 0)
        return err;

    err = avfilter_link(mid, 0, sink, 0);
    if (err < 0)
        return err;

    return avfilter_graph_config(graph, NULL);
}

/* Build CPU filter graph with buffer source, hqdn3d filter, and sink, returning 0 on success. */
static int init_cpu_graph(AVFilterGraph **graph_out,
                          AVFilterContext **src_out,
                          AVFilterContext **sink_out,
                          int w, int h, enum AVPixelFormat fmt)
{
    AVFilterGraph *graph = avfilter_graph_alloc();
    const AVFilter *buf_filt = avfilter_get_by_name("buffer");
    const AVFilter *hq_filt = avfilter_get_by_name("hqdn3d");
    const AVFilter *sink_filt = avfilter_get_by_name("buffersink");
    AVFilterContext *src_ctx = NULL, *hq_ctx = NULL, *sink_ctx = NULL;
    char args[128];
    int err;

    if (!graph)
        return AVERROR(ENOMEM);

    if (!buf_filt || !hq_filt || !sink_filt) {
        if (!buf_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "buffer");
        if (!hq_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "hqdn3d");
        if (!sink_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "buffersink");
        avfilter_graph_free(&graph);
        return AVERROR_FILTER_NOT_FOUND;
    }

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%s:time_base=1/25:pixel_aspect=1/1",
             w, h, av_get_pix_fmt_name(fmt));
    err = avfilter_graph_create_filter(&src_ctx, buf_filt, "in", args, NULL, graph);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    err = avfilter_graph_create_filter(&hq_ctx, hq_filt, "hqdn3d", NULL, NULL, graph);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    err = avfilter_graph_create_filter(&sink_ctx, sink_filt, "out", NULL, NULL, graph);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    err = link_and_config(graph, src_ctx, hq_ctx, sink_ctx);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    *graph_out = graph;
    *src_out = src_ctx;
    *sink_out = sink_ctx;
    return 0;
}

/* Configure Vulkan buffer source with hardware frames context and video stream parameters. */
static int init_gpu_buffersrc(AVFilterGraph *graph, const AVFilter *buf_filt,
                              AVBufferRef *initial_frames_ref, AVFilterContext **src_out)
{
    AVHWFramesContext *hwfc = (AVHWFramesContext *)initial_frames_ref->data;
    AVFilterContext *src_ctx = avfilter_graph_alloc_filter(graph, buf_filt, "in");
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    int err;

    if (!src_ctx || !par) {
        av_free(par);
        return AVERROR(ENOMEM);
    }
    par->format        = AV_PIX_FMT_VULKAN;
    par->width         = hwfc->width;
    par->height        = hwfc->height;
    par->time_base     = (AVRational){ 1, 25 };
    par->sample_aspect_ratio = (AVRational){ 1, 1 };
    par->hw_frames_ctx = initial_frames_ref;

    err = av_buffersrc_parameters_set(src_ctx, par);
    av_free(par);
    if (err < 0)
        return err;

    err = avfilter_init_dict(src_ctx, NULL);
    if (err < 0)
        return err;

    *src_out = src_ctx;
    return 0;
}

/* Build GPU filter graph linking Vulkan buffer source, hqdn3d_vulkan filter, and buffer sink. */
static int init_gpu_graph(AVFilterGraph **graph_out,
                          AVFilterContext **src_out,
                          AVFilterContext **sink_out,
                          AVBufferRef *vk_device_ref,
                          AVBufferRef *initial_frames_ref)
{
    AVFilterGraph *graph = avfilter_graph_alloc();
    const AVFilter *buf_filt = avfilter_get_by_name("buffer");
    const AVFilter *hq_filt = avfilter_get_by_name("hqdn3d_vulkan");
    const AVFilter *sink_filt = avfilter_get_by_name("buffersink");
    AVFilterContext *src_ctx = NULL, *hq_ctx = NULL, *sink_ctx = NULL;
    int err;

    if (!graph)
        return AVERROR(ENOMEM);

    if (!buf_filt || !hq_filt || !sink_filt) {
        if (!buf_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "buffer");
        if (!hq_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "hqdn3d_vulkan");
        if (!sink_filt)
            av_log(NULL, AV_LOG_ERROR, "filter %s not available\n", "buffersink");
        avfilter_graph_free(&graph);
        return AVERROR_FILTER_NOT_FOUND;
    }

    err = init_gpu_buffersrc(graph, buf_filt, initial_frames_ref, &src_ctx);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    hq_ctx = avfilter_graph_alloc_filter(graph, hq_filt, "hqdn3d_vulkan");
    if (!hq_ctx) {
        avfilter_graph_free(&graph);
        return AVERROR(ENOMEM);
    }
    hq_ctx->hw_device_ctx = av_buffer_ref(vk_device_ref);
    err = avfilter_init_dict(hq_ctx, NULL);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    err = avfilter_graph_create_filter(&sink_ctx, sink_filt, "out", NULL, NULL, graph);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    err = link_and_config(graph, src_ctx, hq_ctx, sink_ctx);
    if (err < 0) {
        avfilter_graph_free(&graph);
        return err;
    }

    *graph_out = graph;
    *src_out = src_ctx;
    *sink_out = sink_ctx;
    return 0;
}

/* Push software frame into CPU graph, retrieve denoised output frame, and return 0 on success. */
static int run_cpu_step(AVFilterContext *cpu_src, AVFilterContext *cpu_sink,
                        AVFrame *sw_frame, AVFrame **cpu_out)
{
    AVFrame *cpu_in = av_frame_clone(sw_frame);
    AVFrame *out = av_frame_alloc();
    int err;

    if (!cpu_in || !out) {
        av_frame_free(&cpu_in);
        av_frame_free(&out);
        return AVERROR(ENOMEM);
    }

    err = av_buffersrc_add_frame(cpu_src, cpu_in);
    av_frame_free(&cpu_in);
    if (err < 0) {
        av_frame_free(&out);
        return err;
    }

    err = av_buffersink_get_frame(cpu_sink, out);
    if (err < 0) {
        av_frame_free(&out);
        return err;
    }

    *cpu_out = out;
    return 0;
}

/* Upload the software frame, run the GPU graph and download the output. */
static int run_gpu_step(AVFilterContext *gpu_src, AVFilterContext *gpu_sink,
                        AVBufferRef *upload_ctx, AVFrame *sw_frame,
                        AVFrame **gpu_out_sw)
{
    AVFrame *gpu_in_vk = NULL;
    AVFrame *gpu_out_vk = av_frame_alloc();
    int err;

    if (!gpu_out_vk)
        return AVERROR(ENOMEM);

    err = upload_vk_frame(upload_ctx, sw_frame, &gpu_in_vk);
    if (err < 0) {
        av_frame_free(&gpu_out_vk);
        return err;
    }

    err = av_buffersrc_add_frame(gpu_src, gpu_in_vk);
    av_frame_free(&gpu_in_vk);
    if (err < 0) {
        av_frame_free(&gpu_out_vk);
        return err;
    }

    err = av_buffersink_get_frame(gpu_sink, gpu_out_vk);
    if (err < 0) {
        av_frame_free(&gpu_out_vk);
        return err;
    }

    err = download_vk_frame(gpu_out_vk, gpu_out_sw);
    av_frame_free(&gpu_out_vk);
    if (err < 0)
        return err;

    return 0;
}

typedef struct ResetTest {
    AVBufferRef *vk_device_ref;
    AVBufferRef *upload_ctxs[5];
    AVFilterGraph *cpu_graph, *gpu_graph;
    AVFilterContext *cpu_src, *cpu_sink, *gpu_src, *gpu_sink;
} ResetTest;

/* Run one frame through CPU and GPU pipelines, verify pixel equality, and print status line. */
static int process_one_frame(ResetTest *t, int n, uint32_t *seed, int *is_same)
{
    AVFrame *sw_frame = NULL, *cpu_out = NULL, *gpu_out = NULL;
    int bucket = (n < 4) ? 0 : ((n < 8) ? 1 : 2);
    int err;

    err = create_input_frame(n, seed, &sw_frame);
    if (err < 0)
        return err;

    err = run_cpu_step(t->cpu_src, t->cpu_sink, sw_frame, &cpu_out);
    if (err < 0) {
        av_frame_free(&sw_frame);
        return err;
    }

    err = run_gpu_step(t->gpu_src, t->gpu_sink, t->upload_ctxs[bucket], sw_frame, &gpu_out);
    av_frame_free(&sw_frame);
    if (err < 0) {
        av_frame_free(&cpu_out);
        return err;
    }

    *is_same = compare_planes(cpu_out, gpu_out);
    printf("%d %d %d %s\n", n, gpu_out->width, gpu_out->height,
           *is_same ? "same" : "DIFF");
    fflush(stdout);

    av_frame_free(&cpu_out);
    av_frame_free(&gpu_out);
    return 0;
}

/* Return short symbolic name for common error codes without platform strerror variances. */
static const char *err_to_name(int err)
{
    if (err == AVERROR(EINVAL))
        return "EINVAL";
    if (err == AVERROR(ENOTSUP))
        return "ENOTSUP";
    if (err == AVERROR(ENOMEM))
        return "ENOMEM";
    if (err == AVERROR_EXTERNAL)
        return "EXTERNAL";
    return "other";
}

/* Filter must reject in-stream format change with AVERROR(EINVAL); return any other error. */
static int run_format_step(ResetTest *t, int n, enum AVPixelFormat fmt,
                           AVBufferRef *frames_ref, int *refused)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    /* Drop the endianness suffix so the output matches the reference on any host. */
    const char *format_name = fmt == AV_PIX_FMT_YUV420P10 ? "yuv420p10" :
                              av_get_pix_fmt_name(fmt);
    AVFrame *sw_f = av_frame_alloc();
    AVFrame *gpu_in_vk = NULL;
    AVFrame *out = av_frame_alloc();
    int err, refused_err = 0;

    if (!desc || !sw_f || !out) {
        av_frame_free(&sw_f);
        av_frame_free(&out);
        return AVERROR(ENOMEM);
    }

    sw_f->format = fmt;
    sw_f->width  = 160;
    sw_f->height = 96;
    sw_f->pts    = n;

    err = av_frame_get_buffer(sw_f, 0);
    if (err < 0) {
        av_frame_free(&sw_f);
        av_frame_free(&out);
        return err;
    }

    for (int p = 0; p < desc->nb_components && sw_f->data[p]; p++) {
        int plane_h = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(sw_f->height, desc->log2_chroma_h) : sw_f->height;
        for (int y = 0; y < plane_h; y++)
            memset(sw_f->data[p] + y * sw_f->linesize[p], 0, FFABS(sw_f->linesize[p]));
    }

    err = upload_vk_frame(frames_ref, sw_f, &gpu_in_vk);
    av_frame_free(&sw_f);
    if (err < 0) {
        av_frame_free(&out);
        return err;
    }

    err = av_buffersrc_add_frame(t->gpu_src, gpu_in_vk);
    av_frame_free(&gpu_in_vk);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        refused_err = err;
        *refused = 1;
    } else {
        err = av_buffersink_get_frame(t->gpu_sink, out);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            refused_err = err;
            *refused = 1;
        } else {
            *refused = 0;
        }
    }
    av_frame_free(&out);

    if (*refused)
        printf("%d 160 96 %s refused %s\n", n, format_name,
               err_to_name(refused_err));
    else
        printf("%d 160 96 %s accepted\n", n, format_name);
    fflush(stdout);
    return 0;
}

/* Run a first frame larger than the link through fresh CPU/GPU graphs, verifying parity. */
static int run_first_frame_step(ResetTest *t, int n, uint32_t *seed, int *is_same)
{
    AVFilterGraph *cpu_graph = NULL, *gpu_graph = NULL;
    AVFilterContext *cpu_src = NULL, *cpu_sink = NULL;
    AVFilterContext *gpu_src = NULL, *gpu_sink = NULL;
    AVFrame *sw_frame = NULL, *cpu_out = NULL, *gpu_out = NULL;
    int err;

    *is_same = 0;

    err = init_cpu_graph(&cpu_graph, &cpu_src, &cpu_sink, 128, 72, AV_PIX_FMT_YUV420P);
    if (err < 0)
        goto end;

    err = init_gpu_graph(&gpu_graph, &gpu_src, &gpu_sink, t->vk_device_ref, t->upload_ctxs[0]);
    if (err < 0)
        goto end;

    err = create_input_frame(n, seed, &sw_frame);
    if (err < 0)
        goto end;

    err = run_cpu_step(cpu_src, cpu_sink, sw_frame, &cpu_out);
    if (err < 0)
        goto end;

    err = run_gpu_step(gpu_src, gpu_sink, t->upload_ctxs[2], sw_frame, &gpu_out);
    if (err < 0)
        goto end;

    *is_same = compare_planes(cpu_out, gpu_out);
    printf("%d %d %d first %s\n", n, gpu_out->width, gpu_out->height,
           *is_same ? "same" : "DIFF");
    fflush(stdout);

end:
    av_frame_free(&sw_frame);
    av_frame_free(&cpu_out);
    av_frame_free(&gpu_out);
    avfilter_graph_free(&cpu_graph);
    avfilter_graph_free(&gpu_graph);
    return err;
}

/* Test whether a first frame with format mismatch against link format is refused. */
static int run_f1_step(AVBufferRef *vk_device_ref, AVBufferRef *upload_ctx_444)
{
    AVFilterGraph *graph = NULL;
    AVFilterContext *gpu_src = NULL, *gpu_sink = NULL;
    AVBufferRef *link_frames_ref = NULL;
    AVFrame *sw_f = NULL, *gpu_in_vk = NULL, *gpu_out = NULL;
    const AVPixFmtDescriptor *desc;
    int err, refused_err = 0, refused = 0;

    err = create_vk_frames_ctx(vk_device_ref, 160, 96, AV_PIX_FMT_YUV420P, &link_frames_ref);
    if (err < 0)
        goto end;

    err = init_gpu_graph(&graph, &gpu_src, &gpu_sink, vk_device_ref, link_frames_ref);
    if (err < 0)
        goto end;

    sw_f = av_frame_alloc();
    gpu_out = av_frame_alloc();
    if (!sw_f || !gpu_out) {
        err = AVERROR(ENOMEM);
        goto end;
    }

    sw_f->format = AV_PIX_FMT_YUV444P;
    sw_f->width  = 160;
    sw_f->height = 96;
    sw_f->pts    = 0;

    err = av_frame_get_buffer(sw_f, 0);
    if (err < 0)
        goto end;

    desc = av_pix_fmt_desc_get(AV_PIX_FMT_YUV444P);
    for (int p = 0; p < desc->nb_components && sw_f->data[p]; p++) {
        for (int y = 0; y < sw_f->height; y++)
            memset(sw_f->data[p] + y * sw_f->linesize[p], 0, FFABS(sw_f->linesize[p]));
    }

    err = upload_vk_frame(upload_ctx_444, sw_f, &gpu_in_vk);
    av_frame_free(&sw_f);
    if (err < 0)
        goto end;

    err = av_buffersrc_add_frame(gpu_src, gpu_in_vk);
    av_frame_free(&gpu_in_vk);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        refused_err = err;
        refused = 1;
    } else {
        err = av_buffersink_get_frame(gpu_sink, gpu_out);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            refused_err = err;
            refused = 1;
        } else {
            refused = 0;
        }
    }

    if (refused)
        printf("f1 160 96 yuv444p refused %s\n", err_to_name(refused_err));
    else
        printf("f1 160 96 yuv444p accepted\n");
    fflush(stdout);

    err = 0;

end:
    av_frame_free(&sw_f);
    av_frame_free(&gpu_in_vk);
    av_frame_free(&gpu_out);
    av_buffer_unref(&link_frames_ref);
    avfilter_graph_free(&graph);
    return err;
}

/* Frame 0 has a black first column and frames 1 and 2 are at the p012 maximum.
 * On this input hqdn3d on yuv420p12le computes 4096 in frames 1 and 2, which
 * p012 cannot hold. Print the smallest and largest sample and the OR of the
 * low 4 bits of each output frame. */
static int run_f4_step(AVBufferRef *vk_device_ref)
{
    AVBufferRef *frames_ref = NULL;
    AVFilterGraph *graph = NULL;
    AVFilterContext *gpu_src = NULL, *gpu_sink = NULL;
    AVFrame *sw_f = NULL, *gpu_in_vk = NULL, *gpu_out_vk = NULL, *sw_out = NULL;
    int err;

    err = create_vk_frames_ctx(vk_device_ref, 160, 96, AV_PIX_FMT_P012, &frames_ref);
    if (err < 0)
        goto end;

    err = init_gpu_graph(&graph, &gpu_src, &gpu_sink, vk_device_ref, frames_ref);
    if (err < 0)
        goto end;

    sw_f = av_frame_alloc();
    gpu_out_vk = av_frame_alloc();
    sw_out = av_frame_alloc();
    if (!sw_f || !gpu_out_vk || !sw_out) {
        err = AVERROR(ENOMEM);
        goto end;
    }

    sw_f->format = AV_PIX_FMT_P012;
    sw_f->width  = 160;
    sw_f->height = 96;

    err = av_frame_get_buffer(sw_f, 0);
    if (err < 0)
        goto end;

    for (int n = 0; n < 3; n++) {
        uint32_t min_val = 0xFFFF;
        uint32_t max_val = 0;
        uint32_t low_or = 0;

        err = av_frame_make_writable(sw_f);
        if (err < 0)
            goto end;

        for (int y = 0; y < 96; y++) {
            uint16_t *dst = (uint16_t *)(sw_f->data[0] + y * sw_f->linesize[0]);
            for (int x = 0; x < 160; x++)
                dst[x] = (n == 0 && x == 0) ? 0 : 0xFFF0;
        }
        for (int y = 0; y < 48; y++) {
            uint16_t *dst = (uint16_t *)(sw_f->data[1] + y * sw_f->linesize[1]);
            for (int x = 0; x < 160; x++)
                dst[x] = (n == 0 && x < 2) ? 0 : 0xFFF0;
        }

        sw_f->pts = n;
        err = upload_vk_frame(frames_ref, sw_f, &gpu_in_vk);
        if (err < 0)
            goto end;

        err = av_buffersrc_add_frame(gpu_src, gpu_in_vk);
        av_frame_free(&gpu_in_vk);
        if (err < 0)
            goto end;

        err = av_buffersink_get_frame(gpu_sink, gpu_out_vk);
        if (err < 0)
            goto end;

        sw_out->format = AV_PIX_FMT_P012;
        sw_out->width  = 160;
        sw_out->height = 96;
        err = av_hwframe_transfer_data(sw_out, gpu_out_vk, 0);
        av_frame_unref(gpu_out_vk);
        if (err < 0)
            goto end;

        for (int y = 0; y < 96; y++) {
            const uint16_t *src = (const uint16_t *)(sw_out->data[0] + y * sw_out->linesize[0]);
            for (int x = 0; x < 160; x++) {
                uint16_t v = src[x];
                if (v < min_val)
                    min_val = v;
                if (v > max_val)
                    max_val = v;
                low_or |= (v & 0xFu);
            }
        }
        for (int y = 0; y < 48; y++) {
            const uint16_t *src = (const uint16_t *)(sw_out->data[1] + y * sw_out->linesize[1]);
            for (int x = 0; x < 160; x++) {
                uint16_t v = src[x];
                if (v < min_val)
                    min_val = v;
                if (v > max_val)
                    max_val = v;
                low_or |= (v & 0xFu);
            }
        }

        printf("p012 %d min %u max %u low %u\n", n, min_val, max_val, low_or);
        fflush(stdout);
        av_frame_unref(sw_out);
    }

    err = 0;

end:
    av_frame_free(&sw_f);
    av_frame_free(&gpu_in_vk);
    av_frame_free(&gpu_out_vk);
    av_frame_free(&sw_out);
    av_buffer_unref(&frames_ref);
    avfilter_graph_free(&graph);
    return err;
}

/* Fill a 2x2 yuv420p12 frame: frame 0 all zero, frame 1 luma top row at 4095. */
static int create_neg_frame(int n, AVFrame **out_frame)
{
    AVFrame *f = av_frame_alloc();
    int err;

    if (!f)
        return AVERROR(ENOMEM);

    f->format = AV_PIX_FMT_YUV420P12;
    f->width  = 2;
    f->height = 2;
    f->pts    = n;

    err = av_frame_get_buffer(f, 0);
    if (err < 0) {
        av_frame_free(&f);
        return err;
    }

    memset(f->data[0], 0, FFABS(f->linesize[0]));
    memset(f->data[0] + f->linesize[0], 0, FFABS(f->linesize[0]));
    memset(f->data[1], 0, FFABS(f->linesize[1]));
    memset(f->data[2], 0, FFABS(f->linesize[2]));

    if (n == 1) {
        ((uint16_t *)f->data[0])[0] = 4095;
        ((uint16_t *)f->data[0])[1] = 4095;
    }

    *out_frame = f;
    return 0;
}

/* Frame 1 drives the temporal result at luma (0,1) to -1, which hqdn3d stores
 * as 65535. Print the CPU luma and whether the GPU output matches the CPU. */
static int run_neg_step(AVBufferRef *vk_device_ref)
{
    AVBufferRef *frames_ref = NULL;
    AVFilterGraph *cpu_graph = NULL, *gpu_graph = NULL;
    AVFilterContext *cpu_src = NULL, *cpu_sink = NULL;
    AVFilterContext *gpu_src = NULL, *gpu_sink = NULL;
    AVFrame *sw_f = NULL, *cpu_out = NULL, *gpu_out = NULL;
    int err;

    err = create_vk_frames_ctx(vk_device_ref, 2, 2, AV_PIX_FMT_YUV420P12, &frames_ref);
    if (err < 0)
        goto end;

    err = init_cpu_graph(&cpu_graph, &cpu_src, &cpu_sink, 2, 2, AV_PIX_FMT_YUV420P12);
    if (err < 0)
        goto end;

    err = init_gpu_graph(&gpu_graph, &gpu_src, &gpu_sink, vk_device_ref, frames_ref);
    if (err < 0)
        goto end;

    for (int n = 0; n < 2; n++) {
        const uint16_t *y0;
        const uint16_t *y1;

        err = create_neg_frame(n, &sw_f);
        if (err < 0)
            goto end;

        err = run_cpu_step(cpu_src, cpu_sink, sw_f, &cpu_out);
        if (err < 0)
            goto end;

        err = run_gpu_step(gpu_src, gpu_sink, frames_ref, sw_f, &gpu_out);
        av_frame_free(&sw_f);
        if (err < 0)
            goto end;

        y0 = (const uint16_t *)cpu_out->data[0];
        y1 = (const uint16_t *)(cpu_out->data[0] + cpu_out->linesize[0]);
        printf("neg %d %d %d %s luma %d %d %d %d\n", n, gpu_out->width, gpu_out->height,
               compare_planes(cpu_out, gpu_out) ? "same" : "DIFF",
               y0[0], y0[1], y1[0], y1[1]);
        fflush(stdout);

        av_frame_free(&cpu_out);
        av_frame_free(&gpu_out);
    }

    err = 0;

end:
    av_frame_free(&sw_f);
    av_frame_free(&cpu_out);
    av_frame_free(&gpu_out);
    av_buffer_unref(&frames_ref);
    avfilter_graph_free(&cpu_graph);
    avfilter_graph_free(&gpu_graph);
    return err;
}

/* Release all Vulkan hardware upload contexts, filter graphs, and device context reference. */
static void cleanup_all(ResetTest *t)
{
    for (int i = 0; i < 5; i++)
        av_buffer_unref(&t->upload_ctxs[i]);
    avfilter_graph_free(&t->cpu_graph);
    avfilter_graph_free(&t->gpu_graph);
    av_buffer_unref(&t->vk_device_ref);
}

/* Initialize Vulkan hardware device, upload frames contexts for each test bucket, and graphs. */
static int setup_all(const char *dev_idx, ResetTest *t)
{
    int err;

    err = av_hwdevice_ctx_create(&t->vk_device_ref, AV_HWDEVICE_TYPE_VULKAN, dev_idx, NULL, 0);
    if (err < 0)
        return err;

    err = create_vk_frames_ctx(t->vk_device_ref, 128, 72, AV_PIX_FMT_YUV420P, &t->upload_ctxs[0]);
    if (err < 0)
        return err;

    err = create_vk_frames_ctx(t->vk_device_ref, 96, 56, AV_PIX_FMT_YUV420P, &t->upload_ctxs[1]);
    if (err < 0)
        return err;

    err = create_vk_frames_ctx(t->vk_device_ref, 160, 96, AV_PIX_FMT_YUV420P, &t->upload_ctxs[2]);
    if (err < 0)
        return err;

    err = create_vk_frames_ctx(t->vk_device_ref, 160, 96, AV_PIX_FMT_YUV420P10, &t->upload_ctxs[3]);
    if (err < 0)
        return err;

    err = create_vk_frames_ctx(t->vk_device_ref, 160, 96, AV_PIX_FMT_YUV444P, &t->upload_ctxs[4]);
    if (err < 0)
        return err;

    err = init_cpu_graph(&t->cpu_graph, &t->cpu_src, &t->cpu_sink, 128, 72, AV_PIX_FMT_YUV420P);
    if (err < 0)
        return err;

    err = init_gpu_graph(&t->gpu_graph, &t->gpu_src, &t->gpu_sink, t->vk_device_ref, t->upload_ctxs[0]);
    if (err < 0)
        return err;

    return 0;
}

/* Verify parity on resets, first frame larger than link, format rejection, negative result, and F1/F4 cases. */
int main(int argc, char **argv)
{
    ResetTest t = { 0 };
    const char *dev_idx = NULL;
    uint32_t seed = 0x12345678;
    int err;
    int first_same = 0;
    int refused_10 = 0, refused_444 = 0;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [vulkan device index]\n", argv[0]);
        return 1;
    }
    if (argc >= 2)
        dev_idx = (strncmp(argv[1], "vk:", 3) == 0) ? argv[1] + 3 : argv[1];

    av_log_set_level(AV_LOG_ERROR);

    err = setup_all(dev_idx, &t);
    if (err < 0) {
        fprintf(stderr, "Setup failed: %s (%d)\n", err_to_name(err), err);
        cleanup_all(&t);
        return 1;
    }

    for (int n = 0; n < 12; n++) {
        int is_same = 0;
        err = process_one_frame(&t, n, &seed, &is_same);
        if (err < 0)
            goto end;
    }

    err = run_first_frame_step(&t, 14, &seed, &first_same);
    if (err < 0)
        goto end;

    /* Test yuv420p10 first and abort if accepted, avoiding chroma buffer overflow from yuv444p on unpatched filters. */
    err = run_format_step(&t, 12, AV_PIX_FMT_YUV420P10, t.upload_ctxs[3], &refused_10);
    if (err < 0)
        goto end;
    if (!refused_10) {
        printf("13 160 96 yuv444p skipped\n");
        fflush(stdout);
    } else {
        err = run_format_step(&t, 13, AV_PIX_FMT_YUV444P, t.upload_ctxs[4], &refused_444);
        if (err < 0)
            goto end;
    }

    err = run_f1_step(t.vk_device_ref, t.upload_ctxs[4]);
    if (err < 0)
        goto end;

    err = run_f4_step(t.vk_device_ref);
    if (err < 0)
        goto end;

    err = run_neg_step(t.vk_device_ref);

end:
    if (err < 0)
        fprintf(stderr, "Test failed: %s (%d)\n", err_to_name(err), err);
    cleanup_all(&t);
    return err < 0;
}
