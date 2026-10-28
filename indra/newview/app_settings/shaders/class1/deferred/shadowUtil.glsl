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
    // record 0 (the header) carries the number of records this frame's table holds: a geometry that points past them (a structure that does not match its records) would make this
    // function follow addresses from memory that is not a record, and a GPU fault loses the device for good. Such a hit is not a hit.
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u)
    {
        return false;                       // a record the scene could not vouch for this frame: nothing is read
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
    if ((g.flags & 4096u) != 0u)
    {
        slot = ll_rt_slot(slot);            // the record holds a pin index (the persistent bindless table)
    }
    float alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, lod).a;
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

// Coverage (0..1) of a masked or blended candidate for the SOFT shadows of traced hits (rtShadowRaySoft): the texture alpha is read from a blurred mip (hair strands and foliage are cut-outs finer than a
// pixel: a binary ray gives hard stripes where the main view's filtered shadow maps give a soft shade), and a ramp around the cutoff turns it into a fraction.
// LL_VK_RT_SUN_BLENDCOVER (default 1): a BLENDED card (record flag 512, the alpha pool's draws: most hair) covers by its alpha itself, which is what blending draws (a texel at alpha 0.4 lets 60 % through).
// The ramp around the cutoff is for masked cards; applied to blended hair (cutoff 0.598, the shadow maps' cut) it dropped every wisp under 0.43 from the shadow and made everything over 0.76 a solid.
#ifndef LL_RT_SUN_BLEND_COVER
#define LL_RT_SUN_BLEND_COVER 1
#endif
float rtAlphaCover(uint record, uint prim, vec2 bary, float lod)
{
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return 0.0;
    }
    LLRtGeom g = ll_rt_geoms.g[record];
    if ((g.flags & 256u) != 0u)
    {
        return 0.0;
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
    vec2 uv = vec2(0.0);
    if ((g.flags & 8u) != 0u)
    {
        LLRtTexcoords tc = LLRtTexcoords(g.texcoords);
        uv = tc.t[ix[0]] * w.x + tc.t[ix[1]] * w.y + tc.t[ix[2]] * w.z;
        uv = g.uv0 * uv.x + g.uv1 * uv.y + g.uv2;
    }
    uint tex_index = ((g.flags & 4u) != 0u) ? 0u : (floatBitsToUint(LLRtPositions(g.positions).v[ix[0]].w) & 3u);
    uint pair = (tex_index < 2u) ? g.slots.x : g.slots.y;
    uint slot = (pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu;
    if ((g.flags & 4096u) != 0u)
    {
        slot = ll_rt_slot(slot);
    }
    float alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, lod + 2.0).a;
    if ((g.flags & 64u) != 0u || ((g.flags & 2u) == 0u && (g.flags & 1u) == 0u))
    {
        if ((g.flags & 16u) != 0u)
        {
            LLRtColors col = LLRtColors(g.colors);
            alpha *= (float(col.c[ix[0]] >> 24) * w.x + float(col.c[ix[1]] >> 24) * w.y + float(col.c[ix[2]] >> 24) * w.z) * (1.0 / 255.0);
        }
    }
#if LL_RT_SUN_BLEND_COVER
    if ((g.flags & 512u) != 0u)
    {
        return clamp(alpha, 0.0, 1.0);
    }
#endif
    return clamp((alpha - g.cutoff) * 3.0 + 0.5, 0.0, 1.0);
}

// The scene's instances are in world space relative to an origin the scene call chose, so they do not depend on the camera: record 0 of the geometry table holds how a ray
// moves from eye space into that space: a rotation (3 columns) and the camera's place in it (12 floats packed into the first 48 bytes of the record).
void rtSceneFrame(out mat3 to_scene, out vec3 camera)
{
    LLRtGeom h = ll_rt_geoms.g[0];
    to_scene = mat3(uintBitsToFloat(uvec3(h.positions.x, h.positions.y, h.indices.x)), uintBitsToFloat(uvec3(h.indices.y, h.texcoords.x, h.texcoords.y)),
                    uintBitsToFloat(uvec3(h.colors.x, h.colors.y, h.slots.x)));
    camera = uintBitsToFloat(uvec3(h.slots.y, h.flags, floatBitsToUint(h.cutoff)));
}

// Soft shadows: the sun is a disc, not a point. Each pixel traces rt_sun_samples rays towards random points of the sun's disc (tangent of its angular radius: rt_sun_radius), so a
// shadow edge is as soft as the distance between the caster and the receiver makes it (hard where they touch). One or two rays per pixel are noisy: the sun lightmap's bilateral blur
// (blurLightF.glsl) smooths them. Both uniforms are set by the sun lightmap pass only; every other shader that samples the sun shadow (alpha, water) leaves them 0 and gets one hard ray.
uniform float rt_sun_radius;
uniform int rt_sun_samples;
uniform float rt_sun_alpha;            // <FS> 1 in the alpha passes (blended surfaces: hair), 0 elsewhere (an opaque surface)
uniform float rt_sun_maxdist;          // <FS> metres: a blended (alpha) pixel farther than this is lit without a shadow ray (set by the alpha passes; 0 = no limit, every other shader)
uniform vec2 rt_sun_lod;                // <FS> (near, far) metres: the rays of the sun disc go from rt_sun_samples at `near` down to a quarter of them (at least 2) at `far` and beyond; 0 = all of them (set by the alpha pass only)
uniform float rt_noise_frame;
uniform float rt_sun_self_off;         // <FS> 1: the avatar self shadow work off (an avatar blocks fully, no receiver probe, no start disc); rt-debug.txt sunself=0, a same-lap cost A/B. 0 (unset) = as the defines say

// A ray whose origin or direction is NaN or infinite (a pixel whose eye space position was rebuilt from a zero or far-plane depth, a scene frame record not written yet) is undefined for a
// ray query (GPU-assisted validation: "Ray Origin contains a NaN"): no ray is cast, the caller takes the no-hit answer.
bool rtRayOk(vec3 o, vec3 d)
{
    return !(any(isnan(o)) || any(isinf(o)) || any(isnan(d)) || any(isinf(d)));
}

