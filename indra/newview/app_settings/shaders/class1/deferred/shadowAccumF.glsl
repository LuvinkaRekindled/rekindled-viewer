/**
 * @file shadowAccumF.glsl
 * @brief Temporal accumulation of the ray traced sun shadow (doc/vulkan/PLAN-rt-shadows.md, "Soft ray traced sun shadows"): the sun lightmap (r = sun shadow, g = AO, b/a = spot shadows)
 *        of this frame is blended with last frame's accumulated shadow, found again through the camera matrices (or, for avatar pixels, the avatar velocity target).
 *
 * Output 0 is lightmap shaped (the blur pass reads it in place of the sun lightmap); output 1 is the history for the next frame: r = accumulated shadow, g = view distance, b = how many
 * frames went into it. A history sample is used only if it saw the same surface (view distance within a tolerance, or an avatar pixel that has a velocity) and is clamped to the
 * range of the current pixel's neighbourhood, so a shadow that moves is not smeared.
 *
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc. and Firestorm contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 */

out vec4 frag_data[2];

in vec2 vary_fragcoord;

uniform sampler2D lightMap;         // this frame's sun lightmap
uniform sampler2D diffuseRect;      // history (output 1 of last frame)
uniform sampler2D emissiveRect;     // avatar velocity target: rg = pixels moved (previous minus current), a = 1 where an avatar wrote
uniform mat4 prev_clip_from_view;   // this frame's view space -> last frame's (unjittered) clip space
uniform mat4 prev_view_from_view;   // this frame's view space -> last frame's view space
uniform vec2 render_res;
uniform float history_len;          // frames at most that go into the average
uniform float use_history;          // 0: nothing to reproject from (first frame, a pause, a resize)
uniform float use_velocity;         // 1: the avatar velocity target is valid this frame

vec4 getPosition(vec2 pos_screen);

void main()
{
    vec2 uv = vary_fragcoord;
    vec4 cur = texture(lightMap, uv);
    vec3 p = getPosition(uv).xyz;
    float depth = -p.z;
    float shadow = cur.r;
    float count = 1.0;

    if (use_history > 0.5 && depth > 0.01 && depth < 1500.0)
    {
        vec4 vel = (use_velocity > 0.5) ? texture(emissiveRect, uv) : vec4(0.0);
        bool avatar = vel.a > 0.5;
        vec2 prev_uv = uv;
        float expected = depth;
        bool valid = true;
        if (avatar)
        {
            prev_uv = uv + vel.xy / render_res;
        }
        else
        {
            vec4 pc = prev_clip_from_view * vec4(p, 1.0);
            valid = pc.w > 0.0;
            prev_uv = pc.xy / max(pc.w, 1e-4) * 0.5 + 0.5;
            expected = -(prev_view_from_view * vec4(p, 1.0)).z;
        }
        if (valid && prev_uv.x > 0.0 && prev_uv.x < 1.0 && prev_uv.y > 0.0 && prev_uv.y < 1.0)
        {
            vec4 h = texture(diffuseRect, prev_uv);
            if (avatar || abs(h.g - expected) <= 0.02 * expected + 0.05)
            {
                // the range of the current neighbourhood: history outside it belongs to a shadow that was somewhere else
                vec2 texel = 1.0 / render_res;
                float mn = cur.r, mx = cur.r;
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        float s = texture(lightMap, uv + vec2(float(x), float(y)) * texel).r;
                        mn = min(mn, s);
                        mx = max(mx, s);
                    }
                }
                float hist = clamp(h.r, mn - 0.03, mx + 0.03);
                count = min(h.b + 1.0, history_len);
                shadow = mix(hist, cur.r, 1.0 / count);
            }
        }
    }

    frag_data[0] = vec4(shadow, cur.g, cur.b, cur.a);
    frag_data[1] = vec4(shadow, depth, count, 1.0);
}
