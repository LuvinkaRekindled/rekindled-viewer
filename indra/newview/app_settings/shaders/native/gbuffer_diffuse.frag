// gbuffer_diffuse.frag: a plain (legacy prim) opaque surface of the G-buffer pass. Albedo = vertex colour times the diffuse map, the specular target holds the vertex alpha in all four channels (it doubles as the
// environment intensity of a plain prim), the normal target holds the octahedral normal, the vertex alpha and the flag HAS_ATMOS (0.34); the emissive target (RenderEnableEmissiveBuffer) is zero.
// Matches "Deferred Diffuse Pull Shader" (diffuseIndexedF.glsl). Pairs with gbuffer.vert.

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_normal;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif

void main()
{
    mirror_clip(vary_position);
    const vec3 col = vertex_color.rgb * texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0).rgb;
    const vec3 n = normalize(vary_normal);
    frag_albedo = vec4(col, 0.0);
    frag_spec = vec4(vertex_color.a);
    // the octahedral encoding of the viewer (encodeNormal in globalF.glsl)
    frag_normal = vec4(n.xy / sqrt(8.0 * n.z + 8.0) + 0.5, vertex_color.a, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
