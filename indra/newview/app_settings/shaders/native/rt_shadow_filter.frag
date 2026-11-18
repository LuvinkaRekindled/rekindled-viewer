// rt_shadow_filter.frag: the second pass of the penumbra of the ray traced sun shadow (PLAN-rt-shadow-penumbra.md step 2b): the hard shadow of the centre ray filtered over a disc whose radius is the pixel's
// penumbra half-width (or the average width of the blockers in the window, where that is wider), into the lightmap's sun channel; the other channels pass through. Reads rt_shadow_prep.frag's target with its
// mip chain: the blocker search is one read at the window's level, each filter tap is read at the level of the taps' spacing.
// A translation of "RT Shadow Penumbra Filter" (deferred/rtShadowFilterF.glsl): the same width rules, the same 16 Vogel taps, the same weights and plane test. What is different: the position of a tap is
// rebuilt from the one depth read (lib/viewpos.glsl), and a tap whose position is not a number is rejected (written as `!(d * d <= tol)`, a NaN compares false against the tolerance).

#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 inv_proj;
    vec2 screen_res;
    float pen_max;              // the widest half-width in pixels: the search window's half-width too
    float pen_lod;              // a bias in mip levels on the taps' level
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_lightMap;        // r: the sun shadow (the hard answer of the centre ray), g: SSAO, b and a: the spot shadows
    uint rk_ti_depthMap;
    uint rk_ti_normalMap;
    uint rk_ti_diffuseRect;     // rt_shadow_prep.frag's target with its mip chain (trilinear): r the hard sun shadow, g the half-width in pixels, b the blocker mask
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

#define FILTER_TAPS 16

// a point of the unit disc: the Vogel spiral (the golden angle), evenly spread, the same every frame
vec2 vogel(int i, int n)
{
    const float r = sqrt((float(i) + 0.5) / float(n));
    const float a = float(i) * 2.3999632;
    return vec2(cos(a), sin(a)) * r;
}

void main()
{
    const vec2 tc = vary_fragcoord;
    vec4 c = texture(rk_tbl2d[LL_HM(rk_ti_lightMap)], tc);
    const float depth = texture(rk_tbl2d[LL_HM(rk_ti_depthMap)], tc).r;
    if (depth >= 1.0)
    {
        frag_color = c;             // a sky pixel: nothing reads its sun channel
        return;
    }

    // 1. the width: the pixel's own, or the window's average blocker width where that is wider
    float r = textureLod(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], tc, 0.0).g;
    const vec2 win = textureLod(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], tc, log2(max(pen_max, 1.0))).gb;      // (width sum, blocker count) over the window, each divided by the same pixel count
    if (win.y > 0.001)
    {
        r = max(r, win.x / win.y);
    }
    if (r < 0.5)
    {
        frag_color = c;             // hard: a contact, or no blocker anywhere near
        return;
    }

    const vec3 pos = rk_view_pos(inv_proj, tc, depth);
    const vec3 n = rk_decode_normal(texture(rk_tbl2d[LL_HM(rk_ti_normalMap)], tc).xy);
    // the lightmap blur's plane distance tolerance: looser with distance, where angles and distances are abrupt within a small screen area
    const float tol = pos.z * pos.z * 0.00005;

    // 2. the filter: the hard shadow over the disc of radius r, the weight falling to 0 at the rim; each tap at the mip level of the taps' spacing (sqrt(pi / N) r for N taps on the disc)
    const float lod = max(log2(r * 0.443) + pen_lod, 0.0);
    const vec4 m0 = textureLod(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], tc, lod);
    float sum = m0.r;               // (premultiplied by the surface flag: a = the share of surface pixels in the footprint)
    float wsum = m0.a;
    for (int i = 0; i < FILTER_TAPS; ++i)
    {
        const vec2 u = vogel(i, FILTER_TAPS);
        const vec2 stc = tc + u * r / screen_res;
        const vec3 spos = rk_view_pos(inv_proj, stc, texture(rk_tbl2d[LL_HM(rk_ti_depthMap)], stc).r);
        const float d = dot(n, spos - pos);
        if (!(d * d <= tol))
        {
            continue;
        }
        const float w = 1.0 - 0.75 * dot(u, u);
        const vec4 m = textureLod(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], stc, lod);
        sum += m.r * w;
        wsum += m.a * w;
    }
    if (wsum > 0.001)
    {
        c.r = sum / wsum;
    }
    frag_color = max(c, vec4(0.0));
}
