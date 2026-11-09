// light.glsl: local lights on a lit transparent surface (hair, glass, leaf cards). Needs the lit uniforms of alpha_pass.inc (ALPHA_LIT).
// One light: its distance attenuation is the opaque pass's (2 * (1 - clamp((d / size + falloff) / (1 + falloff), 0, 1))^2, size = the light's radius * 1.5), a spot light is dimmed by the squared cosine of the
// angle to its axis, and the surface takes the light by the angle of its normal. The result is the light's colour times the surface's colour: no specular here.

vec3 alpha_local_light(vec3 light_col, vec3 diffuse, vec3 pos, vec3 n, vec3 center, float size, float falloff, vec3 axis, float point)
{
    const vec3 lv = center - pos;
    const float d2 = dot(lv, lv);
    if (d2 <= 0.0 || d2 >= size * size)
    {
        return vec3(0.0);               // on the light, or beyond its size: no light
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
    const float da = max(0.0, spot * spot * dot(n, l));
    if (da <= 0.0)
    {
        return vec3(0.0);
    }
    return clamp(da * atten, 0.0, 1.0) * light_col * diffuse * (classic_mode > 0 ? 0.9 : 1.0);
}

// a hardware light slot (the avatar backlight; the nearest lights when there is no list): the slot carries its attenuation as (linear, quadratic) with the size and falloff folded in
vec3 alpha_slot_light(int i, vec3 diffuse, vec3 pos, vec3 n)
{
    const float la = light_attenuation[i].x;
    const float fa = light_attenuation[i].y;
    const float size = (12.0 * fa - 9.0) / la;
    if (!(la > 0.0 && size > 0.0))
    {
        return vec3(0.0);               // an unused slot
    }
    return alpha_local_light(light_diffuse[i], diffuse, pos, n, light_position[i].xyz, size, fa - 1.0, light_direction[i], light_attenuation[i].z);
}

// every light that reaches the surface: the avatar backlight, then the light list of the view (or the slots 2-7 without one)
vec3 alpha_lights_sum(vec3 diffuse, vec3 pos, vec3 n)
{
    vec3 sum = alpha_slot_light(1, diffuse, pos, n);
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
            sum += alpha_local_light(l1.rgb, diffuse, pos, n, l0.xyz, l0.w, l1.w, l2.xyz, l2.w);
        }
    }
    else
    {
        for (int i = 2; i < 8; ++i)
        {
            sum += alpha_slot_light(i, diffuse, pos, n);
        }
    }
    return sum;
}
