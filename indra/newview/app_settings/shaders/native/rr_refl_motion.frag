// rr_refl_motion.frag: the motion vector of what a traced reflection shows (for the denoiser): the surface's motion, or where the reflected hit point moved when the reflection was traced. A line for line
// translation of "Ray Reconstruction Reflection Motion" (deferred/rrReflMotionF.glsl); pairs with fullscreen.vert.

#define LEGACY_DEPTH 1

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    mat4 inv_proj;
    mat4 inv_scene;             // jittered clip space of this frame -> world
    mat4 view_proj_cur;         // world -> unjittered clip space of this frame
    mat4 view_proj_prev;        // world -> unjittered clip space of the last frame
    mat3 view_to_world;         // the eye space the G-buffer normals are in -> world
    vec3 cam_pos;               // the camera in the world
    vec2 render_res;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_diffuseRect;     // the surface motion vectors (rg, pixels)
    uint ll_ti_emissiveRect;    // the traced reflection's hit distance (r, metres from the surface; 0 = not traced)
    uint ll_ti_normalMap;
    uint ll_ti_depthMap;
};

#include "lib/legacy.glsl"

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

vec4 getNorm(vec2 pos_screen)
{
    return decodeNormal(texture(ll_tbl2d[LL_HM(ll_ti_normalMap)], pos_screen.xy));
}

float getDepth(vec2 pos_screen)
{
    return texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], pos_screen).r;
}

void main()
{
    vec2 uv = vary_fragcoord;
    vec2 mv = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], uv).rg;
    float t = texture(ll_tbl2d[LL_HM(ll_ti_emissiveRect)], uv).r;
    float depth = getDepth(uv);
    if (t > 0.0 && depth < 1.0)
    {
        vec4 w = inv_scene * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 p = w.xyz / w.w;
        vec3 n = normalize(view_to_world * getNorm(uv).xyz);
        vec3 r = reflect(normalize(p - cam_pos), n);
        vec4 h = vec4(p + r * t, 1.0);
        vec4 cur = view_proj_cur * h;
        vec4 prev = view_proj_prev * h;
        if (cur.w > 0.0 && prev.w > 0.0)
        {
            mv = (prev.xy / prev.w - cur.xy / cur.w) * 0.5 * render_res;
        }
    }
    frag_color = vec4(mv, 0.0, 0.0);
}
