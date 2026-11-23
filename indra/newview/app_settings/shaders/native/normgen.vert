// normgen.vert: a quad over the whole target for the generation of a normal map from a picture (the bump map's emboss "brightness" / "darkness" technique, LLBumpImageList). A line for line translation of "Normal
// Map Generation Program" (deferred/normgenV.glsl); pairs with normgen.frag.

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    gl_Position = vec4(position.x * 2.0 - 1.0, position.y * 2.0 - 1.0, -1.0, 1.0);
    RK_CLIP_Z(gl_Position);      // (GL clip space to Vulkan's, as the old programs get it appended)
    vary_texcoord0 = texcoord0;
}
