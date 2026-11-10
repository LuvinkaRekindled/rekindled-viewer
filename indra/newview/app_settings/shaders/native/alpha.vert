// alpha.vert: the vertex shader of the forward drawn transparent surfaces. A draw is one row of the scene table (alpha_pass.inc: ll_packet_table); the vertex reads its own data from the arrays the row
// names, so there are no vertex inputs. Variants by define: HAS_SKIN + HAS_INDEXED_DRAW (a rigged draw, the hardware reads the indices: the vertex number is the index), nothing (static draws and particles).
// Outputs (the fragment shaders of the family read these): position in eye space, vertex colour (sRGB), texture coordinates, and the texture slot (flat).

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec2 vary_texcoord0;
layout(location = 3) flat out uint vary_slot;
#ifdef DEBUG_ROW
layout(location = 5) flat out uint vary_debug_color;        // the Batch Size view's colour of the draw (the row's skinPad0) and its texture matrix slot (pad.x: 0 = none)
layout(location = 6) flat out uint vary_debug_texmat;
#endif
#ifdef ALPHA_LIT
layout(location = 4) out vec3 vary_norm;        // the surface normal in eye space (a lit surface)
#endif
#ifdef ALPHA_UBER
layout(location = 5) flat out uint vary_fullbright;     // 1: the draw is a fullbright one (flags bit 5)
#endif
#ifdef ALPHA_MAT
// a material draw (flags bits 6-8: 1 a material, 2 with a normal map, 4 with a specular map)
layout(location = 6) flat out uint vary_material;
layout(location = 7) out vec3 vary_tangent;
layout(location = 8) out vec2 vary_texcoord1;
layout(location = 9) out vec2 vary_texcoord2;
layout(location = 10) flat out float vary_sign;
layout(location = 11) flat out uint vary_slot_normal;
layout(location = 12) flat out uint vary_slot_specular;
layout(location = 13) flat out vec4 vary_specular;      // the specular colour (rgb) and glossiness (a)
layout(location = 14) flat out vec2 vary_matparams;     // the environment intensity, the glow
#endif

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
    const vec3 rig_anchor = row_rig_anchor(row);
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
    const vec3 rig_anchor = vec3(0.0);
#endif

    const vec4 eye = rig_transform(model.mv, vertex, rig_anchor);
    gl_Position = rig_transform(model.mvp, vertex, rig_anchor);
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
    const mat4 texmat = row_texture_matrix(row, TexMatTable(ll_packet_texmats));
    vary_texcoord0 = (texmat * vec4(texcoord, 0.0, 1.0)).xy;
    vary_position = eye.xyz;
#ifdef GLOW_EMISSIVE
    {
        // the glow pass (Deferred Emissive Pull Shader, emissivePullV.glsl): the colour is the emissive array: the first byte of the vertex buffer (the positions) plus its offset in the row (pad2)
        uint carry;
        uvec2 emissive_address = row.positions;
        emissive_address.x = uaddCarry(emissive_address.x, floatBitsToUint(row.pad2), carry);
        emissive_address.y += carry;
        vertex_color = unpackUnorm4x8(RowWords(emissive_address).w[color_index]);
    }
#else
    vertex_color = unpackUnorm4x8(RowWords(row.colors).w[color_index]);
#endif
    vary_slot = row_texture_slot(row, position.w);
#ifdef DEBUG_ROW
    vary_debug_color = row.skinPad0;
    vary_debug_texmat = row.pad.x;
#endif
#ifdef ALPHA_UBER
    vary_fullbright = (row.flags & 32u) != 0u ? 1u : 0u;
#endif
#ifdef ALPHA_MAT
    {
        const uint flags = (row.flags >> 6u) & 7u;
        vary_material = flags;
        vary_tangent = vec3(0.0, 0.0, 1.0);
        vary_texcoord1 = vec2(0.0);
        vary_texcoord2 = vec2(0.0);
        vary_sign = 1.0;
        vary_slot_normal = 0u;
        vary_slot_specular = 0u;
        vary_specular = vec4(0.0);
        vary_matparams = vec2(0.0);
        if ((flags & 1u) != 0u)
        {
            vary_specular = row.specular;
            vary_matparams = vec2(row.envIntensity, row.brightness);
            vary_slot_normal = row.mapSlots & 0xFFFFu;
            vary_slot_specular = row.mapSlots >> 16;
            if ((flags & 2u) != 0u)
            {
                const vec4 tangent = RowVec4(row.tangents).v[index];
                vary_texcoord1 = (texmat * vec4(RowVec2(row.texcoords1).t[index], 0.0, 1.0)).xy;
#ifdef HAS_SKIN
                vary_tangent = normalize(mat3(model.mv) * (skin_rotation * tangent.xyz));
#else
                vary_tangent = normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * tangent.xyz);
#endif
                vary_sign = tangent.w;
            }
            if ((flags & 4u) != 0u)
            {
                vary_texcoord2 = (texmat * vec4(RowVec2(row.texcoords2).t[index], 0.0, 1.0)).xy;
            }
        }
    }
#endif

#ifdef ALPHA_LIT
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
#ifdef HAS_SKIN
    vary_norm = normalize(mat3(model.mv) * (skin_rotation * normal));
#else
    vary_norm = particle ? vec3(0.0, 0.0, 1.0) : normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * normal);       // (a particle faces the camera: +z in eye space)
#endif
#endif
}
