// water_pass.inc: the loose uniform blocks of the native water programs (water.vert with water.frag, and water.vert with underwater.frag): both stages of a program include this file, so both declare the same
// blocks. Every name and type is the one the viewer's water programs use: the renderer copies the values by name (LLDrawPoolWater::renderPostDeferred, LLPipeline::bindDeferredShader).
// Defines: UNDERWATER (the program for a camera under the water: no lighting, the fog only), and the viewer's own TRANSPARENT_WATER (the setting RenderTransparentWater, pushed with the global defines),
// SUN_SHADOW, SSR, REFMAP_LEVEL, HERO_PROBES. The water surface has no draw record: it is drawn by attributes (position), so the matrices are loose draw values.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
#ifdef WATER_PULL
    // the table form of the water planes (the manifest lines "Water Pull Shader", "Underwater Pull Shader": WATER_PULL): the vertex buffers of the planes are read by address, three buffer addresses written by the
    // draw record. The matrices below are still the loose draw values (the record has no model matrix of its own: the planes are drawn under the matrices of the pass)
    uvec2 ll_packet_table;
    uvec2 ll_packet_models;
    uvec2 ll_packet_texmats;
#endif
    // the waves: where the camera is, how the two wave maps move
    vec2 waveDir1;
    vec2 waveDir2;
    float time;
    vec3 eyeVec;                // the camera in world space
    float waterHeight;
    float refScale;             // how far the picture below is pushed by the waves

    // a mirror view throws away what is behind its mirror plane
    float mirror_flag;
    vec4 clipPlane;

#ifdef UNDERWATER
    vec4 waterPlane;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
#ifndef TRANSPARENT_WATER
    vec3 waterFogColorLinear;
#endif
#else
    mat4 inv_proj;
    vec3 lightDir;
    vec3 specular;              // the sun's (or moon's) colour, scaled the old way
    float blend_factor;         // how far the first wave map has turned into the second
    float blurMultiplier;       // the water's roughness
    vec3 normScale;
    float fresnelScale;
    float fresnelOffset;
    int classic_mode;

    // the sky of the pass (the haze and the sun/moon terms)
    vec3 lightnorm;
    vec3 sunlight_color;
    vec3 moonlight_color;
    int sun_up_factor;
    vec3 ambient_color;
    vec3 blue_horizon;
    vec3 blue_density;
    float haze_horizon;
    float haze_density;
    float cloud_shadow;
    float density_multiplier;
    float distance_multiplier;
    float max_y;
    vec3 glow;
    float sun_moon_glow_factor;
    float sky_sunlight_scale;

#ifndef TRANSPARENT_WATER
    // without the picture below the water, the water is the fog's colour
    vec4 waterPlane;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
#endif

    // the sun shadow cascades
    mat4 shadow_matrix[6];
    vec4 shadow_clip;
    vec3 sun_dir;
    vec3 moon_dir;
    vec2 shadow_res;
    float shadow_bias;
    float shadow_offset;

    // the probes
    mat3 env_mat;               // world to the probes' frame (the sky's rotation)
    float max_probe_lod;
    int cube_snapshot;

#ifdef SSR
    // screen space reflections: the previous frame's camera (the picture and depth are its), the marcher's settings
    mat4 projection_matrix;
    mat4 inv_modelview_delta;
    float iterationCount;
    float rayStep;
    float distanceBias;
    float depthRejectBias;
    float glossySampleCount;
    float adaptiveStepMultiplier;
    float ssrTraceMode;         // bits of the improved reflection trace (pipeline.cpp ssr_trace_mode_bits): 1 water, 2 other surfaces, 4 water takes fewer steps
#endif
#endif
};

#ifndef UNDERWATER
#include "lib/probe_block.glsl"
#endif

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_matrix;
    mat3 normal_matrix;
    mat4 modelview_projection_matrix;
    uint ll_ti_bumpMap;
    uint ll_ti_exclusionTex;    // 1 where the water shows, 0 where an exclusion surface cuts it out
#ifdef TRANSPARENT_WATER
    uint ll_ti_screenTex;       // the picture below the water (mWaterDis)
#endif
#ifndef UNDERWATER
    uint ll_ti_bumpMap2;
#ifdef TRANSPARENT_WATER
    uint ll_ti_depthMap;
#endif
    uint ll_ti_shadow_shadowMap0;
    uint ll_ti_shadow_shadowMap1;
    uint ll_ti_shadow_shadowMap2;
    uint ll_ti_shadow_shadowMap3;
    uint ll_ti_cubearray_irradianceProbes;
    uint ll_ti_cubearray_reflectionProbes;
#ifdef HERO_PROBES
    uint ll_ti_cubearray_heroProbes;
    uint ll_ti_mirrorView;      // the planar mirror's reflected view
#endif
#ifdef SSR
    uint ll_ti_sceneMap;        // the previous frame's picture and depth
    uint ll_ti_sceneDepth;
#endif
#endif
};
