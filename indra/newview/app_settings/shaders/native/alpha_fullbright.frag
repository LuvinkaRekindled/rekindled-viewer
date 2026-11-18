// alpha_fullbright.frag: a fullbright transparent surface: the texture times the vertex colour, no lighting, then the air (or water) between it and the eye. Matches the viewer's fullbright alpha
// program ("Deferred Fullbright Alpha Pull Shader"): the same clips, the same alpha cutoff, the same haze and water fog. Pairs with alpha.vert.

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#include "lib/atmos.glsl"
#include "lib/fog.glsl"
#include "lib/shade.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);
    frag_color = shade_fullbright(vary_position, texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0), vertex_color);
}
