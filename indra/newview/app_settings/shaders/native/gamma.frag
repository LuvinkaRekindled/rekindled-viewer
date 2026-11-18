// gamma.frag: the picture (linear colour) converted to sRGB for the screen, and with LEGACY_GAMMA the old gamma on top. A line for line translation of "Deferred Gamma Correction Post Process" and
// "Legacy Gamma Correction Post Process" (deferred/postDeferredGammaCorrect.glsl, LEGACY_GAMMA); pairs with fullscreen.vert.

#include "lib/color.glsl"

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float gamma;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

vec3 legacyGamma(vec3 color)
{
    vec3 c = 1. - clamp(color, vec3(0.), vec3(1.));
    c = 1. - pow(c, vec3(gamma)); // s/b inverted already CPU-side

    return c;
}

void main()
{
    //this is the one of the rare spots where diffuseRect contains linear color values (not sRGB)
    vec4 diff = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord);
    diff.rgb = linear_to_srgb(diff.rgb);

#ifdef LEGACY_GAMMA
    diff.rgb = legacyGamma(diff.rgb);
#endif

    diff.rgb = clamp(diff.rgb, vec3(0.0), vec3(1.0));
    frag_color = diff;
}
