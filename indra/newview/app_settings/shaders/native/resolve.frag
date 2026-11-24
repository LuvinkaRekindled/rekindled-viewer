// resolve.frag: the lighting resolve. One full screen pass that turns the G-buffer into the lit colour: for each pixel, by the flag the surface was written with, a glTF (metallic roughness) surface, a legacy
// material surface, or a sky pixel that only gets its colour moved over. Matches the viewer's soften program ("Deferred Soften Shader"; the traced glossy reflection is the RT Reflection pass's, read here as a texture): the sun's shadow and ambient occlusion from the lightmap, the sky's light, the probes (ambient, reflection, the screen space reflection and the hero probe for the glossy), the sun's
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

#define G_TAP(slot) rk_tbl2d[RK_HM(slot)]

// how much of the sky's ambient light the occlusion takes away: occluded places get the sky's light through the occlusion matrix, free ones keep it
vec3 adjust_irradiance(vec3 irradiance, float ambocc)
{
#ifdef HAS_SSAO
    return mix(ssao_effect_mat * min(irradiance * ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance, ambocc);
#else
    return irradiance;                  // (no ambient occlusion: the sky's light is not taken away anywhere)
#endif
}

void main()
{
    const vec2 tc = vary_fragcoord;
    const float depth = texture(G_TAP(rk_ti_depthMap), tc).r;
    const vec4 albedo = texture(G_TAP(rk_ti_diffuseRect), tc);
    const vec4 normal_info = texture(G_TAP(rk_ti_normalMap), tc);
    const float flag = normal_info.w;
#ifdef HAS_EMISSIVE
    const vec3 emissive = texture(G_TAP(rk_ti_emissiveRect), tc).rgb;
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

    const vec3 pos = rk_view_pos(inv_proj, tc, depth);
    const vec3 norm = rk_decode_normal(normal_info.xy);
    // (explicit level 0 from here on: the sky pixels have returned, so the flow differs between neighbouring pixels and an implicit level is undefined; none of these textures has a level chain)
    const vec4 spec = textureLod(G_TAP(rk_ti_specularRect), tc, 0.0);
    // the lightmap (red: the sun's shadow, green: the ambient occlusion) is made by the lighting pass only with sun shadows (SUN_SHADOW) or ambient occlusion (HAS_SSAO), the viewer's global defines;
    // with neither, nothing is read: no shadow takes the sun and nothing is occluded
#if defined(SUN_SHADOW) || defined(HAS_SSAO)
    const vec2 lightmap = textureLod(G_TAP(rk_ti_lightMap), tc, 0.0).rg;
#else
    const vec2 lightmap = vec2(1.0);
#endif
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;
#ifdef SUN_SHADOW
    float scol = max(lightmap.r, albedo.a);            // (the albedo's alpha is a legacy surface's emissive: lit by itself, so no shadow takes it)
#else
    float scol = 1.0;
#endif
#ifdef HAS_SSAO
    const float ambocc = lightmap.g;
#else
    const float ambocc = 1.0;
#endif

    // live debug views (rt-debug.txt resolve_dbg=N, no restart): 2 the lightmap (red: sun shadow, green: occlusion), 3 the albedo, 4 the flag, 5 the normal, 6 the specular, 12 the surface's angle to the light
    if (resolve_dbg != 0)
    {
        if (resolve_dbg == 2) { frag_color = vec4(lightmap.r, lightmap.g, 0.0, 0.0); return; }
        if (resolve_dbg == 3) { frag_color = vec4(albedo.rgb, 0.0); return; }
        if (resolve_dbg == 4) { frag_color = vec4(vec3(flag), 0.0); return; }
        if (resolve_dbg == 5) { frag_color = vec4(norm * 0.5 + 0.5, 0.0); return; }
        if (resolve_dbg == 6) { frag_color = vec4(spec.rgb, 0.0); return; }
        if (resolve_dbg == 12) { frag_color = vec4(vec3(clamp(dot(norm, normalize(light_dir)), 0.0, 1.0)), 0.0); return; }
        if (resolve_dbg == 13) { frag_color = vec4(vec3(depth), 0.0); return; }                                         // the raw depth the lighting reads
        if (resolve_dbg == 14) { frag_color = vec4(vec3(clamp(-pos.z / 40.0, 0.0, 1.0)), 0.0); return; }               // the view distance it makes of it, 0 .. 40 m
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

    // live views of the probes' ambient light (rt-debug.txt resolve_dbg=N, no restart; values above 1 are kept: 4.0 is white, 1.0 a quarter grey, so the 3 times of the probe ambiance shows):
    // 15 the ambient light of the probes (irradiance maps x ambiance, what the lighting gets before the occlusion), 16 the same after the occlusion adjust (what the lighting uses),
    // 17 the sky's own ambient colour (amblit, the part the probes replace), 18 the ambient light of the probes relative to the sky's (probe / sky, 4.0 = white)
    // 19..23: the reflection term of a legacy surface (the gloss environment): 19 what the probes' radiance gives it at its gloss (the sample the lighting takes, 0 to 10, x 0.25), 20 what apply_gloss_env adds to the colour
    // (after the specular colour, the fresnel and the energy balance, x 2, on a grey of half the albedo), 21 the specular colour (linear), 22 the gloss (the specular's alpha), 23 the environment intensity (the normal target's blue)
    // 24, 25, 26: the default probe's RADIANCE cube toward the surface normal at the blurriest mip, at mip 2 and at mip 0 (x 0.25): to set beside view 15, which holds the irradiance map (x the ambiance). The irradiance map is
    // an average of the radiance over the hemisphere, so at the same direction the two should be of about the same size once the ambiance is divided out
    // 27, 28: the spare layer 257 (the default probe's AMBIANCE pass cube kept by RK_VK_AMBPASS_COPY=1) toward the surface normal at mip 0 and at the blurriest mip (x 0.25): the same direction as 26 and 24, so the two
    // passes can be compared as numbers
    // 29: the gloss environment after the hero probe's blend (x 0.25, what the lighting would take with the planar mirror's view mixed in), 30: how much of it the hero probe takes (the weight w, 0 to 1)
    // 31, 32: the nearest probe's own maps toward the surface normal (no mixing, no parallax): 31 its irradiance map (x 3, the ambiance, then x 0.25), 32 its radiance map at the blurriest mip (x 0.25)
    // 33, 34, 35: view 31 with the direction's z (up), y or x flipped: is the cube read upside down or mirrored?
    // 36 / 37: the default probe's radiance (mip 3) straight up / straight down in the cube (a constant colour over the screen: up should be the sky, down the ground), 38 / 39: the same for the nearest probe's irradiance (x 3)
    // 40 .. 45 (needs RK_VK_AMBPASS_COPY=1): the cube's mip 3 straight up, toward +x (the horizon) and straight down: 40 / 41 / 42 the AMBIANCE pass's cube of the default probe (the layer the irradiance maps are made from), 43 / 44 / 45 its RADIANCE pass cube
    // 46 / 47: all six cube directions in six screen columns, left to right +x, -x, +y, -y, +z, -z: 46 the default probe's radiance (mip 3), 47 the nearest probe's irradiance (x 3)
    if (resolve_dbg >= 15 && resolve_dbg <= 47)
    {
        probe_gather(pos);
        const vec3 probe_amb = probe_ambient(pos, norm, amblit);
        vec3 shown = probe_amb;
        float scale = 0.25;
        if (resolve_dbg == 16) { shown = adjust_irradiance(probe_amb, ambocc); }
        if (resolve_dbg == 17) { shown = amblit; }
        if (resolve_dbg == 18) { shown = probe_amb / max(amblit, vec3(1.0e-4)); }
        if (resolve_dbg >= 19)
        {
            const vec4 dbg_spec = vec4(srgb_to_linear(spec.rgb), spec.a);
            const vec3 dbg_reflect = normalize(reflect(pos, norm));
            const vec3 dbg_glossenv = clamp(probe_reflection(pos, dbg_reflect, (1.0 - spec.a) * max_probe_lod), vec3(0.0), vec3(10.0));
            if (resolve_dbg == 19) { shown = dbg_glossenv; }
            if (resolve_dbg == 20)
            {
                const vec3 base = srgb_to_linear(albedo.rgb) * 0.5;
                vec3 lit = base;
                apply_gloss_env(lit, dbg_glossenv, dbg_spec, pos, norm);
                shown = lit - base;
                scale = 2.0;
            }
            if (resolve_dbg == 21) { shown = dbg_spec.rgb; scale = 1.0; }
            if (resolve_dbg == 22) { shown = vec3(dbg_spec.a); scale = 1.0; }
            if (resolve_dbg == 23) { shown = vec3(normal_info.b); scale = 1.0; }
#ifdef HERO_PROBES
            if (resolve_dbg == 29) { shown = probe_hero(dbg_glossenv, pos, norm, spec.a); }
            if (resolve_dbg == 30)
            {
                // mix(0, tex, w) and mix(1, tex, w) differ by 1 - w
                shown = vec3(1.0) - (probe_hero(vec3(1.0), pos, norm, spec.a) - probe_hero(vec3(0.0), pos, norm, spec.a));
                scale = 1.0;
            }
#endif
        }
        if (resolve_dbg == 31 || resolve_dbg == 32)
        {
            const int own = probe_count > 0 ? probe_list[0] : 0;
            if (resolve_dbg == 31)
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_irradianceProbes))], vec4(env_mat * norm, float(refIndex[own].x)), 0.0).rgb * 3.0;
            }
            else
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(env_mat * norm, float(refIndex[own].x)), max_probe_lod).rgb;
            }
        }
        if (resolve_dbg == 46 || resolve_dbg == 47)
        {
            const int col6 = clamp(int(tc.x * 6.0), 0, 5);
            const float sgn = (col6 & 1) == 0 ? 1.0 : -1.0;
            const vec3 dir6 = col6 < 2 ? vec3(sgn, 0.0, 0.0) : (col6 < 4 ? vec3(0.0, sgn, 0.0) : vec3(0.0, 0.0, sgn));
            const int own6 = probe_count > 0 ? probe_list[0] : 0;
            if (resolve_dbg == 46)
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(dir6, float(refIndex[0].x)), 3.0).rgb;
            }
            else
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_irradianceProbes))], vec4(dir6, float(refIndex[own6].x)), 0.0).rgb * 3.0;
            }
            // a marker strip along the bottom of the screen (tc.y < 0.06 or > 0.94, whichever side the screen puts the top on): one colour per column, so the six columns can be seen whatever the cube returns
            if (tc.y < 0.06 || tc.y > 0.94)
            {
                const vec3 marker[6] = vec3[6](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(1.0, 1.0, 0.0));
                shown = marker[col6] * 4.0;
            }
        }
        if (resolve_dbg >= 40 && resolve_dbg <= 45)
        {
            const int k = (resolve_dbg - 40) % 3;
            const vec3 dir_k = k == 0 ? vec3(0.0, 0.0, 1.0) : (k == 1 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, -1.0));
            const float layer = resolve_dbg <= 42 ? 257.0 : float(refIndex[0].x);
            shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(dir_k, layer), 3.0).rgb;
        }
        if (resolve_dbg >= 36 && resolve_dbg <= 39)
        {
            const int own_u = probe_count > 0 ? probe_list[0] : 0;
            const vec3 dir_ud = (resolve_dbg & 1) == 0 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
            if (resolve_dbg <= 37)
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(dir_ud, float(refIndex[0].x)), 3.0).rgb;
            }
            else
            {
                shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_irradianceProbes))], vec4(dir_ud, float(refIndex[own_u].x)), 0.0).rgb * 3.0;
            }
        }
        if (resolve_dbg >= 33 && resolve_dbg <= 35)
        {
            const int own_f = probe_count > 0 ? probe_list[0] : 0;
            vec3 d = env_mat * norm;
            if (resolve_dbg == 33) { d.z = -d.z; }
            else if (resolve_dbg == 34) { d.y = -d.y; }
            else { d.x = -d.x; }
            shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_irradianceProbes))], vec4(d, float(refIndex[own_f].x)), 0.0).rgb * 3.0;
        }
        if (resolve_dbg >= 24 && resolve_dbg <= 26)
        {
            const float cube_lod = resolve_dbg == 24 ? max_probe_lod : (resolve_dbg == 25 ? 2.0 : 0.0);
            shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(env_mat * norm, float(refIndex[0].x)), cube_lod).rgb;
        }
        if (resolve_dbg == 27 || resolve_dbg == 28)
        {
            shown = textureLod(rk_tblcubearray[nonuniformEXT(RK_HM(rk_ti_cubearray_reflectionProbes))], vec4(env_mat * norm, 257.0), resolve_dbg == 28 ? max_probe_lod : 0.0).rgb;
        }
        frag_color = vec4(shown * scale, 0.0);
        return;
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
    int dbg_trace = 0;                  // rt_glossy_reflections 3, 4, 7, 8: 1 a ray was traced and missed, 2 it hit
    vec3 dbg_rt_color = vec3(0.0);

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
        if (rt_glossy_reflections != 0)
        {
            // the traced reflection of the RT Reflection pass replaces the probe result where it hit
            const vec4 rt_refl = textureLod(G_TAP(rk_ti_rtReflMap), tc, 0.0);
            if (rt_glossy_reflections >= 3)
            {
                dbg_trace = rt_refl.a == 0.0 ? 0 : (rt_refl.a > 0.5 ? 2 : 1);
                dbg_rt_color = rt_refl.rgb;
            }
            else
            {
                radiance = mix(radiance, rt_refl.rgb, max(rt_refl.a, 0.0));
            }
        }
