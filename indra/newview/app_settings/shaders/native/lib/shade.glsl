// shade.glsl: the colour of a fullbright transparent surface (shared by alpha_fullbright.frag and the fullbright branch of the uber program alpha_lit.frag).
// The texture times the vertex colour, no lighting, then the air (or water) between it and the eye. Throws the pixel away below the draw's alpha cutoff. Needs color.glsl, atmos.glsl and fog.glsl
// and the uniforms of alpha_pass.inc.

vec4 shade_fullbright(vec3 pos_eye, vec4 texel, vec4 vertex_colour)
{
    const float alpha = texel.a * vertex_colour.a;
    if (alpha < minimum_alpha)
    {
        discard;
    }
#ifdef IS_HUD
    return max(vec4(texel.rgb * vertex_colour.rgb, alpha), vec4(0.0));      // (a HUD is not lit by the world: no atmosphere, no linear conversion)
#else
    vec3 sunlit, amblit, additive, atten;
    atmos_terms(pos_eye, vec3(0.0), sunlit, amblit, additive, atten);       // (no sun glow: the surface does not take part in the sun's light)
    const vec3 color = apply_sky_and_water_fog(pos_eye, additive, atten, srgb_to_linear(texel.rgb * vertex_colour.rgb));
    return max(vec4(color, alpha), vec4(0.0));
#endif
}
