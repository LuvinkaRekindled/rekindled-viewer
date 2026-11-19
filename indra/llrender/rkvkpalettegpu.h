/**
 * @file rkvkpalettegpu.h
 * @brief The layouts of the two records that carry what the GPU needs to make a joint palette itself (PLAN-avatars-100.md D3, stage 3a).
 *
 * A palette is `invBind[j] x world[jointNum[j]]` for every joint j of a skin, packed as 12 floats a joint (the rows of the 3x4), after which a tail entry names the avatar relative anchor. Today the
 * viewer's palette jobs make it on the CPU and write it into the frame's palette arena. These records give the render worker the inputs of the same arithmetic:
 *
 *  - kind 44, one skin block, sent once for a skin (and again when its hash changes): header mFragment = the skin's render handle, mPass = the low 32 bits of the skin's hash, mEntries = the joint
 *    count. Blob: mEntries x 16 floats (the inverse bind matrices, the memory order of LLMatrix4a), then mEntries x S32 (the joint numbers). The worker keeps it in a mapped device buffer until the
 *    handle no longer resolves.
 *  - kind 45, one frame's build, sent after the palette jobs of the batch: header mPass = the viewer frame whose arena the palettes are written into, mEntries = the number of BuildEntry,
 *    mMatrices = the number of AvatarRec, mPaletteFloats = the floats of the world area. Blob: FrameHeader, BuildEntry[mEntries], AvatarRec[mMatrices], then the world area: per avatar the world
 *    matrices by joint number (kJoints x 16 floats) and, when the palette is avatar relative, the relative world matrices (kJoints x 16 floats).
 */

#ifndef RK_RKVKPALETTEGPU_H
#define RK_RKVKPALETTEGPU_H

#include "stdtypes.h"

namespace RKVkPaletteGpu
{
constexpr U32 kKindSkin = 44;
constexpr U32 kKindBuild = 45;
constexpr U32 kJoints = 216;                // LL_CHARACTER_MAX_ANIMATED_JOINTS
constexpr U32 kNone = 0xFFFFFFFFu;

struct FrameHeader
{
    F32 mAnchor[3];                         // the avatar relative anchor of the frame (rk_rig_anchor)
    U32 mAnchorActive;                      // 1: the palettes of this frame are relative to it
    U32 mEntries;
    U32 mAvatars;
    U32 mPad[2];
};
static_assert(sizeof(FrameHeader) == 32, "layout");

struct BuildEntry
{
    U32 mSkin;                              // the skin's render handle
    U32 mSkinHash;                          // low 32 bits of the skin's hash (with the handle, the key of the skin block)
    U32 mAvatar;                            // index of the AvatarRec
    U32 mCount;                             // joints of the skin
    U32 mOutFloat;                          // float offset of this palette in the arena of the frame (kNone: it was not written into an arena)
    U32 mPrevFrame;                         // the viewer frame whose arena holds this entry's palette of the frame before (kNone: no history)
    U32 mPrevFloat;                         // its float offset there
    U32 mRelative;                          // 1: made from the avatar's relative world matrices
};
static_assert(sizeof(BuildEntry) == 32, "layout");

struct AvatarRec
{
    U32 mWorldFloat;                        // float offset of the avatar's world matrices in the world area
    U32 mRelFloat;                          // float offset of its relative world matrices (kNone: none)
};
static_assert(sizeof(AvatarRec) == 8, "layout");
}

#endif // RK_RKVKPALETTEGPU_H
