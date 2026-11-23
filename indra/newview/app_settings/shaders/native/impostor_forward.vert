// impostor_forward.vert: a textured shape with its own coordinates (the avatar preview's impostor pass: the old forward pass of LLDrawPoolAvatar, used by the preview renders of the appearance editor hints and the
// animation / image preview floaters). A line for line translation of "Impostor Shader" (objects/impostorV.glsl); pairs with impostor_forward.frag. (the draw block is declared the same in both stages)

layout(std140, set = 0, binding = 5) uniform RKLooseDraw
{
    mat4 texture_matrix0;
    mat4 modelview_projection_matrix;
    uint rk_ti_diffuseMap;
};

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;

layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    //transform vertex
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0, 0, 1)).xy;
}
