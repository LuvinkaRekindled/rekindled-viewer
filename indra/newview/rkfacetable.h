/**
 * @file rkfacetable.h
 * @brief The face table: one 64-byte row per LLFace.
 *
 * doc/vulkan/PLAN-clean-core.md step C3.1 (first of the geometry rows; C3.2 added the bounds group, C3.3 the priority line). A face owns no copy of where its geometry lives or how it is drawn: it owns a row id, and LLFace's accessors read the row. The row
 * holds what the rebuild writes and the draw info creation, the picking and the tools read together: the slice of the vertex buffer (vertex count and start, index count and start), the texture slot,
 * the reference index, the state word, the colour, the texture entry offset and the bounding sphere radius. The first 64 bytes: one cache line (then the priority line and the bounds). Later steps (C3.2 ...): the public fields (extents, centres), the texture
 * handles and matrix ids, the vertex buffer slice as an arena reference: they get their own row (a second table), this one stays one cache line.
 */

#ifndef LL_LLFACETABLE_H
#define LL_LLFACETABLE_H

#include <cstddef>
#include "rkrowtable.h"
#include "v4color.h"
#include "v3math.h"
#include "v2math.h"
#include "llvector4a.h"

struct LLFaceRow
{
    // ---- the hot slice (64 bytes): what the rebuild writes and the draw info creation, the picking and the tools read together
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
    bool hasMedia;
    bool isMediaAllowed;
    bool inFrustum;                 // true if the face was recently in the main camera frustum according to LLViewerTextureList updates
    U32 drawOrderIndex;             // preserves the draw order of faces that are batched together (alpha sorting of rigged attachments)
    const void* owner;

    // ---- C3.3 the priority line (64 bytes): the texture system's state about this face (distance, times, pixel area, importance, the LLTexRecs record per channel)
    F32 distance;
    F32 lastUpdateTime;
    F32 lastSkinTime;
    F32 lastMoveTime;
    F32 lastPixelAreaUpdate;        // gFrameTimeSeconds when pixelArea was last updated
    F32 vSize;                      // virtual size of the face in texture area (pixelArea adjusted by texture repeats): the desired resolution of the texture
    F32 pixelArea;                  // pixel area the face covers on screen
    F32 importanceToCamera;         // [0, 1]: from the distance to the view point and the angle from the face center to the view direction
    U32 lastTextureUpdate;          // gFrameCount the last time the face was touched by LLViewerTextureList::updateImageDecodePriority
    U32 recIdx[7];                  // the record of this face for each texture channel in LLTexRecs (0: none); 7 = LLRender::NUM_TEXTURE_CHANNELS (static_assert in llface.cpp)

    // ---- C3.2 the bounds group (128 bytes, 16-byte aligned): the octree, the picking and the texture priority read these together
    LLVector4a extents[2];          // bounding box of the face in drawable space
    LLVector4a riggedExtents[2];    // cached bounding box of a rigged face in world space (calculated on demand by calcPixelArea, may be stale)
    LLVector3 centerLocal;
    LLVector3 centerAgent;
    LLVector2 texExtents[2];
    U8 pad2[24];
};
static_assert(sizeof(LLFaceRow) == 256, "four cache lines: the hot slice, the priority line, the bounds");
static_assert(alignof(LLFaceRow) == 16, "rows are 16 byte aligned for the LLVector4a members");
static_assert(offsetof(LLFaceRow, owner) == 56 && offsetof(LLFaceRow, distance) == 64 && offsetof(LLFaceRow, extents) == 128, "the three groups start on their lines");

using LLFaceRows = LLRowTable<LLFaceRow, (1u << 20), 10>;

#endif // LL_LLFACETABLE_H
