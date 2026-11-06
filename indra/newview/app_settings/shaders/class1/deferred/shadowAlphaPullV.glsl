/**
 * @file shadowAlphaPullV.glsl
 * @brief Alpha masked shadow caster vertex shader that reads its vertices, indices, texture coordinates and colours from the viewer's buffers by address
 *        (render packets with textures, Vulkan mirror only).
 *
 * Like shadowPullV.glsl: no vertex attributes, one table entry per draw, the entry number is the draw's instance index. Each entry also names the
 * draw's textures by their slot in the bindless table of the frame (set 2), the alpha cutoff and a few flags; the fragment shader samples the slot the
 * vertex's texture index (position.w, as in the indexed texture shaders) picks.
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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaPositions { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer AlphaTexcoords { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer AlphaColors { uint c[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer AlphaIndices { uint w[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaWeights { vec4 w[]; };

struct AlphaJoint
{
    vec4 c0;
    vec4 c1;
    vec4 c2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaPalette { AlphaJoint j[]; };

// 80 bytes. flags: 1 = fullbright (the vertex colour's alpha does not count), 2 = tree style (no vertex alpha, no dithering), 4 = one texture (no index in
// position.w), 8 = texcoords present, 16 = colours present, 32 = rigged (weights and palette, joints = the palette's joint count)
struct AlphaPacket
{
    uvec2 positions;
    uvec2 indices;
    uvec2 texcoords;
    uvec2 colors;
    uint first;
    uint joints;
    uint model;
    uint flags;
    uvec2 slots;        // four 16 bit slots
    float cutoff;
    uint pad;
    uvec2 weights;
    uvec2 palette;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer AlphaPackets { AlphaPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaModels { mat4 m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;
uniform uvec2 ll_packet_texmats;        // C5 J3: the record's texture matrices (pad of a packet = its number, 0 = none; slot 0 holds the texture animation clock)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaTexmats { mat4 t[]; };
uniform mat4 ll_view_proj;
uniform int shadow_alpha_indexed;   // 1: an indexed draw (the hardware read the index buffer: gl_VertexIndex is the vertex), 0: a draw over the index list that reads the indices here
uniform float ll_target_width;

out vec4 post_pos;
out float target_pos_x;
out float vary_alpha;
out vec2 vary_texcoord0;
flat out uint vary_slot;
flat out uint vary_flags;
flat out float vary_cutoff;

void main()
{
    AlphaPacket pk = AlphaPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint index = uint(gl_VertexIndex);
    if (shadow_alpha_indexed == 0)
    {
        uint word = AlphaIndices(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    vec4 position = AlphaPositions(pk.positions).v[index];
    vec3 local = position.xyz;
    if ((pk.flags & 32u) != 0u)
    {
        // as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
        vec4 weight4 = AlphaWeights(pk.weights).w[index];
        vec4 w = fract(weight4);
        vec4 joint = clamp(floor(weight4), vec4(0.0), vec4(float(pk.joints - 1u)));
        w *= 1.0 / (w.x + w.y + w.z + w.w);
        AlphaPalette palette = AlphaPalette(pk.palette);
        AlphaJoint j1 = palette.j[int(joint.x)];
        AlphaJoint j2 = palette.j[int(joint.y)];
        AlphaJoint j3 = palette.j[int(joint.z)];
        AlphaJoint j4 = palette.j[int(joint.w)];
        mat3 m = mat3(j1.c0.xyz, j1.c1.xyz, j1.c2.xyz) * w.x;
        m += mat3(j2.c0.xyz, j2.c1.xyz, j2.c2.xyz) * w.y;
        m += mat3(j3.c0.xyz, j3.c1.xyz, j3.c2.xyz) * w.z;
        m += mat3(j4.c0.xyz, j4.c1.xyz, j4.c2.xyz) * w.w;
        vec3 trans = vec3(j1.c0.w, j1.c1.w, j1.c2.w) * w.x;
        trans += vec3(j2.c0.w, j2.c1.w, j2.c2.w) * w.y;
        trans += vec3(j3.c0.w, j3.c1.w, j3.c2.w) * w.z;
        trans += vec3(j4.c0.w, j4.c1.w, j4.c2.w) * w.w;
        local = m * position.xyz + trans;
    }
    vec4 pos = ll_view_proj * AlphaModels(ll_packet_models).m[pk.model] * vec4(local, 1.0);

    target_pos_x = 0.5 * (ll_target_width - 1.0) * pos.x;
    post_pos = pos;
    gl_Position = pos;

    uint tex_index = ((pk.flags & 4u) != 0u) ? 0u : (floatBitsToUint(position.w) & 3u);
    uint pair = (tex_index < 2u) ? pk.slots.x : pk.slots.y;
    vary_slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    vary_flags = pk.flags;
    vary_cutoff = pk.cutoff;
    vary_texcoord0 = ((pk.flags & 8u) != 0u) ? AlphaTexcoords(pk.texcoords).t[index] : vec2(0.0);
    if (pk.pad != 0u && (pk.flags & 8u) != 0u)
    {
        mat4 ll_tm = AlphaTexmats(ll_packet_texmats).t[pk.pad];
        if (ll_tm[3][3] == 0.0) ll_tm = ll_texanim_core(ll_tm, AlphaTexmats(ll_packet_texmats).t[0][0].yz);       // a texture animation's slot holds its parameters (llvktexanim.h)
        vary_texcoord0 = (ll_tm * vec4(vary_texcoord0, 0.0, 1.0)).xy;
    }
    vary_alpha = ((pk.flags & 16u) != 0u) ? float(AlphaColors(pk.colors).c[index] >> 24) * (1.0 / 255.0) : 1.0;
}
