// opaque_snap.frag: the lit scene colour (RGBA16F) copied right before the post-deferred geometry (alpha blended surfaces, fullbright, glow) is drawn into it. The reactive mask pass compares it with
// the finished scene colour: where blended surfaces changed the pixel, the depth and the motion the upscaler holds describe the opaque thing behind, not what is seen, so the upscaler is told to
// trust its history less there (a dancer behind a vending machine's screen dragged the screen's image along, the user 2026-11-21). pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = texture(rk_tbl2d[RK_HM(rk_ti_diffuseMap)], vary_fragcoord);
}
