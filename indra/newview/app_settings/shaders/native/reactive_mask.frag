// reactive_mask.frag: the reactive mask the upscaler (FSR) takes: 0 where the scene is what the depth and the motion vectors say, up to 1 where post-deferred geometry (alpha blended surfaces:
// a vending machine's screen, glass, hair layers) changed the pixel after the opaque pass. The opaque colour (opaque_snap.frag, taken before those draws) and the finished scene colour are both
// linear HDR before the tone map, so the change is judged against the brighter of the two (a relative change: a dim surface over a dim one counts as much as a bright one over a bright one).
// pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float react_low;        // the relative change that starts to count (default 0.03)
    float react_high;       // the relative change that counts fully (default 0.25)
    float use_move;         // 1: the specularRect is the world pass's motion target (the upscaler rig's fsrmovreact): a pixel with motion of its own gets at least move_react
    float move_react;
    float gain;             // a factor on the finished mask (the upscaler rig's fsrgain=; 1 normally, 2 from the coverage)
    float use_cover;        // 1: the emissiveRect is the blended pass's coverage (attachment 1 of the screen target, lib/cover.glsl) and the mask is made from it: the share of the pixel the blended surfaces cover, by class
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;      // the scene colour after the post-deferred geometry
    uint rk_ti_emissiveRect;    // the opaque colour (opaque_snap.frag)
    uint rk_ti_specularRect;    // the world pass's motion target (RG16 unorm, x = 1.0: no motion of its own), read when use_move is set
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    float mask;
    if (use_cover > 0.5)
    {
        mask = texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).r;
    }
    else
    {
        vec3 now = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_fragcoord).rgb;
        vec3 before = texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).rgb;
        float change = max(abs(now.r - before.r), max(abs(now.g - before.g), abs(now.b - before.b)));
        float level = max(max(now.r, max(now.g, now.b)), max(before.r, max(before.g, before.b)));
        float rel = change / (level + 0.02);
        mask = clamp((rel - react_low) / max(react_high - react_low, 1.0e-4), 0.0, 1.0);
    }
    if (use_move > 0.5)
    {
        const vec2 own = texture(rk_tbl2d[RK_HM(rk_ti_specularRect)], vary_fragcoord).rg;
        if (own.x < 0.99995)
        {
            mask = max(mask, move_react);
        }
    }
    frag_color = vec4(min(mask * gain, 1.0), 0.0, 0.0, 1.0);
}
