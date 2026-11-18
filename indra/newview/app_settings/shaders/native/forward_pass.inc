// forward_pass.inc: the uniform blocks of the forward lit programs (forward_lit.vert / forward_lit.frag: "Avatar Shader" and "No color alpha mask Shader", the old forward passes of LLDrawPoolAvatar that the avatar
// previews of the appearance editor and the animation / image preview floaters draw with). Both stages declare the same blocks, so both include this file. FWD_AVATAR: the skinned avatar (the matrix palette,
// the projection matrix); otherwise the object with the matrices of the draw.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    // the sky (the old lighting chain: atmospherics of the vertex, the haze colour of the fragment)
    vec3 sun_dir;
    vec3 moon_dir;
    int sun_up_factor;
    float sky_hdr_scale;
    vec3 lightnorm;
    vec3 sunlight_color;
    vec3 moonlight_color;
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
    float sky_ambient_scale;
    int classic_mode;
    // the preview's lights and the surface colour
    vec4 light_position[8];
    vec3 light_diffuse[8];
    vec4 color;
    float minimum_alpha;
#ifdef FWD_AVATAR
    mat4 projection_matrix;
    vec4 matrixPalette[45];
#endif
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
#ifndef FWD_AVATAR
    mat3 normal_matrix;
    mat4 texture_matrix0;
    mat4 modelview_matrix;
    mat4 modelview_projection_matrix;
#endif
    uint rk_ti_diffuseMap;
};
