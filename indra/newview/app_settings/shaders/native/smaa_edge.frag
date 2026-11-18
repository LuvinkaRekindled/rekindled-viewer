// smaa_edge.frag: SMAA's edge detection: the colour edges of the picture. A line for line translation of "SMAA Edge Detection" (deferred/SMAAEdgeDetectF.glsl); pairs with smaa_edge.vert.
// (the predication texture is only there with SMAA_PREDICATION, which the manifest lines set to 0)

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec4 SMAA_RT_METRICS;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
#if SMAA_PREDICATION
    uint rk_ti_predicationTex;
#endif
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in vec4 vary_offset[3];
layout(location = 0) out vec4 frag_color;

#include "lib/smaa.glsl"

// (after the library: its functions have parameters of these names)
#define diffuseRect rk_tbl2d[LL_HM(rk_ti_diffuseRect)]
#if SMAA_PREDICATION
#define predicationTex rk_tbl2d[LL_HM(rk_ti_predicationTex)]
#endif

void main()
{
    vec2 val = SMAAColorEdgeDetectionPS(vary_texcoord0,
                                          vary_offset,
                                          diffuseRect
                                          #if SMAA_PREDICATION
                                          , predicationTex
                                          #endif
                                          );
    frag_color = float4(val,0.0,0.0);
}
