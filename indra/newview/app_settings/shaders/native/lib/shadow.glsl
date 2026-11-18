// shadow.glsl: the sun (or moon) shadow of a surface from the four cascades. Used by the sun lightmap pass and by the lit transparent surfaces, so both see the same shadow.
// The includer declares: shadow_matrix[6], shadow_clip, shadow_res, shadow_bias, shadow_offset, sun_dir, moon_dir, sun_up_factor, and the shadow samplers rk_ti_shadow_shadowMap0 .. 3 (sampler2DShadow slots).
// jitter is a number that changes from row to row of the screen (the sample column is snapped to the shadow texels with a jitter that follows it, so the snapping does not show as lines).

#define SHADOW_MAP(slot) rk_tbl2dshadow[LL_HM(slot)]

#ifdef SUN_SHADOW
// one cascade: five taps with the weights 4 + 1 + 1 + 1 + 1 over 8
float pcf_sun(uint slot, vec4 stc, float jitter)
{
    stc.xyz /= stc.w;
    stc.z += shadow_bias * 2.0;
    stc.x = floor(stc.x * shadow_res.x + fract(jitter * shadow_res.y)) / shadow_res.x;
    const vec2 texel = 1.0 / shadow_res;
    float shadow = texture(SHADOW_MAP(slot), stc.xyz) * 4.0;
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3( 1.5 * texel.x,  0.5 * texel.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3( 0.5 * texel.x, -1.5 * texel.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(-1.5 * texel.x, -0.5 * texel.y, 0.0));
    shadow += texture(SHADOW_MAP(slot), stc.xyz + vec3(-0.5 * texel.x,  1.5 * texel.y, 0.0));
    return clamp(shadow * 0.125, 0.0, 1.0);
}

// the cascades overlap by a quarter of their range at the ends: a pixel in an overlap takes both and the two are blended by their weights
float sun_shadow(vec3 pos, vec3 norm, float jitter)
{
    const vec3 light_dir = normalize(sun_up_factor == 1 ? sun_dir : moon_dir);
    const float ndl = clamp(dot(norm, light_dir), 0.0, 1.0);
    const vec4 spos = vec4(pos + light_dir * (1.0 - ndl) * shadow_offset * 2.0, 1.0);
    if (!(spos.z > -shadow_clip.w))
    {
        return 1.0;         // lit beyond the far split
    }
    const vec4 near_split = shadow_clip * -0.75;
    const vec4 far_split = shadow_clip * -1.25;
    const vec4 transition = near_split - far_split;
    float shadow = 0.0;
    float weight = 0.0;
    if (spos.z < near_split.z)
    {
        const float w = 1.0 - max(spos.z - far_split.z, 0.0) / transition.z;
        shadow += pcf_sun(rk_ti_shadow_shadowMap3, shadow_matrix[3] * spos, jitter) * w;
        weight += w;
        shadow += max((pos.z + shadow_clip.z) / (shadow_clip.z - shadow_clip.w) * 2.0 - 1.0, 0.0);      // the farthest cascade fades out towards the far clip
    }
    if (spos.z < near_split.y && spos.z > far_split.z)
    {
        const float w = 1.0 - max(spos.z - far_split.y, 0.0) / transition.y - max(near_split.z - spos.z, 0.0) / transition.z;
        shadow += pcf_sun(rk_ti_shadow_shadowMap2, shadow_matrix[2] * spos, jitter) * w;
        weight += w;
    }
    if (spos.z < near_split.x && spos.z > far_split.y)
    {
        const float w = 1.0 - max(spos.z - far_split.x, 0.0) / transition.x - max(near_split.y - spos.z, 0.0) / transition.y;
        shadow += pcf_sun(rk_ti_shadow_shadowMap1, shadow_matrix[1] * spos, jitter) * w;
        weight += w;
    }
    if (spos.z > far_split.x)
    {
        const float w = 1.0 - max(near_split.x - spos.z, 0.0) / transition.x;
        shadow += pcf_sun(rk_ti_shadow_shadowMap0, shadow_matrix[0] * spos, jitter) * w;
        weight += w;
    }
    return shadow / weight;
}
#else
// the session has no sun shadows (the viewer's global define SUN_SHADOW is off: the cascades are not rendered): nothing shades the sun. The viewer's programs take the same branch (no HAS_SUN_SHADOW)
float sun_shadow(vec3 pos, vec3 norm, float jitter)
{
    return 1.0;
}
#endif
