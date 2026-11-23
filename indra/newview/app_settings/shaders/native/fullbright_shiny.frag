// fullbright_shiny.frag: a fullbright shiny surface of the opaque pass (the "shiny" setting of a face: the texture times the vertex colour, lit by nothing, with the reflection of the world mixed in by the face's
// environment intensity, which travels in the vertex colour's alpha). Matches "Deferred Fullbright Shiny Pull Shader" and its rigged twin (deferred/fullbrightShinyF.glsl with IS_SHINY_PULL); with IS_HUD
// "HUD Fullbright Shiny Pull Shader" (no linear conversion and no environment: a HUD is not lit by the world). Pairs with gbuffer.vert (the eye space normal is its vary_normal). The manifest line of the
// world's variants has ALPHA_PROBES (the probe blocks of alpha_pass.inc).
// What the viewer's program did and this one does not: it sampled the probes for every pixel (the ambient light of the probes, a screen space reflection trace with glossiness 0 whose answer is always
// nothing, the hero probe for a gloss that is not there) and only used the legacy environment term, which is nothing at an intensity of 0. Here the probes are asked for the legacy reflection only, and only
// where the intensity is above 0.

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#ifndef IS_HUD
#include "lib/probe.glsl"
#include "lib/glossenv.glsl"
#endif

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_normal;

#ifdef GB_MOTION
layout(location = 14) in vec4 vary_cur;
layout(location = 15) in vec4 vary_prev;
#endif

layout(location = 0) out vec4 frag_color;
#include "lib/cover.glsl"

void main()
{
    mirror_clip(vary_position);
    vec4 color = texture(rk_tbl2d[nonuniformEXT(RK_HM(vary_slot))], vary_texcoord0);
    color.rgb *= vertex_color.rgb;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
    const float env_intensity = vertex_color.a;
    if (env_intensity > 0.0)
    {
        const vec3 norm = normalize(vary_normal);
        probe_gather(vary_position);
        vec3 legacyenv = probe_reflection(vary_position, normalize(reflect(vary_position, norm)), 0.0);
#ifdef HERO_PROBES
        legacyenv = probe_hero(legacyenv, vary_position, norm, 1.0);
#endif
        apply_legacy_env(color.rgb, legacyenv, vary_position, norm, env_intensity);
    }
#endif
    color.a = 1.0;
    frag_color = max(color, vec4(0.0));
    rk_write_cover(frag_color.a, 0.0);
#ifdef GB_MOTION
    rk_write_motion(frag_color.a, vary_cur.w > 0.0 && vary_prev.w > 0.0, (vary_prev.xy / vary_prev.w - vary_cur.xy / vary_cur.w) * 0.5 * render_res);
#endif
}
