/**
 * @file avatarVelocityPullV.glsl
 * @brief Avatar velocity pass (doc/vulkan/PLAN-ray-tracing.md, item 6) for rigged draws that read their vertices, indices, joint weights and both matrix palettes
 *        (this frame's and last frame's) from buffers by address (render packets kind 8, Vulkan mirror only).
 *
 * The same work as avatarVelocityV.glsl, with the fetching of shadowSkinPullV.glsl: gl_Position is the JITTERED clip position (ll_view_proj, from the record header; the
 * depth test in the fragment shader needs the pixels of the G-buffer), the motion comes from the unjittered matrices set as ordinary uniforms.
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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinWeights { vec4 w[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SkinIndices { uint w[]; };

struct SkinJoint
{
    vec4 c0;
    vec4 c1;
    vec4 c2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinPalette { SkinJoint j[]; };

// 48 bytes
struct VelocityPacket
{
    uvec2 positions;
    uvec2 indices;
    uvec2 weights;
    uvec2 palette;
    uvec2 prevPalette;
    uint first;
    uint joints;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer VelocityPackets { VelocityPacket p[]; };

uniform uvec2 ll_packet_table;
uniform mat4 ll_view_proj;      // jittered projection * view
uniform mat4 view_proj_cur;     // unjittered projection * view, this frame
uniform mat4 view_proj_prev;    // the same, last frame
uniform int vel_indexed;        // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here

out vec4 vary_clip;
out vec4 vary_cur;
out vec4 vary_prev;

// as getObjectSkinnedTransform() in objectSkinV.glsl
vec3 skinPosition(SkinPalette palette, vec3 position, vec4 joint, vec4 w)
{
    // (a joint with no weight is not read: most vertices have one to three; a zero weight adds nothing)
    SkinJoint j1 = palette.j[int(joint.x)];
    mat3 m = mat3(j1.c0.xyz, j1.c1.xyz, j1.c2.xyz) * w.x;
    vec3 trans = vec3(j1.c0.w, j1.c1.w, j1.c2.w) * w.x;
    if (w.y > 0.0)
    {
        SkinJoint j2 = palette.j[int(joint.y)];
        m += mat3(j2.c0.xyz, j2.c1.xyz, j2.c2.xyz) * w.y;
        trans += vec3(j2.c0.w, j2.c1.w, j2.c2.w) * w.y;
    }
    if (w.z > 0.0)
    {
        SkinJoint j3 = palette.j[int(joint.z)];
        m += mat3(j3.c0.xyz, j3.c1.xyz, j3.c2.xyz) * w.z;
        trans += vec3(j3.c0.w, j3.c1.w, j3.c2.w) * w.z;
    }
    if (w.w > 0.0)
    {
        SkinJoint j4 = palette.j[int(joint.w)];
        m += mat3(j4.c0.xyz, j4.c1.xyz, j4.c2.xyz) * w.w;
        trans += vec3(j4.c0.w, j4.c1.w, j4.c2.w) * w.w;
    }
    return m * position + trans;
}

void main()
{
    VelocityPacket pk = VelocityPackets(ll_packet_table).p[gl_InstanceIndex];
    uint index = uint(gl_VertexIndex);
    if (vel_indexed == 0)
    {
        uint i = pk.first + uint(gl_VertexIndex);
        uint word = SkinIndices(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    vec4 position = SkinPositions(pk.positions).v[index];
    vec4 weight4 = SkinWeights(pk.weights).w[index];

    vec4 w = fract(weight4);
    vec4 joint = floor(weight4);
    joint = clamp(joint, vec4(0.0), vec4(float(pk.joints - 1u)));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    vec3 world_cur = skinPosition(SkinPalette(pk.palette), position.xyz, joint, w);
    vec3 world_prev = skinPosition(SkinPalette(pk.prevPalette), position.xyz, joint, w);

    vary_clip = ll_view_proj * vec4(world_cur, 1.0);
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
}
