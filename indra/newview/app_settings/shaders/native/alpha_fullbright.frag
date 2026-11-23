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

layout(location = 15) in vec3 vary_cover_motion;

layout(location = 0) out vec4 frag_color;
#include "lib/cover.glsl"

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);
    frag_color = shade_fullbright(vary_position, texture(rk_tbl2d[nonuniformEXT(RK_HM(vary_slot))], vary_texcoord0), vertex_color);
    const uint cover_flags = uint(vary_cover_motion.z + 0.5);
    rk_write_cover(frag_color.a, (cover_flags & 1u) != 0u ? 1.0 : 0.0);
    rk_write_motion(frag_color.a, (cover_flags & 2u) != 0u, vary_cover_motion.xy);
}
