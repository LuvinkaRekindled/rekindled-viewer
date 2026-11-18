// alpha_impostor.frag: a transparent surface in the impostor render (the picture an avatar's impostor is made from): the texture times the vertex colour (sRGB, left as it is), the alpha cutoff of the
// draw so that an invisible pixel does not write depth, no lighting, no fog. A translation of "Deferred Alpha Impostor Pull Shader" (deferred/alphaF.glsl with FOR_IMPOSTOR). Pairs with alpha.vert
// (static: no define; rigged: HAS_SKIN + HAS_INDEXED_DRAW).

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);
    const vec4 texel = texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0);
    const float alpha = texel.a * vertex_color.a;
    if (alpha < minimum_alpha)
    {
        discard;
    }
    // (the old program scales the colour by 1.1 in classic mode, here too: the impostor's picture is as bright as the lit ones)
    frag_color = max(vec4(texel.rgb * vertex_color.rgb * (classic_mode > 0 ? 1.1 : 1.0), alpha), vec4(0.0));
}
