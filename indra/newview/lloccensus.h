/**
 * @file lloccensus.h
 * @brief S7 part 1, stage H0 (doc/vulkan/S7-design.md section 9): what the occlusion query machinery costs and what it finds, per frame. Off unless LL_VK_OCC_CENSUS=1
 *        (every hook is one branch on a constant). Header only; the log line (tag OccCensus) is written by LLOccCensus::report() in llvieweroctree.cpp, every 600 world
 *        occlusion passes. Counted per camera (world, shadow cascades, ...); times in processor ticks, converted against the performance counter at the report.
 */
#ifndef LL_LLOCCCENSUS_H
#define LL_LLOCCCENSUS_H

#include "stdtypes.h"
#include <cstdlib>
#include <intrin.h>

namespace LLOccCensus
{
    constexpr int CAMS = 8;                 // >= LLViewerCamera::NUM_CAMERAS
    constexpr int AGES = 8;                 // age of a query result at the read, in frames: 0..6, 7 = older

    struct Cam
    {
        // group->doOcclusion() (the query issue)
        U64 mDoCalls = 0, mDoEarlyFail = 0, mDoSkipPending = 0, mDoIssued = 0, mDoWater = 0, mDoVoCache = 0, mDoDiscardReissue = 0;
        // group->checkOcclusion() (the read)
        U64 mChkCalls = 0, mChkParentOccluded = 0, mChkPending = 0, mChkDiscard = 0, mChkAvailable = 0, mChkForced = 0, mChkNotReady = 0, mChkVisible = 0, mChkOccluded = 0;
        U64 mAge[AGES] = {};                // available reads by age
        U64 mAgeForced[AGES] = {};          // timeout reads by age
        U64 mChkTicks = 0;                  // time spent in the pending branch (the read, state propagation)
        // the cull and the sort
        U64 mCullEarlyFail = 0, mCullDropped = 0;
        U64 mSortGroups = 0, mSortOccluded = 0, mSortElems = 0, mSortOccludedElems = 0;
    };
    inline const bool sOn = getenv("LL_VK_OCC_CENSUS") != nullptr;
    inline Cam sCam[CAMS];

    // LLPipeline::doOcclusion
    inline U64 sPasses[CAMS] = {};                  // passes by camera
    inline U64 sListGroups[CAMS] = {};              // groups in the occlusion list
    inline U64 sListDead[CAMS] = {};
    inline U64 sListOccluded[CAMS] = {};            // ... of which are OCCLUDED right now (their queries are still issued: the topmost occluder rule)
    inline U64 sTicksPass[CAMS] = {};               // the whole doOcclusion
    inline U64 sTicksGroups[CAMS] = {};             // the group loop
    inline U64 sTicksProbes = 0;                    // reflection + hero probe occlusion
    inline U64 sTicksCache = 0;                     // LLVOCachePartition::processOccluders
    inline U64 sCachePasses = 0;
    // reflection / hero probes (LLReflectionMap::doOcclusion)
    inline U64 sProbeCalls = 0, sProbeInside = 0, sProbeNew = 0, sProbeReady = 0, sProbePending = 0, sProbeVisible = 0, sProbeOccluded = 0, sProbeIssued = 0;
    inline U64 sFrames = 0;                         // world passes since the last report
    inline long long sQpc0 = 0;
    inline U64 sTsc0 = 0;

    inline U64 now() { return __rdtsc(); }
    inline Cam* cam(int id) { return sOn && id >= 0 && id < CAMS ? &sCam[id] : nullptr; }
    inline int age_slot(U32 now_frame, U32 issued)
    {
        const U32 a = now_frame - issued;
        return a >= (U32)AGES ? AGES - 1 : (int)a;
    }

    inline void sortNote(int id, bool occluded, U32 elements)       // a group the sort loops of LLPipeline::stateSort looked at
    {
        if (Cam* c = cam(id))
        {
            ++c->mSortGroups;
            c->mSortElems += elements;
            c->mSortOccluded += occluded;
            c->mSortOccludedElems += occluded ? elements : 0;
        }
    }

    void report();                                  // llvieweroctree.cpp
}

#endif // LL_LLOCCCENSUS_H
