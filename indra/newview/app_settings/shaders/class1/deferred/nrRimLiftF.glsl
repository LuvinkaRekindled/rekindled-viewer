/**
 * @file nrRimLiftF.glsl
 *
 * DLSS-NR rim lift (doc/vulkan/PLAN-dlss-nr.md, local testing only). The neural renderer draws a thin dark band on the darker side of silhouette edges.
 * Where it made a pixel darker than its input and the pixel sits within rim_width pixels of a depth discontinuity, the luminance is lifted by rim_strength
 * toward what the model did to the pixel's own neighbourhood away from the edge (the same darkening ratio), never past it; the colour the model chose is
 * kept (the ratio of luminances scales the rgb). Pixels away from depth edges are untouched, so the shading the model adds inside a face stays.
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

uniform sampler2D diffuseRect;      // the neural renderer's output
uniform sampler2D emissiveRect;     // its input (the picture before it)
uniform sampler2D depthMap;         // the scene depth (render size)
uniform vec2 depth_res;             // the depth texture's size in pixels
uniform vec2 clip_planes;           // near, far
uniform float rim_width;            // how far from a depth edge the lift reaches, in depth pixels
uniform float rim_strength;         // 0 none .. 1 the input's luminance at the edge
uniform float rim_edge;             // the relative step in linear depth that counts as an edge

float lin(float d)
{
    d = d * 2.0 - 1.0;
    return clip_planes.x * 2.0 * clip_planes.y / (clip_planes.y + clip_planes.x - d * (clip_planes.y - clip_planes.x));
}

void main()
{
    vec2 tc = vary_fragcoord;
    vec4 nr = texture(diffuseRect, tc);
    frag_color = nr;
    if (rim_strength <= 0.0)
    {
        return;
    }
    const vec3 lum = vec3(0.299, 0.587, 0.114);
    float ln = dot(nr.rgb, lum);
    float ls = dot(texture(emissiveRect, tc).rgb, lum);
    if (ln >= ls || ls < 1e-4)
    {
        return;         // not darkened: not a rim
    }
    // the edge: a step in linear depth within rim_width pixels, in any of eight directions
    float d0 = lin(texture(depthMap, tc).r);
    vec2 px = rim_width / depth_res;
    float edge = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float a = float(i) * 0.785398;
        vec2 dir = vec2(cos(a), sin(a));
        float d1 = lin(texture(depthMap, tc + dir * px).r);
        float d2 = lin(texture(depthMap, tc + dir * px * 0.5).r);
        edge = max(edge, max(abs(d1 - d0) / max(min(d0, d1), 1e-3), abs(d2 - d0) / max(min(d0, d2), 1e-3)));
    }
    float w = smoothstep(rim_edge * 0.5, rim_edge, edge) * rim_strength;
    if (w <= 0.0)
    {
        return;
    }
    // what the model did to this pixel's own neighbourhood, away from the edge: the darkening ratio (nr / input) of the samples on this pixel's side of the edge
    // (a depth close to d0), two and three rim widths out. The pixel is lifted toward that ratio, never past it: the model's local tone stays, only the extra
    // darkening it drew at the edge goes (lifting toward the untouched input instead made a light band wherever one part crosses another)
    float ratio_sum = 0.0, ratio_n = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float a = float(i) * 0.785398;
        vec2 dir = vec2(cos(a), sin(a));
        for (int k = 2; k <= 3; ++k)
        {
            vec2 t = tc + dir * px * float(k);
            float dk = lin(texture(depthMap, t).r);
            if (abs(dk - d0) / max(min(d0, dk), 1e-3) < rim_edge)
            {
                float lnk = dot(texture(diffuseRect, t).rgb, lum);
                float lsk = dot(texture(emissiveRect, t).rgb, lum);
                if (lsk > 1e-4)
                {
                    ratio_sum += lnk / lsk;
                    ratio_n += 1.0;
                }
            }
        }
    }
    if (ratio_n < 1.0)
    {
        return;
    }
    float target = ls * (ratio_sum / ratio_n);
    if (target <= ln)
    {
        return;         // the neighbourhood is as dark or darker: nothing extra was drawn here
    }
    float lt = mix(ln, target, w);
    frag_color.rgb = nr.rgb * (lt / max(ln, 1e-4));
}
