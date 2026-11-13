// avatar_shadow.frag: the system avatar's shadow caster. Plain (no define): depth only, as shadow.frag. SHADOW_ALPHA: the texture's alpha decides (the hair, the eyelashes, the skin of an alpha layer): none below
// 0.05 casts, below minimum_alpha only every other pixel column casts (a screen door, the shadow map is filtered afterwards). SHADOW_COLOR_ALPHA (the alpha shadow program): the draw's colour alpha counts too.
// The column is the pixel's own (gl_FragCoord): the old programs computed `floor(0.5 * (width - 1) * ndc.x)` from two varyings and a division, which is the same column for a shadow map whose width is a
// multiple of four (every one the viewer makes) and a phase that wandered over the map for the others.

#include "avatar_shadow.inc"

#ifdef SHADOW_ALPHA
layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec2 vary_texcoord0;
#endif

layout(location = 0) out vec4 frag_color;

void main()
{
#ifdef SHADOW_ALPHA
    float alpha = texture(ll_tbl2d[nonuniformEXT(LL_HM(ll_ti_diffuseMap))], vary_texcoord0).a;
#ifdef SHADOW_COLOR_ALPHA
    alpha *= color.a;
#endif
    if (alpha < 0.05)                                       // treat as totally transparent
    {
        discard;
    }
    if (alpha < minimum_alpha && (int(gl_FragCoord.x) & 1) == 0)       // treat as semi transparent
    {
        discard;
    }
#endif
    frag_color = vec4(1.0);
}
