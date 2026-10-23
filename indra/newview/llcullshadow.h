/**
 * @file llcullshadow.h
 * @brief S7 C1 (doc/vulkan/S7-design.md section 14): the flat group table and a shadow-mode GPU frustum test of the main view's cull. Measurement only: nothing here changes what is culled or drawn.
 *        Off unless LL_VK_CULL_SHADOW=1. LL_VK_CULL_FACTORS="a,b,c" sets the three margin factors (default 1,1.5,2.5). The code is in pipeline.cpp (build, send, apply), the worker side is kind 35.
 */
#ifndef LL_LLCULLSHADOW_H
#define LL_LLCULLSHADOW_H

#include "stdtypes.h"
#include <cstdlib>
#include <mutex>
#include <unordered_map>

class LLCamera;

namespace LLCullShadow
{
    inline const bool sOn = getenv("LL_VK_CULL_SHADOW") != nullptr;
    inline const bool sPoseLog = getenv("LL_VK_POSE_LOG") != nullptr;      // one line a second: the camera's yaw, pitch and origin (for the run scripts), without the rest
    void poseLog(LLCamera& camera);

    // the groups that have a row id, by id (a group unregisters itself in its destructor)
    inline std::mutex sMutex;
    inline std::unordered_map<U32, void*> sGroups;
    inline void forget(U32 id)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sGroups.erase(id);
    }

    void build(LLCamera& camera);       // end of the main view's updateCull: the table, the CPU set, the verdicts that came back
    void send(LLCamera& camera);        // the occlusion point of the world pass: the table to the worker
}

#endif // LL_LLCULLSHADOW_H
