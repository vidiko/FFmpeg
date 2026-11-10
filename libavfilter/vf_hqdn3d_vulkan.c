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

#include <float.h>
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "vulkan_filter.h"
#include "filters.h"
#include "video.h"
#include "vf_hqdn3d.h"
#include "hqdn3d_coefs.h"

extern const unsigned char ff_hqdn3d_horizontal_comp_spv_data[];
extern const unsigned int  ff_hqdn3d_horizontal_comp_spv_len;
extern const unsigned char ff_hqdn3d_vertical_comp_spv_data[];
extern const unsigned int  ff_hqdn3d_vertical_comp_spv_len;

typedef struct HQDN3DShaderPushData {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t lut_bits;
    uint32_t is_msb;
    uint32_t is_semiplanar;
    uint32_t plane_idx;
    uint32_t spatial_offset;
    uint32_t temporal_offset;
    uint32_t h_offset_u;
    uint32_t h_offset_v;
    uint32_t frame_ant_offset_u;
    uint32_t frame_ant_offset_v;
    uint32_t first_frame;
    uint32_t spatial_enabled;
} HQDN3DShaderPushData;

typedef struct HQDN3DVulkanContext {
    FFVulkanContext vkctx;

    int initialized;
    int init_error;
    FFVkExecPool e;
    AVVulkanDeviceQueueFamily *qf;

    FFVulkanShader shd_horizontal;
    FFVulkanShader shd_vertical;

    FFVkBuffer tables_buf;
    FFVkBuffer h_buf;
    FFVkBuffer frame_ant_buf;

    double strength[4];

    int hsub, vsub;
    int depth;
    int is_msb;
    int is_semiplanar;
    int width, height;

    int spatial_enabled[2];
    int first_frame;

    AVBufferRef *out_frames_ref;
} HQDN3DVulkanContext;

/* Check if pixel format is supported, set depth/MSB/semiplanar properties, return 0 or ENOSYS. */
static int validate_format(AVFilterContext *ctx, enum AVPixelFormat sw_format,
                           int *depth, int *is_msb, int *is_semiplanar)
{
    switch (sw_format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
        *depth = 8;
        *is_msb = 0;
        *is_semiplanar = 0;
        return 0;
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV444P10:
        *depth = 10;
        *is_msb = 0;
        *is_semiplanar = 0;
        return 0;
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUV444P12:
        *depth = 12;
        *is_msb = 0;
        *is_semiplanar = 0;
        return 0;
    case AV_PIX_FMT_YUV420P16:
    case AV_PIX_FMT_YUV444P16:
        *depth = 16;
        *is_msb = 0;
        *is_semiplanar = 0;
        return 0;
    case AV_PIX_FMT_NV12:
        *depth = 8;
        *is_msb = 0;
        *is_semiplanar = 1;
        return 0;
    case AV_PIX_FMT_P010:
        *depth = 10;
        *is_msb = 1;
        *is_semiplanar = 1;
        return 0;
    case AV_PIX_FMT_P012:
        *depth = 12;
        *is_msb = 1;
        *is_semiplanar = 1;
        return 0;
    case AV_PIX_FMT_P016:
        *depth = 16;
        *is_msb = 1;
        *is_semiplanar = 1;
        return 0;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported sw_format: %s\n",
               av_get_pix_fmt_name(sw_format));
        return AVERROR(ENOSYS);
    }
}

/* Set default filter strengths and initialize the Vulkan context, returning 0 on success. */
static av_cold int hqdn3d_vulkan_init(AVFilterContext *ctx)
{
    HQDN3DVulkanContext *s = ctx->priv;

    s->out_frames_ref = NULL;
    ff_hqdn3d_default_strengths(s->strength);

    av_log(ctx, AV_LOG_VERBOSE, "ls:%f cs:%f lt:%f ct:%f\n",
           s->strength[LUMA_SPATIAL], s->strength[CHROMA_SPATIAL],
           s->strength[LUMA_TMP], s->strength[CHROMA_TMP]);

    return ff_vk_filter_init(ctx);
}

/* Free GPU coefficient lookup table, horizontal intermediate, and temporal history buffers. */
static void free_buffers(AVFilterContext *ctx)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;

    ff_vk_free_buf(vkctx, &s->tables_buf);
    ff_vk_free_buf(vkctx, &s->h_buf);
    ff_vk_free_buf(vkctx, &s->frame_ant_buf);
}

