// moon.frag: the moon's texture times its brightness, faded near the horizon; the transparent pixels are discarded so the quad does not hide the stars. A line for line translation of
// "Deferred Windlight Moon Program pull" (deferred/moonF.glsl); pairs with moon.vert.

#include "sky_pass.inc"
#include "lib/flags.glsl"

layout(location = 0) in vec2 vary_texcoord0;

layout(location = 0) out vec4 frag_albedo;
layout(location = 1) out vec4 frag_spec;
layout(location = 2) out vec4 frag_normal;
#if defined(HAS_EMISSIVE)
layout(location = 3) out vec4 frag_emissive;
#endif
// the motion target (attachment 4 of the world pass, RG16 unorm, only when an upscaler is on; an output no attachment backs is dropped): this surface does not move by itself, so it writes the value the clear holds, 1.0 in x = "no motion of its own" (the motion pass then takes the camera motion)
layout(location = 4) out vec2 frag_velocity;

void main()
{
    frag_velocity = vec2(1.0, 0.0);
    // Restore Pre-EEP alpha fade moon near horizon
    float fade = 1.0;
    if (moon_dir.z > 0)
        fade = clamp(moon_dir.z * moon_dir.z * 4.0, 0.0, 1.0);

    vec4 c      = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_texcoord0.xy);

    // SL-14113 Don't write to depth; prevent moon's quad from hiding stars which should be visible
    // Moon texture has transparent pixels <0x55,0x55,0x55,0x00>
    if (c.a <= 2. / 255.) // 0.00784
    {
        discard;
    }

    c.rgb *= moon_brightness;
    c.a   *= fade;

    frag_albedo = vec4(0);
    frag_spec = vec4(0.0);
    frag_normal = vec4(0.0, 0.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_albedo = vec4(0);
    frag_emissive = vec4(c.rgb, c.a);
#else
    frag_albedo = vec4(c.rgb, c.a);
#endif
}
