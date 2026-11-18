/**
 * @file rkvkngx.h
 * @brief The NVIDIA NGX bootstrap for DLSS-NR (rkvkdlssnr.h), the one NVIDIA feature that is left, for LOCAL builds only (doc/vulkan/PLAN-remove-nvidia.md).
 *        Compiled in only when the CMake option REKINDLED_LOCAL_NR is ON and the DLSS SDK was found at configure time (LL_NGX_NR); a release build has none of it, and every call here
 *        is a stub that says "not available". There is no DLSS super resolution or ray reconstruction in the viewer any more: the upscaler is AMD FSR (rkvkfsr.h).
 */

#ifndef LL_LLVKNGX_H
#define LL_LLVKNGX_H

#if defined(LL_VULKAN)

#include "rkvkcontext.h"

#include <vector>

namespace LLVkNgx
{
    // before the instance / device are created: the extensions NGX wants for DLSS-NR (only the ones that exist are added)
    void addInstanceExtensions(std::vector<const char*>& out);
    void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out);

    // after the device exists: starts NGX and asks whether DLSS-NR can run here; false (NGX is shut down again) if not
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // layouts NGX reads / writes the images in
    VkImageLayout inputLayout(const LLVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // LL_VULKAN
#endif // LL_LLVKNGX_H