/* Release execution pool, compute shaders, buffers, output frames context, and Vulkan state. */
static av_cold void hqdn3d_vulkan_uninit(AVFilterContext *ctx)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;

    ff_vk_exec_pool_free(vkctx, &s->e);
    ff_vk_shader_free(vkctx, &s->shd_horizontal);
    ff_vk_shader_free(vkctx, &s->shd_vertical);
    free_buffers(ctx);
    av_buffer_unref(&s->out_frames_ref);
    ff_vk_uninit(&s->vkctx);

    s->initialized = 0;
}

/* Map coefficient buffer, precalculate LUT tables for all four channels, and unmap buffer. */
static int calc_coefs(AVFilterContext *ctx)
{
    HQDN3DVulkanContext *s = ctx->priv;
    int lut_bits = HQDN3D_LUT_BITS(s->depth);
    int table_entries = 512 << lut_bits;
    uint8_t *mapped;
    int err;

    err = ff_vk_map_buffer(&s->vkctx, &s->tables_buf, &mapped, 0);
    if (err < 0)
        return err;

    for (int i = 0; i < 4; i++) {
        int16_t *ct = (int16_t *)mapped + i * table_entries;
        ff_hqdn3d_precalc_coefs(s->strength[i], s->depth, ct);
    }

    s->spatial_enabled[0] = s->strength[LUMA_SPATIAL] != 0.0;
    s->spatial_enabled[1] = s->strength[CHROMA_SPATIAL] != 0.0;

    return ff_vk_unmap_buffer(&s->vkctx, &s->tables_buf, 1);
}

/* Allocate Vulkan storage buffers for coefficient LUTs, intermediate rows, and history frame. */
static int init_buffers(AVFilterContext *ctx, int w, int h)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    int lut_bits = HQDN3D_LUT_BITS(s->depth);
    size_t tables_sz = (size_t)4 * (512 << lut_bits) * sizeof(int16_t);
    int cw = AV_CEIL_RSHIFT(w, s->hsub);
    int ch = AV_CEIL_RSHIFT(h, s->vsub);
    size_t samples = (size_t)w * h + (size_t)2 * cw * ch;
    size_t data_sz = samples * sizeof(uint16_t);
    uint32_t limit = vkctx->props.properties.limits.maxStorageBufferRange;
    int err;

    if (tables_sz > limit) {
        av_log(ctx, AV_LOG_ERROR, "LUT tables need %zu bytes, above the device limit of %u\n",
               tables_sz, limit);
        return AVERROR(ENOTSUP);
    }

    if (data_sz > limit) {
        av_log(ctx, AV_LOG_ERROR, "Frame of %dx%d needs %zu bytes per buffer, above the device limit of %u\n",
               w, h, data_sz, limit);
        return AVERROR(ENOTSUP);
    }

    err = ff_vk_create_buf(vkctx, &s->tables_buf, tables_sz, NULL, NULL,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (err < 0)
        return err;

    err = ff_vk_create_buf(vkctx, &s->h_buf, data_sz, NULL, NULL,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (err < 0)
        return err;

    err = ff_vk_create_buf(vkctx, &s->frame_ant_buf, data_sz, NULL, NULL,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (err < 0)
        return err;

    return 0;
}

/* Compile and link the horizontal filtering compute shader pipeline and register execution. */
static int init_pipeline_horizontal(AVFilterContext *ctx, int planes)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const FFVulkanDescriptorSetBinding desc[] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = planes },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = 1 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = 1 },
    };
    int err;

    err = ff_vk_shader_load(&s->shd_horizontal, VK_SHADER_STAGE_COMPUTE_BIT, NULL,
                            (uint32_t[]){ 1, 32, 1 }, 0);
    if (err < 0)
        return err;

    err = ff_vk_shader_add_push_const(&s->shd_horizontal, 0, sizeof(HQDN3DShaderPushData),
                                      VK_SHADER_STAGE_COMPUTE_BIT);
    if (err < 0)
        return err;

    ff_vk_shader_add_descriptor_set(vkctx, &s->shd_horizontal, desc, FF_ARRAY_ELEMS(desc), 0);
    err = ff_vk_shader_link(vkctx, &s->shd_horizontal,
                            ff_hqdn3d_horizontal_comp_spv_data,
                            ff_hqdn3d_horizontal_comp_spv_len, "main");
    if (err < 0)
        return err;

    return ff_vk_shader_register_exec(vkctx, &s->e, &s->shd_horizontal);
}

