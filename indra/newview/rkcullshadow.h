/**
 * @file rkcullshadow.h
 * @brief S7 C1 / C1b (doc/vulkan/S7-design.md sections 14-16): the flat group table of the main view's cull, kept by the groups and read by the worker (the kept sets, the late list, the view requests).
 *        Always on. RK_VK_CULL_FACTORS="a,b,c" sets the three margin factors (default 1,1.5,2.5). The table is kept up to date by the groups (C1b) and sent to the worker as delta rows (C1c). RK_VK_CULL_VERIFY=1: every 60 frames the full walk is compared with the kept table. The camera's yaw is logged once a second while the table is built (poseLog; for the run scripts).
 *        The code is in pipeline.cpp (build, send, apply, the hooks' functions), the hooks themselves in llvieweroctree.cpp, the worker side is kind 35.
 */
#ifndef RK_RKCULLSHADOW_H
#define RK_RKCULLSHADOW_H

#include "stdtypes.h"
#include <atomic>
#include <cstdlib>

class LLCamera;
class LLViewerOctreeGroup;
class LLSpatialBridge;

namespace RKCullShadow
{
    void poseLog(LLCamera& camera);

    // C1b: the hooks
    void added(LLViewerOctreeGroup* group);         // a group was constructed: it gets a slot at the next processing if its partition is covered
    void forget(LLViewerOctreeGroup* group);        // the destructor: the slot is freed
    void touch(LLViewerOctreeGroup* group);         // something the row shows changed: refresh it at the next processing

    void build(LLCamera& camera);       // end of the main view's updateCull: the table, the CPU set, the verdicts that came back
    void send(LLCamera& camera);        // the occlusion point of the world pass: the table to the worker
}

#endif // RK_RKCULLSHADOW_H
