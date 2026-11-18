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
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    vec4 sunDiscA = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_texcoord0.xy);
    vec4 sunDiscB = texture(rk_tbl2d[RK_HM(rk_ti_altDiffuseMap)], vary_texcoord0.xy);
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
