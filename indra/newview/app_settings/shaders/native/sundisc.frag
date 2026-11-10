// sundisc.frag: the sun disc: two disc textures blended by blend_factor, written as the emissive colour (or the albedo without the emissive target) with the flag that keeps the lighting resolve's atmosphere off.
// A line for line translation of "Deferred Windlight Sun Program pull" (deferred/sunDiscF.glsl); pairs with sundisc.vert.

#include "sky_pass.inc"
#include "lib/flags.glsl"

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in float sun_fade;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif

void main()
{
    vec4 sunDiscA = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
    vec4 sunDiscB = texture(ll_tbl2d[LL_HM(ll_ti_altDiffuseMap)], vary_texcoord0.xy);
    vec4 c     = mix(sunDiscA, sunDiscB, blend_factor);

    // SL-9806 stars poke through
    //c.a *= sun_fade;

    frag_albedo = vec4(0);
    frag_spec = vec4(0.0f);
    frag_normal = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_albedo = vec4(0);
    frag_emissive = c;
#else
    frag_albedo = c;
#endif
}
