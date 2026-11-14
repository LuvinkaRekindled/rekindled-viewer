// gbuffer.vert: the vertex shader of the plain (legacy prim) G-buffer draws from the scene table: the diffuse family, opaque and alpha mask. A draw is one row of the table (alpha_pass.inc: ll_packet_table);
// the vertex reads its own data from the arrays the row names. Variants by define: HAS_SKIN + HAS_INDEXED_DRAW (a rigged draw: the hardware reads the indices, the vertex number is the index), nothing (a static draw).
// Outputs (gbuffer_diffuse.frag and gbuffer_diffuse_mask.frag read these): eye space position, eye space normal, vertex colour, texture coordinates, the texture slot and the mask cutoff (flat).

#include "alpha_pass.inc"
#include "lib/rows.glsl"

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec2 vary_texcoord0;
layout(location = 3) flat out uint vary_slot;
layout(location = 4) out vec3 vary_normal;
layout(location = 5) flat out float vary_cutoff;
#ifdef GB_MATERIAL
// a legacy material draw (the materials family): tangent frame and the second and third texture coordinates for the maps, the slots of the maps, and the draw's own values
layout(location = 6) out vec3 vary_tangent;
layout(location = 7) flat out float vary_sign;
layout(location = 8) out vec2 vary_texcoord1;
layout(location = 9) out vec2 vary_texcoord2;
layout(location = 10) flat out uint vary_slot_normal;
layout(location = 11) flat out uint vary_slot_specular;
layout(location = 12) flat out vec4 vary_specular;         // the specular colour (rgb) and glossiness (a)
layout(location = 13) flat out vec2 vary_matparams;        // the environment intensity, the emissive brightness
#endif

#ifdef GB_BUMP
// a bump mapped draw (the bump pass): the tangent frame (tangent and its sign; the normal is vary_normal) and the bump image's slot; the image is read in the fragment program with the diffuse texture's coordinates
layout(location = 6) out vec3 vary_tangent;
layout(location = 7) flat out float vary_sign;
layout(location = 10) flat out uint vary_slot_normal;
#endif
#ifdef GB_EMBOSS
// an embossed draw (the post deferred emboss pass): the second coordinate set (the same image, read twice, the second time offset)
layout(location = 8) out vec2 vary_texcoord1;
#endif

#if defined(HAS_SKIN) && !defined(HAS_INDEXED_DRAW)
#error "a rigged draw of the G-buffer family is an indexed one"
#endif

void main()
{
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(ll_packet_models).m[row.model];

#ifdef HAS_SKIN
    const uint index = row_clamp_index(row, uint(gl_VertexIndex));
#else
    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = row_clamp_index(row, (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu));
#endif
    const vec4 position = RowVec4(row.positions).v[index];
    const vec3 normal = RowVec4(row.normals).v[index].xyz;

#ifdef HAS_SKIN
    mat3 skin_rotation;
    vec3 skin_translation;
    row_skin(row, index, skin_rotation, skin_translation);
    const vec3 vertex = skin_rotation * position.xyz + skin_translation;
    const vec3 rig_anchor = row_rig_anchor(row);
    vary_normal = normalize(mat3(model.mv) * (skin_rotation * normal));
#else
    const vec3 vertex = position.xyz;
    const vec3 rig_anchor = vec3(0.0);
    vary_normal = normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * normal);
#endif
    vary_position = rig_transform(model.mv, vertex, rig_anchor).xyz;
    gl_Position = rig_transform(model.mvp, vertex, rig_anchor);
    // (the table's matrices are GL clip space: see alpha.vert)
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;

    const mat4 texmat = row_texture_matrix(row, TexMatTable(ll_packet_texmats));
    vary_texcoord0 = (texmat * vec4(RowVec2(row.texcoords).t[index], 0.0, 1.0)).xy;
    vertex_color = unpackUnorm4x8(RowWords(row.colors).w[index]);
#ifdef GB_MATERIAL
    vary_slot = row.slots.x & 0xFFFFu;      // (a material draw has one diffuse texture)
#else
    vary_slot = row_texture_slot(row, position.w);
#endif
    vary_cutoff = row.cutoff;
#ifdef GB_MATERIAL
    vary_specular = row.specular;
    vary_matparams = vec2(row.envIntensity, row.brightness);
    vary_slot_normal = row.mapSlots & 0xFFFFu;
    vary_slot_specular = row.mapSlots >> 16;
    vary_tangent = vec3(0.0, 0.0, 1.0);
    vary_sign = 1.0;
    vary_texcoord1 = vec2(0.0);
    vary_texcoord2 = vec2(0.0);
#ifdef MAT_NORMAL_MAP
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
#endif
#ifdef MAT_SPEC_MAP
    vary_texcoord2 = (texmat * vec4(RowVec2(row.texcoords2).t[index], 0.0, 1.0)).xy;
#endif
#endif
#ifdef GB_BUMP
    {
        const vec4 tangent = RowVec4(row.tangents).v[index];
#ifdef HAS_SKIN
        vary_tangent = normalize(mat3(model.mv) * (skin_rotation * tangent.xyz));
#else
        vary_tangent = normalize(mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz) * tangent.xyz);
#endif
        vary_sign = tangent.w;
        vary_slot_normal = row.mapSlots & 0xFFFFu;
    }
#endif
#ifdef GB_EMBOSS
    vary_texcoord1 = (texmat * vec4(RowVec2(row.texcoords1).t[index], 0.0, 1.0)).xy;
#endif
}
