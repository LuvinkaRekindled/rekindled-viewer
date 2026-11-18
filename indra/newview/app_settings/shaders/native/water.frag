// water.frag: the water surface seen from above: the picture below (pushed by the ripples), the reflection of the world (the probes, the screen space trace, the hero probe or the planar mirror) mixed in by
// the fresnel terms, and the sun's glint. A translation of the "Water Shader" (class 3 environment/waterF.glsl); pairs with water.vert. Same waves, same fresnel terms, same fade, same reflection (the probes
// without the automatic ones, the water's own screen space trace, the hero probe), same sun term (the glTF specular lobe of a metal, shadowed and dimmed by the air): the picture is the same.
// What is different is the work: the lighting terms the old shader made and the water never used (the ambient light and the haze, the irradiance of the probes) are not made; the plane's frame comes from the
// normal matrix and not from varyings; the second wave map is read only while the first is turning into it (blend_factor above 0; the old shader read both every pixel); the first depth read and position
// of the picture below were used by nothing (the shoreline fade is off) and are gone; the sun term is the one lobe of the metal (no diffuse part to compute), made once.
// The ray traced reflection of the water (rt_water_reflections) is not here: sessions with ray queries keep the old program (manifest: rt).

#define WATER_PASS 1
#define LEGACY_COLOR 1
#ifndef TRANSPARENT_WATER
#define LEGACY_WATER 1
#endif

#include "water_pass.inc"
#include "lib/legacy.glsl"
#include "lib/viewpos.glsl"
#include "lib/atmos.glsl"
#include "lib/pbr_core.glsl"
#include "lib/shadow.glsl"
#include "lib/probe.glsl"
#ifdef RK_RT_QUERY
#include "lib/rt_scene.glsl"
// the water pass sets none of the ray query code's other uniforms (the `uniform` lines of lib/rt.glsl): in the stock water they were 0 for every pixel (one hard sun ray, no avatar self shadow work, no alpha pass limits, no soft hit shadows).
// As constants the code behind them is not in the program (the soft ray loops, the receiver probe, the hair walk): the same picture, a smaller program
const float rt_sun_radius = 0.0;
const int rt_sun_samples = 0;
const float rt_sun_alpha = 0.0;
const float rt_sun_maxdist = 0.0;
const vec2 rt_sun_lod = vec2(0.0);
const float rt_noise_frame = 0.0;
const float rt_sun_strength = 0.0;
const int rt_sun_pen_rays = 0;
const float rt_sun_self_off = 0.0;
const int rt_hit_shadow_variant = 0;
const int rt_hair_soft = 0;
const int rt_hit_shadow_samples = 0;
#include "lib/rt.glsl"
#endif
#ifdef SSR
#include "lib/poisson.glsl"
#include "lib/ssr.glsl"
#endif

layout(location = 0) in vec4 refCoord;
layout(location = 1) in vec4 littleWave;
layout(location = 2) in vec4 view;
layout(location = 3) in vec3 vary_position;
layout(location = 0) out vec4 frag_color;

// the plane's frame in eye space: x, the normal (z up in world) and the cross of the two
vec3 vN, vT, vB;

vec3 transform_normal(vec3 n)
{
    return normalize(n.x * vT + n.y * vB + n.z * vN);
}

// the three wave normals: the big wave, and the two small ones; each is a normal of the first wave map, turning into the second one with blend_factor (a single map: 0, the second is not read)
void generate_waves(out vec3 wave1, out vec3 wave2, out vec3 wave3)
{
    const vec2 big_wave = vec2(refCoord.w, view.w);
    vec3 a1 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], big_wave).xyz;
    vec3 a2 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], littleWave.xy).xyz;
    vec3 a3 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], littleWave.zw).xyz;
    if (blend_factor > 0.0)
    {
        a1 = mix(a1, texture(rk_tbl2d[RK_HM(rk_ti_bumpMap2)], big_wave).xyz, blend_factor);
        a2 = mix(a2, texture(rk_tbl2d[RK_HM(rk_ti_bumpMap2)], littleWave.xy).xyz, blend_factor);
        a3 = mix(a3, texture(rk_tbl2d[RK_HM(rk_ti_bumpMap2)], littleWave.zw).xyz, blend_factor);
    }
    wave1 = a1 * 2.0 - 1.0;
    wave2 = a2 * 2.0 - 1.0;
    wave3 = a3 * 2.0 - 1.0;
}

