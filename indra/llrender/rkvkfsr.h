/**
 * @file rkvkfsr.h
 * @brief AMD FSR 3.1 upscaler for the Vulkan mirror, from the FidelityFX SDK v1.1.4 source (MIT) built into static libraries, no DLL. The second backend of
 *        rkvkupscale.h: the upscaler of the viewer (rkvkupscale.h), it works on any GPU. Compiled in when the SDK headers were found at configure time (RK_FSR); stubs otherwise.
 *        doc/vulkan/PLAN-fsr-upscaler.md
 */

#ifndef RK_RKVKFSR_H
#define RK_RKVKFSR_H

#if defined(RK_VULKAN)

#include "rkvkupscale.h"    // Params

namespace RKVkFsr
{
    // before the device is created: the extensions the FSR backend assumes the application enabled (it loads their functions by the KHR names and calls them without a check: with
    // VK_KHR_get_memory_requirements2 not enabled the pointer is null and ffxCreateContext crashed, 2026-10-27). Only the ones the device has are added.
    void addDeviceExtensions(VkPhysicalDevice device, std::vector<const char*>& out);

    // after the device exists: sets up the SDK backend; false (and a log line saying why) if FSR cannot be used here
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // Records the upscale into 'cmd'. The images must be in the layouts inputLayout()/outputLayout() name. Makes the FSR context on first use and whenever the sizes change.
    bool evaluate(VkCommandBuffer cmd, const RKVkUpscale::Params& p);

    // FSR reads every input as a sampled image and writes the output as a storage image
    VkImageLayout inputLayout(const RKVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // RK_VULKAN
#endif // RK_RKVKFSR_H
