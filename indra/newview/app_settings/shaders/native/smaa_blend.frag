// smaa_blend.frag: SMAA's neighbourhood blending: the picture blended along the edges by the weights. A line for line translation of "SMAA Neighborhood Blending"
// (deferred/SMAANeighborhoodBlendF.glsl); pairs with smaa_blend.vert. (the velocity texture is only there with SMAA_REPROJECTION, which the manifest lines set to 0)

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    vec4 SMAA_RT_METRICS;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseRect;
    uint ll_ti_blendTex;
#if SMAA_REPROJECTION
    uint ll_ti_velocityTex;
#endif
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in vec4 vary_offset;
layout(location = 0) out vec4 frag_color;

#include "lib/color.glsl"      // (the library converts between sRGB and linear: the viewer's own functions, as the stock library files gave it)
#include "lib/smaa.glsl"

// (after the library: its functions have parameters of these names)
#define diffuseRect ll_tbl2d[LL_HM(ll_ti_diffuseRect)]
#define blendTex ll_tbl2d[LL_HM(ll_ti_blendTex)]
#if SMAA_REPROJECTION
#define velocityTex ll_tbl2d[LL_HM(ll_ti_velocityTex)]
#endif

void main()
{
    frag_color = SMAANeighborhoodBlendingPS(vary_texcoord0,
                                            vary_offset,
                                            diffuseRect,
                                            blendTex
                                            #if SMAA_REPROJECTION
                                            , velocityTex
                                            #endif
                                            );
}
