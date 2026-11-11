/**
 * @file rtShadowPrepF.glsl
 *
 * <FS> Rekindled: the penumbra filter's input (doc/vulkan/PLAN-rt-shadow-penumbra.md, step 2b), one pixel of a target with a mip chain (mRtSunHard, RGBA16F, TMG_AUTO):
 *   r = the hard sun shadow of the centre ray (the lightmap's sun channel)
 *   g = this pixel's penumbra half-width in render pixels: blocker distance x tan(sun radius) x focal / depth, capped (0 where the pixel is lit: no blocker)
 *   b = 1 where the pixel has a blocker, 0 where it is lit
 *   a = 1 where the pixel is a surface that belongs to its surroundings, 0 where it is sky or an island: a pixel with two or more of its four neighbours off its tangent plane (or
 *       sky), such as a one pixel gap of a mesh that shows the scene behind it (the sky, or a lit wall: the user's flecks, sun behind him, his skin in shadow, a bright scene ahead,
 *       sunfilter=0 made them go, 2026-11-10); r is premultiplied by it. Such a pixel's sun value is another surface's, and in the mip chain it was averaged into the skin around
 *       it, flecks that came and went with the upscaler's jitter: the same fault as the half resolution lightmap blur's (cdbe206729). The filter divides by the average of a, so
 *       only pixels that belong to their surface count. (A straight silhouette keeps its pixels: one neighbour across; a 3 x 3 hole or larger keeps its middle.)
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
uniform vec2 screen_res;           // the G-buffer size (the neighbours' distance in texture coordinates)

in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);
float getDepth(vec2 pos_screen);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float sun = texture(lightMap, tc).r;
    float valid = getDepth(tc) < 1.0 ? 1.0 : 0.0;
    vec3 pos = getPosition(tc).xyz;
    if (valid > 0.0)
    {
        // an island: two or more of the four neighbours are sky or off this pixel's tangent plane (the plane distance tolerance of the lightmap blur and of the filter)
        vec3 n = getNorm(tc).xyz;
        float tol = pos.z * pos.z * 0.00005;
        vec2 px = 1.0 / screen_res;
        int off = 0;
        for (int i = 0; i < 4; ++i)
        {
            vec2 s = tc + vec2((i == 0 ? px.x : (i == 1 ? -px.x : 0.0)), (i == 2 ? px.y : (i == 3 ? -px.y : 0.0)));
            if (getDepth(s) >= 1.0)
            {
                ++off;
                continue;
            }
            float d = dot(n, getPosition(s).xyz - pos);
            if (d * d > tol)
            {
                ++off;
            }
        }
        if (off >= 2)
        {
            valid = 0.0;
        }
    }
    float blocker_t = texture(emissiveRect, tc).r;
    float depth = -pos.z;
    float r = blocker_t > 0.0 ? min(blocker_t * pen_tan * pen_focal / max(depth, 0.05), pen_max) : 0.0;
    frag_color = vec4(sun * valid, r, blocker_t > 0.0 ? 1.0 : 0.0, valid);
}
