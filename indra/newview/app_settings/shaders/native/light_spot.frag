// light_spot.frag: a projector (a spot light with an image and an ambiance), lit onto what is behind its volume. Two programs from one source, matching the viewer's:
//   LIGHT_VOLUME + LIGHT_SPOT   "Deferred SpotLight Pull Shader" (deferred/spotLightF.glsl with RK_PROJECTOR_PULL): the projectors of a pass from one record, the box light_volume.vert makes, the projector's row
//                               (matrix, plane, range, focus, shadow index and fade, the image's bindless slot) read from the table by the instance
//   LIGHT_MULTI_SPOT            "Deferred MultiSpotLight Shader": the projector that has the camera inside its box, drawn on the full screen triangle (fullscreen.vert), its values in uniforms, its image bound by name
// The shading is the viewer's, term for term: the projected image's diffuse colour and its ambiance, the shadow of the spot shadow maps, the legacy highlight and the image's reflection in a shiny legacy surface
// (a glTF surface takes the diffuse and the ambiance through the GGX terms).
// What is different: the position comes from the depth with the few multiplications the projection leaves; the volume's sphere, the projector's near plane and the image's frame are tested before the G-buffer is read
// (a pixel outside the frame has nothing to show but the image's reflection in a shiny legacy surface, which the normal buffer's sample says: everything else leaves there); the albedo and the material are read
// for the pixels that are lit; the BRDF of a glTF surface is evaluated once (the old program evaluated it twice with the same arguments); the projector's row is read here, only for pixels of the volume that the
// sphere test keeps, instead of being sent as 27 flat varyings.

#include "light_pass.inc"
#include "lib/gbuffer.glsl"
#include "lib/viewpos.glsl"
#include "lib/color.glsl"
#include "lib/flags.glsl"
#include "lib/pbr_core.glsl"
#include "lib/lightvol.glsl"

#ifdef LIGHT_VOLUME
layout(location = 0) flat in vec4 vary_light_a;
layout(location = 1) flat in vec4 vary_light_b;
layout(location = 2) flat in uvec2 vary_light_row;
#endif

layout(location = 0) out vec4 frag_color;

struct Projector
{
    mat4 mat;           // screen space to light space
    vec3 n;             // the plane's normal
    vec3 p;             // a point of the plane the projection emits from
    vec3 origin;        // the origin of the projection
    float range;
    float ambiance;
    float focus;
    float lod;          // number of mips of the image
    float shadow_fade;
    int shadow_idx;     // which spot shadow (0 or 1), or -1
    uint slot;          // the image's bindless slot (LIGHT_VOLUME)
};

Projector load_projector()
{
    Projector pr;
#ifdef LIGHT_VOLUME
    LightRows rows = LightRows(rk_light_table);
    const uint at = vary_light_row.x * LIGHT_ROW_STRIDE;
    const vec4 r2 = rows.v[at + 2u];
    const vec4 r3 = rows.v[at + 3u];
    const vec4 r4 = rows.v[at + 4u];
    const vec4 r5 = rows.v[at + 5u];
    const vec4 r6 = rows.v[at + 6u];
    pr.slot = floatBitsToUint(r2.z);
    pr.shadow_idx = floatBitsToInt(r2.w);
    pr.n = r3.xyz;
    pr.p = r4.xyz;
    pr.range = r4.w;
    pr.origin = r5.xyz;
    pr.ambiance = r5.w;
    pr.focus = r6.x;
    pr.lod = r6.y;
    pr.shadow_fade = r6.w;
    pr.mat = mat4(rows.v[at + 7u], rows.v[at + 8u], rows.v[at + 9u], rows.v[at + 10u]);
#else
    pr.slot = 0u;
    pr.shadow_idx = proj_shadow_idx;
    pr.n = proj_n;
    pr.p = proj_p;
    pr.range = proj_range;
    pr.origin = proj_origin;
    pr.ambiance = proj_ambiance;
    pr.focus = proj_focus;
    pr.lod = proj_lod;
    pr.shadow_fade = shadow_fade;
    pr.mat = proj_mat;
#endif
    return pr;
}

vec4 proj_image(Projector pr, vec2 tc, float lod)
{
#ifdef LIGHT_VOLUME
    return textureLod(rk_tbl2d[nonuniformEXT(RK_HM(pr.slot))], tc, lod);
#else
    return textureLod(LV_TAP(rk_ti_projectionMap), tc, lod);
#endif
}

