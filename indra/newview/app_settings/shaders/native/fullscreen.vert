// fullscreen.vert: the full screen triangle every screen-space pass uses. The texture coordinate follows the position.
layout(location = 0) in vec3 position;
layout(location = 0) out vec2 vary_fragcoord;

void main()
{
    gl_Position = vec4(position, 1.0);
    vary_fragcoord = position.xy * 0.5 + 0.5;
}
