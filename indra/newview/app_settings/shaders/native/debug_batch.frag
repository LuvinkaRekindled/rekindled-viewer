// debug_batch.frag: the Batch Size and Texture Animation debug views of the scene table. Pairs with alpha.vert (DEBUG_ROW, HIGHLIGHT_PASS; HAS_SKIN and HAS_INDEXED_DRAW for the rigged draws).
// The loose uniform `color` (alpha_pass.inc) selects the view: color.r above 0.5 = Texture Animation (yellow at half alpha on the draws that have a texture matrix, nothing on the others),
// else Batch Size: the colour of the draw that the viewer made when it built the draw's entry (LLDrawInfo::getDebugColor, the colour the old view gave the draw), read from the table row.

#include "alpha_pass.inc"

layout(location = 5) flat in uint vary_debug_color;
layout(location = 6) flat in uint vary_debug_texmat;

layout(location = 0) out vec4 frag_color;

void main()
{
    if (color.r > 0.5)
    {
        if (vary_debug_texmat == 0u)
        {
            discard;
        }
        frag_color = vec4(1.0, 1.0, 0.0, 0.5);
    }
    else
    {
        frag_color = unpackUnorm4x8(vary_debug_color);
    }
}
