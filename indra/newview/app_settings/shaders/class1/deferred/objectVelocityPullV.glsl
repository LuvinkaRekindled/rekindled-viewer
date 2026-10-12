/**
 * @file objectVelocityPullV.glsl
 * @brief Velocity pass (doc/vulkan/PLAN-ray-tracing.md, item 6, step 1b) for draws that move as a whole (vehicles, physical objects, worn rigid attachments): the vertices
 *        and indices are read from buffers by address (render packets kind 9, Vulkan mirror only), the motion comes from the draw's model matrix of this frame and of the frame before.
 *
 * Like shadowPullV.glsl for the fetching and avatarVelocityPullV.glsl for the output: gl_Position is the JITTERED clip position (ll_view_proj, from the record header; the depth test
 * in avatarVelocityF.glsl needs the pixels of the G-buffer), the motion uses the unjittered matrices set as ordinary uniforms.
 *
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc. and Firestorm contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 */

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ObjectIndices { uint w[]; };

// 32 bytes
struct ObjectPacket
{
    uvec2 positions;
    uvec2 indices;
    uint first;
    uint count;
    uint model;
    uint pad;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer ObjectPackets { ObjectPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ObjectModels { mat4 m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;         // this frame's model matrices
uniform uvec2 ll_packet_prev_models;    // last frame's, same numbers
uniform mat4 ll_view_proj;              // jittered projection * view
uniform mat4 view_proj_cur;             // unjittered projection * view, this frame
uniform mat4 view_proj_prev;            // the same, last frame

out vec4 vary_clip;
out vec4 vary_cur;
out vec4 vary_prev;

void main()
{
    ObjectPacket pk = ObjectPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = ObjectIndices(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec4 position = ObjectPositions(pk.positions).v[index];

    vec4 world_cur = ObjectModels(ll_packet_models).m[pk.model] * vec4(position.xyz, 1.0);
    vec4 world_prev = ObjectModels(ll_packet_prev_models).m[pk.model] * vec4(position.xyz, 1.0);

    vary_clip = ll_view_proj * world_cur;
    vary_cur = view_proj_cur * world_cur;
    vary_prev = view_proj_prev * world_prev;
    gl_Position = vary_clip;
}
