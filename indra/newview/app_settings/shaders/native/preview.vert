// preview.vert: the mesh upload preview's model: lit by the preview's three directional lights and an ambient colour. With HAS_SKIN (the manifest line "Skinned Object Preview Shader") the vertex is skinned by the mesh's
// joint palette first (lib/objectskin.glsl). A line for line translation of "Object Preview Shader" (objects/previewV.glsl); pairs with preview.frag. (the draw block is declared the same in both stages)

#ifdef HAS_SKIN
#include "lib/objectskin.glsl"
#endif

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat3 normal_matrix;
    mat4 texture_matrix0;
    vec4 ambient_color; // <FS:Beq/> add ambient color to preview shader
    mat4 modelview_projection_matrix;
    vec4 color;
    vec4 light_position[8];
    vec3 light_diffuse[8];
#ifdef HAS_SKIN
    mat4 modelview_matrix;
    mat4 projection_matrix;
#endif
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord0;

layout(location = 0) out vec4 vertex_color;
layout(location = 1) out vec2 vary_texcoord0;

float calcDirectionalLight(vec3 n, vec3 l)
{
    float a = max(dot(n,l),0.0);
    return a;
}

void main()
{
    vec3 norm;
#ifdef HAS_SKIN
    mat4 mat = getObjectSkinnedTransform();
    mat = modelview_matrix * mat;
    vec4 pos = mat * vec4(position.xyz, 1.0);
    gl_Position = projection_matrix * pos;
    norm = normalize((mat*vec4(normal.xyz+position.xyz,1.0)).xyz-pos.xyz);
#else
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    norm = normalize(normal_matrix * normal);
#endif
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;

    vec4 col = ambient_color; // <FS:Beq/> add ambient color to preview shader

    // Collect normal lights (need to be divided by two, as we later multiply by 2)
    col.rgb += light_diffuse[1].rgb * calcDirectionalLight(norm, light_position[1].xyz);
    col.rgb += light_diffuse[2].rgb * calcDirectionalLight(norm, light_position[2].xyz);
    col.rgb += light_diffuse[3].rgb * calcDirectionalLight(norm, light_position[3].xyz);

    vertex_color = col*color;
}
