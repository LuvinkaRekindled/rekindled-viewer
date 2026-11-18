// texcompare.vert: a textured shape with its own coordinates (the scene monitor's comparison of two pictures). A line for line translation of "Two Texture Compare Shader" (interface/twotexturecompareV.glsl);
// pairs with texcompare.frag. (the draw block is declared the same in both stages) The second coordinate is passed on and was never read by the fragment shader: it is not read here either.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_tex0;
    uint ll_ti_tex1;
    uint ll_ti_dither_tex;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = texcoord0;
}
