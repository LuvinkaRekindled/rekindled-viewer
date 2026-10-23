/**
 * @file llcullshadow.h
 * @brief S7 C1 / C1b (doc/vulkan/S7-design.md sections 14-16): the flat group table and a shadow-mode GPU frustum test of the main view's cull. Measurement only: nothing here changes what is culled or drawn.
 *        Off unless LL_VK_CULL_SHADOW=1. LL_VK_CULL_FACTORS="a,b,c" sets the three margin factors (default 1,1.5,2.5). The table is kept up to date by the groups (C1b) and sent to the worker as delta rows (C1c). LL_VK_CULL_VERIFY=1: every 60 frames the full walk is compared with the kept table. LL_VK_POSE_LOG=1: the camera's yaw once a second (for the run scripts).
 *        The code is in pipeline.cpp (build, send, apply, the hooks' functions), the hooks themselves in llvieweroctree.cpp, the worker side is kind 35.
 */
#ifndef LL_LLCULLSHADOW_H
#define LL_LLCULLSHADOW_H

#include "stdtypes.h"
#include <cstdlib>

class LLCamera;
class LLViewerOctreeGroup;

namespace LLCullShadow
{
    inline const bool sOn = getenv("LL_VK_CULL_SHADOW") != nullptr;
    inline const bool sPoseLog = getenv("LL_VK_POSE_LOG") != nullptr;      // one line a second: the camera's yaw, pitch and origin (for the run scripts), without the rest
    inline const bool sVerify = sOn && getenv("LL_VK_CULL_VERIFY") != nullptr;
    inline const bool sCompare = sOn && !(getenv("LL_VK_CULL_COMPARE") && getenv("LL_VK_CULL_COMPARE")[0] == '0');     // LL_VK_CULL_COMPARE=0: no comparison with the CPU set (the production cost of the table and its test)
    void poseLog(LLCamera& camera);

    // C1b: the hooks (each call site tests sOn first, so a run without the switch pays one branch)
    void added(LLViewerOctreeGroup* group);         // a group was constructed: it gets a slot at the next processing if its partition is covered
    void forget(LLViewerOctreeGroup* group);        // the destructor: the slot is freed
    void touch(LLViewerOctreeGroup* group);         // something the row shows changed: refresh it at the next processing

    void build(LLCamera& camera);       // end of the main view's updateCull: the table, the CPU set, the verdicts that came back
    void send(LLCamera& camera);        // the occlusion point of the world pass: the table to the worker
}

#endif // LL_LLCULLSHADOW_H
