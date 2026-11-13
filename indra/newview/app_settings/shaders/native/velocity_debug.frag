// velocity_debug.frag: the debug view of a motion target: red/green = pixels moved in x/y (full at 4 pixels), blue = the alpha flag (avatar pixels in the avatar velocity target).
// A line for line translation of "Avatar Velocity Debug" (deferred/avatarVelocityDebugF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    float show_flag;     // 1: blue = the alpha flag (the avatar target); 0: the target has no alpha
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 v = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord);
    frag_color = vec4(clamp(abs(v.xy) * 0.25, 0.0, 1.0), v.a * 0.35 * show_flag, 1.0);
}
