/**
 * @file llviscensus.h
 * @brief S7 B0 / C0 (doc/vulkan/S7-design.md section 13): where the main thread's visibility work goes (cull, state sort, post sort), which pass types the scene table serves and which the
 *        CPU pools draw, and what a fragment cull over the table items would drop (frustum and previous-frame Hi-Z, both tested in the worker). Measurement only: nothing here changes what is drawn.
 *        Off unless LL_VK_VISCENSUS=1 (every hook is one branch on a constant); LL_VK_VISCENSUS_BOXES=0 leaves the box test out. Header only; the log lines (tag VisCensus) are written by
 *        LLVisCensus::report() in pipeline.cpp every 600 world post sorts. Times are rdtsc ticks, converted against the performance counter at the report.
 *        NOT a plain-run tool: its laps and the box test cost CPU (see the notes of the run it is used in).
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
    constexpr int ITEM_TYPES = 64;          // table item types: 0-25 static, 26-51 rigged (kGeoTypeCount each), then the alpha runs (static, rigged)

    inline U64 now() { return __rdtsc(); }

    // updateCull (world camera, main view)
    inline U64 sCullFrames = 0, sCullTicks = 0;
    // stateSort (world camera, main view)
    inline U64 sSortCalls = 0, sSortTicks = 0, sSortTicksCheck = 0, sSortCheckCalls = 0;
    inline U64 sSortTicksMarkVis = 0, sSortMarkVisN = 0, sSortTicksLod = 0, sSortLodCalls = 0, sSortLodTrue = 0;
    inline U64 sSortTicksDrawable = 0, sSortDrawableCalls = 0, sSortTicksDist = 0, sSortDistCalls = 0;
    inline U64 sSortTicksBridge = 0, sSortBridgeCalls = 0;
    inline U64 sSortDrawableGroups = 0, sSortVisibleGroups = 0, sSortOccluded = 0;
    // how much of the visible set and of the sorted drawables changes from one frame to the next (the render map loop's groups against the frame before; the drawables of the two loops by kind)
    inline U64 sChurnGroups = 0, sChurnKept = 0, sChurnSerial = 0, sChurnNewInfo = 0, sChurnLeft = 0, sChurnFrames = 0;
    inline U32 sChurnFrame = 0, sChurnCurN = 0, sChurnCurKept = 0, sChurnPrevN = 0;
    inline U64 sMarkActive = 0, sMarkAvatar = 0, sMarkBridge = 0, sDrawActive = 0, sDrawAvatar = 0, sDrawStatic = 0;
    // postSort (world camera, main view)
    inline U64 sPostCalls = 0, sPostTicks = 0, sPostTicksRebuildGeom = 0, sPostTicksMap = 0, sPostTicksMesh = 0, sPostTicksAlpha = 0;
    // visible groups of the render map loop by partition, and what the table holds of them
    inline U64 sPartGroups[PARTS] = {}, sPartElems[PARTS] = {}, sPartWithTable[PARTS] = {}, sPartAllServed[PARTS] = {}, sPartInfos[PARTS] = {}, sPartTris[PARTS] = {};
    // draw infos of the visible groups by pass type: served by the table (not listed in the render map) and listed (drawn by the pool loops, through the typed records)
    inline U64 sPassServedInfos[PASSES] = {}, sPassServedTris[PASSES] = {}, sPassListedInfos[PASSES] = {}, sPassListedTris[PASSES] = {};
    inline U64 sAlphaGroups = 0, sAlphaServedItems = 0, sAlphaItemsTotal = 0, sRiggedAlphaGroups = 0;
    // faces of drawables without a volume (terrain, water, trees, grass, sky, avatar bodies ...) enqueued, by pool type
    inline U64 sPoolFaces[POOLS] = {}, sPoolDrawables = 0;
    // B0: the table items of the frame, by item type
    inline U64 sBFrames = 0, sBItems[ITEM_TYPES] = {}, sBTris[ITEM_TYPES] = {}, sBUnbounded[ITEM_TYPES] = {}, sBNoRoom = 0;
    inline U64 sBGroupsPartial = 0, sBGroupsFull = 0;
    // the verdicts of the worker's test for the boxes sent (2-4 frames earlier), per item type: frustum (the GPU's, bits 17 / 19 of the same-frame test), hidden by the pyramid of the same frame at the occlusion point (A),
    // hidden by the previous frame's pyramid (B, tested with the previous view-projection); and the CPU's own frustum test for comparison
    inline U64 sBTested[ITEM_TYPES] = {}, sBTestedTris[ITEM_TYPES] = {};
    inline U64 sBGpuOut[ITEM_TYPES] = {}, sBGpuOutTris[ITEM_TYPES] = {};
    inline U64 sBCpuOut[ITEM_TYPES] = {}, sBCpuOutNotGpu[ITEM_TYPES] = {}, sBGpuOutNotCpu[ITEM_TYPES] = {};
    inline U64 sBHidA[ITEM_TYPES] = {}, sBHidATris[ITEM_TYPES] = {}, sBHidB[ITEM_TYPES] = {}, sBHidBTris[ITEM_TYPES] = {}, sBValidB[ITEM_TYPES] = {};
    inline U64 sBDropB[ITEM_TYPES] = {}, sBDropBTris[ITEM_TYPES] = {};          // frustum or hidden by the previous frame's pyramid: what a cull in the G-buffer pass could drop
    inline U64 sBAvatars = 0, sBAvatarTris = 0;                                 // distinct avatars whose extents bound rigged items (per frame), their rigged triangles
    inline U64 sBBatches = 0, sBBatchesValidB = 0;

    inline long long sQpc0 = 0;
    inline U64 sTsc0 = 0;

    void report();                          // pipeline.cpp
}

#endif // LL_LLVISCENSUS_H
