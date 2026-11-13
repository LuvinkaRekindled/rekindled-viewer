// velocity.frag: the motion of a surface in the picture, in pixels (rg; b 0, a 1), for what the avatar and object velocity pass draws. A line for line translation of the viewer's avatarVelocityF.glsl, shared by
// "Object Velocity (packets)", "Avatar Velocity (rigged, packets)" and "Avatar Velocity (body) pull"; pairs with velocity_object.vert / velocity_skin.vert / velocity_body.vert.
// A fragment that is not the surface the G-buffer holds at its pixel (behind it, or in front of it by more than the tolerance) is thrown away: the pass has no depth test of its own.

#include "velocity_pass.inc"

layout(location = 0) in vec4 vary_clip;
layout(location = 1) in vec4 vary_cur;
layout(location = 2) in vec4 vary_prev;

layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 uv = (vary_clip.xy / vary_clip.w) * 0.5 + 0.5;
    const float depth = textureLod(ll_tbl2d[LL_HM(ll_ti_depthMap)], uv, 0.0).r;
    const float gbuffer_dist = proj_z.y / ((depth * 2.0 - 1.0) + proj_z.x);
    if (abs(vary_clip.w - gbuffer_dist) > depth_tolerance * gbuffer_dist + 0.005)
    {
        discard;
    }

    vec2 mv = vec2(0.0);
    if (vary_prev.w > 0.0 && vary_cur.w > 0.0)
    {
        mv = (vary_prev.xy / vary_prev.w - vary_cur.xy / vary_cur.w) * 0.5 * render_res;
    }
    frag_color = vec4(mv, 0.0, 1.0);
}
