// texfilter.frag: the picture, thrown away where it is darker than the tolerance, the rest at alpha 0.95 (the scene monitor's difference picture). A line for line translation of "One Texture Filter Shader"
// (interface/onetexturefilterF.glsl); pairs with texfilter.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float tolerance;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_tex0;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = texture(rk_tbl2d[RK_HM(rk_ti_tex0)], vary_texcoord0.xy);
    if (frag_color[0] + frag_color[1] + frag_color[2] < tolerance)
    {
        discard;
    }
    else
    {
        frag_color[3] = 0.95;
    }
}
