// stars.frag: the stars: the texture times the vertex colour, faded by custom_alpha and twinkling; written as the emissive colour (or the albedo without the emissive target) with the flag that keeps the
// lighting resolve's atmosphere off. A line for line translation of "Deferred Star Program pull" (deferred/starsF.glsl); pairs with stars.vert.

#include "sky_pass.inc"
#include "lib/flags.glsl"

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec2 screenpos;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif

float twinkle()
{
    float d = fract(screenpos.x + screenpos.y);
    return abs(d);
}

// See:
// ALM off: class1/environment/starsF.glsl
// ALM on : class1/deferred/starsF.glsl
void main()
{
    // camera above water: class1\deferred\starsF.glsl
    // camera below water: class1\environment\starsF.glsl
    vec4 col_a = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
    vec4 col_b = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
    vec4 col = mix(col_b, col_a, blend_factor);

    col.rgb *= vertex_color.rgb;

    float factor = smoothstep(0.0f, 0.9f, custom_alpha);
    col.a = (col.a * factor) * 32.0f;

    col.a *= twinkle();

    frag_spec = vec4(0.0f);
    frag_normal = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_albedo = vec4(0);
    frag_emissive = col;
#else
    frag_albedo = col;
#endif
}
