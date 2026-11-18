// alpha_pbr.frag: a glTF (metallic roughness) blended transparent surface, lit. The base colour times the vertex colour, the normal map, the occlusion / roughness / metallic map and the emissive map, the
// sun with its cascade shadow, the image based light from the reflection probes (with the screen space reflection and the hero probe for the glossiest), every local light that reaches it (the view's
// light list, the same lights the opaque surfaces get), then the air or water between it and the eye. Matches the viewer's glTF blend program ("Deferred PBR Alpha Pull Shader") including classic mode's own
// mixing. Pairs with alpha_pbr.vert (ALPHA_LIT, ALPHA_PROBES, ALPHA_PBR).

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#include "lib/atmos.glsl"
#include "lib/fog.glsl"
#include "lib/shadow.glsl"
#include "lib/shadow_alpha.glsl"
#include "lib/probe.glsl"
#if defined(SSR)
#include "lib/viewpos.glsl"
#include "lib/ssr.glsl"
#endif
#include "lib/pbr.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_tc_base;
layout(location = 3) in vec2 vary_tc_normal;
layout(location = 4) in vec2 vary_tc_orm;
layout(location = 5) in vec2 vary_tc_emissive;
layout(location = 6) in vec3 vary_normal;
layout(location = 7) in vec3 vary_tangent;
layout(location = 8) flat in float vary_sign;
layout(location = 9) flat in uvec4 vary_slots;
layout(location = 10) flat in vec2 vary_factors;
layout(location = 11) flat in vec3 vary_emissive;

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);

    const vec4 base = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.x))], vary_tc_base);
    const vec3 color_factor = vertex_color.rgb * srgb_to_linear(base.rgb);      // (the vertex colour is not converted: the viewer's glTF blend does not either)

    // the normal map (a draw without one has slot 0: the flat normal)
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (vary_slots.y != 0u)
    {
        tangent_normal = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.y))], vary_tc_normal).xyz * 2.0 - 1.0;
    }
    const vec3 binormal = vary_sign * cross(vary_normal, vary_tangent);
    vec3 norm = normalize(tangent_normal.x * vary_tangent + tangent_normal.y * binormal + tangent_normal.z * vary_normal);
    norm *= gl_FrontFacing ? 1.0 : -1.0;

    const vec3 pos = vary_position;
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;
    const float scol = sun_shadow_alpha(pos, norm, gl_FragCoord.xy / screen_res);

    vec3 sunlit, amblit, additive, atten;
    atmos_terms_lit(pos, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }

    // occlusion (r), roughness (g), metallic (b); the emissive map is sRGB, its factor linear
    const vec3 orm = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.z))], vary_tc_orm).rgb;
    const float roughness = orm.g * vary_factors.x;
    const float metallic = orm.b * vary_factors.y;
    // (no emissive colour, no emissive: the map is not read then; the lookup is in a branch, so it takes the derivatives from outside it)
    const vec2 emissive_dx = dFdx(vary_tc_emissive), emissive_dy = dFdy(vary_tc_emissive);
    vec3 emissive = vec3(0.0);
    if (vary_emissive != vec3(0.0))
    {
        emissive = vary_emissive * srgb_to_linear(textureGrad(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.w))], vary_tc_emissive, emissive_dx, emissive_dy).rgb);
    }

    // the image based light: the ambient (other skies; a classic sky's own ambient light is kept) and the reflection of the world, blurred by the roughness
    const float gloss = 1.0 - roughness;
    probe_gather(pos);
    vec3 irradiance = amblit;
    if (classic_mode == 0)
    {
        irradiance = probe_ambient(pos, norm, amblit);
    }
    vec3 radiance = probe_reflection(pos, normalize(reflect(pos, norm)), roughness * max_probe_lod);
#if defined(SSR)
    if (cube_snapshot != 1 && gloss >= 0.9)
    {
        const vec4 ssr = ssr_transparent(pos.xy * 0.5 + 0.5, pos, norm, gloss);
        radiance = mix(radiance, ssr.rgb, ssr.a);
    }
#endif
#ifdef HERO_PROBES
    radiance = probe_hero(radiance, pos, norm, gloss);
#endif

    const vec3 f0 = vec3(0.04);
    const vec3 diffuse_color = color_factor * (vec3(1.0) - f0) * (1.0 - metallic);
    const vec3 specular_color = mix(f0, color_factor, metallic);
    const PbrSurface surface = pbr_surface(norm, -normalize(pos), diffuse_color, specular_color, roughness);

    vec3 color = pbr_base_light(surface, roughness, light_dir, sunlit, scol, radiance, irradiance, emissive, orm.r);
    color += pbr_lights_sum(surface, pos);
    color = apply_sky_and_water_fog(pos, additive, atten, color);

    frag_color = max(vec4(color * (classic_mode > 0 ? 1.1 : 1.0), base.a * vertex_color.a), vec4(0.0));
}
