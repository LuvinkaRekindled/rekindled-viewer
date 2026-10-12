/**
 * @file rtMaterialDebugF.glsl
 *
 * <FS> Ray scene material table debug view (doc/vulkan/PLAN-rt-materials.md, stage M2).
 * $LicenseInfo:firstyear=2026:viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc. and Firestorm contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

// A ray from the eye through every pixel, traced against the ray scene, shaded from the geometry records and the material table only (no G-buffer, no cascades). The proof that
// records, addresses, uvs, vertex colours, texture slots and materials are right before anything depends on them (reflections).
// Modes (uniform rt_debug_mode): 1 the hit's albedo, 2 the G-buffer's albedo, 3 the difference between them (x4), 4 the hit's flat normal, 5 the material id as a colour,
// 6 status: green hit with a material, yellow hit without a material row, red hit with an unusable record, blue the ray found nothing where the G-buffer has a surface,
// magenta the hit is at another depth than the G-buffer's surface (above 3 %), black sky.
out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform int rt_debug_mode;

vec4 getPosition(vec2 pos_screen);

void dbgSceneFrame(out mat3 to_scene, out vec3 camera)
{
    LLRtGeom h = ll_rt_geoms.g[0];
    to_scene = mat3(uintBitsToFloat(uvec3(h.positions.x, h.positions.y, h.indices.x)), uintBitsToFloat(uvec3(h.indices.y, h.texcoords.x, h.texcoords.y)),
                    uintBitsToFloat(uvec3(h.colors.x, h.colors.y, h.slots.x)));
    camera = uintBitsToFloat(uvec3(h.slots.y, h.flags, floatBitsToUint(h.cutoff)));
}

// what a hit looks like through its record and material. status: 0 fine, 1 unusable record, 2 no material row. Returns false when the record cannot be read at all.
float dbg_raw_alpha = 0.0;      // mode 10: the texture alpha x vertex alpha x colour alpha of the last surface looked at, the cutoff it is held against, whether the material is alpha masked
float dbg_cutoff = 0.5;
bool dbg_masked = false;

bool dbgSurface(uint record, uint prim, vec2 bary, float lod, out vec4 albedo, out vec3 flat_normal, out uint mat_id, out uint status)
{
    albedo = vec4(1.0, 0.0, 1.0, 1.0);
    flat_normal = vec3(0.0, 1.0, 0.0);
    mat_id = 0u;
    status = 1u;
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u || (g.indices.x == 0u && g.indices.y == 0u) || (g.positions.x == 0u && g.positions.y == 0u))
    {
        return false;
    }
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    LLRtIndices ib = LLRtIndices(g.indices);
    uint ix[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        uint i = g.first + prim * 3u + k;
        uint word = ib.w[i >> 1];
        ix[k] = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    LLRtPositions pb = LLRtPositions(g.positions);
    vec3 p0 = pb.v[ix[0]].xyz, p1 = pb.v[ix[1]].xyz, p2 = pb.v[ix[2]].xyz;
    flat_normal = normalize(cross(p1 - p0, p2 - p0));
    mat_id = g.material;
    status = 2u;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        albedo = vec4(0.5, 0.5, 0.5, 1.0);
        return true;
    }
    LLRtMaterial m = LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id];
    status = 0u;
    uint mflags = floatBitsToUint(m.params.z);
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = m.uv0 * uv.x + m.uv1 * uv.y + m.uv2;
    }
    uint tex_index = 0u;
    if ((mflags & 16u) != 0u)
    {
        tex_index = floatBitsToUint(pb.v[ix[0]].w) & 3u;
    }
    uint pair = (tex_index < 2u) ? m.slots.x : m.slots.y;
    uint slot = ll_rt_slot((pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu);       // the material holds pin indices
    vec4 tex = textureLod(ll_rt_tex[nonuniformEXT(slot)], uv, lod);
    vec4 vcol = vec4(1.0);
    if ((g.flags & 16u) != 0u)
    {
        LLRtColors col = LLRtColors(g.colors);
        vcol = (unpackUnorm4x8(col.c[ix[0]]) * w.x + unpackUnorm4x8(col.c[ix[1]]) * w.y + unpackUnorm4x8(col.c[ix[2]]) * w.z);
    }
    if ((mflags & 64u) != 0u)
    {
        vcol.a = 1.0;                                           // the legacy materials passes test the texture alpha alone
    }
    albedo = tex * vcol * m.color;
    // the G-buffer holds sRGB values for legacy materials and linear ones for glTF: modes 1 to 3 are compared in the G-buffer's space
    // (legacy materials keep their sRGB values in the G-buffer, glTF ones are linear: only glTF is converted here)
    if ((mflags & 2u) != 0u)
    {
        albedo.rgb = mix(albedo.rgb / 12.92, pow((albedo.rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), albedo.rgb));
    }
    dbg_raw_alpha = albedo.a;
    dbg_cutoff = ((mflags & 4u) != 0u) ? m.emissive.w : 0.5;
    dbg_masked = (mflags & 4u) != 0u;
    if ((mflags & 4u) != 0u)
    {
        albedo.a = (albedo.a >= m.emissive.w) ? 1.0 : 0.0;     // alpha masked: the draw's own cutoff, as the G-buffer pass applies it (hair edges, foliage)
    }
    if ((mflags & 1u) != 0u)
    {
        albedo.a = 1.0;
    }
    return true;
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec3 ref = texture(diffuseRect, uv).rgb;
    vec3 pos = getPosition(uv).xyz;
    float depth = length(pos);
    if (depth > 1500.0 || depth < 0.01)
    {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    mat3 to_scene;
    vec3 camera;
    dbgSceneFrame(to_scene, camera);
    float lod = clamp(0.5 * log2(max(depth, 1.0)) - 1.0, 0.0, 4.0);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, 0, 0x03, camera, 0.01, to_scene * (pos / depth), depth * 1.5 + 5.0);
    uint steps = 0u;
    while (rayQueryProceedEXT(rq))
    {
        if (++steps > 96u)
        {
            rayQueryTerminateEXT(rq);
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            // a masked structure: the material's alpha decides, as the shadow alpha test does with its own table
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            vec4 a;
            vec3 n;
            uint mid, st;
            if (dbgSurface(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), lod, a, n, mid, st))
            {
                if (a.a >= 0.5 || rt_debug_mode == 7 || rt_debug_mode == 10 || (rt_debug_mode == 9 && ((int(gl_FragCoord.x) / 12 + int(gl_FragCoord.y) / 12) & 1) == 1))       // mode 7: no alpha test (shows which triangles exist, cards included); mode 9: the difference view, alpha test off in a checkerboard of 12 pixel blocks
                {
                    rayQueryConfirmIntersectionEXT(rq);
                }
            }
            else
            {
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        frag_color = (rt_debug_mode == 6) ? vec4(0.0, 0.2, 1.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float t = rayQueryGetIntersectionTEXT(rq, true);
    uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
    vec4 albedo;
    vec3 n_obj;
    uint mat_id, status;
    bool ok = dbgSurface(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), rayQueryGetIntersectionBarycentricsEXT(rq, true), lod, albedo, n_obj, mat_id, status);
    mat4x3 w2o = rayQueryGetIntersectionWorldToObjectEXT(rq, true);
    vec3 n_world = normalize(n_obj * mat3(w2o));
    if (rt_debug_mode == 10)
    {
        // red: the surface alpha, green: 1 where it passes the material cutoff (0 where the alpha test would drop it), blue: the cutoff; blue only (no red) = alpha is 0 and the material is not masked
        frag_color = vec4(dbg_raw_alpha, dbg_raw_alpha >= dbg_cutoff ? 1.0 : 0.0, dbg_masked ? dbg_cutoff : 0.0, 1.0);
    }
    else
    if (rt_debug_mode == 1 || rt_debug_mode == 7)
    {
        frag_color = vec4(ok ? albedo.rgb : vec3(1.0, 0.0, 1.0), 1.0);
    }
    else if (rt_debug_mode == 2)
    {
        frag_color = vec4(ref, 1.0);
    }
    else if (rt_debug_mode == 3 || rt_debug_mode == 9)
    {
        frag_color = vec4(clamp(abs(albedo.rgb - ref) * 4.0, 0.0, 1.0), 1.0);
    }
    else if (rt_debug_mode == 4)
    {
        frag_color = vec4(n_world * 0.5 + 0.5, 1.0);
    }
    else if (rt_debug_mode == 5)
    {
        frag_color = vec4(fract(float(mat_id) * vec3(0.1031, 0.1030, 0.0973) * 7.0) * 0.8 + 0.1, 1.0);
    }
    else
    {
        vec3 c = vec3(0.0, 0.8, 0.0);
        if (!ok)
        {
            c = vec3(1.0, 0.0, 0.0);
        }
        else if (status == 2u)
        {
            c = vec3(0.9, 0.8, 0.0);
        }
        if (abs(t - depth) > 0.03 * depth + 0.05)
        {
            c = vec3(0.7, 0.0, 0.7);
        }
        frag_color = vec4(c, 1.0);
    }
}