#ifdef HERO_PROBES
        // after the traced reflection: the glass of a mirror shows the planar view (the avatar as the main view draws it: its hair, tints and translucent layers come from the real alpha pass), not the ray scene's version
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
        // TEST 2026-11-24, live ablation (rt-debug.txt resolve_dbg = 100 + mask): 1 no gloss environment, 2 no sun specular lobe, 4 no screen space / traced reflection, 8 no hero probe, 16 no legacy environment, 32 the sky's ambient instead of the probes'
        const int abl = resolve_dbg >= 100 ? resolve_dbg - 100 : 0;
        const float env = (abl & 16) != 0 ? 0.0 : normal_info.b;

        vec3 irradiance = amblit;
        if (classic_mode == 0 && (abl & 32) == 0)
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
        if (cube_snapshot != 1 && (abl & 4) == 0)
        {
            const vec4 ssr = ssr_opaque(tc, pos, norm, glossiness);
            glossenv = mix(glossenv, ssr.rgb, ssr.a);
            legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
        }
#endif
        if (rt_glossy_reflections != 0 && (abl & 4) == 0)
        {
            const vec4 rt_refl = textureLod(G_TAP(rk_ti_rtReflMap), tc, 0.0);
            if (rt_glossy_reflections >= 3)
            {
                dbg_trace = rt_refl.a == 0.0 ? 0 : (rt_refl.a > 0.5 ? 2 : 1);
                dbg_rt_color = rt_refl.rgb;
            }
            else
            {
                glossenv = mix(glossenv, rt_refl.rgb, max(rt_refl.a, 0.0));
            }
        }
