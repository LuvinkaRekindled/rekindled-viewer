// sun_lightmap.frag: the lightmap of the main view. r: the sun (or moon) shadow from the four cascades, g: ambient occlusion, b and a: the shadows of the two projector lights that have a shadow map.
// Same lookups, weights and rules as the old sun program with SSAO; what is different is the cost: the view position of a pixel and of every occlusion tap is rebuilt from its depth with
// the few multiplications the projection leaves (lib/viewpos.glsl), and the cascade lookup is one function that goes straight to the cascade(s) the pixel is in.
// This program stands in for the old one only when the old one has shadows and SSAO and no ray tracing (manifest: require). With LL_HALF it is the half resolution lightmap (gNativeSunHalfProgram).
// With LL_NO_AO it is the lightmap of a reflection probe's snapshot ("Deferred Sun Probe Shader", native only): the same without the occlusion (g is 1: the probe's own light has none), so no noise texture, no tap
// loop and none of the occlusion uniforms.
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    mat4 shadow_matrix[6];
    vec4 shadow_clip;
    vec3 sun_dir;
    vec3 moon_dir;
    vec2 screen_res;
    vec2 shadow_res;
    vec2 proj_shadow_res;
    float shadow_bias;
    float shadow_offset;
    float spot_shadow_bias;
    float spot_shadow_offset;
#ifndef LL_NO_AO
    float ssao_radius;
    float ssao_max_radius;
    float ssao_factor;
    float ssao_factor_inv;
    float ao_noise_frame;
#endif
    int sun_up_factor;
    int spot_shadow_mask;       // bit 0 / 1: the projector with a shadow map in slot 0 / 1 exists this frame
#ifdef LL_HALF
    vec2 half_offset;           // which pixel of its 2x2 block a half resolution pixel reads: (1, 1) the lower right one, or one of the four, a different one each frame with the upscaler (lightmap_upsample.frag uses the same)
#endif
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_depthMap;
    uint ll_ti_normalMap;
#ifndef LL_NO_AO
    uint ll_ti_noiseMap;
#endif
    uint ll_ti_shadow_shadowMap0;
    uint ll_ti_shadow_shadowMap1;
    uint ll_ti_shadow_shadowMap2;
    uint ll_ti_shadow_shadowMap3;
    uint ll_ti_shadow_shadowMap4;
    uint ll_ti_shadow_shadowMap5;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

#include "lib/shadow.glsl"

