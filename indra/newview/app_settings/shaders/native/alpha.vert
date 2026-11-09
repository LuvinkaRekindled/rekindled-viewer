// alpha.vert: the vertex shader of the forward drawn transparent surfaces. A draw is one row of the scene table (alpha_pass.inc: ll_packet_table); the vertex reads its own data from the arrays the row
// names, so there are no vertex inputs. Variants by define: HAS_SKIN + HAS_INDEXED_DRAW (a rigged draw, the hardware reads the indices: the vertex number is the index), nothing (static draws and particles).
// Outputs (the fragment shaders of the family read these): position in eye space, vertex colour (sRGB), texture coordinates, and the texture slot (flat).

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec2 vary_texcoord0;
layout(location = 3) flat out uint vary_slot;

#if defined(HAS_SKIN) && !defined(HAS_INDEXED_DRAW)
#error "a rigged draw of the alpha family is an indexed one"
#endif

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];

#ifdef HAS_SKIN
    const uint index = uint(gl_VertexIndex);
    const vec4 position = RowVec4(row.positions).v[index];
    mat3 skin_rotation;
    vec3 skin_translation;
    row_skin(row, index, skin_rotation, skin_translation);
    const vec3 vertex = skin_rotation * position.xyz + skin_translation;
    uint color_index = index;
#else
    const bool particle = (row.flags & 16u) != 0u;
    uint index = 0u;
    uint particle_corner_id = 0u;
    vec3 vertex;
    if (particle)
    {
        vertex = particle_corner(row, model, index, particle_corner_id);       // (index becomes the particle's row)
    }
    else
    {
        const uint at = row.first + uint(gl_VertexIndex);
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec4 position = RowVec4(row.positions).v[index];
    if (!particle)
    {
        vertex = position.xyz;
    }
    const uint color_index = index;
#endif

    const vec4 eye = model.mv * vec4(vertex, 1.0);
    gl_Position = model.mvp * vec4(vertex, 1.0);
    // the matrices of the scene table are in GL clip space (z in [-w, w], what the viewer's own shaders compute); Vulkan's is [0, w]. Every viewer vertex shader gets this line from the translator; a program
    // that takes its matrix from the table has to do it itself, or its depth is wrong (the first native alpha test drew a sign over the tree in front of it).
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;

    vec2 texcoord = RowVec2(row.texcoords).t[index];
#ifndef HAS_SKIN
    if (particle)
    {
        texcoord = vec2((particle_corner_id & 2u) != 0u ? 1.0 : 0.0, (particle_corner_id & 1u) != 0u ? 0.0 : 1.0);
    }
#endif
    vary_texcoord0 = (row_texture_matrix(row, TexMatTable(ll_packet_texmats)) * vec4(texcoord, 0.0, 1.0)).xy;
    vary_position = eye.xyz;
    vertex_color = unpackUnorm4x8(RowWords(row.colors).w[color_index]);
    vary_slot = row_texture_slot(row, position.w);
}
