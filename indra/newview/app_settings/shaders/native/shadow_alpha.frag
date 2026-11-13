// shadow_alpha.frag: an alpha tested shadow caster (pairs with shadow_alpha.vert, which says what the flags mean). The texel's alpha against the packet's cutoff, then for the old style materials the vertex
// alpha and a screen door for the semi transparent texels: none below 0.05 casts, below 0.88 only every other pixel column casts. glTF alpha mask casters (flag 64) have no screen door, opaque casters
// (flag 2048) sample nothing. The pixel column comes from gl_FragCoord: the old program derived it from `floor(0.5 * (width - 1) * ndc.x)` through two varyings and a division, which is the same column for a
// shadow map whose width is a multiple of four (every one the viewer makes).

layout(location = 0) in float vary_alpha;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) flat in uint vary_slot;
layout(location = 3) flat in uint vary_flags;
layout(location = 4) flat in float vary_cutoff;

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = vec4(1.0);
    if ((vary_flags & 2048u) != 0u)
    {
        return;                         // an opaque caster (glTF opaque): no alpha test, nothing sampled
    }
    float alpha = texture(ll_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0).a;
    if ((vary_flags & 64u) != 0u)
    {
        // glTF alpha mask: the texture's alpha times the vertex alpha against the material's cutoff, no dithering
        if ((vary_flags & 1u) == 0u)
        {
            alpha *= vary_alpha;
        }
        if (alpha < vary_cutoff)
        {
            discard;
        }
        return;
    }
    if (alpha < vary_cutoff)
    {
        discard;
    }
    if ((vary_flags & 2u) == 0u)
    {
        if ((vary_flags & 1u) == 0u)
        {
            alpha *= vary_alpha;
        }
        if (alpha < 0.05)               // treat as totally transparent
        {
            discard;
        }
        if (alpha < 0.88 && (int(gl_FragCoord.x) & 1) == 0)     // treat as semi transparent
        {
            discard;
        }
    }
}
