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

layout (set = 0, binding = 0) uniform readonly uimage2D in_img[];

layout (set = 0, binding = 1, scalar) readonly buffer TablesBuffer {
    int16_t coef_tables[];
};

layout (set = 0, binding = 2, scalar) buffer HBuffer {
    uint16_t h_data[];
};

layout (set = 0, binding = 3, scalar) buffer FrameAntBuffer {
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

uint lowpass(uint prev_val, uint cur_val, uint t_offset, uint l_bits)
{
    int d = (int(prev_val) - int(cur_val)) >> (8 - int(l_bits));
    int center = 256 << int(l_bits);
    int idx = int(t_offset) + center + d;
    int coef = int(coef_tables[idx]);
    return uint(int(cur_val) + coef);
}

void process_planar_row(uint y)
{
    uint h_row_start = h_offset_u + y * width;
    uint raw0 = imageLoad(in_img[plane_idx], ivec2(0, int(y))).x;
    uint pixel_ant = load_sample(raw0, depth, is_msb);

    if (y == 0u) {
        for (uint x = 0u; x < width; x++) {
            uint raw = imageLoad(in_img[plane_idx], ivec2(int(x), 0)).x;
            uint cur = load_sample(raw, depth, is_msb);
            pixel_ant = lowpass(pixel_ant, cur, spatial_offset, lut_bits);
            h_data[h_row_start + x] = uint16_t(pixel_ant);
        }
    } else {
        h_data[h_row_start + 0u] = uint16_t(pixel_ant);
        for (uint x = 0u; x + 1u < width; x++) {
            uint raw = imageLoad(in_img[plane_idx], ivec2(int(x + 1u), int(y))).x;
            uint cur = load_sample(raw, depth, is_msb);
            pixel_ant = lowpass(pixel_ant, cur, spatial_offset, lut_bits);
            h_data[h_row_start + x + 1u] = uint16_t(pixel_ant);
        }
    }
}

void process_semiplanar_row(uint y)
{
    uint h_row_u = h_offset_u + y * width;
    uint h_row_v = h_offset_v + y * width;
    uvec4 raw0 = imageLoad(in_img[1], ivec2(0, int(y)));
    uint pixel_ant_u = load_sample(raw0.x, depth, is_msb);
    uint pixel_ant_v = load_sample(raw0.y, depth, is_msb);

    if (y == 0u) {
        for (uint x = 0u; x < width; x++) {
            uvec4 raw = imageLoad(in_img[1], ivec2(int(x), 0));
            uint cur_u = load_sample(raw.x, depth, is_msb);
            uint cur_v = load_sample(raw.y, depth, is_msb);
            pixel_ant_u = lowpass(pixel_ant_u, cur_u, spatial_offset, lut_bits);
            pixel_ant_v = lowpass(pixel_ant_v, cur_v, spatial_offset, lut_bits);
            h_data[h_row_u + x] = uint16_t(pixel_ant_u);
            h_data[h_row_v + x] = uint16_t(pixel_ant_v);
        }
    } else {
        h_data[h_row_u + 0u] = uint16_t(pixel_ant_u);
        h_data[h_row_v + 0u] = uint16_t(pixel_ant_v);
        for (uint x = 0u; x + 1u < width; x++) {
            uvec4 raw = imageLoad(in_img[1], ivec2(int(x + 1u), int(y)));
            uint cur_u = load_sample(raw.x, depth, is_msb);
            uint cur_v = load_sample(raw.y, depth, is_msb);
            pixel_ant_u = lowpass(pixel_ant_u, cur_u, spatial_offset, lut_bits);
            pixel_ant_v = lowpass(pixel_ant_v, cur_v, spatial_offset, lut_bits);
            h_data[h_row_u + x + 1u] = uint16_t(pixel_ant_u);
            h_data[h_row_v + x + 1u] = uint16_t(pixel_ant_v);
        }
    }
}

void main()
{
    uint y = gl_GlobalInvocationID.y;
    if (y >= height || spatial_enabled == 0u)
        return;

    if (is_semiplanar != 0u)
        process_semiplanar_row(y);
    else
        process_planar_row(y);
}