/* Compile and link the vertical/temporal compute shader pipeline and register execution. */
static int init_pipeline_vertical(AVFilterContext *ctx, int planes)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const FFVulkanDescriptorSetBinding desc[] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = planes },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = planes },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = 1 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = 1 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .stages = VK_SHADER_STAGE_COMPUTE_BIT, .elems = 1 },
    };
    int err;

    err = ff_vk_shader_load(&s->shd_vertical, VK_SHADER_STAGE_COMPUTE_BIT, NULL,
                            (uint32_t[]){ 32, 1, 1 }, 0);
    if (err < 0)
        return err;

    err = ff_vk_shader_add_push_const(&s->shd_vertical, 0, sizeof(HQDN3DShaderPushData),
                                      VK_SHADER_STAGE_COMPUTE_BIT);
    if (err < 0)
        return err;

    ff_vk_shader_add_descriptor_set(vkctx, &s->shd_vertical, desc, FF_ARRAY_ELEMS(desc), 0);
    err = ff_vk_shader_link(vkctx, &s->shd_vertical,
                            ff_hqdn3d_vertical_comp_spv_data,
                            ff_hqdn3d_vertical_comp_spv_len, "main");
    if (err < 0)
        return err;

    return ff_vk_shader_register_exec(vkctx, &s->e, &s->shd_vertical);
}

