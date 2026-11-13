// preview_physics.vert: the physics shape preview's model: transformed, with its texture coordinate. A line for line translation of "Preview Physics Shader" (objects/previewPhysicsV.glsl); pairs with
// preview_physics.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    vec4 color;
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;

layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;
}
