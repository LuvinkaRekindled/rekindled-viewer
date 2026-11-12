/**
 * @file class3/deferred/rtReflectionF.glsl
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

/*[EXTRA_CODE_HERE]*/

#define FLT_MAX 3.402823466e+38

// <FS> the RT Reflection pass (doc/vulkan/PLAN-rt-reflection-pass.md): the traced glossy reflection, carved out of softenLightF.glsl so the lighting pass compiles without the ray queries.
// Two outputs: 0 = the reflection (rgb) and how much of it replaces the probe result (a; -1 = the ray was traced and found nothing, 0 = not traced), 1 = the hit distance for ray reconstruction (R).
out vec4 frag_data[2];

const float M_PI = 3.14159265;

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
uniform sampler2D lightMap;
#endif

uniform sampler2D     lightFunc;

uniform float blur_size;
uniform float blur_fidelity;

#if defined(HAS_SSAO)
uniform float ssao_irradiance_scale;
uniform float ssao_irradiance_max;
#endif

// Inputs
uniform vec4 clipPlane;
uniform mat3 env_mat;
uniform mat3  ssao_effect_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int  sun_up_factor;
uniform int classic_mode;

in vec2 vary_fragcoord;

uniform mat4 inv_proj;
uniform vec2 screen_res;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec3  atmosFragLightingLinear(vec3 l, vec3 additive, vec3 atten);
vec3  scaleSoftClipFragLinear(vec3 l);

// reflection probe interface
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
    vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
float getDepth(vec2 pos_screen);

vec3 linear_to_srgb(vec3 c);
vec3 srgb_to_linear(vec3 c);

uniform vec4 waterPlane;

uniform int cube_snapshot;

uniform float sky_hdr_scale;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten);

GBufferInfo getGBuffer(vec2 screenpos);
vec2 BRDF(float NoV, float roughness);       // probe (modes 10, 11)

#if defined(HAS_SUN_SHADOW) && defined(LL_RT_REFLECTIONS)
#define SOFTEN_RT_REFLECTION 1
uniform int rt_glossy_reflections;
uniform float rr_hit_distance;  // 1: the alpha of the output carries the traced reflection's hit distance (ray reconstruction is on)
uniform int rt_hit_quality;     // how a traced hit is lit: 0 flat diffuse (rtReflection), 1 the lighting of the main pass at the hit (normal map, roughness / metallic / specular map, probes)
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback);
bool rtTraceHit(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                out float shadow, out bool fullbright, out vec4 mr, out vec4 spec);
bool rtTraceHitEx(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, bool layers, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                  out float shadow, out bool fullbright, out vec4 mr, out vec4 spec,
                  out float layer_alpha, out vec3 l_albedo, out vec3 l_emissive, out vec3 l_n_geo, out vec3 l_n, out vec3 l_hp, out float l_t, out float l_shadow, out bool l_fb, out vec4 l_mr, out vec4 l_spec,
                  out float m_alpha, out vec3 m_albedo, out vec3 m_emissive, out vec3 m_n_geo, out vec3 m_n, out vec3 m_hp, out float m_t, out float m_shadow, out bool m_fb, out vec4 m_mr, out vec4 m_spec);
void preProbeSample(vec3 pos);
void probeListSave();
void probeListReset();
void probeListRestore();
vec3 sampleProbes(vec3 pos, vec3 dir, float lod);
vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit);
uniform float max_probe_lod;