// Test switch of the traced glossy reflection (rt-debug.txt glossyrt=20 / 21 / 22, set by the soften pass only; 0 everywhere else): how a hit is lit by the sun. 1: always sunlit (no shadow ray),
// 2: the shadow ray starts 0.15 m off the surface (instead of 0.02 m), 3: a reflection ray may look at 1024 masked candidates (instead of 64) before it gives up.
uniform int rt_hit_shadow_variant;
// <FS> Hair in the traced mirror (test switch, second number of D:/claude/rr-debug.txt): a skinned alpha MASKED draw (hair cards) is tested hit or miss against its cutoff at the pixel's sub-pixel position, so
// thin strands pop in and out while the view moves (nothing averages one ray per pixel then). 1: the cutoff becomes a smooth step and such a candidate is treated like a blended layer (the opacity
// it has, not all or nothing), 2: also the opacity of the layer is the combined opacity of every such card the ray crossed (order independent), 3: the same on a blurrier mip of the alpha.
uniform int rt_hair_soft;
bool rt_soft_masked = false;            // set by rtShadeSurfaceEx: the last surface looked at was a skinned masked draw taken softly
uniform int rt_hit_shadow_samples;      // shadow rays per local light at a traced hit: 1 (one ray at the light's centre), 2 or 4 (the light is a disc, soft shadows); 0 = not set (4)

// <FS> the number of masked candidates the sun ray may test before it stops and calls the pixel lit (LL_VK_RT_CANDIDATES, default 96). Under a dense canopy a ray meets hundreds of leaf quads; at 96 whether it reaches
// an opaque hit or enough leaves depends on the ray's start and the traversal order, so the shadow flips with the view. LL_VK_RT_CAPDEBUG=1: a ray that stopped at the limit is drawn fully shadowed instead of lit.
#ifndef LL_RT_MAX_CANDIDATES
#define LL_RT_MAX_CANDIDATES 96u
#endif
#ifdef LL_RT_CAP_DEBUG
#define LL_RT_CAPPED_RESULT 0.0
#else
#define LL_RT_CAPPED_RESULT 1.0
#endif

// 1 = nothing in the way for tmax metres (the sun: 2000, a local light: its distance), 0 = blocked
float rtShadowRay(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir) || !(tmax > 0.005))
    {
        return 1.0;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0x0F, ray_origin, 0.005, ray_dir, tmax);
    // a ray is given a limited number of masked candidates: a structure that is not what it should be (garbage geometry spread over the scene) must cost a frame some shadows, not
    // a GPU job that outlasts the 2 seconds the driver allows (VK_ERROR_DEVICE_LOST: the viewer cannot go on). The ray stops with what it has found: no hit, lit.
    uint candidates = 0u;
    bool capped = false;
    while (rayQueryProceedEXT(rq))
    {
        if (++candidates > LL_RT_MAX_CANDIDATES)
        {
            rayQueryTerminateEXT(rq);
            capped = true;
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            if (rtAlphaHit(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), pos_screen, lod))
            {
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
    }
    if (capped && rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return LL_RT_CAPPED_RESULT;
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

float rtSunRay(vec3 origin, vec3 dir, vec2 pos_screen, float lod)
{
    return rtShadowRay(origin, dir, 2000.0, pos_screen, lod);
}

// Soft version for the shadows of traced hits (the glossy mirror, local lights and the sun at a hit): masked and blended candidates do not stop the ray, each one lets through the fraction its blurred
// alpha leaves (rtAlphaCover), the product is the light that arrives; an opaque hit blocks all. 1 = lit, 0 = blocked.
// LL_VK_RT_SUN_SELFNEAR (metres, default 0.8): the distance up to which a skinned avatar surface counts as a translucent self shadow (see rtShadowRaySoftS). The floor of the light fades out between 40 % of it and
// all of it, so a hand held at that distance does not cast two shadows (a light one for the fingers inside the limit and a dark one for the wrist outside it)
#ifndef LL_RT_SUN_SELF_NEAR
#define LL_RT_SUN_SELF_NEAR 0.8
#endif
// LL_VK_RT_SUN_SELFFADE (fraction of SELFNEAR, default 0.4): where the fade of the floor starts; 1 = no fade (the floor is the same for every close avatar surface: no contours of the head's layers in a shadow)
#ifndef LL_RT_SUN_SELF_FADE
#define LL_RT_SUN_SELF_FADE 0.4
#endif
#define LL_RT_SUN_SELF_NEAR0 (LL_RT_SUN_SELF_NEAR * LL_RT_SUN_SELF_FADE)
// LL_VK_RT_SUN_PENUMBRA (metres, default 0 = off): the width of the penumbra of a close avatar self shadow on the skin. A ray from 5 cm away gives a crisp edge (a copy of the lips on the neck, the fingers on the shoulder),
// where skin scatters light a few millimetres sideways and blurs it. When the first ray of a pixel hit a close skinned surface at distance t, the other rays of the pixel go to a disc of radius PENUMBRA / t (at most 0.35).
#ifndef LL_RT_SUN_PENUMBRA
#define LL_RT_SUN_PENUMBRA 0.0
#endif
// LL_VK_RT_SUN_PENUMBRA_RAYS (default 8, at most 16): the rays of a pixel whose first ray met a close skinned surface. Two rays on a wide disc give a handful of levels per pixel, and a snapshot has no accumulation to
// smooth them: the soft edge is a stack of steps (the lips and the jaw drawn as contours in the shadow on the neck). Only those pixels pay.
#ifndef LL_RT_SUN_PENUMBRA_RAYS
#define LL_RT_SUN_PENUMBRA_RAYS 8
#endif
float rt_near_skin_t = 0.0;                 // out of rtShadowRaySoftS: the distance of the nearest close skinned surface its ray met (0 = none)
// how much of the floor (1 - strength) a close skinned surface at distance t keeps: 1 up to LL_RT_SUN_SELF_NEAR0, fading to 0 at LL_RT_SUN_SELF_NEAR (a plain cut when there is no fade: smoothstep is undefined then)
float rtSelfFloorScale(float t)
{
    if (LL_RT_SUN_SELF_NEAR0 >= LL_RT_SUN_SELF_NEAR)
    {
        return t < LL_RT_SUN_SELF_NEAR ? 1.0 : 0.0;
    }
    return 1.0 - smoothstep(LL_RT_SUN_SELF_NEAR0, LL_RT_SUN_SELF_NEAR, t);
}
// Self shadows of an avatar are translucent: a skinned avatar surface (a hair card, the chin, a strand over the cheek, an arm over the torso) within LL_RT_SUN_SELF_NEAR metres of the pixel it shadows lets
// light through (scattering in hair and skin, bounced light): together they block at most `strength` of the light. Farther away, and everything that is not an avatar, blocks what it covers (the ground shadow
// of an avatar stays dark). Nothing here has to know which object is hair: rigged hair and a mesh head are both just skinned records, and the distance decides.
float rtShadowRaySoftS(vec3 origin, vec3 dir, float tmax, float lod, float strength)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir) || !(tmax > 0.005))
    {
        return 1.0;
    }
    rt_near_skin_t = 0.0;
    float transmit = 1.0;                       // the light that gets past everything but close skinned avatar surfaces
    float skin = 1.0;                           // the light that gets past the close skinned avatar surfaces: their product, but never less than the floor (a cap on the TOTAL, a lock of hair is dozens of cards)
                                                // (the floor 1 - strength fades to 0 between LL_RT_SUN_SELF_NEAR0 and LL_RT_SUN_SELF_NEAR of the NEAREST of those surfaces, rt_near_skin_t: the first occluder decides
                                                // how dark the shadow is and what is behind it adds nothing, a silhouette; the farthest one made fingers behind a hand darken the shadow through the hand)
    const float skin_floor = 1.0 - strength;
    float t_start = 0.005;
    uint mask = 0x0Fu;                          // everything; behind the first close solid avatar surface only the world (0x03)
    bool capped = false;                        // the candidate limit (LL_VK_RT_CANDIDATES, default 96) was reached: the walk stopped before it met everything on the way
    bool none = true;
    // a close solid avatar hit does not end the ray: it counts as covering fully (so the floor applies) and one more ray goes on behind it through the WORLD alone, for a tree or an awning over the
    // avatar. It no longer walks the avatar: a mesh head is several closed shells (the face, the mouth bag, two rows of teeth, the tongue, the eyes) and a walk through them that gave up after four
    // solid hits drew the shells' contours in the chin's shadow on the neck (a darker disc for the mouth, two lines for the teeth). The nearest surface decides the shadow; the rest is a silhouette.
    for (int segment = 0; segment < 2; ++segment)
    {
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsNoneEXT, mask, ray_origin, t_start, ray_dir, tmax);
        uint candidates = 0u;
        while (rayQueryProceedEXT(rq))
        {
            if (++candidates > LL_RT_MAX_CANDIDATES)
            {
                rayQueryTerminateEXT(rq);
                capped = true;
                break;
            }
            if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
                const bool skinned = record != 0u && record < ll_rt_geoms.g[0].first && (ll_rt_geoms.g[record].flags & 8192u) != 0u;
                const float t_cand = rayQueryGetIntersectionTEXT(rq, false);
                const bool close = t_cand < LL_RT_SUN_SELF_NEAR;
                const float cover = rtAlphaCover(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)), rayQueryGetIntersectionBarycentricsEXT(rq, false), lod);
                if (skinned && close)
                {
                    skin *= 1.0 - cover;
                    rt_near_skin_t = (rt_near_skin_t == 0.0) ? t_cand : min(rt_near_skin_t, t_cand);
                }
                else
                {
                    transmit *= 1.0 - cover;
                }
                if (transmit * max(skin, skin_floor * rtSelfFloorScale(rt_near_skin_t)) < 0.03)
                {
                    rayQueryTerminateEXT(rq);
#if LL_RT_SUN_DEBUG == 1
                    return 1.0;             // test: only solid (opaque) hits make a shadow
#else
                    return 0.0;
#endif
                }
            }
        }
        none = rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
        if (none)
        {
            break;
        }
        // a solid hit: a close skinned avatar surface is translucent too (only when there is a floor to apply), anything else blocks
        const float t_hit = rayQueryGetIntersectionTEXT(rq, true);
        const uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
        const bool skinned = record != 0u && record < ll_rt_geoms.g[0].first && (ll_rt_geoms.g[record].flags & 8192u) != 0u;
