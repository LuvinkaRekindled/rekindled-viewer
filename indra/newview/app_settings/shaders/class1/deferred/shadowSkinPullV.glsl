/**
 * @file shadowSkinPullV.glsl
 * @brief Rigged shadow caster vertex shader that reads its vertices, indices, joint weights and matrix palette from buffers by address
 *        (render packets, Vulkan mirror only).
 *
 * Like shadowPullV.glsl, with the skinning of objectSkinV.glsl: the entry names the draw's matrix palette (12 floats per joint, as the viewer uploads it as
 * a mat3x4 array) and the vertex buffer's weight4 array.
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
struct SkinPacket
{
    uvec2 positions;
    uvec2 indices;
    uvec2 weights;
    uvec2 palette;
    uint first;
    uint joints;
    uint model;
    uint pad;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer SkinPackets { SkinPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinModels { mat4 m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;
uniform mat4 ll_view_proj;

void main()
{
    SkinPacket pk = SkinPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = SkinIndices(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec4 position = SkinPositions(pk.positions).v[index];
    vec4 weight4 = SkinWeights(pk.weights).w[index];

    // as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
    vec4 w = fract(weight4);
    vec4 joint = floor(weight4);
    joint = clamp(joint, vec4(0.0), vec4(float(pk.joints - 1u)));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    SkinPalette palette = SkinPalette(pk.palette);
    SkinJoint j1 = palette.j[int(joint.x)];
    SkinJoint j2 = palette.j[int(joint.y)];
    SkinJoint j3 = palette.j[int(joint.z)];
    SkinJoint j4 = palette.j[int(joint.w)];

    mat3 m = mat3(j1.c0.xyz, j1.c1.xyz, j1.c2.xyz) * w.x;
    m += mat3(j2.c0.xyz, j2.c1.xyz, j2.c2.xyz) * w.y;
    m += mat3(j3.c0.xyz, j3.c1.xyz, j3.c2.xyz) * w.z;
    m += mat3(j4.c0.xyz, j4.c1.xyz, j4.c2.xyz) * w.w;
    vec3 trans = vec3(j1.c0.w, j1.c1.w, j1.c2.w) * w.x;
    trans += vec3(j2.c0.w, j2.c1.w, j2.c2.w) * w.y;
    trans += vec3(j3.c0.w, j3.c1.w, j3.c2.w) * w.z;
    trans += vec3(j4.c0.w, j4.c1.w, j4.c2.w) * w.w;

    vec3 skinned = m * position.xyz + trans;
    gl_Position = ll_view_proj * SkinModels(ll_packet_models).m[pk.model] * vec4(skinned, 1.0);
}
