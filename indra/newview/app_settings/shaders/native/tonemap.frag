// tonemap.frag: the tone mapping and gamma of the post chain: the lit linear colour times the exposure of the exposure target, the tone curve (PBR neutral or ACES, mixed with the plain exposed colour by
// tonemap_mix), then optionally the sRGB encoding and the legacy gamma. A line for line translation of the six "... Tonemap ... Post Process" programs (deferred/postDeferredTonemap.glsl and
// tonemapUtilF.glsl); pairs with fullscreen.vert. Defines: NO_POST (no tone curve, clamp only), GAMMA_CORRECT, LEGACY_GAMMA.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float gamma;
    float exposure;
    float tonemap_mix;
    int tonemap_type;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_exposureMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(1.0));
    return mix(1.055 * pow(c, vec3(0.41666)) - 0.055, c * 12.92, lessThan(c, vec3(0.0031308)));
}

//===============================================================
// tone mapping taken from Khronos sample implementation
//===============================================================

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
const mat3 ACESInputMat = mat3
(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);

// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat = mat3
(
    1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602
);

// ACES filmic tone map approximation
// see https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
vec3 RRTAndODTFit(vec3 color)
{
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}

// tone mapping
vec3 toneMapACES_Hill(vec3 color)
{
    color = ACESInputMat * color;

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = ACESOutputMat * color;

    // Clamp to [0, 1]
    color = clamp(color, 0.0, 1.0);

    return color;
}

// Khronos Neutral tonemapping
// https://github.com/KhronosGroup/ToneMapping/tree/main
// Input color is non-negative and resides in the Linear Rec. 709 color space.
// Output color is also Linear Rec. 709, but in the [0, 1] range.
vec3 PBRNeutralToneMapping(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1. - startCompression;
    float newPeak = 1. - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
    return mix(color, newPeak * vec3(1, 1, 1), g);
}

vec3 toneMap(vec3 color)
{
#ifndef NO_POST
    vec3 linear_input_color = color;

    float exp_scale = texture(rk_tbl2d[LL_HM(rk_ti_exposureMap)], vec2(0.5, 0.5)).r;
    float final_exposure = exposure * exp_scale;

    vec3 exposed_color = color * final_exposure;
    vec3 tonemapped_color = exposed_color;

    switch (tonemap_type)
    {
    case 0:
        tonemapped_color = PBRNeutralToneMapping(exposed_color);
        break;
    case 1:
        tonemapped_color = toneMapACES_Hill(exposed_color);
        break;
    }

    vec3 exposed_linear_input = linear_input_color * final_exposure;

    color = mix(exposed_linear_input, tonemapped_color, tonemap_mix);
    color = clamp(color, 0.0, 1.0);
#else
    color *= exposure * texture(rk_tbl2d[LL_HM(rk_ti_exposureMap)], vec2(0.5, 0.5)).r;
    color = clamp(color, 0.0, 1.0);
#endif
    return color;
}

#ifdef GAMMA_CORRECT
vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma));       // s/b inverted already CPU-side
    return c;
}
#endif

void main()
{
    // this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], vary_fragcoord);

#ifndef NO_POST
    diff.rgb = toneMap(diff.rgb);
#else
    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
#endif

#ifdef GAMMA_CORRECT
    diff.rgb = linear_to_srgb(diff.rgb);
#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif
#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));       // We should always be 0-1 past this point

    frag_color = diff;
}