// what the world looks like in the water: the probes (the automatic ones do not take part), the screen space trace of the water over them where it finds something, the hero probe or the planar mirror
// over both for a smooth enough surface
vec3 water_reflection(vec2 tc, vec3 pos, vec3 norm, float gloss)
{
    probe_gather_water(pos);
    vec3 env = probe_reflection(pos, normalize(reflect(pos, norm)), (1.0 - gloss) * max_probe_lod);
#ifdef SSR
    if (cube_snapshot != 1 && gloss >= 0.9)
    {
        const vec4 ssr = ssr_water(tc, pos, norm, gloss, vec3(0.0, 0.0, 1.0) * env_mat);       // (world up in eye space: a hit on the water itself is no reflection)
        env = mix(env, ssr.rgb, ssr.a);
    }
#endif
#ifdef HERO_PROBES
    env = probe_hero(env, pos, norm, gloss);
#endif
    return env;
}

void main()
{
    vec3 wave1, wave2, wave3;
    generate_waves(wave1, wave2, wave3);

    const vec2 distort = (refCoord.xy / refCoord.z) * 0.5 + 0.5;
    const float water_mask = textureLod(rk_tbl2d[RK_HM(rk_ti_exclusionTex)], distort, 0.0).r;

    if (mirror_flag > 0.0 && dot(vary_position, clipPlane.xyz) + clipPlane.w < 0.0)
    {
        discard;                // (a mirror view throws away what is behind its mirror plane)
    }
#ifndef TRANSPARENT_WATER
    if (water_mask < 1.0)
    {
        discard;                // (no picture below: an exclusion surface cuts the water out)
    }
#endif

    vN = normal_matrix * vec3(0.0, 0.0, 1.0);
    vT = normal_matrix * vec3(1.0, 0.0, 0.0);
    vB = cross(vN, vT);

    const vec3 pos = vary_position;
    const float dist = length(pos);
    const vec3 viewVec = normalize(pos);
    const vec3 light_eye = normal_matrix * lightDir;

    // the waves together, and the fresnel terms: df2.x how much the reflection shows (the three waves' own angles to the camera), df2.y the reflection's strength
    const vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;
    const vec3 view_n = normalize(view.xyz);
    vec3 df3 = max(vec3(0.0), vec3(dot(view_n, wave1), dot(view_n, (wave2 + wave3) * 0.5), dot(view_n, wave3)) * fresnelScale + fresnelOffset);
    df3 *= df3;
    const vec2 df2 = max(vec2(0.0), vec2(df3.x + df3.y + df3.z, dot(view_n, wavef) * fresnelScale + fresnelOffset));

    const vec3 up = normalize(vN);
    const float vdu = clamp(-dot(viewVec, up) * 2.0, 0.0, 1.0);
    vec3 wave_ibl = wavef * normScale;
    wave_ibl.z *= 2.0;
    wave_ibl = transform_normal(wave_ibl);
    const vec3 norm = transform_normal(wavef);

    // the ripples push the picture below; the farther the water, the less
    const vec3 waver = wavef * 3.0;
    const float dmod = sqrt(dist);
    vec2 distort2 = distort + waver.xy * refScale / max(dmod, 1.0) * 2.0;
    distort2 = clamp(distort2, vec2(0.0), vec2(0.999));

    // the sun: what the air above the water leaves of it, and the cascade shadow
    vec3 sunlit, amblit, additive, atten;
    atmos_terms(pos, light_eye, sunlit, amblit, additive, atten);
    if (classic_mode < 1)
    {
        sunlit = srgb_to_linear(sunlit);
    }
    sunlit *= sky_sunlight_scale;
    // (the sun's shadow is looked up below, only where the glint is above the last level of the picture: its one use is the glint)

#ifdef TRANSPARENT_WATER
    // the picture below: where the water is cut out (exclusion mask) the ripples do not push it; where the surface under the water is nearer than the water, they do not either
    float fade = water_mask;
    distort2 = mix(distort, distort2, min(1.0, fade * 10.0));
    const float depth = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], distort2, 0.0).r;
    if (pos.z < rk_view_pos(inv_proj, distort2, depth).z - 0.05)
    {
        distort2 = distort;
    }
    const vec4 fb = textureLod(rk_tbl2d[RK_HM(rk_ti_screenTex)], distort2, 0.0);
