// light_pass.inc: the loose uniform blocks of the native deferred light programs (light_volume.vert, light_point.frag, light_multi.frag, light_spot.frag include this file, so both stages declare the same blocks).
// Every name and type is the one the viewer's light programs use: the renderer copies the values by name (Program::adoptFrom / the uniform calls of the pipeline). Defines of the manifest lines:
//   LIGHT_VOLUME      a light of the pass is a row of a table (rk_light_table, written by the draw record), drawn as the box of its volume: the vertex shader makes the box, the rows are read by instance
//   LIGHT_SPOT        (with LIGHT_VOLUME) the row is a projector's: 11 vec4 instead of 3
//   LIGHT_MULTI       up to 16 point lights that cover the whole screen, in uniform arrays, one triangle
//   LIGHT_MULTI_SPOT  one projector that covers the whole screen, its values in uniforms, one triangle

#ifdef LIGHT_SPOT
#define LIGHT_ROW_STRIDE 11u
#else
#define LIGHT_ROW_STRIDE 3u
#endif

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightRows { vec4 v[]; };

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
#ifdef LIGHT_VOLUME
    mat4 modelview_projection_matrix;
    mat4 modelview_matrix;
    uvec2 rk_light_table;           // the rows of the record (written by the worker when the draw runs)
#endif
    mat4 inv_proj;
    vec2 screen_res;

#ifdef LIGHT_MULTI
    int light_count;
    vec4 light[16];                 // .w = size
    vec4 light_col[16];             // .a = falloff
    float far_z;
    int classic_mode;
#endif

#ifdef LIGHT_MULTI_SPOT
    // the projector of this draw (setupSpotLight) and its light
    vec3 center;
    float size;
    vec3 color;
    float falloff;
    int classic_mode;
    mat4 proj_mat;                  // screen space to light space
    vec3 proj_p;                    // plane the projection is emitting from (in screen space)
    vec3 proj_n;
    vec3 proj_origin;               // origin of the projection, for the angular attenuation
    float proj_range;               // range between the near clip and the far clip plane of the projection
    float proj_ambiance;
    float proj_focus;               // distance from the plane where the blurring begins
    float proj_lod;                 // number of mips of the projected image
    int proj_shadow_idx;
    float shadow_fade;
#endif
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
    uint rk_ti_normalMap;
    uint rk_ti_diffuseRect;
    uint rk_ti_specularRect;
    uint rk_ti_lightFunc;
#if defined(LIGHT_SPOT) || defined(LIGHT_MULTI_SPOT)
    uint rk_ti_lightMap;            // the spot shadows (b and a)
#endif
#ifdef LIGHT_MULTI_SPOT
    uint rk_ti_projectionMap;
#endif
};
