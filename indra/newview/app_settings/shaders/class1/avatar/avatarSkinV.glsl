/**
 * @file avatarSkinV.glsl
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


in vec4 weight;

uniform vec4 matrixPalette[45];

mat4 getSkinnedTransform()
{
    mat4 ret;
    // a NaN or out-of-range weight (the vertex data is rewritten every update) turned into an index far outside the palette: the read then lands in memory that is not ours (device lost)
    float wx = weight.x;
    // The palette holds 15 joints (slots 0-14). A body mesh can use all 15 (the right wrist of a mesh with its ancestor entries is joint 14, weight 14.0 for its pure vertices): the
    // limit used to be 13.999, which sent every vertex of joint 14 to joint 0 (the stretched spike from the right hand). The blend partner of joint 14 is joint 14 itself
    // (weight 14.x does not occur), so no read goes past slot 14 of a row (slot 45 would be outside the array).
    if (!(wx >= 0.0 && wx <= 14.0))
    {
        wx = 0.0;
    }
    int i = int(floor(wx));
    int n = min(i + 1, 14);
    float x = fract(wx);

    ret[0] = matrixPalette[i+0];
    ret[1] = matrixPalette[i+15];
    ret[2] = matrixPalette[i+30];
    if (x > 0.0)        // (the second joint of the pair is not read when the weight is zero: most body vertices belong to one joint)
    {
        ret[0] = mix(ret[0], matrixPalette[n+0], x);
        ret[1] = mix(ret[1], matrixPalette[n+15], x);
        ret[2] = mix(ret[2], matrixPalette[n+30], x);
    }
    ret[3] = vec4(0,0,0,1);

    return ret;

#ifdef IS_AMD_CARD
    // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
    vec4 dummy1 = matrixPalette[0];
    vec4 dummy2 = matrixPalette[44];
#endif
}
