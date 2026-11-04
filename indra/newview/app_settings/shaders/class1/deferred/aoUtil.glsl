/**
 * @file class1/deferred/aoUtil.glsl
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

uniform sampler2D   noiseMap;
uniform sampler2D   depthMap;

uniform float ssao_radius;
uniform float ssao_max_radius;
uniform float ssao_factor;
uniform float ssao_factor_inv;

uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int ao_samples;          // <FS> 4: half the taps, a different half on each frame (even and odd kernel entries) for the upscaler to average (set only when the AO noise moves with it); anything else: all 8
uniform float ao_noise_frame;    // <FS> set by the sun lightmap pass (the frame of the jitter sequence, 0 without the upscaler): the noise lookup of the AO moves on every frame so that the upscaler averages it (the sun shadow's own noise, rt_noise_frame, moves only with ray traced shadows)

vec2 getScreenCoordinateAo(vec2 screenpos)
{
    vec2 sc = screenpos.xy * 2.0;
    return sc - vec2(1.0, 1.0);
}

float getDepthAo(vec2 pos_screen)
{
    float depth = texture(depthMap, pos_screen).r;
    return depth;
}

vec4 getPositionAo(vec2 pos_screen)
{
    float depth = getDepthAo(pos_screen);
    vec2 sc = getScreenCoordinateAo(pos_screen);
    vec4 ndc = vec4(sc.x, sc.y, 2.0*depth-1.0, 1.0);
    vec4 pos = inv_proj * ndc;
    pos /= pos.w;
    pos.w = 1.0;
    return pos;
}

vec2 getKern(int i)
{
    vec2 kern[8];
    // exponentially (^2) distant occlusion samples spread around origin
    kern[0] = vec2(-1.0, 0.0) * 0.125*0.125;
    kern[1] = vec2(1.0, 0.0) * 0.250*0.250;
    kern[2] = vec2(0.0, 1.0) * 0.375*0.375;
    kern[3] = vec2(0.0, -1.0) * 0.500*0.500;
    kern[4] = vec2(0.7071, 0.7071) * 0.625*0.625;
    kern[5] = vec2(-0.7071, -0.7071) * 0.750*0.750;
    kern[6] = vec2(-0.7071, 0.7071) * 0.875*0.875;
    kern[7] = vec2(0.7071, -0.7071) * 1.000*1.000;

    return kern[i] / screen_res;
}

//calculate decreases in ambient lighting when crowded out (SSAO)
void aoAccumulate(int ki, vec2 pos_screen, vec2 noise_reflect, float scale, vec3 pos_world, vec3 norm, inout float angle_hidden, inout float points)
{
    vec2 samppos_screen = pos_screen + scale * reflect(getKern(ki), noise_reflect);
    vec3 samppos_world = getPositionAo(samppos_screen).xyz;

    vec3 diff = pos_world - samppos_world;
    float dist2 = dot(diff, diff);

    // assume each sample corresponds to an occluding sphere with constant radius, constant x-sectional area
    // --> solid angle shrinking by the square of distance
    float funky_val = (dot((samppos_world - 0.05*norm - pos_world), norm) > 0.0) ? 1.0 : 0.0;
    angle_hidden = angle_hidden + funky_val * min(1.0/dist2, ssao_factor_inv);

    // 'blocked' samples (significantly closer to camera relative to pos_world) are "no data", not "no occlusion"
    float diffz_val = (diff.z > -1.0) ? 1.0 : 0.0;
    points = points + diffz_val;
}

float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen)
{
    float ret = 1.0;
    vec3 pos_world = pos.xyz;
    // <FS> the lookup moves by whole texels from frame to frame (a golden ratio sequence, 64 frames long): the same pattern every frame is a fixed grain that nothing averages (the lightmap blur that
    // used to smooth it is left out when the upscaler is on); with a pattern that changes the upscaler's accumulation turns it into a smooth value
    vec2 noise_shift = floor(fract(ao_noise_frame * vec2(0.7548776662, 0.5698402910)) * 128.0) / 128.0;
    vec2 noise_reflect = texture(noiseMap, pos_screen.xy * (screen_res / 128) + noise_shift).xy;

    float angle_hidden = 0.0;
    float points = 0;

    float scale = min(ssao_radius / -pos_world.z, ssao_max_radius);

    // it was found that keeping # of samples a constant was the fastest, probably due to compiler optimizations (unrolling?): the two loops each have a constant count
    if (ao_samples == 4)
    {
        int odd = int(ao_noise_frame) & 1;
        for (int i = 0; i < 4; i++)
        {
            aoAccumulate(2 * i + odd, pos_screen, noise_reflect, scale, pos_world, norm, angle_hidden, points);
        }
    }
    else
    {
        for (int i = 0; i < 8; i++)
        {
            aoAccumulate(i, pos_screen, noise_reflect, scale, pos_world, norm, angle_hidden, points);
        }
    }

    angle_hidden = min(ssao_factor*angle_hidden/points, 1.0);

    float points_val = (points > 0.0) ? 1.0 : 0.0;
    ret = (1.0 - (points_val * angle_hidden));

    ret = max(ret, 0.0);
    return min(ret, 1.0);
}

