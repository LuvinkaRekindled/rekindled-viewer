// forward_lit.frag: the old forward lit fragment stage of the avatar previews: the texture thrown away under a minimum alpha, times the vertex colour, plus the atmosphere (atmosLighting). A line for line translation
// of "Avatar Shader" and "No color alpha mask Shader" (avatar/avatarF.glsl, objects/simpleF.glsl: default_lighting() of lighting/lightAlphaMaskF.glsl, windlight/atmosphericsF.glsl; scaleSoftClip is the
// identity in the old files and is not translated); pairs with forward_lit.vert. (both stages include forward_pass.inc: the blocks are declared the same)

#define LEGACY_COLOR 1

#include "forward_pass.inc"
#include "lib/legacy.glsl"

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec3 vary_AdditiveColor;
layout(location = 3) in vec3 vary_AtmosAttenuation;

layout(location = 0) out vec4 frag_color;

vec3 atmosFragLighting(vec3 light, vec3 additive, vec3 atten)
{
    light *= atten.r;
    additive = srgb_to_linear(additive*2.0);
    additive *= sky_hdr_scale;
    light += additive;
    return light;
}

vec3 atmosLighting(vec3 light)
{
    return atmosFragLighting(light, vary_AdditiveColor, vary_AtmosAttenuation);
}

void main()
{
    vec4 color = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_texcoord0.xy);

    if (color.a < minimum_alpha)
    {
        discard;
    }

    color *= vertex_color;

    color.rgb = atmosLighting(color.rgb);

    frag_color = max(color, vec4(0));
}
