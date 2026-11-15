// motionvectors.frag: the screen motion of every pixel for the upscaler: the camera's motion from the depth (jittered clip space of this frame to the unjittered clip spaces of this frame and the last one),
// replaced by the avatar velocity target's own motion where an avatar wrote. A line for line translation of "Motion Vectors" (deferred/motionVectorsF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 to_cur;            // jittered clip space of this frame -> unjittered clip space of this frame
    mat4 to_prev;           // jittered clip space of this frame -> unjittered clip space of the last frame
    vec2 render_res;
    float use_avatar;       // 1 when the avatar velocity pass ran this frame, 2: the diffuseRect is the world pass's motion target (RG16 unorm, ll_motion_encode: x >= 0.99995 means none)
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_depthMap;
    uint ll_ti_diffuseRect;     // the avatar velocity target: rg = total screen motion of an avatar pixel, a = 1 where an avatar wrote
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float depth = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 cur = to_cur * ndc;
    vec4 prev = to_prev * ndc;
    vec2 cur_ndc = cur.xy / cur.w;
    vec2 prev_ndc = prev.xy / prev.w;

    // NDC spans 2 units across the render target. Texel rows grow with NDC y here (the mirror keeps GL's orientation), so no flip is needed.
    vec2 mv = (prev_ndc - cur_ndc) * 0.5 * render_res;

    if (use_avatar > 1.5)
    {
        vec2 e = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord).rg;
        if (e.x < 0.99995)
        {
            mv = (e * 2.0 - 1.0) * 1024.0;
        }
    }
    else if (use_avatar > 0.5)
    {
        vec4 av = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord);
        if (av.a > 0.5)
        {
            mv = av.xy;     // an avatar pixel: its own motion replaces the camera motion
        }
    }
    frag_color = vec4(mv, 0.0, 0.0);
}
