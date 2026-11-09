// alpha_pbr_hud.frag: a glTF blended surface in the HUD: no lighting, no air. The base colour times the vertex colour plus the emissive colour, back to sRGB (the HUD pass writes sRGB values).
// Matches the viewer's "HUD PBR Alpha Pull Shader". Pairs with alpha_pbr.vert (ALPHA_PBR, ALPHA_PBR_HUD).

#include "alpha_pass.inc"
#include "lib/color.glsl"

layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_tc_base;
layout(location = 5) in vec2 vary_tc_emissive;
layout(location = 9) flat in uvec4 vary_slots;
layout(location = 11) flat in vec3 vary_emissive;

layout(location = 0) out vec4 frag_color;

void main()
{
    const vec4 base = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.x))], vary_tc_base);
    const vec3 emissive = vary_emissive * srgb_to_linear(texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.w))], vary_tc_emissive).rgb);
    const vec3 color = vertex_color.rgb * srgb_to_linear(base.rgb) + emissive;
    frag_color = max(vec4(linear_to_srgb(color), base.a * vertex_color.a), vec4(0.0));
}
