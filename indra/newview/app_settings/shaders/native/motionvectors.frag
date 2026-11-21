// motionvectors.frag: the screen motion of every pixel for the upscaler: the camera's motion from the depth (jittered clip space of this frame to the unjittered clip spaces of this frame and the last one),
// replaced by the avatar velocity target's own motion where an avatar wrote. A line for line translation of "Motion Vectors" (deferred/motionVectorsF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 to_cur;            // jittered clip space of this frame -> unjittered clip space of this frame
    mat4 to_prev;           // jittered clip space of this frame -> unjittered clip space of the last frame
    vec2 render_res;
    float use_avatar;       // 1 when the avatar velocity pass ran this frame, 2: the diffuseRect is the world pass's motion target (RG16 unorm, rk_motion_encode: x >= 0.99995 means none)
    float use_snapshot;     // 1: the emissiveRect holds the depth of the world pass; where the final depth is nearer, something drew over the pixel after the world pass and the motion kept there is not its own
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
    uint rk_ti_diffuseRect;     // the avatar velocity target: rg = total screen motion of an avatar pixel, a = 1 where an avatar wrote
    uint rk_ti_emissiveRect;    // the world pass's depth (R32F, depth_snap.frag), read when use_snapshot is set
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float depth = texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 cur = to_cur * ndc;
    vec4 prev = to_prev * ndc;
    vec2 cur_ndc = cur.xy / cur.w;
    vec2 prev_ndc = prev.xy / prev.w;

    // NDC spans 2 units across the render target. Texel rows grow with NDC y here (the mirror keeps GL's orientation), so no flip is needed.
    vec2 mv = (prev_ndc - cur_ndc) * 0.5 * render_res;

    if (use_avatar > 1.5)
    {
        vec2 e = texture(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], vary_fragcoord).rg;
        // a fullbright surface (drawn after lighting, in the post-deferred pass) writes no motion: the dancer behind a fullbright wall left its motion in the world pass's target, and the wall's
        // pixels would take it (the upscaler then drags the wall's texture along with the dancer: the user 2026-11-21). The world pass's own depth tells: nearer in the end = drawn over since.
        const bool overdrawn = use_snapshot > 0.5 && depth < texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).r - 1.0e-7;
        if (e.x < 0.99995 && !overdrawn)
        {
            mv = (e * 2.0 - 1.0) * 1024.0;
        }
    }
    else if (use_avatar > 0.5)
    {
        vec4 av = texture(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], vary_fragcoord);
        if (av.a > 0.5)
        {
            mv = av.xy;     // an avatar pixel: its own motion replaces the camera motion
        }
    }
    frag_color = vec4(mv, 0.0, 0.0);
}
