// waterhaze.vert: the water haze's geometry: a full screen triangle under the water, the water patch's own clip position above it. A line for line translation of "Water Haze Shader"
// (class 3 deferred/waterHazeV.glsl); pairs with waterhaze.frag. (the uniform blocks are declared the same in both stages)

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
#ifdef WATER_PULL
    uvec2 ll_packet_table;      // the table form of the water patches (WATER_PULL, "Water Haze Pull Shader"): three buffer addresses written by the draw record
    uvec2 ll_packet_models;
    uvec2 ll_packet_texmats;
#endif
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

#ifdef WATER_PULL
#include "lib/rows.glsl"
#else
layout(location = 0) in vec3 position;
#endif
layout(location = 0) out vec4 vary_fragcoord;

void main()
{
#ifdef WATER_PULL
    const Row row = RowTable(ll_packet_table).r[gl_InstanceIndex];
    const uint at = row.first + uint(gl_VertexIndex);
    const uint word = RowWords(row.indices).w[at >> 1];
    const uint index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    const vec3 position = RowVec4(row.positions).v[index].xyz;
#endif
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
