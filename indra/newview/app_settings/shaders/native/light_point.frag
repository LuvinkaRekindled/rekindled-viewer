// light_point.frag: the volume of one point light (the box light_volume.vert makes), lit onto what is behind it. A translation of "Deferred Light Pull Shader" (deferred/pointLightF.glsl with LL_LIGHT_PULL); pairs with
// light_volume.vert (LIGHT_VOLUME). Added to the lit picture by the pass's blending: the alpha is 0.
// What is different: the position comes from the depth with the few multiplications the projection leaves (lib/viewpos.glsl) and the pixel's place from gl_FragCoord (no varying and no division); a pixel outside
// the light's sphere (the box has its corners) leaves before the G-buffer is read, a legacy surface that faces away from the light before its albedo is; a pixel that is not a glTF one skips the PBR terms and the
// other way round. The old program tested the facing after clamping the angle to a minimum of 1e-6 (the test never took): a surface facing away got a 1e-6 sliver of light, here it gets none.

#include "light_pass.inc"
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"
#include "lib/color.glsl"
#include "lib/flags.glsl"
#include "lib/pbr_core.glsl"
#include "lib/lightvol.glsl"

layout(location = 0) flat in vec4 vary_light_a;
layout(location = 1) flat in vec4 vary_light_b;
layout(location = 2) flat in uvec2 vary_light_row;

layout(location = 0) out vec4 frag_color;

void main()
{
    const vec2 uv = gl_FragCoord.xy / screen_res;
    const float depth = textureLod(LV_TAP(ll_ti_depthMap), uv, 0.0).r;
    const vec3 pos = ll_view_pos(inv_proj, uv, depth);

    const float size = vary_light_b.w;
    const vec3 lv = vary_light_b.xyz - pos;
    const float d2 = dot(lv, lv);
    if (d2 >= size * size)
    {
        discard;
    }

    LightSurface s = light_surface_geometry(textureLod(LV_TAP(ll_ti_normalMap), uv, 0.0), pos);
    if (!s.pbr && dot(s.n, lv) <= 0.0)
    {
        discard;
    }
    light_surface_material(uv, s);

    const float dist = sqrt(d2) / size;
    const vec3 light_color = vary_light_a.rgb;
    const float falloff = vary_light_a.w;
    const vec3 lit = s.pbr ? light_pbr(s, lv, dist, light_color, falloff) : light_legacy(s, lv, dist, light_color, falloff);
    frag_color = vec4(max(lit * (vary_light_row.y > 0u ? 0.9 : 1.0), vec3(0.0)), 0.0);
}
