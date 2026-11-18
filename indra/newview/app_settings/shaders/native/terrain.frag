// terrain.frag: the legacy (texture) terrain of the G-buffer pass: four detail textures blended by three steps of a height ramp. Albedo = the blend, the specular target the old program's constant, the normal target
// the octahedral normal with the flag HAS_ATMOS (0.34), the emissive target (RenderEnableEmissiveBuffer) zero. Matches "Deferred Terrain Pull Shader" (terrainF.glsl). Pairs with terrain.vert.
// What is different from the old program, which fetches all seven textures for every pixel:
//  - the three ramp lookups (a small texture) come first and give the weight of each of the four layers (the old program's mix of mixes written out: w0 = af * a1, w1 = af * (1 - a1), w2 = (1 - af) * a2,
//    w3 = (1 - af) * (1 - a2)); a detail texture is fetched only when its weight can show: under 1/1024 it adds less than a quarter of a level of an 8 bit channel (4 layers at most: one level at the very
//    worst), so on most of the terrain, where the ramp sits inside one or two layers, two of the four large fetches are not made;
//  - the fetches that are conditional take their derivatives from the coordinate (textureGrad, worked out once before the branches), so they are the same as the unconditional ones;
//  - no clamp of the result to non negative (a blend of 8 bit textures with weights in 0..1 cannot be negative).

#define TERRAIN_PASS 1
#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_detail_0;
    uint ll_ti_detail_1;
    uint ll_ti_detail_2;
    uint ll_ti_detail_3;
    uint ll_ti_alpha_ramp;
};

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec3 vary_normal;
layout(location = 2) in vec2 vary_detail;
layout(location = 3) in vec2 vary_ramp;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

const float kMinWeight = 1.0 / 1024.0;

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    mirror_clip(vary_position);

    // the ramp: the same texture read at the coordinate, and 2 and 1 to the left of it (the old program's vary_texcoord0.zw, vary_texcoord1.xy, vary_texcoord1.zw)
    const float alpha1 = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_ramp).a;
    const float alpha2 = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_ramp - vec2(2.0, 0.0)).a;
    const float alpha_final = texture(ll_tbl2d[LL_HM(ll_ti_alpha_ramp)], vary_ramp - vec2(1.0, 0.0)).a;

    const float w0 = alpha_final * alpha1;
    const float w1 = alpha_final * (1.0 - alpha1);
    const float w2 = (1.0 - alpha_final) * alpha2;
    const float w3 = (1.0 - alpha_final) * (1.0 - alpha2);

    // the derivatives once, outside the branches
    const vec2 dx = dFdx(vary_detail);
    const vec2 dy = dFdy(vary_detail);

    vec3 color = vec3(0.0);
    if (w0 > kMinWeight)
    {
        color += w0 * textureGrad(ll_tbl2d[LL_HM(ll_ti_detail_0)], vary_detail, dx, dy).rgb;
    }
    if (w1 > kMinWeight)
    {
        color += w1 * textureGrad(ll_tbl2d[LL_HM(ll_ti_detail_1)], vary_detail, dx, dy).rgb;
    }
    if (w2 > kMinWeight)
    {
        color += w2 * textureGrad(ll_tbl2d[LL_HM(ll_ti_detail_2)], vary_detail, dx, dy).rgb;
    }
    if (w3 > kMinWeight)
    {
        color += w3 * textureGrad(ll_tbl2d[LL_HM(ll_ti_detail_3)], vary_detail, dx, dy).rgb;
    }

    frag_albedo = vec4(color, 0.0);          // (alpha 0: the atmospherics come downstream)
    frag_spec = vec4(0.0, 0.0, 0.0, -1.0);
    const vec3 n = normalize(vary_normal);
    // the octahedral encoding of the viewer (encodeNormal in globalF.glsl), environment 0, the flag HAS_ATMOS
    frag_normal = vec4(n.xy / sqrt(8.0 * n.z + 8.0) + 0.5, 0.0, 0.34);
#if defined(HAS_EMISSIVE)
    frag_emissive = vec4(0.0);
#endif
}
