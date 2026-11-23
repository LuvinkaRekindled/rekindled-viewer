// mask_nocolor.vert: a mesh drawn by range (rk_pull_draw_range: one row, the arrays named by the row) with the matrices of the pass in loose uniforms: the position, the normal and the texture coordinates of the
// vertex, no vertex colour. Stands in for "Deferred Diffuse Non-Indexed Alpha Mask No Color Shader pull" (deferred/diffuseNoColorV.glsl through the pull twin); pairs with mask_nocolor.frag.
// Defines: AVATAR_PASS (the draw block's texture slot), MASK_NOCOLOR (the matrices of the draw block of alpha_pass.inc).

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out vec3 vary_normal;

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const uint at = row.first + uint(gl_VertexIndex);
    uint index = at;
    if ((row.flags & 4u) == 0u)
    {
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
    const vec2 texcoord0 = RowVec2(row.texcoords).t[index];

    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0.0, 1.0)).xy;
    vary_normal = normalize(normal_matrix * normal);
}
