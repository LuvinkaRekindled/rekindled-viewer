// copy.frag: the colour of one texture, unchanged.
layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec2 tc;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], tc);
}
