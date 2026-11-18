// cof.frag: the depth of field's circle of confusion: the picture with the blur radius of each pixel (from its depth and the focus) in the alpha channel. A line for line translation of "Deferred CoF Shader"
// (deferred/cofF.glsl); pairs with fullscreen.vert.

layout(std140, set = 0, binding = 0) uniform RKLooseUniforms
{
    float focal_distance;
    float blur_constant;
    float tan_pixel_angle;
    float magnification;
    float max_cof;
    mat4 inv_proj;
};

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    uint ll_ti_diffuseRect;
    uint ll_ti_depthMap;
};

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 0) out vec4 frag_color;

float calc_cof(float depth)
{
    float sc = (depth-focal_distance)/-depth*blur_constant;

    sc /= magnification;

    // tan_pixel_angle = pixel_length/-depth;
    float pixel_length =  tan_pixel_angle*-focal_distance;

    sc = sc/pixel_length;
    sc *= 1.414;

    return sc;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    float z = texture(ll_tbl2d[LL_HM(ll_ti_depthMap)], tc).r;
    z = z*2.0-1.0;
    vec4 ndc = vec4(0.0, 0.0, z, 1.0);
    vec4 p = inv_proj*ndc;
    float depth = p.z/p.w;

    vec4 diff = texture(ll_tbl2d[LL_HM(ll_ti_diffuseRect)], vary_fragcoord.xy);

    float sc = calc_cof(depth);
    sc = min(sc, max_cof);
    sc = max(sc, -max_cof);

    frag_color.rgb = diff.rgb;
    frag_color.a = sc/max_cof*0.5+0.5;
}