/* Verify that the Vulkan device was created with all required shader and storage features enabled. */
static int check_device_features(AVFilterContext *ctx)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const VkPhysicalDeviceFeatures *feats = &vkctx->hwctx->device_features.features;
    const void *pnext = vkctx->hwctx->device_features.pNext;
    const VkPhysicalDeviceVulkan11Features *v11;
    const VkPhysicalDevice16BitStorageFeatures *s16;
    const VkPhysicalDeviceVulkan12Features *v12;
    const VkPhysicalDeviceScalarBlockLayoutFeatures *sbl;
    const VkPhysicalDeviceDescriptorIndexingFeatures *di;
    int has_storageBuffer16BitAccess;
    int has_scalarBlockLayout;
    int has_runtimeDescriptorArray;

    if (!feats->shaderInt16) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: shaderInt16\n");
        return AVERROR(ENOTSUP);
    }
    if (!feats->shaderStorageImageReadWithoutFormat) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: shaderStorageImageReadWithoutFormat\n");
        return AVERROR(ENOTSUP);
    }
    if (!feats->shaderStorageImageWriteWithoutFormat) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: shaderStorageImageWriteWithoutFormat\n");
        return AVERROR(ENOTSUP);
    }

    v11 = ff_vk_find_struct(pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    s16 = ff_vk_find_struct(pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    has_storageBuffer16BitAccess = (v11 && v11->storageBuffer16BitAccess) ||
                                   (s16 && s16->storageBuffer16BitAccess);
    if (!has_storageBuffer16BitAccess) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: storageBuffer16BitAccess\n");
        return AVERROR(ENOTSUP);
    }

    v12 = ff_vk_find_struct(pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    sbl = ff_vk_find_struct(pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES);
    has_scalarBlockLayout = (v12 && v12->scalarBlockLayout) ||
                            (sbl && sbl->scalarBlockLayout);
    if (!has_scalarBlockLayout) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: scalarBlockLayout\n");
        return AVERROR(ENOTSUP);
    }

    di = ff_vk_find_struct(pnext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    has_runtimeDescriptorArray = (v12 && v12->runtimeDescriptorArray) ||
                                 (di && di->runtimeDescriptorArray);
    if (!has_runtimeDescriptorArray) {
        av_log(ctx, AV_LOG_ERROR, "Vulkan device lacks required feature: runtimeDescriptorArray\n");
        return AVERROR(ENOTSUP);
    }

    return 0;
}

/* Find compute queue, initialize execution pool, allocate buffers, and compile shader pipelines. */
static int init_filter(AVFilterContext *ctx, AVFrame *in)
{
    HQDN3DVulkanContext *s = ctx->priv;
    FFVulkanContext *vkctx = &s->vkctx;
    const int planes = av_pix_fmt_count_planes(s->vkctx.output_format);
    int err;

    err = check_device_features(ctx);
    if (err < 0)
        return err;

    s->qf = ff_vk_qf_find(vkctx, VK_QUEUE_COMPUTE_BIT, 0);
    if (!s->qf) {
        av_log(ctx, AV_LOG_ERROR, "Device has no compute queues\n");
        return AVERROR(ENOTSUP);
    }

    err = ff_vk_exec_pool_init(vkctx, s->qf, &s->e, s->qf->num * 4, 0, 0, 0, NULL);
    if (err < 0)
        return err;

    err = init_buffers(ctx, in->width, in->height);
    if (err < 0)
        return err;

    err = calc_coefs(ctx);
    if (err < 0)
        return err;

    err = init_pipeline_horizontal(ctx, planes);
    if (err < 0)
        return err;

    err = init_pipeline_vertical(ctx, planes);
    if (err < 0)
        return err;

    s->initialized = 1;
    s->first_frame = 1;
    return 0;
}

/* Populate shader push constant structure with plane dimensions, buffer offsets, and bit depth. */
static void prepare_push_data(HQDN3DVulkanContext *s, HQDN3DShaderPushData *pd, int p, int w, int h)
{
    int lut_bits = HQDN3D_LUT_BITS(s->depth);
    int table_entries = 512 << lut_bits;
    int cw = AV_CEIL_RSHIFT(w, s->hsub);
    int ch = AV_CEIL_RSHIFT(h, s->vsub);
    uint32_t off_u = (uint32_t)(w * h);
    uint32_t off_v = (uint32_t)(w * h + cw * ch);

    pd->depth = s->depth;
    pd->lut_bits = lut_bits;
    pd->is_msb = s->is_msb;
    pd->first_frame = s->first_frame;

    if (p == 0) {
        pd->width = w;
        pd->height = h;
        pd->is_semiplanar = 0;
        pd->plane_idx = 0;
        pd->spatial_offset = 0 * table_entries;
        pd->temporal_offset = 1 * table_entries;
        pd->h_offset_u = 0;
        pd->h_offset_v = 0;
        pd->frame_ant_offset_u = 0;
        pd->frame_ant_offset_v = 0;
        pd->spatial_enabled = s->spatial_enabled[0];
    } else if (s->is_semiplanar) {
        pd->width = cw;
        pd->height = ch;
        pd->is_semiplanar = 1;
        pd->plane_idx = 1;
        pd->spatial_offset = 2 * table_entries;
        pd->temporal_offset = 3 * table_entries;
        pd->h_offset_u = off_u;
        pd->h_offset_v = off_v;
        pd->frame_ant_offset_u = off_u;
        pd->frame_ant_offset_v = off_v;
        pd->spatial_enabled = s->spatial_enabled[1];
    } else {
        pd->width = cw;
        pd->height = ch;
        pd->is_semiplanar = 0;
        pd->plane_idx = p;
        pd->spatial_offset = 2 * table_entries;
        pd->temporal_offset = 3 * table_entries;
        pd->h_offset_u = (p == 1) ? off_u : off_v;
        pd->h_offset_v = 0;
        pd->frame_ant_offset_u = (p == 1) ? off_u : off_v;
        pd->frame_ant_offset_v = 0;
        pd->spatial_enabled = s->spatial_enabled[1];
    }
}

/* Bind input image views, coefficient tables, and horizontal intermediate buffer to descriptors. */
static int update_descriptors_horizontal(HQDN3DVulkanContext *s, FFVkExecContext *exec,
                                         AVFrame *in, VkImageView *in_views)
{
    FFVulkanContext *vkctx = &s->vkctx;
    int err;

    ff_vk_shader_update_img_array(vkctx, exec, &s->shd_horizontal, in, in_views,
                                  0, 0, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    err = ff_vk_shader_update_desc_buffer(vkctx, exec, &s->shd_horizontal, 0, 1, 0,
                                          &s->tables_buf, 0, s->tables_buf.size, VK_FORMAT_UNDEFINED);
    if (err < 0)
        return err;
    return ff_vk_shader_update_desc_buffer(vkctx, exec, &s->shd_horizontal, 0, 2, 0,
                                           &s->h_buf, 0, s->h_buf.size, VK_FORMAT_UNDEFINED);
}

/* Bind input and output image views, LUTs, intermediate, and history buffers to descriptors. */
static int update_descriptors_vertical(HQDN3DVulkanContext *s, FFVkExecContext *exec,
                                       AVFrame *in, AVFrame *out,
                                       VkImageView *in_views, VkImageView *out_views)
{
    FFVulkanContext *vkctx = &s->vkctx;
    int err;

    ff_vk_shader_update_img_array(vkctx, exec, &s->shd_vertical, in, in_views,
                                  0, 0, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    ff_vk_shader_update_img_array(vkctx, exec, &s->shd_vertical, out, out_views,
                                  0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE);
    err = ff_vk_shader_update_desc_buffer(vkctx, exec, &s->shd_vertical, 0, 2, 0,
                                          &s->tables_buf, 0, s->tables_buf.size, VK_FORMAT_UNDEFINED);
    if (err < 0)
        return err;
    err = ff_vk_shader_update_desc_buffer(vkctx, exec, &s->shd_vertical, 0, 3, 0,
                                          &s->h_buf, 0, s->h_buf.size, VK_FORMAT_UNDEFINED);
    if (err < 0)
        return err;
    return ff_vk_shader_update_desc_buffer(vkctx, exec, &s->shd_vertical, 0, 4, 0,
                                           &s->frame_ant_buf, 0, s->frame_ant_buf.size, VK_FORMAT_UNDEFINED);
}

/* Record horizontal and vertical compute dispatches separated by a pipeline memory barrier. */
static int dispatch_passes(HQDN3DVulkanContext *s, FFVkExecContext *exec,
                           AVFrame *in, AVFrame *out,
                           VkImageView *in_views, VkImageView *out_views,
                           int w, int h)
{
    FFVulkanContext *vkctx = &s->vkctx;
    FFVulkanFunctions *vk = &vkctx->vkfn;
    int nb_passes = s->is_semiplanar ? 2 : 3;
    HQDN3DShaderPushData pd;
    int err;

    err = update_descriptors_horizontal(s, exec, in, in_views);
    if (err < 0)
        return err;

    ff_vk_exec_bind_shader(vkctx, exec, &s->shd_horizontal);
    for (int p = 0; p < nb_passes; p++) {
        prepare_push_data(s, &pd, p, w, h);
        if (pd.spatial_enabled == 0)
            continue;
        ff_vk_shader_update_push_const(vkctx, exec, &s->shd_horizontal,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);
        vk->CmdDispatch(exec->buf, 1, FFALIGN(pd.height, 32) / 32, 1);
    }

    VkBufferMemoryBarrier2 h_bar = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s->h_buf.buf,
        .offset = 0,
        .size = s->h_buf.size,
    };
    vk->CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pBufferMemoryBarriers = &h_bar,
        .bufferMemoryBarrierCount = 1,
    });

    err = update_descriptors_vertical(s, exec, in, out, in_views, out_views);
    if (err < 0)
        return err;

    ff_vk_exec_bind_shader(vkctx, exec, &s->shd_vertical);
    for (int p = 0; p < nb_passes; p++) {
        prepare_push_data(s, &pd, p, w, h);
        ff_vk_shader_update_push_const(vkctx, exec, &s->shd_vertical,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);
        vk->CmdDispatch(exec->buf, FFALIGN(pd.width, 32) / 32, 1, 1);
    }

    return 0;
}

