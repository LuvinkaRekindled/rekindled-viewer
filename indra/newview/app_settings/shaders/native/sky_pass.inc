// sky_pass.inc: the loose uniform blocks of the translated sky programs (sky, clouds, sun disc, moon, stars). Every name and type is the one the viewer's old programs use (the values are copied by name).
// The blocks are the same in both stages of every program of the set.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    // the scene table of the record
    uvec2 rk_packet_table;
    uvec2 rk_packet_models;
    uvec2 rk_packet_texmats;

    // the sky of the pass
    vec3 camPosLocal;
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
    int cube_snapshot;

    // the sky dome's fragment shader
    float moisture_level;
    float droplet_radius;
    float ice_level;

    // the clouds
    vec3 cloud_color;
    float cloud_scale;
    float blend_factor;
    vec3 cloud_pos_density1;
    vec3 cloud_pos_density2;
    float cloud_variance;
    vec2 cloud_scroll_delta;       // how far the cloud noise scrolled since the frame before, in texture coordinates (the motion of the layer: clouds.frag; 0 where it does not scroll)

    // the moon, the stars
    vec3 moon_dir;
    float moon_brightness;
    float custom_alpha;
    float time;

#ifdef HDRI
    // the HDRI sky (the program "Environment Map Program pull": an HDRI image is loaded)
    float sky_hdr_scale;
    float hdri_split_screen;    // how much of the screen (from the left) shows the HDRI: 1 all of it, 0 none
    mat3 env_mat;               // the HDRI's rotation
#endif
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_rainbow_map;
    uint rk_ti_halo_map;
    uint rk_ti_diffuseMap;
    uint rk_ti_altDiffuseMap;
    uint rk_ti_cloud_noise_texture;
    uint rk_ti_cloud_noise_texture_next;
#ifdef HDRI
    uint rk_ti_environmentMap;  // the HDRI image (equirectangular)
#endif
};

layout(std140, set = 0, binding = 6) uniform RKLooseMaterial
{
    vec4 color;
};
