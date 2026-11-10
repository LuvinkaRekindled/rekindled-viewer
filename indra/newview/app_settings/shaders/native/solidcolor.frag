// solidcolor.frag: a flat colour with the alpha of a texture. A line for line translation of "Solid Color Shader" (interface/solidcolorF.glsl); pairs with solidcolor.vert.

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_tex0;
};

layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    vec4 color;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    float alpha = texture(ll_tbl2d[LL_HM(ll_ti_tex0)], vary_texcoord0.xy).a * color.a;
    frag_color = vec4(color.rgb, alpha);
}
