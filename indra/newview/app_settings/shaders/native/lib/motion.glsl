// lib/motion.glsl: the motion target of the world pass (attachment 4, RG16 unorm; PLAN-velocity-fold-and-skin-once.md). A surface that moves by itself writes its screen motion in pixels (this frame's position
// to last frame's, both under the unjittered camera of their frame, the sign of the old avatar velocity pass: prev - cur) as (motion / 1024 + 1) / 2; x = 1.0 is reserved for "no motion of its own" (the clear value,
// and what every surface that does not move writes), the motion pass then takes the camera motion from the depth.
vec2 ll_motion_encode(vec4 cur, vec4 prev, vec2 res)
{
    if (!(cur.w > 0.0) || !(prev.w > 0.0))
    {
        return vec2(1.0, 0.0);
    }
    const vec2 motion = (prev.xy / prev.w - cur.xy / cur.w) * 0.5 * res;
    return clamp(motion * (0.5 / 1024.0) + 0.5, vec2(0.0), vec2(0.99998, 1.0));
}