/* Allocate and initialize output Vulkan hardware frames context matching input properties. */
static int create_out_frames_ctx(AVFilterContext *ctx, AVFrame *in)
{
    HQDN3DVulkanContext *s = ctx->priv;
    AVHWFramesContext *in_hwfc = (AVHWFramesContext *)in->hw_frames_ctx->data;
    AVHWFramesContext *frames_ctx;
    AVVulkanFramesContext *vk_frames;
    AVBufferRef *device_ref = in_hwfc->device_ref ? in_hwfc->device_ref : ctx->hw_device_ctx;
    int err;

    av_buffer_unref(&s->out_frames_ref);

    s->out_frames_ref = av_hwframe_ctx_alloc(device_ref);
    if (!s->out_frames_ref)
        return AVERROR(ENOMEM);

    frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;
    frames_ctx->format    = AV_PIX_FMT_VULKAN;
    frames_ctx->sw_format = in_hwfc->sw_format;
    frames_ctx->width     = in->width;
    frames_ctx->height    = in->height;

    vk_frames = frames_ctx->hwctx;
    vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
    vk_frames->usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_STORAGE_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    err = av_hwframe_ctx_init(s->out_frames_ref);
    if (err < 0) {
        av_buffer_unref(&s->out_frames_ref);
        return err;
    }

    ctx->outputs[0]->w = in->width;
    ctx->outputs[0]->h = in->height;
    return 0;
}

