// light_multi.frag: up to 16 point lights that have the camera inside their box (the full screen triangle draws them all at once): each pixel takes every light that reaches it. A translation of the 16 programs
// "Deferred MultiLight Shader 0" to "15" (deferred/multiPointLightF.glsl with LIGHT_COUNT 1 to 16), now one program: the number of lights is a uniform (light_count) and the loop stops there, as the light arrays
// are the 16 the pass can hold. Pairs with fullscreen.vert (LIGHT_MULTI).
// What is different: one program instead of sixteen (the pass chose one by the count: sixteen pipelines to build and to switch between); the position comes from the depth first, and the lights that reach
// the pixel are found before anything else is read (a pixel none of them reaches, the sky and everything beyond the lights, leaves at once: the old program read the whole G-buffer for it); the terms of a
// glTF surface's view are made once for the pixel and not again for each light; the lights that reach the pixel are visited by their bits, not tested again.

#include "light_pass.inc"
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"
#include "lib/color.glsl"
#include "lib/flags.glsl"
#include "lib/pbr_core.glsl"
#include "lib/lightvol.glsl"

layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 uv = gl_FragCoord.xy / screen_res;
    const float depth = textureLod(LV_TAP(rk_ti_depthMap), uv, 0.0).r;
    const vec3 pos = rk_view_pos(inv_proj, uv, depth);
    if (pos.z < far_z)
    {
        discard;
    }

    // the lights that reach the pixel
    uint reach = 0u;
    const int count = min(light_count, 16);
    for (int i = 0; i < count; ++i)
    {
        const vec3 lv = light[i].xyz - pos;
        if (dot(lv, lv) <= light[i].w * light[i].w)
        {
            reach |= 1u << uint(i);
        }
    }
    if (reach == 0u)
    {
        discard;
    }

    LightSurface s = light_surface_geometry(textureLod(LV_TAP(rk_ti_normalMap), uv, 0.0), pos);
    light_surface_material(uv, s);

    vec3 lit = vec3(0.0);
    while (reach != 0u)
    {
        const int i = findLSB(reach);
        reach &= reach - 1u;
        const vec3 lv = light[i].xyz - pos;
        const float dist = length(lv) / light[i].w;
        if (s.pbr)
        {
            lit += light_pbr(s, lv, dist, light_col[i].rgb, light_col[i].a);
        }
        else if (dot(s.n, lv) > 0.0)
        {
            lit += light_legacy(s, lv, dist, light_col[i].rgb, light_col[i].a);
        }
    }
    frag_color = vec4(max(lit * (classic_mode > 0 ? 0.9 : 1.0), vec3(0.0)), 0.0);
}
