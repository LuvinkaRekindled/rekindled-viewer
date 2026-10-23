/**
 * @file class1\deferred\terrainPullV.glsl
 *
 * S6.6a: the terrain pass from the geometry packet path. Vertices, normals and the second texture coordinate are read by address from the patch's vertex buffer (the faces are the entries of the record:
 * kind 32), the matrices from the record's matrix table; the outputs are those of terrainV.glsl. (The texture matrix of the old pass is the identity: the pool never sets one.)
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoVec2 { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GeoWords { uint w[]; };

// 160 bytes, as diffusePullV.glsl
struct GeoPacket
{
    uvec2 positions;
    uvec2 normals;
    uvec2 texcoords;
    uvec2 colors;
    uvec2 indices;
    uvec2 tangents;
    uvec2 texcoords1;
    uvec2 texcoords2;
    uint first;
    uint model;
    uint flags;
    uint mapSlots;
    uvec2 slots;
    uvec2 pad;
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad2;
    uvec2 weights;
    uvec2 palette;
    uint joints;
    uint skinPad0;
    uint skinPad1;
    uint skinPad2;
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

uniform vec4 object_plane_s;
uniform vec4 object_plane_t;

out vec3 pos;
out vec3 vary_normal;
out vec4 vary_texcoord0;
out vec4 vary_texcoord1;

void main()
{
    GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = GeoWords(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec3 position = GeoVec4(pk.positions).v[index].xyz;
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
    vec2 texcoord1 = GeoVec2(pk.texcoords1).t[index];
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];

    vec4 pre_pos = vec4(position, 1.0);
    gl_Position = m.mvp * pre_pos;
    pos = (m.mv * pre_pos).xyz;

    vary_normal = normalize(mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz) * normal);

    vary_texcoord0.xy = vec2(dot(pre_pos, object_plane_s), dot(pre_pos, object_plane_t));
    vary_texcoord0.zw = texcoord1;
    vary_texcoord1.xy = texcoord1 - vec2(2.0, 0.0);
    vary_texcoord1.zw = texcoord1 - vec2(1.0, 0.0);
}
