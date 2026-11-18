// lightmap_upsample.frag: the half resolution lightmap (sun shadow, occlusion, projector shadows) back to full resolution, joint bilateral: the four nearest half resolution pixels are weighted by the bilinear
// weights and kept only when their surface lies in the tangent plane of this pixel (the rule the old blur used for its taps), so shadows and occlusion do not bleed over silhouettes.
// A half resolution pixel was computed at the full resolution pixel (2i + 1) of its block (sun_lightmap.frag, LL_HALF): its depth is read from there.
// The plane test goes both ways: the tap must lie in the plane of this pixel's surface AND this pixel must lie in the plane of the tap's surface. One way alone lets a tap across a silhouette through when the pixel's
// own surface is seen edge on (an arm against the ground: the arm's normal is across the view, the ground tap's depth gap does not show along it), and the ground's leaf shadows then bleed onto the arm's edge as a
// stepped line (Adam, 2026-11-09). The ground tap's normal points up: the arm pixel is far off its plane, so it is refused.
// With the half resolution sun pass and the blur at half resolution (the same blur program, the radius halved) this replaces the sun pass and the two blur passes at full resolution.
// ups_mode (a live key of rt-debug.txt, a bisect switch): 0 = the program (hard accept of the taps in both planes), 1 = plain bilinear, no surface test, 2 = soft weights
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 inv_proj;
    vec2 screen_res;
    int ups_mode;
    vec2 half_offset;           // the pixel of its 2x2 block a half resolution pixel was computed at (sun_lightmap.frag)
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
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
    const vec2 f = (uv * screen_res - half_offset - 0.5) / 2.0;        // the position in the half resolution grid: pixel i was sampled at the full resolution position 2i + half_offset + 0.5
    const vec2 base = floor(f);
    const vec2 frac = f - base;

    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    vec4 nearest = vec4(1.0);           // the fallback when no tap is on this surface: the one that is closest to this pixel's tangent plane (the least off the planes both ways; not the biggest bilinear weight: at a silhouette that is often the surface behind)
    float nearest_dd = 1.0e30;
    for (int k = 0; k < 4; ++k)
    {
        const vec2 off = vec2(float(k & 1), float(k >> 1));
        const vec2 idx = clamp(base + off, vec2(0.0), half_res - 1.0);
        const float w = (off.x > 0.5 ? frac.x : 1.0 - frac.x) * (off.y > 0.5 ? frac.y : 1.0 - frac.y);
        const vec4 col = texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], (idx + 0.5) / half_res);
        const vec2 tap_uv = (min(idx * 2.0 + half_offset, screen_res - 1.0) + 0.5) / screen_res;
        const float tap_depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tap_uv).r;
        if (tap_depth >= 1.0)
        {
            continue;                   // a sky pixel's lightmap value (1.0) is not a lighting value of this surface
        }
        const vec3 tap_pos = ll_view_pos(inv_proj, tap_uv, tap_depth);
        const vec3 tap_norm = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], tap_uv).xy);
        const float d = dot(norm, tap_pos - pos);
        const float d_back = dot(tap_norm, pos - tap_pos);
        const float off_plane = d * d + d_back * d_back;        // how far the two surfaces are from each other, both ways (the fallback takes the tap with the least)
        if (off_plane < nearest_dd)
        {
            nearest_dd = off_plane;
            nearest = col;
        }
        if (ups_mode == 1)
        {
            sum += col * w;
            wsum += w;
        }
        else if (ups_mode == 2)
        {
            const float k = clamp(1.0 - off_plane / (tolerance2 * 4.0), 0.0, 1.0);     // fades out over four times the tolerance: no jump in the weights between neighbouring pixels
            sum += col * (w * k * k);
            wsum += w * k * k;
        }
        else if (d * d <= tolerance2 && d_back * d_back <= tolerance2)
        {
            sum += col * w;
            wsum += w;
        }
    }
    frag_color = wsum > 1.0e-4 ? sum / wsum : nearest;
}