/* Check format, size output if first frame differs from link, reset buffers on size changes. */
static int check_format_and_size(AVFilterContext *ctx, AVFrame *in)
{
    HQDN3DVulkanContext *s = ctx->priv;
    AVHWFramesContext *hwfc = (AVHWFramesContext *)in->hw_frames_ctx->data;
    int err;

    if (hwfc->sw_format != s->vkctx.input_format) {
        av_log(ctx, AV_LOG_ERROR, "Input pixel format changed from %s to %s\n",
               av_get_pix_fmt_name(s->vkctx.input_format), av_get_pix_fmt_name(hwfc->sw_format));
        return AVERROR(EINVAL);
    }

    err = validate_format(ctx, hwfc->sw_format, &s->depth, &s->is_msb, &s->is_semiplanar);
    if (err < 0)
        return err;

    if (!s->initialized) {
        s->hsub = av_pix_fmt_desc_get(hwfc->sw_format)->log2_chroma_w;
        s->vsub = av_pix_fmt_desc_get(hwfc->sw_format)->log2_chroma_h;
    }

    if (in->width != s->width || in->height != s->height) {
        if (s->initialized) {
            free_buffers(ctx);
            err = init_buffers(ctx, in->width, in->height);
            if (err < 0)
                return s->init_error = err;
            err = calc_coefs(ctx);
            if (err < 0)
                return s->init_error = err;
            err = create_out_frames_ctx(ctx, in);
            if (err < 0)
                return s->init_error = err;
            s->first_frame = 1;
        } else if (in->width  != ctx->outputs[0]->w ||
                   in->height != ctx->outputs[0]->h) {
            /* The first frame may differ from the configured link size. */
            err = create_out_frames_ctx(ctx, in);
            if (err < 0)
                return err;
        }
        s->width = in->width;
        s->height = in->height;
    }
    return 0;
}

/* Create image views and insert pipeline barriers transitioning input and output frame images. */
static int setup_views_and_barriers(HQDN3DVulkanContext *s, FFVkExecContext *exec,
                                    AVFrame *in, AVFrame *out,
                                    VkImageView *in_views, VkImageView *out_views)
{
    FFVulkanContext *vkctx = &s->vkctx;
    VkImageMemoryBarrier2 img_bar[AV_NUM_DATA_POINTERS * 2];
    int nb_img_bar = 0;
    int err;

    err = ff_vk_exec_add_dep_frame(vkctx, exec, in, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    if (err < 0)
        return err;
    err = ff_vk_exec_add_dep_frame(vkctx, exec, out, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    if (err < 0)
        return err;

    err = ff_vk_create_imageviews(vkctx, exec, in_views, in, FF_VK_REP_UINT);
    if (err < 0)
        return err;
    err = ff_vk_create_imageviews(vkctx, exec, out_views, out, FF_VK_REP_UINT);
    if (err < 0)
        return err;

    ff_vk_frame_barrier(vkctx, exec, in, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED);
    ff_vk_frame_barrier(vkctx, exec, out, img_bar, &nb_img_bar,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED);
    vkctx->vkfn.CmdPipelineBarrier2(exec->buf, &(VkDependencyInfo) {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pImageMemoryBarriers = img_bar,
        .imageMemoryBarrierCount = nb_img_bar,
    });
    return 0;
}

/* Allocate an output frame and copy input frame properties. */
static int alloc_output_frame(HQDN3DVulkanContext *s, AVFilterLink *outlink, AVFrame *in, AVFrame **out_ret)
{
    AVFrame *out;

    if (s->out_frames_ref) {
        int err;

        out = av_frame_alloc();
        if (!out)
            return AVERROR(ENOMEM);
        err = av_hwframe_get_buffer(s->out_frames_ref, out, 0);
        if (err < 0) {
            av_frame_free(&out);
            return err;
        }
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out)
            return AVERROR(ENOMEM);
    }

    int err = av_frame_copy_props(out, in);
    if (err < 0) {
        av_frame_free(&out);
        return err;
    }

    *out_ret = out;
    return 0;
}

/* Submit filtering command buffer to Vulkan queue, wait for execution, or discard on error. */
static int exec_filter(HQDN3DVulkanContext *s, AVFrame *in, AVFrame *out)
{
    FFVulkanContext *vkctx = &s->vkctx;
    VkImageView in_views[AV_NUM_DATA_POINTERS];
    VkImageView out_views[AV_NUM_DATA_POINTERS];
    FFVkExecContext *exec = ff_vk_exec_get(vkctx, &s->e);
    int err;

    err = ff_vk_exec_start(vkctx, exec);
    if (err < 0)
        return err;

    err = setup_views_and_barriers(s, exec, in, out, in_views, out_views);
    if (err < 0)
        goto fail;

    err = dispatch_passes(s, exec, in, out, in_views, out_views, in->width, in->height);
    if (err < 0)
        goto fail;

    err = ff_vk_exec_submit(vkctx, exec);
    if (err < 0)
        return err;
    ff_vk_exec_wait(vkctx, exec);

    return 0;

fail:
    ff_vk_exec_discard(vkctx, exec);
    return err;
}

/* Validate incoming frame, lazily initialize pipelines, execute filter, and push output frame. */
static int hqdn3d_vulkan_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    HQDN3DVulkanContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int err;

    /* A failed initialization or resize is terminal. */
    if (s->init_error < 0) {
        av_frame_free(&in);
        return s->init_error;
    }

    err = check_format_and_size(ctx, in);
    if (err < 0) {
        av_frame_free(&in);
        return err;
    }

    if (!s->initialized) {
        err = init_filter(ctx, in);
        if (err < 0) {
            s->init_error = err;
            av_frame_free(&in);
            return err;
        }
    }

    err = alloc_output_frame(s, outlink, in, &out);
    if (err < 0) {
        av_frame_free(&in);
        return err;
    }

    err = exec_filter(s, in, out);
    if (err < 0) {
        av_frame_free(&in);
        av_frame_free(&out);
        return err;
    }

    s->first_frame = 0;
    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

/* Apply a runtime command and recompute the lookup tables if the filter is initialized;
 * return the stored error after a failed initialization or resize. */
static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    HQDN3DVulkanContext *s = ctx->priv;
    int ret;

    if (s->init_error < 0)
        return s->init_error;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    if (s->initialized)
        return calc_coefs(ctx);

    return 0;
}

