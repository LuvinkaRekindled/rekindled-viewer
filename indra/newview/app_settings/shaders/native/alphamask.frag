// alphamask.frag: the vertex colour times the texture, thrown away under minimum_alpha (the avatar's baked texture layers). A line for line translation of "Alpha Mask Shader" (interface/alphamaskF.glsl);
// pairs with alphamask.vert.

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint ll_ti_diffuseMap;
};

layout(std140, set = 0, binding = 6) uniform RKLooseMaterial
{
    float minimum_alpha;
};

layout(location = 0) in vec4 vertex_color;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    const vec4 col = vertex_color * texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)], vary_texcoord0.xy);
    if (col.a < minimum_alpha)
    {
        discard;
    }
    frag_color = max(col, vec4(0.0));
}
