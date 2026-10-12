/**
 * @file avatarVelocityF.glsl
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

// Writes the total screen motion (pixels, previous minus current, as motionVectorsF.glsl) of an avatar vertex, with alpha = 1 as the "an avatar wrote here" flag.
// A pixel is kept only when the avatar surface is where the G-buffer put its depth: occluded parts, alpha cut-outs the G-buffer discarded and thin geometry fail the test.
out vec4 frag_color;

in vec4 vary_clip;
in vec4 vary_cur;
in vec4 vary_prev;

uniform sampler2D depthMap;
uniform vec2 render_res;
uniform vec2 proj_z;            // jittered projection [2][2] and [3][2]: the distance of a depth buffer value is proj_z.y / (ndc_z + proj_z.x)
uniform float depth_tolerance;  // relative

void main()
{
    vec2 uv = (vary_clip.xy / vary_clip.w) * 0.5 + 0.5;
    float depth = texture(depthMap, uv).r;
    float gbuffer_dist = proj_z.y / ((depth * 2.0 - 1.0) + proj_z.x);
    if (abs(vary_clip.w - gbuffer_dist) > depth_tolerance * gbuffer_dist + 0.005)
    {
        discard;
    }

    vec2 mv = vec2(0.0);
    if (vary_prev.w > 0.0 && vary_cur.w > 0.0)
    {
        mv = (vary_prev.xy / vary_prev.w - vary_cur.xy / vary_cur.w) * 0.5 * render_res;
    }
    frag_color = vec4(mv, 0.0, 1.0);
}
