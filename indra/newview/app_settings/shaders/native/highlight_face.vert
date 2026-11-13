// highlight_face.vert: a face of the selection highlight (the selected faces, the faces of the "paint 'em red" lists, the normal map and specular map views, the parcel owners on the terrain): the face's own vertex
// buffer, the picture's coordinate through the draw's texture matrix. A translation of the viewer's highlight vertex shaders ("Highlight Shader", "Highlight Normals Shader", "Highlight Spec Shader":
// interface/highlightV.glsl, highlightNormV.glsl, highlightSpecV.glsl), one file: HL_UV picks the coordinate. Pairs with highlight_face.frag. HAS_SKIN is the rigged form (the manifest line "Skinned Highlight
// Shader"): the vertex is skinned by the mesh's joint palette first (lib/objectskin.glsl).

#include "highlight_face.inc"
#ifdef HAS_SKIN
#include "lib/objectskin.glsl"
#endif

layout(location = 0) in vec3 position;
#if HL_UV == 1
layout(location = 3) in vec2 texcoord;          // texcoord1
#elif HL_UV == 2
layout(location = 4) in vec2 texcoord;          // texcoord2
#else
layout(location = 2) in vec2 texcoord;          // texcoord0
#endif
layout(location = 0) out vec2 vary_texcoord0;

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
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord, 0.0, 1.0)).xy;
}
