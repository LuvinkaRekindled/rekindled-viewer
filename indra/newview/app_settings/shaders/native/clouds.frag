// clouds.frag: the cloud layer's colour and opacity from two noise textures (blended by blend_factor) looked up at four scrolled coordinates, with self shadowing; written as the emissive colour (or the albedo
// without the emissive target) with alpha 'alpha1' and the flag that keeps the lighting resolve's atmosphere off. A line for line translation of "Deferred Windlight Cloud Program pull" (deferred/cloudsF.glsl,
// including its early discard where there is no cloud); pairs with clouds.vert.

#include "sky_pass.inc"
#include "lib/flags.glsl"

layout(location = 0) in vec3 vary_CloudColorSun;
layout(location = 1) in vec3 vary_CloudColorAmbient;
layout(location = 2) in float vary_CloudDensity;
layout(location = 3) in vec2 vary_texcoord0;
layout(location = 4) in vec2 vary_texcoord1;
layout(location = 5) in vec2 vary_texcoord2;
layout(location = 6) in vec2 vary_texcoord3;
layout(location = 7) in float altitude_blend_factor;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

vec4 cloudNoise(vec2 uv)
{
    vec4 a = texture(ll_tbl2d[LL_HM(ll_ti_cloud_noise_texture)], uv);
    if (blend_factor <= 0.0)
    {
        return a;       // (mix(a, b, 0) is a: the driver sets the factor to 0 with one noise texture or no scroll; the second image is not read then)
    }
    vec4 b = texture(ll_tbl2d[LL_HM(ll_ti_cloud_noise_texture_next)], uv);
    vec4 cloud_noise_sample = mix(a, b, blend_factor);
    return cloud_noise_sample;
}

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    // Set variables
    vec2 uv1 = vary_texcoord0.xy;
    vec2 uv2 = vary_texcoord1.xy;
    vec3 cloudColorSun = vary_CloudColorSun;
    vec3 cloudColorAmbient = vary_CloudColorAmbient;
    float cloudDensity = vary_CloudDensity;
    vec2 uv3 = vary_texcoord2.xy;
    vec2 uv4 = vary_texcoord3.xy;

    if (cloud_scale < 0.001)
    {
        discard;
    }

    // (both are the noise times cloud_variance: exactly 0 without a variance, and then the four lookups are not made)
    vec2 disturbance = vec2(0.0);
    vec2 disturbance2 = vec2(0.0);
    if (cloud_variance != 0.0)
    {
        disturbance  = vec2(cloudNoise(uv1 / 8.0f).x, cloudNoise((uv3 + uv1) / 16.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);
        disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0f).x, cloudNoise((uv4 + uv2) / 8.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);
    }

    // Offset texture coords
    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);    //large texture, visible density
    uv2 += cloud_pos_density1.xy;   //large texture, self shadow
    uv3 += cloud_pos_density2.xy;   //small texture, visible density
    uv4 += cloud_pos_density2.xy;   //small texture, self shadow

    float density_variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x + disturbance2.y) * 4.0);

    cloudDensity *= 1.0 - (density_variance * density_variance);

    // Compute alpha1, the main cloud opacity
    float alpha1 = (cloudNoise(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;
    alpha1 = min(max(alpha1 + cloudDensity, 0.) * 10 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 = 1. - alpha1 * alpha1;

    alpha1 *= altitude_blend_factor;

    alpha1 = clamp(alpha1, 0.0, 1.0);

    // <FS> where there is no cloud the blend (source alpha 0) leaves the destination exactly as it is in every attachment, so the pixel is thrown away here:
    // no second noise lookup, no colour work, and no blended write into the four G-buffer targets for the clear parts of the sky. The picture is identical.
    if (alpha1 <= 0.0)
    {
        discard;
    }

    // Compute alpha2, for self shadowing effect
    // (1 - alpha2) will later be used as percentage of incoming sunlight
    float alpha2 = (cloudNoise(uv2).x - 0.5);
    alpha2 = min(max(alpha2 + cloudDensity, 0.) * 2.5 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha2 = 1. - alpha2;
    alpha2 = 1. - alpha2 * alpha2;

    // Combine
    vec3 cloud_color_out;
    cloud_color_out = (cloudColorSun * (1. - alpha2) + cloudColorAmbient);

    cloud_color_out.rgb = clamp(cloud_color_out.rgb, vec3(0), vec3(1));

    cloud_color_out.rgb *= 2.0;

    /// Gamma correct for WL (soft clip effect).
    frag_spec = vec4(0.0, 0.0, 0.0, 0.0);
    frag_normal = vec4(0, 0, 0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_albedo = vec4(0);
    frag_emissive = vec4(cloud_color_out.rgb, alpha1);
#else
    frag_albedo = vec4(cloud_color_out.rgb, alpha1);
#endif
}
