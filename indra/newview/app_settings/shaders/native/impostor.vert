// impostor.vert: the quads of the avatar impostors in the G-buffer pass (the picture of a whole far avatar, made by LLVOAvatar::generateImpostor), all of a pass in one draw: a row of the impostor table
// (3 vec4: the centre with the diffuse slot, the left half axis with the normal slot, the up half axis with the specular slot; rkvkbridge.h ImpostorQuadRow) a draw instance, 6 vertices a row, the corners
// in the order of the old immediate mode quad (LLVOAvatar::renderImpostor). Stands in for "Deferred Impostor Shader" (deferred/impostorV.glsl), which has no table: only this program draws (kind 41).
// The matrices are the viewer's (GL clip space): the depth is remapped to Vulkan's as the old programs' is (rkvkshader.cpp).

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    uvec2 rk_impostor_table;
};
layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ImpostorRows { vec4 v[]; };

layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) flat out uint vary_slot_diffuse;
layout(location = 2) flat out uint vary_slot_normal;
layout(location = 3) flat out uint vary_slot_specular;

void main()
{
    ImpostorRows rows = ImpostorRows(rk_impostor_table);
    const uint base = uint(gl_InstanceIndex) * 3u;
    const vec4 a = rows.v[base];
    const vec4 b = rows.v[base + 1u];
    const vec4 c = rows.v[base + 2u];
    // the corners of the two triangles: centre + sl * left + su * up, the texture coordinate
    const float sl[6] = float[6](1.0, -1.0, -1.0, 1.0, -1.0, 1.0);
    const float su[6] = float[6](-1.0, -1.0, 1.0, -1.0, 1.0, 1.0);
    const vec2 uv[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    const uint k = uint(gl_VertexIndex);
    const vec3 position = a.xyz + sl[k] * b.xyz + su[k] * c.xyz;
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    RK_CLIP_Z(gl_Position);
    vary_texcoord0 = uv[k];
    vary_slot_diffuse = floatBitsToUint(a.w);
    vary_slot_normal = floatBitsToUint(b.w);
    vary_slot_specular = floatBitsToUint(c.w);
}
