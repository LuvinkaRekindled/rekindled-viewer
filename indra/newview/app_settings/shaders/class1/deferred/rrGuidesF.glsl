/**
 * @file rrGuidesF.glsl
 *
 * Guide buffers for DLSS ray reconstruction, made from the G-buffer: what the denoiser needs to know about the surface a (noisy) pixel shows.
 *   0: diffuse albedo (linear)
 *   1: specular albedo = F0 times the environment BRDF (the split sum approximation, as in the lighting of the main pass)
 *   2: world space normal in xyz, perceptual roughness in w
 * Sky and other pixels that are not lit by a surface get a white diffuse albedo (the radiance is then the whole signal), no specular and a rough surface facing the viewer.
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

out vec4 frag_data[4];

in vec2 vary_fragcoord;

uniform mat3 view_to_world;     // the rotation from the eye space the G-buffer normals are in to the world space the denoiser is told about

// <FS> the fourth output is the motion vector of what the traced reflection shows (what rrReflMotionF.glsl made in a pass of its own): the reads and the maths are the same
uniform sampler2D diffuseMap;       // the surface motion vectors (rg, pixels)
uniform sampler2D bumpMap;          // the traced reflection's hit distance (r, metres from the surface; 0 = not traced)
uniform mat4 inv_scene;             // jittered clip space of this frame -> world
uniform mat4 view_proj_cur;         // world -> unjittered clip space of this frame
uniform mat4 view_proj_prev;        // world -> unjittered clip space of the last frame
uniform vec3 cam_pos;               // the camera in the world
uniform vec2 render_res;
uniform float use_refl;             // 0: the hit distance is not valid yet (the output is the surface motion)

vec4 getNorm(vec2 pos_screen);

vec4 getPosition(vec2 pos_screen);
float getDepth(vec2 pos_screen);
vec3 srgb_to_linear(vec3 c);
GBufferInfo getGBuffer(vec2 screenpos);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

// the analytic approximation of the split sum environment BRDF (Karis, mobile) applied to F0
vec3 envBrdfApprox(vec3 f0, float roughness, float nov)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nov)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + vec3(ab.y);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);

    vec3 diffuse = vec3(1.0);
    vec3 specular = vec3(0.0);
    vec3 n = vec3(0.0, 0.0, 1.0);
    float rough = 1.0;

    if (depth < 1.0)
    {
        GBufferInfo gb = getGBuffer(tc);
        if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
        {
            // the sky: the defaults
        }
        else
        {
            vec3 v = -normalize(getPosition(tc).xyz);
            float nov = clamp(abs(dot(gb.normal, v)), 0.001, 1.0);
            vec3 f0 = vec3(0.04);
            if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
            {
                vec3 orm = gb.specular.rgb;
                rough = clamp(orm.g, 0.0, 1.0);
                vec3 d = vec3(0.0);
                calcDiffuseSpecular(gb.albedo.rgb, orm.b, d, f0);
                diffuse = d;
            }
            else
            {
                // legacy materials: sRGB colours, the alpha of the specular is the glossiness
                diffuse = srgb_to_linear(gb.albedo.rgb);
                float gloss = gb.specular.a;
                rough = clamp(1.0 - gloss, 0.0, 1.0);
                if (gloss > 0.0)
                {
                    f0 = max(srgb_to_linear(gb.specular.rgb), vec3(0.04));
                }
            }
            specular = envBrdfApprox(f0, rough, nov);
            n = normalize(view_to_world * gb.normal);
        }
    }

    frag_data[0] = vec4(clamp(diffuse, 0.0, 1.0), 1.0);
    frag_data[1] = vec4(clamp(specular, 0.0, 1.0), 1.0);
    frag_data[2] = vec4(n, rough);

    vec2 mv = texture(diffuseMap, tc).rg;
    float t = use_refl > 0.5 ? texture(bumpMap, tc).r : 0.0;
    if (t > 0.0 && depth < 1.0)
    {
        vec4 w = inv_scene * vec4(tc * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 p = w.xyz / w.w;
        vec3 nw = normalize(view_to_world * getNorm(tc).xyz);
        vec3 r = reflect(normalize(p - cam_pos), nw);
        vec4 h = vec4(p + r * t, 1.0);
        vec4 cur = view_proj_cur * h;
        vec4 prev = view_proj_prev * h;
        if (cur.w > 0.0 && prev.w > 0.0)
        {
            mv = (prev.xy / prev.w - cur.xy / cur.w) * 0.5 * render_res;
        }
    }
    frag_data[3] = vec4(mv, 0.0, 0.0);
}
