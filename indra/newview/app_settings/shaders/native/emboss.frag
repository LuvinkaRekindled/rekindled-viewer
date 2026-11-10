// emboss.frag: the post deferred emboss pass (the "bumpiness" brightness / darkness kinds): the alpha of the bump image read twice, with the first and the second texture coordinate set, combined into a
// brightness that is added to the picture: max(a0 + (1 - a1) - 0.5, 0). Matches "Deferred Emboss Pull Shader" (objects/bumpF.glsl). Pairs with gbuffer.vert (GB_EMBOSS).

#include "alpha_pass.inc"
#include "lib/clip.glsl"

layout(location = 0) in vec3 vary_position;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 8) in vec2 vary_texcoord1;

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    const uint slot = LL_HM(vary_slot);
    const float tex0 = texture(ll_tbl2d[nonuniformEXT(slot)], vary_texcoord0).a;
    const float tex1 = texture(ll_tbl2d[nonuniformEXT(slot)], vary_texcoord1).a;
    frag_color = vec4(max(tex0 + (1.0 - tex1) - 0.5, 0.0));
}
