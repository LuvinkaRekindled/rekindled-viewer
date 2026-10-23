/**
 * @file llviscensus.h
 * @brief S7 B0 / C0 (doc/vulkan/S7-design.md section 13): where the main thread's visibility work goes (cull, state sort, post sort), which pass types the scene table serves and which the
 *        CPU pools draw, and what a fragment cull over the table items would drop (frustum on the CPU, Hi-Z in the worker). Off unless LL_VK_VISCENSUS=1 (every hook is one branch on a
 *        constant); LL_VK_VISCENSUS_BOXES=0 leaves the box test out. Header only; the log lines (tag VisCensus) are written by LLVisCensus::report() in pipeline.cpp, every 600 world
 *        post sorts. Times in processor ticks (rdtsc), converted against the performance counter at the report. Measurement only: nothing here changes what is drawn.
 */
#ifndef LL_LLVISCENSUS_H
#define LL_LLVISCENSUS_H

#include "stdtypes.h"
#include <cstdlib>
#include <intrin.h>

namespace LLVisCensus
{
    inline const bool sOn = getenv("LL_VK_VISCENSUS") != nullptr;
    inline const bool sBoxes = sOn && !(getenv("LL_VK_VISCENSUS_BOXES") && getenv("LL_VK_VISCENSUS_BOXES")[0] == '0');

    constexpr int PARTS = 16;               // >= LLViewerRegion::NUM_PARTITIONS
    constexpr int PASSES = 80;              // >= the number of PASS_ types
    constexpr int POOLS = 24;               // >= the pool types
    constexpr int ITEM_TYPES = 48;          // table item types: 0-19 static, 20-39 rigged, 40 alpha, 41 rigged alpha

    inline U64 now() { return __rdtsc(); }

    // updateCull (world camera, the main view)
    inline U64 sCullFrames = 0, sCullTicks = 0, sCullTicksPart[PARTS] = {}, sCullTicksCache = 0, sCullTicksSky = 0;
    inline U64 sCullNodes = 0, sCullFrustumChecks = 0, sCullFullIn = 0, sCullMarked = 0;
    // stateSort (world camera, the main view)
    inline U64 sSortCalls = 0, sSortTicks = 0, sSortTicksCheck = 0, sSortCheckCalls = 0;
    inline U64 sSortTicksMarkVis = 0, sSortMarkVisN = 0, sSortTicksLod = 0, sSortLodCalls = 0, sSortLodTrue = 0;
    inline U64 sSortTicksDrawable = 0, sSortDrawableCalls = 0, sSortTicksDist = 0, sSortDistCalls = 0, sSortTicksEnqueue = 0, sSortEnqueueFaces = 0;
    inline U64 sSortTicksBridge = 0, sSortBridgeCalls = 0;
    inline U64 sSortDrawableGroups = 0, sSortVisibleGroups = 0, sSortBridges = 0, sSortDrawables = 0, sSortOccluded = 0;
    // postSort (world camera, the main view)
    inline U64 sPostCalls = 0, sPostTicks = 0;
    inline U64 sPostTicksRebuildGeom = 0, sPostTicksMap = 0, sPostTicksMesh = 0, sPostTicksAlpha = 0;
    // visible groups by partition and what the table holds of them
    inline U64 sPartGroups[PARTS] = {}, sPartElems[PARTS] = {}, sPartWithTable[PARTS] = {}, sPartAllServed[PARTS] = {}, sPartInfos[PARTS] = {}, sPartTris[PARTS] = {};
    // draw infos of the visible groups by pass type: served by the table (not listed in the render map) and listed (drawn by the pool loops, through the typed records)
    inline U64 sPassServedInfos[PASSES] = {}, sPassServedTris[PASSES] = {}, sPassListedInfos[PASSES] = {}, sPassListedTris[PASSES] = {};
    inline U64 sAlphaGroups = 0, sAlphaServedItems = 0, sAlphaItemsTotal = 0, sRiggedAlphaGroups = 0;
    // faces of drawables without a volume (terrain, water, trees, grass, sky, avatar bodies, particles ...) enqueued by pool type
    inline U64 sPoolFaces[POOLS] = {}, sPoolDrawables[POOLS] = {};
    // B0: the table items of the frame, by item type
    inline U64 sBFrames = 0, sBItems[ITEM_TYPES] = {}, sBTris[ITEM_TYPES] = {}, sBUnbounded[ITEM_TYPES] = {}, sBOutside[ITEM_TYPES] = {}, sBOutsideTris[ITEM_TYPES] = {}, sBNoRoom = 0;
    inline U64 sBGroupsPartial = 0, sBGroupsFull = 0;
    // the verdicts of the Hi-Z test for the items the frustum kept (sent N frames earlier): A = against the pyramid of the same frame at the occlusion point, B = against the pyramid of the frame before
    inline U64 sBTested[ITEM_TYPES] = {}, sBTestedTris[ITEM_TYPES] = {}, sBHidA[ITEM_TYPES] = {}, sBHidATris[ITEM_TYPES] = {}, sBHidB[ITEM_TYPES] = {}, sBHidBTris[ITEM_TYPES] = {}, sBValidB[ITEM_TYPES] = {};
    inline U64 sBBatches = 0, sBBatchesValidB = 0, sBGpuOutside = 0;

    inline long long sQpc0 = 0;
    inline U64 sTsc0 = 0;

    struct Lap
    {
        U64& mAcc;
        U64 mT0;
        explicit Lap(U64& acc) : mAcc(acc), mT0(sOn ? now() : 0) {}
        ~Lap() { if (sOn) mAcc += now() - mT0; }
    };

    void report();                          // pipeline.cpp
}

#endif // LL_LLVISCENSUS_H
