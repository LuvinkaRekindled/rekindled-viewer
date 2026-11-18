// haze.frag: the sky haze over the post deferred geometry: per pixel the atmospheric attenuation as the blend alpha and the haze light (linear, times sky_hdr_scale) as the colour; sky pixels are discarded.
// A line for line translation of "Haze Shader" (class 3 deferred/hazeF.glsl with softenLightV.glsl); pairs with fullscreen.vert. The helper functions are the old ones (lib/legacy.glsl).

#define LEGACY_COLOR 1
#define LEGACY_SKY 1
#define LEGACY_DEPTH 1

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 inv_proj;
    vec3 sun_dir;
    vec3 moon_dir;
    int sun_up_factor;
    vec4 waterPlane;
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
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
    uint rk_ti_normalMap;
};

#include "lib/legacy.glsl"

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

float getDepth(vec2 pos_screen)
{
    float depth = texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], pos_screen).r;
    return depth;
}

vec4 getNorm(vec2 screenpos)
{
    // (level 0 explicitly: the lookup now comes after the discard of the sky pixels, and an implicit level needs the neighbours of the quad; the G-buffer has no mip levels)
    vec4 norm = decodeNormal(textureLod(rk_tbl2d[RK_HM(rk_ti_normalMap)], screenpos.xy, 0.0));
    return norm;
}

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    if (depth >= 1.0)
    {
        //should only be true of sky, clouds, sun/moon, and stars: nothing to haze, no position, normal or atmosphere work for these pixels
        discard;
    }
    vec4  pos          = getPositionWithDepth(tc, depth);
    vec4  norm         = getNorm(tc);
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm.xyz, light_dir, sunlit, amblit, additive, atten);

    // mask off atmospherics below water (when camera is under water)
    bool do_atmospherics = false;
    if (dot(vec3(0), waterPlane.xyz) + waterPlane.w > 0.0 ||
        dot(pos.xyz, waterPlane.xyz) + waterPlane.w > 0.0)
    {
        do_atmospherics = true;
    }

    vec3  irradiance = vec3(0);
    vec3  radiance  = vec3(0);

    float alpha = 0.0;

    if (do_atmospherics)
    {
        alpha = atten.r;
        color = srgb_to_linear(additive * 2.0);
        color *= sky_hdr_scale;
    }
    else
    {
        color = vec3(0, 0, 0);
        alpha = 1.0;
    }

    frag_color = max(vec4(color.rgb, alpha), vec4(0));  // output linear since local lights will be added to this shader's results
}
