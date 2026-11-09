// alpha_pass.inc: the loose uniform blocks of the native alpha programs (alpha.vert and the alpha fragment shaders include this file, so both stages declare the same blocks).
// Every name and type is the one the viewer's alpha programs use: the renderer copies the values by name when the native program stands in (Program::adoptFrom).

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    // the scene table of the record: three buffer addresses (rows, model matrices, texture matrices), written by the draw record
    uvec2 ll_packet_table;
    uvec2 ll_packet_models;
    uvec2 ll_packet_texmats;

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
};

layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    float minimum_alpha;        // the alpha cutoff of the draw (changes with the draw: its own block)
};
