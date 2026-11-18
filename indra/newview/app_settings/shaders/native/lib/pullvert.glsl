// lib/pullvert.glsl: the first lines of every pull vertex shader of a translated program (what the viewer's RK_VERTEX_PULL rewrite puts at the start of main, llshadermgr.cpp pull_prologue): the index of the
// vertex, read from the index array unless the row says the vertex number is the index. Needs rows.glsl and the table addresses of the includer.
uint pull_index(Row row)
{
    const uint at = row.first + uint(gl_VertexIndex);
    uint index = at;
    if ((row.flags & 4u) == 0u)
    {
        const uint word = RowWords(row.indices).w[at >> 1];
        index = (at & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu);
    }
    return index;
}
