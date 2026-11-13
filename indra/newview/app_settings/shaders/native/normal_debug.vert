// normal_debug.vert: the normals debug view (Advanced > Rendering > Info Displays > Normals): the vertices of a face's own vertex buffer, each handed on with the tip of its normal (and, with
// HAS_ATTRIBUTE_TANGENT, of its tangent) for the geometry stage to draw as lines. A translation of "Normal Debug Shader" (interface/normaldebugV.glsl, the static form: the skinned form was never bound);
// pairs with normal_debug.geom and normal_debug.frag. The tips are remapped to Vulkan's clip space here as the vertex position is (the old program's translator remapped the position only).

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 projection_matrix;
    float debug_normal_draw_length;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_matrix;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec4 normal_g;
#ifdef HAS_ATTRIBUTE_TANGENT
layout(location = 8) in vec4 tangent;
layout(location = 1) out vec4 tangent_g;
#endif

// *NOTE: Should use the modelview_projection_matrix here in the non-skinned case for efficiency, but opting for the simpler implementation as this is debug code (the old shader's note).
// world_pos = mat * vec4(position.xyz, 1.0)
vec4 get_screen_normal(vec3 position, vec4 world_pos, vec3 normal, mat4 mat)
{
    vec4 world_norm = mat * vec4((position + normal), 1.0);
    world_norm.xyz -= world_pos.xyz;
    world_norm.xyz = debug_normal_draw_length * normalize(world_norm.xyz);
    world_norm.xyz += world_pos.xyz;
    vec4 clip = projection_matrix * world_norm;
    clip.z = (clip.z + clip.w) * 0.5;       // (GL clip space to Vulkan's)
    return clip;
}

void main()
{
    const mat4 mat = modelview_matrix;

    vec4 world_pos = mat * vec4(position.xyz, 1.0);

    gl_Position = projection_matrix * world_pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    normal_g = get_screen_normal(position.xyz, world_pos, normal.xyz, mat);
#ifdef HAS_ATTRIBUTE_TANGENT
    tangent_g = get_screen_normal(position.xyz, world_pos, tangent.xyz, mat);
#endif
}
