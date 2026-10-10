/**
 * @file class1/deferred/shadowUtil.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

uniform sampler2D   normalMap;

#if defined(SUN_SHADOW)
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#endif

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4;
uniform sampler2DShadow shadowMap5;
#endif

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform vec2 shadow_res;
uniform vec2 proj_shadow_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform float shadow_offset;
uniform float spot_shadow_bias;
uniform float spot_shadow_offset;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;

float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir)
{
#if defined(SUN_SHADOW)
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;
    stc.z += offset * 2.0;
    stc.x = floor(stc.x*shadow_res.x + fract(pos_screen.y*shadow_res.y))/shadow_res.x; // add some chaotic jitter to X sample pos according to Y to disguise the snapping going on here
    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs * 4.0;
    shadow += texture(shadowMap, stc.xyz+vec3( 1.5/shadow_res.x,  0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3( 0.5/shadow_res.x, -1.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-1.5/shadow_res.x, -0.5/shadow_res.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-0.5/shadow_res.x,  1.5/shadow_res.y, 0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
#else
    return 1.0;
#endif
}

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
// <FS> Ray query sun shadow (LL_VK_RT_SHADOWS): one ray from the surface towards the sun against the frame's scene (llvkrt.cpp), in eye space like everything here.
// Opaque casters commit their hit at once; alpha masked ones (instance mask 2) hand each candidate triangle to rtAlphaHit, which repeats the test of shadowAlphaPullF.glsl
// with the texture of the geometry record (set 3 binding 1) read through the frame's bindless table. Rigged avatars are not in the scene yet (PLAN-rt-shadows.md stage C).
bool rtAlphaHit(uint record, uint prim, vec2 bary, vec2 pos_screen, float lod)
{
    LLRtGeom g = ll_rt_geoms.g[record];
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    LLRtIndices ib = LLRtIndices(g.indices);
    uint ix[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        uint i = g.first + prim * 3u + k;
        uint word = ib.w[i >> 1];
        ix[k] = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
    }
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = g.uv0 * uv.x + g.uv1 * uv.y + g.uv2;          // the texture animation / glTF transform of the draw
    }
    uint tex_index = ((g.flags & 4u) != 0u) ? 0u : (floatBitsToUint(LLRtPositions(g.positions).v[ix[0]].w) & 3u);
    uint pair = (tex_index < 2u) ? g.slots.x : g.slots.y;
    uint slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    float alpha = textureLod(ll_rt_tex[nonuniformEXT(slot)], uv, lod).a;
    if ((g.flags & 64u) != 0u)
    {
        // glTF alpha mask: texture alpha times vertex alpha against the material's cutoff, no dithering (pbrShadowAlphaMaskF.glsl)
        if ((g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
        return alpha >= g.cutoff;
    }
    if (alpha < g.cutoff)
    {
        return false;
    }
    if ((g.flags & 2u) == 0u)
    {
        if ((g.flags & 1u) == 0u && (g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
        if (alpha < 0.05)
        {
            return false;                   // totally transparent
        }
        if (alpha < 0.88 && fract(0.5 * floor(pos_screen.x)) < 0.25)
        {
            return false;                   // semi-transparent: dithered like the shadow maps
        }
    }
    return true;
}

float rtSunShadow(vec3 pos, vec3 norm, vec3 light_dir, vec2 pos_screen)
{
    if (dot(norm, light_dir) <= 0.0)
    {
        return 0.0;                         // facing away from the light
    }
    if (-pos.z > shadow_clip.w)
    {
        return 1.0;                         // beyond the shadow range: lit, as with the cascades
    }
    vec3 origin = pos + norm * (0.015 + 0.0006 * -pos.z);
    float lod = clamp(0.5 * log2(max(-pos.z, 1.0)) - 1.0, 0.0, 4.0);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0x03, origin, 0.005, light_dir, 2000.0);
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            if (rtAlphaHit(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), pos_screen, lod))
            {
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
#endif

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
float sampleCascadeShadow(vec3 pos, vec3 norm, vec2 pos_screen)
#else
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
#endif
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            lpos = shadow_matrix[3]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            lpos = shadow_matrix[2]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.y, 0.0)/transition_domain.y;
            w -= max(near_split.z-spos.z, 0.0)/transition_domain.z;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            lpos = shadow_matrix[1]*spos;

            float w = 1.0;
            w -= max(spos.z-far_split.x, 0.0)/transition_domain.x;
            w -= max(near_split.y-spos.z, 0.0)/transition_domain.y;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            lpos = shadow_matrix[0]*spos;

            float w = 1.0;
            w -= max(near_split.x-spos.z, 0.0)/transition_domain.x;
            //w = clamp(w, 0.0, 1.0);
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir)*w;
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
    }
    else
    {
        return 1.0f; // lit beyond the far split...
    }
    //shadow = min(dp_directional_light,shadow);
    return shadow;
#else
    return 1.0;
#endif
}

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    float rt = rtSunShadow(pos, norm, light_dir, pos_screen);
#if LL_RT_SHADOWS == 1
    return rt;
#elif LL_RT_SHADOWS == 5
    // debug: rays only, with the sun swung sideways so that shadows fall far from their casters (a high sun puts them under the feet and hides what is in the scene)
    return rtSunShadow(pos, norm, normalize(light_dir + vec3(1.2, 0.0, 0.0)), pos_screen);
#elif LL_RT_SHADOWS == 4
    // x-ray of the skinned avatar structures (instance mask 4): a ray from the eye through this pixel; lit where the ray scene has a surface at the depth of the visible one,
    // dark elsewhere. Shows what of the avatars the ray scene really covers, in their current pose.
    float depth = length(pos);
    rayQueryEXT xq;
    rayQueryInitializeEXT(xq, ll_rt_tlas, gl_RayFlagsOpaqueEXT, 0x04, vec3(0.0), 0.01, pos / depth, depth + 3.0);
    while (rayQueryProceedEXT(xq)) { }
    if (rayQueryGetIntersectionTypeEXT(xq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return 0.0;
    }
    return abs(rayQueryGetIntersectionTEXT(xq, true) - depth) < max(0.04, 0.012 * depth) ? 1.0 : 0.0;
#elif LL_RT_SHADOWS == 2
    // split screen: the cascades on the left half, the rays on the right
    if (abs(pos_screen.x * 2.0 - screen_res.x) < 4.0)
    {
        return 0.0;                         // a dark line where the halves meet
    }
    return (pos_screen.x * 2.0 < screen_res.x) ? sampleCascadeShadow(pos, norm, pos_screen) : rt;
#else
    // disagreement map: white where the two do not agree about lit and shadowed, black elsewhere
    float cascade = sampleCascadeShadow(pos, norm, pos_screen);
    return abs(cascade - rt) > 0.5 ? 1.0 : 0.0;
#endif
}
#endif

float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    float shadow = 0.0f;
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);
    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;

        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        {
            float w = 1.0;
            w -= max(spos.z-far_split.z, 0.0)/transition_domain.z;

            if (index == 0)
            {
                lpos = shadow_matrix[4]*spos;
                shadow += pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy)*w;
            }
            else
            {
                lpos = shadow_matrix[5]*spos;
                shadow += pcfSpotShadow(shadowMap5, lpos, 0.8, spos.xy)*w;
            }
            weight += w;
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
        }

        shadow /= weight;
    }
    else
    {
        shadow = 1.0f;
    }
    return shadow;
#else
    return 1.0;
#endif
}

