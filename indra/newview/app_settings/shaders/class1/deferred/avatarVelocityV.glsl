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

// both palettes, the current and last frame's: a joint with no weight is not read (most vertices have one to three; a zero weight adds nothing)
void skinBoth(vec3 p, int i1, int i2, int i3, int i4, vec4 w, out vec3 world_cur, out vec3 world_prev)
{
    mat3 mc = mat3(matrixPalette[i1]) * w.x;
    vec3 tc = vec3(matrixPalette[i1][0].w, matrixPalette[i1][1].w, matrixPalette[i1][2].w) * w.x;
    mat3 mp = mat3(matrixPalettePrev[i1]) * w.x;
    vec3 tp = vec3(matrixPalettePrev[i1][0].w, matrixPalettePrev[i1][1].w, matrixPalettePrev[i1][2].w) * w.x;
    if (w.y > 0.0)
    {
        mc += mat3(matrixPalette[i2]) * w.y;
        tc += vec3(matrixPalette[i2][0].w, matrixPalette[i2][1].w, matrixPalette[i2][2].w) * w.y;
        mp += mat3(matrixPalettePrev[i2]) * w.y;
        tp += vec3(matrixPalettePrev[i2][0].w, matrixPalettePrev[i2][1].w, matrixPalettePrev[i2][2].w) * w.y;
    }
    if (w.z > 0.0)
    {
        mc += mat3(matrixPalette[i3]) * w.z;
        tc += vec3(matrixPalette[i3][0].w, matrixPalette[i3][1].w, matrixPalette[i3][2].w) * w.z;
        mp += mat3(matrixPalettePrev[i3]) * w.z;
        tp += vec3(matrixPalettePrev[i3][0].w, matrixPalettePrev[i3][1].w, matrixPalettePrev[i3][2].w) * w.z;
    }
    if (w.w > 0.0)
    {
        mc += mat3(matrixPalette[i4]) * w.w;
        tc += vec3(matrixPalette[i4][0].w, matrixPalette[i4][1].w, matrixPalette[i4][2].w) * w.w;
        mp += mat3(matrixPalettePrev[i4]) * w.w;
        tp += vec3(matrixPalettePrev[i4][0].w, matrixPalettePrev[i4][1].w, matrixPalettePrev[i4][2].w) * w.w;
    }
    world_cur = mc * p + tc;
    world_prev = mp * p + tp;
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

    vec3 world_cur, world_prev;
    skinBoth(position, i1, i2, i3, i4, w, world_cur, world_prev);

    vary_clip = projection_matrix * (modelview_matrix * vec4(world_cur, 1.0));
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
}
