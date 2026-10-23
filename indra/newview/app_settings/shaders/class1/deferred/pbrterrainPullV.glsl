/**
 * @file class1\environment\pbrterrainV.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/* S6.6a: the PBR terrain pass from the geometry packet path (kind 32): position, normal, tangent and texcoord1 are read by address from the patch's vertex buffer and the matrices from the
   record's matrix table; everything below is pbrterrainV.glsl unchanged. */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeoVec2 { vec2 t[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GeoWords { uint w[]; };

struct GeoPacket
{
    uvec2 positions;
    uvec2 normals;
    uvec2 texcoords;
    uvec2 colors;
    uvec2 indices;
    uvec2 tangents;
    uvec2 texcoords1;
    uvec2 texcoords2;
    uint first;
    uint model;
    uint flags;
    uint mapSlots;
    uvec2 slots;
    uvec2 pad;
    vec4 specular;
    float envIntensity;
    float cutoff;
    float brightness;
    float pad2;
    uvec2 weights;
    uvec2 palette;
    uint joints;
    uint skinPad0;
    uint skinPad1;
    uint skinPad2;
};

struct GeoMatrix
{
    mat4 mv;
    mat4 mvp;
    vec4 n0;
    vec4 n1;
    vec4 n2;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoPackets { GeoPacket p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GeoMatrices { GeoMatrix m[]; };

uniform uvec2 ll_packet_table;
uniform uvec2 ll_packet_models;

#define TERRAIN_PBR_DETAIL_EMISSIVE 0
#define TERRAIN_PBR_DETAIL_OCCLUSION -1
#define TERRAIN_PBR_DETAIL_NORMAL -2
#define TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS -3

#define TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE 0
#define TERRAIN_PAINT_TYPE_PBR_PAINTMAP 1

mat3 normal_matrix;
mat4 modelview_matrix;
mat4 modelview_projection_matrix;
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
uniform float region_scale;
#endif

vec3 position;
vec3 normal;
vec4 tangent;
vec2 texcoord1;

out vec3 vary_position;
out vec3 vary_normal;
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
out vec3 vary_vertex_normal; // Used by pbrterrainUtilF.glsl
#endif
#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
out vec3 vary_tangents[4];
flat out float vary_signs[4];
#endif

// vary_texcoord* are used for terrain composition, vary_coords are used for terrain UVs
#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
out vec4 vary_texcoord0;
out vec4 vary_texcoord1;
#elif TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
out vec2 vary_texcoord;
#endif
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
out vec4[10] vary_coords;
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
out vec4[2] vary_coords;
#endif

// *HACK: Each material uses only one texture transform, but the KHR texture
// transform spec allows handling texture transforms separately for each
// individual texture info.
uniform vec4[5] terrain_texture_transforms;

vec2 terrain_texture_transform(vec2 vertex_texcoord, vec4[2] khr_gltf_transform);
vec4 terrain_tangent_space_transform(vec4 vertex_tangent, vec3 vertex_normal, vec4[2] khr_gltf_transform);

void main()
{
    {
        GeoPacket pk = GeoPackets(ll_packet_table).p[gl_InstanceIndex];
        uint i = pk.first + uint(gl_VertexIndex);
        uint word = GeoWords(pk.indices).w[i >> 1];
        uint index = ((i & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
        position = GeoVec4(pk.positions).v[index].xyz;
        normal = GeoVec4(pk.normals).v[index].xyz;
        tangent = GeoVec4(pk.tangents).v[index];
        texcoord1 = GeoVec2(pk.texcoords1).t[index];
        GeoMatrix m = GeoMatrices(ll_packet_models).m[pk.model];
        modelview_matrix = m.mv;
        modelview_projection_matrix = m.mvp;
        normal_matrix = mat3(m.n0.xyz, m.n1.xyz, m.n2.xyz);
    }
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    vary_position = (modelview_matrix*vec4(position.xyz, 1.0)).xyz;

    vec3 n = normal_matrix * normal;
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
    vary_vertex_normal = normal;
#endif
    vec3 t = normal_matrix * tangent.xyz;

#if (TERRAIN_PBR_DETAIL >= TERRAIN_PBR_DETAIL_NORMAL)
    {
        vec4[2] ttt;
        vec4 transformed_tangent;
        // material 1
        ttt[0].xyz = terrain_texture_transforms[0].xyz;
        ttt[1].x = terrain_texture_transforms[0].w;
        ttt[1].y = terrain_texture_transforms[1].x;
        transformed_tangent = terrain_tangent_space_transform(vec4(t, tangent.w), n, ttt);
        vary_tangents[0] = normalize(transformed_tangent.xyz);
        vary_signs[0] = transformed_tangent.w;
        // material 2
        ttt[0].xyz = terrain_texture_transforms[1].yzw;
        ttt[1].xy = terrain_texture_transforms[2].xy;
        transformed_tangent = terrain_tangent_space_transform(vec4(t, tangent.w), n, ttt);
        vary_tangents[1] = normalize(transformed_tangent.xyz);
        vary_signs[1] = transformed_tangent.w;
        // material 3
        ttt[0].xy = terrain_texture_transforms[2].zw;
        ttt[0].z = terrain_texture_transforms[3].x;
        ttt[1].xy = terrain_texture_transforms[3].yz;
        transformed_tangent = terrain_tangent_space_transform(vec4(t, tangent.w), n, ttt);
        vary_tangents[2] = normalize(transformed_tangent.xyz);
        vary_signs[2] = transformed_tangent.w;
        // material 4
        ttt[0].x = terrain_texture_transforms[3].w;
        ttt[0].yz = terrain_texture_transforms[4].xy;
        ttt[1].xy = terrain_texture_transforms[4].zw;
        transformed_tangent = terrain_tangent_space_transform(vec4(t, tangent.w), n, ttt);
        vary_tangents[3] = normalize(transformed_tangent.xyz);
        vary_signs[3] = transformed_tangent.w;
    }
#endif
    vary_normal = normalize(n);

    // Transform and pass tex coords
    {
        vec4[2] ttt;
#define transform_xy()             terrain_texture_transform(position.xy,               ttt)
#if TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 3
// Don't care about upside-down (transform_xy_flipped())
#define transform_yz()             terrain_texture_transform(position.yz,               ttt)
#define transform_negx_z()         terrain_texture_transform(position.xz * vec2(-1, 1), ttt)
#define transform_yz_flipped()     terrain_texture_transform(position.yz * vec2(-1, 1), ttt)
#define transform_negx_z_flipped() terrain_texture_transform(position.xz,               ttt)
        // material 1
        ttt[0].xyz = terrain_texture_transforms[0].xyz;
        ttt[1].x = terrain_texture_transforms[0].w;
        ttt[1].y = terrain_texture_transforms[1].x;
        vary_coords[0].xy = transform_xy();
        vary_coords[0].zw = transform_yz();
        vary_coords[1].xy = transform_negx_z();
        vary_coords[1].zw = transform_yz_flipped();
        vary_coords[2].xy = transform_negx_z_flipped();
        // material 2
        ttt[0].xyz = terrain_texture_transforms[1].yzw;
        ttt[1].xy = terrain_texture_transforms[2].xy;
        vary_coords[2].zw = transform_xy();
        vary_coords[3].xy = transform_yz();
        vary_coords[3].zw = transform_negx_z();
        vary_coords[4].xy = transform_yz_flipped();
        vary_coords[4].zw = transform_negx_z_flipped();
        // material 3
        ttt[0].xy = terrain_texture_transforms[2].zw;
        ttt[0].z = terrain_texture_transforms[3].x;
        ttt[1].xy = terrain_texture_transforms[3].yz;
        vary_coords[5].xy = transform_xy();
        vary_coords[5].zw = transform_yz();
        vary_coords[6].xy = transform_negx_z();
        vary_coords[6].zw = transform_yz_flipped();
        vary_coords[7].xy = transform_negx_z_flipped();
        // material 4
        ttt[0].x = terrain_texture_transforms[3].w;
        ttt[0].yz = terrain_texture_transforms[4].xy;
        ttt[1].xy = terrain_texture_transforms[4].zw;
        vary_coords[7].zw = transform_xy();
        vary_coords[8].xy = transform_yz();
        vary_coords[8].zw = transform_negx_z();
        vary_coords[9].xy = transform_yz_flipped();
        vary_coords[9].zw = transform_negx_z_flipped();
#elif TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT == 1
        // material 1
        ttt[0].xyz = terrain_texture_transforms[0].xyz;
        ttt[1].x = terrain_texture_transforms[0].w;
        ttt[1].y = terrain_texture_transforms[1].x;
        vary_coords[0].xy = transform_xy();
        // material 2
        ttt[0].xyz = terrain_texture_transforms[1].yzw;
        ttt[1].xy = terrain_texture_transforms[2].xy;
        vary_coords[0].zw = transform_xy();
        // material 3
        ttt[0].xy = terrain_texture_transforms[2].zw;
        ttt[0].z = terrain_texture_transforms[3].x;
        ttt[1].xy = terrain_texture_transforms[3].yz;
        vary_coords[1].xy = transform_xy();
        // material 4
        ttt[0].x = terrain_texture_transforms[3].w;
        ttt[0].yz = terrain_texture_transforms[4].xy;
        ttt[1].xy = terrain_texture_transforms[4].zw;
        vary_coords[1].zw = transform_xy();
#endif
    }

#if TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    vec2 tc = texcoord1.xy;
    vary_texcoord0.zw = tc.xy;
    vary_texcoord1.xy = tc.xy-vec2(2.0, 0.0);
    vary_texcoord1.zw = tc.xy-vec2(1.0, 0.0);
#elif TERRAIN_PAINT_TYPE == TERRAIN_PAINT_TYPE_PBR_PAINTMAP
    vary_texcoord = position.xy / region_scale;
#endif
}
