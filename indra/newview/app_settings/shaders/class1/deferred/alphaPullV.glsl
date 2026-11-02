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

// <FS> GPU particles (doc/vulkan/PLAN-gpu-particles.md, PLAN-gpu-particles-gpu-sort.md). Two kinds of particle draw, both 6 vertices a particle, the quad made here as LLVOPartGroup::getGeometry did on the CPU:
// facing the camera, up = world up unless the particle follows its velocity; corners 0..3 are the old quad's vertices (up-left, down-left, up-right, down-right).
//  flags & 16 (stage 1): one ROW a particle in the vertex arrays (positions: the centre in agent space, normals.xyz: the velocity of a follow-velocity particle or zero, texcoords: half width and half
//    height, colours, emissive); a ribbon piece is two rows (w of the position 1 and 2).
//  flags & 32 (G0): one RECORD a slot (the laws of LLViewerPartRecord, written by fs_store_write in llviewerpartsim.cpp); the index buffer holds the order of the slots, and the vertex is looked up through it.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ParticleSources { vec4 s[]; };      // s[0] = (now high, now low); a source k is s[1 + 2k] (position), s[2 + 2k] (target)
uniform uvec2 ll_packet_sources;

vec4 ll_rec_color = vec4(1.0);
float ll_rec_glow = 0.0;
bool ll_rec_is = false;

vec3 ll_billboard(GeoMatrix m, vec3 center, vec3 vel, vec2 half_size, uint corner)
{
    mat3 rot = mat3(m.mv);
    vec3 at = rot * center + m.mv[3].xyz;               // from the camera to the particle
    vec3 side = cross(at, m.mv[2].xyz);                 // (mv[2].xyz: the agent up axis in eye space)
    float side_len = length(side);
    vec3 right = (side_len > 1e-8) ? side / side_len : rot * vec3(0.0, 1.0, 0.0);
    vec3 upv = cross(right, at);
    float up_len = length(upv);
    vec3 up = (up_len > 1e-8) ? upv / up_len : m.mv[2].xyz;
    if (dot(vel, vel) > 0.0)
    {
        vec3 nv = rot * vel;
        vec2 f = vec2(dot(nv, right), dot(nv, up));
        float fl = length(f);
        if (fl > 1e-6)
        {
            f /= fl;
            vec3 new_up = f.x * right + f.y * up;
            vec3 new_right = f.y * right - f.x * up;
            up = normalize(new_up);
            right = normalize(new_right);
        }
    }
    float sx = ((corner & 2u) != 0u) ? 1.0 : -1.0;
    float sy = ((corner & 1u) != 0u) ? -1.0 : 1.0;
    vec3 d_eye = right * (sx * half_size.x) + up * (sy * half_size.y);
    return center + d_eye * rot;                        // (back to agent space: the transpose of the rotation)
}

// the vertical motion above a floor at height 0 with the bounce of LLViewerPartRecord::evaluate (fs_bounce_z in llviewerpartsim.cpp): the flights between the bounces
void ll_bounce_z(float z0, float v0, float a, float age, out float z_out, out float v_out)
{
    float z = z0, v = v0, left = age;
    if (z < 0.0)
    {
        z = -z;
        v *= -0.75;
    }
    for (int bounce = 0; bounce < 24; ++bounce)
    {
        float hit = -1.0;
        if (a == 0.0)
        {
            if (v < 0.0)
            {
                hit = max(-z / v, 0.0);
            }
        }
        else
        {
            float disc = v * v - 2.0 * a * z;
            if (disc >= 0.0)
            {
                float root = sqrt(disc);
                float t1 = (-v - root) / a, t2 = (-v + root) / a;
                float lo = min(t1, t2), hi = max(t1, t2);
                if (lo > -1e-7 && v + a * lo < 0.0)
                {
                    hit = max(lo, 0.0);
                }
                else if (hi > -1e-7 && v + a * hi < 0.0)
                {
                    hit = max(hi, 0.0);
                }
            }
        }
        if (hit < 0.0 || hit > left)
        {
            z_out = z + v * left + 0.5 * a * left * left;
            v_out = v + a * left;
            return;
        }
        v = (v + a * hit) * -0.75;
        z = 0.0;
        left -= hit;
        if (abs(v) < 0.02 && a <= 0.0)
        {
            break;
        }
    }
    z_out = 0.0;
    v_out = 0.0;
}

