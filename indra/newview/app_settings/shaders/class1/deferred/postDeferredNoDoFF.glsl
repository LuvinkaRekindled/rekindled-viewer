/**
 * @file postDeferredNoDoFF.glsl
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

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;

uniform vec2 screen_res;
uniform vec2 depth_texel;
uniform float depth_radius;
uniform vec2 depth_jitter;
in vec2 vary_fragcoord;

//=================================
// borrowed noise from:
//  <https://www.shadertoy.com/view/4dS3Wd>
//  By Morgan McGuire @morgan3d, http://graphicscodex.com
//
float hash(float n) { return fract(sin(n) * 1e4); }
float hash(vec2 p) { return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x)))); }

float noise(float x) {
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), u);
}

float noise(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);

    // Four corners in 2D of a tile
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    // Simple 2D lerp using smoothstep envelope between the values.
    // return vec3(mix(mix(a, b, smoothstep(0.0, 1.0, f.x)),
    //          mix(c, d, smoothstep(0.0, 1.0, f.x)),
    //          smoothstep(0.0, 1.0, f.y)));

    // Same code, with the clamps in smoothstep and common subexpressions
    // optimized away.
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

//=============================

vec3 clampHDRRange(vec3 color);


void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord.xy);

#ifdef HAS_NOISE
    vec2 tc = vary_fragcoord.xy*screen_res*4.0;
    vec3 seed = (diff.rgb+vec3(1.0))*vec3(tc.xy, tc.x+tc.y);
    vec3 nz = vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb));
    diff.rgb += nz*0.003;
#endif

    diff.rgb = clampHDRRange(diff.rgb);
    frag_color = diff;

    // <FS> This depth is what everything drawn after the upscale (selection outlines, beacons...) is tested against. With DLSS it was drawn with the camera jitter,
    // so a line lying on a surface passed or failed its depth test depending on the frame (the outline of a selected object vibrated). depth_texel is non-zero
    // while such an overlay is on screen: the farthest depth of the 3x3 render pixels around is then used, plus the local depth range as a margin. The outline
    // lines are drawn with a polygon offset that pushes them just behind the surface (a few output pixels of slope), so without the margin they sat on the edge of
    // the test and the jitter phase decided; with it a line on a surface is always in front and a line well behind a surface (hidden part) still is not.
    float depth = texture(depthMap, vary_fragcoord.xy).r;
    if (depth_texel.x > 0.0)
    {
        if (depth_radius < 0.0)
        {
            depth = 1.0;    // no depth test for overlays
        }
        else if (depth_radius == 0.0)
        {
            // Depth of the point that was drawn at this pixel without the jitter: the render pixel (i, j) holds the scene point at (i, j) minus the jitter, so the
            // unjittered position is looked up at uv + jitter. Depth is linear over the screen on a plane, so the bilinear value of the four render pixels around is the
            // same whatever phase the jitter has; only a depth edge inside the four changes it. The margin is then the range of the four (a plane's slope).
            vec2 size = vec2(textureSize(depthMap, 0));
            vec2 f = (vary_fragcoord.xy + depth_jitter) * size - vec2(0.5);
            vec2 base = clamp(floor(f), vec2(0.0), size - vec2(2.0));
            vec2 fr = clamp(f - base, vec2(0.0), vec2(1.0));
            vec2 uv0 = (base + vec2(0.5)) / size;
            vec2 t = 1.0 / size;
            float a = texture(depthMap, uv0).r;
            float b = texture(depthMap, uv0 + vec2(t.x, 0.0)).r;
            float c = texture(depthMap, uv0 + vec2(0.0, t.y)).r;
            float d = texture(depthMap, uv0 + t).r;
            float bil = mix(mix(a, b, fr.x), mix(c, d, fr.x), fr.y);
            float hi = max(max(a, b), max(c, d));
            float lo = min(min(a, b), min(c, d));
            // a depth edge (the far one more than 3 % farther than the near one, 1 - depth is about 1 / distance) gets no margin: the margin is for the slope of a
            // surface, and at an occluder's silhouette it would let the hidden outline of the selection show through along the edge
            bool edge = (1.0 - lo) > 1.03 * max(1.0 - hi, 1.0e-7);
            depth = min(1.0, bil + (edge ? 0.0 : 3.0 * (hi - lo)) + 1.0e-6);
        }
        else
        {
            vec2 t = depth_texel;
            vec2 uv = vary_fragcoord.xy;
            float lo = depth;
            float hi = depth;
            for (int j = -2; j <= 2; ++j)
            {
                for (int i = -2; i <= 2; ++i)
                {
                    if (abs(float(i)) <= depth_radius && abs(float(j)) <= depth_radius)
                    {
                        float d = texture(depthMap, uv + vec2(float(i), float(j)) * t).r;
                        lo = min(lo, d);
                        hi = max(hi, d);
                    }
                }
            }
            depth = min(1.0, hi + (hi - lo) + 1.0e-6);
        }
    }
    gl_FragDepth = depth;
}

