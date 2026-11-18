// preview_physics.frag: the physics preview's texture times a colour. A line for line translation of "Preview Physics Shader" (objects/previewPhysicsF.glsl); pairs with preview_physics.vert.
// (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    vec4 color;
    uint ll_ti_diffuseMap;
};

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = max(texture(ll_tbl2d[LL_HM(ll_ti_diffuseMap)],vary_texcoord0.xy) * color, vec4(0));
}
