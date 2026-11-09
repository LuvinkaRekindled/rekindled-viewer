// color.glsl: sRGB <-> linear (the standard piecewise curves; the viewer's textures and vertex colours are sRGB, the lighting is linear).

vec3 srgb_to_linear(vec3 c)
{
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(1.0));
    return mix(1.055 * pow(c, vec3(0.41666)) - 0.055, c * 12.92, lessThan(c, vec3(0.0031308)));
}
