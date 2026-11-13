// objectskin.glsl: the skin of a rigged mesh object drawn from its own vertex buffer (the attribute fed draws of the debug views: the skinned highlight, the skinned flat colour of the debug draws). A line for line
// translation of the viewer's avatar/objectSkinV.glsl (getObjectSkinnedTransform): the vertex's four joints (`weight4`: the joint in the integer part, the share in the fraction) blend the joint palette, the
// result is the transform of the mesh's skin in eye space (the viewer sends the palette as 3x4 matrices, uploadMatrixPalette). The palette is the LLLooseSkin block: joint j has its three columns (each a vec4:
// the matrix column and one component of the translation in w) at 3 * j, 3 * j + 1, 3 * j + 2 (the viewer's matrix upload puts a 3x4 matrix in 48 bytes: LL_MAX_JOINTS_PER_MESH_OBJECT = 110 of them).
// The includer needs no declarations: this file declares the attribute and the block.

layout(location = 10) in vec4 weight4;

layout(std140, set = 0, binding = 8) uniform LLLooseSkin
{
    vec4 matrixPalette[330];
};

mat4 getObjectSkinnedTransform()
{
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    index = min(index, vec4(109.0));
    index = max(index, vec4(0.0));

    w *= 1.0 / (w.x + w.y + w.z + w.w);

    const int i1 = int(index.x) * 3;
    const int i2 = int(index.y) * 3;
    const int i3 = int(index.z) * 3;
    const int i4 = int(index.w) * 3;

    // (a joint with no weight is not read: most vertices have one to three; a zero weight adds nothing)
    mat3 mat = mat3(matrixPalette[i1].xyz, matrixPalette[i1 + 1].xyz, matrixPalette[i1 + 2].xyz) * w.x;
    vec3 trans = vec3(matrixPalette[i1].w, matrixPalette[i1 + 1].w, matrixPalette[i1 + 2].w) * w.x;
    if (w.y > 0.0)
    {
        mat += mat3(matrixPalette[i2].xyz, matrixPalette[i2 + 1].xyz, matrixPalette[i2 + 2].xyz) * w.y;
        trans += vec3(matrixPalette[i2].w, matrixPalette[i2 + 1].w, matrixPalette[i2 + 2].w) * w.y;
    }
    if (w.z > 0.0)
    {
        mat += mat3(matrixPalette[i3].xyz, matrixPalette[i3 + 1].xyz, matrixPalette[i3 + 2].xyz) * w.z;
        trans += vec3(matrixPalette[i3].w, matrixPalette[i3 + 1].w, matrixPalette[i3 + 2].w) * w.z;
    }
    if (w.w > 0.0)
    {
        mat += mat3(matrixPalette[i4].xyz, matrixPalette[i4 + 1].xyz, matrixPalette[i4 + 2].xyz) * w.w;
        trans += vec3(matrixPalette[i4].w, matrixPalette[i4 + 1].w, matrixPalette[i4 + 2].w) * w.w;
    }
    mat4 ret;

    ret[0] = vec4(mat[0], 0.0);
    ret[1] = vec4(mat[1], 0.0);
    ret[2] = vec4(mat[2], 0.0);
    ret[3] = vec4(trans, 1.0);

    return ret;
}
