// copy.vert: a full screen triangle; the texture coordinate follows the position.
layout(location = 0) in vec3 position;
layout(location = 0) out vec2 tc;

void main()
{
    tc = position.xy * 0.5 + 0.5;
    gl_Position = vec4(position, 1.0);
}
