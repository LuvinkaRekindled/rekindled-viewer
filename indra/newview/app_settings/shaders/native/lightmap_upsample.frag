// lightmap_upsample.frag: the half resolution lightmap (sun shadow, occlusion, projector shadows) back to full resolution, joint bilateral: the four nearest half resolution pixels are weighted by the bilinear
// weights and kept only when their surface lies in the tangent plane of this pixel (the rule the old blur used for its taps), so shadows and occlusion do not bleed over silhouettes.
// A half resolution pixel was computed at the full resolution pixel (2i + 1) of its block (sun_lightmap.frag, LL_HALF): its depth is read from there.
// With the half resolution sun pass and the blur at half resolution (the same blur program, the radius halved) this replaces the sun pass and the two blur passes at full resolution.
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    vec2 screen_res;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_lightMap;        // the half resolution lightmap
    uint ll_ti_depthMap;
    uint ll_ti_normalMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 uv = vary_fragcoord;
    const float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], uv).r;
    if (depth >= 1.0)
    {
        frag_color = vec4(1.0);         // a sky pixel: nothing reads the lightmap there
        return;
    }
    const vec3 norm = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], uv).xy);
    const vec3 pos = ll_view_pos(inv_proj, uv, depth);
    const float tolerance2 = pos.z * pos.z * 0.00005;

    const vec2 half_res = floor((screen_res + 1.0) * 0.5);
    const vec2 f = uv * screen_res / 2.0 - 0.75;        // the position in the half resolution grid: pixel i is sampled at the full resolution position 2i + 1.5
    const vec2 base = floor(f);
    const vec2 frac = f - base;

    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    vec4 nearest = vec4(1.0);
    float nearest_w = -1.0;
    for (int k = 0; k < 4; ++k)
    {
        const vec2 off = vec2(float(k & 1), float(k >> 1));
        const vec2 idx = clamp(base + off, vec2(0.0), half_res - 1.0);
        const float w = (off.x > 0.5 ? frac.x : 1.0 - frac.x) * (off.y > 0.5 ? frac.y : 1.0 - frac.y);
        const vec4 col = texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], (idx + 0.5) / half_res);
        const vec2 tap_uv = (min(idx * 2.0 + 1.0, screen_res - 1.0) + 0.5) / screen_res;
        const float tap_depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tap_uv).r;
        if (w > nearest_w)
        {
            nearest_w = w;
            nearest = col;
        }
        if (tap_depth >= 1.0)
        {
            continue;                   // a sky pixel's lightmap value (1.0) is not a lighting value of this surface
        }
        const float d = dot(norm, ll_view_pos(inv_proj, tap_uv, tap_depth) - pos);
        if (d * d <= tolerance2)
        {
            sum += col * w;
            wsum += w;
        }
    }
    frag_color = wsum > 0.0 ? sum / wsum : nearest;
}
