// nr_rim_lift.frag: the neural renderer's rim lift: where the model darkened a pixel next to a depth edge more than its own neighbourhood, the pixel is lifted back toward the neighbourhood's darkening. A line for
// line translation of "DLSS-NR Rim Lift" (deferred/nrRimLiftF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec2 depth_res;             // the depth texture's size in pixels
    vec2 clip_planes;           // near, far
    float rim_width;            // how far from a depth edge the lift reaches, in depth pixels
    float rim_strength;         // 0 none .. 1 the input's luminance at the edge
    float rim_edge;             // the relative step in linear depth that counts as an edge
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;     // the neural renderer's output
    uint rk_ti_emissiveRect;    // its input (the picture before it)
    uint rk_ti_depthMap;        // the scene depth (render size)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

#define DIFFUSE rk_tbl2d[RK_HM(rk_ti_diffuseRect)]
#define EMISSIVE rk_tbl2d[RK_HM(rk_ti_emissiveRect)]
#define DEPTH rk_tbl2d[RK_HM(rk_ti_depthMap)]

float lin(float d)
{
    d = d * 2.0 - 1.0;
    return clip_planes.x * 2.0 * clip_planes.y / (clip_planes.y + clip_planes.x - d * (clip_planes.y - clip_planes.x));
}

void main()
{
    vec2 tc = vary_fragcoord;
    vec4 nr = texture(DIFFUSE, tc);
    frag_color = nr;
    if (rim_strength <= 0.0)
    {
        return;
    }
    const vec3 lum = vec3(0.299, 0.587, 0.114);
    float ln = dot(nr.rgb, lum);
    float ls = dot(texture(EMISSIVE, tc).rgb, lum);
    if (ln >= ls || ls < 1e-4)
    {
        return;         // not darkened: not a rim
    }
    // the edge: a step in linear depth within rim_width pixels, in any of eight directions
    float d0 = lin(texture(DEPTH, tc).r);
    vec2 px = rim_width / depth_res;
    float edge = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float a = float(i) * 0.785398;
        vec2 dir = vec2(cos(a), sin(a));
        float d1 = lin(texture(DEPTH, tc + dir * px).r);
        float d2 = lin(texture(DEPTH, tc + dir * px * 0.5).r);
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
            float dk = lin(texture(DEPTH, t).r);
            if (abs(dk - d0) / max(min(d0, dk), 1e-3) < rim_edge)
            {
                float lnk = dot(texture(DIFFUSE, t).rgb, lum);
                float lsk = dot(texture(EMISSIVE, t).rgb, lum);
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
