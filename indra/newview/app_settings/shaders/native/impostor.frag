// impostor.frag: the impostor of an avatar in the G-buffer pass: the three images of the impostor target (diffuse, normal, specular) copied to the G-buffer's targets as they are: the diffuse alpha is tested
// against minimum_alpha, the normal image's first three channels are written with the flag HAS_ATMOS (0.34). The slots of the three images come from the row (impostor.vert).
// Stands in for "Deferred Impostor Shader" (deferred/impostorF.glsl).

layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    float minimum_alpha;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) flat in uint vary_slot_diffuse;
layout(location = 2) flat in uint vary_slot_normal;
layout(location = 3) flat in uint vary_slot_specular;

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
    const vec4 col = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_diffuse))], vary_texcoord0);
    if (col.a < minimum_alpha)
    {
        discard;
    }
    frag_albedo = vec4(col.rgb, 0.0);
    frag_spec = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_specular))], vary_texcoord0);
    frag_normal = vec4(texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot_normal))], vary_texcoord0).xyz, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
