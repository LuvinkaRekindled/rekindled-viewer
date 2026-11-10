// highlight.frag: a flat colour. The fragment shader of the Highlight Transparent view's skinned draws (the rigged alpha masked, invisible and blended draws of the scene table): the colour is the loose
// uniform `color` of the viewer's "Highlight Pull Skin Shader" (alpha_pass.inc, HIGHLIGHT_PASS). Pairs with alpha.vert (HAS_SKIN, HAS_INDEXED_DRAW).

#include "alpha_pass.inc"

layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = color;
}
