// reflectionmip.frag: a texture's colour with alpha 0 (a mip step of the probe blur). A line for line translation of "Reflection Mip Shader" (interface/reflectionmipF.glsl); pairs with reflectionmip.vert.

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec3 col = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_texcoord0.xy).rgb;
    frag_color = vec4(col, 0.0);
}
