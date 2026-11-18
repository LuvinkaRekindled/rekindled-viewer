// avatarskin.glsl: the skin of one vertex of the system avatar (the legacy body: skin, hair, eyes, eyelashes of LLViewerJointMesh) out of the joint palette uniform. The includer declares the palette
// (`vec4 matrixPalette[45]` in the RKLooseSkin block: joint i has its three rows at i, i + 15 and i + 30) and includes lib/rows.glsl first. Used by avatar.vert (the G-buffer pass) and avatar_shadow.vert.
// The weight of a vertex is one float: the joint number in the integer part, the share of the next joint in the fraction. The weights array is named by the row (all zero address: a mesh without one reads joint 0).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RowFloats { float f[]; };

// the three rows of the blended joint transform of vertex `index`
void avatar_skin(Row row, uint index, out vec4 t0, out vec4 t1, out vec4 t2)
{
    // a NaN or out of range weight reads joint 0 (the vertex data is rewritten every update); joint 14 blends with itself
    float wx = (row.weights.x != 0u || row.weights.y != 0u) ? RowFloats(row.weights).f[index] : 0.0;
    if (!(wx >= 0.0 && wx <= 14.0))
    {
        wx = 0.0;
    }
    const int i = int(floor(wx));
    const int n = min(i + 1, 14);
    const float x = fract(wx);
    t0 = matrixPalette[i];
    t1 = matrixPalette[i + 15];
    t2 = matrixPalette[i + 30];
    if (x > 0.0)
    {
        t0 = mix(t0, matrixPalette[n], x);
        t1 = mix(t1, matrixPalette[n + 15], x);
        t2 = mix(t2, matrixPalette[n + 30], x);
    }
}
