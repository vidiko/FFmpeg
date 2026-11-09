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

#pragma shader_stage(compute)

#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_image_load_formatted : require
#extension GL_EXT_nonuniform_qualifier : require

layout (local_size_x_id = 253, local_size_y_id = 254, local_size_z_id = 255) in;

layout (set = 0, binding = 0) uniform readonly  uimage2D in_img[];
layout (set = 0, binding = 1) uniform writeonly uimage2D out_img[];

layout (set = 0, binding = 2, scalar) readonly buffer TablesBuffer {
    int16_t coef_tables[];
};

layout (set = 0, binding = 3, scalar) readonly buffer HBuffer {
    uint16_t h_data[];
};

layout (set = 0, binding = 4, scalar) buffer FrameAntBuffer {
    uint16_t frame_ant_data[];
};

layout (push_constant, scalar) uniform PushConsts {
    uint width;
    uint height;
    uint depth;
    uint lut_bits;
    uint is_msb;
    uint is_semiplanar;
    uint plane_idx;
    uint spatial_offset;
    uint temporal_offset;
    uint h_offset_u;
    uint h_offset_v;
    uint frame_ant_offset_u;
    uint frame_ant_offset_v;
    uint first_frame;
    uint spatial_enabled;
};

uint load_sample(uint raw, uint s_depth, uint s_msb)
{
    uint shift = 16u - s_depth;
    uint sample_val = (s_msb != 0u) ? (raw >> shift) : raw;
    uint rounding = (shift > 0u) ? (((1u << shift) - 1u) >> 1u) : 0u;
    return (sample_val << shift) + rounding;
}

uint store_sample(uint val, uint s_depth, uint s_msb)
{
    uint shift = 16u - s_depth;
    uint stored = val >> shift;
    /* MSB-packed formats cannot hold a code above the depth maximum. */
    if (s_msb != 0u && shift != 0u)
        stored = min(stored, (1u << s_depth) - 1u);
    return (s_msb != 0u) ? (stored << shift) : stored;
}

int lowpass(int prev_val, int cur_val, uint t_offset, uint l_bits)
{
    int d = (prev_val - cur_val) >> (8 - int(l_bits));
    int center = 256 << int(l_bits);
    d = clamp(d, -center, center - 1);
    int idx = int(t_offset) + center + d;
    int coef = int(coef_tables[idx]);
    return cur_val + coef;
}

void process_planar_temporal(uint x)
{
    for (uint y = 0u; y < height; y++) {
        uint raw = imageLoad(in_img[plane_idx], ivec2(int(x), int(y))).x;
        int cur = int(load_sample(raw, depth, is_msb));
        uint fa_idx = frame_ant_offset_u + y * width + x;
        int prev_ant = (first_frame != 0u) ? cur : int(frame_ant_data[fa_idx]);
        int tmp = lowpass(prev_ant, cur, temporal_offset, lut_bits);
        frame_ant_data[fa_idx] = uint16_t(tmp);
        uint out_val = store_sample(uint(tmp), depth, is_msb);
        imageStore(out_img[plane_idx], ivec2(int(x), int(y)), uvec4(out_val, 0, 0, 0));
    }
}

void process_semiplanar_temporal(uint x)
{
    for (uint y = 0u; y < height; y++) {
        uvec4 raw = imageLoad(in_img[1], ivec2(int(x), int(y)));
        int cur_u = int(load_sample(raw.x, depth, is_msb));
        int cur_v = int(load_sample(raw.y, depth, is_msb));
        uint fa_u = frame_ant_offset_u + y * width + x;
        uint fa_v = frame_ant_offset_v + y * width + x;
        int prev_u = (first_frame != 0u) ? cur_u : int(frame_ant_data[fa_u]);
        int prev_v = (first_frame != 0u) ? cur_v : int(frame_ant_data[fa_v]);
        int tmp_u = lowpass(prev_u, cur_u, temporal_offset, lut_bits);
        int tmp_v = lowpass(prev_v, cur_v, temporal_offset, lut_bits);
        frame_ant_data[fa_u] = uint16_t(tmp_u);
        frame_ant_data[fa_v] = uint16_t(tmp_v);
        uint out_u = store_sample(uint(tmp_u), depth, is_msb);
        uint out_v = store_sample(uint(tmp_v), depth, is_msb);
        imageStore(out_img[1], ivec2(int(x), int(y)), uvec4(out_u, out_v, 0, 0));
    }
}