// local lights at a hit (rt_hit_quality 2): the nearest point lights as arrays (eye space centre and size, linear colour and falloff) and the first projector (spot light) through the
// uniforms its own shader uses (proj_mat, proj_n, proj_origin, proj_lod ... set by LLPipeline::setupSpotLight, the projected texture is projectionMap, colour / size / falloff of the light)
const int RT_HIT_LIGHTS = 8;
const float RT_LIGHT_SIZE = 0.5;        // metres: the radius of the disc the soft shadow rays of a local light are aimed at (rtShadowRayArea)
uniform int rt_hit_light_count;
uniform vec4 rt_hit_light_pos[8];
uniform vec4 rt_hit_light_col[8];
uniform vec4 rt_hit_light_dir[8];        // xyz: the axis a spot light looks along (eye space), w: 1 for a spot light
uniform int rt_hit_spot;
uniform vec3 rt_hit_spot_center;
uniform vec3 proj_origin;
uniform float proj_ambiance;
uniform float proj_lod;
uniform float proj_focus;
uniform float proj_range;
uniform vec3 color;
uniform float falloff;
float rtShadowRay(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod);
float rtShadowRayHit(vec3 origin, vec3 dir, float tmax, vec2 pos_screen, float lod);
float rtShadowRayArea(vec3 origin, vec3 light_center, float margin, float radius, vec2 pos_screen, float lod);
bool clipProjectedLightVars(vec3 light_center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv);
vec3 getProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv);
float calcLegacyDistanceAttenuation(float distance, float falloff);
void pbrPunctual(vec3 diffuseColor, vec3 specularColor, float perceptualRoughness, float metallic, vec3 n, vec3 v, vec3 l, out float nl, out vec3 diff, out vec3 spec);
#endif
vec3 clampHDRRange(vec3 color);

