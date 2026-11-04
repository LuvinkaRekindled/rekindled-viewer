/**
 * @file class2/deferred/sunLightSSAOF.glsl
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

//class 2 -- shadows and SSAO

// Inputs
in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
float getDepth(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);

uniform sampler2D diffuseRect;     // <FS> last frame's shadow history (rg: shadow, depth; b: frames averaged) when RenderRTShadowAdaptiveRays asks, else the G-buffer albedo (unused)
uniform int spot_shadow_mask;       // <FS> bit 0 / 1: the projector light with a shadow map in slot 0 / 1 exists this frame (pipeline.cpp, mShadowSpotLight). Only those lights read the lightmap's b / a channel (spotLightF.glsl proj_shadow_idx), so nothing is sampled for the others
uniform float adapt_hist;            // 1: diffuseRect is the history
#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
void rtSetAdaptCount(float c);      // (shadowUtil.glsl defines it only with the ray traced shadows)
#endif

float sampleDirectionalShadow(vec3 shadow_pos, vec3 norm, vec2 pos_screen);
float sampleSpotShadow(vec3 shadow_pos, vec3 norm, int index, vec2 pos_screen);
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen);

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    // <FS> a sky pixel (nothing drawn: the far plane) has no shadow or occlusion to work out: the soften pass takes its colour from the G-buffer and never reads the lightmap there
    if (getDepth(pos_screen) >= 1.0)
    {
        frag_color = vec4(1.0);
        return;
    }
    vec4 pos  = getPosition(pos_screen);
    vec4 norm = getNorm(pos_screen);

#if defined(SUN_SHADOW) && defined(LL_RT_SHADOWS)
    if (adapt_hist > 0.5)
    {
        rtSetAdaptCount(texture(diffuseRect, pos_screen).b);
    }
#endif

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
    col.g = calcAmbientOcclusion(pos, norm.xyz, pos_screen);
    col.b = (spot_shadow_mask & 1) != 0 ? sampleSpotShadow(pos.xyz, norm.xyz, 0, pos_screen) : 1.0;
    col.a = (spot_shadow_mask & 2) != 0 ? sampleSpotShadow(pos.xyz, norm.xyz, 1, pos_screen) : 1.0;

    frag_color = clamp(col, vec4(0), vec4(1));
}
