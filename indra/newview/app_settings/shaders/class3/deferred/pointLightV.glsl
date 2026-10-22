/**
 * @file class3\deferred\pointLightV.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

uniform mat4 modelview_projection_matrix;
uniform mat4 modelview_matrix;

#ifdef LL_LIGHT_PULL
// S6 LV1 (doc/vulkan/S6-census.md): the light volumes of a pass drawn from one record: the light (centre, size, colour, falloff, which side of the box the camera is on) is read from a table by the
// instance number, the corners of the box fan are made from the vertex number (the same fan as get_box_fan_indices / sOcclusionIndices and the cube of ll_create_cube_vb), nothing is bound but the table.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightRows { vec4 v[]; };
uniform uvec2 ll_light_table;
const uint kFan[64] = uint[64](
    7u, 6u, 2u, 3u, 1u, 5u, 4u, 6u,
    3u, 2u, 0u, 1u, 5u, 7u, 6u, 2u,
    5u, 4u, 6u, 7u, 3u, 1u, 0u, 4u,
    1u, 0u, 4u, 5u, 7u, 3u, 2u, 0u,
    6u, 0u, 2u, 3u, 7u, 5u, 4u, 0u,
    2u, 4u, 0u, 1u, 3u, 7u, 6u, 4u,
    4u, 2u, 6u, 7u, 5u, 1u, 0u, 2u,
    0u, 6u, 4u, 5u, 1u, 3u, 2u, 6u);
flat out vec3 vary_light_color;
flat out float vary_light_falloff;
flat out float vary_light_size;
flat out int vary_light_classic;
#ifdef LL_PROJECTOR_PULL
// S6 LV2: the projector rows are 11 vec4: centre+size, colour+falloff, (side, classic, slot, shadow index), n+near, p+range, origin+ambiance, (focus, lod, ambient lod, shadow fade), the 4 columns of proj_mat
flat out mat4 vary_proj_mat;
flat out vec3 vary_proj_n;
flat out vec3 vary_proj_p;
flat out vec3 vary_proj_origin;
flat out float vary_proj_near;
flat out float vary_proj_range;
flat out float vary_proj_ambiance;
flat out float vary_proj_focus;
flat out float vary_proj_lod;
flat out float vary_proj_ambient_lod;
flat out float vary_proj_shadow_fade;
flat out int vary_proj_shadow_idx;
flat out uint vary_proj_slot;
const uint kRowStride = 11u;
#else
const uint kRowStride = 3u;
#endif
#else
in vec3 position;

uniform vec3 center;
uniform float size;
#endif

out vec4 vary_fragcoord;
out vec3 trans_center;

void main()
{
#ifdef LL_LIGHT_PULL
    LightRows rows = LightRows(ll_light_table);
    uint at = uint(gl_InstanceIndex) * kRowStride;
    vec4 r0 = rows.v[at];
    vec4 r1 = rows.v[at + 1u];
    vec4 r2 = rows.v[at + 2u];
    vec3 center = r0.xyz;
    float size = r0.w;
    vary_light_size = size;
    vary_light_color = r1.xyz;
    vary_light_falloff = r1.w;
    vary_light_classic = int(floatBitsToUint(r2.y));
#ifdef LL_PROJECTOR_PULL
    vary_proj_slot = floatBitsToUint(r2.z);
    vary_proj_shadow_idx = int(floatBitsToUint(r2.w));
    vec4 r3 = rows.v[at + 3u];
    vec4 r4 = rows.v[at + 4u];
    vec4 r5 = rows.v[at + 5u];
    vec4 r6 = rows.v[at + 6u];
    vary_proj_n = r3.xyz;
    vary_proj_near = r3.w;
    vary_proj_p = r4.xyz;
    vary_proj_range = r4.w;
    vary_proj_origin = r5.xyz;
    vary_proj_ambiance = r5.w;
    vary_proj_focus = r6.x;
    vary_proj_lod = r6.y;
    vary_proj_ambient_lod = r6.z;
    vary_proj_shadow_fade = r6.w;
    vary_proj_mat = mat4(rows.v[at + 7u], rows.v[at + 8u], rows.v[at + 9u], rows.v[at + 10u]);
#endif
    uint corner = kFan[floatBitsToUint(r2.x) * 8u + uint(gl_VertexIndex)];
    vec3 position = vec3(((corner & 4u) != 0u) ? 1.0 : -1.0, ((corner & 2u) != 0u) ? 1.0 : -1.0, ((corner & 1u) != 0u) ? 1.0 : -1.0);
#endif
    //transform vertex
    vec3 p = position*size+center;
    vec4 pos = modelview_projection_matrix * vec4(p.xyz, 1.0);
    vary_fragcoord = pos;
    trans_center = (modelview_matrix*vec4(center.xyz, 1.0)).xyz;
    gl_Position = pos;
}
