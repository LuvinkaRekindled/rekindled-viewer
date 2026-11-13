// debug.vert: a flat coloured shape from its own vertex buffer (the rotation ring of the manipulators, the joint rotation manipulators, the glTF scene's debug draws, the hero probe's boxes). A line for line
// translation of "Debug Shader" (interface/debugV.glsl); pairs with debug.frag. With HAS_SKIN (the manifest line "Skinned Debug Shader": the rigged meshes of the spatial partition's debug views) the vertex is
// skinned by the mesh's joint palette first (lib/objectskin.glsl), as the old shader with HAS_SKIN.

#ifdef HAS_SKIN
#include "lib/objectskin.glsl"

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 projection_matrix;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_matrix;
};
#else
layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
};
#endif

layout(location = 0) in vec3 position;

void main()
{
#ifdef HAS_SKIN
    mat4 mat = getObjectSkinnedTransform();
    mat = modelview_matrix * mat;
    vec4 pos = mat * vec4(position.xyz, 1.0);
    gl_Position = projection_matrix * pos;
#else
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
#endif
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
}
