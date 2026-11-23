// motionvectors.frag: the screen motion of every pixel for the upscaler: the camera's motion from the depth (jittered clip space of this frame to the unjittered clip spaces of this frame and the last one),
// replaced by the avatar velocity target's own motion where an avatar wrote. A line for line translation of "Motion Vectors" (deferred/motionVectorsF.glsl); pairs with fullscreen.vert.
#include "lib/motion.glsl"

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    mat4 to_cur;            // jittered clip space of this frame -> unjittered clip space of this frame
    mat4 to_prev;           // jittered clip space of this frame -> unjittered clip space of the last frame
    vec2 render_res;
    float use_avatar;       // 1 when the avatar velocity pass ran this frame, 2: the diffuseRect is the world pass's motion target (RG16 unorm, rk_motion_encode: x >= 0.99995 means none)
    float use_snapshot;     // 1: the emissiveRect holds the depth of the world pass; where the final depth is nearer, something drew over the pixel after the world pass and the motion kept there is not its own
    float use_blend;        // 1: the specularRect holds the reactive mask (reactive_mask.frag); where it is above blend_cut, blended surfaces changed the pixel after the opaque pass and the motion kept there is the opaque thing behind
    float own_scale;        // the rig's calibration (rt-debug.txt fsrown=): a factor on the world pass's own motion, 1 normally
    float blend_cut;        // the mask value from which the pixel takes the camera's motion (RK_VK_MOTIONBLEND_CUT, default 0.1: a faint blended surface over a moving thing still drags it)
    float use_bmotion;      // 1: the normalRect is the blended pass's motion attachment (RG16, x = 1.0: none): where a skinned blended surface wrote its motion, that is the pixel's motion
    float hair_gap;         // > 0 (the rig's fsrhairmv=2): a pixel drawn over after the world pass whose surface is within this many metres of the world pass's surface (rigged blended hair writes depth just above the head) keeps the motion of the world pass
    float proj_c;           // the projection's [2][2] and [3][2] (view distance from depth: d / (2 depth - 1 + c))
    float proj_d;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
    uint rk_ti_diffuseRect;     // the avatar velocity target: rg = total screen motion of an avatar pixel, a = 1 where an avatar wrote
    uint rk_ti_emissiveRect;    // the world pass's depth (R32F, depth_snap.frag), read when use_snapshot is set
    uint rk_ti_specularRect;    // the reactive mask (R8, reactive_mask.frag), read when use_blend is set
    uint rk_ti_normalMap;      // the blended pass's motion attachment (screen target attachment 2), read when use_bmotion is set
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float depth = texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], vary_fragcoord).r;
    vec4 ndc = vec4(vary_fragcoord * 2.0 - 1.0, rk_depth_ndc(depth), 1.0);
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
        const float world_depth = use_snapshot > 0.5 ? texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).r : rk_depth_far();
        bool overdrawn = use_snapshot > 0.5 && rk_depth_in_front(depth, world_depth, 1.0e-7);
        bool hair_core = false;
        if (overdrawn && hair_gap > 0.0)
        {
            const float dist_now = proj_d / (rk_depth_ndc(depth) + proj_c);
            const float dist_world = proj_d / (rk_depth_ndc(world_depth) + proj_c);
            hair_core = abs(dist_world - dist_now) < hair_gap;
            overdrawn = overdrawn && !hair_core;
        }
        // a blended surface (a vending machine's screen over a dancer) writes no motion either: its pixel is what is seen, so it moves with the camera, not with the dancer behind it. Left with the
        // dancer's motion the upscaler dragged the screen's image along (the user 2026-11-21); the reactive mask alone made it throw its history away and the screen jittered.
        const bool blended = !hair_core && use_blend > 0.5 && texture(rk_tbl2d[RK_HM(rk_ti_specularRect)], vary_fragcoord).r > blend_cut;
        if (e.x < 0.99995 && !overdrawn && !blended)
        {
            // the world pass wrote the motion of the surface itself under this frame's camera (2026-11-22: it used to write the whole motion under the two cameras, and at 3000 m the float32 rounding of the
            // two absolute camera matrices made 0.1 px of random motion on every rigged pixel: the avatar rippled under FSR); the camera's own motion, from the depth in double precision, is added here
            mv += rk_motion_decode(e) * own_scale;
        }
    }
    if (use_avatar > 1.5 && use_bmotion > 0.5)
    {
        // a skinned blended surface (hair, shoes with alpha parts, clothes) is what is seen here: its own motion replaces the camera's and the opaque thing behind it
        const vec2 bm = texture(rk_tbl2d[RK_HM(rk_ti_normalMap)], vary_fragcoord).rg;
        if (bm.x < 0.99995)
        {
            mv = (prev_ndc - cur_ndc) * 0.5 * render_res + rk_motion_decode(bm) * own_scale;
        }
    }
    if (use_avatar > 1.5)
    {
        // (nothing more here: the branches above and below are the older paths)
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
