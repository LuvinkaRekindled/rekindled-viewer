// lib/gbuffer.glsl: reading the G-buffer. The G-buffer layout is today's (PLAN-shaders-rewrite.md section 10 item 2): normal buffer = octahedral xy in the first two channels.

// the view space normal of a normal buffer sample
vec3 rk_decode_normal(vec2 enc)
{
    vec2 fenc = enc * 4.0 - 2.0;
    float f = dot(fenc, fenc);
    return vec3(fenc * sqrt(1.0 - f * 0.25), 1.0 - f * 0.5);
}
