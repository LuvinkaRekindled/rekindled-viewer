// highlight_face.inc: the loose uniform blocks of the selection highlight programs (highlight_face.vert and highlight_face.frag include this file, so both stages declare the same blocks). Every name and type is
// the one the viewer's "Highlight Shader" uses: the draw's matrices and texture slot, and the colour (the viewer sets it with the immediate colour call, the program's `color` uniform).
// Defines of the manifest line: HL_UV (the texture coordinate the face's picture is read by: 0 texcoord0, the diffuse map; 1 texcoord1, the normal map; 2 texcoord2, the specular map), HL_GBUFFER (the G-buffer form).

#ifdef HAS_SKIN
// the skinned form ("Skinned Highlight Shader": the old rigged draws of the alpha pool's debug view): the mesh's skin transform, then the modelview and the projection, as the old shader with HAS_SKIN
layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 projection_matrix;
};
#endif

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
#ifdef HAS_SKIN
    mat4 modelview_matrix;
#else
    mat4 modelview_projection_matrix;
#endif
    mat4 texture_matrix0;
    uint ll_ti_diffuseMap;
};

layout(std140, set = 0, binding = 6) uniform LLLooseMaterial
{
    vec4 color;
};
