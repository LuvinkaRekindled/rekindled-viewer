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

#ifdef HAS_SKIN
// S4 A1: rigged draws (kind 25 records): the weight4 array and the joint palette of the draw are read by address, as in shadowSkinPullV.glsl
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinWeights { vec4 w[]; };

struct SkinJoint
{
    vec4 c0;
    vec4 c1;
    vec4 c2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinPalette { SkinJoint j[]; };
#endif

// 160 bytes. flags: 1 = one texture (no texture index in position.w)
struct GeoPacket
{
    uvec2 positions;
    uvec2 normals;
    uvec2 texcoords;
    uvec2 colors;
    uvec2 indices;
    uvec2 tangents;     // the materials passes
    uvec2 texcoords1;
    uvec2 texcoords2;
    uint first;
    uint model;
    uint flags;
    uint mapSlots;      // normal map slot | specular map slot << 16 (the materials passes)
    uvec2 slots;        // four 16 bit slots of the diffuse textures
    uvec2 pad;
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad2;
    uvec2 weights;     // 160 bytes: the rigged draws' weights and palette (the entries of the other draws leave them 0)
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

#ifdef LL_MASK_CUTOFF
flat out float minimum_alpha;        // S6 X3: the alpha mask cutoff of the draw (the old pass set it per draw as a uniform)
#endif
out vec3 vary_normal;
out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec3 vary_position;
flat out uint vary_slot;

#ifdef HAS_SKIN
// as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
void skinTransform(GeoPacket pk, uint index, out mat3 sm, out vec3 trans)
{
#ifdef SKIN_NOOP
    sm = mat3(1.0);
    trans = vec3(0.0);
    return;
#endif
    vec4 weight4 = SkinWeights(pk.weights).w[index];
    vec4 w = fract(weight4);
    vec4 joint = floor(weight4);
    joint = clamp(joint, vec4(0.0), vec4(float(pk.joints - 1u)));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    SkinPalette palette = SkinPalette(pk.palette);
    SkinJoint j1 = palette.j[int(joint.x)];
    SkinJoint j2 = palette.j[int(joint.y)];
    SkinJoint j3 = palette.j[int(joint.z)];
    SkinJoint j4 = palette.j[int(joint.w)];

    sm = mat3(j1.c0.xyz, j1.c1.xyz, j1.c2.xyz) * w.x;
    sm += mat3(j2.c0.xyz, j2.c1.xyz, j2.c2.xyz) * w.y;
    sm += mat3(j3.c0.xyz, j3.c1.xyz, j3.c2.xyz) * w.z;
    sm += mat3(j4.c0.xyz, j4.c1.xyz, j4.c2.xyz) * w.w;
    trans = vec3(j1.c0.w, j1.c1.w, j1.c2.w) * w.x;
    trans += vec3(j2.c0.w, j2.c1.w, j2.c2.w) * w.y;
    trans += vec3(j3.c0.w, j3.c1.w, j3.c2.w) * w.z;
    trans += vec3(j4.c0.w, j4.c1.w, j4.c2.w) * w.w;
}
#endif

void main()
{
    GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
#if defined(HAS_SKIN) && defined(HAS_INDEXED_DRAW)
    uint index = uint(gl_VertexIndex);          // S4 A1: an indexed draw (the hardware read the index buffer)
#else
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = GeoWords(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
#endif
    vec4 position = GeoVec4(pk.positions).v[index];
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];

#ifdef HAS_SKIN
    mat3 sm;
    vec3 trans;
    skinTransform(pk, index, sm, trans);
    vec3 skinned = sm * position.xyz + trans;
    vary_position = (m.mv * vec4(skinned, 1.0)).xyz;
    gl_Position = m.mvp * vec4(skinned, 1.0);
    vary_normal = normalize(mat3(m.mv) * (sm * normal));
#else
    vary_position = (m.mv * vec4(position.xyz, 1.0)).xyz;
    gl_Position = m.mvp * vec4(position.xyz, 1.0);
    vary_normal = normalize(mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz) * normal);
#endif
    vary_texcoord0 = GeoVec2(pk.texcoords).t[index];
    vertex_color = unpackUnorm4x8(GeoWords(pk.colors).w[index]);

    uint tex_index = ((pk.flags & 1u) != 0u) ? 0u : (floatBitsToUint(position.w) & 3u);
    uint pair = (tex_index < 2u) ? pk.slots.x : pk.slots.y;
    vary_slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
#ifdef LL_MASK_CUTOFF
    minimum_alpha = pk.cutoff;
#endif
}
