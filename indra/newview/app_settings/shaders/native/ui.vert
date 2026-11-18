// ui.vert: the user interface: the vertex colour times the texture. A line for line translation of "UI Shader" (interface/uiV.glsl); pairs with ui.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 6) in vec4 diffuse_color;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vary_texcoord0;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
    vertex_color = diffuse_color;
}
