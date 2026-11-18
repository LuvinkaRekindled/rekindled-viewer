// texcompare.frag: the absolute difference of two pictures, a channel that is under the dither picture's value (times dither_scale) set to zero (the scene monitor). A line for line translation of "Two Texture
// Compare Shader" (interface/twotexturecompareF.glsl); pairs with texcompare.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float dither_scale;
    float dither_scale_s;
    float dither_scale_t;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_tex0;
    uint rk_ti_tex1;
    uint rk_ti_dither_tex;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = abs(texture(rk_tbl2d[RK_HM(rk_ti_tex0)], vary_texcoord0.xy) - texture(rk_tbl2d[RK_HM(rk_ti_tex1)], vary_texcoord0.xy));

    const vec2 dither_coord = vec2(vary_texcoord0[0] * dither_scale_s, vary_texcoord0[1] * dither_scale_t);
    const vec4 dither_vec = texture(rk_tbl2d[RK_HM(rk_ti_dither_tex)], dither_coord.xy);

    for (int i = 0; i < 3; i++)
    {
        if (frag_color[i] < dither_vec[i] * dither_scale)
        {
            frag_color[i] = 0.0;
        }
    }
}
