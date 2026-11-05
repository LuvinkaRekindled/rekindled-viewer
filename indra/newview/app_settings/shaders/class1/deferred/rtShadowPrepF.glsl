/**
 * @file rtShadowPrepF.glsl
 *
 * <FS> Rekindled: the penumbra filter's input (doc/vulkan/PLAN-rt-shadow-penumbra.md, step 2b), one pixel of a target with a mip chain (mRtSunHard, RGBA16F, TMG_AUTO):
 *   r = the hard sun shadow of the centre ray (the lightmap's sun channel)
 *   g = this pixel's penumbra half-width in render pixels: blocker distance x tan(sun radius) x focal / depth, capped (0 where the pixel is lit: no blocker)
 *   b = 1 where the pixel has a blocker, 0 where it is lit
 * The mip chain is generated when the pass ends. A level's texel is the box average of its footprint, so at the level of the filter's search window g / b is the AVERAGE half-width
 * of the blockers in the window (the lit pixels count in neither): the blocker search of percentage-closer soft shadows, with every pixel of the window counted, in one read
 * (a fixed set of 12 point taps missed the shadow beside a gap pixel, which then kept no width and stayed hard: light rims between overlapping leaf shadows, 2026-11-04 23:46).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D lightMap;        // r: the sun shadow (the hard answer of the centre ray)
uniform sampler2D emissiveRect;    // the blocker distance map (mRtBlocker, R16F, metres along the sun ray; 0 = lit, no blocker)
uniform float pen_tan;             // tan of the sun's angular radius (RenderRTShadowSoftness)
uniform float pen_focal;           // the focal length in render pixels
uniform float pen_max;             // the widest half-width in pixels (rt-debug.txt penmax)

in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float sun = texture(lightMap, tc).r;
    float blocker_t = texture(emissiveRect, tc).r;
    float depth = -getPosition(tc).z;
    float r = blocker_t > 0.0 ? min(blocker_t * pen_tan * pen_focal / max(depth, 0.05), pen_max) : 0.0;
    frag_color = vec4(sun, r, blocker_t > 0.0 ? 1.0 : 0.0, 1.0);
}
