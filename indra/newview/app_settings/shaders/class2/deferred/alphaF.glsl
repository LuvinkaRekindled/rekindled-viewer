/**
 * @file alphaF.glsl
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

//class2/deferred/alphaF.glsl

/*[EXTRA_CODE_HERE]*/

#define INDEXED 1
#define NON_INDEXED 2
#define NON_INDEXED_NO_COLOR 3

out vec4 frag_color;

uniform mat3 env_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int classic_mode;

#ifdef USE_DIFFUSE_TEX
uniform sampler2D diffuseMap;
#endif

in vec3 vary_fragcoord;
in vec3 vary_position;
in vec2 vary_texcoord0;
in vec3 vary_norm;

#ifdef USE_VERTEX_COLOR
in vec4 vertex_color; //vertex color should be treated as sRGB
#endif

uniform float minimum_alpha;

uniform mat4 proj_mat;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;
uniform vec4 light_position[8];
uniform vec3 light_direction[8];
uniform vec4 light_attenuation[8];
uniform vec3 light_diffuse[8];

// <FS> shaders family 2 (doc/vulkan/SHADERS-F2-alpha.md): the light list of the view (LLRender::setAlphaLights): every light in view that the opaque surfaces are lit by, not the six nearest to the camera.
// Three vec4 a light: (centre xyz, size), (colour rgb, falloff), (spot axis xyz, 1 = point light / 0 = spot), in eye space. alpha_light_count -1: no list, the hardware slots 2-7 are used (as before).
// The size and falloff are the ones the opaque pass uses; the two light functions below take the attenuation in the form of the hardware slots, so it is rebuilt: la = (12 fa - 9) / size.
uniform int alpha_light_count;
uniform vec4 alpha_lights[192];

void waterClip(vec3 pos);

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

float getAmbientClamp();

void mirrorClip(vec3 pos);

#ifdef ALPHA_UBER
// item D1 (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the program also draws the fullbright alpha draws (deferred/fullbrightF.glsl with IS_ALPHA), chosen per draw by the flag the vertex shader passes,
// so the runs of a lit and a fullbright draw in the blend order are one run
flat in uint vary_fullbright;
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten);
#endif

void preProbeSample(vec3 pos);
vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit);
#ifdef ALPHA_UBER_MAT
// item D2 (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the material alpha draws, shaded as the blend path of materialF.glsl does (the same lighting, the same terms), chosen per draw by the flags the vertex shader
// passes (alphaPullV.glsl). The code below is that path with its compile time choices (normal map, specular map) made at run time and its samplers read from the bindless table.
flat in uint vary_material;
in vec3 vary_tangent;
in vec2 vary_texcoord1;
in vec2 vary_texcoord2;
flat in float vary_sign;
flat in uint vary_slot_normal;
flat in uint vary_slot_specular;
flat in vec4 vary_specular;
flat in vec2 vary_matparams;
uniform sampler2D lightFunc;
void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
#endif
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);

