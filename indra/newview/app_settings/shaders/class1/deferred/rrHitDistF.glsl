/**
 * @file rrHitDistF.glsl
 *
 * Moves the traced reflection's hit distance out of the alpha of the lit picture (where the soften pass left it) for DLSS ray reconstruction.
 *   copy mode (clear_only = 0): the alpha of diffuseRect becomes the red of the output (target: the hit distance texture)
 *   clear mode (clear_only = 1): outputs alpha 0 (drawn into the lit picture with only its alpha written, so that the glow amount starts at zero again)
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

uniform sampler2D diffuseRect;
uniform float clear_only;

void main()
{
    float d = clear_only > 0.5 ? 0.0 : texture(diffuseRect, vary_fragcoord).a;
    frag_color = vec4(d, d, d, 0.0);
}
