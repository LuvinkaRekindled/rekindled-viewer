/**
 * @file pbrGlowPullV.glsl
 * @brief Vertex shader of the glow (emissive) second draw of the PBR (glTF) alpha items, drawn from the scene table (Vulkan mirror only, S5e): the work of pbrglowV.glsl with the vertex data read from
 *        the viewer's buffers by address, the matrices taken from the pass's tables and the material's values read from the draw's PBR row (see pbrAlphaPullV.glsl, which draws the colour of the same
 *        item from the same row). The fragment shader is pbrglowF.glsl with the texture lookups and per-draw uniforms rewritten as for pbralphaF (LL_BINDLESS_PBR in llshadermgr.cpp).
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

// 160 bytes (the table's row; the rigged tail is not read here)
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
    uvec2 slots;        // slots.x: base colour | normal << 16, slots.y: occlusion roughness metallic | emissive << 16
    uvec2 pad;          // pad.x: the texture animation matrix slot, pad.y: the PBR row
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    uint emissiveOffset;        // byte offset of the emissive array in the vertex buffer
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
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoTexMatrices { mat4 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PbrRows { vec4 q[]; };

#ifdef HAS_SKIN
// S5d: rigged alpha draws: the weight4 array and the joint palette of the draw are read by address, as in diffusePullV.glsl; the draw is an indexed one (the hardware reads the indices)
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinWeights { vec4 w[]; };

struct SkinJoint
{
    vec4 c0;
    vec4 c1;
    vec4 c2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SkinPalette { SkinJoint j[]; };

// as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
void skinTransform(GeoPacket pk, uint index, out mat3 sm, out vec3 trans)
{
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

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;
uniform uvec2 ll_packet_texmats;
uniform uvec2 ll_packet_pbr;

out vec2 base_color_texcoord;
out vec2 emissive_texcoord;
out vec4 vertex_emissive;

flat out uint vary_slot;
flat out uint vary_slot_normal;
flat out uint vary_slot_specular;
flat out uint vary_slot_emissive;
flat out float metallicFactor;
flat out float roughnessFactor;
flat out vec3 emissiveColor;
flat out float minimum_alpha;

vec2 texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform, mat4 sl_animation_transform);

void main()
{
    GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
#if defined(HAS_SKIN) && defined(HAS_INDEXED_DRAW)
    uint index = uint(gl_VertexIndex);          // an indexed draw (the hardware read the index buffer)
#else
    uint i = pk.first + uint(gl_VertexIndex);
    uint word = GeoWords(pk.indices).w[i >> 1];
    uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
#endif
    vec4 position = GeoVec4(pk.positions).v[index];
    vec2 texcoord0 = GeoVec2(pk.texcoords).t[index];
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];
#ifdef HAS_SKIN
    mat3 sm;
    vec3 trans;
    skinTransform(pk, index, sm, trans);
    vec3 placed = sm * position.xyz + trans;
#else
    vec3 placed = position.xyz;
#endif

    PbrRows rows = PbrRows(ll_packet_pbr);
    uint row = pk.pad.y * 10u;
    vec4 base_tr[2] = vec4[2](rows.q[row + 0u], rows.q[row + 1u]);
    vec4 emissive_tr[2] = vec4[2](rows.q[row + 6u], rows.q[row + 7u]);
    vec4 misc0 = rows.q[row + 8u];
    vec4 misc1 = rows.q[row + 9u];

    mat4 texmat = mat4(1.0);
    if (pk.pad.x != 0u)
    {
        texmat = GeoTexMatrices(ll_packet_texmats).t[pk.pad.x];
        // <FS> a texture animation's slot holds its parameters (llvktexanim.h): the matrix is made against the clock in slot 0
        if (texmat[3][3] == 0.0) texmat = ll_texanim_core(texmat, GeoTexMatrices(ll_packet_texmats).t[0][0].yz);
    }

    gl_Position = m.mvp * vec4(placed, 1.0);

    base_color_texcoord = texture_transform(texcoord0, base_tr, texmat);
    emissive_texcoord = texture_transform(texcoord0, emissive_tr, texmat);

    uint carry;
    uvec2 emissive_address = pk.positions;
    emissive_address.x = uaddCarry(emissive_address.x, pk.emissiveOffset, carry);
    emissive_address.y += carry;
    vertex_emissive = unpackUnorm4x8(GeoWords(emissive_address).w[index]);

    vary_slot = pk.slots.x & 0xFFFFu;
    vary_slot_normal = pk.slots.x >> 16;
    vary_slot_specular = pk.slots.y & 0xFFFFu;
    vary_slot_emissive = pk.slots.y >> 16;
    roughnessFactor = misc0.x;
    metallicFactor = misc0.y;
    emissiveColor = vec3(misc0.z, misc0.w, misc1.x);
    minimum_alpha = misc1.y;
}
