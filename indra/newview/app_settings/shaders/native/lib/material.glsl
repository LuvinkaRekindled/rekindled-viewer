// material.glsl: a transparent surface with a legacy material (specular colour and glossiness, an optional specular map, an optional normal map, an environment intensity, a glow): the blend path of the
// viewer's material shader. Needs the lit libraries (color, clip, atmos, fog, shadow, light, probe, ssr) and the material uniforms and varyings of alpha_pass.inc / alpha_lit.frag.
// Same terms as the viewer's: the sun's specular lobe from the lightFunc table, the gloss and the legacy environment from the probes (and the screen), the lights' specular, the glow.

// the sun's specular lobe table: x = the half angle, y = glossiness. (Read at level 0 explicitly: it is read inside branches and loops that differ from pixel to pixel, where an implicit level is undefined; on this GPU
// the same mistake in the screen march hung the shader. The table has one level.)
#define LIGHT_FUNC ll_tbl2d[LL_HM(ll_ti_lightFunc)]

#include "lib/glossenv.glsl"

// one local light on a material surface: the diffuse of alpha_local_light plus the light's specular (from the lightFunc table). glare collects the brightest specular, which makes the surface more opaque
vec3 alpha_mat_light(vec3 light_col, vec3 diffuse, vec4 spec, vec3 pos, vec3 n, vec3 view_dir, vec3 center, float size, float falloff, vec3 axis, float point, inout float glare)
{
    const vec3 lv = center - pos;
    const float d2 = dot(lv, lv);
    if (d2 <= 0.0 || d2 >= size * size)
    {
        return vec3(0.0);
    }
    const float dist = sqrt(d2) / size;
    float atten = clamp(1.0 - (dist + falloff) / (1.0 + falloff), 0.0, 1.0);
    atten = 2.0 * atten * atten;
    if (atten <= 0.0)
    {
        return vec3(0.0);
    }
    const vec3 l = lv / (dist * size);
    const float spot = max(dot(-axis, l), point);
    const float da = spot * spot * dot(n, l);        // (not clamped: the specular term below uses it as it is, as the old one does)
    vec3 col = vec3(0.0);
    float lit = 0.0;
    if (da >= 0.0)
    {
        lit = clamp(da * atten, 0.0, 1.0);
        col = lit * light_col * diffuse;
    }
    if (spec.a > 0.0)
    {
        const vec3 h = normalize(l + view_dir);
        const float nh = dot(n, h);
        const float nv = dot(n, view_dir);
        const float vh = dot(view_dir, h);
        const float fres = pow(1.0 - dot(h, view_dir), 5.0) * 0.4 + 0.5;
        const float gtdenom = 2.0 * nh;
        const float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * da / vh));
        if (nh > 0.0)
        {
            const float scol = fres * textureLod(LIGHT_FUNC, vec2(nh, spec.a), 0.0).r * gt / (nh * da);
            const vec3 speccol = clamp(lit * scol * light_col * spec.rgb, vec3(0.0), vec3(1.0));
            col += speccol;
            glare = max(glare, speccol.r) + max(max(speccol.r, speccol.g), max(speccol.b, 0.0));
        }
    }
    return max(col * (classic_mode > 0 ? 0.9 : 1.0), vec3(0.0));
}

// the hardware slot form of a light (the avatar backlight; the nearest lights without a list): see alpha_slot_light
vec3 alpha_mat_slot_light(int i, vec3 diffuse, vec4 spec, vec3 pos, vec3 n, vec3 view_dir, inout float glare)
{
    const float la = light_attenuation[i].x;
    const float fa = light_attenuation[i].y;
    const float size = (12.0 * fa - 9.0) / la;
    if (!(la > 0.0 && size > 0.0))
    {
        return vec3(0.0);
    }
    return alpha_mat_light(light_diffuse[i], diffuse, spec, pos, n, view_dir, light_position[i].xyz, size, fa - 1.0, light_direction[i], light_attenuation[i].z, glare);
}

vec3 alpha_mat_lights_sum(vec3 diffuse, vec4 spec, vec3 pos, vec3 n, inout float glare)
{
    const vec3 view_dir = normalize(-pos);
    vec3 sum = alpha_mat_slot_light(1, diffuse, spec, pos, n, view_dir, glare);
    if (alpha_light_count >= 0)
    {
        for (int i = 0; i < alpha_light_count; ++i)
        {
            const vec4 l0 = alpha_lights[i * 3];
            const vec3 lvec = l0.xyz - pos.xyz;
            if (dot(lvec, lvec) >= l0.w * l0.w)
            {
                continue;
            }
            const vec4 l1 = alpha_lights[i * 3 + 1];
            const vec4 l2 = alpha_lights[i * 3 + 2];
            sum += alpha_mat_light(l1.rgb, diffuse, spec, pos, n, view_dir, l0.xyz, l0.w, l1.w, l2.xyz, l2.w, glare);
        }
    }
    else
    {
        for (int i = 2; i < 8; ++i)
        {
            sum += alpha_mat_slot_light(i, diffuse, spec, pos, n, view_dir, glare);
        }
    }
    return sum;
}

