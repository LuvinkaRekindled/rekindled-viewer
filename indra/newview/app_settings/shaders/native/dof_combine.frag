// dof_combine.frag: the depth of field's last step: the blurred picture (diffuseRect, at res_scale of the size) mixed into the sharp one (lightMap) by each pixel's circle of confusion. A line for line translation of
// "Deferred DoFCombine Shader" (deferred/dofCombineF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    vec2 screen_res;
    float max_cof;
    float res_scale;
    float dof_width;
    float dof_height;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint rk_ti_diffuseRect;
    uint rk_ti_lightMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

vec4 dofSample(vec2 tc)
{
    tc.x = min(tc.x, dof_width);
    tc.y = min(tc.y, dof_height);

    return texture(rk_tbl2d[RK_HM(rk_ti_diffuseRect)], tc);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    vec4 dof = dofSample(vary_fragcoord.xy*res_scale);

    vec4 diff = texture(rk_tbl2d[RK_HM(rk_ti_lightMap)], vary_fragcoord.xy);

    float a = min(abs(diff.a*2.0-1.0) * max_cof*res_scale*res_scale, 1.0);

    if (a > 0.25 && a < 0.75)
    { //help out the transition a bit
        float sc = a/res_scale;

        vec4 col;
        col = texture(rk_tbl2d[RK_HM(rk_ti_lightMap)], vary_fragcoord.xy+vec2(sc,sc)/screen_res);
        col += texture(rk_tbl2d[RK_HM(rk_ti_lightMap)], vary_fragcoord.xy+vec2(-sc,sc)/screen_res);
        col += texture(rk_tbl2d[RK_HM(rk_ti_lightMap)], vary_fragcoord.xy+vec2(sc,-sc)/screen_res);
        col += texture(rk_tbl2d[RK_HM(rk_ti_lightMap)], vary_fragcoord.xy+vec2(-sc,-sc)/screen_res);

        diff = mix(diff, col*0.25, a);
    }

    frag_color = mix(diff, dof, a);
}
