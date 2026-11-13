// lib/shadow_alpha.glsl: the sun shadow of a transparent surface (the lit alpha programs: alpha_lit.frag, avatar_alpha.frag, alpha_pbr.frag). Included after lib/shadow.glsl. pos_screen is the pixel's place on the screen (0 to 1).
// A session with ray queries (LL_RT_QUERY: the manifest option rtquery) traces the sun's ray as the stock alpha programs did (sampleDirectionalShadow of shadowUtil.glsl with LL_RT_SHADOWS 1: lib/rt.glsl, the stock text); the
// alpha pass sets the sun disc, the sample count, the lod and the distance limit (lldrawpoolalpha.cpp: the members of rt_uniforms.inc in alpha_pass.inc). Otherwise the cascades are sampled (jittered by the screen row).

#ifdef LL_RT_QUERY
#include "lib/rt_scene.glsl"
#include "lib/rt.glsl"
float sun_shadow_alpha(vec3 pos, vec3 norm, vec2 pos_screen)
{
    return rt_sun_shadow(pos, norm, pos_screen);
}
#else
float sun_shadow_alpha(vec3 pos, vec3 norm, vec2 pos_screen)
{
    return sun_shadow(pos, norm, pos_screen.y);
}
#endif
