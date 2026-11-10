// solidcolor.vert: a flat colour with the alpha of a texture (the mask shapes of the UI and the selection outlines). A line for line translation of "Solid Color Shader" (interface/solidcolorV.glsl);
// pairs with solidcolor.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_tex0;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = texcoord0;
}
