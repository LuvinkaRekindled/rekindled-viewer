// buffer_visual.frag: the debug view of one target of the deferred buffer: the texture at a mip level. A line for line translation of "Deferred Buffer Visualization Shader"
// (deferred/postDeferredVisualizeBuffers.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float mipLevel;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 diff = textureLod(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], vary_fragcoord.xy, mipLevel);

    frag_color = diff;
}