#if LL_RT_SUN_DEBUG == 0
        if (skin_floor > 0.0 && skinned && t_hit < LL_RT_SUN_SELF_NEAR && segment == 0)
        {
            skin = 0.0;
            rt_near_skin_t = (rt_near_skin_t == 0.0) ? t_hit : min(rt_near_skin_t, t_hit);
            t_start = t_hit + 0.002;
            mask = 0x03u;
            continue;
        }
#endif
        break;
    }
#ifdef LL_RT_CAP_DEBUG
    if (capped && none)
    {
        return 0.0;                         // test (LL_VK_RT_CAPDEBUG=1): a ray that stopped at the limit is drawn fully shadowed
    }
#endif
    const float floor_now = skin_floor * rtSelfFloorScale(rt_near_skin_t);
#if LL_RT_SUN_DEBUG == 1
    return none ? 1.0 : 0.0;                // test (LL_VK_RT_SUN_DEBUG=1): only solid (opaque) hits make a shadow, what the alpha loop found is ignored
#elif LL_RT_SUN_DEBUG == 2
    return none ? transmit * max(skin, floor_now) : 1.0;      // test (=2): only what the alpha loop found makes a shadow, solid hits are ignored
#else
    return none ? transmit * max(skin, floor_now) : 0.0;
#endif
}

// strength: the most light the skinned avatar cards (hair) may block together (1 = all, as before; only masked and blended candidates reach the loop: opaque geometry is a committed hit and blocks everything)
float rtShadowRaySoft(vec3 origin, vec3 dir, float tmax, float lod)
{
    return rtShadowRaySoftS(origin, dir, tmax, lod, 1.0);
}

// what the traced hits call (the sun at a hit and, through rtShadowRayArea, the local lights)
float rtShadowRayHit(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod)
{
    return rtShadowRaySoft(origin, dir, tmax, lod);
}

// Shadow of a local light of the traced hits: the light has a size (a lamp, a projector box), so its shadow edges have a penumbra: 4 soft rays to points of a disc of `radius` metres around the
// light's centre (perpendicular to the way to the light), rotated per pixel. A single ray from a point gave a hard edge that followed the triangles of the occluder (the hair on the shoulder:
// stair steps). rt_hit_shadow_variant 9 (glossyrt=28): the single ray of before. margin = how far short of the target a ray stops (the light sits inside its prim).
float rtShadowRayArea(vec3 origin, vec3 light_center, float margin, float radius, vec2 pos_screen, float lod)
{
    vec3 to_light = light_center - origin;
    float dist = length(to_light);
    if (!(dist > 0.01))
    {
        return 1.0;
    }
    vec3 L = to_light / dist;
    if (rt_hit_shadow_variant == 9)
    {
        return rtShadowRayHit(origin, L, dist - margin, pos_screen, lod);
    }
    const int samples = rt_hit_shadow_samples <= 0 ? 4 : min(rt_hit_shadow_samples, 4);
    if (samples == 1)
    {
        return rtShadowRayHit(origin, L, dist - margin, pos_screen, lod);
    }
    vec3 up = abs(L.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(L, up));
    vec3 b = cross(L, t);
    // interleaved gradient noise of the pixel (a much finer, less grainy pattern than a hash: neighbouring pixels differ evenly), moved on every frame so that the temporal upscaler averages it
    const float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * rt_noise_frame, vec2(0.06711056, 0.00583715))));
    float sum = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        const float a = rot + (samples == 2 ? 3.1415927 : 1.5707963) * float(i);
        const float rr = radius * ((i & 1) == 0 ? 0.45 : 0.9);
        vec3 target = light_center + (t * cos(a) + b * sin(a)) * rr;
        vec3 dv = target - origin;
        float dd = length(dv);
        sum += rtShadowRayHit(origin, dv / dd, max(dd - margin, 0.01), pos_screen, lod);
    }
    return sum / float(samples);
}

