// avatar_alpha.frag: the system avatar's blended parts (the alpha pass of the avatar pool: clothing layers, the skin's alpha layers): the body part's texture, lit as every other lit transparent surface
// (lib/shade_lit.glsl: the sky, the sun with its cascade shadow, the view's lights, the air or water between it and the eye). A translation of "Deferred Avatar Alpha Shader" (deferred/alphaF.glsl with
// IS_AVATAR_SKIN, USE_DIFFUSE_TEX): no vertex colour (white), the alpha cutoff of the draw, no water clip (an avatar is not clipped against the water plane). Pairs with avatar.vert (AVATAR_PASS, no cloth).
// What is different: the lit shading is the function the object programs share (alpha_lit.frag), not a copy.

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#include "lib/atmos.glsl"
#include "lib/fog.glsl"
#include "lib/shadow.glsl"
#include "lib/shadow_alpha.glsl"
#include "lib/light.glsl"
#ifdef ALPHA_PROBES
#include "lib/probe.glsl"
#endif
#include "lib/shade_lit.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec3 vary_normal;

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    const vec4 texel = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_diffuseMap))], vary_texcoord0);
    frag_color = shade_lit(vary_position, vary_normal, texel, vec4(1.0));
}