// a record at the frame's clock: LLViewerPartRecord::evaluate, and the colour, scale and glow of the age; the corner of its quad
vec3 ll_record_corner(GeoPacket pk, GeoMatrix m, uint slot, uint corner)
{
    vec4 a = GeoVec4(pk.positions).v[slot];             // pos0, t0 high
    vec4 b = GeoVec4(pk.normals).v[slot];               // vel0, t0 low
    vec4 c = GeoVec4(pk.tangents).v[slot];              // accel, max age
    vec4 d = GeoVec4(pk.weights).v[slot];               // target at spawn, floor height
    vec2 scale0 = GeoVec2(pk.texcoords).t[slot];
    vec2 scale1 = GeoVec2(pk.texcoords1).t[slot];
    uvec2 misc = floatBitsToUint(GeoVec2(pk.texcoords2).t[slot]);
    vec4 color0 = unpackUnorm4x8(GeoWords(pk.colors).w[slot]);
    vec4 color1 = unpackUnorm4x8(misc.y);
    uint flags = misc.x;
    uint mode = (flags >> 16) & 3u;
    bool bounce = ((flags >> 18) & 1u) != 0u;

    ParticleSources table = ParticleSources(ll_packet_sources);
    vec4 clock = table.s[0];
    float age = (clock.x - a.w) + (clock.y - b.w);
    float max_age = c.w;
    float frac = age / max_age;
    bool alive = age >= 0.0 && age <= max_age;

    vec3 pos0 = a.xyz, vel0 = b.xyz, acc = c.xyz;
    vec3 src_pos = vec3(0.0), src_target = vec3(0.0);
    if (mode != 0u)
    {
        uint k = pk.pad.y + (flags >> 24);
        src_pos = table.s[1u + 2u * k].xyz;
        src_target = table.s[2u + 2u * k].xyz;
    }
    vec3 pos, vel;
    if (mode == 2u)
    {
        vec3 delta = src_target - src_pos;
        pos = src_pos + delta * frac;
        vel = delta;
    }
    else if (mode == 3u)
    {
        // the pull towards the target solved (LLViewerPartRecord::evaluate)
        float r0 = max_age, r = max(max_age - age, 0.0);
        float rho = r / r0, rho4 = rho * rho * rho * rho;
        vec3 u0 = (d.xyz - pos0) / r0;
        vec3 w0 = u0 - vel0;
        vec3 cc = w0 + acc * (r0 / 3.0);
        vec3 u = u0 + cc * (0.25 * (1.0 - rho4)) - acc * ((r0 - r) / 3.0);
        vec3 w = cc * rho4 - acc * (r / 3.0);
        pos = src_target - u * r;
        vel = u - w;
    }
    else
    {
        vec3 base = (mode == 1u) ? src_pos : pos0;
        pos = base + vel0 * age + acc * (0.5 * age * age);
        vel = vel0 + acc * age;
        if (bounce)
        {
            float floor_z = (mode == 1u) ? src_pos.z : d.w;
            float z0 = (mode == 1u) ? 0.0 : pos0.z - d.w;
            float z, v;
            ll_bounce_z(z0, vel0.z, acc.z, age, z, v);
            pos.z = floor_z + z;
            vel.z = v;
        }
    }

    vec2 scale = ((flags >> 20) & 1u) != 0u ? scale0 * (1.0 - frac) + frac * scale1 : scale0;
    vec4 color = ((flags >> 19) & 1u) != 0u ? color0 * (1.0 - frac) + color1 * frac : color0;
    float glow_start = float(flags & 255u) / 255.0, glow_end = float((flags >> 8) & 255u) / 255.0;
    ll_rec_color = color;
    ll_rec_glow = floor((glow_start + (glow_end - glow_start) * frac) * 255.0 + 0.5) / 255.0;
    ll_rec_is = true;
    vec3 follow = (((flags >> 21) & 1u) != 0u) ? vel : vec3(0.0);
    vec2 half_size = alive ? 0.5 * scale : vec2(0.0);   // (a dead record draws a degenerate quad)
    return ll_billboard(m, pos, follow, half_size, corner);
}

