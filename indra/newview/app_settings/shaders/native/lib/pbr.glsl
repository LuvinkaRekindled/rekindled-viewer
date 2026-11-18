// pbr.glsl: the lighting of a glTF (metallic roughness) surface for the blended pass: the sun, the image based light from the probes, and every local light that reaches it.
// Same model and the same constants as the viewer's glTF lighting (GGX distribution, Smith visibility, Schlick fresnel, the split sum lookup for the probes, classic mode's own mixing in sRGB), so the picture
// is the same. What is different is the cost: everything that depends on the view alone (the view term of the visibility, the roughness terms, the fresnel colours) is made once for the surface and not
// again for every light, and a light that cannot reach the surface is skipped before any of it.
// Needs color.glsl, ALPHA_LIT's uniforms (classic_mode, the light slots and list) and rk_ti_brdfLut (alpha_pass.inc, ALPHA_PBR).

#include "lib/pbr_core.glsl"

// the split sum lookup: x scales the specular colour, y is the bias. (The second coordinate is the glossiness, not the roughness: the viewer's lookup is addressed that way. Level 0: one level.)
vec2 pbr_brdf(float nv, float gloss)
{
    return textureLod(rk_tbl2d[RK_HM(rk_ti_brdfLut)], clamp(vec2(nv, gloss), vec2(0.004), vec2(0.996)), 0.0).rg;
}

// the light of the sky and the sun on the surface, plus its glow: irradiance / radiance are what the probes give (ambient, reflection), sunlit and scol the sun and its shadow
vec3 pbr_base_light(PbrSurface s, float roughness, vec3 light_dir, vec3 sunlit, float scol, vec3 radiance, vec3 irradiance, vec3 emissive, float ao)
{
    const vec2 brdf = pbr_brdf(s.nv, 1.0 - roughness);
    float nl;
    const vec3 punctual = pbr_punctual(s, normalize(light_dir), nl);
    vec3 color;
    if (classic_mode > 0)
    {
        // classic mode mixes the sky and the sun in sRGB (as the old surfaces do) and ignores the occlusion map for the diffuse light
        const vec3 ambient = srgb_to_linear(irradiance * 0.9);
        vec3 sun = vec3(min(pow(nl, 1.2), scol));
        // times pi: the diffuse colour carries 1/pi, which a legacy sky's sun does not expect
        sun = srgb_to_linear(linear_to_srgb(sun) * sunlit * 0.7) * PBR_PI;
        const vec3 final_sun = clamp(sun * (punctual * scol), vec3(0.0), vec3(10.0));
        color = srgb_to_linear(linear_to_srgb(ambient * s.diffuse) + linear_to_srgb(final_sun) * 1.1);
    }
    else
    {
        color = irradiance * s.diffuse * ao + clamp(nl * punctual, vec3(0.0), vec3(10.0)) * sunlit * 3.0 * scol;
    }
    return color + radiance * (s.f0 * brdf.x + brdf.y) * ao + emissive;
}

// one local light: its distance attenuation is the opaque pass's (2 * (1 - clamp((d / size + falloff) / (1 + falloff), 0, 1))^2, size = the light's radius * 1.5), a spot light is dimmed by the squared
// cosine of the angle to its axis, and the surface takes it through the lighting terms above
vec3 pbr_local_light(PbrSurface s, vec3 pos, vec3 light_col, vec3 center, float size, float falloff, vec3 axis, float point)
{
    const vec3 lv = center - pos;
    const float d2 = dot(lv, lv);
    if (d2 <= 0.0 || d2 >= size * size)
    {
        return vec3(0.0);
    }
    const float length_lv = sqrt(d2);
    const float dist = length_lv / size;
    float atten = clamp(1.0 - (dist + falloff) / (1.0 + falloff), 0.0, 1.0);
    atten = 2.0 * atten * atten;
    if (atten <= 0.0)
    {
        return vec3(0.0);
    }
    const vec3 l = lv / length_lv;
    const float spot = max(dot(-axis, l), point);
    float nl;
    const vec3 punctual = pbr_punctual(s, l, nl);
    // (3.0: the balance with the legacy materials; 0.9: classic mode's)
    return light_col * (spot * spot * atten * 3.0 * (classic_mode > 0 ? 0.9 : 1.0)) * clamp(nl * punctual, vec3(0.0), vec3(10.0));
}

#ifdef ALPHA_PBR
// a hardware light slot (the avatar backlight; the nearest lights when there is no list): the slot carries its attenuation as (linear, quadratic) with the size and falloff folded in
vec3 pbr_slot_light(int i, PbrSurface s, vec3 pos)
{
    const float la = light_attenuation[i].x;
    const float fa = light_attenuation[i].y;
    const float size = (12.0 * fa - 9.0) / la;
    if (!(la > 0.0 && size > 0.0))
    {
        return vec3(0.0);               // an unused slot
    }
    return pbr_local_light(s, pos, light_diffuse[i], light_position[i].xyz, size, fa - 1.0, light_direction[i], light_attenuation[i].z);
}

// every light that reaches the surface: the avatar backlight, then the light list of the view (or the slots 2-7 without one)
vec3 pbr_lights_sum(PbrSurface s, vec3 pos)
{
    vec3 sum = pbr_slot_light(1, s, pos);
    if (alpha_light_count >= 0)
    {
        for (int i = 0; i < alpha_light_count; ++i)
        {
            const vec4 l0 = alpha_lights[i * 3];
            const vec3 lvec = l0.xyz - pos;
            if (dot(lvec, lvec) >= l0.w * l0.w)
            {
                continue;
            }
            const vec4 l1 = alpha_lights[i * 3 + 1];
            const vec4 l2 = alpha_lights[i * 3 + 2];
            sum += pbr_local_light(s, pos, l1.rgb, l0.xyz, l0.w, l1.w, l2.xyz, l2.w);
        }
    }
    else
    {
        for (int i = 2; i < 8; ++i)
        {
            sum += pbr_slot_light(i, s, pos);
        }
    }
    return sum;
}
#endif
