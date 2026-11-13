// rt_shadow_prep.frag: the first pass of the penumbra of the ray traced sun shadow (PLAN-rt-shadow-penumbra.md step 2b): into a target that gets a mip chain, the hard sun channel of each surface pixel, its
// penumbra half-width in pixels (from the blocker distance of the centre ray, in metres: width = distance * tan(sun radius) * focal / depth, at most pen_max), and the blocker mask. rt_shadow_filter.frag reads
// the chain: one read at the window's level is the search for blockers over the whole window.
// A translation of "RT Shadow Penumbra Prep" (deferred/rtShadowPrepF.glsl). What is different: a sky pixel returns at once with the values the old program ends with there (its blocker is 0: the sun pass writes
// 0 for the sky), the depth of a pixel is read once (the old one read it for the sky test and again for the position), and the island test of the four neighbours rejects a neighbour whose position is not a
// number as well (a NaN compares false against the tolerance; written so that it counts as off the plane).

#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    float pen_tan;              // tan of the sun's angular radius (RenderRTShadowSoftness)
    float pen_focal;            // the focal length in render pixels
    float pen_max;              // the widest half-width in pixels
    vec2 screen_res;            // the G-buffer size (the neighbours' distance in texture coordinates)
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_lightMap;        // r: the sun shadow (the hard answer of the centre ray)
    uint ll_ti_depthMap;
    uint ll_ti_normalMap;
    uint ll_ti_emissiveRect;    // the blocker distance map (mRtBlocker, R16F, metres along the sun ray; 0 = lit, no blocker)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 tc = vary_fragcoord;
    const float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tc).r;
    if (depth >= 1.0)
    {
        frag_color = vec4(0.0);         // a sky pixel: no surface, no blocker
        return;
    }
    const float sun = texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], tc).r;
    const vec3 pos = ll_view_pos(inv_proj, tc, depth);

    // an island: two or more of the four neighbours are sky or off this pixel's tangent plane (the plane distance tolerance of the lightmap blur and of the filter)
    float valid = 1.0;
    {
        const vec3 n = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], tc).xy);
        const float tol = pos.z * pos.z * 0.00005;
        const vec2 px = 1.0 / screen_res;
        int off = 0;
        for (int i = 0; i < 4; ++i)
        {
            const vec2 s = tc + vec2((i == 0 ? px.x : (i == 1 ? -px.x : 0.0)), (i == 2 ? px.y : (i == 3 ? -px.y : 0.0)));
            const float sd = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], s).r;
            if (sd >= 1.0)
            {
                ++off;
                continue;
            }
            const float d = dot(n, ll_view_pos(inv_proj, s, sd) - pos);
            if (!(d * d <= tol))
            {
                ++off;
            }
        }
        if (off >= 2)
        {
            valid = 0.0;
        }
    }
    const float blocker_t = texture(ll_tbl2d[LL_HM(ll_ti_emissiveRect)], tc).r;
    const float r = blocker_t > 0.0 ? min(blocker_t * pen_tan * pen_focal / max(-pos.z, 0.05), pen_max) : 0.0;
    frag_color = vec4(sun * valid, r, blocker_t > 0.0 ? 1.0 : 0.0, valid);
}
