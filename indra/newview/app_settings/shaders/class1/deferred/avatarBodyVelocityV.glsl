/**
 * @file avatarBodyVelocityV.glsl
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

// The avatar's own body meshes (one float weight, 15 joints, 45 vec4 palettes as avatarSkinV.glsl) with WORLD space palettes (no modelview folded in).
uniform mat4 modelview_matrix;
uniform mat4 projection_matrix;
uniform mat4 view_proj_cur;
uniform mat4 view_proj_prev;

uniform vec4 matrixPalette[45];
uniform vec4 matrixPalettePrev[45];

in vec3 position;
in vec4 weight;

out vec4 vary_clip;
out vec4 vary_cur;
out vec4 vary_prev;

vec3 skinPosition(vec4 p, vec4 a0, vec4 a1, vec4 a2, vec4 b0, vec4 b1, vec4 b2, float x)
{
    vec4 r0 = mix(a0, b0, x);
    vec4 r1 = mix(a1, b1, x);
    vec4 r2 = mix(a2, b2, x);
    return vec3(dot(r0, p), dot(r1, p), dot(r2, p));
}

void main()
{
    int i = int(floor(weight.x));
    float x = fract(weight.x);
    vec4 p = vec4(position, 1.0);

    vec3 world_cur = skinPosition(p, matrixPalette[i], matrixPalette[i + 15], matrixPalette[i + 30], matrixPalette[i + 1], matrixPalette[i + 16], matrixPalette[i + 31], x);
    vec3 world_prev = skinPosition(p, matrixPalettePrev[i], matrixPalettePrev[i + 15], matrixPalettePrev[i + 30], matrixPalettePrev[i + 1], matrixPalettePrev[i + 16], matrixPalettePrev[i + 31], x);

    vary_clip = projection_matrix * (modelview_matrix * vec4(world_cur, 1.0));
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
}
