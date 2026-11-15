// gbuffer_bump.frag: a bump mapped (legacy "bumpiness") surface of the G-buffer pass: the diffuse texture times the vertex colour, the bump image read as a tangent space normal with the diffuse
// coordinates, no specular colour (the vertex alpha is both the specular value and the environment intensity), flag HAS_ATMOS.
// Matches "Deferred Bump Pull Shader" / "Deferred Bump Pull Skin Shader" (bumpF.glsl). Pairs with gbuffer.vert (GB_BUMP): the tangent frame is two varyings and a flat sign (the old pass sent three vec3 rows).

#include "alpha_pass.inc"
#include "lib/clip.glsl"
#include "lib/texneed.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_normal;
layout(location = 5) flat in float vary_cutoff;
layout(location = 6) in vec3 vary_tangent;
layout(location = 7) flat in float vary_sign;
layout(location = 10) flat in uint vary_slot_normal;

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
layout(location = 5) out vec2 frag_texneed;       // the texture need (lib/texneed.glsl): idea 3

void main()
{
    frag_texneed = ll_texneed(vary_slot, vary_texcoord0);
#ifdef GB_MOTION
    frag_velocity = ll_motion_encode(vary_cur, vary_prev, render_res);
#else
    frag_velocity = vec2(1.0, 0.0);
#endif
    mirror_clip(vary_position);

    vec4 col = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0);
    if (col.a < vary_cutoff)
    {
        discard;
    }
    col *= vertex_color;

    const vec3 tangent_normal = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_normal))], vary_texcoord0).rgb * 2.0 - 1.0;
    const vec3 binormal = vary_sign * cross(vary_normal, vary_tangent);
    const vec3 norm = normalize(tangent_normal.x * vary_tangent + tangent_normal.y * binormal + tangent_normal.z * vary_normal);

    frag_albedo = vec4(col.rgb, 0.0);
    frag_spec = vertex_color.aaaa;
    frag_normal = vec4(norm.xy / sqrt(8.0 * norm.z + 8.0) + 0.5, vertex_color.a, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
