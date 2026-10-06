/**
 * @file diffusePullV.glsl
 * @brief Vertex shader of the deferred diffuse pass drawn from render packets (Vulkan mirror only): the work of diffuseV.glsl for static geometry, with the vertex
 *        data read from the viewer's buffers by address and the matrices taken from the record.
 *
 * No vertex attributes and no bound vertex or index buffer: the worker writes one entry per draw (buffer addresses, index range, matrix number, the slots of the
 * draw's textures in the bindless table) and draws every entry as a draw of its own whose instance index is the entry number. The matrices of a record
 * (modelview, modelview * projection and the normal matrix) are computed by the viewer, once per model matrix, the way LLRender::syncMatrices() does. The fragment
 * shader is the one of the ordinary pass (diffuseIndexedF.glsl) with a diffuseLookup() that samples the slot this shader passes (see LL_BINDLESS_DIFFUSE in llshadermgr.cpp).
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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoVec2 { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GeoWords { uint w[]; };

// 96 bytes. flags: 1 = one texture (no texture index in position.w)
struct GeoPacket
{
    uvec2 positions;
    uvec2 normals;
    uvec2 texcoords;
    uvec2 colors;
    uvec2 indices;
    uint first;
    uint model;
    uint flags;
    uint mapSlots;      // normal map slot | specular map slot << 16 (the materials passes)
    uvec2 slots;        // four 16 bit slots of the diffuse textures
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad;
};

// 176 bytes
struct GeoMatrix
{
    mat4 mv;
    mat4 mvp;
    vec4 n0;
    vec4 n1;
    vec4 n2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoPackets { GeoPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoMatrices { GeoMatrix m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;

out vec3 vary_normal;
out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec3 vary_position;
flat out uint vary_slot;

void main()
{
    GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = GeoWords(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec4 position = GeoVec4(pk.positions).v[index];
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];

    vary_position = (m.mv * vec4(position.xyz, 1.0)).xyz;
    gl_Position = m.mvp * vec4(position.xyz, 1.0);
    vary_normal = normalize(mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz) * normal);
    vary_texcoord0 = GeoVec2(pk.texcoords).t[index];
    vertex_color = unpackUnorm4x8(GeoWords(pk.colors).w[index]);

    uint tex_index = ((pk.flags & 1u) != 0u) ? 0u : (floatBitsToUint(position.w) & 3u);
    uint pair = (tex_index < 2u) ? pk.slots.x : pk.slots.y;
    vary_slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
}
