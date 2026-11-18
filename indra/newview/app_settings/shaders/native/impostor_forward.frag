// impostor_forward.frag: the texture, thrown away under a minimum alpha. A line for line translation of "Impostor Shader" (objects/impostorF.glsl); pairs with impostor_forward.vert.
// (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float minimum_alpha;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 color = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)],vary_texcoord0.xy);

    if (color.a < minimum_alpha)
    {
        discard;
    }

    frag_color = max(color, vec4(0));
}
