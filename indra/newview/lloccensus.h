/**
 * @file lloccensus.h
 * @brief S7 part 1 (doc/vulkan/S7-design.md sections 9-11): what the Hi-Z occlusion pass costs and finds, per frame. Off unless LL_VK_OCC_CENSUS=1
 *        (every hook is one branch on a constant). Header only; the log lines (tag OccCensus) are written by LLOccCensus::report() in llvieweroctree.cpp, every 600 world
 *        occlusion passes. Times in processor ticks, converted against the performance counter at the report.
 */
#ifndef LL_LLOCCCENSUS_H
#define LL_LLOCCCENSUS_H

#include "stdtypes.h"
#include <cstdlib>
#include "llvkplatform.h"

namespace LLOccCensus
{
    constexpr int CAMS = 9;                 // >= LLViewerCamera::NUM_CAMERAS
    constexpr int AGES = 8;                 // age of a result when applied, in frames: 0..6, 7 = older

    struct Cam
    {
        // the cull and the sort
        U64 mChkCalls = 0;                  // checkOcclusion calls
        U64 mCullEarlyFail = 0, mCullDropped = 0;
        U64 mSortGroups = 0, mSortOccluded = 0, mSortElems = 0, mSortOccludedElems = 0;
    };
    inline const bool sOn = getenv("LL_VK_OCC_CENSUS") != nullptr;
    inline Cam sCam[CAMS];

    // LLPipeline::doOcclusion
    inline U64 sPasses[CAMS] = {};                  // passes by camera
    inline U64 sListGroups[CAMS] = {};              // groups in the occlusion list
    inline U64 sListDead[CAMS] = {};
    inline U64 sListOccluded[CAMS] = {};            // ... of which are OCCLUDED right now (their boxes are still tested: the topmost occluder rule)
    inline U64 sTicksPass[CAMS] = {};               // the whole doOcclusion
    inline U64 sTicksGroups[CAMS] = {};             // the group loop
    inline U64 sTicksProbes = 0;                    // reflection + hero probe boxes
    inline U64 sTicksCache = 0;                     // LLVOCachePartition::processOccluders
    inline U64 sCachePasses = 0;
    // reflection / hero probes (LLReflectionMap::hizCollect / hizResult)
    inline U64 sProbeCalls = 0, sProbeInside = 0, sProbeAdded = 0, sProbeAwaiting = 0, sProbeVisible = 0, sProbeOccluded = 0;
    inline U64 sFrames = 0;                         // world passes since the last report
    inline long long sQpc0 = 0;
    inline U64 sTsc0 = 0;
    // the Hi-Z batches (world camera)
    inline U64 sHizSends = 0, sHizRefused = 0, sHizBoxes = 0, sHizApplied = 0, sHizOccluded = 0, sHizLost = 0, sHizSuperseded = 0, sHizDead = 0, sHizStale = 0;
    inline U64 sHizLag[AGES] = {};                  // frames from the send to the apply
    inline U64 sHizLod[16] = {};
    inline U64 sHizWhy[4] = {};                     // special cases: cut by the near plane (visible), off screen (occluded), beyond the far plane (visible), wholly behind the camera (occluded)
    inline U64 sHizTicksCollect = 0, sHizTicksSend = 0, sHizTicksApply = 0;      // main thread time of the Hi-Z pieces

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
