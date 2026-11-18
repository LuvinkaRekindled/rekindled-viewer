// debug.frag: the colour the caller set. A line for line translation of "Debug Shader" (interface/debugF.glsl); pairs with debug.vert.

layout(std140, set = 0, binding = 6) uniform RKLooseMaterial
{
    vec4 color;
};

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = max(color, vec4(0.0));
}
