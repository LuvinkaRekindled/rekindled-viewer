// gbuffer_material.frag: a legacy material surface (specular colour and glossiness, an optional specular map, an optional normal map, an environment intensity, an alpha mode) of the G-buffer pass.
// Defines: MAT_NORMAL_MAP, MAT_SPEC_MAP, MAT_ALPHA_MODE (0 none, 2 mask, 3 emissive: the diffuse alpha is the emissive amount). Writes the viewer's three targets: albedo (sRGB, the emissive amount in alpha),
// specular colour and glossiness, the octahedral normal with the environment intensity and the flag HAS_ATMOS (0.34); the emissive target (RenderEnableEmissiveBuffer) is zero.
// Matches "Material Pull Shader N" (materialF.glsl, the G-buffer branch). Pairs with gbuffer.vert (GB_MATERIAL, the same defines).

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_normal;
layout(location = 5) flat in float vary_cutoff;
layout(location = 6) in vec3 vary_tangent;
layout(location = 7) flat in float vary_sign;
layout(location = 8) in vec2 vary_texcoord1;
layout(location = 9) in vec2 vary_texcoord2;
layout(location = 10) flat in uint vary_slot_normal;
layout(location = 11) flat in uint vary_slot_specular;
layout(location = 12) flat in vec4 vary_specular;
layout(location = 13) flat in vec2 vary_matparams;

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
    mirror_clip(vary_position);

    vec4 diffcol = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0);
    diffcol.rgb *= vertex_color.rgb;
#if MAT_ALPHA_MODE == 2
    if (diffcol.a < vary_cutoff - 0.001953125)      // (the bias is half an 8 bit step: no acne at the boundary)
    {
        discard;
    }
#endif

#ifdef MAT_SPEC_MAP
    vec4 spec = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_specular))], vary_texcoord2);
    spec.rgb *= vary_specular.rgb;
#else
    const vec4 spec = vec4(vary_specular.rgb, 1.0);
#endif
    const float env = vary_matparams.x * spec.a;
    float glossiness = vary_specular.a;

#ifdef MAT_NORMAL_MAP
    vec4 normal_sample = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_normal))], vary_texcoord1);
    glossiness *= normal_sample.a;
    const vec3 tangent_normal = normal_sample.xyz * 2.0 - 1.0;
    const vec3 binormal = vary_sign * cross(vary_normal, vary_tangent);
    const vec3 norm = normalize(tangent_normal.x * vary_tangent + tangent_normal.y * binormal + tangent_normal.z * vary_normal);
#else
    const vec3 norm = normalize(vary_normal);
#endif

#if MAT_ALPHA_MODE == 3
    const float emissive = max(diffcol.a, vary_matparams.y);
#else
    const float emissive = vary_matparams.y;
#endif

    frag_albedo = max(vec4(diffcol.rgb, emissive), vec4(0.0));        // (the G-buffer is sRGB for legacy materials)
    frag_spec = max(vec4(spec.rgb, glossiness), vec4(0.0));
    frag_normal = vec4(norm.xy / sqrt(8.0 * norm.z + 8.0) + 0.5, env, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
