// waterhaze.frag: the water fog over what is under the water plane: the fog colour and how much of the surface's own light gets through, as a blend of (ONE, SOURCE_ALPHA). Above the water it also discards
// outside the exclusion mask and depth tests by hand. A line for line translation of "Water Haze Shader" (class 3 deferred/waterHazeF.glsl); pairs with waterhaze.vert.

#define LEGACY_COLOR 1
#define LEGACY_DEPTH 1
#define LEGACY_WATER 1

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    uvec2 rk_packet_table;      // the geometry is read from the record's rows (the water patches above the water, the full screen triangle under it): three buffer addresses written by the draw record
    uvec2 rk_packet_models;
    uvec2 rk_packet_texmats;
    mat4 inv_proj;
    vec4 waterPlane;
    int above_water;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_exclusionTex;
    uint rk_ti_depthMap;
};

#include "lib/legacy.glsl"

layout(location = 0) in vec4 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

float getDepth(vec2 pos_screen)
{
    float depth = texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], pos_screen).r;
    return depth;
}

void main()
{
    vec2  tc           = vary_fragcoord.xy / vary_fragcoord.w * 0.5 + 0.5;
    float depth        = getDepth(tc.xy);
    float mask = texture(rk_tbl2d[RK_HM(rk_ti_exclusionTex)], tc.xy).r;

    if (above_water > 0)
    {
        // Just discard if we're in the exclusion mask.
        // The previous invisiprim hack we're replacing would also crank up water fog desntiy.
        // But doing that makes exclusion surfaces very slow as we'd need to render even more into the mask.
        // - Geenz 2025-02-06
        if (mask < 1)
        {
            discard;
        }

        // we want to depth test when the camera is above water, but some GPUs have a hard time
        // with depth testing against render targets that are bound for sampling in the same shader
        // so we do it manually here
        float cur_depth = vary_fragcoord.z / vary_fragcoord.w * 0.5 + 0.5;
        if (cur_depth > depth)
        {
            discard;
        }
    }

    vec4  pos          = getPositionWithDepth(tc, depth);

    vec4 fogged = getWaterFogView(pos.xyz);
    fogged.a = max(pow(fogged.a, 1.7), 0);

    frag_color = max(fogged, vec4(0));  // output linear since local lights will be added to this shader's results
}