// <FS> Ray traced reflections (doc/vulkan/PLAN-rt-materials.md, stage M4): one ray from a surface into the ray scene, the hit shaded from the geometry records and the material table.
// Used first by the water (waterF.glsl, permutation LL_RT_REFLECTIONS). They live here because the scene declarations (the ll_rt_* block, with the function ll_rt_slot) are added to every
// file that mentions ll_rt_tlas, and two files of one program may not both have them.
vec3 rtSrgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// What the surface of a hit looks like: linear albedo, emissive, the (smooth or flat) normal in object space, alpha for the mask test. false: the record cannot be read.
// The same decode as the material debug view (rtMaterialDebugF.glsl dbgSurface), which is the proof that records, uvs, colours and slots are right.
// full: also decode what the lighting of a hit needs (hit shading quality 1): n_shaded = the normal with the normal map applied (object space, a tangent frame from the triangle's uv derivatives),
// mr = (metallic, roughness, 1 for a glTF material else 0, the emissive amount of a legacy material whose diffuse alpha is an emissive mask (else 0)), spec = legacy specular map (linear rgb, alpha = glossiness; alpha 0 = no specular map). Without full they are neutral.
bool rtShadeSurfaceEx(uint record, uint prim, vec2 bary, float lod, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_obj, out vec3 n_shaded, out float alpha, out bool fullbright, out vec4 mr, out vec4 spec)
{
    rt_soft_masked = false;
    mr = vec4(0.0, 1.0, 0.0, 0.0);
    spec = vec4(0.0);
    n_shaded = vec3(0.0, 1.0, 0.0);
    albedo = vec3(0.5);
    emissive = vec3(0.0);
    n_obj = vec3(0.0, 1.0, 0.0);
    alpha = 1.0;
    fullbright = false;
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
    n_obj = normalize(cross(p1 - p0, p2 - p0));
    if ((g.flags & 1024u) != 0u)
    {
        LLRtNormals nb = LLRtNormals(g.normals);
        vec3 ns = nb.n[ix[0]].xyz * w.x + nb.n[ix[1]].xyz * w.y + nb.n[ix[2]].xyz * w.z;
        n_obj = dot(ns, ns) > 1e-8 ? normalize(ns) : n_obj;
    }
    n_shaded = n_obj;
    uint mat_id = g.material;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        return true;                    // no material: a neutral grey
    }
    LLRtMaterial m = LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id];
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
    uint slot = ll_rt_slot((pair >> ((tex_index & 1u) * 16u)) & 0xFFFFu);
    rt_soft_masked = rt_hair_soft != 0 && (mflags & 4u) != 0u && (g.flags & 8192u) != 0u;
    vec4 tex = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(slot))], uv, rt_soft_masked && rt_hair_soft == 3 ? lod + 1.5 : lod);
    vec4 vcol = vec4(1.0);
    if ((g.flags & 16u) != 0u)
    {
        LLRtColors col = LLRtColors(g.colors);
        vcol = (unpackUnorm4x8(col.c[ix[0]]) * w.x + unpackUnorm4x8(col.c[ix[1]]) * w.y + unpackUnorm4x8(col.c[ix[2]]) * w.z);
    }
    if ((mflags & 64u) != 0u)
    {
        vcol.a = 1.0;                   // the legacy materials passes test the texture alpha alone
    }
    alpha = tex.a * vcol.a * m.color.a;
    if ((mflags & 4u) != 0u)
    {
        if (rt_soft_masked)
        {
            alpha = smoothstep(m.emissive.w - 0.2, m.emissive.w + 0.2, alpha);       // hair taken softly: the opacity near the cutoff, not all or nothing
        }
        else
        {
            alpha = (alpha >= m.emissive.w) ? 1.0 : 0.0;        // alpha masked: the draw's own cutoff
        }
    }
    if ((mflags & 2u) != 0u)
    {
        albedo = rtSrgbToLinear(tex.rgb) * rtSrgbToLinear(vcol.rgb) * m.color.rgb;     // glTF: the factor is linear
        vec3 emissive_tex = vec3(1.0);
        uint emissive_pin = m.slots2.y & 0xFFFFu;
        if (emissive_pin != 0u)
        {
            uint emissive_slot = ll_rt_slot(emissive_pin);
            if (emissive_slot != 0u)
            {
                emissive_tex = rtSrgbToLinear(textureLod(ll_rt_tex[nonuniformEXT(LL_HM(emissive_slot))], uv, lod).rgb);
            }
        }
        emissive = m.emissive.xyz * emissive_tex;
    }
    else
    {
        albedo = rtSrgbToLinear(tex.rgb * vcol.rgb * m.color.rgb);                     // legacy: texture and vertex colour are sRGB values
    }
    fullbright = (mflags & 1u) != 0u;
    if (full && !fullbright)
    {
        if ((mflags & 8u) != 0u && (mflags & 2u) == 0u)
        {
            mr.w = -1.0;                            // a legacy material that is alpha BLENDED (hair, some clothes): the viewer lights it in the forward alpha pass, not in the soften pass (softenLightF.glsl rtHitForwardLights)
        }
        else if ((mflags & 32u) != 0u && (mflags & 2u) == 0u)
        {
            mr.w = clamp(tex.a, 0.0, 1.0);          // DIFFUSE_ALPHA_MODE_EMISSIVE: the stock lighting blends the lit colour towards the albedo by this (softenLightF.glsl: mix(color, baseColor, baseColor.a))
        }
        // the normal map through a tangent frame (the same decode as the material debug view, rtMaterialDebugF.glsl); its alpha is the glossiness mask of a legacy material
        float normal_alpha = 1.0;
        uint normal_pin = m.slots2.x & 0xFFFFu;
        if (normal_pin != 0u && (g.flags & 8u) != 0u)
        {
            uint normal_slot = ll_rt_slot(normal_pin);
            if (normal_slot != 0u && (g.flags & 8192u) != 0u)
            {
                // a skinned avatar draw (kRecSkinned): the tangent frame below is made per triangle and the avatar's normals are smooth (skinned by the compute pass): the frame jumps at every triangle edge
                // and roughens skin, hair edges and hands. Only the alpha of the map (the legacy glossiness mask) is read.
                normal_alpha = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(normal_slot))], uv, lod).a;
            }
            else if (normal_slot != 0u)
            {
                LLRtTexcoords tcn = LLRtTexcoords(g.texcoords);
                vec2 a0 = tcn.t[ix[0]], a1 = tcn.t[ix[1]], a2 = tcn.t[ix[2]];
                a0 = m.uv0 * a0.x + m.uv1 * a0.y + m.uv2;
                a1 = m.uv0 * a1.x + m.uv1 * a1.y + m.uv2;
                a2 = m.uv0 * a2.x + m.uv1 * a2.y + m.uv2;
                vec3 e1 = p1 - p0, e2 = p2 - p0;
                vec2 d1 = a1 - a0, d2 = a2 - a0;
                float det = d1.x * d2.y - d2.x * d1.y;
                if (abs(det) > 1e-12)
                {
                    vec3 tangent = (e1 * d2.y - e2 * d1.y) / det;
                    vec3 bitangent = (e2 * d1.x - e1 * d2.x) / det;
                    tangent = tangent - n_obj * dot(n_obj, tangent);
                    if (dot(tangent, tangent) > 1e-12)
                    {
                        tangent = normalize(tangent);
                        vec3 bit = cross(n_obj, tangent);
                        bit *= (dot(bit, bitangent) < 0.0) ? -1.0 : 1.0;       // the handedness of the uv layout
                        vec4 nmt = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(normal_slot))], uv, lod);
                        vec3 nm = nmt.xyz * 2.0 - 1.0;
                        normal_alpha = nmt.a;
                        n_shaded = normalize(tangent * nm.x + bit * nm.y + n_obj * nm.z);
                    }
                }
            }
        }
        // roughness and metallic: glTF = factors times the metallic-roughness map (green, blue); legacy = the specular map (rgb colour, alpha glossiness), none = matte
        uint mr_pin = m.slots2.x >> 16;
        uint mr_slot = mr_pin != 0u ? ll_rt_slot(mr_pin) : 0u;
        if ((mflags & 2u) != 0u)
        {
            float metallic = m.params.x, rough = m.params.y;
            if (mr_slot != 0u)
            {
                vec3 t = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(mr_slot))], uv, lod).rgb;
                rough *= t.g;
                metallic *= t.b;
            }
            mr = vec4(clamp(metallic, 0.0, 1.0), clamp(rough, 0.0, 1.0), 1.0, 0.0);
        }
        else if (mr_slot != 0u)
        {
            // like the G-buffer pass of legacy materials (materialF.glsl): colour = specular map x the material's specular colour (m.emissive.xyz), glossiness = the exponent / 255 (m.params.w) x normal map alpha
            vec4 sm = textureLod(ll_rt_tex[nonuniformEXT(LL_HM(mr_slot))], uv, lod);
            spec = vec4(rtSrgbToLinear(sm.rgb * m.emissive.xyz), m.params.w * normal_alpha);
        }
    }
    return true;
}

