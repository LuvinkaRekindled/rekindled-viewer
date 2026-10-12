/**
 * @file motionVectorsF.glsl
 *
 * Camera-only motion vectors for temporal upscalers: where was this pixel last frame, in render pixels (previous minus current, as DLSS wants).
 * Moving objects and avatars are not accounted for yet.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D depthMap;
uniform mat4 to_cur;        // jittered clip space of this frame -> unjittered clip space of this frame
uniform mat4 to_prev;       // jittered clip space of this frame -> unjittered clip space of the last frame
uniform vec2 render_res;
uniform sampler2D diffuseRect;  // <FS> the avatar velocity target: rg = total screen motion of an avatar pixel, a = 1 where an avatar wrote
uniform float use_avatar;       // 1 when the avatar velocity pass ran this frame

void main()
{
    float depth = texture(depthMap, vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 cur = to_cur * ndc;
    vec4 prev = to_prev * ndc;
    vec2 cur_ndc = cur.xy / cur.w;
    vec2 prev_ndc = prev.xy / prev.w;
    // NDC spans 2 units across the render target. Texel rows grow with NDC y here (the mirror keeps GL's orientation), so no flip is needed.
    vec2 mv = (prev_ndc - cur_ndc) * 0.5 * render_res;
    if (use_avatar > 0.5)
    {
        vec4 av = texture(diffuseRect, vary_fragcoord);
        if (av.a > 0.5)
        {
            mv = av.xy;     // an avatar pixel: its own motion replaces the camera motion
        }
    }
    frag_color = vec4(mv, 0.0, 0.0);
}
