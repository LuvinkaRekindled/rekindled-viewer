// reflectionmip.vert: a rectangle splatted by a matrix, the texture coordinate following the position (the mip steps of the reflection probe blur). A line for line translation of "Reflection Mip Shader"
// (interface/splattexturerectV.glsl); pairs with reflectionmip.frag and gaussian.frag. (the draw block is declared the same in all three)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_diffuseRect;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = position.xy * 0.5 + 0.5;
}
