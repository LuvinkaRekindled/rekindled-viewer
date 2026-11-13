// normal_debug.geom: a line from each vertex of the triangle to the tip of its normal (yellow) and, with HAS_ATTRIBUTE_TANGENT, to the tip of its tangent (cyan). A translation of "Normal Debug Shader"
// (interface/normaldebugG.glsl); pairs with normal_debug.vert and normal_debug.frag. The vertex stage has remapped the positions to Vulkan's clip space already. The first geometry stage of the native path.
//
// *NOTE: Geometry shaders have a reputation for being slow. Consider using compute shaders instead, which have a reputation for being fast. This geometry shader in particular seems to run fine on my machine,
// but I won't vouch for this in performance-critical areas. -Cosmic, 2023-09-28 (the old shader's note)

layout(location = 0) in vec4 normal_g[];
#ifdef HAS_ATTRIBUTE_TANGENT
layout(location = 1) in vec4 tangent_g[];
#endif

layout(location = 0) out vec4 vertex_color;

layout(triangles) in;
#ifdef HAS_ATTRIBUTE_TANGENT
layout(line_strip, max_vertices = 12) out;
#else
layout(line_strip, max_vertices = 6) out;
#endif

void triangle_normal_debug(int i)
{
    // Normal
    vec4 normal_color = vec4(1.0, 1.0, 0.0, 1.0);
    gl_Position = gl_in[i].gl_Position;
    vertex_color = normal_color;
    EmitVertex();
    gl_Position = normal_g[i];
    vertex_color = normal_color;
    EmitVertex();
    EndPrimitive();

#ifdef HAS_ATTRIBUTE_TANGENT
    // Tangent
    vec4 tangent_color = vec4(0.0, 1.0, 1.0, 1.0);
    gl_Position = gl_in[i].gl_Position;
    vertex_color = tangent_color;
    EmitVertex();
    gl_Position = tangent_g[i];
    vertex_color = tangent_color;
    EmitVertex();
    EndPrimitive();
#endif
}

void main()
{
    triangle_normal_debug(0);
    triangle_normal_debug(1);
    triangle_normal_debug(2);
}
