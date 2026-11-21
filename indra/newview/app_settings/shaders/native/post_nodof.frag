// post_nodof.frag: the last pass before the picture goes to the screen (or the upscaler's output): the picture copied with its colours kept in the displayable range (HAS_NOISE: and a dither added so that the
// gradients do not band), and the depth the overlays are tested against. A translation of "Deferred Post NoDoF Shader" and "Deferred Post NoDoF Noise Shader" (deferred/postDeferredNoDoFF.glsl, HAS_NOISE);
// pairs with fullscreen.vert.
// The depth: with the upscaler's jitter the scene was drawn shifted by a fraction of a pixel, so a line lying on a surface (the outline of a selected object, a beacon) passed or failed its depth test depending
// on the frame. depth_texel is non-zero while such an overlay is on screen, and then the farthest depth of the render pixels around is used plus the local depth range as a margin: depth_radius < 0 no depth
// test; 0 the unjittered position's bilinear depth, with the slope's margin except at an edge; 1 or 2 the 3x3 or 5x5 neighbourhood. The outline lines are drawn with a polygon offset that pushes them just
// behind the surface, so without the margin they sat on the edge of the test and the jitter phase decided; with it a line on a surface is in front and a line well behind a surface (a hidden part) still is not.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
#ifdef HAS_NOISE
    vec2 screen_res;
#endif
    vec2 depth_texel;
    float depth_radius;
    vec2 depth_jitter;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

#ifdef HAS_NOISE
// the dither: an integer hash of the pixel (PCG) for each channel, 0 to 1 and the same level as the value noise it replaces (which cost 24 sin() a pixel for a dither of 0.003: AUDIT-shaders-2026-11-18.md section 2 item 2)
uint pcg_hash(uint v)
{
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float dither_channel(uvec2 px, uint channel)
{
    return float(pcg_hash(px.x + pcg_hash(px.y + pcg_hash(channel)))) * (1.0 / 4294967295.0);
}
#endif

// there are situations where the colour goes to something insane (infinities, NaNs): a safety measure; the range is the 0 to 11.2 that HDR displays expect
vec3 clamp_hdr_range(vec3 color)
{
    color = mix(color, vec3(1.0), isinf(color));
    color = mix(color, vec3(0.0), isnan(color));
    return clamp(color, vec3(0.0), vec3(11.2));
}

void main()
{
    vec4 diff = texture(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], vary_fragcoord);

#ifdef HAS_NOISE
    const uvec2 px = uvec2(vary_fragcoord * screen_res);
    const vec3 nz = vec3(dither_channel(px, 0u), dither_channel(px, 1u), dither_channel(px, 2u));
    diff.rgb += nz * 0.003;
#endif
    frag_color = vec4(clamp_hdr_range(diff.rgb), diff.a);

    float depth = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], vary_fragcoord, 0.0).r;
    if (depth_texel.x > 0.0)
    {
        if (depth_radius < 0.0)
        {
            depth = 1.0;    // no depth test for overlays
        }
        else if (depth_radius == 0.0)
        {
            // the render pixel (i, j) holds the scene point at (i, j) minus the jitter, so the unjittered position is looked up at uv + jitter. Depth is linear over the screen on a plane, so the bilinear value of the
            // four render pixels around is the same whatever phase the jitter has; only a depth edge inside the four changes it. The margin is then the range of the four (a plane's slope).
            const vec2 size = vec2(textureSize(rk_tbl2d[RK_HM(rk_ti_depthMap)], 0));
            const vec2 f = (vary_fragcoord + depth_jitter) * size - vec2(0.5);
            const vec2 base = clamp(floor(f), vec2(0.0), size - vec2(2.0));
            const vec2 fr = clamp(f - base, vec2(0.0), vec2(1.0));
            const vec2 uv0 = (base + vec2(0.5)) / size;
            const vec2 t = 1.0 / size;
            const float a = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], uv0, 0.0).r;
            const float b = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], uv0 + vec2(t.x, 0.0), 0.0).r;
            const float c = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], uv0 + vec2(0.0, t.y), 0.0).r;
            const float d = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], uv0 + t, 0.0).r;
            const float bil = mix(mix(a, b, fr.x), mix(c, d, fr.x), fr.y);
            const float hi = max(max(a, b), max(c, d));
            const float lo = min(min(a, b), min(c, d));
            // a depth edge (the far one more than 3 % farther than the near one, 1 - depth is about 1 / distance) gets no margin: the margin is for the slope of a surface, and at an occluder's silhouette it
            // would let the hidden outline of the selection show through along the edge
            const bool edge = (1.0 - lo) > 1.03 * max(1.0 - hi, 1.0e-7);
            depth = min(1.0, bil + (edge ? 0.0 : 3.0 * (hi - lo)) + 1.0e-6);
        }
        else
        {
            const vec2 t = depth_texel;
            float lo = depth;
            float hi = depth;
            for (int j = -2; j <= 2; ++j)
            {
                for (int i = -2; i <= 2; ++i)
                {
                    if (abs(float(i)) <= depth_radius && abs(float(j)) <= depth_radius)
                    {
                        const float d = textureLod(rk_tbl2d[RK_HM(rk_ti_depthMap)], vary_fragcoord + vec2(float(i), float(j)) * t, 0.0).r;
                        lo = min(lo, d);
                        hi = max(hi, d);
                    }
                }
            }
            // the margin is for the slope of a surface, as in the bilinear mode: at a depth edge (an avatar in front of a wall) hi - lo is the whole step between the two surfaces, and hi + (hi - lo)
            // lay behind the wall or past the far plane in the band around every silhouette: tags and hover texts behind the wall showed through there (the user 2026-11-21, along the outline of his avatar)
            const bool edge = (1.0 - lo) > 1.03 * max(1.0 - hi, 1.0e-7);
            depth = min(1.0, hi + (edge ? 0.0 : (hi - lo)) + 1.0e-6);
        }
    }
    gl_FragDepth = depth;
}
