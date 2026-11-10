// alpha_pass.inc: the loose uniform blocks of the native alpha programs (alpha.vert and the alpha fragment shaders include this file, so both stages declare the same blocks).
// Every name and type is the one the viewer's alpha programs use: the renderer copies the values by name when the native program stands in (Program::adoptFrom).
// ALPHA_LIT (a define of the manifest line) adds what a lit surface needs: the sun, the shadow cascades, the lights.

// ALPHA_PBR (a glTF blend surface: alpha_pbr.vert / alpha_pbr.frag) uses the same probes and screen space reflections as a material one
#if defined(ALPHA_MAT) || (defined(ALPHA_PBR) && !defined(ALPHA_PBR_HUD))
#define ALPHA_REFLECTS 1
#endif

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    // the scene table of the record: three buffer addresses (rows, model matrices, texture matrices), written by the draw record
    uvec2 ll_packet_table;
    uvec2 ll_packet_models;
    uvec2 ll_packet_texmats;
#ifdef ALPHA_PBR
    uvec2 ll_packet_pbr;        // the glTF material rows of the record (10 vec4 a row: four texture transforms, roughness / metallic / emissive, the cutoff)
#endif

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

    // the water plane (eye space), the water fog, and the clip plane of a mirror view
    vec4 waterPlane;
    float waterSign;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
    float mirror_flag;
    vec4 clipPlane;

#ifdef AVATAR_PASS
    // the system avatar (avatar.vert): the projection (its palette matrices are in eye space) and, with the cloth on, the wind of the clothing
    mat4 projection_matrix;
#ifdef AVATAR_CLOTH
    vec4 gWindDir;
    vec4 gSinWaveParams;
    vec4 gGravity;
#endif
#endif

#ifdef TERRAIN_PASS
    // the legacy terrain (terrain.vert): the plane the detail textures are projected from (the texture matrix is not a uniform here: the pull path takes it from the row, row_texture_matrix)
    vec4 object_plane_s;
    vec4 object_plane_t;
#endif

#ifdef ALPHA_LIT
    int classic_mode;
    float sky_sunlight_scale;
    float sky_ambient_scale;
    vec3 sun_dir;
    vec3 moon_dir;
    vec2 screen_res;

    // the sun shadow cascades
    mat4 shadow_matrix[6];
    vec4 shadow_clip;
    vec2 shadow_res;
    float shadow_bias;
    float shadow_offset;

    // the hardware light slots (slot 1: the avatar backlight, 2-7: the nearest local lights when there is no list)
    vec4 light_position[8];
    vec3 light_direction[8];
    vec4 light_attenuation[8];
    vec3 light_diffuse[8];

    // the light list of the view (LLRender::setAlphaLights): three vec4 a light, alpha_light_count -1: no list
    int alpha_light_count;
    vec4 alpha_lights[192];
#endif

#ifdef ALPHA_PROBES
    mat3 env_mat;               // world to the probes' frame (the sky's rotation)
    float max_probe_lod;
    int cube_snapshot;
#endif

#if defined(ALPHA_REFLECTS) && defined(SSR)
    // screen space reflections: the previous frame's camera (the picture and depth are its), the marcher's settings
    mat4 projection_matrix;
    mat4 inv_proj;
    mat4 inv_modelview_delta;
    float iterationCount;
    float rayStep;
    float distanceBias;
    float depthRejectBias;
    float adaptiveStepMultiplier;
#endif
};

#ifdef ALPHA_PROBES
// the reflection probes of the view: one block for the whole frame, bound by the renderer (named block 0 = binding 1). The members and their order are the viewer's (the CPU side writes the block).
#define MAX_REFMAP_COUNT 256
layout(std140, set = 0, binding = 1) uniform ReflectionProbes
{
    mat4 refBox[MAX_REFMAP_COUNT];          // the box of a user override probe: three outward planes and the box's thickness along them
    mat4 heroBox;
    vec4 refSphere[MAX_REFMAP_COUNT];       // bounding spheres of the probes, the closest to the camera first
    vec4 refParams[MAX_REFMAP_COUNT];       // x irradiance scale, y radiance scale, z fade in, w near
    vec4 heroSphere;
    ivec4 refIndex[MAX_REFMAP_COUNT];       // x cube map layer, y index of the neighbour list (in ivec4s), z neighbour count, w priority (negative: a box probe)
    ivec4 refNeighbor[1024];
    ivec4 refBucket[256];                   // by depth: the first and the one past the last probe that can reach it
    int refmapCount;
    int heroShape;
    int heroMipCount;
    int heroProbeCount;
};
#endif

layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    float minimum_alpha;        // the alpha cutoff of the draw (changes with the draw: its own block)
};

#if defined(ALPHA_LIT) || defined(ALPHA_PROBES) || defined(AVATAR_PASS)
layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
#ifdef AVATAR_PASS
    uint ll_ti_diffuseMap;              // the body part's texture
#endif
#ifdef ALPHA_LIT
    uint ll_ti_shadow_shadowMap0;
    uint ll_ti_shadow_shadowMap1;
    uint ll_ti_shadow_shadowMap2;
    uint ll_ti_shadow_shadowMap3;
#endif
#ifdef ALPHA_PROBES
    uint ll_ti_cubearray_irradianceProbes;
    uint ll_ti_cubearray_reflectionProbes;
#ifdef HERO_PROBES
    uint ll_ti_cubearray_heroProbes;
#endif
#endif
#ifdef ALPHA_MAT
    uint ll_ti_lightFunc;               // the sun's specular lobe table
#endif
#ifdef ALPHA_PBR
    uint ll_ti_brdfLut;                 // the split sum lookup of the image based light (x: scale of the specular colour, y: bias)
#endif
#if defined(ALPHA_REFLECTS) && defined(SSR)
    uint ll_ti_sceneMap;                // the previous frame's picture and depth
    uint ll_ti_sceneDepth;
#endif
};
#endif
