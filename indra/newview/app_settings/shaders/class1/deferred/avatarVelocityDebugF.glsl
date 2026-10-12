/**
 * @file avatarVelocityDebugF.glsl
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

// Debug view of a motion target: red/green = pixels moved in x/y (full at 4 pixels), blue = the alpha flag (avatar pixels in the avatar velocity target).
out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform float show_flag;     // 1: blue = the alpha flag (the avatar target); 0: the target has no alpha

void main()
{
    vec4 v = texture(diffuseRect, vary_fragcoord);
    frag_color = vec4(clamp(abs(v.xy) * 0.25, 0.0, 1.0), v.a * 0.35 * show_flag, 1.0);
}
