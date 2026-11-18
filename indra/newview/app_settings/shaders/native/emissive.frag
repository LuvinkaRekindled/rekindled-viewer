// emissive.frag: the glow pass: only the alpha channel is written (the glow amount): the diffuse texture's alpha times the emissive array's alpha. A line for line translation of "Deferred Emissive Pull Shader"
// and its skin variant (deferred/emissiveF.glsl with RK_BINDLESS_DIFFUSE); pairs with alpha.vert (GLOW_EMISSIVE: the colour comes from the emissive array).

#include "alpha_pass.inc"

layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;

layout(location = 0) out vec4 frag_color;

void main()
{
    // NOTE: when this shader is used, only alpha is being written to
    float a = texture(rk_tbl2d[nonuniformEXT(RK_HM(vary_slot))], vary_texcoord0.xy).a * vertex_color.a;
    frag_color = max(vec4(0, 0, 0, a), vec4(0));
}