bool rtShadeSurface(uint record, uint prim, vec2 bary, float lod, out vec3 albedo, out vec3 emissive, out vec3 n_obj, out float alpha, out bool fullbright)
{
    vec3 n_shaded;
    vec4 mr, spec;
    return rtShadeSurfaceEx(record, prim, bary, lod, false, albedo, emissive, n_obj, n_shaded, alpha, fullbright, mr, spec);
}

// Is the record's material a legacy material that is alpha BLENDED (hair, some clothes)? The main view draws those in the forward alpha pass over the opaque layers behind them.
bool rtRecordBlended(uint record)
{
    if (record == 0u || record >= ll_rt_geoms.g[0].first)
    {
        return false;
    }
    uint mat_id = ll_rt_geoms.g[record].material;
    if (mat_id == 0u || mat_id >= 16384u)
    {
        return false;
    }
    uint mf = floatBitsToUint(LLRtMaterials(ll_rt_geoms.g[0].normals).m[mat_id].params.z);
    return (mf & 8u) != 0u && (mf & 2u) == 0u;
}

// Decode a hit that is known by record, triangle, barycentrics and distance (a committed intersection, or a blended candidate kept aside): the surface, the eye space normals facing the ray,
// the sun ray from the hit. w2o = the instance's world to object matrix of the intersection.
bool rtResolveHit(uint record, uint prim, vec2 bary, mat4x3 w2o, float t, vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, mat3 to_scene,
                  out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float shadow, out bool fullbright, out vec4 mr, out vec4 spec, out float alpha)
{
    n_geo_eye = vec3(0.0, 0.0, 1.0);
    n_eye = n_geo_eye;
    hit_pos = origin;
    shadow = 0.0;
    float lod = rt_hit_shadow_variant == 4 ? 0.0 : clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0);
    vec3 n_obj, n_shaded;
    if (!rtShadeSurfaceEx(record, prim, bary, lod, full, albedo, emissive, n_obj, n_shaded, alpha, fullbright, mr, spec))
    {
        return false;                   // a record that cannot be read: not a hit
    }
    n_geo_eye = normalize(normalize(n_obj * mat3(w2o)) * to_scene);
    n_eye = full ? normalize(normalize(n_shaded * mat3(w2o)) * to_scene) : n_geo_eye;
    if (dot(n_geo_eye, dir) > 0.0)
    {
        n_geo_eye = -n_geo_eye;         // two sided
        n_eye = -n_eye;
    }
    hit_pos = origin + dir * t;
    if (!fullbright)
    {
        float nl = clamp(dot(n_geo_eye, sun_dir), 0.0, 1.0);
        if (nl > 0.0)
        {
            if (rt_hit_shadow_variant == 1)
            {
                shadow = 1.0;
            }
            else
            {
                shadow = rtShadowRayHit(hit_pos + n_geo_eye * ((rt_hit_shadow_variant == 2 ? 0.15 : 0.02) + 0.0006 * length(hit_pos)), sun_dir, 2000.0, pos_screen, lod);
            }
        }
    }
    return true;
}

