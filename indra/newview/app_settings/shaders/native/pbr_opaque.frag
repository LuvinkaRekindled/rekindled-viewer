// pbr_opaque.frag: a glTF (metallic roughness) opaque or mask surface of the G-buffer pass. Writes the three G-buffer targets of the viewer's layout (and the emissive one when RenderEnableEmissiveBuffer is on):
// albedo (the base colour times the vertex colour, linear), occlusion / roughness / metallic, and the octahedral normal with the flag HAS_PBR (0.67). Matches "Deferred PBR Opaque Pull Shader" (pbropaqueF.glsl
// with LL_BINDLESS_PBR): same reads, same outputs. Pairs with alpha_pbr.vert (ALPHA_PBR, PBR_OPAQUE; HAS_SKIN + HAS_INDEXED_DRAW for a rigged draw).

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"

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
layout(location = 12) flat in float vary_cutoff;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_orm;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    mirror_clip(vary_position);

    vec4 base = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.x))], vary_tc_base);
    base.rgb = srgb_to_linear(base.rgb);
    base *= vertex_color;       // (the vertex colour is not converted: the viewer's glTF shaders do not either)
    if (base.a < vary_cutoff)
    {
        discard;
    }

    // the normal map (a draw without one has slot 0: the flat normal)
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (vary_slots.y != 0u)
    {
        tangent_normal = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.y))], vary_tc_normal).xyz * 2.0 - 1.0;
    }
    const vec3 binormal = vary_sign * cross(vary_normal, vary_tangent);
    vec3 norm = normalize(tangent_normal.x * vary_tangent + tangent_normal.y * binormal + tangent_normal.z * vary_normal);
    norm *= gl_FrontFacing ? 1.0 : -1.0;

    // occlusion (r), roughness (g), metallic (b)
    vec3 orm = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.z))], vary_tc_orm).rgb;
    orm.g *= vary_factors.x;
    orm.b *= vary_factors.y;

    frag_albedo = max(vec4(base.rgb, 0.0), vec4(0.0));
    frag_orm = max(vec4(orm, 0.0), vec4(0.0));
    // the octahedral encoding of the viewer (encodeNormal in globalF.glsl): xy of the normal over sqrt(8 z + 8), the environment intensity 0, the flag HAS_PBR
    frag_normal = vec4(norm.xy / sqrt(8.0 * norm.z + 8.0) + 0.5, 0.0, 0.67);
#if defined(HAS_EMISSIVE)
    frag_emissive = max(vec4(vary_emissive * srgb_to_linear(texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slots.w))], vary_tc_emissive).rgb), 0.0), vec4(0.0));
#endif
}
