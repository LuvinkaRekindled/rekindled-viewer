// terrain_bake.vert: the vertex stage of the PBR terrain's paint bake: the patch's vertices with the coordinates the alpha ramp is sampled at. A line for line translation of "Terrain Bake Shader"
// (interface/pbrTerrainBakeV.glsl); pairs with terrain_bake.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 modelview_projection_matrix;
    uint rk_ti_alpha_ramp;
};

layout(location = 0) in vec3 position;
layout(location = 3) in vec2 texcoord1;
layout(location = 0) out vec4 vary_texcoord0;
layout(location = 1) out vec4 vary_texcoord1;

void main()
{
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;      // (GL clip space to Vulkan's, as the old programs get it appended)

    vec2 tc = texcoord1.xy;

    vary_texcoord0 = vec4(0.0);
    vary_texcoord0.zw = tc.xy;
    vary_texcoord1.xy = tc.xy-vec2(2.0, 0.0);
    vary_texcoord1.zw = tc.xy-vec2(1.0, 0.0);
}
