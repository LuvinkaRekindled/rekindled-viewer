// alpha_pbr_preview.vert: the vertex stage of the material preview's glTF blend program ("Deferred PBR Alpha Shader": the floater that edits a glTF material draws a sphere with it): the vertex buffer's own attributes
// and the draw's loose matrices, texture transforms and material factors, with the outputs of alpha_pbr.vert (alpha_pbr.frag reads them: the same fragment shader as the scene's glTF blend surfaces). A translation of
// the attribute path of "Deferred PBR Alpha Shader" (deferred/pbralphaV.glsl): the transform of a coordinate and of the tangent space is lib/gltf.glsl's (the stock texture_transform and tangent_space_transform, in
// the form the table rows of the pull program use: tr0 = scale and rotation, tr1 = offset). The textures are the draw's (rk_ti_diffuseMap ...): the slots are passed to the fragment stage as the table's rows do.
// Defines (the manifest line): ALPHA_LIT, ALPHA_PROBES, ALPHA_PBR, PBR_PREVIEW.

#include "alpha_pass.inc"
#include "lib/gltf.glsl"

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord0;
layout(location = 6) in vec4 diffuse_color;
layout(location = 8) in vec4 tangent;

layout(location = 0) out vec3 vary_position;
layout(location = 1) out vec4 vertex_color;
layout(location = 2) out vec2 vary_tc_base;
layout(location = 5) out vec2 vary_tc_emissive;
layout(location = 9) flat out uvec4 vary_slots;         // base colour, normal, occlusion roughness metallic, emissive
layout(location = 11) flat out vec3 vary_emissive;      // the emissive colour factor (linear)
layout(location = 3) out vec2 vary_tc_normal;
layout(location = 4) out vec2 vary_tc_orm;
layout(location = 6) out vec3 vary_normal;
layout(location = 7) out vec3 vary_tangent;
layout(location = 8) flat out float vary_sign;
layout(location = 10) flat out vec2 vary_factors;       // roughness, metallic

void main()
{
    //transform vertex
    vec4 vert = modelview_projection_matrix * vec4(position.xyz, 1.0);
    gl_Position = vert;
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)

    vary_position = (modelview_matrix * vec4(position.xyz, 1.0)).xyz;
    vary_tc_base = gltf_texcoord(texcoord0, texture_base_color_transform[0], texture_base_color_transform[1], texture_matrix0);
    vary_tc_normal = gltf_texcoord(texcoord0, texture_normal_transform[0], texture_normal_transform[1], texture_matrix0);
    vary_tc_orm = gltf_texcoord(texcoord0, texture_metallic_roughness_transform[0], texture_metallic_roughness_transform[1], texture_matrix0);
    vary_tc_emissive = gltf_texcoord(texcoord0, texture_emissive_transform[0], texture_emissive_transform[1], texture_matrix0);

    vec3 n = normal_matrix * normal;
    vec3 t = normal_matrix * tangent.xyz;
    n = normalize(n);
    const vec4 frame = gltf_tangent(vec4(t, tangent.w), n, texture_normal_transform[0], texture_matrix0);
    vary_tangent = normalize(frame.xyz);
    vary_sign = frame.w;
    vary_normal = n;
    vertex_color = diffuse_color;

    vary_slots = uvec4(rk_ti_diffuseMap, rk_ti_bumpMap, rk_ti_specularMap, rk_ti_emissiveMap);
    vary_emissive = emissiveColor;
    vary_factors = vec2(roughnessFactor, metallicFactor);
}
