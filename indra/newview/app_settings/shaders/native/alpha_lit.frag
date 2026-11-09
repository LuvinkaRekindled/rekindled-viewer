// alpha_lit.frag: a lit transparent surface (plain diffuse: no material maps). Texture times vertex colour, the ambient sky and the sun with its cascade shadow, every local light that reaches it
// (the view's light list, the same lights the opaque surfaces get), then the air or water between it and the eye. Matches the viewer's lit alpha program ("Deferred Alpha Pull Shader") including classic
// mode's own mixing; classic skies only for now (the probe ambient of the other skies is not written yet: the manifest line says `when classic_mode=1`).
// Pairs with alpha.vert (ALPHA_LIT).

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#include "lib/atmos.glsl"
#include "lib/fog.glsl"
#include "lib/shade.glsl"
#include "lib/shadow.glsl"
#include "lib/light.glsl"
#ifdef ALPHA_PROBES
#include "lib/probe.glsl"
#endif

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_norm;
#ifdef ALPHA_UBER
layout(location = 5) flat in uint vary_fullbright;      // the row's fullbright flag: the lit and the fullbright draws of the blend order are one run (one program)
#endif

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);

    const vec4 texel = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0);
#ifdef ALPHA_UBER
    if (vary_fullbright != 0u)
    {
        frag_color = shade_fullbright(vary_position, texel, vertex_color);
        return;
    }
#endif
    const float alpha = texel.a * vertex_color.a;
    if (alpha < minimum_alpha)
    {
        discard;
    }
    const vec3 diffuse = srgb_to_linear(texel.rgb * vertex_color.rgb);

    const vec3 pos = vary_position;
    const vec3 norm = vary_norm;
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;

    // the cascade lookup comes after the alpha discard: pixels that are thrown away do not pay for it
    const float shadow = sun_shadow(pos, norm, gl_FragCoord.y / screen_res.y);

    vec3 sunlit, amblit, additive, atten;
    atmos_terms_lit(pos, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }

    const float ndl = clamp(dot(norm, light_dir), 0.0, 1.0);
    vec3 color;
    if (classic_mode > 0)
    {
        // classic mode mixes the sky and the sun in sRGB, then goes back to linear
        const vec3 sun_contrib = vec3(min(pow(ndl, 1.2), shadow));
        color = srgb_to_linear(amblit * 0.9 + linear_to_srgb(sun_contrib) * sunlit * 0.7);
    }
    else
    {
        // the ambient light of the other skies comes from the reflection probes
        vec3 irradiance = amblit;
#ifdef ALPHA_PROBES
        probe_gather(pos);
        irradiance = probe_ambient(pos, norm, amblit);
#endif
        color = irradiance + min(ndl, shadow) * sunlit;
    }
    color *= diffuse;
    color += alpha_lights_sum(diffuse, pos, norm);
    color = apply_sky_and_water_fog(pos, additive, atten, color);

    frag_color = max(vec4(color * (classic_mode > 0 ? 1.1 : 1.0), alpha), vec4(0.0));
}
