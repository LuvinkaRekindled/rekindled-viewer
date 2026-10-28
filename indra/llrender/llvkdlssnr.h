/**
 * @file llvkdlssnr.h
 * @brief DLSS-NR (DLSS 5 neural rendering) probe for the Vulkan mirror. doc/vulkan/PLAN-dlss-nr.md step one: with LL_VK_DLSS_NR=1 the viewer asks the
 *        NGX core what it knows about the feature (NGX feature id 18, no public header) and logs every answer. Nothing is created, nothing in the
 *        frame changes. Local testing only, never shipped: the only nvngx_dlssnr.dll we have is the NVIDIA-signed one from a mod bundle.
 */

#ifndef LL_LLVKDLSSNR_H
#define LL_LLVKDLSSNR_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"

#include <vector>

typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;
typedef struct NVSDK_NGX_FeatureDiscoveryInfo NVSDK_NGX_FeatureDiscoveryInfo;

namespace LLVkDlssNr
{
    // the value of LL_VK_DLSS_NR (0 when unset): 1 probe only; higher values are reserved for the later steps of the plan
    int requested();

    // the NGX feature id of DLSS-NR: 18 (NVSDK_NGX_Feature_Reserved18 in the public enum; the driver's NGX core lists DLSSNR there)
    int featureId();

    // After NGX is initialised, with the capability parameters NGX owns and the discovery info the viewer uses (its project id, data path): logs the
    // DLSSNR.* capabilities, the feature requirements of ids 14..18, the extension lists of 18 and the scratch buffer size. Only with requested() > 0.
    void probe(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* capabilities, const NVSDK_NGX_FeatureDiscoveryInfo& base);
}

#endif // LL_VULKAN
#endif // LL_LLVKDLSSNR_H