// The trace and the decode of the hit, without lighting it: everything the lighting of the soften pass needs (hit quality 1, softenLightF.glsl) or the flat shading below.
// full: decode normal map, roughness / metallic / specular map as well. n_geo_eye = the (smooth) normal of the geometry, n_eye = the shaded one (normal map), both eye space, facing the ray.
// shadow = the sun ray towards sun_dir from the hit (0 or 1, 0 when the geometry faces away), mr / spec as rtShadeSurfaceEx. Returns false for a miss or a record that cannot be read.
// layers: alpha BLENDED surfaces (hair) never stop the ray, like they do not in the main view where the alpha pass draws them over the opaque scene: the nearest one in front of the hit comes back
// as l_* (layer_alpha its opacity, 0 = none), to be shaded and composited over the hit by the caller. A blended surface with nothing behind it is the hit itself, layer_alpha = -its opacity.
bool rtTraceHitEx(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, bool layers, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                  out float shadow, out bool fullbright, out vec4 mr, out vec4 spec,
                  out float layer_alpha, out vec3 l_albedo, out vec3 l_emissive, out vec3 l_n_geo, out vec3 l_n, out vec3 l_hp, out float l_t, out float l_shadow, out bool l_fb, out vec4 l_mr, out vec4 l_spec)
{
    albedo = vec3(0.0);
    emissive = vec3(0.0);
    n_geo_eye = vec3(0.0, 0.0, 1.0);
    n_eye = n_geo_eye;
    hit_pos = origin;
    t = 0.0;
    shadow = 0.0;
    fullbright = false;
    mr = vec4(0.0, 1.0, 0.0, 0.0);
    spec = vec4(0.0);
    layer_alpha = 0.0;
    l_albedo = vec3(0.0);
    l_emissive = vec3(0.0);
    l_n_geo = n_geo_eye;
    l_n = n_geo_eye;
    l_hp = origin;
    l_t = 0.0;
    l_shadow = 0.0;
    l_fb = false;
    l_mr = mr;
    l_spec = spec;
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    float tmax = 1500.0;
    const vec3 ray_origin = to_scene * origin + camera;
    const vec3 ray_dir = to_scene * dir;
    if (!rtRayOk(ray_origin, ray_dir))
    {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, 0, 0x0F, ray_origin, 0.02, ray_dir, tmax);
    float lod_start = rt_hit_shadow_variant == 4 ? 0.0 : clamp(0.5 * log2(max(length(origin), 1.0)) - 1.0, 0.0, 4.0);
    uint steps = 0u;
    // the nearest blended candidate seen so far (its place in the scene, kept until the ray is done)
    float c_t = 1e30, c_alpha = 0.0;
    float transmit = 1.0;                       // hair taken softly (rt_hair_soft 2, 3): what is left of the light after every such card the ray crossed
    float seen = 0.0, seen_alpha = 0.0;         // debug: how many blended candidates the ray met and the highest opacity among them
    uint c_record = 0u, c_prim = 0u;
    vec2 c_bary = vec2(0.0);
    mat4x3 c_w2o = mat4x3(1.0);
    while (rayQueryProceedEXT(rq))
    {
        if (++steps > (rt_hit_shadow_variant == 3 ? 1024u : 64u))
        {
            rayQueryTerminateEXT(rq);
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            // a masked structure: its own alpha decides
            uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
            uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
            vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            vec3 a, e, n;
            float alpha;
            bool fb;
            if (rtShadeSurface(record, prim, bary, lod_start, a, e, n, alpha, fb))
            {
                if (layers && (rtRecordBlended(record) || rt_soft_masked))
                {
                    seen += 1.0;
                    seen_alpha = max(seen_alpha, alpha);
                    float ct = rayQueryGetIntersectionTEXT(rq, false);
                    if (alpha >= 0.03 && rt_soft_masked && rt_hair_soft >= 2)
                    {
                        transmit *= 1.0 - min(alpha, 1.0);
                    }
                    if (alpha >= 0.03 && ct < c_t)
                    {
                        c_t = ct;
                        c_alpha = min(alpha, 1.0);
                        c_record = record;
                        c_prim = prim;
                        c_bary = bary;
                        c_w2o = rayQueryGetIntersectionWorldToObjectEXT(rq, false);
                    }
                }
                else if (alpha >= 0.5)
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
    if (rt_hair_soft >= 2 && c_record != 0u)
    {
        c_alpha = max(c_alpha, 1.0 - transmit);       // the combined opacity of the hair cards the ray crossed
    }
    const bool have_main = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    l_t = seen;                          // debug (rt_hit_quality 14): without a layer these two tell what the ray saw of blended surfaces
    l_shadow = seen_alpha;
    if (!have_main && c_record == 0u)
    {
        return false;
    }
    float alpha;
    if (have_main)
    {
        t = rayQueryGetIntersectionTEXT(rq, true);
        uint record = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)) + uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
        if (!rtResolveHit(record, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)), rayQueryGetIntersectionBarycentricsEXT(rq, true), rayQueryGetIntersectionWorldToObjectEXT(rq, true),
                          t, origin, dir, sun_dir, pos_screen, full, to_scene, albedo, emissive, n_geo_eye, n_eye, hit_pos, shadow, fullbright, mr, spec, alpha))
        {
            return false;
        }
        if (c_record != 0u && c_t < t)
        {
            l_t = c_t;
            float la;
            if (rtResolveHit(c_record, c_prim, c_bary, c_w2o, c_t, origin, dir, sun_dir, pos_screen, full, to_scene, l_albedo, l_emissive, l_n_geo, l_n, l_hp, l_shadow, l_fb, l_mr, l_spec, la))
            {
                layer_alpha = c_alpha;
            }
        }
        return true;
    }
    // only a blended surface on the way: it is the hit, partly transparent over whatever the caller has behind it
    t = c_t;
    if (!rtResolveHit(c_record, c_prim, c_bary, c_w2o, c_t, origin, dir, sun_dir, pos_screen, full, to_scene, albedo, emissive, n_geo_eye, n_eye, hit_pos, shadow, fullbright, mr, spec, alpha))
    {
        return false;
    }
    layer_alpha = -c_alpha;
    return true;
}

bool rtTraceHit(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                out float shadow, out bool fullbright, out vec4 mr, out vec4 spec)
{
    float la, lt, ls;
    vec3 l_albedo, l_emissive, l_n_geo, l_n, l_hp;
    bool l_fb;
    vec4 l_mr, l_spec;
    return rtTraceHitEx(origin, dir, sun_dir, pos_screen, full, false, albedo, emissive, n_geo_eye, n_eye, hit_pos, t, shadow, fullbright, mr, spec,
                        la, l_albedo, l_emissive, l_n_geo, l_n, l_hp, lt, ls, l_fb, l_mr, l_spec);
}