void adjustIrradiance(inout vec3 irradiance, float ambocc)
{
    // use sky settings ambient or irradiance map sample, whichever is brighter
    //irradiance = max(amblit_linear, irradiance);

#if defined(HAS_SSAO)
    irradiance = mix(ssao_effect_mat * min(irradiance.rgb*ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance.rgb, ambocc);
#endif
}

#ifdef SOFTEN_RT_REFLECTION
// What one local light adds to a hit, from the same formulas as pointLightF / spotLightF: radiance = the light's colour x its distance attenuation x its shadow (x the projected texture for a
// projector), L = unit vector from the hit to the light. pbr: pbrPunctual with the 3.25 balance factor, legacy: lambert on the albedo plus the blinn-phong highlight of the specular map.
struct RtHitSurface    // the parts of the hit the lights need (same unit only: a plain struct of the soften pass)
{
    bool pbr;
    vec3 diffuseColor;
    vec3 specularColor;
    float roughness;
    float metallic;
    vec3 albedo;
    vec4 spec;
};

vec3 rtLightOnHit(vec3 radiance, vec3 L, vec3 n, vec3 v, RtHitSurface s)
{
    if (s.pbr)
    {
        float nl = 0.0;
        vec3 diffPunc = vec3(0.0);
        vec3 specPunc = vec3(0.0);
        pbrPunctual(s.diffuseColor, s.specularColor, s.roughness, s.metallic, n, v, L, nl, diffPunc, specPunc);
        return radiance * 3.25 * clamp(nl * (diffPunc + specPunc), vec3(0.0), vec3(10.0));
    }
    vec3 h, l;
    float nh, nl, nv, vh, lightDist;
    calcHalfVectors(L, n, v, h, l, nh, nl, nv, vh, lightDist);
    vec3 c = radiance * nl * s.albedo;
    if (s.spec.a > 0.0 && nh > 0.0)
    {
        float lit = min(nl * 6.0, 1.0);
        float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
        float gtdenom = 2.0 * nh;
        float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
        float scol = fres * textureLod(lightFunc, vec2(nh, s.spec.a), 0.0).r * gt / (nh * nl);
        c += clamp(radiance * lit * scol * s.spec.rgb, vec3(0.0), vec3(1.0));
    }
    return c;
}

// the ray to a light stops short of the light's centre: the light sits inside a prim (a lamp, the projector box), whose own surface would block the ray; the main pass does not shadow lights at all.
// rt_hit_quality 4 (and 5 below) skip the shadow rays: a test of that
bool rt_lights_noshadow = false;
vec3 rt_dbg_point = vec3(0.0);       // the debug view (rt_hit_quality 3 / 4): what the point lights and the projector added, and how far the projector got
vec3 rt_dbg_spot = vec3(0.0);
float rt_dbg_status = 0.0;
float rtLightMargin(float d)
{
    return max(0.3, 0.1 * d);
}

vec3 rtHitLocalLights(vec3 hp, vec3 n_geo, vec3 n, vec3 v, float lod, vec2 tc, RtHitSurface s)
{
    vec3 sum = vec3(0.0);
    vec3 base = hp + n_geo * (0.02 + 0.0006 * length(hp));
    for (int i = 0; i < RT_HIT_LIGHTS; ++i)
    {
        if (i >= rt_hit_light_count)
        {
            break;
        }
        vec3 lv = rt_hit_light_pos[i].xyz - hp;
        float d = length(lv);
        float sz = rt_hit_light_pos[i].w;
        if (d >= sz || d < 0.001)
        {
            continue;
        }
        vec3 L = lv / d;
        if (dot(n, L) <= 0.0 && dot(n_geo, L) <= 0.0)
        {
            continue;
        }
        float atten = calcLegacyDistanceAttenuation(d / sz, rt_hit_light_col[i].a);
        if (atten <= 0.0)
        {
            continue;
        }
        if (rt_hit_light_dir[i].w > 0.5)
        {
            continue;                       // spot lights: the textured projector below, or the forward lighting of blended hits
        }
        // a light that adds next to nothing at this hit needs no shadow ray
        const bool faint = max(rt_hit_light_col[i].r, max(rt_hit_light_col[i].g, rt_hit_light_col[i].b)) * atten * max(dot(n, L), 0.0) < 0.004;
        float sh = (rt_lights_noshadow || faint) ? 1.0 : rtShadowRayArea(base, rt_hit_light_pos[i].xyz, rtLightMargin(d), RT_LIGHT_SIZE, tc, lod);
        vec3 add = rtLightOnHit(rt_hit_light_col[i].rgb * atten * sh, L, n, v, s);
        sum += add;
        rt_dbg_point += add;
    }
    if (rt_hit_spot != 0)
    {
        float sdist, l_dist;
        vec3 slv;
        vec4 proj_tc;
        if (!clipProjectedLightVars(rt_hit_spot_center, hp, sdist, l_dist, slv, proj_tc))
        {
            float atten = calcLegacyDistanceAttenuation(sdist, falloff);
            rt_dbg_status = 0.3;
            if (atten > 0.0 && proj_tc.z > 0.0 && proj_tc.x > 0.0 && proj_tc.x < 1.0 && proj_tc.y > 0.0 && proj_tc.y < 1.0)
            {
                rt_dbg_status = 0.6;
                vec3 L = normalize(proj_origin - hp);
                float nl = dot(n, L);
                vec3 to_center = rt_hit_spot_center - hp;
                float cd = length(to_center);
                float sh = 1.0;
                if (nl > 0.0 && cd > 0.05 && !rt_lights_noshadow)
                {
                    sh = rtShadowRayArea(base, rt_hit_spot_center, rtLightMargin(cd), RT_LIGHT_SIZE, tc, lod);
                }
                if (nl > 0.0)
                {
                    rt_dbg_status = 1.0;
                }
                float amb_da = 0.0;
                float lit = 0.0;
                if (nl > 0.0)
                {
                    lit = nl * atten;
                    amb_da = (nl * 0.5 + 0.5) * proj_ambiance;
                    vec3 add = rtLightOnHit(getProjectedLightDiffuseColor(l_dist, proj_tc.xy) * atten * sh, L, n, v, s);
                    sum += add;
                    rt_dbg_spot += add;
                }
                // the projector's ambiance (the soft light around the cone), not shadowed in the main pass either
                vec3 amb_rgb = getProjectedLightAmbiance(amb_da, atten, lit, nl, 1.0, proj_tc.xy);
                if (s.pbr)
                {
                    float nl2 = 0.0;
                    vec3 diffPunc = vec3(0.0);
                    vec3 specPunc = vec3(0.0);
                    pbrPunctual(s.diffuseColor, s.specularColor, s.roughness, s.metallic, n, v, L, nl2, diffPunc, specPunc);
                    sum += amb_rgb * 3.25 * clamp(nl2 * (diffPunc + specPunc), vec3(0.0), vec3(10.0));
                }
                else
                {
                    sum += s.albedo * amb_rgb * max(dot(-L, n), 0.0);
                }
            }
        }
    }
    return sum;
}

// Blended legacy hits (hair): the viewer lights them in the forward alpha pass (materialF.glsl calcPointLightOrSpotLight) with the nearby lights as plain lights: a spot light is a wide cone with the
// spot exponent 2 (no projected texture, no edge, no shadow), the attenuation is tweaked to match the deferred one, the result is scaled by 0.9 (classic mode) and the whole colour by 1.1 (classic mode)
vec3 rtHitForwardLights(vec3 hp, vec3 n, vec3 albedo)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < RT_HIT_LIGHTS; ++i)
    {
        if (i >= rt_hit_light_count)
        {
            break;
        }
        vec3 lv = rt_hit_light_pos[i].xyz - hp;
        float d = length(lv);
        float sz = rt_hit_light_pos[i].w;
        if (d >= sz || d < 0.001)
        {
            continue;
        }
        lv /= d;
        float atten = calcLegacyDistanceAttenuation(d / sz, rt_hit_light_col[i].a);
        float da = 1.0;
        if (rt_hit_light_dir[i].w > 0.5)
        {
            float spot = max(dot(-rt_hit_light_dir[i].xyz, lv), 0.0);
            da *= spot * spot;
        }
        da *= dot(n, lv);
        if (da > 0.0)
        {
            sum += clamp(da * atten, 0.0, 1.0) * rt_hit_light_col[i].rgb * albedo;
        }
    }
    return sum * (classic_mode > 0 ? 0.9 : 1.0);
}

