// ui.frag: the user interface: the vertex colour times the texture. A line for line translation of "UI Shader" (interface/uiF.glsl); pairs with ui.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = vertex_color * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
}