void process_planar_spatial(uint x)
{
    int line_ant = int(h_data[h_offset_u + x]);
    for (uint y = 0u; y < height; y++) {
        int spat_tmp = line_ant;
        if (y > 0u) {
            int h_val = int(h_data[h_offset_u + y * width + x]);
            spat_tmp = lowpass(line_ant, h_val, spatial_offset, lut_bits);
            line_ant = int(uint16_t(spat_tmp));
        }
        uint fa_idx = frame_ant_offset_u + y * width + x;
        int prev_ant;
        if (first_frame != 0u) {
            uint raw = imageLoad(in_img[plane_idx], ivec2(int(x), int(y))).x;
            prev_ant = int(load_sample(raw, depth, is_msb));
        } else {
            prev_ant = int(frame_ant_data[fa_idx]);
        }
        int temp_tmp = lowpass(prev_ant, spat_tmp, temporal_offset, lut_bits);
        frame_ant_data[fa_idx] = uint16_t(temp_tmp);
        uint out_val = store_sample(uint(temp_tmp), depth, is_msb);
        imageStore(out_img[plane_idx], ivec2(int(x), int(y)), uvec4(out_val, 0, 0, 0));
    }
}

void process_semiplanar_spatial(uint x)
{
    int line_ant_u = int(h_data[h_offset_u + x]);
    int line_ant_v = int(h_data[h_offset_v + x]);
    for (uint y = 0u; y < height; y++) {
        int spat_tmp_u = line_ant_u;
        int spat_tmp_v = line_ant_v;
        if (y > 0u) {
            int h_u = int(h_data[h_offset_u + y * width + x]);
            int h_v = int(h_data[h_offset_v + y * width + x]);
            spat_tmp_u = lowpass(line_ant_u, h_u, spatial_offset, lut_bits);
            spat_tmp_v = lowpass(line_ant_v, h_v, spatial_offset, lut_bits);
            line_ant_u = int(uint16_t(spat_tmp_u));
            line_ant_v = int(uint16_t(spat_tmp_v));
        }
        uint fa_u = frame_ant_offset_u + y * width + x;
        uint fa_v = frame_ant_offset_v + y * width + x;
        int prev_u, prev_v;
        if (first_frame != 0u) {
            uvec4 raw = imageLoad(in_img[1], ivec2(int(x), int(y)));
            prev_u = int(load_sample(raw.x, depth, is_msb));
            prev_v = int(load_sample(raw.y, depth, is_msb));
        } else {
            prev_u = int(frame_ant_data[fa_u]);
            prev_v = int(frame_ant_data[fa_v]);
        }
        int temp_tmp_u = lowpass(prev_u, spat_tmp_u, temporal_offset, lut_bits);
        int temp_tmp_v = lowpass(prev_v, spat_tmp_v, temporal_offset, lut_bits);
        frame_ant_data[fa_u] = uint16_t(temp_tmp_u);
        frame_ant_data[fa_v] = uint16_t(temp_tmp_v);
        uint out_u = store_sample(uint(temp_tmp_u), depth, is_msb);
        uint out_v = store_sample(uint(temp_tmp_v), depth, is_msb);
        imageStore(out_img[1], ivec2(int(x), int(y)), uvec4(out_u, out_v, 0, 0));
    }
}

void main()
{
    uint x = gl_GlobalInvocationID.x;
    if (x >= width)
        return;

    if (spatial_enabled == 0u) {
        if (is_semiplanar != 0u)
            process_semiplanar_temporal(x);
        else
            process_planar_temporal(x);
    } else {
        if (is_semiplanar != 0u)
            process_semiplanar_spatial(x);
        else
            process_planar_spatial(x);
    }
}
