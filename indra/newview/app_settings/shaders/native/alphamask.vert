// alphamask.vert: the alpha mask of the avatar's baked texture layers (LLTexLayer: the layers' shapes drawn into the bake, the vertex colour times the texture, thrown away under a minimum alpha). A line for
// line translation of "Alpha Mask Shader" (interface/alphamaskV.glsl); pairs with alphamask.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 6) in vec4 diffuse_color;
layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vary_texcoord0;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
    vertex_color = diffuse_color;
}
