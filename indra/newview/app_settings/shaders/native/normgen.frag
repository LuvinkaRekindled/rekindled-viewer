// normgen.frag: a normal map from a picture: the height is the picture's luminance (or its inverse, bump_code 2), the normal is made from the four neighbours' differences, the height goes in alpha. A line for
// line translation of "Normal Map Generation Program" (deferred/normgenF.glsl); pairs with normgen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float stepX;
    float stepY;
    float norm_scale;
    int bump_code;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_srcMap;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

#define BE_BRIGHTNESS 1
#define BE_DARKNESS 2

// get luminance or inverse luminance depending on bump_code
float getBumpValue(vec2 texcoord)
{
    const vec3 c = texture(rk_tbl2d[LL_HM(rk_ti_srcMap)], texcoord).rgb;
    const vec3 WEIGHT = vec3(0.2995, 0.5875, 0.1145);
    float l = dot(c, WEIGHT);
    if (bump_code == BE_DARKNESS)
    {
        l = 1.0 - l;
    }
    return l;
}

void main()
{
    const float c = getBumpValue(vary_texcoord0);
    const float scaler = 512.0;

    const vec3 right = vec3(norm_scale, 0.0, (getBumpValue(vary_texcoord0 + vec2(stepX, 0.0)) - c) * scaler);
    const vec3 left = vec3(-norm_scale, 0.0, (getBumpValue(vary_texcoord0 - vec2(stepX, 0.0)) - c) * scaler);
    const vec3 up = vec3(0.0, -norm_scale, (getBumpValue(vary_texcoord0 - vec2(0.0, stepY)) - c) * scaler);
    const vec3 down = vec3(0.0, norm_scale, (getBumpValue(vary_texcoord0 + vec2(0.0, stepY)) - c) * scaler);

    vec3 norm = cross(right, down) + cross(down, left) + cross(left, up) + cross(up, right);
    norm = normalize(norm);
    norm *= 0.5;
    norm += 0.5;
    frag_color = vec4(norm, c);
}
