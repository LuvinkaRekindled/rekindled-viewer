/**
 * @file rtShadowFilterF.glsl
 *
 * <FS> Rekindled: the penumbra of the ray traced sun shadow from the blocker distance (doc/vulkan/PLAN-rt-shadow-penumbra.md, step 2b).
 *
 * The sun lightmap pass traces ONE deterministic ray per pixel at the centre of the sun's disc (rt-debug.txt suncentre): its answer is a hard, perfectly still shadow, and the
 * distance to the blocker it met is in a second map. rtShadowPrepF.glsl puts that hard shadow, each pixel's penumbra half-width in pixels (blocker distance x tan(sun radius) x
 * focal / depth: 1.2 cm per metre of blocker distance at 0.35 degrees, a leaf 20 m up gives 24 cm, a hand 30 cm from a leg 4 mm) and a blocker mask into a target with a mip
 * chain. This pass turns the hard edge into the physical penumbra, the way percentage-closer soft shadows do:
 *   1. the width: the pixel's own half-width, or where that is smaller, the AVERAGE half-width of the blockers in the search window around it, read from the mip chain in one
 *      fetch (width sum / blocker count at the window's level: every pixel of the window counted, a lit pixel beside a shadow gets the shadow's width).
 *   2. the filter: the hard shadow averaged over a disc of that radius (a fixed Vogel spiral, no rotation: the same picture every frame), weights falling off to the rim, each
 *      tap read at the mip level of the taps' spacing (so a tap is the average of its share of the disc, not one pixel of it; rt-debug.txt penlod biases that level).
 * A tap on another surface (the lightmap blur's plane distance test) does not count, and neither do the sky pixels inside a tap's mip footprint: the prep target holds the sun channel
 * premultiplied by a surface flag (a), and every read here is divided by the flag's average (a sky pixel's sun value is 1 and was averaged into the skin around a one pixel gap of a
 * mesh: flecks, as the half resolution lightmap blur's, cdbe206729). Only the sun channel (r) is filtered; SSAO (g) and the spot shadows (b, a) pass through.
 *
 * No stochastic term anywhere: nothing to accumulate, nothing for the upscaler to average. Earlier forms, 2026-11-04: separable (streaks and rings), 16 point taps (fibrous edges),
 * 12 point search taps (light rims between overlapping shadows).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D lightMap;        // r: the sun shadow (the hard answer of the centre ray), g: SSAO, b and a: the spot shadows
uniform sampler2D diffuseRect;     // rtShadowPrepF.glsl's target with its mip chain (trilinear): r the hard sun shadow, g the half-width in pixels, b the blocker mask
uniform vec2 screen_res;
uniform float pen_max;             // the widest half-width in pixels (rt-debug.txt penmax, 8): the search window's half-width too
uniform float pen_lod;             // a bias in mip levels on the taps' level (rt-debug.txt penlod, 1)

in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
vec4 getNorm(vec2 pos_screen);
float getDepth(vec2 pos_screen);

#define FILTER_TAPS 16

// a point of the unit disc: the Vogel spiral (the golden angle), evenly spread, the same every frame
vec2 vogel(int i, int n)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * 2.3999632;
    return vec2(cos(a), sin(a)) * r;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 c = texture(lightMap, tc);
    if (getDepth(tc) >= 1.0)
    {
        frag_color = c;             // a sky pixel: nothing reads its sun channel
        return;
    }

    // 1. the width: the pixel's own, or the window's average blocker width where that is wider
    float r = textureLod(diffuseRect, tc, 0.0).g;
    vec2 win = textureLod(diffuseRect, tc, log2(max(pen_max, 1.0))).gb;      // (width sum, blocker count) over the window, each divided by the same pixel count
    if (win.y > 0.001)
    {
        r = max(r, win.x / win.y);
    }
    if (r < 0.5)
    {
        frag_color = c;             // hard: a contact, or no blocker anywhere near
        return;
    }

    vec3 pos = getPosition(tc).xyz;
    vec3 n = getNorm(tc).xyz;
    // the lightmap blur's plane distance tolerance: looser with distance, where angles and distances are abrupt within a small screen area
    float tol = pos.z * pos.z * 0.00005;

    // 2. the filter: the hard shadow over the disc of radius r, the weight falling to 0 at the rim; each tap at the mip level of the taps' spacing (sqrt(pi / N) r for N taps on the disc)
    float lod = max(log2(r * 0.443) + pen_lod, 0.0);
    vec4 m0 = textureLod(diffuseRect, tc, lod);
    float sum = m0.r;           // (premultiplied by the surface flag: a = the share of surface pixels in the footprint)
    float wsum = m0.a;
    for (int i = 0; i < FILTER_TAPS; ++i)
    {
        vec2 u = vogel(i, FILTER_TAPS);
        vec2 stc = tc + u * r / screen_res;
        vec3 spos = getPosition(stc).xyz;
        float d = dot(n, spos - pos);
        if (d * d > tol)
        {
            continue;
        }
        float w = 1.0 - 0.75 * dot(u, u);
        vec4 m = textureLod(diffuseRect, stc, lod);
        sum += m.r * w;
        wsum += m.a * w;
    }
    if (wsum > 0.001)
    {
        c.r = sum / wsum;
    }
    frag_color = max(c, vec4(0.0));
}
