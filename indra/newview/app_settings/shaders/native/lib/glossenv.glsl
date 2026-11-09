// glossenv.glsl: the two environment terms of a legacy material surface (the shiny setting and the specular colour with a glossiness): what the probes' reflection adds to the colour. Shared by the
// transparent material program (material.glsl) and the lighting resolve (resolve.frag). Needs nothing but its arguments.

// the gloss environment on a surface: dimmed, by the fresnel of the view, the specular colour and what the surface's own colour already takes (a cheap energy balance)
void apply_gloss_env(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm)
{
    glossenv *= 0.5;
    float fresnel = clamp(1.0 + dot(normalize(pos), norm), 0.3, 1.0);
    fresnel *= fresnel * spec.a;
    glossenv *= spec.rgb * fresnel;
    glossenv *= vec3(1.0) - color;
    color += glossenv * 0.5;
}

// the legacy environment (the shiny setting of a face): mixed in by its intensity
void apply_legacy_env(inout vec3 color, vec3 legacyenv, vec3 pos, vec3 norm, float intensity)
{
    float fresnel = 1.0 + dot(normalize(pos), norm);
    fresnel = min(fresnel * fresnel + intensity, 1.0);
    color = mix(color, legacyenv * (intensity * fresnel) * 0.5, intensity);
}
