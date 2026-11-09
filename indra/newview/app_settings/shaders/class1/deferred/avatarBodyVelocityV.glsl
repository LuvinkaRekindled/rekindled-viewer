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

vec3 dotRows(vec4 r0, vec4 r1, vec4 r2, vec4 p)
{
    return vec3(dot(r0, p), dot(r1, p), dot(r2, p));
}

void main()
{
    float wx = weight.x;
    if (!(wx >= 0.0 && wx <= 14.0))            // NaN or out-of-range: an index outside the palette reads memory that is not ours (slots 0-14: see avatarSkinV.glsl)
    {
        wx = 0.0;
    }
    int i = int(floor(wx));
    int n = min(i + 1, 14);
    float x = fract(wx);
    vec4 p = vec4(position, 1.0);

    // (the second joint of the pair is not read when the weight is zero: most body vertices belong to one joint)
    vec4 c0 = matrixPalette[i];
    vec4 c1 = matrixPalette[i + 15];
    vec4 c2 = matrixPalette[i + 30];
    vec4 p0 = matrixPalettePrev[i];
    vec4 p1 = matrixPalettePrev[i + 15];
    vec4 p2 = matrixPalettePrev[i + 30];
    if (x > 0.0)
    {
        c0 = mix(c0, matrixPalette[n], x);
        c1 = mix(c1, matrixPalette[n + 15], x);
        c2 = mix(c2, matrixPalette[n + 30], x);
        p0 = mix(p0, matrixPalettePrev[n], x);
        p1 = mix(p1, matrixPalettePrev[n + 15], x);
        p2 = mix(p2, matrixPalettePrev[n + 30], x);
    }
    vec3 world_cur = dotRows(c0, c1, c2, p);
    vec3 world_prev = dotRows(p0, p1, p2, p);

    vary_clip = projection_matrix * (modelview_matrix * vec4(world_cur, 1.0));
    vary_cur = view_proj_cur * vec4(world_cur, 1.0);
    vary_prev = view_proj_prev * vec4(world_prev, 1.0);
    gl_Position = vary_clip;
}
