/**
 * @file llattcensus.h
 * @brief Stage 0 of doc/vulkan/ATTACHMENTS-design.md: how much of the main thread's per-frame move chain (drawable and bridge updateMove, octree partition moves, the moved list) belongs to the
 *        attachments of avatars, split into rigid attachments, rigged attachments and everything else, plus why rigid attachments would not be "plain" for a bulk matrix pass.
 *        Measurement only: nothing here changes what is updated. Off unless LL_VK_ATTCENSUS=1 (every hook is one branch on a constant). Log tag AttCensus every 600 frames
 *        (LLAttCensus::frame() is called from LLViewerObjectList::update). Times are rdtsc ticks converted against the performance counter at the report.
 */
#ifndef LL_LLATTCENSUS_H
#define LL_LLATTCENSUS_H

#include "stdtypes.h"
#include <cstdlib>
#include "llvkplatform.h"

class LLDrawable;
class LLViewerObject;

namespace LLAttCensus
{
    inline const bool sOn = getenv("LL_VK_ATTCENSUS") != nullptr;

    enum Kind { OTHER = 0, RIGID = 1, RIGGED = 2, NKIND = 3 };

    struct Cell
    {
        U64 mCalls = 0, mTicks = 0;
    };

    inline U64 now() { return __rdtsc(); }

    // the chain, by kind of the drawable (inclusive times: a drawable's updateMove contains its partition move)
    inline Cell sDrawableMove[NKIND], sBridgeMove[NKIND], sPartitionMove[NKIND];
    inline U64 sPartitionLeftGroup[NKIND] = {};         // partition moves that did not stay in their group (removed and put again)
    inline U64 sMovedListIter[NKIND] = {}, sMovedListEarly[NKIND] = {};     // moved list entries visited / found already moved this frame
    inline U64 sMovedListByUpdate[NKIND] = {};           // ... of which updateMove was called by the list itself

    // rigid attachments with a bridge, as seen by the attachment loop of idleUpdateMisc: why one would not be plain for a bulk matrix pass
    enum Reason
    {
        R_SEEN = 0, R_SELECTED, R_FLEXIBLE, R_ANIMATED, R_LIGHT, R_SOUND, R_PARTICLES, R_LINKSET, R_HUD, R_RECENT_SERVER, R_NOT_PLAIN, R_PLAIN, R_CHILDREN, R_COUNT
    };
    inline U64 sReason[R_COUNT] = {};
    inline U64 sAvatarsHidden = 0, sAttachmentsOnHidden = 0, sAvatarsLooped = 0;    // avatars the loop skipped (not a detailed update) and the attachments they wear

    inline U32 sFrame0 = 0;
    inline long long sQpc0 = 0;
    inline U64 sTsc0 = 0;

    Kind kindOf(const LLDrawable* drawable);

    struct Scope
    {
        Cell* mCell = nullptr;
        U64 mT0 = 0;
        Scope(Cell (&cells)[NKIND], const LLDrawable* drawable)
        {
            if (sOn)
            {
                mCell = &cells[kindOf(drawable)];
                mT0 = now();
            }
        }
        ~Scope()
        {
            if (mCell)
            {
                mCell->mTicks += now() - mT0;
                ++mCell->mCalls;
            }
        }
    };

    void classify(const LLViewerObject* attached, const LLDrawable* drawable);   // one rigid attachment: the reason counters
    void frame();                           // llviewerobjectlist.cpp, once a frame: the report every 600 frames
}

#endif // LL_LLATTCENSUS_H
