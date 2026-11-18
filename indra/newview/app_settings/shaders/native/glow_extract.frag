// glow_extract.frag: the glow's source: the lit picture with the alpha channel set to how much of it glows (the larger of the picture's own alpha and the luminance, or the warmth, above a threshold times a
// maximum). A translation of "Glow Extract Shader (Post)" and "... (+Noise)" (effects/glowExtractF.glsl, HAS_NOISE for the dithered one); pairs with fullscreen.vert.
// What is different: only the term that counts is made (warmthAmount is 0 unless the user asks for the warmth: mix(lum, warmth, 0) is the luminance, and mix(lum, warmth, 1) the warmth).

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
#ifdef HAS_NOISE
    vec2 screen_res;
#endif
    float minLuminance;
    float maxExtractAlpha;
    vec3 lumWeights;
    vec3 warmthWeights;
    float warmthAmount;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseMap;
#ifdef HAS_NOISE
    uint ll_ti_glowNoiseMap;
#endif
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 col = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_fragcoord);
    float amount = 0.0;
    if (warmthAmount < 1.0)
    {
        // the luminance (NTSC weights, http://en.wikipedia.org/wiki/Luma_%28video%29)
        amount = (1.0 - warmthAmount) * smoothstep(minLuminance, minLuminance + 1.0, dot(col.rgb, lumWeights));
    }
    if (warmthAmount > 0.0)
    {
        amount += warmthAmount * smoothstep(minLuminance, minLuminance + 1.0, max(col.r * warmthWeights.r, max(col.g * warmthWeights.g, col.b * warmthWeights.b)));
    }

#ifdef HAS_NOISE
    // dithering: reduces the banding of the reduced precision glow buffer (the noise map is 128 by 128; the extraction is in screen space)
    const float TRUE_NOISE_RES = 128.0;
    const vec3 glow_noise = texture(ll_tbl2d[LL_HM(ll_ti_glowNoiseMap)], vary_fragcoord * (screen_res / TRUE_NOISE_RES)).xyz;
    const float NOISE_DEPTH = 64.0;
    col.rgb = max(col.rgb + glow_noise / NOISE_DEPTH, vec3(0.0));
#endif
    frag_color = vec4(col.rgb, max(col.a, amount * maxExtractAlpha));
}
