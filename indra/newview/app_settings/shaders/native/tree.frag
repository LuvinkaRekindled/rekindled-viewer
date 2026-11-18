// tree.frag: the Linden trees of the G-buffer pass: the pool's one texture, alpha tested at minimum_alpha (0.5), albedo = the texel (the old program multiplied by a constant white vertex colour), the specular
// target zero, the normal target the octahedral normal with the flag HAS_ATMOS (0.34), the emissive target (RenderEnableEmissiveBuffer) zero. Matches "Deferred Tree Pull Shader" (treeF.glsl).
// Pairs with tree.vert.

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec3 vary_normal;
layout(location = 2) in vec2 vary_texcoord0;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
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
    const vec4 col = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_texcoord0);
    if (col.a < minimum_alpha)
    {
        discard;
    }
    frag_albedo = vec4(col.rgb, 0.0);
    frag_spec = vec4(0.0);
    const vec3 n = normalize(vary_normal);
    // the octahedral encoding of the viewer (encodeNormal in globalF.glsl), environment 0, the flag HAS_ATMOS
    frag_normal = vec4(n.xy / sqrt(8.0 * n.z + 8.0) + 0.5, 0.0, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
