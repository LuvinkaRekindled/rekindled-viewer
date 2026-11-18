// mask_nocolor.frag: an alpha masked surface of the G-buffer pass with no vertex colour: the texture's alpha against the draw's cutoff (minimum_alpha), the texture's colour as the albedo (not converted), no specular,
// environment intensity 0, flag HAS_ATMOS. A line for line translation of "Deferred Diffuse Non-Indexed Alpha Mask No Color Shader" (deferred/diffuseAlphaMaskNoColorF.glsl); pairs with mask_nocolor.vert.

#include "alpha_pass.inc"

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in vec3 vary_normal;

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
    const vec4 col = texture(rk_tbl2d[nonuniformEXT(RK_HM(rk_ti_diffuseMap))], vary_texcoord0);
    if (col.a < minimum_alpha)
    {
        discard;
    }
    const vec3 n = normalize(vary_normal);
    frag_albedo = vec4(col.rgb, 0.0);
    frag_spec = vec4(0.0);
    frag_normal = vec4(n.xy / sqrt(8.0 * n.z + 8.0) + 0.5, 0.0, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
