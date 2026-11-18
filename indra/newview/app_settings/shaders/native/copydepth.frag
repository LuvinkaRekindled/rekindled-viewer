// copydepth.frag: the colour of one texture and the depth of another, unchanged (the depth copies of the post chain). A line for line translation of "Copy Depth Shader" (interface/copyF.glsl with
// COPY_DEPTH); pairs with copy.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec2 tc;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], tc);
    gl_FragDepth = texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], tc).r;
}
