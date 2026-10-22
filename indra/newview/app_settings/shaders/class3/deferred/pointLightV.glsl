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
    vec4 r0 = rows.v[uint(gl_InstanceIndex) * 3u];
    vec4 r1 = rows.v[uint(gl_InstanceIndex) * 3u + 1u];
    vec4 r2 = rows.v[uint(gl_InstanceIndex) * 3u + 2u];
    vec3 center = r0.xyz;
    float size = r0.w;
    vary_light_size = size;
    vary_light_color = r1.xyz;
    vary_light_falloff = r1.w;
    vary_light_classic = int(floatBitsToUint(r2.y));
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