// the image's colour (linear, premultiplied by its alpha) at a blur level, faded towards the frame's edge: the diffuse light, and (the specular's own extra factor kept) the reflection
vec4 proj_diffuse_image(Projector pr, vec2 tc, float lod)
{
    vec4 ret = proj_image(pr, tc, lod);
    ret.rgb = srgb_to_linear(ret.rgb);
    const vec2 dist = vec2(0.5) - abs(tc - vec2(0.5));
    const float det = min(lod / (pr.lod * 0.5), 1.0);
    const float d = min(dist.x, dist.y);
    return ret * clamp(d / (0.25 * det), 0.0, 1.0);
}

vec4 proj_specular_image(Projector pr, vec2 tc, float lod)
{
    vec4 ret = proj_image(pr, tc, lod);
    ret.rgb = srgb_to_linear(ret.rgb);
    const vec2 dist = vec2(0.5) - abs(tc - vec2(0.5));
    const float det = min(lod / (pr.lod * 0.5), 1.0);
    float d = min(dist.x, dist.y);
    d *= min(1.0, d * (pr.lod - lod));          // (the viewer's extra factor compared to the diffuse: kept, it is part of the look)
    return ret * clamp(d / (0.25 * det), 0.0, 1.0);
}

vec4 proj_ambient_image(Projector pr, vec2 tc)
{
    vec4 ret = proj_image(pr, tc, pr.lod);
    ret.rgb = srgb_to_linear(ret.rgb);
    const vec2 dist = tc - vec2(0.5);
    return ret * min(clamp((0.25 - dot(dist, dist)) / 0.25, 0.0, 1.0), 1.0);
}

// the projected light on a surface at light_distance along the projection: the light's colour times the image at the blur level of that distance
vec3 proj_light_diffuse(Projector pr, vec3 light_color, float light_distance, vec2 tc)
{
    const float lod = clamp((light_distance - pr.focus) / pr.range, 0.0, 1.0) * pr.lod;
    const vec4 plcol = proj_diffuse_image(pr, tc, lod);
    return light_color * plcol.rgb * plcol.a;
}

vec3 proj_light_ambiance(Projector pr, vec3 light_color, float amb_da, float attenuation, float lit, float nl, vec2 tc)
{
    const vec4 amb_plcol = proj_ambient_image(pr, tc);
    amb_da += pr.ambiance;
    amb_da += (nl * nl * 0.5 + 0.5) * pr.ambiance;
    amb_da *= attenuation;
    amb_da = min(amb_da, 1.0 - lit);
    return amb_da * light_color * (amb_plcol.rgb * amb_plcol.a);
}

