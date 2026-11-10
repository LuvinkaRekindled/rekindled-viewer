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
uniform int shadow_indexed;     // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here

void main()
{
    SkinPacket pk = SkinPackets(ll_packet_table).p[gl_InstanceIndex];
    uint index = uint(gl_VertexIndex);
    if (shadow_indexed == 0)
    {
        uint i = pk.first + uint(gl_VertexIndex);
        uint word = SkinIndices(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    vec4 position = SkinPositions(pk.positions).v[index];
    vec4 weight4 = SkinWeights(pk.weights).w[index];

    // as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
    vec4 w = fract(weight4);
    vec4 joint = floor(weight4);
    joint = clamp(joint, vec4(0.0), vec4(float(pk.joints - 1u)));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    SkinPalette palette = SkinPalette(pk.palette);
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

    vec3 skinned = m * position.xyz + trans;
    mat4 model = SkinModels(ll_packet_models).m[pk.model];
    // avatar relative skinning (ll_rig_anchor): the skinned vertex is relative to the anchor in the palette's tail entry (zero: a plain palette); the anchor goes through the matrices as one term of its own
    vec3 rig_a = palette.j[pk.joints].c0.xyz;
    if (rig_a == vec3(0.0))
    {
        gl_Position = ll_view_proj * model * vec4(skinned, 1.0);
    }
    else
    {
        gl_Position = ll_view_proj * (model * vec4(skinned, 0.0)) + ll_view_proj * (model * vec4(rig_a, 1.0));
    }
}
