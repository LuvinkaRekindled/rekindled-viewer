/**
 * @file rrReflMotionF.glsl
 *
 * Specular motion vectors for DLSS ray reconstruction: where the image a traced reflection shows was in the last frame, in render pixels (previous minus current, like motionVectorsF.glsl).
 * The mirror surface does not move with the camera but what it reflects does (parallax): the point the reflection ray hit is projected with this frame's and last frame's camera
 * (static scene: objects that move themselves are not covered). Pixels that were not traced keep the surface vector.
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

uniform sampler2D diffuseRect;      // the surface motion vectors (rg, pixels)
uniform sampler2D emissiveRect;     // the traced reflection's hit distance (r, metres from the surface; 0 = not traced)
uniform mat4 inv_scene;             // jittered clip space of this frame -> world
uniform mat4 view_proj_cur;         // world -> unjittered clip space of this frame
uniform mat4 view_proj_prev;        // world -> unjittered clip space of the last frame
uniform vec3 cam_pos;               // the camera in the world
uniform mat3 view_to_world;         // the eye space the G-buffer normals are in -> world
uniform vec2 render_res;

vec4 getNorm(vec2 pos_screen);
float getDepth(vec2 pos_screen);

void main()
{
    vec2 uv = vary_fragcoord;
    vec2 mv = texture(diffuseRect, uv).rg;
    float t = texture(emissiveRect, uv).r;
    float depth = getDepth(uv);
    if (t > 0.0 && depth < 1.0)
    {
        vec4 w = inv_scene * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 p = w.xyz / w.w;
        vec3 n = normalize(view_to_world * getNorm(uv).xyz);
        vec3 r = reflect(normalize(p - cam_pos), n);
        vec4 h = vec4(p + r * t, 1.0);
        vec4 cur = view_proj_cur * h;
        vec4 prev = view_proj_prev * h;
        if (cur.w > 0.0 && prev.w > 0.0)
        {
            mv = (prev.xy / prev.w - cur.xy / cur.w) * 0.5 * render_res;
        }
    }
    frag_color = vec4(mv, 0.0, 0.0);
}
