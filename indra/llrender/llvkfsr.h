/**
 * @file llvkfsr.h
 * @brief AMD FSR 3.1 upscaler for the Vulkan mirror, through the FidelityFX SDK v1.1.4 ffx-api (amd_fidelityfx_vk.dll, loaded at run time). The second backend of
 *        llvkupscale.h: it takes the same Params as DLSS and works on any GPU. Compiled in when the SDK headers were found at configure time (LL_FSR); stubs otherwise.
 *        doc/vulkan/PLAN-fsr-upscaler.md
 */

#ifndef LL_LLVKFSR_H
#define LL_LLVKFSR_H

#if defined(LL_VULKAN)

#include "llvkdlss.h"       // Params

namespace LLVkFsr
{
    // after the device exists: loads the DLL; false (and a log line saying why) if FSR cannot be used here
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // Records the upscale into 'cmd'. The images must be in the layouts inputLayout()/outputLayout() name. Makes the FSR context on first use and whenever the sizes change.
    bool evaluate(VkCommandBuffer cmd, const LLVkDlss::Params& p);

    // FSR reads every input as a sampled image and writes the output as a storage image
    VkImageLayout inputLayout(const LLVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // LL_VULKAN
#endif // LL_LLVKFSR_H
