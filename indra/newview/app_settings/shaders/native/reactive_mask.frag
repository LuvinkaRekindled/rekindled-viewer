// reactive_mask.frag: the reactive mask the upscaler (FSR) takes: 0 where the scene is what the depth and the motion vectors say, up to 1 where post-deferred geometry (alpha blended surfaces:
// a vending machine's screen, glass, hair layers) changed the pixel after the opaque pass. The opaque colour (opaque_snap.frag, taken before those draws) and the finished scene colour are both
// linear HDR before the tone map, so the change is judged against the brighter of the two (a relative change: a dim surface over a dim one counts as much as a bright one over a bright one).
// pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float react_low;        // the relative change that starts to count (default 0.03)
    float react_high;       // the relative change that counts fully (default 0.25)
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;      // the scene colour after the post-deferred geometry
    uint rk_ti_emissiveRect;    // the opaque colour (opaque_snap.frag)
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec3 now = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_fragcoord).rgb;
    vec3 before = texture(rk_tbl2d[RK_HM(rk_ti_emissiveRect)], vary_fragcoord).rgb;
    float change = max(abs(now.r - before.r), max(abs(now.g - before.g), abs(now.b - before.b)));
    float level = max(max(now.r, max(now.g, now.b)), max(before.r, max(before.g, before.b)));
    float rel = change / (level + 0.02);
    float mask = clamp((rel - react_low) / max(react_high - react_low, 1.0e-4), 0.0, 1.0);
    frag_color = vec4(mask, 0.0, 0.0, 1.0);
}
