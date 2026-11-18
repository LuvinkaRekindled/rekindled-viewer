// pbr_glow.frag: the glow second draw of a glTF (PBR) blend surface: only the alpha channel is written (the glow amount): the largest channel of the emissive colour factor times the emissive texture (sRGB, to
// linear) times the emissive array's alpha. Matches "PBR Glow Pull Shader" and its rigged twin (deferred/pbrglowF.glsl with pbrGlowPullV.glsl); pairs with alpha_pbr.vert (ALPHA_PBR, ALPHA_PBR_HUD,
// GLOW_EMISSIVE, the rigged line HAS_SKIN + HAS_INDEXED_DRAW). Same discard as the viewer's: the base colour texture's alpha (without the vertex colour) under the material's cutoff.
// What the viewer's program did and this one does not: it fetched the base colour texture for every pixel, though a cutoff of 0 (every blend and opaque material) can never discard, and the emissive texture
// even where the factor or the array's alpha made the answer 0.

#include "alpha_pass.inc"
#include "lib/color.glsl"

layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_tc_base;
layout(location = 5) in vec2 vary_tc_emissive;
layout(location = 9) flat in uvec4 vary_slots;
layout(location = 11) flat in vec3 vary_emissive;
layout(location = 12) flat in float vary_cutoff;

layout(location = 0) out vec4 frag_color;

void main()
{
    if (vary_cutoff > 0.0 && texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slots.x))], vary_tc_base).a < vary_cutoff)
    {
        discard;
    }

    float lum = 0.0;
    if (vertex_color.a > 0.0 && max(max(vary_emissive.r, vary_emissive.g), vary_emissive.b) > 0.0)
    {
        const vec3 emissive = vary_emissive * srgb_to_linear(texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slots.w))], vary_tc_emissive).rgb);
        lum = max(max(emissive.r, emissive.g), emissive.b) * vertex_color.a;
    }
    // NOTE: when this shader is used, only alpha is being written to
    frag_color = vec4(0.0, 0.0, 0.0, lum);
}
