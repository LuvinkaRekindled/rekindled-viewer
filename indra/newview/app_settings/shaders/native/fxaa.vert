// fxaa.vert: a full screen triangle for the FXAA pass: the coordinate of the screen (vary_fragcoord) and the one the picture is read by (vary_tc: the same scaled by tc_scale, the share of the target the picture
// fills). A line for line translation of the viewer's post vertex shader (deferred/postDeferredV.glsl; its screen_res uniform was never read); pairs with fxaa.frag. (the pass block is declared the same in
// both stages)

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    vec2 tc_scale;
    vec2 rcp_screen_res;
};

layout(location = 0) in vec3 position;
layout(location = 0) out vec2 vary_fragcoord;
layout(location = 1) out vec2 vary_tc;

void main()
{
    gl_Position = vec4(position, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_tc = (position.xy * 0.5 + 0.5) * tc_scale;
    vary_fragcoord = position.xy * 0.5 + 0.5;
}
