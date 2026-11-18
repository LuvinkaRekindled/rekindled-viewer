// resolve_pass.inc: the loose uniform blocks of the native lighting resolve (resolve.frag). Every name and type is the one the viewer's soften program uses: the renderer copies the values by name when the native
// program stands in (Program::adoptFrom). Defines of the manifest line: RESOLVE_PASS, ALPHA_PROBES (the probe block below); the viewer's own HAS_EMISSIVE, SSR, REFMAP_LEVEL, REF_SAMPLE_COUNT, HERO_PROBES.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 inv_proj;
    vec2 screen_res;

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
    float sky_hdr_scale;
    float sky_sunlight_scale;
    float sky_ambient_scale;

    int classic_mode;
    vec3 sun_dir;
    vec3 moon_dir;

    // ambient occlusion's effect on the sky light
    mat3 ssao_effect_mat;
    float ssao_irradiance_scale;
    float ssao_irradiance_max;

    // the probes
    mat3 env_mat;               // world to the probes' frame (the sky's rotation)
    float max_probe_lod;
    int cube_snapshot;
    int resolve_dbg;            // live debug view of the resolve (rt-debug.txt resolve_dbg), 0 = none
    vec4 clipPlane;             // the hero probe fades in across a mirror's clip plane
    int rt_glossy_reflections;  // 0: no traced reflection for this view (the RT Reflection pass did not run), 1-2: it replaces the probe result, 3, 4 and 7, 8: debug views of it

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
    float ssrTraceMode;                 // bits of the improved reflection trace (pipeline.cpp ssr_trace_mode_bits): 1 water, 2 other surfaces (read here), 4 water takes fewer steps
#endif
};

#include "lib/probe_block.glsl"

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_specularRect;
    uint rk_ti_normalMap;
    uint rk_ti_depthMap;
    uint rk_ti_lightMap;
    uint rk_ti_lightFunc;
    uint rk_ti_brdfLut;
    uint rk_ti_rtReflMap;               // the RT Reflection pass's output: rgb the traced reflection, a how much of it replaces the probe result (-1 traced and missed, 0 not traced)
#ifdef HAS_EMISSIVE
    uint rk_ti_emissiveRect;
#endif
    uint rk_ti_cubearray_irradianceProbes;
    uint rk_ti_cubearray_reflectionProbes;
#ifdef HERO_PROBES
    uint rk_ti_cubearray_heroProbes;
    uint rk_ti_mirrorView;              // the planar mirror's reflected view (the old programs' sampler mirrorView)
#endif
#ifdef SSR
    uint rk_ti_sceneMap;
    uint rk_ti_sceneDepth;
#endif
};
