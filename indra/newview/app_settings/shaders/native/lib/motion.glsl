// lib/motion.glsl: the motion target of the world pass (attachment 4, RG16 unorm; PLAN-velocity-fold-and-skin-once.md). A surface that moves by itself writes its screen motion in pixels (this frame's position
// to last frame's, BOTH UNDER THIS FRAME'S CAMERA since 2026-11-22: the motion of the surface alone, the motion pass adds the camera's motion from the depth, in double precision; with each pose under the camera of
// its own frame the rounding of two absolute camera matrices at 3000 m made 0.1 px of random motion; the sign of the old avatar velocity pass: prev - cur). x = 1.0 is reserved for "no motion of its own" (the clear value,
// and what every surface that does not move writes), the motion pass then takes the camera motion from the depth.
// 2026-11-22: the value is COMPANDED, e = 0.5 + 0.5 * sign(m) * sqrt(|m| / 1024), decoded as m = sign(s) * s * s * 1024 with s = 2 e - 1. It was linear, (m / 1024 + 1) / 2: 16 bits over +-1024 pixels is a step of
// 0.031 pixel, and an idle sway of 0.01 to 0.2 pixel a frame came out as a staircase of such steps (bands in the motion view), a rounding error of up to 0.016 pixel a frame in a pattern fixed in space; FSR keeps
// history over ten or twenty frames at a fast frame rate, so the errors added up to a swim of the face along the steps' edges. Companded the step is 0.0004 pixel at 0.05 pixel, 0.002 at 1 pixel, 0.02 at 100.
// the encoding of a motion in pixels (the companding above)
vec2 rk_motion_encode_px(vec2 motion)
{
    const vec2 s = sign(motion) * sqrt(min(abs(motion) * (1.0 / 1024.0), vec2(1.0)));
    return clamp(s * 0.5 + 0.5, vec2(0.0), vec2(0.99998, 1.0));
}

vec2 rk_motion_encode(vec4 cur, vec4 prev, vec2 res)
{
    if (!(cur.w > 0.0) || !(prev.w > 0.0))
    {
        return vec2(1.0, 0.0);
    }
    return rk_motion_encode_px((prev.xy / prev.w - cur.xy / cur.w) * 0.5 * res);
}

// the motion in pixels of an encoded value (the motion pass and the debug view)
vec2 rk_motion_decode(vec2 e)
{
    const vec2 s = e * 2.0 - 1.0;
    return sign(s) * s * s * 1024.0;
}
