// smaa_weights.frag: SMAA's blending weight calculation (the edges become the weights of the blend). A line for line translation of "SMAA Blending Weights" (deferred/SMAABlendWeightsF.glsl);
// pairs with smaa_weights.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec4 SMAA_RT_METRICS;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_edgesTex;
    uint rk_ti_areaTex;
    uint rk_ti_searchTex;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in vec2 vary_pixcoord;
layout(location = 2) in vec4 vary_offset[3];
layout(location = 0) out vec4 frag_color;

#include "lib/smaa.glsl"

// (after the library: its functions have parameters of these names)
#define edgesTex rk_tbl2d[RK_HM(rk_ti_edgesTex)]
#define areaTex rk_tbl2d[RK_HM(rk_ti_areaTex)]
#define searchTex rk_tbl2d[RK_HM(rk_ti_searchTex)]

void main()
{
    frag_color = SMAABlendingWeightCalculationPS(vary_texcoord0,
                                                 vary_pixcoord,
                                                 vary_offset,
                                                 edgesTex,
                                                 areaTex,
                                                 searchTex,
                                                 vec4(0.0)
                                                 );
}
