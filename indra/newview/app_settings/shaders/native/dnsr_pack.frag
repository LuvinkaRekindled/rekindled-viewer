// dnsr_pack.frag: the radiance the AMD FidelityFX reflection denoiser takes (PLAN-amd-denoiser.md): the traced reflection's colour in rgb and, in the alpha channel where the denoiser reads it as the
// length of the reflected ray, the hit distance (metres from the surface to what the ray hit, the second output of the RT Reflection pass; 0 where nothing was traced). Pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseMap;      // the RT Reflection pass's colour (rgb; a = how much of the pixel was traced)
    uint ll_ti_bumpMap;         // its hit distance (r, metres)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 refl = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], tc);
    float dist = texture(ll_tbl2d[LL_HM(ll_ti_bumpMap)], tc).r;
    frag_color = vec4(refl.a > 0.0 ? refl.rgb : vec3(0.0), refl.a > 0.0 ? dist : 0.0);
}
