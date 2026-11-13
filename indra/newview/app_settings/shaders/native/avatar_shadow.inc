// avatar_shadow.inc: the loose uniform block of the system avatar's shadow programs (avatar_shadow.vert and avatar_shadow.frag include this file, so both stages declare the same block). Every name and type is
// the one the viewer's avatar shadow programs use. Defines of the manifest line: SHADOW_ALPHA (the alpha tested casters), SHADOW_COLOR_ALPHA (the draw's colour alpha counts too).

layout(std140, set = 0, binding = 0) uniform LLLooseUniforms
{
    uvec2 ll_packet_table;      // the rows of the record (written by the draw record)
    mat4 projection_matrix;     // the shadow camera's projection (the palette matrices are in eye space)
#ifdef SHADOW_ALPHA
    float minimum_alpha;        // below it a texel casts only every other pixel column
#ifdef SHADOW_COLOR_ALPHA
    vec4 color;                 // the draw's colour
#endif
#endif
};