// A ray from origin (eye space) along dir (unit, eye space) against the scene. The hit is lit like a legacy surface of the deferred pass: (ambient + sun * N.L * shadow) * albedo + emissive,
// the shadow from a ray towards the sun; fullbright surfaces show their albedo. Hits far away fade into fallback (the probe / sky colour already known for that direction).
// Returns rgb and a = 1 for a hit, a = 0 for a miss (the caller keeps what it has). This is the flat shading of the water and of the glossy mirror at hit quality 0.
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback)
{
    vec3 albedo, emissive, n_geo_eye, n_eye, hit_pos;
    float t, shadow;
    bool fullbright;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, sun_dir, pos_screen, false, albedo, emissive, n_geo_eye, n_eye, hit_pos, t, shadow, fullbright, mr, spec))
    {
        return vec4(0.0);
    }
    vec3 color;
    if (fullbright)
    {
        color = albedo + emissive;
    }
    else
    {
        float nl = clamp(dot(n_geo_eye, sun_dir), 0.0, 1.0);
        color = (amblit_linear + nl * shadow * sunlit_linear) * albedo + emissive;
    }
    float fade = smoothstep(250.0, 1200.0, t);
    return vec4(mix(color, fallback, fade), 1.0);
}

// <FS> test switches (LL_VK_RT_SUN_BIAS metres, default 0.003: how far off the surface along the normal the ray starts. It was 0.015 until 2026-10-27: anything closer to a surface than that could not shadow
// it, so the shadow of a strand of hair gave out where the strand came close to the face; 0.003 is what the user found right by eye. LL_VK_RT_SUN_NOFACE=1: a surface facing away from the light is traced too)
#ifndef LL_RT_SUN_BIAS
#define LL_RT_SUN_BIAS 0.003
#endif
// LL_VK_RT_SUN_TMIN (metres, default 0 = off): the sun ray starts this far along the light direction: whatever is closer to the surface than that along the light cannot shadow it, while a shadow from farther up
// (a chin on the chest) keeps its whole shape. The first answer to the hard streaks of hair on skin (0.04), not needed since the soft alpha ray and the small start offset (below) give hair a continuous shadow.
// LL_VK_RT_SUN_BIAS_ALPHA (metres, default 0.015): the same for blended surfaces (hair cards a few millimetres apart shade each other, grey strands in the hair, when it is as small as the one of skin)
#ifndef LL_RT_SUN_BIAS_ALPHA
#define LL_RT_SUN_BIAS_ALPHA 0.015
#endif
// LL_VK_RT_SUN_ALPHA_STRENGTH (0..1, default 0.6; 1 = an avatar blocks its own light fully, the look before 2026-10-28): the most light the close surfaces of an avatar may block together on that same
// avatar (rtShadowRaySoftS: hair on the face, a hand on the shoulder, the chin on the chest). Hair and skin are translucent and bounce light, so a self shadow is never as dark as an opaque object's.
// 0.6 is the user's choice by eye (2026-10-28, with SELFNEAR 0.8, the receiver probe and the 4 mm start disc).
// LL_VK_RT_SUN_HAIRFILL (0..1, default 0): the shadow of a blended surface (the hair itself) is lifted toward lit by this fraction: bounced light and scattering fill the shadows hair has on itself.
#ifndef LL_RT_SUN_ALPHA_STRENGTH
#define LL_RT_SUN_ALPHA_STRENGTH 0.6
#endif
#ifndef LL_RT_SUN_HAIRFILL
#define LL_RT_SUN_HAIRFILL 0.0
#endif
#ifndef LL_RT_SUN_DEBUG
#define LL_RT_SUN_DEBUG 0
#endif
#ifndef LL_RT_SUN_TMIN
#define LL_RT_SUN_TMIN 0.0
#endif
// LL_VK_RT_SUN_SOFTALPHA (metres, default 12, 0 = off): up to this view distance the sun ray lets each masked or blended card (hair) through by the fraction its blurred alpha leaves (rtShadowRaySoft) instead of
// the yes/no alpha test of rtShadowRay: a lock of hair then casts a continuous, partly transparent shadow, not one that is a full shadow or nothing per pixel (gaps, stair steps along the cards)
#ifndef LL_RT_SUN_SOFTALPHA
#define LL_RT_SUN_SOFTALPHA 12.0
#endif
float rtSunRayAt(vec3 origin, vec3 dir, vec2 pos_screen, float lod, float view_dist, float strength)
{
    if (view_dist < LL_RT_SUN_SOFTALPHA)
    {
        return rtShadowRaySoftS(origin, dir, 2000.0, lod, strength);
    }
    return rtSunRay(origin, dir, pos_screen, lod);
}

