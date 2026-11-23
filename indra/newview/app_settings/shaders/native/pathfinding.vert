// pathfinding.vert: the pathfinding overlay's shapes (the navmesh, the characters, the linksets): a vertex colour tinted by a simple three light shading (PF_NO_NORMAL: flat, for the shapes without normals). A line
// for line translation of "Pathfinding Shader" (interface/pathfindingV.glsl) and "PathfindingNoNormals Shader" (interface/pathfindingNoNormalV.glsl); pairs with pathfinding.frag.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float tint;
#ifndef PF_NO_NORMAL
    float ambiance;
#endif
    float alpha_scale;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
};

layout(location = 0) in vec3 position;
layout(location = 6) in vec4 diffuse_color;
#ifndef PF_NO_NORMAL
layout(location = 1) in vec3 normal;
#endif

layout(location = 0) out vec4 vertex_color;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)

#ifdef PF_NO_NORMAL
    vertex_color = vec4(diffuse_color.rgb * tint, diffuse_color.a*alpha_scale);
#else
    vec3 l1 = vec3(-0.75, 1, 1.0)*0.5;
    vec3 l2 = vec3(0.5, -0.6, 0.4)*0.25;
    vec3 l3 = vec3(0.5, -0.8, 0.3)*0.5;

    float lit = max(dot(normal, l1), 0.0);
    lit += max(dot(normal, l2), 0.0);
    lit += max(dot(normal, l3), 0.0);

    lit = clamp(lit, ambiance, 1.0);

    vertex_color = vec4(diffuse_color.rgb * tint * lit, diffuse_color.a*alpha_scale);
#endif
}