// The lighting of one decoded hit (the steps of the header comment above): the colour of the surface seen along dir at hp, before the fade into the fallback.
vec3 rtLitHitColor(vec3 origin, vec3 dir, vec3 light_dir_n, vec3 sunlit_in, vec3 amblit_in, vec2 tc, vec3 albedo, vec3 emissive, vec3 n_geo, vec3 n, vec3 hp, float t, float shadow, bool fb, vec4 mr, vec4 spec)
{
    vec3 color;
    if (fb)
    {
        color = albedo + emissive;
    }
    else
    {
        // the probe list is state of the invocation (reflectionProbeF.glsl): the pixel's own list is put aside, the hit gets a clean one, the pixel's comes back at the end
        probeListSave();
        probeListReset();
        if (rt_hit_quality != 23)           // (23: a cost test without any probe lookup at the hit)
        {
            preProbeSample(hp);
        }
        vec3 irradiance = amblit_in;
        if (classic_mode == 0 && rt_hit_quality != 23)
        {
            irradiance = sampleProbeAmbient(hp, n, amblit_in);
        }
        vec3 refl = reflect(dir, n);
        vec3 v = -dir;
        if (mr.z > 0.5)
        {
            float rough = max(mr.y, 0.04);
            vec3 radiance = rt_hit_quality == 23 ? vec3(0.1) : sampleProbes(hp, refl, rough * max_probe_lod);
            vec3 diffuseColor, specularColor;
            calcDiffuseSpecular(albedo, mr.x, diffuseColor, specularColor);
            color = pbrBaseLight(diffuseColor, specularColor, mr.x, v, n, rough, light_dir_n, sunlit_in, shadow, radiance, irradiance, emissive, 1.0, vec3(0.0), vec3(1.0));
            if (rt_hit_quality >= 2)
            {
                RtHitSurface s;
                s.pbr = true;
                s.diffuseColor = diffuseColor;
                s.specularColor = specularColor;
                s.roughness = rough;
                s.metallic = mr.x;
                s.albedo = albedo;
                s.spec = vec4(0.0);
                color += rtHitLocalLights(hp, n_geo, n, v, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
            }
        }
        else
        {
            float da = clamp(dot(n, light_dir_n), 0.0, 1.0);
            vec3 sl = sunlit_in;
            vec3 c;
            if (classic_mode > 0)
            {
                da = pow(da, 1.2);
                vec3 sun_contrib = vec3(min(da, shadow));
                c = srgb_to_linear(irradiance * 0.9 + linear_to_srgb(sun_contrib) * sl * 0.7);
                sl = srgb_to_linear(sl);
            }
            else
            {
                c = irradiance + min(da, shadow) * sl;
            }
            c *= albedo;
            if (spec.a > 0.0)
            {
                vec3 h, l;
                float nh, nl, nv, vh, lightDist;
                calcHalfVectors(light_dir_n, n, v, h, l, nh, nl, nv, vh, lightDist);
                if (nl > 0.0 && nh > 0.0)
                {
                    float lit = min(nl * 6.0, 1.0);
                    float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
                    float gtdenom = 2.0 * nh;
                    float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
                    float sc = shadow * fres * textureLod(lightFunc, vec2(nh, spec.a), 0.0).r * gt / (nh * nl);
                    c += lit * sc * sl * spec.rgb;
                }
                vec3 glossenv = rt_hit_quality == 23 ? vec3(0.0) : sampleProbes(hp, refl, (1.0 - spec.a) * max_probe_lod);
                applyGlossEnv(c, glossenv, spec, dir, n);
            }
            if (rt_hit_quality >= 2 && mr.w > -0.5)
            {
                RtHitSurface s;
                s.pbr = false;
                s.diffuseColor = albedo;
                s.specularColor = vec3(0.0);
                s.roughness = 1.0;
                s.metallic = 0.0;
                s.albedo = albedo;
                s.spec = spec;
                c += rtHitLocalLights(hp, n_geo, n, v, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
            }
            c = mix(c, albedo, max(mr.w, 0.0));       // a legacy material with an emissive mask in the diffuse alpha: the lit colour is blended towards the albedo, like the soften pass does for the pixels of the main view
            if (mr.w < -0.5 && rt_hit_quality >= 2)
            {
                c += rtHitForwardLights(hp, n, albedo);         // blended: lit like the forward alpha pass (the lights block above is for opaque hits)
                c *= (classic_mode > 0 ? 1.1 : 1.0);
            }
            color = c + emissive;
        }
    }
    if (!fb)
    {
        probeListRestore();
    }
    return color;
}

// <FS> A traced reflection ray whose hit is lit like a surface of the main pass (doc/vulkan/PLAN-glossy-mirror-quality.md, steps A and B): the hit's normal map, roughness / metallic
// (glTF, through pbrBaseLight) or specular map (legacy materials, the blinn-phong lines of main() below), the sun with the ray's shadow, and the reflection probes sampled AT THE HIT for ambient
// light and for the surface's own specular reflection. rt_hit_quality 0 keeps the flat diffuse shading of rtReflection (the water uses that one only).
vec4 rtHitLightsOnly(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);
vec4 rtHitProjectorInternals(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);

// <FS> For DLSS ray reconstruction (pipeline.cpp, rrHitDistF.glsl): how far the traced reflection ray of this pixel went (metres from its origin to the hit; RT_HIT_DISTANCE_MISS when it found nothing,
// a far away environment). 0 = not traced. Written to the alpha of the soften output, moved to a target of its own right after the lighting.
const float RT_HIT_DISTANCE_MISS = 500.0;
float rt_hit_dist_out = 0.0;

vec4 rtReflectionLit(vec3 origin, vec3 dir, vec3 light_dir_n, vec3 sunlit_in, vec3 amblit_in, vec2 tc, vec3 fallback)
{
    if (rt_hit_quality <= 0)
    {
        return rtReflection(origin, dir, light_dir_n, sunlit_in, amblit_in, tc, fallback);
    }
    rt_lights_noshadow = (rt_hit_quality == 4 || rt_hit_quality == 5);
    if (rt_hit_quality == 3 || rt_hit_quality == 4)
    {
        return rtHitLightsOnly(origin, dir, light_dir_n, tc);
    }
    if (rt_hit_quality == 6)
    {
        return rtHitProjectorInternals(origin, dir, light_dir_n, tc);
    }
    if (rt_hit_quality >= 7 && rt_hit_quality <= 9 || rt_hit_quality >= 11 && rt_hit_quality <= 13)
    {
        // test views of the hit: 7 = material (red metallic, green roughness, blue 0.5 for a glTF material / the legacy glossiness x 0.5 + 0.5 for one with a specular map), 8 = the probe ambient at the hit
        // (irradiance), 9 = the sun term alone (N.L x shadow x sunlit, white = lit)
        vec3 a7, e7, ng7, n7, hp7;
        float t7, sh7;
        bool fb7;
        vec4 mr7, sp7;
        if (!rtTraceHit(origin, dir, light_dir_n, tc, true, a7, e7, ng7, n7, hp7, t7, sh7, fb7, mr7, sp7))
        {
            return vec4(0.0);
        }
        if (rt_hit_quality == 7)
        {
            return vec4(mr7.x, mr7.y, (mr7.z > 0.5 ? 0.5 : 0.0) + (sp7.a > 0.0 ? 0.5 * sp7.a + 0.5 : 0.0), fb7 ? 1.0 : 0.75);
        }
        if (rt_hit_quality == 11)
        {
            return vec4(a7, 1.0);                       // the albedo of the hit (linear)
        }
        if (rt_hit_quality == 12)
        {
            preProbeSample(hp7);
            return vec4((classic_mode == 0 ? sampleProbeAmbient(hp7, n7, amblit_in) : amblit_in) * a7, 1.0);       // the ambient part of a legacy hit: irradiance x albedo
        }
        if (rt_hit_quality == 13)
        {
            // yellow: a legacy material that is alpha blended; else red: a glTF material, green: the emissive mask amount, blue: the legacy glossiness
            return mr7.w < -0.5 ? vec4(1.0, 1.0, 0.0, 1.0) : vec4(mr7.z, mr7.w, sp7.a, 1.0);
        }
        if (rt_hit_quality == 8)
        {
            preProbeSample(hp7);
            return vec4(classic_mode == 0 ? sampleProbeAmbient(hp7, n7, amblit_in) : amblit_in, 1.0);
        }
        return vec4(vec3(clamp(dot(n7, light_dir_n), 0.0, 1.0) * sh7) * sunlit_in, 1.0);
    }
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    float layer_alpha, l_t, l_shadow;
    vec3 l_albedo, l_emissive, l_n_geo, l_n, l_hp;
    bool l_fb;
    vec4 l_mr, l_spec;
    float m_alpha, m_t, m_shadow;
    vec3 m_albedo, m_emissive, m_n_geo, m_n, m_hp;
    bool m_fb;
    vec4 m_mr, m_spec;
    if (!rtTraceHitEx(origin, dir, light_dir_n, tc, rt_hit_quality != 24, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec, layer_alpha, l_albedo, l_emissive, l_n_geo, l_n, l_hp, l_t, l_shadow, l_fb, l_mr, l_spec,
                      m_alpha, m_albedo, m_emissive, m_n_geo, m_n, m_hp, m_t, m_shadow, m_fb, m_mr, m_spec))
    {
        rt_hit_dist_out = RT_HIT_DISTANCE_MISS;
        return vec4(0.0);
    }
    rt_hit_dist_out = t;
    if (rt_hit_quality == 22)
    {
        // debug: red = opacity of the nearest blended layer, green = of the second, blue = a blended surface is the hit itself (nothing opaque behind it)
        return vec4(max(layer_alpha, 0.0), m_alpha, layer_alpha < 0.0 ? 1.0 : 0.0, 1.0);
    }
    if (rt_hit_quality == 14)
    {
        // the hit's own albedo dimmed (so the avatar and the room can be told apart), plus: red = opacity of a blended layer in front of it, green = the highest opacity of any blended surface the ray met without using it
        // (it was behind the hit or too transparent), blue = how many (x0.25), yellow added where the opaque hit is a blended material itself, white where a blended surface was the hit (layer_alpha < 0)
        vec3 dbg = 0.3 * albedo;
        if (layer_alpha > 0.0)
        {
            dbg += vec3(layer_alpha, 0.0, 0.0);
        }
        else
        {
            dbg += vec3(0.0, l_shadow, min(l_t * 0.25, 1.0)) * 0.7;
        }
        if (layer_alpha < 0.0)
        {
            dbg += vec3(-layer_alpha);
        }
        if (mr.w < -0.5)
        {
            dbg += vec3(0.4, 0.4, 0.0);
        }
        return vec4(dbg, 1.0);
    }
    if (rt_hit_quality == 15)
    {
        // the albedo of the blended layer (linear), magenta where there is none; its distance in front of the hit in blue-less red channel is not shown
        return layer_alpha > 0.0 ? vec4(l_albedo, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
    }
    if (rt_hit_quality == 18)
    {
        return vec4(n_geo * 0.5 + 0.5, 1.0);          // test: the geometric normal of the hit (eye space)
    }
    if (rt_hit_quality == 17)
    {
        // test: every hit is lit as if its normal looked at the viewer (the flat triangle normals of avatars are suspected of leaving the hair and face in the dark)
        n = -dir;
        n_geo = -dir;
    }
    // the hit, then the blended surfaces in front of it (hair, an avatar's shell), the farther one first: each lit like any hit and composited over what is behind by its opacity, like the alpha pass of
    // the main view. One call site of the hit lighting for all of them (the function is big: three inlined copies tripled the compile time of this pass)
    vec3 color = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        const bool layer_pass = i > 0;
        if (layer_pass && (layer_alpha <= 0.0 || (i == 1 && m_alpha <= 0.0)))
        {
            continue;
        }
        const bool far_layer = i == 1;
        const vec3 s_albedo = i == 0 ? albedo : (far_layer ? m_albedo : l_albedo);
        const vec3 s_emissive = i == 0 ? emissive : (far_layer ? m_emissive : l_emissive);
        const vec3 s_n_geo = i == 0 ? n_geo : (far_layer ? m_n_geo : l_n_geo);
        const vec3 s_n = i == 0 ? n : (far_layer ? m_n : l_n);
        const vec3 s_hp = i == 0 ? hp : (far_layer ? m_hp : l_hp);
        const float s_t = i == 0 ? t : (far_layer ? m_t : l_t);
        const float s_shadow = i == 0 ? shadow : (far_layer ? m_shadow : l_shadow);
        const bool s_fb = i == 0 ? fb : (far_layer ? m_fb : l_fb);
        const vec4 s_mr = i == 0 ? mr : (far_layer ? m_mr : l_mr);
        const vec4 s_spec = i == 0 ? spec : (far_layer ? m_spec : l_spec);
        const vec3 lit = rtLitHitColor(origin, dir, light_dir_n, sunlit_in, amblit_in, tc, s_albedo, s_emissive, s_n_geo, s_n, s_hp, s_t, s_shadow, s_fb, s_mr, s_spec);
        color = i == 0 ? lit : mix(color, lit, far_layer ? m_alpha : layer_alpha);
    }
    float fade = smoothstep(250.0, 1200.0, t);
    color = mix(color, fallback, fade);
    if (layer_alpha < 0.0)
    {
        color = mix(fallback, color, -layer_alpha);        // only a blended surface on the way: partly transparent over what the caller has behind
    }
    return vec4(color, 1.0);
}
#endif

#ifdef SOFTEN_RT_REFLECTION
// rt_hit_quality 3 (test): a traced hit shows only what the local lights add (x4), magenta where the soften pass was sent no light at all
vec4 rtHitLightsOnly(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc)
{
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, light_dir_n, tc, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec))
    {
        return vec4(0.0);
    }
    if (rt_hit_light_count == 0 && rt_hit_spot == 0)
    {
        return vec4(1.0, 0.0, 1.0, 1.0);
    }
    RtHitSurface s;
    s.pbr = mr.z > 0.5;
    calcDiffuseSpecular(albedo, mr.x, s.diffuseColor, s.specularColor);
    s.roughness = max(mr.y, 0.04);
    s.metallic = mr.x;
    s.albedo = albedo;
    s.spec = spec;
    if (!s.pbr)
    {
        s.diffuseColor = albedo;
        s.specularColor = vec3(0.0);
        s.roughness = 1.0;
        s.metallic = 0.0;
    }
    rtHitLocalLights(hp, n_geo, n, -dir, clamp(0.5 * log2(max(t + length(origin), 1.0)) - 1.0, 0.0, 4.0), tc, s);
    // red: the point lights, green: the projector (x4), blue: how far the projector got (0.3 inside its range, 0.6 inside its cone, 1 facing the hit)
    return vec4(4.0 * dot(rt_dbg_point, vec3(0.3, 0.59, 0.11)), 4.0 * dot(rt_dbg_spot, vec3(0.3, 0.59, 0.11)), rt_dbg_status, 1.0);
}
#endif

