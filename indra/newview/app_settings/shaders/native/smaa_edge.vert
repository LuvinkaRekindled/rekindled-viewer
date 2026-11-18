// smaa_edge.vert: the vertex stage of SMAA's edge detection (a full screen triangle and the offsets of the neighbours). A line for line translation of "SMAA Edge Detection" (deferred/SMAAEdgeDetectV.glsl);
// pairs with smaa_edge.frag; the manifest lines define SMAA_GLSL_4, SMAA_PREDICATION, SMAA_REPROJECTION and the preset.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec4 SMAA_RT_METRICS;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out vec4 vary_offset[3];

#include "lib/smaa.glsl"

void main()
{
    gl_Position = vec4(position.xyz, 1.0);
    vary_texcoord0 = (gl_Position.xy*0.5+0.5);
    SMAAEdgeDetectionVS(vary_texcoord0, vary_offset);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
}
