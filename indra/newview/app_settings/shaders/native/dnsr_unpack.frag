// dnsr_unpack.frag: the denoised reflection back into the form the lighting pass reads (PLAN-amd-denoiser.md): the denoiser's colour with the alpha of the RT Reflection pass (how much of the pixel was
// traced; the lighting pass blends by it). A pixel that was not traced (a = 0) keeps what the pass wrote. Pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    float dbg_mode;             // rt-debug.txt dnsrdbg=: 0 the denoised colour, 1 the raw traced colour (no denoiser), 2 the difference x 8, 3 magenta where the denoiser wrote black over a lit pixel
};

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
    den = max(den, vec3(0.0));
    if (dbg_mode > 2.5)
    {
        if (dot(den, vec3(1.0)) == 0.0 && dot(orig.rgb, vec3(1.0)) > 0.0)
        {
            den = vec3(1.0, 0.0, 1.0);
        }
        else
        {
            den = orig.rgb;
        }
    }
    else if (dbg_mode > 1.5)
    {
        den = abs(den - orig.rgb) * 8.0;
    }
    else if (dbg_mode > 0.5)
    {
        den = orig.rgb;
    }
    frag_color = orig.a > 0.0 ? vec4(den, orig.a) : orig;
}
