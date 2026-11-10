// terrain.vert: the vertex program of the legacy (texture) terrain of the G-buffer pass, from the scene table (one row a patch: its vertex buffer by address, the region's matrix block). The detail textures are
// projected from the object planes (object_plane_s / t, the world position scaled and offset), the height ramp coordinates come from the second texture coordinate set.
// Matches "Deferred Terrain Pull Shader" (terrainV.glsl). Pairs with terrain.frag. What is different: the three ramp positions of the old program (the coordinate, and the same minus 2 and minus 1 in x) are
// worked out in the fragment program from the one coordinate, so the vertex program passes two vec2 and not two vec4 (10 floats an interpolated pixel, not 14).

#define TERRAIN_PASS 1
#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec3 vary_normal;
layout(location = 2) out vec2 vary_detail;      // the detail textures' coordinate
layout(location = 3) out vec2 vary_ramp;        // the height ramp's coordinate

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];

    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);

    const vec4 position = vec4(RowVec4(row.positions).v[index].xyz, 1.0);
    const vec3 normal = RowVec4(row.normals).v[index].xyz;

    gl_Position = model.mvp * position;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (the table's matrices are GL clip space: see alpha.vert)
    vary_position = (model.mv * position).xyz;
    vary_normal = normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * normal);

    // texgen_object of the old program: the world position through the two planes, then the texture matrix. The matrix is the row's (the old pull program builds it the same way: the row's texture matrix slot,
    // identity when there is none, a texture animation's parameters made into a matrix against the clock); it is not a uniform of the program. z is 0 and w 1, so only its x and y rows and the translation count
    const mat4 tm = row_texture_matrix(row, TexMatTable(ll_packet_texmats));
    const float u = dot(position, object_plane_s);
    const float v = dot(position, object_plane_t);
    vary_detail = vec2(tm[0][0] * u + tm[1][0] * v + tm[3][0],
                       tm[0][1] * u + tm[1][1] * v + tm[3][1]);
    vary_ramp = RowVec2(row.texcoords1).t[index];
}
