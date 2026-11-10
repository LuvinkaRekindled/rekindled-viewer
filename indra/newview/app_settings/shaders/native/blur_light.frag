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
    vec4 half_src;              // the blur of the half resolution lightmap: xy the size of the G-buffer, zw the pixel of its 2x2 block a lightmap pixel was computed at (sun_lightmap.frag, half_offset); x = 0: the lightmap has the G-buffer's size
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_lightMap;
    uint ll_ti_normalMap;
    uint ll_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

// Where in the G-buffer the surface of a lightmap position is. With the lightmap at the G-buffer's size: the same position. With the half resolution lightmap a pixel i holds the lighting of ONE pixel of its 2x2 block
// (2i + half_src.zw): the depth and the normal are read from exactly that pixel, so the surface test below is about the surface the value belongs to. Read at the lightmap position itself (the corner between the
// four pixels: one of them, by rounding) the test is about a neighbour: a one pixel gap in a mesh that shows the sky, or a silhouette, then passes as "the same surface" and its lightmap value (1 for the sky)
// is spread over the taps of the pixels around it (flecks of sunlight on shadowed skin).
vec2 gbuffer_uv(vec2 uv)
{
    if (half_src.x <= 0.0)
    {
        return uv;
    }
    const vec2 idx = clamp(floor(uv * screen_res), vec2(0.0), screen_res - 1.0);
    return (min(idx * 2.0 + half_src.zw, half_src.xy - 1.0) + 0.5) / half_src.xy;
}

void main()
{
    const vec2 tc0 = vary_fragcoord;
    const vec2 g0 = gbuffer_uv(tc0);
    const float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], g0).r;
    const vec4 ccol = texture(ll_tbl2d[LL_HM(ll_ti_lightMap)], tc0);
    if (depth >= 1.0)
    {
        frag_color = ccol;          // a sky pixel: the lightmap is 1.0 there and nothing reads it
        return;
    }

    const vec3 norm = ll_decode_normal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], g0).xy);
    const vec3 pos = ll_view_pos(inv_proj, g0, depth);

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
            const vec2 sampg = gbuffer_uv(samptc);
            const vec3 samppos = ll_view_pos(inv_proj, sampg, texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], sampg).r);
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
