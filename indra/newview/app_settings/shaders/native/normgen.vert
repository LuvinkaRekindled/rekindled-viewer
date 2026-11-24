// normgen.vert: a quad over the whole target for the generation of a normal map from a picture (the bump map's emboss "brightness" / "darkness" technique, LLBumpImageList). A line for line translation of "Normal
// Map Generation Program" (deferred/normgenV.glsl); pairs with normgen.frag.

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 0) out vec2 vary_texcoord0;

void main()
{
    // z 0: a quad over the whole target at a depth inside Vulkan's clip volume (0 to w). The old text put -1 here and remapped it with RK_CLIP_Z; since the reversed depth that macro takes the z as it is, and -1 is
    // outside the volume: the quad was clipped away and no normal map was drawn (found 2026-11-24 with the same defect in radiance.vert)
    gl_Position = vec4(position.x * 2.0 - 1.0, position.y * 2.0 - 1.0, 0.0, 1.0);
    vary_texcoord0 = texcoord0;
}
