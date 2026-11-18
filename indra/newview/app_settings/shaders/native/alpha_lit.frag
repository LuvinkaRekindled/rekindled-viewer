// alpha_lit.frag: a lit transparent surface (plain diffuse: no material maps). Texture times vertex colour, the ambient sky and the sun with its cascade shadow, every local light that reaches it
// (the view's light list, the same lights the opaque surfaces get), then the air or water between it and the eye. Matches the viewer's lit alpha program ("Deferred Alpha Pull Shader") including classic
// mode's own mixing; classic skies only for now (the probe ambient of the other skies is not written yet: the manifest line says `when classic_mode=1`).
// Pairs with alpha.vert (ALPHA_LIT).

#include "alpha_pass.inc"
#include "lib/color.glsl"
#include "lib/clip.glsl"
#include "lib/atmos.glsl"
#include "lib/fog.glsl"
#include "lib/shade.glsl"
#include "lib/shadow.glsl"
#include "lib/shadow_alpha.glsl"
#include "lib/light.glsl"
#ifdef ALPHA_PROBES
#include "lib/probe.glsl"
#endif
#include "lib/shade_lit.glsl"
#ifdef ALPHA_MAT
#if defined(SSR)
#include "lib/viewpos.glsl"
#include "lib/ssr.glsl"
#endif
#include "lib/material.glsl"
#endif

layout(location = 0) in vec3 vary_position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 vary_texcoord0;
layout(location = 3) flat in uint vary_slot;
layout(location = 4) in vec3 vary_norm;
#ifdef ALPHA_UBER
layout(location = 5) flat in uint vary_fullbright;      // the row's fullbright flag: the lit and the fullbright draws of the blend order are one run (one program)
#endif
#ifdef ALPHA_MAT
layout(location = 6) flat in uint vary_material;
layout(location = 7) in vec3 vary_tangent;
layout(location = 8) in vec2 vary_texcoord1;
layout(location = 9) in vec2 vary_texcoord2;
layout(location = 10) flat in float vary_sign;
layout(location = 11) flat in uint vary_slot_normal;
layout(location = 12) flat in uint vary_slot_specular;
layout(location = 13) flat in vec4 vary_specular;
layout(location = 14) flat in vec2 vary_matparams;
#endif

layout(location = 0) out vec4 frag_color;

void main()
{
    mirror_clip(vary_position);
    water_clip(vary_position);

    const vec4 texel = texture(rk_tbl2d[nonuniformEXT(LL_HM(vary_slot))], vary_texcoord0);
#ifdef ALPHA_MAT
    if ((vary_material & 1u) != 0u)
    {
        frag_color = shade_material(vary_position, texel, vertex_color, vary_norm, vary_tangent, vary_sign, vary_texcoord0, vary_texcoord1, vary_texcoord2,
                                    vary_material, vary_slot_normal, vary_slot_specular, vary_specular, vary_matparams);
        return;
    }
#endif
#ifdef ALPHA_UBER
    if (vary_fullbright != 0u)
    {
        frag_color = shade_fullbright(vary_position, texel, vertex_color);
        return;
    }
#endif
    frag_color = shade_lit(vary_position, vary_norm, texel, vertex_color);       // (lib/shade_lit.glsl, shared with avatar_alpha.frag)
}
