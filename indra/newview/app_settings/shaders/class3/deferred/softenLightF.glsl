/**
 * @file class3/deferred/softenLightF.glsl
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

out vec4 frag_color;

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
uniform int rt_hit_quality;     // how a traced hit is lit: 0 flat diffuse (rtReflection), 1 the lighting of the main pass at the hit (normal map, roughness / metallic / specular map, probes)
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback);
bool rtTraceHit(vec3 origin, vec3 dir, vec3 sun_dir, vec2 pos_screen, bool full, out vec3 albedo, out vec3 emissive, out vec3 n_geo_eye, out vec3 n_eye, out vec3 hit_pos, out float t,
                out float shadow, out bool fullbright, out vec4 mr, out vec4 spec);
void preProbeSample(vec3 pos);
vec3 sampleProbes(vec3 pos, vec3 dir, float lod);
vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit);
uniform float max_probe_lod;

// local lights at a hit (rt_hit_quality 2): the nearest point lights as arrays (eye space centre and size, linear colour and falloff) and the first projector (spot light) through the
// uniforms its own shader uses (proj_mat, proj_n, proj_origin, proj_lod ... set by LLPipeline::setupSpotLight, the projected texture is projectionMap, colour / size / falloff of the light)
const int RT_HIT_LIGHTS = 8;
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
        float sh = rt_lights_noshadow ? 1.0 : rtShadowRay(base, L, d - rtLightMargin(d), tc, lod);
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
                    sh = rtShadowRay(base, to_center / cd, cd - rtLightMargin(cd), tc, lod);
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

// <FS> A traced reflection ray whose hit is lit like a surface of the main pass (doc/vulkan/PLAN-glossy-mirror-quality.md, steps A and B): the hit's normal map, roughness / metallic
// (glTF, through pbrBaseLight) or specular map (legacy materials, the blinn-phong lines of main() below), the sun with the ray's shadow, and the reflection probes sampled AT THE HIT for ambient
// light and for the surface's own specular reflection. rt_hit_quality 0 keeps the flat diffuse shading of rtReflection (the water uses that one only).
vec4 rtHitLightsOnly(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);
vec4 rtHitProjectorInternals(vec3 origin, vec3 dir, vec3 light_dir_n, vec2 tc);

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
    if (!rtTraceHit(origin, dir, light_dir_n, tc, true, albedo, emissive, n_geo, n, hp, t, shadow, fb, mr, spec))
    {
        return vec4(0.0);
    }
    vec3 color;
    if (fb)
    {
        color = albedo + emissive;
    }
    else
    {
        preProbeSample(hp);
        vec3 irradiance = amblit_in;
        if (classic_mode == 0)
        {
            irradiance = sampleProbeAmbient(hp, n, amblit_in);
        }
        vec3 refl = reflect(dir, n);
        vec3 v = -dir;
        if (mr.z > 0.5)
        {
            float rough = max(mr.y, 0.04);
            vec3 radiance = sampleProbes(hp, refl, rough * max_probe_lod);
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
                vec3 glossenv = sampleProbes(hp, refl, (1.0 - spec.a) * max_probe_lod);
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
    float fade = smoothstep(250.0, 1200.0, t);
    return vec4(mix(color, fallback, fade), 1.0);
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
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);

    GBufferInfo gb = getGBuffer(tc);

    vec3 colorEmissive = gb.emissive.rgb;
    float envIntensity = gb.envIntensity;
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec4 baseColor     = gb.albedo;
    vec4 spec        = gb.specular; // NOTE: PBR linear Emissive

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
    vec2 scol_ambocc = texture(lightMap, vary_fragcoord.xy).rg;
#endif

#if defined(HAS_SUN_SHADOW)
    float scol       = max(scol_ambocc.r, baseColor.a);
#else
    float scol = 1.0;
#endif
#if defined(HAS_SSAO)
    float ambocc     = scol_ambocc.g;
#else
    float ambocc = 1.0;
#endif

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);

    if (classic_mode > 0)
        sunlit *= 1.35;

    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3  radiance  = vec3(0);
    int dbg_branch = 0;                  // probe (remove): 1 PBR path, 2 legacy path, 3 other
    int dbg_trace = 0;                   // probe: 1 a reflection ray was traced and missed, 2 it hit
    vec3 dbg_radiance = vec3(0.0);       // probe (mode 10): the reflection value just before the lighting call
    vec2 dbg_brdf = vec2(0.0);           // probe (mode 11): the reflectance response the lighting uses
    vec3 dbg_orm = vec3(0.0);            // probe: occlusion, roughness, metallic of a PBR pixel (mode 9)
    vec3 dbg_rt_color = vec3(0.0);       // probe: the colour the traced reflection found (mode 7)
    float dbg_gloss = 0.0;               // probe: the gloss of the pixel

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        dbg_branch = 1;
        vec3 orm = spec.rgb;
        float perceptualRoughness = orm.g;
        float metallic = orm.b;
        float ao = orm.r;

        vec3  irradiance = amblit_linear;

        // PBR IBL
        float gloss      = 1.0 - perceptualRoughness;

        sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);

#ifdef SOFTEN_RT_REFLECTION
        dbg_gloss = gloss;
        dbg_orm = vec3(ao, perceptualRoughness, metallic);
        if (rt_glossy_reflections != 0 && rt_glossy_reflections != 2 && rt_glossy_reflections != 5 && rt_glossy_reflections != 6 && gloss >= (rt_glossy_reflections >= 4 ? 0.0 : 0.9))
        {
            // <FS> a ray along the reflected view direction into the ray scene: a hit replaces the probe / screen space result, a miss keeps it (3: hits green, misses red; 4: the same for every gloss)
            vec4 rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, radiance);
            if (rt_glossy_reflections >= 3)
            {
                dbg_trace = rt_refl.a > 0.5 ? 2 : 1;       // the views 3 and 4 only show where it traced and what it found
                dbg_rt_color = rt_refl.rgb;
            }
            else
            {
                radiance = mix(radiance, rt_refl.rgb, rt_refl.a);
            }
        }
#endif

        adjustIrradiance(irradiance, ambocc);

        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor.rgb, metallic, diffuseColor, specularColor);

        vec3 v = -normalize(pos.xyz);
        dbg_radiance = radiance;
        dbg_brdf = BRDF(clamp(dot(gb.normal, v), 0.0, 1.0), 1.0 - perceptualRoughness);
        color = pbrBaseLight(diffuseColor, specularColor, metallic, v, gb.normal, perceptualRoughness, light_dir, sunlit_linear, scol, radiance, irradiance, colorEmissive, ao, additive, atten);
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI))
    {
        // actual HDRI sky, just copy color value
        color = colorEmissive.rgb;
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        //should only be true of WL sky, port over base color value and scale for fake HDR
#if defined(HAS_EMISSIVE)
        color = colorEmissive.rgb;
#else
        color = baseColor.rgb;
#endif
        color = srgb_to_linear(color);
        color *= sky_hdr_scale;
    }
    else
    {
        dbg_branch = 2;
        // legacy shaders are still writng sRGB to gbuffer
        baseColor.rgb = srgb_to_linear(baseColor.rgb);

        spec.rgb = srgb_to_linear(spec.rgb);

        float da          = clamp(dot(gb.normal, light_dir.xyz), 0.0, 1.0);

        vec3 irradiance = amblit;
        vec3 glossenv = vec3(0);
        vec3 legacyenv = vec3(0);

        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, envIntensity, false, amblit_linear);

#ifdef SOFTEN_RT_REFLECTION
        dbg_gloss = spec.a;
        if (rt_glossy_reflections != 0 && rt_glossy_reflections != 2 && rt_glossy_reflections != 5 && rt_glossy_reflections != 6 && spec.a >= (rt_glossy_reflections >= 4 ? 0.01 : 0.9))
        {
            vec4 rt_refl = rtReflectionLit(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, glossenv);
            if (rt_glossy_reflections >= 3)
            {
                dbg_trace = rt_refl.a > 0.5 ? 2 : 1;
                dbg_rt_color = rt_refl.rgb;
            }
            else
            {
                glossenv = mix(glossenv, rt_refl.rgb, rt_refl.a);
            }
        }
#endif

        adjustIrradiance(irradiance, ambocc);

        // apply lambertian IBL only (see pbrIbl)
        color.rgb = irradiance;

        if (classic_mode > 0)
        {
            da = pow(da,1.2);
            vec3 sun_contrib = vec3(min(da, scol));

            color.rgb = srgb_to_linear(color.rgb * 0.9 + (linear_to_srgb(sun_contrib) * sunlit_linear * 0.7));
            sunlit_linear = srgb_to_linear(sunlit_linear);
        }
        else
        {
            vec3 sun_contrib = min(da, scol) * sunlit_linear;
            color.rgb += sun_contrib;
        }

        color.rgb *= baseColor.rgb;

        vec3 refnormpersp = reflect(pos.xyz, gb.normal);

        if (spec.a > 0.0)
        {
            vec3  lv = light_dir.xyz;
            vec3  h, l, v = -normalize(pos.xyz);
            float nh, nl, nv, vh, lightDist;
            vec3 n = gb.normal;
            calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

            if (nl > 0.0 && nh > 0.0)
            {
                float lit = min(nl*6.0, 1.0);

                float sa = nh;
                float fres = pow(1 - vh, 5) * 0.4+0.5;
                float gtdenom = 2 * nh;
                float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

                scol *= fres*texture(lightFunc, vec2(nh, spec.a)).r*gt/(nh*nl);
                color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
            }

            // add radiance map
            applyGlossEnv(color, glossenv, spec, pos.xyz, gb.normal);

        }

        color.rgb = mix(color.rgb, baseColor.rgb, baseColor.a);

        if (envIntensity > 0.0)
        {  // add environment map
            applyLegacyEnv(color, legacyenv, spec, pos.xyz, gb.normal, envIntensity);
        }
   }

    //color.r = classic_mode > 0 ? 1.0 : 0.0;
#ifdef SOFTEN_RT_REFLECTION
    if (rt_hit_quality == 10 && depth < 1.0)
    {
        // test (hitq=10): the surface of every pixel is shaded by the hit shading of the traced mirror instead of the lighting above, from a ray of the eye. The same surface, the same lights: any difference to
        // hitq=0 is a difference of the two shadings (the traced mirror's ray scene, material rows and lighting of a hit against the G-buffer and its lighting)
        vec4 rt_eye = rtReflectionLit(vec3(0.0), normalize(pos.xyz), normalize(light_dir), sunlit_linear, amblit_linear, tc, color.rgb);
        color.rgb = mix(color.rgb, rt_eye.rgb, rt_eye.a);
    }
    if (rt_glossy_reflections == 3 || rt_glossy_reflections == 4)
    {
        // probe: green where the reflection ray hit something, red where it missed (the pixels that were traced only; 4 traces every gloss)
        if (dbg_trace != 0)
        {
            color.rgb = dbg_trace == 2 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        }
    }
    else if (rt_glossy_reflections == 12)
    {
        if (dbg_branch == 1)
        {
            color.rgb = vec3(BRDF(clamp(dot(gb.normal, -normalize(pos.xyz)), 0.0, 1.0), 0.98), 0.0);       // probe: the BRDF lookup at roughness argument 0.98 (the mirror asks for 1.0)
        }
    }
    else if (rt_glossy_reflections == 13)
    {
        if (dbg_branch == 1)
        {
            color.rgb = vec3(BRDF(clamp(dot(gb.normal, -normalize(pos.xyz)), 0.0, 1.0), 0.5), 0.0);        // probe: the BRDF lookup at roughness argument 0.5
        }
    }
    else if (rt_glossy_reflections == 15)
    {
        color.rgb = vec3(1.0, 0.0, 1.0);       // probe: plumbing check for values above 11 (every pixel magenta)
    }
    else if (rt_glossy_reflections == 14)
    {
        if (dbg_branch != 0)
        {
            color.rgb = vec3(BRDF(tc.x, 1.0 - tc.y), 0.0);       // probe: the BRDF lookup texture itself: view angle left to right, roughness argument 0 (bottom) to 1 (top); scale red, bias green
        }
    }
    else if (rt_glossy_reflections == 10)
    {
        if (dbg_branch == 1)
        {
            color.rgb = dbg_radiance;
        }
    }
    else if (rt_glossy_reflections == 11)
    {
        if (dbg_branch == 1)
        {
            color.rgb = vec3(dbg_brdf, 0.0);          // probe: the BRDF lookup's two terms (scale red, bias green)
        }
    }
    else if (rt_glossy_reflections == 9)
    {
        if (dbg_branch == 1)
        {
            color.rgb = dbg_orm;                    // probe: occlusion (red), roughness (green), metallic (blue) of the PBR path pixels
        }
    }
    else if (rt_glossy_reflections == 8)
    {
        // probe: the base colour of the surface (the G-buffer's albedo) where something was traced, linear
        if (dbg_trace != 0)
        {
            color.rgb = baseColor.rgb;
        }
    }
    else if (rt_glossy_reflections == 7)
    {
        // probe: the colour the traced reflection found, at every traced pixel (the rest keeps the picture)
        if (dbg_trace == 2)
        {
            color.rgb = dbg_rt_color;
        }
    }
    else if (rt_glossy_reflections == 5)
    {
        color.rgb = dbg_branch != 0 ? vec3(dbg_gloss) : vec3(0.0, 0.0, 0.3);        // probe: the gloss as a grey level (PBR: 1 - roughness, legacy: the specular alpha)
    }
    else if (rt_glossy_reflections == 2)
    {
        color.rgb = dbg_branch == 1 ? vec3(1.0, 0.0, 1.0) : color.rgb;              // probe: every pixel of the PBR path
    }
    else if (rt_glossy_reflections == 6)
    {
        color.rgb = mix(color.rgb, dbg_branch == 1 ? vec3(1.0, 0.0, 0.0) : (dbg_branch == 2 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0)), 0.6);       // probe: red PBR path, green legacy path, blue the rest
    }
#endif
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;

    frag_color.rgb = clampHDRRange(color.rgb * final_scale); //output linear since local lights will be added to this shader's results
    frag_color.a = 0.0;
}
