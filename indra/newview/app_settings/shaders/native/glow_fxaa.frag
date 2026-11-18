// glow_fxaa.frag: the picture with its luminance in alpha, the input the FXAA shaders want (the pass before them when RenderFSAASamples is on). A line for line translation of "Glow CombineFXAA Shader"
// (interface/glowcombineFXAAF.glsl; its screen_res uniform was never read); pairs with copy.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
};

layout(location = 0) in vec2 tc;
layout(location = 0) out vec4 frag_color;

void main()
{
    const vec3 col = texture(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], tc).rgb;
    frag_color = vec4(col, dot(col, vec3(0.299, 0.587, 0.144)));
}
