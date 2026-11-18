// velocity_debug.frag: the debug view of a motion target: red/green = pixels moved in x/y (full at 4 pixels), blue = the alpha flag (avatar pixels in the avatar velocity target).
// A line for line translation of "Avatar Velocity Debug" (deferred/avatarVelocityDebugF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float show_flag;     // 1: blue = the alpha flag (the avatar target); 0: the target has no alpha; 2: the world pass's motion target (see main)
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 v = texture(rk_tbl2d[LL_HM(rk_ti_diffuseRect)], vary_fragcoord);
    if (show_flag > 1.5)
    {
        // show_flag 2: the world pass's motion target itself (RG16 unorm, lib/motion.glsl): x = 1.0 = "no motion of its own" (dark blue), anything else is a motion of its own: red/green = pixels (full at 4)
        if (v.x >= 0.99995)
        {
            frag_color = vec4(0.0, 0.0, 0.4, 1.0);
        }
        else
        {
            frag_color = vec4(clamp(abs((v.xy * 2.0 - 1.0) * 1024.0) * 0.25, 0.0, 1.0), 0.0, 1.0);
        }
        return;
    }
    frag_color = vec4(clamp(abs(v.xy) * 0.25, 0.0, 1.0), v.a * 0.35 * show_flag, 1.0);
}
