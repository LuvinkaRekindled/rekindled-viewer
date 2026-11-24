// radiance.vert: the view direction of a probe filter pass (the radiance and the irradiance filters): the position through the model view matrix. A line for line translation of
// "Radiance Gen Shader", "Hero Radiance Gen Shader" and "Irradiance Gen Shader" (interface/radianceGenV.glsl, irradianceGenV.glsl); pairs with radiance.frag and irradiance.frag.
// (the draw block is declared the same in all three)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_matrix;
    uint rk_ti_cubearray_reflectionProbes;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec3 vary_dir;

void main()
{
    // the quad's z is -1 (it is the forward part of the direction): inside OpenGL's clip volume (-w to w) but outside Vulkan's (0 to w), where the whole quad was clipped away and every copy of the pass target
    // took what the target held before: the picture of the last captured face, in all six faces of every probe map (the cube dump of 2026-11-24)
    gl_Position = vec4(position.xy, 0.0, 1.0);
    vary_dir = vec3(modelview_matrix * vec4(position, 1.0)).xyz;
}
