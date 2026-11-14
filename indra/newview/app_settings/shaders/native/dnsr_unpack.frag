// dnsr_unpack.frag: the denoised reflection back into the form the lighting pass reads (PLAN-amd-denoiser.md): the denoiser's colour with the alpha of the RT Reflection pass (how much of the pixel was
// traced; the lighting pass blends by it). A pixel that was not traced (a = 0) keeps what the pass wrote. Pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseMap;      // the denoiser's output (rgb)
    uint ll_ti_bumpMap;         // the RT Reflection pass's colour (rgb; a = how much of the pixel was traced)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 orig = texture(ll_tbl2d[LL_HM(ll_ti_bumpMap)], tc);
    vec3 den = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], tc).rgb;
    frag_color = orig.a > 0.0 ? vec4(max(den, vec3(0.0)), orig.a) : orig;
}
