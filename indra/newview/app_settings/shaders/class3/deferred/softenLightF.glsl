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
vec4 rtReflection(vec3 origin, vec3 dir, vec3 sun_dir, vec3 sunlit_linear, vec3 amblit_linear, vec2 pos_screen, vec3 fallback);
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
            vec4 rt_refl = rtReflection(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, radiance);
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
            vec4 rt_refl = rtReflection(pos.xyz + gb.normal * 0.02, normalize(reflect(pos.xyz, gb.normal)), normalize(light_dir), sunlit_linear, amblit_linear, tc, glossenv);
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
