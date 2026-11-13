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

void main()
{
    const vec4 col = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_diffuseMap))], vary_texcoord0);
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