#ifdef SOFTEN_RT_REFLECTION
// rt_hit_quality 6 (test): the projector's uniforms and what its texture returns at the hit. red: |light colour| / 2, green: proj_lod / 10, blue: the projected diffuse colour x4 (luminance);
// the same pixel is also marked (alpha 1) only when the projector's range / cone test passed; magenta: no projector uploaded
vec4 rtHitProjectorInternals(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc)
{
    vec3 albedo, emissive, n_geo, n, hp;
    float t, shadow;
    bool fb;
    vec4 mr, spec;
    if (!rtTraceHit(origin, dir, light_dir_n, tc, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec))
    {
        return vec4(0.0);
    }
    if (rt_hit_spot == 0)
    {
        return vec4(1.0, 0.0, 1.0, 1.0);
    }
    float sdist, l_dist;
    vec3 slv;
    vec4 proj_tc;
    vec3 dl = vec3(0.0);
    if (!clipProjectedLightVars(rt_hit_spot_center, hp, sdist, l_dist, slv, proj_tc))
    {
        if (proj_tc.x > 0.0 && proj_tc.x < 1.0 && proj_tc.y > 0.0 && proj_tc.y < 1.0 && proj_tc.z > 0.0)
        {
            dl = getProjectedLightDiffuseColor(l_dist, proj_tc.xy);
        }
    }
    return vec4(clamp(length(color) * 0.5, 0.0, 1.0), clamp(proj_lod * 0.1, 0.0, 1.0), clamp(4.0 * dot(dl, vec3(0.3, 0.59, 0.11)), 0.0, 1.0), 1.0);
}
#endif

