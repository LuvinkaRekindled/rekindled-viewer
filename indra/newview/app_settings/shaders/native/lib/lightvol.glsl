// lightvol.glsl: what the deferred local light passes (light_point.frag, light_multi.frag, light_spot.frag) share: the surface of a pixel read from the G-buffer, and what one light gives it.
// Needs light_pass.inc (the uniform blocks), color.glsl (srgb_to_linear), flags.glsl, gbuffer.glsl, viewpos.glsl and pbr_core.glsl.
// The model is the viewer's: a glTF surface takes the light through the GGX terms (pbr_punctual) times the legacy distance attenuation and a factor 3.25 that balances it with the legacy materials, a legacy
// surface takes its diffuse by the angle of its normal and its highlight by the lookup of the gloss (lightFunc). What is different from the viewer's programs is the order: the position comes from the depth
// first, the pixels the light does not reach leave before anything else is read, the normal comes next (the facing test needs nothing more), and the albedo and the material only for the pixels that are lit.

#define LV_TAP(slot) rk_tbl2d[RK_HM(slot)]

// the legacy distance attenuation of the viewer's lights: 2 * (1 - clamp((d + falloff) / (1 + falloff), 0, 1))^2, d = the distance over the light's size
float light_attenuation(float dist, float falloff)
{
    float atten = 1.0 - clamp((dist + falloff) / (1.0 + falloff), 0.0, 1.0);
    return 2.0 * atten * atten;
}

struct LightSurface
{
    vec3 n;             // the normal (eye space)
    vec3 v;             // to the eye
    float nv;           // the angle of the view to the normal, kept off zero
    float env;          // the environment intensity (a legacy surface's shiny setting)
    bool pbr;           // a glTF surface
    vec3 diffuse;       // legacy: the diffuse colour (linear)
    vec4 spec;          // legacy: the specular colour (linear) and the gloss
    PbrSurface p;       // glTF: the terms of the view
};

// the geometry of the surface: from the normal buffer's sample and the position (nothing else is read yet)
LightSurface light_surface_geometry(vec4 normal_info, vec3 pos)
{
    LightSurface s;
    s.n = rk_decode_normal(normal_info.xy);
    s.v = -normalize(pos);
    s.nv = clamp(dot(s.n, s.v), 1e-6, 1.0);
    s.env = normal_info.b;
    s.pbr = GET_GBUFFER_FLAG(normal_info.w, GBUFFER_FLAG_HAS_PBR);
    s.diffuse = vec3(0.0);
    s.spec = vec4(0.0);
    return s;
}

// the material of the surface: the albedo and the specular buffer's sample, for the pixels a light reaches
void light_surface_material(vec2 uv, inout LightSurface s)
{
    const vec4 albedo = textureLod(LV_TAP(rk_ti_diffuseRect), uv, 0.0);
    const vec4 spec = textureLod(LV_TAP(rk_ti_specularRect), uv, 0.0);
    if (s.pbr)
    {
        // the specular buffer holds occlusion (r), roughness (g), metallic (b)
        const float metallic = spec.b;
        const vec3 f0 = vec3(0.04);
        const vec3 diffuse_color = albedo.rgb * (vec3(1.0) - f0) * (1.0 - metallic);
        const vec3 specular_color = mix(f0, albedo.rgb, metallic);
        s.p = pbr_surface(s.n, s.v, diffuse_color, specular_color, spec.g);
    }
    else
    {
        s.diffuse = srgb_to_linear(albedo.rgb);
        s.spec = vec4(srgb_to_linear(spec.rgb), spec.a);
    }
}

// a glTF surface lit by one light: lv the unnormalized surface to light vector, dist its length over the light's size
vec3 light_pbr(LightSurface s, vec3 lv, float dist, vec3 light_color, float falloff)
{
    float nl;
    const vec3 punctual = pbr_punctual(s.p, normalize(lv), nl);
    return (light_attenuation(dist, falloff) * 3.25) * light_color * clamp(nl * punctual, vec3(0.0), vec3(10.0));
}

// a legacy surface lit by one light (the caller has tested that the light is on the surface's side): the diffuse, and the highlight of the gloss
vec3 light_legacy(LightSurface s, vec3 lv, float dist, vec3 light_color, float falloff)
{
    const float atten = light_attenuation(dist, falloff);
    const vec3 l = normalize(lv);
    const float nl = clamp(dot(s.n, l), 1e-6, 1.0);
    vec3 col = light_color * (nl * atten) * s.diffuse;
    if (s.spec.a > 0.0)
    {
        const vec3 h = normalize(l + s.v);
        const float nh = clamp(dot(s.n, h), 1e-6, 1.0);
        const float vh = clamp(dot(s.v, h), 1e-6, 1.0);
        const float x = 1.0 - vh;
        const float fres = x * x * x * x * x * 0.4 + 0.5;
        const float gtdenom = 2.0 * nh;
        const float gt = max(0.0, min(gtdenom * s.nv / vh, gtdenom * nl / vh));
        const float scol = fres * textureLod(LV_TAP(rk_ti_lightFunc), vec2(nh, s.spec.a), 0.0).r * gt / (nh * nl);
        col += (min(nl * 6.0, 1.0) * atten * scol) * light_color * s.spec.rgb;
    }
    return col;
}