float pcf_spot(uint slot, vec4 stc, float bias_scale, vec2 snap_seed)
{
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    stc.x = floor(proj_shadow_res.x * stc.x + fract(snap_seed.y * 0.666666666)) / proj_shadow_res.x;
    vec2 off = 1.0 / proj_shadow_res;
    off.y *= 1.5;
    float shadow = texture(SHADOW_MAP(slot), stc.xyz);
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(off.x * 2.0, off.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(off.x, -off.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(-off.x, off.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(-off.x * 2.0, -off.y, 0.0));
    return shadow * 0.2;
}

// the shadow of a projector light: only the pixels inside its frustum read it (the light pass discards the rest before it looks at the lightmap)
float spot_shadow(vec3 pos, vec3 norm, int index)
{
    pos += norm * spot_shadow_offset;
    const vec4 spos = vec4(pos, 1.0);
    if (!(spos.z > -shadow_clip.w))
    {
        return 1.0;
    }
    const vec4 near_split = shadow_clip * -0.75;
    const vec4 far_split = shadow_clip * -1.25;
    const vec4 transition = near_split - far_split;
    const float w = 1.0 - max(spos.z - far_split.z, 0.0) / transition.z;
    const vec4 lpos = shadow_matrix[4 + index] * spos;
    const vec2 stc_xy = lpos.xy / lpos.w;
    if (lpos.w <= 0.0 || stc_xy.x < -0.05 || stc_xy.x > 1.05 || stc_xy.y < -0.05 || stc_xy.y > 1.05)
    {
        return 1.0;
    }
    float shadow = pcf_spot(index == 0 ? ll_ti_shadow_shadowMap4 : ll_ti_shadow_shadowMap5, lpos, 0.8, spos.xy) * w;
    shadow += max((pos.z + shadow_clip.z) / (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0);
    return shadow / w;
}

#ifndef LL_NO_AO
// ambient occlusion: eight taps spread (squared) around the pixel, each one an occluding sphere of the same size; samples much closer to the camera are "no data", not "no occlusion"
const vec2 kAoKern[8] = vec2[8](
    vec2(-1.0, 0.0) * 0.125 * 0.125, vec2(1.0, 0.0) * 0.250 * 0.250, vec2(0.0, 1.0) * 0.375 * 0.375, vec2(0.0, -1.0) * 0.500 * 0.500,
    vec2(0.7071, 0.7071) * 0.625 * 0.625, vec2(-0.7071, -0.7071) * 0.750 * 0.750, vec2(-0.7071, 0.7071) * 0.875 * 0.875, vec2(0.7071, -0.7071) * 1.000 * 1.000);

float ambient_occlusion(vec3 pos, vec3 norm, vec2 uv)
{
    const vec2 noise_shift = floor(fract(ao_noise_frame * vec2(0.7548776662, 0.5698402910)) * 128.0) / 128.0;
    const vec2 noise_reflect = texture(ll_tbl2d[LL_HM(ll_ti_noiseMap)], uv * (screen_res / 128.0) + noise_shift).xy;
    const float scale = min(ssao_radius / -pos.z, ssao_max_radius);
    float angle_hidden = 0.0;
    float points = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        const vec2 tap = uv + scale * reflect(kAoKern[i] / screen_res, noise_reflect);
        const vec3 tap_pos = ll_view_pos(inv_proj, tap, texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tap).r);
        const vec3 diff = pos - tap_pos;
        const float dist2 = dot(diff, diff);
        const float hidden = dot(tap_pos - 0.05 * norm - pos, norm) > 0.0 ? 1.0 : 0.0;
        angle_hidden += hidden * min(1.0 / dist2, ssao_factor_inv);
        points += diff.z > -1.0 ? 1.0 : 0.0;
    }
    angle_hidden = min(ssao_factor * angle_hidden / points, 1.0);
    const float ret = 1.0 - (points > 0.0 ? angle_hidden : 0.0);
    return clamp(ret, 0.0, 1.0);
}
#endif

void main()
{
#ifdef LL_HALF
    // half resolution: this pixel stands for a 2x2 block of the full resolution picture and reads the full resolution pixel half_offset of the block (2i + half_offset: the lower right one, or a different one each frame with the upscaler); lightmap_upsample.frag reads the same pixel's depth
    const vec2 half_res = floor((screen_res + 1.0) * 0.5);
    const vec2 uv = (floor(vary_fragcoord * half_res) * 2.0 + half_offset + 0.5) / screen_res;
#else
    const vec2 uv = vary_fragcoord;
#endif
    const float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], uv).r;
    if (depth >= 1.0)
    {
        frag_color = vec4(1.0);         // a sky pixel: nothing reads the lightmap there
        return;
    }
    const vec3 pos = ll_view_pos(inv_proj, uv, depth);
    const vec3 norm = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], uv).xy);

    vec4 col;
    col.r = sun_shadow(pos, norm, uv.y);
#ifdef LL_NO_AO
    col.g = 1.0;
#else
    col.g = ambient_occlusion(pos, norm, uv);
#endif
    col.b = (spot_shadow_mask & 1) != 0 ? spot_shadow(pos, norm, 0) : 1.0;
    col.a = (spot_shadow_mask & 2) != 0 ? spot_shadow(pos, norm, 1) : 1.0;
    frag_color = clamp(col, vec4(0.0), vec4(1.0));
}