void main()
{
    frag_data[0] = vec4(0.0);
    frag_data[1] = vec4(0.0);
#ifdef SOFTEN_RT_REFLECTION
    if (rt_glossy_reflections == 0)
    {
        return;
    }
    vec2  tc    = vary_fragcoord.xy;
    float depth = getDepth(tc.xy);
    if (depth >= 1.0)
    {
        return;         // the sky
    }
    vec4  pos = getPositionWithDepth(tc, depth);
    GBufferInfo gb = getGBuffer(tc);

    const bool sky_pixel = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS);
    if (sky_pixel)
    {
        return;
    }
    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;
    vec3 sunlit, amblit, additive, atten;
    calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec4 rt_refl = vec4(0.0);
    bool traced = false;
    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        float gloss = 1.0 - gb.specular.g;
        if (gloss >= (rt_glossy_reflections >= 4 ? 0.0 : 0.9))
        {
            // the probe result of the lighting pass (its fallback for a miss, for the far fade and for a blended surface alone)
            vec3 irradiance = amblit_linear;
            vec3 radiance = vec3(0.0);
            sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);
            rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, radiance);
            traced = true;
        }
    }
    else
    {
        vec4 spec = gb.specular;
        spec.rgb = srgb_to_linear(spec.rgb);
        if (spec.a >= (rt_glossy_reflections >= 4 ? 0.01 : 0.9))
        {
            vec3 irradiance = amblit;
            vec3 glossenv = vec3(0);
            vec3 legacyenv = vec3(0);
            sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, gb.envIntensity, false, amblit_linear);
            rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, glossenv);
            traced = true;
        }
    }
    if (traced)
    {
        frag_data[0] = vec4(rt_refl.rgb, rt_refl.a != 0.0 ? rt_refl.a : -1.0);
        frag_data[1] = vec4(rt_hit_dist_out, 0.0, 0.0, 0.0);
    }
#endif
}