vec3 calcPointLightOrSpotLight(vec3 light_col, vec3 diffuse, vec3 v, vec3 n, vec4 lp, vec3 ln, float la, float fa, float is_pointlight, float ambiance)
{
    // SL-14895 inverted attenuation work-around
    // This routine is tweaked to match deferred lighting, but previously used an inverted la value. To reconstruct
    // that previous value now that the inversion is corrected, we reverse the calculations in LLPipeline::setupHWLights()
    // to recover the `adjusted_radius` value previously being sent as la.
    float falloff_factor = (12.0 * fa) - 9.0;
    float inverted_la = falloff_factor / la;
    // Yes, it makes me want to cry as well. DJH

    vec3 col = vec3(0);

    //get light vector
    vec3 lv = lp.xyz-v;

    //get distance
    float dist = length(lv);
    float da = 1.0;

    /*if (dist > inverted_la)
    {
        return col;
    }

    clip to projector bounds
     vec4 proj_tc = proj_mat * lp;

    if (proj_tc.z < 0
     || proj_tc.z > 1
     || proj_tc.x < 0
     || proj_tc.x > 1
     || proj_tc.y < 0
     || proj_tc.y > 1)
    {
        return col;
    }*/

    if (dist > 0.0 && inverted_la > 0.0)
    {
        dist /= inverted_la;

        //normalize light vector
        lv = normalize(lv);

        //distance attenuation
        float dist_atten = clamp(1.0-(dist-1.0*(1.0-fa))/fa, 0.0, 1.0);
        dist_atten *= dist_atten;
        dist_atten *= 2.0f;

        if (dist_atten <= 0.0)
        {
           return col;
        }

        // spotlight coefficient.
        float spot = max(dot(-ln, lv), is_pointlight);
        da *= spot*spot; // GL_SPOT_EXPONENT=2

        //angular attenuation
        da *= dot(n, lv);
        da = max(0.0, da);

        float lit = 0.0f;

        float amb_da = 0.0;//ambiance;
        if (da > 0)
        {
            lit = clamp(da * dist_atten, 0.0, 1.0);
            col = lit * light_col * diffuse;
            amb_da += (da*0.5+0.5) * ambiance;
        }
        amb_da += (da*da*0.5 + 0.5) * ambiance;
        amb_da *= dist_atten;
        amb_da = min(amb_da, 1.0f - lit);

        // SL-10969 ... need to work out why this blows out in many setups...
        //col.rgb += amb_da * light_col * diffuse;

        // no spec for alpha shader...
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    col = max(col * final_scale, vec3(0));
    return col;
}

#ifdef ALPHA_UBER_MAT
vec3 matPointLight(vec3 light_col, vec3 npos, vec3 diffuse, vec4 spec, vec3 v, vec3 n, vec4 lp, vec3 ln, float la, float fa, float is_pointlight, inout float glare, float ambiance)
{
    // SL-14895 inverted attenuation work-around (as materialF.glsl)
    float falloff_factor = (12.0 * fa) - 9.0;
    float inverted_la = falloff_factor / la;

    vec3 col = vec3(0);

    vec3 lv = lp.xyz - v;
    float dist = length(lv);
    float da = 1.0;

    dist /= inverted_la;

    if (dist > 0.0 && inverted_la > 0.0)
    {
        lv = normalize(lv);

        float dist_atten = clamp(1.0 - (dist - 1.0*(1.0 - fa)) / fa, 0.0, 1.0);
        dist_atten *= dist_atten;
        dist_atten *= 2.0f;

        if (dist_atten <= 0.0)
        {
            return col;
        }

        float spot = max(dot(-ln, lv), is_pointlight);
        da *= spot*spot; // GL_SPOT_EXPONENT=2

        da *= dot(n, lv);

        float lit = 0.0f;

        float amb_da = ambiance;
        if (da >= 0)
        {
            lit = clamp(da * dist_atten, 0.0, 1.0);
            col = lit * light_col * diffuse;
            amb_da += (da*0.5 + 0.5) * ambiance;
        }
        amb_da += (da*da*0.5 + 0.5) * ambiance;
        amb_da *= dist_atten;
        amb_da = min(amb_da, 1.0f - lit);

        if (spec.a > 0.0)
        {
            vec3 h = normalize(lv + npos);
            float nh = dot(n, h);
            float nv = dot(n, npos);
            float vh = dot(npos, h);
            float sa = nh;
            float fres = pow(1 - dot(h, npos), 5)*0.4 + 0.5;

            float gtdenom = 2 * nh;
            float gt = max(0, min(gtdenom * nv / vh, gtdenom * da / vh));

            if (nh > 0.0)
            {
                float scol = fres*texture(lightFunc, vec2(nh, spec.a)).r*gt / (nh*da);
                vec3 speccol = lit*scol*light_col.rgb*spec.rgb;
                speccol = clamp(speccol, vec3(0), vec3(1));
                col += speccol;

                float cur_glare = max(speccol.r, speccol.g);
                cur_glare = max(cur_glare, speccol.b);
                glare = max(glare, speccol.r);
                glare += max(cur_glare, 0.0);
            }
        }
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    return max(col * final_scale, vec3(0.0, 0.0, 0.0));
}

// the blend path of materialF.glsl main()
vec4 materialAlpha()
{
    // diffcol == diffuse map combined with vertex color
    vec4 diffcol = diffuseLookup(vary_texcoord0.xy);
    diffcol.rgb *= vertex_color.rgb;

    // spec == specular map combined with specular color
    vec4 spec;
    if ((vary_material & 4u) != 0u)
    {
        spec = llFbTex(vary_slot_specular, vary_texcoord2.xy);
        spec.rgb *= vary_specular.rgb;
    }
    else
    {
        spec = vec4(vary_specular.rgb, 1.0);
    }
    float env = vary_matparams.x * spec.a;
    float glossiness = vary_specular.a;
    vec3 norm;
    if ((vary_material & 2u) != 0u)
    {
        vec4 vNt = llFbTex(vary_slot_normal, vary_texcoord1.xy);
        glossiness *= vNt.a;
        vNt.xyz = vNt.xyz * 2 - 1;
        vec3 vN = vary_norm;
        vec3 vT = vary_tangent.xyz;
        vec3 vB = vary_sign * cross(vN, vT);
        norm = normalize(vNt.x * vT + vNt.y * vB + vNt.z * vN);
    }
    else
    {
        norm = normalize(vary_norm);
    }

    float emissive = vary_matparams.y;

    //forward rendering, output lit linear color
    diffcol.rgb = srgb_to_linear(diffcol.rgb);
    spec.rgb = srgb_to_linear(spec.rgb);
    spec.a = glossiness; // pack glossiness into spec alpha for lighting functions

    vec3 pos = vary_position;

    float shadow = 1.0;
#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos, norm, vary_texcoord0.xy);
#endif

    vec4 diffuse = diffcol;

    vec3 color = vec3(0,0,0);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVarsLinear(pos.xyz, norm.xyz, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 ambenv = amblit;
    vec3 glossenv;
    vec3 legacyenv;
    sampleReflectionProbesLegacy(ambenv, glossenv, legacyenv, pos.xy*0.5+0.5, pos.xyz, norm.xyz, glossiness, env, true, amblit_linear);

    color = ambenv;

    float da          = clamp(dot(norm.xyz, light_dir.xyz), 0.0, 1.0);
    if (classic_mode > 0)
    {
        da = pow(da,1.2);
        vec3 sun_contrib = vec3(min(da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color *= diffcol.rgb;

    float glare = 0.0;

    if (glossiness > 0.0)
    {
        vec3  lv = light_dir.xyz;
        vec3  h, l, v = -normalize(pos.xyz);
        float nh, nl, nv, vh, lightDist;
        vec3 n = norm.xyz;
        calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

        if (nl > 0.0 && nh > 0.0)
        {
            float lit = min(nl*6.0, 1.0);

            float sa = nh;
            float fres = pow(1 - vh, 5) * 0.4+0.5;
            float gtdenom = 2 * nh;
            float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

            float scol = shadow*fres*texture(lightFunc, vec2(nh, glossiness)).r*gt/(nh*nl);
            color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
        }

        // add radiance map
        applyGlossEnv(color, glossenv, spec, pos.xyz, norm.xyz);
    }

    color = mix(color.rgb, diffcol.rgb, emissive);

    if (env > 0.0)
    {  // add environmentmap
        applyLegacyEnv(color, legacyenv, spec, pos.xyz, norm.xyz, env);

        float cur_glare = max(max(legacyenv.r, legacyenv.g), legacyenv.b);
        cur_glare = clamp(cur_glare, 0, 1);
        cur_glare *= env;
        glare += cur_glare;
    }

    vec3 npos = normalize(-pos.xyz);
    vec3 light = vec3(0, 0, 0);

#define MAT_LIGHT_LOOP(i) light.rgb += matPointLight(light_diffuse[i].rgb, npos, diffuse.rgb, spec, pos.xyz, norm.xyz, light_position[i], light_direction[i].xyz, light_attenuation[i].x, light_attenuation[i].y, light_attenuation[i].z, glare, light_attenuation[i].w );

    MAT_LIGHT_LOOP(1)
    if (alpha_light_count >= 0)
    {
        for (int i = 0; i < alpha_light_count; ++i)
        {
            const vec4 l0 = alpha_lights[i * 3];
            const vec4 l1 = alpha_lights[i * 3 + 1];
            const vec4 l2 = alpha_lights[i * 3 + 2];
            const float fa = l1.w + 1.0;
            light.rgb += matPointLight(l1.rgb, npos, diffuse.rgb, spec, pos.xyz, norm.xyz, vec4(l0.xyz, 1.0), l2.xyz, (12.0 * fa - 9.0) / l0.w, fa, l2.w, glare, 0.0);
        }
    }
    else
    {
        MAT_LIGHT_LOOP(2)
        MAT_LIGHT_LOOP(3)
        MAT_LIGHT_LOOP(4)
        MAT_LIGHT_LOOP(5)
        MAT_LIGHT_LOOP(6)
        MAT_LIGHT_LOOP(7)
    }

    color += light;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, vec4(color, 1.0)).rgb;

    glare *= 1.0-emissive;
    glare = min(glare, 1.0);
    float al = max(diffcol.a, glare) * vertex_color.a;
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
    return max(vec4(color * final_scale, al), vec4(0));
}
#endif

void main()
{
    mirrorClip(vary_position);

    vec2 frag = vary_fragcoord.xy/vary_fragcoord.z*0.5+0.5;

    vec4 pos = vec4(vary_position, 1.0);
#ifndef IS_AVATAR_SKIN
    // clip against water plane unless this is a legacy avatar skin
    waterClip(pos.xyz);
#endif
    vec3 norm = vary_norm;

#ifdef ALPHA_UBER_MAT
    if ((vary_material & 1u) != 0u)
    {
        frag_color = materialAlpha();       // item D2: a material draw
        return;
    }
#endif
#ifdef ALPHA_UBER
    if (vary_fullbright != 0u)
    {
        // a fullbright alpha draw: as fullbrightF.glsl (IS_ALPHA, HAS_ALPHA_MASK): the texture times the vertex colour, the fog; no lighting
        vec4 fb_color = diffuseLookup(vary_texcoord0.xy);
        float fb_alpha = fb_color.a * vertex_color.a;
        if (fb_alpha < minimum_alpha)
        {
            discard;
        }
        fb_color.rgb *= vertex_color.rgb;
        fb_color.a = fb_alpha;
        fb_color.rgb = srgb_to_linear(fb_color.rgb);
        vec3 fb_sunlit;
        vec3 fb_amblit;
        vec3 fb_additive;
        vec3 fb_atten;
        calcAtmosphericVars(pos.xyz, vec3(0), 1.0, fb_sunlit, fb_amblit, fb_additive, fb_atten);
        fb_color.rgb = applySkyAndWaterFog(pos.xyz, fb_additive, fb_atten, fb_color).rgb;
        frag_color = max(fb_color, vec4(0));
        return;
    }
#endif

    float shadow = 1.0f;

#ifdef USE_DIFFUSE_TEX
    vec4 diffuse_tap = texture(diffuseMap,vary_texcoord0.xy);
#endif

#ifdef USE_INDEXED_TEX
    vec4 diffuse_tap = diffuseLookup(vary_texcoord0.xy);
#endif

    vec4 diffuse_srgb = diffuse_tap;

#ifdef FOR_IMPOSTOR
    vec4 color;
    color.rgb = diffuse_srgb.rgb;
    color.a = 1.0;

    float final_alpha = diffuse_srgb.a * vertex_color.a;
    diffuse_srgb.rgb *= vertex_color.rgb;

    // Insure we don't pollute depth with invis pixels in impostor rendering
    //
    if (final_alpha < minimum_alpha)
    {
        discard;
    }

    color.rgb = diffuse_srgb.rgb;
    color.a = final_alpha;

#else // FOR_IMPOSTOR

    vec4 diffuse_linear = vec4(srgb_to_linear(diffuse_srgb.rgb), diffuse_srgb.a);

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir: moon_dir; // TODO -- factor out "sun_up_factor" and just send in the appropriate light vector

    float final_alpha = diffuse_linear.a;

#ifdef IS_AVATAR_SKIN
    if(final_alpha < minimum_alpha)
    {
        discard;
    }
#endif

#ifdef USE_VERTEX_COLOR
    final_alpha *= vertex_color.a;

    if (final_alpha < minimum_alpha)
    { // TODO: figure out how to get invisible faces out of
        // render batches without breaking glow
        discard;
    }

    diffuse_srgb.rgb *= vertex_color.rgb;
    diffuse_linear.rgb = srgb_to_linear(diffuse_srgb.rgb);
#endif // USE_VERTEX_COLOR

    // <FS> the sun shadow lookup (the most expensive part of this shader) comes after the alpha discards above, so the pixels that are thrown away do not pay for it
#ifdef HAS_SUN_SHADOW
    shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
#endif

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 irradiance = amblit;
    // <FS> only the ambient part of sampleReflectionProbesLegacy: this shader uses nothing else it returns (no glossy or legacy environment term), and the rest of that call (the hero probe taps and the
    // screen space reflection march, which it makes for a transparent surface whatever the glossiness) cost 0.7 ms of 1.1 for the static alpha runs of a crowd (rt-debug lap, 2026-11-04). Same value: the legacy
    // call sets the ambient term to amblit, and, outside classic mode, to sampleProbeAmbient of the list preProbeSample makes.
    if (classic_mode == 0)
    {
        preProbeSample(pos.xyz);
        irradiance = sampleProbeAmbient(pos.xyz, norm.xyz, amblit_linear);
    }


    float da = dot(norm.xyz, light_dir.xyz);
          da = clamp(da, -1.0, 1.0);

    float final_da = da;
          final_da = clamp(final_da, 0.0f, 1.0f);

    vec4 color = vec4(0.0);

    color.a   = final_alpha;

    color.rgb = irradiance;
    if (classic_mode > 0)
    {
        final_da = pow(final_da,1.2);
        vec3 sun_contrib = vec3(min(final_da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(final_da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color.rgb *= diffuse_linear.rgb;

    vec4 light = vec4(0,0,0,0);

   #define LIGHT_LOOP(i) light.rgb += calcPointLightOrSpotLight(light_diffuse[i].rgb, diffuse_linear.rgb, pos.xyz, norm, light_position[i], light_direction[i].xyz, light_attenuation[i].x, light_attenuation[i].y, light_attenuation[i].z, light_attenuation[i].w);

    LIGHT_LOOP(1)
    if (alpha_light_count >= 0)
    {
        for (int i = 0; i < alpha_light_count; ++i)
        {
            const vec4 l0 = alpha_lights[i * 3];
            const vec4 l1 = alpha_lights[i * 3 + 1];
            const vec4 l2 = alpha_lights[i * 3 + 2];
            const float fa = l1.w + 1.0;
            light.rgb += calcPointLightOrSpotLight(l1.rgb, diffuse_linear.rgb, pos.xyz, norm, vec4(l0.xyz, 1.0), l2.xyz, (12.0 * fa - 9.0) / l0.w, fa, l2.w, 0.0);
        }
    }
    else
    {
        LIGHT_LOOP(2)
        LIGHT_LOOP(3)
        LIGHT_LOOP(4)
        LIGHT_LOOP(5)
        LIGHT_LOOP(6)
        LIGHT_LOOP(7)
    }

    // sum local light contrib in linear colorspace
    color.rgb += light.rgb;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, color).rgb;

#ifdef ALPHA_PULL_DBGF
    // diagnostics of the scene table alpha path (LL_VK_SCENE_ALPHA_DBGF): the colour is one term of the lighting
#if ALPHA_PULL_DBGF == 1
    color.rgb = vec3(shadow);
#elif ALPHA_PULL_DBGF == 2
    color.rgb = irradiance;
#elif ALPHA_PULL_DBGF == 3
    color.rgb = sunlit_linear;
#elif ALPHA_PULL_DBGF == 4
    color.rgb = vec3(final_da);
#elif ALPHA_PULL_DBGF == 5
    color.rgb = vec3(frag, 0.0);
#elif ALPHA_PULL_DBGF == 6
    color.rgb = norm * 0.5 + 0.5;
#elif ALPHA_PULL_DBGF == 7
    color.rgb = amblit;
#elif ALPHA_PULL_DBGF == 8
    color.rgb = diffuse_linear.rgb;
#endif
    color.a = 1.0;
#endif

#endif // #else // FOR_IMPOSTOR
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
#ifdef IS_HUD
    color.rgb = linear_to_srgb(color.rgb);
    final_scale = 1;
#endif

    color.rgb *= final_scale;
    frag_color = max(color, vec4(0));
}