vec3 ll_particle_corner(GeoPacket pk, GeoMatrix m, out uint row, out uint corner)
{
    uint pi = pk.first + uint(gl_VertexIndex);
    uint n = pi / 6u;
    uint c = pi - n * 6u;
    corner = (c == 0u) ? 0u : ((c == 1u || c == 3u) ? 1u : ((c == 2u || c == 5u) ? 2u : 3u));
    if ((pk.flags & 32u) != 0u)
    {
        uint word = GeoWords(pk.indices).w[n >> 1];
        row = ((n & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
        return ll_record_corner(pk, m, row, corner);
    }
    row = n;
    vec4 center4 = GeoVec4(pk.positions).v[row];
    vec3 center = center4.xyz;
    if (center4.w > 0.5)
    {
        // a ribbon piece: row = the particle (w 1) and row + 1 = the point it joins to (w 2); corners 0 and 1 are on the joined point, 2 and 3 on the particle (the old quad's order); the normals hold the half axis
        if (center4.w > 1.5)
        {
            return center;                          // (the second row draws a degenerate piece: nothing)
        }
        bool joined = corner < 2u;
        vec4 other = GeoVec4(pk.positions).v[row + 1u];
        vec3 axis = GeoVec4(pk.normals).v[joined ? row + 1u : row].xyz;
        float side = ((corner & 1u) == 0u) ? 1.0 : -1.0;
        vec3 at_point = joined ? other.xyz : center;
        if (joined)
        {
            row = row + 1u;                         // (its colour and glow)
        }
        return at_point + axis * side;
    }
    return ll_billboard(m, center, GeoVec4(pk.normals).v[row].xyz, GeoVec2(pk.texcoords).t[row], corner);
}

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
#ifdef HAS_SKIN
    const bool is_particle = false;
#else
    bool is_particle = (pk.flags & 48u) != 0u;
#endif
    uint particle_corner = 0u;
#if defined(HAS_SKIN) && defined(HAS_INDEXED_DRAW)
    uint index = uint(gl_VertexIndex);          // an indexed draw (the hardware read the index buffer)
#else
    uint index;
    if (is_particle)
    {
        index = 0u;                             // (the row, below)
    }
    else
    {
        uint i = pk.first + uint(gl_VertexIndex);
        uint word = GeoWords(pk.indices).w[i >> 1];
        index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
#endif
    GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];
#ifndef HAS_SKIN
    vec3 particle_pos = vec3(0.0);
    if (is_particle)
    {
        particle_pos = ll_particle_corner(pk, m, index, particle_corner);
    }
#endif
    vec4 position = GeoVec4(pk.positions).v[index];

#ifdef HAS_SKIN
    mat3 sm;
    vec3 trans;
    skinTransform(pk, index, sm, trans);
    vec4 vert = vec4(sm * position.xyz + trans, 1.0);
#else
    vec4 vert = vec4(is_particle ? particle_pos : position.xyz, 1.0);
#endif
    vec4 pos = m.mv * vert;
    vec4 clip = m.mvp * vert;
    gl_Position = clip;

    vec2 texcoord = GeoVec2(pk.texcoords).t[index];
    if (is_particle)
    {
        texcoord = vec2(((particle_corner & 2u) != 0u) ? 1.0 : 0.0, ((particle_corner & 1u) != 0u) ? 0.0 : 1.0);
    }
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
    if (ll_rec_is)
    {
        vertex_color = ll_rec_color;            // (a record: the colour of its age)
    }

#ifdef IS_FULLBRIGHT_PULL
    calcAtmospherics(pos.xyz);
#else
    vec3 normal = GeoVec4(pk.normals).v[index].xyz;
#ifdef HAS_SKIN
    vary_norm = normalize(mat3(m.mv) * (sm * normal));
#else
    vary_norm = normalize(mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz) * normal);
    if (is_particle)
    {
        vary_norm = vec3(0.0, 0.0, 1.0);        // (a particle's normal is the direction back to the camera: +z in eye space)
    }
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
