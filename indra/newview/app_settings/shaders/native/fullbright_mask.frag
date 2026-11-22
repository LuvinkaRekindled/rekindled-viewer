// NO_MASK: the opaque fullbright pass ("Deferred Fullbright Pull Shader" and its rigged twin, "HUD Fullbright Pull Shader": deferred/fullbrightF.glsl without HAS_ALPHA_MASK): the same colour, no cutoff.
// fullbright_mask.frag: a fullbright alpha masked surface: the texture times the vertex colour, discarded below the draw's cutoff, converted to linear (not for the HUD). A line for line translation of the
// "... Fullbright Alpha Mask Pull Shader" programs (deferred/fullbrightF.glsl with HAS_ALPHA_MASK, no IS_ALPHA): the cutoff of the draw comes from the scene table's vertex shader (gbuffer.vert, the flat
// varying; the programs with RK_MASK_CUTOFF) or from the uniform minimum_alpha (alpha.vert, CUTOFF_UNIFORM: the depth only passes of the alpha runs). Define IS_HUD: no linear conversion.

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
#if !defined(CUTOFF_UNIFORM) && !defined(NO_MASK)
layout(location = 5) flat in float vary_cutoff;
#endif

layout(location = 0) out vec4 frag_color;
#include "lib/cover.glsl"

void main()
{
    mirror_clip(vary_position);

    vec4 color = texture(rk_tbl2d[nonuniformEXT(RK_HM(vary_slot))], vary_texcoord0.xy);

    float final_alpha = color.a * vertex_color.a;

    // <FS> final alpha (with the vertex colour's): the depth only pass of the rigged alpha must agree with the colour shaders about what is above its cutoff
#ifndef NO_MASK
#ifdef CUTOFF_UNIFORM
    if (final_alpha < minimum_alpha)
#else
    if (final_alpha < vary_cutoff)
#endif
    {
        discard;
    }
#endif

    color.rgb *= vertex_color.rgb;

    color.a = final_alpha;

#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#endif

    frag_color = max(color, vec4(0));
    rk_write_cover(frag_color.a);
}
