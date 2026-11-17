/**
 * @file llcullshadow.h
 * @brief S7 C1 / C1b (doc/vulkan/S7-design.md sections 14-16): the flat group table and a shadow-mode GPU frustum test of the main view's cull. Measurement only: nothing here changes what is culled or drawn.
 *        Off unless LL_VK_CULL_SHADOW=1. LL_VK_CULL_FACTORS="a,b,c" sets the three margin factors (default 1,1.5,2.5). The table is kept up to date by the groups (C1b) and sent to the worker as delta rows (C1c). LL_VK_CULL_VERIFY=1: every 60 frames the full walk is compared with the kept table. LL_VK_POSE_LOG=1: the camera's yaw once a second (for the run scripts).
 *        The code is in pipeline.cpp (build, send, apply, the hooks' functions), the hooks themselves in llvieweroctree.cpp, the worker side is kind 35.
 */
#ifndef LL_LLCULLSHADOW_H
#define LL_LLCULLSHADOW_H

#include "stdtypes.h"
#include "llvkenv.h"        // <FS> ll_vk_env: the stack's defaults
#include <atomic>
#include <cstdlib>

class LLCamera;
class LLViewerOctreeGroup;
class LLSpatialBridge;

namespace LLCullShadow
{
    inline const bool sOn = ll_vk_env("LL_VK_CULL_SHADOW") != nullptr;
    inline const bool sPoseLog = getenv("LL_VK_POSE_LOG") != nullptr;      // one line a second: the camera's yaw, pitch and origin (for the run scripts), without the rest
    inline const bool sVerify = sOn && getenv("LL_VK_CULL_VERIFY") != nullptr;
    inline const bool sCompare = sOn && !(ll_vk_env("LL_VK_CULL_COMPARE") && ll_vk_env("LL_VK_CULL_COMPARE")[0] == '0');     // LL_VK_CULL_COMPARE=0: no comparison with the CPU set (the production cost of the table and its test)
    // C2 (doc/vulkan/S7-design.md section 18): LL_VK_CULL_USE=1 derives the main view's visible groups from the GPU verdict next to the walk and compares them (the walk is still what is used); =2 the covered partitions
    // are not walked, their lists come from the verdict. LL_VK_CULL_USE_VARIANT=1..3 picks the margin variant (default 2).
    inline const unsigned sUseEnv = sOn && getenv("LL_VK_CULL_USE") ? (unsigned)atoi(getenv("LL_VK_CULL_USE")) : 0u;
    inline std::atomic<unsigned> sUse{ sUseEnv };      // (live while the table runs: rt-debug.txt culluse=0/1/2 overrides it, a same-lap A/B of the walk against the verdict)
    inline const unsigned sUseVariant = getenv("LL_VK_CULL_USE_VARIANT") ? (unsigned)atoi(getenv("LL_VK_CULL_USE_VARIANT")) : 2u;
    bool useSkip(class LLSpatialPartition* part);   // the partition is handled by useVerdict this frame (its root is rebound here): the caller does not walk it
    void useVerdict(LLCamera& camera);              // after the partition loop of the main view's updateCull: the lists of the skipped partitions
    bool useMatches(LLCamera& camera);              // the verdict was made for a frustum of this shape (aspect, field of view, far limit): the margins cover the camera's motion, not a change of shape (a snapshot has no UI: another aspect); false = this frame is walked
    void poseLog(LLCamera& camera);

    // C1b: the hooks (each call site tests sOn first, so a run without the switch pays one branch)
    void added(LLViewerOctreeGroup* group);         // a group was constructed: it gets a slot at the next processing if its partition is covered
    void forget(LLViewerOctreeGroup* group);        // the destructor: the slot is freed
    void touch(LLViewerOctreeGroup* group);         // something the row shows changed: refresh it at the next processing
    void touchBridge(LLSpatialBridge* bridge);      // stage 2 of doc/vulkan/PLAN-visible-set-owner.md: the bridge's matrix or extents changed: every group of its octree shows another world box

    void build(LLCamera& camera);       // end of the main view's updateCull: the table, the CPU set, the verdicts that came back
    void send(LLCamera& camera);        // the occlusion point of the world pass: the table to the worker
}

#endif // LL_LLCULLSHADOW_H