#else
    const float fade = 1.0;
    const vec4 fb = applyWaterFogViewLinear(viewVec * 2048.0, vec4(1.0));
#endif

    const float perceptualRoughness = blurMultiplier;
    vec3 radiance = water_reflection(distort2, pos, wave_ibl, 1.0 - perceptualRoughness);

// the reflection is a variant of its own (the manifest define RK_RT_WATER_REFLECTION): the water without it is the sun shadow ray alone, a much smaller program (the setting FSVkRTWater is off by default)
#if defined(RK_RT_QUERY) && defined(SUN_SHADOW) && defined(RK_RT_WATER_REFLECTION)
    if (rt_water_reflections == 2)
    {
        radiance = vec3(1.0, 0.0, 1.0);        // plumbing check: the code runs
    }
    else if (rt_water_reflections != 0)
    {
        // <FS> a ray from the water surface into the ray scene: a hit replaces what the probes and the screen space trace found, a miss keeps it (3: hits green, misses red, upward rays only)
        const vec3 rt_dir = normalize(reflect(pos, wave_ibl));
        if (dot(rt_dir, up) > 0.0)
        {
            const vec4 rt_refl = rtReflection(pos + wave_ibl * 0.05, rt_dir, normalize(transform_normal(lightDir)), sunlit, amblit, distort, radiance);
            if (rt_water_reflections == 3)
            {
                radiance = rt_refl.a > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            }
            else
            {
                radiance = mix(radiance, rt_refl.rgb, rt_refl.a);
            }
        }
        else if (rt_water_reflections == 3)
        {
            radiance = vec3(0.0, 0.0, 1.0);    // the reflection ray points down
        }
    }
#endif

    // the sun's glint: the specular lobe of a metal (no diffuse light) in the sun's colour
    const vec3 specular_linear = srgb_to_linear(specular);
    const vec3 v = -viewVec;
    const vec3 light_dir = transform_normal(lightDir);
    const PbrSurface s = pbr_surface(normalize(norm + up * max(dist, 32.0) / 32.0 * (1.0 - vdu)), v, vec3(0.0), specular_linear, perceptualRoughness);
    float nl;
    const vec3 lobe = pbr_punctual(s, normalize(light_dir), nl);
    // the glint is the lobe in the sun's colour times the shadow (at most 1): where it is already under 1/1024 (a quarter of the last level of an 8 bit picture) the shadow cannot show, so the cascade taps (or the sun ray of a
    // ray traced session) are not made there: they were about half of the water's texture reads (AUDIT-shaders-2026-11-18.md section 4 item 1)
    vec3 punctual = clamp(nl * lobe, vec3(0.0), vec3(10.0)) * sunlit * atten;
    if (max(punctual.r, max(punctual.g, punctual.b)) > (1.0 / 1024.0))
    {
#if defined(RK_RT_QUERY) && defined(SUN_SHADOW)
        const float shadow = rt_sun_shadow(pos, norm, distort);       // (a session with ray queries: the sun shadow of the stock water is a ray, sampleDirectionalShadow of shadowUtil.glsl)
#else
        const float shadow = sun_shadow(pos, norm, distort.y);
#endif
        punctual *= shadow;
    }
    else
    {
        punctual = vec3(0.0);
    }

    radiance *= df2.y;
    vec3 color = mix(fb.rgb, radiance, min(1.0, df2.x)) + punctual;

    // the shore: a short fade, so that the water does not look too soft from afar
    color = mix(fb.rgb, color, min(1.0, fade * 60.0));

    frag_color = min(vec4(1.0), max(vec4(color, 0.0), vec4(0.0)));      // (the alpha of the old shader was min(brightest glint, 0) times the mask: always 0)
}
