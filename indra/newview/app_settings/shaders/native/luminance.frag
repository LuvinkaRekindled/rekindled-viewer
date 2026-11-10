// luminance.frag: the exposure sample: one luminance value from the lit colour (diffuseRect) and the emissive target of a window of the screen. A line for line translation of "Luminance"
// (deferred/luminanceF.glsl); pairs with fullscreen.vert. The sky is left out of the diffuse luminance scale (HDRI and no-atmosphere sky pixels, by the normal target's flag).

#define GBUFFER_FLAG_SKIP_ATMOS   0.0
#define GBUFFER_FLAG_HAS_HDRI     1.0
#define GET_GBUFFER_FLAG(data, flag)    (abs(data-flag)< 0.1)

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    float diffuse_luminance_scale;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseRect;
    uint ll_ti_emissiveRect;
    uint ll_ti_normalMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

float lum(vec3 col)
{
    vec3 l = vec3(0.2126, 0.7152, 0.0722);
    return dot(l, col);
}

void main()
{
    vec2 tc = vary_fragcoord * 0.6 + 0.2;
    tc.y -= 0.1;        // HACK - nudge exposure sample down a little bit to favor ground over sky

    vec3 c = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], tc).rgb;
    vec4 norm = texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], tc);

    if (!GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_HAS_HDRI) &&
        !GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_SKIP_ATMOS))
    {
        // Apply the diffuse luminance scale to objects but not the sky
        c *= diffuse_luminance_scale;
    }

    c += texture(ll_tbl2d[LL_HM(ll_ti_emissiveRect)], tc).rgb;

    float L = lum(c);
    frag_color = vec4(max(L, 0.0));
}
