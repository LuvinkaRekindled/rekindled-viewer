// glow_combine.frag: the lit picture plus the glow target: the sum of two textures. A line for line translation of "Glow Combine Shader" (interface/glowcombineF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_emissiveRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = texture(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], vary_fragcoord) + texture(rk_tbl2d[LL_HM(rk_ti_emissiveRect)], vary_fragcoord);
}
