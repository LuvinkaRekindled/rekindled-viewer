// smaa_blend.vert: the vertex stage of SMAA's neighbourhood blending. A line for line translation of "SMAA Neighborhood Blending" (deferred/SMAANeighborhoodBlendV.glsl); pairs with smaa_blend.frag.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec4 SMAA_RT_METRICS;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out vec4 vary_offset;

#include "lib/smaa.glsl"

void main()
{
    gl_Position = vec4(position.xyz, 1.0);
    vary_texcoord0 = (gl_Position.xy*0.5+0.5);
    SMAANeighborhoodBlendingVS(vary_texcoord0, vary_offset);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
}
