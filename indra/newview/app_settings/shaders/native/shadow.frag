// shadow.frag: a shadow caster's fragment. The pass writes depth (the colour mask is off for the cascades); the program only has to have a fragment stage.

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = vec4(1.0);
}