#define OFFSET(x) offsetof(HQDN3DVulkanContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM)
static const AVOption hqdn3d_vulkan_options[] = {
    { "luma_spatial",   "spatial luma strength",    OFFSET(strength[LUMA_SPATIAL]),   AV_OPT_TYPE_DOUBLE, { .dbl = 0.0 }, 0, DBL_MAX, FLAGS },
    { "chroma_spatial", "spatial chroma strength",  OFFSET(strength[CHROMA_SPATIAL]), AV_OPT_TYPE_DOUBLE, { .dbl = 0.0 }, 0, DBL_MAX, FLAGS },
    { "luma_tmp",       "temporal luma strength",   OFFSET(strength[LUMA_TMP]),       AV_OPT_TYPE_DOUBLE, { .dbl = 0.0 }, 0, DBL_MAX, FLAGS },
    { "chroma_tmp",     "temporal chroma strength", OFFSET(strength[CHROMA_TMP]),     AV_OPT_TYPE_DOUBLE, { .dbl = 0.0 }, 0, DBL_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(hqdn3d_vulkan);

static const AVFilterPad hqdn3d_vulkan_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &hqdn3d_vulkan_filter_frame,
        .config_props = &ff_vk_filter_config_input,
    }
};

static const AVFilterPad hqdn3d_vulkan_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_vk_filter_config_output,
    }
};

const FFFilter ff_vf_hqdn3d_vulkan = {
    .p.name         = "hqdn3d_vulkan",
    .p.description  = NULL_IF_CONFIG_SMALL("Apply a High Quality 3D Denoiser using Vulkan"),
    .p.priv_class   = &hqdn3d_vulkan_class,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .priv_size      = sizeof(HQDN3DVulkanContext),
    .init           = &hqdn3d_vulkan_init,
    .uninit         = &hqdn3d_vulkan_uninit,
    .process_command = &process_command,
    FILTER_INPUTS(hqdn3d_vulkan_inputs),
    FILTER_OUTPUTS(hqdn3d_vulkan_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_VULKAN),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE | FF_FILTER_FLAG_DYNAMIC_FRAME_SIZE,
};
