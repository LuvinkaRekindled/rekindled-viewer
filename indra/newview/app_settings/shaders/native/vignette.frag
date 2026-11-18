// vignette.frag: the vignette of the post effects: the picture darkened towards the corners. A line for line translation of "Vignette Post" (post/exoVignetteF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec3 vignette;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseRect;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

void main ()
{
    vec4 diff = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord.xy);
    vec2 tc = vary_fragcoord - 0.5f;
    float vignette_val = 1 - dot(tc, tc);
    diff.rgb *= clamp(pow(mix(1, vignette_val * vignette_val * vignette_val * vignette_val * vignette.z, vignette.x), vignette.y), 0, 1);
    frag_color = diff;
}
