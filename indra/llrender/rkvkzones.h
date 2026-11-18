/**
 * @file rkvkzones.h
 * @brief GPU timing zones on the Vulkan timeline, for code outside the render library (no RK_VULKAN needed to include this).
 */

#ifndef RK_RKVKZONES_H
#define RK_RKVKZONES_H

namespace RKVkZones
{
    // true on the thread that writes the Vulkan command queue while the Vulkan worker runs
    bool available();
    // a named GPU zone begins (name) / ends (name ignored) at this point of the frame; see RKVkAsync::gpuZone
    void zone(const char* name, bool begin);
}

#endif // RK_RKVKZONES_H
