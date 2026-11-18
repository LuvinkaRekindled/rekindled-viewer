// gaussian.frag: one direction of a nine tap gaussian blur (the probe blur). A line for line translation of the program the viewer also names "Reflection Mip Shader" (interface/gaussianF.glsl, with
// splattexturerectV); pairs with reflectionmip.vert. The manifest picks it by the uniform resScale, which only this program has.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float resScale;
    vec2 direction;         // texture direction, <1, 0> or <0, 1>
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec3 col = vec3(0, 0, 0);

    float w[9] = float[9](0.0002, 0.0060, 0.0606, 0.2417, 0.3829, 0.2417, 0.0606, 0.0060, 0.0002);

    for (int i = 0; i < 9; ++i)
    {
        vec2 tc = vary_texcoord0 + (i - 4) * direction * resScale;
        col += texture(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], tc).rgb * w[i];
    }

    frag_color = max(vec4(col, 0.0), vec4(0));
}
