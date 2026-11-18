// preview.frag: the preview model's texture times the lit vertex colour. A line for line translation of "Object Preview Shader" (objects/previewF.glsl); pairs with preview.vert.
// (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat3 normal_matrix;
    mat4 texture_matrix0;
    vec4 ambient_color;
    mat4 modelview_projection_matrix;
    vec4 color;
    vec4 light_position[8];
    vec3 light_diffuse[8];
#ifdef HAS_SKIN
    mat4 modelview_matrix;
    mat4 projection_matrix;
#endif
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 color = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)],vary_texcoord0.xy) * vertex_color;
    frag_color = max(color, vec4(0));
}
