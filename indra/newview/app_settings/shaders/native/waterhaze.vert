// waterhaze.vert: the water haze's geometry: a full screen triangle under the water, the water patch's own clip position above it. A line for line translation of "Water Haze Shader"
// (class 3 deferred/waterHazeV.glsl); pairs with waterhaze.frag. (the uniform blocks are declared the same in both stages)

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    vec4 waterPlane;
    int above_water;
    vec4 waterFogColor;
    float waterFogDensity;
    float waterFogKS;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    mat4 modelview_projection_matrix;
    uint ll_ti_exclusionTex;
    uint ll_ti_depthMap;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec4 vary_fragcoord;

void main()
{
    //transform vertex
    vec4 pos = vec4(position.xyz, 1.0);

    if (above_water > 0)
    {
        pos = modelview_projection_matrix * pos;
    }

    gl_Position = pos;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    vary_fragcoord = pos;
}
