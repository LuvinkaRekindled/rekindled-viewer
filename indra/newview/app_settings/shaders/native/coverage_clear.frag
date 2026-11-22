// coverage_clear.frag: sets the coverage attachment (attachment 1 of the screen target, R8) to 0 at the start of the post-deferred pass. The program has one output, location 1, so the renderer writes nothing else (the
// colour write mask follows a program's outputs). Pairs with fullscreen.vert.

layout(location = 0) in vec2 vary_fragcoord;
layout(location = 1) out vec4 frag_cover;

void main()
{
    frag_cover = vec4(0.0);
}