// the colour and alpha of a material surface. texel: the diffuse texture's tap, vcolor: the vertex colour; the rest comes from the vertex shader's flat outputs (material flags, slots, specular, parameters)
vec4 shade_material(vec3 pos, vec4 texel, vec4 vcolor, vec3 vnorm, vec3 vtangent, float vsign, vec2 tc0, vec2 tc1, vec2 tc2,
                    uint material, uint slot_normal, uint slot_specular, vec4 vspec, vec2 matparams)
{
    vec4 diffcol = vec4(texel.rgb * vcolor.rgb, texel.a);

    vec4 spec;
    if ((material & 4u) != 0u)
    {
        spec = texture(ll_tbl2d[nonuniformEXT(LL_HM(slot_specular))], tc2);
        spec.rgb *= vspec.rgb;
    }
    else
    {
        spec = vec4(vspec.rgb, 1.0);
    }
    const float env = matparams.x * spec.a;
    float glossiness = vspec.a;
    vec3 norm;
    if ((material & 2u) != 0u)
    {
        vec4 map = texture(ll_tbl2d[nonuniformEXT(LL_HM(slot_normal))], tc1);
        glossiness *= map.a;
        map.xyz = map.xyz * 2.0 - 1.0;
        const vec3 bitangent = vsign * cross(vnorm, vtangent);
        norm = normalize(map.x * vtangent + map.y * bitangent + map.z * vnorm);
    }
    else
    {
        norm = normalize(vnorm);
    }
    const float emissive = matparams.y;

    diffcol.rgb = srgb_to_linear(diffcol.rgb);
    spec.rgb = srgb_to_linear(spec.rgb);
    spec.a = glossiness;                        // (the lighting functions take the glossiness in the specular's alpha)

    const float shadow = sun_shadow(pos, norm, tc0.y);
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;
    vec3 sunlit, amblit, additive, atten;
    atmos_terms_lit(pos, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }

    // the light the probes give: the ambient (other skies), the gloss environment (a mip of the radiance maps by glossiness, screen space reflections over it, the hero probe for the glossiest) and the legacy
    // environment (the sharpest mip). The probe list is only made when one of them is wanted.
    vec3 ambenv = amblit;
    vec3 glossenv = vec3(0.0);
    vec3 legacyenv = vec3(0.0);
    const bool want_gloss = glossiness > 0.0;
    const bool want_legacy = env > 0.0;
#ifdef MAT_NO_PROBES
    if (false)
#else
    if (classic_mode == 0 || want_gloss || want_legacy)
#endif
    {
        probe_gather(pos);
        if (classic_mode == 0)
        {
            ambenv = probe_ambient(pos, norm, amblit);
        }
        const vec3 reflect_dir = normalize(reflect(pos, norm));
        if (want_gloss)
        {
            glossenv = probe_reflection(pos, reflect_dir, (1.0 - glossiness) * max_probe_lod);
        }
        if (want_legacy)
        {
            legacyenv = probe_reflection(pos, reflect_dir, 0.0);
        }
#if defined(SSR) && !defined(MAT_NO_SSR)          // (MAT_NO_SSR / MAT_NO_PROBES: switches of the manifest line to bisect a GPU hang)
        if (cube_snapshot != 1)
        {
            const vec4 ssr = ssr_transparent(pos.xy * 0.5 + 0.5, pos, norm, glossiness);
            glossenv = mix(glossenv, ssr.rgb, ssr.a);
            legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
        }
#endif
#ifdef HERO_PROBES
        glossenv = probe_hero(glossenv, pos, norm, glossiness);
        if (want_legacy)
        {
            legacyenv = probe_hero(legacyenv, pos, norm, 1.0);
        }
#endif
        glossenv = clamp(glossenv, vec3(0.0), vec3(10.0));
    }

    float ndl = clamp(dot(norm, light_dir), 0.0, 1.0);
    vec3 color = ambenv;
    if (classic_mode > 0)
    {
        ndl = pow(ndl, 1.2);
        color = srgb_to_linear(color * 0.9 + linear_to_srgb(vec3(min(ndl, shadow))) * sunlit * 0.7);
        sunlit = srgb_to_linear(sunlit);
    }
    else
    {
        color += min(ndl, shadow) * sunlit;
    }
    color *= diffcol.rgb;

    float glare = 0.0;
    if (glossiness > 0.0)
    {
        // the sun's specular lobe
        const vec3 v = -normalize(pos);
        const vec3 l = normalize(light_dir);
        const vec3 h = normalize(l + v);
        const float eps = 0.000001;
        const float nh = clamp(dot(norm, h), eps, 1.0);
        const float nl = clamp(dot(norm, l), eps, 1.0);
        const float nv = clamp(dot(norm, v), eps, 1.0);
        const float vh = clamp(dot(v, h), eps, 1.0);
        if (nl > 0.0 && nh > 0.0)
        {
            const float lit = min(nl * 6.0, 1.0);
            const float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
            const float gtdenom = 2.0 * nh;
            const float gt = max(0.0, min(gtdenom * nv / vh, gtdenom * nl / vh));
            const float scol = shadow * fres * textureLod(LIGHT_FUNC, vec2(nh, glossiness), 0.0).r * gt / (nh * nl);
            color += lit * scol * sunlit * spec.rgb;
        }
        apply_gloss_env(color, glossenv, spec, pos, norm);
    }

    color = mix(color, diffcol.rgb, emissive);

    if (env > 0.0)
    {
        apply_legacy_env(color, legacyenv, pos, norm, env);
        glare += clamp(max(max(legacyenv.r, legacyenv.g), legacyenv.b), 0.0, 1.0) * env;
    }

    color += alpha_mat_lights_sum(diffcol.rgb, spec, pos, norm, glare);
    color = apply_sky_and_water_fog(pos, additive, atten, color);

    glare = min(glare * (1.0 - emissive), 1.0);
    const float alpha = max(diffcol.a, glare) * vcolor.a;
    return max(vec4(color * (classic_mode > 0 ? 1.1 : 1.0), alpha), vec4(0.0));
}
