/**
 * @file materialPullV.glsl
 * @brief Vertex shader of the deferred materials passes drawn from render packets (Vulkan mirror only): the work of materialV.glsl for static geometry, with the vertex
 *        data read from the viewer's buffers by address, the matrices taken from the record and the per-draw material values passed on to the fragment shader as
 *        flat varyings (the fragment shader is materialF.glsl with these uniforms and its samplers turned into inputs, see LL_BINDLESS_MATERIAL in llshadermgr.cpp).
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

// 128 bytes
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
    uint mapSlots;      // normal map slot | specular map slot << 16
    uvec2 slots;        // four 16 bit slots of the diffuse textures
    uvec2 pad;
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad2;
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

out vec3 vary_position;
out vec3 vary_normal;
out vec4 vertex_color;
out vec2 vary_texcoord0;

#ifdef HAS_NORMAL_MAP
out vec3 vary_tangent;
flat out float vary_sign;
out vec2 vary_texcoord1;
flat out uint vary_slot_normal;
#endif

#ifdef HAS_SPECULAR_MAP
out vec2 vary_texcoord2;
flat out uint vary_slot_specular;
#endif

flat out uint vary_slot;
flat out float emissive_brightness;
flat out float env_intensity;
flat out vec4 specular_color;
#if (DIFFUSE_ALPHA_MODE == 2)
flat out float minimum_alpha;
#endif

void main()
{
    GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = GeoWords(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    vec4 position = GeoVec4(pk.positions).v[index];
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];
    mat3 normal_matrix = mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz);

    gl_Position = m.mvp * vec4(position.xyz, 1.0);
    vary_position = (m.mv * vec4(position.xyz, 1.0)).xyz;
    vary_texcoord0 = GeoVec2(pk.texcoords).t[index];
    vary_normal = normalize(normal_matrix * normal);
    vertex_color = unpackUnorm4x8(GeoWords(pk.colors).w[index]);

#ifdef HAS_NORMAL_MAP
    vec4 tangent = GeoVec4(pk.tangents).v[index];
    vary_texcoord1 = GeoVec2(pk.texcoords1).t[index];
    vary_tangent = normalize(normal_matrix * tangent.xyz);
    vary_sign = tangent.w;
    vary_slot_normal = pk.mapSlots & 0xFFFFu;
#endif

#ifdef HAS_SPECULAR_MAP
    vary_texcoord2 = GeoVec2(pk.texcoords2).t[index];
    vary_slot_specular = pk.mapSlots >> 16;
#endif

    vary_slot = pk.slots.x & 0xFFFFu;
    emissive_brightness = pk.brightness;
    env_intensity = pk.envIntensity;
    specular_color = pk.specular;
#if (DIFFUSE_ALPHA_MODE == 2)
    minimum_alpha = pk.cutoff;
#endif
}
