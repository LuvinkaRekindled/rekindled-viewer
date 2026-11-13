// shade_lit.glsl: the colour of a lit transparent surface of plain diffuse (no material maps), shared by alpha_lit.frag (the objects' blended faces) and avatar_alpha.frag (the system avatar's blended parts).
// The texture times the vertex colour (sRGB), the ambient sky (or the probes') and the sun with its cascade shadow, every local light that reaches it (the view's light list), then the air or water between it
// and the eye. Throws the pixel away below the draw's alpha cutoff. Needs color.glsl, atmos.glsl, fog.glsl, shadow.glsl, light.glsl (and probe.glsl with ALPHA_PROBES) and the uniforms of alpha_pass.inc.

vec4 shade_lit(vec3 pos, vec3 norm, vec4 texel, vec4 vertex_colour)
{
    const float alpha = texel.a * vertex_colour.a;
    if (alpha < minimum_alpha)
    {
        discard;
    }
    const vec3 diffuse = srgb_to_linear(texel.rgb * vertex_colour.rgb);
    const vec3 light_dir = sun_up_factor == 1 ? sun_dir : moon_dir;

    // the cascade lookup comes after the alpha discard: pixels that are thrown away do not pay for it
    const float shadow = sun_shadow(pos, norm, gl_FragCoord.y / screen_res.y);

    vec3 sunlit, amblit, additive, atten;
    atmos_terms_lit(pos, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
    {
        sunlit *= 1.35;
    }

    const float ndl = clamp(dot(norm, light_dir), 0.0, 1.0);
    vec3 color;
    if (classic_mode > 0)
    {
        // classic mode mixes the sky and the sun in sRGB, then goes back to linear
        const vec3 sun_contrib = vec3(min(pow(ndl, 1.2), shadow));
        color = srgb_to_linear(amblit * 0.9 + linear_to_srgb(sun_contrib) * sunlit * 0.7);
    }
    else
    {
        // the ambient light of the other skies comes from the reflection probes
        vec3 irradiance = amblit;
#ifdef ALPHA_PROBES
        probe_gather(pos);
        irradiance = probe_ambient(pos, norm, amblit);
#endif
        color = irradiance + min(ndl, shadow) * sunlit;
    }
    color *= diffuse;
    color += alpha_lights_sum(diffuse, pos, norm);
    color = apply_sky_and_water_fog(pos, additive, atten, color);

    return max(vec4(color * (classic_mode > 0 ? 1.1 : 1.0), alpha), vec4(0.0));
}
