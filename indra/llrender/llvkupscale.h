/**
 * @file llvkupscale.h
 * @brief The upscaler of the Vulkan mirror: one interface over DLSS (llvkdlss.h) and AMD FSR 3.1 (llvkfsr.h). The callers talk to this; it picks the backend once, when the device is made.
 *        LL_VK_UPSCALER=dlss|fsr forces one (default: DLSS where NGX offers it, else FSR). doc/vulkan/PLAN-fsr-upscaler.md
 */

#ifndef LL_LLVKUPSCALE_H
#define LL_LLVKUPSCALE_H

#if defined(LL_VULKAN)

#include "llvkdlss.h"

namespace LLVkUpscale
{
    using Params = LLVkDlss::Params;

    // an upscaler is wanted at all (LL_VK_DLSS is set by the FSVkDLSS setting)
    bool requested();

    // before the instance / device are created: what the backends want on top of ours
    void addInstanceExtensions(std::vector<const char*>& out);
    void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out);

    // after the device exists: picks and starts the backend; false (and log lines saying why) if none can be used here
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // the active backend's name ("DLSS", "FSR", "none")
    const char* backendName();

    // ray reconstruction (a DLSS feature): never with FSR
    bool rayReconstructionAvailable();

    bool evaluate(VkCommandBuffer cmd, const Params& p);

    // the mip bias for filtered texture lookups at the current render scale (log2 of the scale; FSR asks for one more stop when it upscales)
    float lodBias();
    void setLodBias(float bias);

    VkImageLayout inputLayout(const LLVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // LL_VULKAN
#endif // LL_LLVKUPSCALE_H
