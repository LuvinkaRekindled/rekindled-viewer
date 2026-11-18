// glow.frag: the blur step of the glow chain: the eight taps weighted by the kernel, times glowStrength. A line for line translation of "Glow Shader (Post)" (effects/glowF.glsl); pairs with glow.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec2 glowDelta;
    float glowStrength;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec4 vary_texcoord0;
layout(location = 1) in vec4 vary_texcoord1;
layout(location = 2) in vec4 vary_texcoord2;
layout(location = 3) in vec4 vary_texcoord3;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 col = vec4(0.0, 0.0, 0.0, 0.0);

    float kern[8];
    kern[0] = 0.25; kern[1] = 0.5; kern[2] = 0.8; kern[3] = 1.0;
    kern[4] = 1.0;  kern[5] = 0.8; kern[6] = 0.5; kern[7] = 0.25;

    col += kern[0] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
    col += kern[1] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord1.xy);
    col += kern[2] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord2.xy);
    col += kern[3] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord3.xy);
    col += kern[4] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.zw);
    col += kern[5] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord1.zw);
    col += kern[6] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord2.zw);
    col += kern[7] * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord3.zw);

    frag_color = max(vec4(col.rgb * glowStrength, col.a), vec4(0));
}
