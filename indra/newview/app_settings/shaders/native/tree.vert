// tree.vert: the vertex program of the Linden trees of the G-buffer pass, from the scene table (one row a face: its own vertex buffer by address, the region's matrix block). Matches "Deferred Tree Pull Shader"
// (treeV.glsl). Pairs with tree.frag. What is different: the old program passes a vertex colour that is the constant white (vertex_color = vec4(1,1,1,1), multiplied into the albedo); it is left out, one vec4
// less to interpolate and one multiplication less a pixel.

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec3 vary_normal;
layout(location = 2) out vec2 vary_texcoord0;

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(rk_packet_models).m[row.model];

    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);

    const vec4 position = vec4(RowVec4(row.positions).v[index].xyz, 1.0);
    const vec3 normal = RowVec4(row.normals).v[index].xyz;

    gl_Position = model.mvp * position;
    RK_CLIP_Z(gl_Position);      // (the table's matrices are GL clip space: see alpha.vert)
    vary_position = (model.mv * position).xyz;
    vary_normal = normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * normal);
    // the texture matrix is the row's (the old pull program builds it that way: identity when the row has none)
    vary_texcoord0 = (row_texture_matrix(row, TexMatTable(rk_packet_texmats)) * vec4(RowVec2(row.texcoords).t[index], 0.0, 1.0)).xy;
}
