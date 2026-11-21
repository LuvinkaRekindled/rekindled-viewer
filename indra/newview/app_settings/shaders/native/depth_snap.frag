// depth_snap.frag: the depth of the world pass as a colour texture (R32F), copied right after the world pass and before the post-deferred geometry (fullbright and the like) is drawn over it. The motion pass
// compares it with the final depth: a pixel whose depth is nearer in the end was overdrawn after the world pass, so the motion the world pass left there (a dancer behind a fullbright wall) is stale there.
// pairs with fullscreen.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = vec4(texture(rk_tbl2d[RK_HM(rk_ti_depthMap)], vary_fragcoord).r, 0.0, 0.0, 1.0);
}
