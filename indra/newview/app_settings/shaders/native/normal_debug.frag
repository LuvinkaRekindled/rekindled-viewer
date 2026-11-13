// normal_debug.frag: the colour the geometry stage gave the line. A line for line translation of "Normal Debug Shader" (interface/normaldebugF.glsl); pairs with normal_debug.vert and normal_debug.geom.

layout(location = 0) in vec4 vertex_color;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = max(vertex_color, vec4(0));
}