void main()
{
    const vec2 uv = gl_FragCoord.xy / screen_res;
    const float depth = textureLod(LV_TAP(rk_ti_depthMap), uv, 0.0).r;
    const vec3 pos = rk_view_pos(inv_proj, uv, depth);

#ifdef LIGHT_VOLUME
    const vec3 light_color = vary_light_a.rgb;
    const float light_falloff = vary_light_a.w;
    const vec3 light_center = vary_light_b.xyz;
    const float light_size = vary_light_b.w;
    const bool classic = vary_light_row.y > 0u;
#else
    const vec3 light_color = color;
    const float light_falloff = falloff;
    const vec3 light_center = center;
    const float light_size = size;
    const bool classic = classic_mode > 0;
#endif

    // the volume's sphere
    const vec3 lv0 = light_center - pos;
    float dist = length(lv0);
    if (dist >= light_size)
    {
        discard;
    }
    dist /= light_size;

    const Projector pr = load_projector();
    // the pixel in the projector's frame
    const float l_dist = -dot(lv0, pr.n);
    vec4 proj_tc = pr.mat * vec4(pos, 1.0);
    if (proj_tc.z < 0.0)
    {
        discard;
    }
    proj_tc.xyz /= proj_tc.w;
    const bool in_frame = proj_tc.x > 0.0 && proj_tc.x < 1.0 && proj_tc.y > 0.0 && proj_tc.y < 1.0;

    LightSurface s = light_surface_geometry(textureLod(LV_TAP(rk_ti_normalMap), uv, 0.0), pos);
    // outside the frame a glTF surface gets nothing, and a legacy one only the image's reflection (a shiny surface's)
    if (!in_frame && (s.pbr || s.env <= 0.0))
    {
        discard;
    }
    light_surface_material(uv, s);

    float shadow = 1.0;
    if (pr.shadow_idx >= 0)
    {
        const vec4 shd = textureLod(LV_TAP(rk_ti_lightMap), uv, 0.0);
        shadow = clamp((pr.shadow_idx == 0 ? shd.b : shd.a) + pr.shadow_fade, 0.0, 1.0);
    }

    const float dist_atten = light_attenuation(dist, light_falloff);
    if (dist_atten <= 0.0)
    {
        discard;
    }

    // the angle to the projector's origin (the old program clamped it to a minimum of 1e-6, which makes its test of the sign always pass: kept)
    const vec3 l = normalize(pr.origin - pos);
    const float nl = clamp(dot(s.n, l), 1e-6, 1.0);
    vec3 final_color = vec3(0.0);

    if (s.pbr)
    {
        // the image's light through the GGX terms, and the ambiance of the image through the same
        float pnl;
        const vec3 punctual = pbr_punctual(s.p, l, pnl);
        const vec3 brdf = clamp(pnl * punctual, vec3(0.0), vec3(10.0));
        const vec3 dlit = proj_light_diffuse(pr, light_color, l_dist, proj_tc.xy);
        final_color += (dist_atten * 3.25 * shadow) * dlit * brdf;
        const float amb_da = (nl * 0.5 + 0.5) * pr.ambiance;
        const vec3 amb_rgb = proj_light_ambiance(pr, light_color, amb_da, dist_atten, 0.0, pnl, proj_tc.xy) * 3.25;
        final_color += amb_rgb * brdf;
    }
    else
    {
        vec3 dlit = vec3(0.0);
        if (in_frame && proj_tc.z > 0.0)
        {
            const float lit = nl * dist_atten;
            dlit = proj_light_diffuse(pr, light_color, l_dist, proj_tc.xy);
            final_color = dlit * lit * s.diffuse * shadow;
            // (unshadowed for consistency between forward and deferred: the viewer's own note)
            const float amb_da = (nl * 0.5 + 0.5) * pr.ambiance;
            const vec3 amb_rgb = proj_light_ambiance(pr, light_color, amb_da, dist_atten, lit, nl, proj_tc.xy);
            final_color += s.diffuse * amb_rgb * max(dot(-l, s.n), 0.0);
        }

        if (s.spec.a > 0.0 && dlit != vec3(0.0))
        {
            dlit *= min(nl * 6.0, 1.0) * dist_atten;
            const vec3 h = normalize(l + s.v);
            const float nh = clamp(dot(s.n, h), 1e-6, 1.0);
            const float vh = clamp(dot(s.v, h), 1e-6, 1.0);
            const float x = 1.0 - vh;
            const float fres = x * x * x * x * x * 0.4 + 0.5;
            const float gtdenom = 2.0 * nh;
            const float gt = max(0.0, min(gtdenom * s.nv / vh, gtdenom * nl / vh));
            const float scol = fres * textureLod(LV_TAP(rk_ti_lightFunc), vec2(nh, s.spec.a), 0.0).r * gt / (nh * nl);
            final_color += clamp(dlit * scol * s.spec.rgb * shadow, vec3(0.0), vec3(1.0));
        }

        if (s.env > 0.0)
        {
            // the image's reflection: from the point in the direction of the reflection to the projector's plane
            const vec3 ref = reflect(normalize(pos), s.n);
            const float ds = dot(ref, pr.n);
            if (ds < 0.0)
            {
                const vec3 pfinal = pos + ref * dot(pr.p - pos, pr.n) / ds;
                vec4 stc = pr.mat * vec4(pfinal, 1.0);
                if (stc.z > 0.0)
                {
                    stc /= stc.w;
                    if (stc.x < 1.0 && stc.y < 1.0 && stc.x > 0.0 && stc.y > 0.0)
                    {
                        final_color += light_color * proj_specular_image(pr, stc.xy, (1.0 - s.spec.a) * (pr.lod * 0.6)).rgb * shadow * s.env;
                    }
                }
            }
        }
    }

    // (the old program: "not sure why, but this line prevents MATBUG-194")
    final_color = max(final_color, vec3(0.0));
    frag_color = vec4(final_color * (classic ? 0.9 : 1.0), 0.0);
}
