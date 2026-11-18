// underwater.frag: the water plane seen from below: the picture under the water pushed by the waves (or the fog's own colour when the picture is off), with the water's fog over it. A translation of "Underwater
// Shader" (class 3 environment/underWaterF.glsl); pairs with water.vert (UNDERWATER: the blocks of water_pass.inc are the short ones). The helper functions are the old ones (lib/legacy.glsl).
// What is different: the three wave maps and the exclusion mask are read before the mirror clip and the others by explicit level (they are single level targets), so that no pixel's lookup depends on the
// pixels around it after a discard.

#define LEGACY_COLOR 1
#define LEGACY_WATER 1

#include "water_pass.inc"
#include "lib/legacy.glsl"

layout(location = 0) in vec4 refCoord;
layout(location = 1) in vec4 littleWave;
layout(location = 2) in vec4 view;
layout(location = 3) in vec3 vary_position;
layout(location = 0) out vec4 frag_color;

void main()
{
    // the wave maps (mipmapped: the level comes from the pixels around)
    const vec3 wave1 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], vec2(refCoord.w, view.w)).xyz * 2.0 - 1.0;
    const vec3 wave2 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], littleWave.xy).xyz * 2.0 - 1.0;
    const vec3 wave3 = texture(rk_tbl2d[RK_HM(rk_ti_bumpMap)], littleWave.zw).xyz * 2.0 - 1.0;

    if (mirror_flag > 0.0 && dot(vary_position, clipPlane.xyz) + clipPlane.w < 0.0)
    {
        discard;                // (a mirror view throws away what is behind its mirror plane)
    }

    const vec2 screen_tc = (refCoord.xy / refCoord.z) * 0.5 + 0.5;
    const float water_mask = textureLod(rk_tbl2d[RK_HM(rk_ti_exclusionTex)], screen_tc, 0.0).r;

    // the ripples push the picture below (not where an exclusion surface cuts the water out)
    const vec3 wavef = normalize(wave1 + wave2 + wave3);
    const vec2 distort = mix(screen_tc, screen_tc + wavef.xy * refScale, water_mask);

#ifdef TRANSPARENT_WATER
    const vec4 fb = textureLod(rk_tbl2d[RK_HM(rk_ti_screenTex)], distort, 0.0);
#else
    const vec4 fb = vec4(waterFogColorLinear, 0.0);
#endif

    frag_color = max(applyWaterFogViewLinearNoClip(vary_position, fb), vec4(0.0));
}
