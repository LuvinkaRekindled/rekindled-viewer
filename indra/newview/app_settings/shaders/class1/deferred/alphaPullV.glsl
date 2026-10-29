/**
 * @file alphaPullV.glsl
 * @brief Vertex shader of the forward lit alpha pass drawn from the scene table (Vulkan mirror only, S5a): the work of alphaV.glsl (IS_FULLBRIGHT_PULL: fullbrightV.glsl) for static, unskinned
 *        draws, with the vertex data read from the viewer's buffers by address and the matrices taken from the pass's tables, as diffusePullV.glsl does for the G-buffer.
 *
 * The row of a draw (the 160 byte GeoPacket the worker keeps) names the arrays, the model matrix number, the slots of the draw's diffuse textures in the bindless table and, in pad.x, the slot of
 * its texture matrix (0 = none). The fragment shader is the ordinary alpha shader with a diffuseLookup() that samples the slot this shader passes (LL_BINDLESS_DIFFUSE in llshadermgr.cpp).
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
    uvec2 slots;        // four 16 bit slots of the diffuse textures
    uvec2 pad;          // pad.x: the slot of the draw's texture matrix (0 = none)
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
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoTexMatrices { mat4 t[]; };

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

#ifdef IS_FULLBRIGHT_PULL
void calcAtmospherics(vec3 inPositionEye);
out vec3 vary_position;
out vec4 vertex_color;
out vec2 vary_texcoord0;
#else
uniform float near_clip;
out vec3 vary_fragcoord;
out vec3 vary_position;
out vec4 vertex_color;
out vec2 vary_texcoord0;
out vec3 vary_norm;
#endif
flat out uint vary_slot;
#ifdef LL_MASK_CUTOFF
flat out float minimum_alpha;        // S6 X3: the alpha mask cutoff of the draw
#endif

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
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];

#ifdef HAS_SKIN
    mat3 sm;
    vec3 trans;
    skinTransform(pk, index, sm, trans);
    vec4 vert = vec4(sm * position.xyz + trans, 1.0);
#else
    vec4 vert = vec4(position.xyz, 1.0);
#endif
    vec4 pos = m.mv * vert;
    vec4 clip = m.mvp * vert;
    gl_Position = clip;

    vec2 texcoord = GeoVec2(pk.texcoords).t[index];
    if (pk.pad.x != 0u)
    {
        // <FS> a texture animation's slot holds its parameters (llvktexanim.h): the matrix is made against the clock in slot 0
        mat4 ll_tm = GeoTexMatrices(ll_packet_texmats).t[pk.pad.x];
        if (ll_tm[3][3] == 0.0) ll_tm = ll_texanim_core(ll_tm, GeoTexMatrices(ll_packet_texmats).t[0][0].yz);
        texcoord = (ll_tm * vec4(texcoord, 0.0, 1.0)).xy;
    }
    vary_texcoord0 = texcoord;
    vary_position = pos.xyz;
    vertex_color = unpackUnorm4x8(GeoWords(pk.colors).w[index]);

#ifdef IS_FULLBRIGHT_PULL
    calcAtmospherics(pos.xyz);
#else
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
#ifdef HAS_SKIN
    vary_norm = normalize(mat3(m.mv) * (sm * normal));
#else
    vary_norm = normalize(mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz) * normal);
#endif
    vary_fragcoord = clip.xyz + vec3(0.0, 0.0, near_clip);
#endif

    uint tex_index = ((pk.flags & 1u) != 0u) ? 0u : (floatBitsToUint(position.w) & 3u);
    uint pair = (tex_index < 2u) ? pk.slots.x : pk.slots.y;
    vary_slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
#ifdef LL_MASK_CUTOFF
    minimum_alpha = pk.cutoff;
#endif

#if defined(ALPHA_PULL_DBG)
    // diagnostics (LL_VK_SCENE_ALPHA_DBG): 1 = the vertex colour's rgb is white, 2 = the stand-in texture (slot 0, white), 3 = both
#if (ALPHA_PULL_DBG & 1)
    vertex_color.rgb = vec3(1.0);
#endif
#if (ALPHA_PULL_DBG & 2)
    vary_slot = 0u;
#endif
#if (ALPHA_PULL_DBG & 4) && !defined(IS_FULLBRIGHT_PULL)
    vary_norm = vec3(0.0, 0.0, 1.0);        // 4 = a fixed normal (a bad normal matrix would show as black lighting)
#endif
#endif
}