// LL_VK_RT_SUN_SELFRECV (metres, default 0.02, 0 = off): the translucent self shadow is for an avatar surface shadowing the SAME avatar (hair on the face, the chin on the chest). The ground at the feet is
// within SELFNEAR of the legs too, and its shadow faded out toward the feet (the user's snapshot, 2026-10-28 01:31). The G-buffer does not say what a pixel is, the ray scene does: a probe through the
// receiver's own surface (from this far under it to this far over it, along its normal) into the avatar structures alone (mask 0x0C) hits when the receiver is avatar geometry. Only a pixel whose first
// ray met a close avatar surface pays the probe; a receiver that is not an avatar traces that ray again at full strength. (Verified 02:13 with a stripes view: hits the whole avatar, hair included, nothing else.)
#ifndef LL_RT_SUN_SELF_RECV
#define LL_RT_SUN_SELF_RECV 0.02
#endif
bool rtReceiverIsAvatar(vec3 pos, vec3 norm)
{
    mat3 to_scene;
    vec3 camera;
    rtSceneFrame(to_scene, camera);
    const vec3 ray_origin = to_scene * (pos - norm * LL_RT_SUN_SELF_RECV) + camera;
    const vec3 ray_dir = to_scene * norm;
    if (!rtRayOk(ray_origin, ray_dir))
    {
        return false;
    }
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ll_rt_tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0x0C, ray_origin, 0.0, ray_dir, 2.0 * LL_RT_SUN_SELF_RECV);
    uint steps = 0u;
    while (rayQueryProceedEXT(rq))
    {
        if (++steps > 16u)
        {
            rayQueryTerminateEXT(rq);
            break;
        }
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// LL_VK_RT_SUN_SSS (metres, default 0.004, 0 = off; the user chose 4 mm by eye, 2026-10-28): each ray of a pixel starts at a random point of a disc of this radius on the receiver's surface (the tangent plane), not at the pixel itself. Skin scatters
// the light it receives a few millimetres sideways, so the shadow a face shows is the shadow averaged over that much of its surface: a soft edge on BOTH sides of the caster's silhouette. A wider sun disc
// (LL_RT_SUN_PENUMBRA) could only widen the rays of a pixel whose first ray had already met a caster, so the outside of every edge stayed crisp. A few millimetres on the ground is nothing next to the
// sun's own penumbra, so it applies to every receiver. No extra rays; the accumulation smooths the noise of the random start.
#ifndef LL_RT_SUN_SSS
#define LL_RT_SUN_SSS 0.004
#endif

float rtSunShadowCore(vec3 pos, vec3 norm, vec3 light_dir, vec2 pos_screen)
{
#ifndef LL_RT_SUN_NOFACE
    if (dot(norm, light_dir) <= 0.0)
    {
        return 0.0;                         // facing away from the light
    }
#endif
    if (-pos.z > shadow_clip.w)
    {
        return 1.0;                         // beyond the shadow range: lit, as with the cascades
    }
    if (rt_sun_maxdist > 0.0 && -pos.z > rt_sun_maxdist)
    {
        return 1.0;                         // <FS> a blended surface this far away: the shadow ray is not worth its cost (a few pixels of hair, glass or foliage), the opaque ground behind it keeps its shadow
    }
    vec3 origin = pos + norm * ((rt_sun_alpha > 0.5 ? LL_RT_SUN_BIAS_ALPHA : LL_RT_SUN_BIAS) + 0.0006 * -pos.z) + light_dir * LL_RT_SUN_TMIN;
    float lod = clamp(0.5 * log2(max(-pos.z, 1.0)) - 1.0, 0.0, 4.0);
    int samples = (rt_sun_radius > 0.0) ? clamp(rt_sun_samples, 1, 16) : 1;
    if (rt_sun_lod.x > 0.0 && samples > 4)
    {
        // <FS> far from the camera the penumbra is a fraction of a pixel and the temporal upscaler averages the noise: fewer rays (the alpha pass traces for every blended pixel, in a crowd that is hair, clothes and
        // glass of dozens of avatars and cost over 5 ms of GPU with 12 rays)
        float k = smoothstep(rt_sun_lod.x, max(rt_sun_lod.y, rt_sun_lod.x + 1.0), -pos.z);
        samples = clamp(int(mix(float(samples), float(max(samples / 4, 2)), k) + 0.5), 1, 16);
    }
    // a per pixel rotation (interleaved gradient noise, shifted by the frame when it changes every frame), used by the sun's disc and by the start disc on the receiver
    vec2 px = gl_FragCoord.xy + vec2(5.588238, 5.588238) * rt_noise_frame;
    float rot = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    // a frame of tangents on the receiver for the start disc (LL_RT_SUN_SSS)
    vec3 b1 = normalize(cross(norm, abs(norm.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b2 = cross(norm, b1);
    const bool self_on = rt_sun_self_off < 0.5;
    float strength = self_on ? LL_RT_SUN_ALPHA_STRENGTH : 1.0;  // this pixel's: 1 (avatar surfaces block fully) once the probe says the receiver is not an avatar
    if (samples == 1 && rt_sun_radius <= 0.0)
    {
        vec3 o = origin;
        if (LL_RT_SUN_SSS > 0.0 && self_on)
        {
            o += (b1 * cos(rot) + b2 * sin(rot)) * (LL_RT_SUN_SSS * 0.7);
        }
        float one = rtSunRayAt(o, light_dir, pos_screen, lod, -pos.z, strength);
        if (strength < 1.0 && rt_near_skin_t > 0.0 && LL_RT_SUN_SELF_RECV > 0.0 && !rtReceiverIsAvatar(pos, norm))
        {
            one = rtSunRayAt(o, light_dir, pos_screen, lod, -pos.z, 1.0);
        }
        return one;
    }
    // a frame of tangents for the sun's disc and a golden angle spiral
    vec3 t1 = normalize(cross(light_dir, abs(light_dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 t2 = cross(light_dir, t1);
    float lit = 0.0;
    float skin_t = 0.0;                         // the distance of the close avatar surface the first ray met (0 = none): the other rays of the pixel get a wider disc (LL_RT_SUN_PENUMBRA)
    for (int i = 0; i < samples; ++i)
    {
        float radius = rt_sun_radius;
        if (i > 0 && skin_t > 0.0 && LL_RT_SUN_PENUMBRA > 0.0)
        {
            radius = clamp(LL_RT_SUN_PENUMBRA / skin_t, rt_sun_radius, 0.35);
        }
        float r = sqrt((float(i) + 0.5) / float(samples)) * radius;
        float phi = rot + float(i) * 2.3999632;
        vec3 dir = normalize(light_dir + (t1 * cos(phi) + t2 * sin(phi)) * r);
        vec3 o = origin;
        if (LL_RT_SUN_SSS > 0.0 && self_on)
        {
            // the start disc: the same spiral as the sun's disc, turned so the two do not line up
            float rs = sqrt((float(i) + 0.5) / float(samples)) * LL_RT_SUN_SSS;
            float ps = rot + 1.0 + float(i) * 2.3999632;
            o += (b1 * cos(ps) + b2 * sin(ps)) * rs;
        }
        float one = rtSunRayAt(o, dir, pos_screen, lod, -pos.z, strength);
        if (i == 0)
        {
            if (strength < 1.0 && rt_near_skin_t > 0.0 && LL_RT_SUN_SELF_RECV > 0.0 && !rtReceiverIsAvatar(pos, norm))
            {
                strength = 1.0;
                one = rtSunRayAt(o, dir, pos_screen, lod, -pos.z, strength);
            }
            skin_t = rt_near_skin_t;
            if (skin_t > 0.0 && LL_RT_SUN_PENUMBRA > 0.0)
            {
                samples = clamp(max(samples, LL_RT_SUN_PENUMBRA_RAYS), 1, 16);
            }
        }
        lit += one;
    }
    return lit / float(samples);
}

float rtSunShadow(vec3 pos, vec3 norm, vec3 light_dir, vec2 pos_screen)
{
    float lit = rtSunShadowCore(pos, norm, light_dir, pos_screen);
    if (rt_sun_alpha > 0.5)
    {
        lit = mix(lit, 1.0, LL_RT_SUN_HAIRFILL);        // <FS> a blended surface (hair): bounced light and scattering fill part of the shadows it has on itself
    }
    return lit;
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
    mat3 x_to_scene;
    vec3 x_camera;
    rtSceneFrame(x_to_scene, x_camera);
    rayQueryEXT xq;
    rayQueryInitializeEXT(xq, ll_rt_tlas, gl_RayFlagsOpaqueEXT, 0x04, x_camera, 0.01, x_to_scene * (pos / depth), depth + 3.0);
    uint x_steps = 0u;
    while (rayQueryProceedEXT(xq))
    {
        if (++x_steps > 96u)
        {
            rayQueryTerminateEXT(xq);
            break;
        }
    }
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

