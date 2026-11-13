// debug.vert: a flat coloured shape from its own vertex buffer (the rotation ring of the manipulators, the joint rotation manipulators, the glTF scene's debug draws, the hero probe's boxes). A line for line
// translation of "Debug Shader" (interface/debugV.glsl, without HAS_SKIN: the skinned form stays a stock program); pairs with debug.frag.

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
};

layout(location = 0) in vec3 position;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
}
