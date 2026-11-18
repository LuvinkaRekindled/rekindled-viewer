// sky.frag: the sky dome's colour: the vertex haze colour plus the rainbow and the ice halo, times two, clamped; written as the emissive colour (or the albedo without the emissive target) with the flag that
// keeps the lighting resolve's atmosphere off the sky. A line for line translation of "Deferred Windlight Sky Shader pull" (deferred/skyF.glsl); pairs with sky.vert. With HDRI (the manifest line "Environment Map Program pull": an HDRI image is loaded) the part of the
// screen left of the split line shows the image instead of the sky (the HAS_HDRI part of the old shader).

#include "sky_pass.inc"
#include "lib/flags.glsl"

layout(location = 0) in vec3 vary_HazeColor;
layout(location = 1) in float vary_LightNormPosDot;
#ifdef HDRI
layout(location = 2) in vec4 vary_position;
layout(location = 3) in vec3 vary_rel_pos;
#endif

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

#define PI 3.14159265

vec3 rainbow(float d)
{
    // Interesting values of d are -0.75 .. -0.825, i.e. when view vec nearly opposite of sun vec
    // Rainbox tex is mapped with REPEAT, so -.75 as tex coord is same as 0.25.  -0.825 -> 0.175. etc.
    // SL-13629
    // Unfortunately the texture is inverted, so we need to invert the y coord, but keep the interesting
    // part within the same 0.175..0.250 range, i.e. d = (1 - d) - 1.575
    d         = clamp(-0.575 - d, 0.0, 1.0);

    // With the colors in the lower 1/4 of the texture, inverting the coords leaves most of it inaccessible.
    // So, we can stretch the texcoord above the colors (ie > 0.25) to fill the entire remaining coordinate
    // space. This improves gradation, reduces banding within the rainbow interior. (1-0.25) / (0.425/0.25) = 4.2857
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;

    float rad = (droplet_radius - 5.0f) / 1024.0f;
    return pow(texture(rk_tbl2d[LL_HM(rk_ti_rainbow_map)], vec2(rad + 0.5, d)).rgb, vec3(1.8)) * moisture_level;
}

vec3 halo22(float d)
{
    d       = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1 - (d * d), 0, 1));
    return texture(rk_tbl2d[LL_HM(rk_ti_halo_map)], vec2(0, v)).rgb * ice_level;
}

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    vec3 sky_color;

#ifdef HDRI
    const vec3 frag_coord = vary_position.xyz / vary_position.w;
    if (-frag_coord.x > ((1.0 - hdri_split_screen) * 2.0 - 1.0))
    {
        vec3 pos = normalize(vary_rel_pos);
        pos = env_mat * pos;
        const vec2 tex_coord = vec2(atan(pos.z, pos.x) + PI, acos(pos.y)) / vec2(2.0 * PI, PI);
        sky_color = textureLod(rk_tbl2d[LL_HM(rk_ti_environmentMap)], tex_coord, 0.0).rgb * sky_hdr_scale;
        sky_color = min(sky_color, vec3(8192.0 * 8192.0 * 16.0));       // stupidly large value arrived at by binary search -- avoids framebuffer corruption from some HDRIs
        frag_normal = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_HAS_HDRI);
    }
    else
#endif
    {
        // Potential Fill-rate optimization.  Add cloud calculation
        // back in and output alpha of 0 (so that alpha culling kills
        // the fragment) if the sky wouldn't show up because the clouds
        // are fully opaque.
        sky_color = vary_HazeColor;

        float  rel_pos_lightnorm = vary_LightNormPosDot;
        float optic_d = rel_pos_lightnorm;
        vec3  halo_22 = halo22(optic_d);
        sky_color.rgb += rainbow(optic_d);
        sky_color.rgb += halo_22;
        sky_color.rgb *= 2.;
        sky_color.rgb = clamp(sky_color.rgb, vec3(0), vec3(5));
        frag_normal = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
    }

    frag_spec = vec4(0);
#if defined(HAS_EMISSIVE)
    frag_albedo = vec4(0);
    frag_emissive = vec4(sky_color.rgb, 1.0);
#else
    frag_albedo = vec4(sky_color.rgb, 1.0);
#endif
}
