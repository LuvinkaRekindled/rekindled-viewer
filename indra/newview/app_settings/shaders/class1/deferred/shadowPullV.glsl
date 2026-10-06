/**
 * @file shadowPullV.glsl
 * @brief Shadow caster vertex shader that reads its vertices and indices from the viewer's buffers by address (render packets, Vulkan mirror only).
 *
 * No vertex attributes and no bound vertex or index buffer: the worker writes a table with one entry per draw (the buffer addresses, the index range and the model
 * matrix number) and draws every entry as a draw of its own whose instance index is the entry number. Only the position is read: the vertex buffers keep the
 * positions first, as 16 byte vec4s, and the indices are 16 bit.
 *
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 */

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ShadowIndices { uint w[]; };

struct ShadowPacket
{
    uvec2 positions;
    uvec2 indices;
    uint first;
    uint count;
    uint model;
    uint pad;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ShadowPackets { ShadowPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowModels { mat4 m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;
uniform mat4 ll_view_proj;

void main()
{
    ShadowPacket pk = ShadowPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = ShadowIndices(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec4 position = ShadowPositions(pk.positions).v[index];
    gl_Position = ll_view_proj * ShadowModels(ll_packet_models).m[pk.model] * vec4(position.xyz, 1.0);
}
