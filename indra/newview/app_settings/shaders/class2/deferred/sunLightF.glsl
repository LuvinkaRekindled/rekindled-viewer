/**
 * @file sunLightF.glsl
 *
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
#if defined(LL_RT_SHADOWS)
// <FS> PLAN-rt-shadow-penumbra.md step 1: attachment 1 (R16F) takes the blocker distance of the centre sun ray in metres (0 = none); the cascades have one output as before.
// LL_RT_SHADOWS is a permutation of the sun program (llviewershadermgr.cpp): a program's own files are compiled with its mDefines, the global map reaches only the shared utility files (2026-11-04)
layout(location = 1) out vec4 frag_blocker;
float rtSunBlockerT();
#endif

//class 2, shadows, no SSAO

// Inputs
in vec2 vary_fragcoord;

uniform vec3 sun_dir;
uniform float shadow_bias;

vec4 getNorm(vec2 pos_screen);
vec4 getPosition(vec2 pos_screen);
float getDepth(vec2 pos_screen);

uniform sampler2D diffuseRect;     // <FS> last frame's shadow history (rg: shadow, depth; b: frames averaged) when RenderRTShadowAdaptiveRays asks, else the G-buffer albedo (unused)
uniform int spot_shadow_mask;       // <FS> bit 0 / 1: the projector light with a shadow map in slot 0 / 1 exists this frame (pipeline.cpp, mShadowSpotLight); nothing is sampled for the others
uniform float adapt_hist;            // 1: diffuseRect is the history
#if defined(LL_RT_SHADOWS)
void rtSetAdaptCount(float c);      // (shadowUtil.glsl defines it only with the ray traced shadows)
#endif

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen);

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    // <FS> a sky pixel (nothing drawn: the far plane) has no shadow to work out: the soften pass takes its colour from the G-buffer and never reads the lightmap there
    if (getDepth(pos_screen) >= 1.0)
    {
        frag_color = vec4(1.0);
#if defined(LL_RT_SHADOWS)
        frag_blocker = vec4(0.0);
#endif
        return;
    }
    vec4 pos        = getPosition(pos_screen);
    vec4 norm       = getNorm(pos_screen);

#if defined(LL_RT_SHADOWS)
    if (adapt_hist > 0.5)
    {
        rtSetAdaptCount(texture(diffuseRect, pos_screen).b);
    }
#endif

    vec4 col;
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
    col.g = 1.0f;
    col.b = (spot_shadow_mask & 1) != 0 ? sampleSpotShadow(pos.xyz, norm.xyz, 0, pos_screen) : 1.0;
    col.a = (spot_shadow_mask & 2) != 0 ? sampleSpotShadow(pos.xyz, norm.xyz, 1, pos_screen) : 1.0;

    frag_color = clamp(col, vec4(0), vec4(1));
#if defined(LL_RT_SHADOWS)
    frag_blocker = vec4(rtSunBlockerT(), 0.0, 0.0, 1.0);
#endif
}
