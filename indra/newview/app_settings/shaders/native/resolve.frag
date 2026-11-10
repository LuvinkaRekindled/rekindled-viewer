// resolve.frag: the lighting resolve. One full screen pass that turns the G-buffer into the lit colour: for each pixel, by the flag the surface was written with, a glTF (metallic roughness) surface, a legacy
// material surface, or a sky pixel that only gets its colour moved over. Matches the viewer's soften program without ray tracing ("Deferred Soften Shader": the ray traced variants stay with the old
// program): the sun's shadow and ambient occlusion from the lightmap, the sky's light, the probes (ambient, reflection, the screen space reflection and the hero probe for the glossy), the sun's
// highlight. Local lights are added by their own passes afterwards, and the haze comes after those: nothing of that is here.
// What is different is the cost: a sky pixel reads nothing but its colour, the view position comes from the depth with the few multiplications the projection leaves (lib/viewpos.glsl), and the pixel
// runs only the branch of its own kind.

// RESOLVE_DBG: a bisect switch (0 = the program; 1 = classes in colour, 2 = lightmap, 3 = albedo, 4 = flag, 5 = normal, 6 = specular, 7 = light direction, 8 = sunlit, 9 = amblit, 10 = sun up / classic flags, 11 = probe ambient, 12 = n.l, 13 = probe counts); resolve-bisect.ps1 in the work directory rewrites it while a lap runs
#define RESOLVE_DBG 0

#include "resolve_pass.inc"
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"
#include "lib/color.glsl"
#include "lib/atmos.glsl"
#include "lib/probe.glsl"
#ifdef SSR
#include "lib/poisson.glsl"
#include "lib/ssr.glsl"
#endif
#include "lib/pbr.glsl"
#include "lib/glossenv.glsl"

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

// the flags of the normal buffer's last channel (the writers: GBUFFER_FLAG_* of the viewer's shaders)
const float FLAG_SKIP_ATMOS = 0.0;      // the WL sky: its colour is the albedo
const float FLAG_HAS_ATMOS = 0.34;      // a legacy surface
const float FLAG_HAS_PBR = 0.67;        // a glTF surface
const float FLAG_HAS_HDRI = 1.0;        // a HDRI sky: its colour is the emissive target

bool is_flag(float data, float flag)
{
    return abs(data - flag) < 0.1;
}

#define G_TAP(slot) ll_tbl2d[LL_HM(slot)]

