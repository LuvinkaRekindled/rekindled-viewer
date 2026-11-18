// dnsr_sununpack.frag: the denoised sun shadow back into the sun lightmap (PLAN-amd-denoiser.md): a copy of the lightmap whose r channel (the sun shadow) is the denoiser's, except where the centre ray's
// answer was a partial value (the avatar's self shadow, hair and glass translucency: 0.02 .. 0.98), which the denoiser's binary mask cannot carry. g, b and a (occlusion, the projector shadows) are
// untouched. Pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseMap;      // the sun lightmap as the sun pass wrote it
    uint ll_ti_bumpMap;         // the denoiser's output (r: the filtered visibility, 1 lit)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 orig = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], tc);
    if (orig.r <= 0.02 || orig.r >= 0.98)
    {
        orig.r = clamp(texture(ll_tbl2d[LL_HM(ll_ti_bumpMap)], tc).r, 0.0, 1.0);
    }
    frag_color = orig;
}
