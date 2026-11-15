// avatar.frag: the system avatar in the G-buffer pass: the body part's texture, alpha tested against minimum_alpha, no specular, environment intensity 0, the eye space normal, flag HAS_ATMOS.
// Stands in for "Deferred Avatar Shader pull" (deferred/avatarF.glsl). Pairs with avatar.vert.

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec3 vary_normal;

#ifdef GB_MOTION
#include "lib/motion.glsl"
layout(location = 14) in vec4 vary_cur;
layout(location = 15) in vec4 vary_prev;
#endif

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
#ifdef GB_MOTION
    frag_velocity = ll_motion_encode(vary_cur, vary_prev, render_res);
#else
    frag_velocity = vec2(1.0, 0.0);
#endif
    mirror_clip(vary_position);
    const vec4 diff = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_diffuseMap))], vary_texcoord0);
    if (diff.a < minimum_alpha)
    {
        discard;
    }
    const vec3 n = normalize(vary_normal);
    frag_albedo = vec4(diff.rgb, 0.0);
    frag_spec = vec4(0.0);
    frag_normal = vec4(n.xy / sqrt(8.0 * n.z + 8.0) + 0.5, 0.0, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
