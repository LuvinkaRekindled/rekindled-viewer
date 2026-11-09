// atmos.glsl: what the air between the eye and a surface does, from the sky of the pass (the uniforms of alpha_pass.inc). Same model and same constants as the viewer's windlight terms.
//   atten:     how much of the surface's own light survives the way to the eye (per channel; the fog uses the red one)
//   additive:  the haze light added on top (sRGB-ish, as the old terms: the fog function converts it)
//   sunlit / amblit: the light a surface gets from the sun (or moon) and from the ambient sky, after the air above it has taken its share
// light_dir (eye space, unit, or zero) only shapes the glow of the haze around the sun: a zero vector is "no sun glow", which is what unlit surfaces use.

void atmos_terms(vec3 pos_eye, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten)
{
    vec3 rel_pos = pos_eye;
    if (abs(rel_pos.y) > max_y)
    {
        rel_pos *= max_y / rel_pos.y;           // (a surface far above or below the sky's top is treated as at its top)
    }
    const vec3 rel_dir = normalize(rel_pos);
    const float rel_len = length(rel_pos);

    vec3 sunlight = sun_up_factor == 1 ? sunlight_color : moonlight_color;

    // the sun light gets redder and dimmer with the air it crosses at a low angle
    const vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    sunlight *= exp(-light_atten / max(1e-6, lightnorm.y));

    vec3 density = max(blue_density + vec3(haze_density), vec3(1e-6));
    const vec3 blue_weight = blue_density / density;
    const vec3 haze_weight = vec3(haze_density) / density;

    atten = exp(-density * (rel_len * density_multiplier) * distance_multiplier);

    // the glow of the haze around the sun: 0 at the sun, growing away from it
    float haze_glow = dot(rel_dir, lightnorm) * max(0.0, dot(light_dir, rel_dir));
    haze_glow = max(1.0 - haze_glow, 0.001) * glow.x;
    haze_glow = clamp(pow(haze_glow, glow.z), -100000.0, 100000.0);
    haze_glow = (haze_glow + 0.25) * sun_moon_glow_factor;

    // more cloud, more ambient
    const vec3 ambient = ambient_color + (vec3(1.0) - ambient_color) * cloud_shadow * 0.5;
    const vec3 cs = sunlight * (1.0 - cloud_shadow);
    additive = (blue_horizon * blue_weight) * (cs + ambient) + (haze_horizon * haze_weight) * (cs * haze_glow + ambient);
    additive = min(additive * (vec3(1.0) - atten), vec3(10.0));

    sunlit = sunlight;
    amblit = pow(ambient, vec3(0.9)) * 0.57;
}

#ifdef ALPHA_LIT
// ---- the lit version: the same terms with the light a surface of normal `norm` gets from the sun (or moon) and the ambient sky, in linear colour. Needs color.glsl and, from the includer, classic_mode,
// sky_sunlight_scale and sky_ambient_scale. classic mode keeps the colours in sRGB-ish form (the lit shader mixes them in sRGB), the other mode works in linear.

// a little light from the side opposite the sun, so that areas in shadow keep some detail
float ambient_lighting(vec3 norm, vec3 light_dir)
{
    float a = min(abs(dot(norm, light_dir)), 1.0) * 0.5;
    return 1.0 - a * a;
}

void atmos_terms_lit(vec3 pos_eye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten)
{
    atmos_terms(pos_eye, light_dir, sunlit, amblit, additive, atten);
    amblit *= ambient_lighting(norm, light_dir);
    if (classic_mode < 1)
    {
        amblit = srgb_to_linear(amblit);
        amblit = vec3(dot(amblit, vec3(0.2126, 0.7152, 0.0722)));
        sunlit = srgb_to_linear(sunlit);
    }
    sunlit *= sky_sunlight_scale;
    amblit *= sky_ambient_scale;
}
#endif
