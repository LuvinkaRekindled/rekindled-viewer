// highlight_face.frag: the highlight colour times the face's picture. A translation of "Highlight Shader" (interface/highlightF.glsl; also the normal map and specular map views) and, with HL_GBUFFER, of
// "Deferred Highlight Shader" (deferred/highlightF.glsl: the parcel owners' tint of the terrain, written to the G-buffer: albedo, no specular, a straight up normal that skips the atmosphere, no emissive).
// Pairs with highlight_face.vert.

#include "highlight_face.inc"

layout(location = 0) in vec2 vary_texcoord0;

#ifdef HL_GBUFFER
layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#ifdef HAS_EMISSIVE
layout(location = 3) out vec4 frag_emissive;
#endif
#else
layout(location = 0) out vec4 frag_color;
#endif

void main()
{
    const vec4 shown = color * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0);
#ifdef HL_GBUFFER
    frag_albedo = shown;
    frag_spec = vec4(0.0);
    frag_normal = vec4(0.0, 1.0, 0.0, 0.0);         // (GBUFFER_FLAG_SKIP_ATMOS is 0: no haze over it)
#ifdef HAS_EMISSIVE
    frag_emissive = vec4(0.0);
#endif
#else
    frag_color = max(shown, vec4(0.0));
#endif
}