#ifdef HERO_PROBES
        if ((abl & 8) == 0)
        {
            glossenv = probe_hero(glossenv, pos, norm, glossiness);       // (after the traced reflection: the glass of a mirror shows the planar view)
            if (env > 0.0)
            {
                legacyenv = probe_hero(legacyenv, pos, norm, 1.0);
            }
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
            if (nl > 0.0 && nh > 0.0 && (abl & 2) == 0)
            {
                const float lit = min(nl * 6.0, 1.0);
                const float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
                const float gtdenom = 2.0 * nh;
                const float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
                const float lobe = scol * fres * textureLod(G_TAP(rk_ti_lightFunc), vec2(nh, glossiness), 0.0).r * gt / (nh * nl);
                color += lit * lobe * sun_light * specular.rgb;
            }
            if ((abl & 1) == 0)
            {
                apply_gloss_env(color, glossenv, specular, pos, norm);
            }
        }

        color = mix(color, diffuse, albedo.a);
        if (env > 0.0)
        {
            apply_legacy_env(color, legacyenv, pos, norm, env);
        }
    }

    // debug views of the traced reflection (the setting glossyrt): 3 and 4 green where the ray hit, red where it missed, 7 the colour it found, 8 the surface's own colour where it traced
    if (rt_glossy_reflections == 3 || rt_glossy_reflections == 4)
    {
        color = dbg_trace == 0 ? color : (dbg_trace == 2 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0));
    }
    else if (rt_glossy_reflections == 7)
    {
        color = dbg_trace == 2 ? dbg_rt_color : color;
    }
    else if (rt_glossy_reflections == 8)
    {
        color = dbg_trace != 0 ? albedo.rgb : color;
    }
    // (the old shader's output: linear, and a safety range so that one bad value cannot spread through the glow and the exposure)
    color *= classic_mode > 0 ? 1.1 : 1.0;
    color = mix(color, vec3(1.0), isinf(color));
    color = mix(color, vec3(0.0), isnan(color));
    frag_color = vec4(clamp(color, vec3(0.0), vec3(11.2)), 0.0);
}
