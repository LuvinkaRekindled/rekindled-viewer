// pbr_opaque_hud.frag: a glTF opaque or mask surface in the HUD: no lighting, no G-buffer; the base colour times the vertex colour plus the emissive colour, back to sRGB (the HUD pass writes sRGB values), alpha 0.
// Matches "HUD PBR Opaque Pull Shader" (pbropaqueF.glsl, IS_HUD). Pairs with alpha_pbr.vert (ALPHA_PBR, ALPHA_PBR_HUD, PBR_OPAQUE).

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
    vec4 base = texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slots.x))], vary_tc_base);
    base.a *= vertex_color.a;
    if (base.a < vary_cutoff)
    {
        discard;
    }
    const vec3 emissive = vary_emissive * srgb_to_linear(texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slots.w))], vary_tc_emissive).rgb);
    const vec3 color = vertex_color.rgb * srgb_to_linear(base.rgb) + emissive;
    frag_color = vec4(linear_to_srgb(color), 0.0);
}