// how much of the sky's ambient light the occlusion takes away: occluded places get the sky's light through the occlusion matrix, free ones keep it
vec3 adjust_irradiance(vec3 irradiance, float ambocc)
{
    return mix(ssao_effect_mat * min(irradiance * ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance, ambocc);
}

void main()
{
    const vec2 tc = vary_fragcoord;
    const float depth = texture(G_TAP(ll_ti_depthMap), tc).r;
    const vec4 albedo = texture(G_TAP(ll_ti_diffuseRect), tc);
    const vec4 normal_info = texture(G_TAP(ll_ti_normalMap), tc);
    const float flag = normal_info.w;
#ifdef HAS_EMISSIVE
    const vec3 emissive = texture(G_TAP(ll_ti_emissiveRect), tc).rgb;
#else
    const vec3 emissive = vec3(0.0);
#endif

    // sky pixels: nothing to light, the colour moves over (the late drawn sky writes it into the albedo)
    if (is_flag(flag, FLAG_HAS_HDRI) && !is_flag(flag, FLAG_HAS_PBR))
    {
        frag_color = vec4(clamp(emissive * (classic_mode > 0 ? 1.1 : 1.0), vec3(0.0), vec3(11.2)), 0.0);
        return;
    }
    if (is_flag(flag, FLAG_SKIP_ATMOS))
    {
#ifdef HAS_EMISSIVE
        vec3 sky = emissive;
#else
        vec3 sky = albedo.rgb;
#endif
        sky = srgb_to_linear(sky) * sky_hdr_scale;
        frag_color = vec4(clamp(sky * (classic_mode > 0 ? 1.1 : 1.0), vec3(0.0), vec3(11.2)), 0.0);
        return;
    }

    const vec3 pos = ll_view_pos(inv_proj, tc, depth);
    const vec3 norm = ll_decode_normal(normal_info.xy);
    // (explicit level 0 from here on: the sky pixels have returned, so the flow differs between neighbouring pixels and an implicit level is undefined; none of these textures has a level chain)
    const vec4 spec = textureLod(G_TAP(ll_ti_specularRect), tc, 0.0);
    const vec2 lightmap = textureLod(G_TAP(ll_ti_lightMap), tc, 0.0).rg;
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;
    float scol = max(lightmap.r, albedo.a);            // (the albedo's alpha is a legacy surface's emissive: lit by itself, so no shadow takes it)
    const float ambocc = lightmap.g;

    // live debug views (rt-debug.txt resolve_dbg=N, no restart): 2 the lightmap (red: sun shadow, green: occlusion), 3 the albedo, 4 the flag, 5 the normal, 6 the specular, 12 the surface's angle to the light
    if (resolve_dbg != 0)
    {
        if (resolve_dbg == 2) { frag_color = vec4(lightmap.r, lightmap.g, 0.0, 0.0); return; }
        if (resolve_dbg == 3) { frag_color = vec4(albedo.rgb, 0.0); return; }
        if (resolve_dbg == 4) { frag_color = vec4(vec3(flag), 0.0); return; }
        if (resolve_dbg == 5) { frag_color = vec4(norm * 0.5 + 0.5, 0.0); return; }
        if (resolve_dbg == 6) { frag_color = vec4(spec.rgb, 0.0); return; }
        if (resolve_dbg == 12) { frag_color = vec4(vec3(clamp(dot(norm, normalize(light_dir)), 0.0, 1.0)), 0.0); return; }
    }

#if RESOLVE_DBG == 1
    frag_color = vec4(is_flag(flag, FLAG_HAS_PBR) ? vec3(1.0, 0.0, 1.0) : (is_flag(flag, FLAG_HAS_ATMOS) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 1.0, 0.0)), 0.0);
    return;
#elif RESOLVE_DBG == 2
    frag_color = vec4(lightmap.r, lightmap.g, 0.0, 0.0);
    return;
#elif RESOLVE_DBG == 3
    frag_color = vec4(albedo.rgb, 0.0);
    return;
#elif RESOLVE_DBG == 4
    frag_color = vec4(vec3(flag), 0.0);
    return;
#elif RESOLVE_DBG == 5
    frag_color = vec4(norm * 0.5 + 0.5, 0.0);
    return;
#elif RESOLVE_DBG == 6
    frag_color = vec4(spec.rgb, 0.0);
    return;
#endif
    vec3 sunlit, amblit, additive, atten;
    atmos_terms_lit(pos, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }

#if RESOLVE_DBG == 7
    frag_color = vec4(normalize(light_dir) * 0.5 + 0.5, 0.0);           // the light direction (eye space)
    return;
#elif RESOLVE_DBG == 8
    frag_color = vec4(clamp(sunlit, vec3(0.0), vec3(1.0)), 0.0);        // the sun's colour after the air
    return;
#elif RESOLVE_DBG == 9
    frag_color = vec4(clamp(amblit, vec3(0.0), vec3(1.0)), 0.0);        // the sky's ambient colour
    return;
#elif RESOLVE_DBG == 10
    frag_color = vec4(sun_up_factor == 1 ? 1.0 : 0.0, classic_mode > 0 ? 1.0 : 0.0, 0.0, 0.0);      // red: sun up, green: classic sky
    return;
#elif RESOLVE_DBG == 11
    probe_gather(pos);
    frag_color = vec4(clamp(probe_ambient(pos, norm, amblit), vec3(0.0), vec3(1.0)), 0.0);        // the probes' ambient light
    return;
#elif RESOLVE_DBG == 12
    frag_color = vec4(vec3(clamp(dot(norm, normalize(light_dir)), 0.0, 1.0)), 0.0);              // the angle of the surface to the light
    return;
#elif RESOLVE_DBG == 14
    frag_color = vec4(sun_dir * 0.5 + 0.5, 0.0);                                    // the raw sun_dir uniform as a colour
    return;
#elif RESOLVE_DBG == 15
    frag_color = vec4(length(sun_dir) * 0.5, length(moon_dir) * 0.5, 0.0, 0.0);      // the lengths of sun_dir and moon_dir (a unit vector gives 0.5)
    return;
#elif RESOLVE_DBG == 13
    probe_gather(pos);
    frag_color = vec4(float(refmapCount) / 32.0, float(probe_count) / 8.0, probe_automatic ? 1.0 : 0.0, 0.0);      // probes in the frame, probes that reach the pixel, automatic probes on
    return;
#endif
    probe_gather(pos);
    const vec3 v = -normalize(pos);
    const vec3 reflect_dir = normalize(reflect(pos, norm));
    vec3 color;

    if (is_flag(flag, FLAG_HAS_PBR))
    {
        const float roughness = spec.g;
        const float metallic = spec.b;
        const float gloss = 1.0 - roughness;

        vec3 irradiance = amblit;
        if (classic_mode == 0)
        {
            irradiance = probe_ambient(pos, norm, amblit);
        }
        vec3 radiance = probe_reflection(pos, reflect_dir, roughness * max_probe_lod);
#ifdef SSR
        if (cube_snapshot != 1 && gloss >= 0.9)
        {
            const vec4 ssr = ssr_opaque(tc, pos, norm, gloss);
            radiance = mix(radiance, ssr.rgb, ssr.a);
        }
#endif
#ifdef HERO_PROBES
        radiance = probe_hero(radiance, pos, norm, gloss);
#endif
        irradiance = adjust_irradiance(irradiance, ambocc);

        const vec3 f0 = vec3(0.04);
        const vec3 diffuse_color = albedo.rgb * (vec3(1.0) - f0) * (1.0 - metallic);
        const vec3 specular_color = mix(f0, albedo.rgb, metallic);
        const PbrSurface surface = pbr_surface(norm, v, diffuse_color, specular_color, roughness);
        color = pbr_base_light(surface, roughness, light_dir, sunlit, scol, radiance, irradiance, emissive, spec.r);
    }
    else
    {
        // a legacy material surface (the G-buffer holds sRGB values for it)
        const vec3 diffuse = srgb_to_linear(albedo.rgb);
        vec4 specular = vec4(srgb_to_linear(spec.rgb), spec.a);
        const float glossiness = specular.a;
        const float env = normal_info.b;

        vec3 irradiance = amblit;
        if (classic_mode == 0)
        {
            irradiance = probe_ambient(pos, norm, amblit);
        }
        vec3 glossenv = vec3(0.0);
        vec3 legacyenv = vec3(0.0);
        if (glossiness > 0.0)
        {
            glossenv = probe_reflection(pos, reflect_dir, (1.0 - glossiness) * max_probe_lod);
        }
        if (env > 0.0)
        {
            legacyenv = probe_reflection(pos, reflect_dir, 0.0);
        }
#ifdef SSR
        if (cube_snapshot != 1)
        {
            const vec4 ssr = ssr_opaque(tc, pos, norm, glossiness);
            glossenv = mix(glossenv, ssr.rgb, ssr.a);
            legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
        }
#endif
#ifdef HERO_PROBES
        glossenv = probe_hero(glossenv, pos, norm, glossiness);
        if (env > 0.0)
        {
            legacyenv = probe_hero(legacyenv, pos, norm, 1.0);
        }
#endif
        glossenv = clamp(glossenv, vec3(0.0), vec3(10.0));
        irradiance = adjust_irradiance(irradiance, ambocc);

        float da = clamp(dot(norm, light_dir), 0.0, 1.0);
        color = irradiance;
        vec3 sun_light = sunlit;
        if (classic_mode > 0)
        {
            da = pow(da, 1.2);
            color = srgb_to_linear(color * 0.9 + linear_to_srgb(vec3(min(da, scol))) * sunlit * 0.7);
            sun_light = srgb_to_linear(sunlit);
        }
        else
        {
            color += min(da, scol) * sunlit;
        }
        color *= diffuse;

        if (glossiness > 0.0)
        {
            // the sun's specular lobe
            const vec3 l = normalize(light_dir);
            const vec3 h = normalize(l + v);
            const float eps = 0.000001;
            const float nh = clamp(dot(norm, h), eps, 1.0);
            const float nl = clamp(dot(norm, l), eps, 1.0);
            const float nv = clamp(dot(norm, v), eps, 1.0);
            const float vh = clamp(dot(v, h), eps, 1.0);
            if (nl > 0.0 && nh > 0.0)
            {
                const float lit = min(nl * 6.0, 1.0);
                const float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
                const float gtdenom = 2.0 * nh;
                const float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
                const float lobe = scol * fres * textureLod(G_TAP(ll_ti_lightFunc), vec2(nh, glossiness), 0.0).r * gt / (nh * nl);
                color += lit * lobe * sun_light * specular.rgb;
            }
            apply_gloss_env(color, glossenv, specular, pos, norm);
        }

        color = mix(color, diffuse, albedo.a);
        if (env > 0.0)
        {
            apply_legacy_env(color, legacyenv, pos, norm, env);
        }
    }

    // (the old shader's output: linear, and a safety range so that one bad value cannot spread through the glow and the exposure)
    color *= classic_mode > 0 ? 1.1 : 1.0;
    color = mix(color, vec3(1.0), isinf(color));
    color = mix(color, vec3(0.0), isnan(color));
    frag_color = vec4(clamp(color, vec3(0.0), vec3(11.2)), 0.0);
}
