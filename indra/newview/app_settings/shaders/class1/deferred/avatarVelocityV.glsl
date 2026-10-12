/**
 * @file avatarVelocityV.glsl
 *
 * <FS> Avatar motion vectors (doc/vulkan/PLAN-ray-tracing.md, "Design notes for item 6").
 * $LicenseInfo:firstyear=2026:viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc. and Firestorm contributors
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
 * $/LicenseInfo$
 */

// Rigged draws (weight4, mat3x4 palettes, as objectSkinV.glsl): the vertex skinned with this frame's palette and last frame's, both in world space.
// gl_Position is the JITTERED clip position like the G-buffer's (the depth test in the fragment shader needs the same pixels); the motion uses the unjittered matrices.
uniform mat4 modelview_matrix;
uniform mat4 projection_matrix;
uniform mat4 view_proj_cur;     // unjittered projection * view, this frame
uniform mat4 view_proj_prev;    // the same, last frame

uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];
uniform mat3x4 matrixPalettePrev[MAX_JOINTS_PER_MESH_OBJECT];

in vec3 position;
in vec4 weight4;

out vec4 vary_clip;
out vec4 vary_cur;
out vec4 vary_prev;

vec3 skinPosition(vec3 p, mat3x4 a, mat3x4 b, mat3x4 c, mat3x4 d, vec4 w)
{
    mat3 m = mat3(a) * w.x + mat3(b) * w.y + mat3(c) * w.z + mat3(d) * w.w;
    vec3 t = vec3(a[0].w, a[1].w, a[2].w) * w.x + vec3(b[0].w, b[1].w, b[2].w) * w.y
           + vec3(c[0].w, c[1].w, c[2].w) * w.z + vec3(d[0].w, d[1].w, d[2].w) * w.w;
    return m * p + t;
}

void main()
{
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);
    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT - 1));
    index = max(index, vec4(0.0));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    int i1 = int(index.x);
    int i2 = int(index.y);
    int i3 = int(index.z);
    int i4 = int(index.w);

    vec3 world_cur = skinPosition(position, matrixPalette[i1], matrixPalette[i2], matrixPalette[i3], matrixPalette[i4], w);
    vec3 world_prev = skinPosition(position, matrixPalettePrev[i1], matrixPalettePrev[i2], matrixPalettePrev[i3], matrixPalettePrev[i4], w);

    vary_clip = projection_matrix * (modelview_matrix * vec4(world_cur, 1.0));
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
}
