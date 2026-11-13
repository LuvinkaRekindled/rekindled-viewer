// exposure.frag: the eye's adaptation: one value (a pixel of the exposure target) from the average luminance of the picture (the last mip of the emissive target, the one the luminance pass writes into), mapped
// by the dynamic exposure settings, and (USE_LAST_EXPOSURE) moved from the previous value towards it at the adaptation speed. A line for line translation of "Exposure" (with USE_LAST_EXPOSURE) and
// "Exposure (no fade)" (deferred/exposureF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    float dt;
    vec4 dynamic_exposure_params;
    vec4 dynamic_exposure_params2;
};

layout(std140, set = 0, binding = 5) uniform LLLooseDraw
{
    uint ll_ti_emissiveRect;
#ifdef USE_LAST_EXPOSURE
    uint ll_ti_exposureMap;
#endif
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float L = textureLod(ll_tbl2d[LL_HM(ll_ti_emissiveRect)], vec2(0.5), 8.0).r;
    const float max_L = dynamic_exposure_params.x;
    L = clamp(L, 0.0, max_L);
    L /= max_L;
    L = L * L;
    float s = mix(dynamic_exposure_params.z, dynamic_exposure_params.y, L);
#ifdef USE_LAST_EXPOSURE
    const float prev = textureLod(ll_tbl2d[LL_HM(ll_ti_exposureMap)], vec2(0.5), 0.0).r;
    const float speed = -log(dynamic_exposure_params.w) / dynamic_exposure_params2.w;
    s = mix(prev, s, 1.0 - exp(-speed * dt));
#endif
    frag_color = max(vec4(s, s, s, dt), vec4(0.0));
}
