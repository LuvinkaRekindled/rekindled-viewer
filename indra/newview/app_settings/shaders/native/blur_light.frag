// blur_light.frag: the depth-aware blur of the lightmap (sun shadow in r, SSAO in g, spot shadows in b and a), one direction per pass (`delta`).
// Same taps, weights and rules as the old blur (13 taps along the direction, a tap counts when its surface is within a distance tolerance of the pixel's tangent plane, the weights of g
// are the y of the kernel, the others the x); what is different is the cost: a sky pixel returns at once (nothing reads the lightmap there: the resolve takes the sky from the G-buffer),
// and the position of a tap is rebuilt from its depth with the few multiplications the projection leaves (lib/viewpos.glsl).
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    vec3 kern[4];
    vec2 delta;
    vec2 screen_res;
    float dist_factor;
    float kern_scale;
    float sun_passthrough;      // 1: the sun channel (r) keeps its own value (the ray traced sun shadow's penumbra is the filter's)
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_lightMap;
    uint ll_ti_normalMap;
    uint ll_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 tc0 = vary_fragcoord;
    const float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tc0).r;
    const vec4 ccol = texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], tc0);
    if (depth >= 1.0)
    {
        frag_color = ccol;          // a sky pixel: the lightmap is 1.0 there and nothing reads it
        return;
    }

    const vec3 norm = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], tc0).xy);
    const vec3 pos = ll_view_pos(inv_proj, tc0, depth);

    vec2 dlt = kern_scale * delta / (1.0 + norm.xy * norm.xy);
    dlt /= max(-pos.z * dist_factor, 1.0);

    vec2 defined_weight = kern[0].xy;       // the centre tap, already fetched
    vec4 col = defined_weight.xyxx * ccol;

    // the tolerance grows with the distance: angles and distances are more abrupt within a small screen area far away
    const float tolerance2 = pos.z * pos.z * 0.00005;

    // the sampling origin is nudged by a screen space pattern to hide the ghosting at the edges where the radius is large
    vec2 tc = tc0 * screen_res;
    float tc_mod = 0.5 * (tc.x + tc.y);
    tc_mod -= floor(tc_mod);
    tc_mod *= 2.0;
    tc += (tc_mod - 0.5) * kern[1].z * dlt * 0.5;

    // the 7 tap offsets of one side: the 4 kernel entries and the 3 midpoints between them
    vec3 k[7];
    k[0] = kern[0];
    k[2] = kern[1];
    k[4] = kern[2];
    k[6] = kern[3];
    k[1] = (k[0] + k[2]) * 0.5;
    k[3] = (k[2] + k[4]) * 0.5;
    k[5] = (k[4] + k[6]) * 0.5;

    for (int side = 0; side < 2; ++side)
    {
        const float sgn = side == 0 ? 1.0 : -1.0;
        for (int i = 1; i < 7; ++i)
        {
            const vec2 samptc = (tc + sgn * k[i].z * dlt * 2.0) / screen_res;
            const vec3 samppos = ll_view_pos(inv_proj, samptc, texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], samptc).r);
            const float d = dot(norm, samppos - pos);       // distance from the tangent plane
            if (d * d <= tolerance2)
            {
                col += texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], samptc) * k[i].xyxx;
                defined_weight += k[i].xy;
            }
        }
    }

    col /= defined_weight.xyxx;
    col.r = mix(col.r, ccol.r, sun_passthrough);
    frag_color = max(col, vec4(0.0));
}
