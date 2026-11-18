// pbrterrain.vert: the PBR terrain of the G-buffer pass, from the geometry packet path (the pull twin of the viewer's program, pbrterrainV.glsl with LL_VERTEX_PULL). The row names the patch's arrays (positions,
// normals, tangents, the second texture coordinate set); the model matrices come from the model block. What is different from the old program, which sends the texture coordinates of every material to the
// fragment shader (up to 10 vec4 for the triplanar mapping, 4 tangents and 4 signs for the normal maps): the vertex sends what they are made from (the object space position, the normal and the tangent), and the
// fragment shader makes the coordinates of the materials a pixel really uses (two on most of the terrain). They are affine in the position, so the interpolation is the same.
// Defines (the manifest and the viewer's global defines): TERRAIN_PBR_DETAIL, TERRAIN_PAINT_TYPE, TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT, PBR_TERRAIN_PASS.

#include "alpha_pass.inc"
#include "lib/rows.glsl"

#define TERRAIN_PBR_DETAIL_EMISSIVE 0
#define TERRAIN_PBR_DETAIL_OCCLUSION -1
#define TERRAIN_PBR_DETAIL_NORMAL -2
#define TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS -3

#define TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE 0
#define TERRAIN_PAINT_TYPE_PBR_PAINTMAP 1

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec3 vary_normal;
layout(location = 2) out vec3 vary_object;              // the object space position: the texture coordinates are made from it
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
layout(location = 3) out vec3 vary_object_normal;       // the triplanar weights and the side of each slice come from the object space normal
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
layout(location = 4) out vec3 vary_tangent;             // eye space, before the materials' own transforms
layout(location = 5) flat out float vary_tangent_sign;
#endif
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
layout(location = 6) out vec2 vary_tc;                  // the composition coordinate (the three alpha ramp lookups are offsets of it)
#endif

void main()
{
    const Row row = RowTable(rk_packet_table).r[gl_InstanceIndex];
    const ModelBlock model = ModelTable(rk_packet_models).m[row.model];
    const uint at = row.first + uint(gl_VertexIndex);
    uint index = at;
    if ((row.flags & 4u) == 0u)
    {
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    const vec3 position = RowVec4(row.positions).v[index].xyz;
    const vec3 normal = RowVec4(row.normals).v[index].xyz;
    const mat3 normal_matrix = mat3(model.n0.xyz, model.n1.xyz, model.n2.xyz);

    gl_Position = model.mvp * vec4(position, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's)
    vary_position = (model.mv * vec4(position, 1.0)).xyz;
    vary_normal = normalize(normal_matrix * normal);
    vary_object = position;
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
    vary_object_normal = normal;
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    // (a patch without tangents has the tangents address equal to the positions': the tangent is 0, as the old program's unset attribute)
    const vec4 tangent = all(equal(row.tangents, row.positions)) ? vec4(0.0) : RowVec4(row.tangents).v[index];
    vary_tangent = normal_matrix * tangent.xyz;
    vary_tangent_sign = tangent.w;
#endif
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    vary_tc = RowVec2(row.texcoords1).t[index];
#endif
}
