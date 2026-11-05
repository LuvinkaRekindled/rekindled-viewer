/**
 * @file llfacetable.h
 * @brief The face table: one 64-byte row per LLFace.
 *
 * doc/vulkan/PLAN-clean-core.md step C3.1 (first of the geometry rows). A face owns no copy of where its geometry lives or how it is drawn: it owns a row id, and LLFace's accessors read the row. The row
 * holds what the rebuild writes and the draw info creation, the picking and the tools read together: the slice of the vertex buffer (vertex count and start, index count and start), the texture slot,
 * the reference index, the state word, the colour, the texture entry offset and the bounding sphere radius. 64 bytes: one cache line. Later steps (C3.2 ...): the public fields (extents, centres), the texture
 * handles and matrix ids, the vertex buffer slice as an arena reference: they get their own row (a second table), this one stays one cache line.
 */

#ifndef LL_LLFACETABLE_H
#define LL_LLFACETABLE_H

#include "llrowtable.h"
#include "v4color.h"

struct LLFaceRow
{
    LLColor4 faceColor;             // overrides material color if state has USE_FACE_COLOR
    U32 indicesCount;
    U32 indicesIndex;               // index into the vertex buffer's index array (0xFFFFFFFF: uninitialized position)
    U32 state;                      // the face state word (also the canary of the table)
    S32 referenceIndex;
    S32 teOffset;                   // which texture entry
    F32 boundingSphereRadius;
    U16 geomCount;                  // vertex count for this face
    U16 geomIndex;                  // starting index into the vertex buffer's vertex array
    U8  textureIndex;               // index of texture channel to use for pseudo-atlasing
    U8  pad[7];
    const void* owner;
};
static_assert(sizeof(LLFaceRow) == 64, "one cache line");

using LLFaceRows = LLRowTable<LLFaceRow, (1u << 20), 10>;

#endif // LL_LLFACETABLE_H
