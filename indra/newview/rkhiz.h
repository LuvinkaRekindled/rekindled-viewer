/**
 * @file rkhiz.h
 * @brief S7 H3: the targets of the Hi-Z occlusion pass and the batch they are tested in. See doc/vulkan/S7-design.md.
 */
#ifndef RK_RKHIZ_H
#define RK_RKHIZ_H

#include "stdtypes.h"
#include "llvector4a.h"

// <FS> S7 H3 (doc/vulkan/S7-design.md): occlusion is decided by the worker's Hi-Z pass, not by queries. Whatever wants a verdict (an occlusion group, a reflection probe) is an RKHizTarget: RKHiz collects the
// boxes of the targets once a frame (the world camera, at the point of the G-buffer pass where the occluders have been drawn), sends them as one record, and gives each target the result of the batch it was in
// when the GPU has finished the frame (2-4 frames later, never blocking).
class RKHizTarget
{
public:
    virtual ~RKHizTarget();
    virtual void hizResult(bool occluded) = 0;      // the verdict of the batch mHizSerial named (mHizSerial is 0 by then)
    bool isHizAwaiting() const { return mHizSerial != 0; }
    U32 mHizId = 0;             // the registry id (given when the target first has a box)
    U32 mHizSerial = 0;         // the batch whose result is awaited; 0 = none
    U32 mHizBatch = 0;          // the number of the batch the target was last added to (a target is in a batch once)
    bool mHizOccludedOnce = false;  // <FS> the last verdict said occluded and was not applied: a target goes occluded on two agreeing verdicts and comes back on one (plan 8.38: one stale verdict costs nothing)
};

class RKHiz
{
public:
    static void begin();                                                // before the boxes of a pass are collected
    static void add(RKHizTarget* target, const LLVector4a& center, F32 half_x, F32 half_y, F32 half_z, bool water);
    static void send(const F32* view_proj);                           // one record for the boxes collected; their targets now await its result
    static void apply();                                                // the results of the batches the GPU has finished, to their targets (once per cull of the world camera)
    // (RK_VK_BLINK=1, a diagnostic) the last verdict applied to a target: out[0] the word (bit 0 occluded, bits 8-15 the pyramid level, 16-19 the special cases), out[1] the box's nearest depth, out[2] the farthest depth of the
    // pyramid texels it covers (both as float bits), out[3] the frame it was applied, out[4] 1 when it was stale for the camera. False when none is kept.
    static bool lastVerdict(const RKHizTarget* target, U32* out5);
    static RKHizTarget* sWatch;                                       // (the bug hunt's watcher, RK_VK_WATCH_OBJECT in pipeline.cpp) the target whose verdicts are logged (VkWatch) when its answer changes
};

#endif // RK_RKHIZ_H
