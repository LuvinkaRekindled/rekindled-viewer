// solidcolor.frag: a flat colour with the alpha of a texture. A line for line translation of "Solid Color Shader" (interface/solidcolorF.glsl); pairs with solidcolor.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_tex0;
};

layout(std140, set = 0, binding = 6) uniform RKLooseMaterial
{
    vec4 color;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    float alpha = texture(rk_tbl2d[RK_HM(rk_ti_tex0)], vary_texcoord0.xy).a * color.a;
    frag_color = vec4(color.rgb, alpha);
}
