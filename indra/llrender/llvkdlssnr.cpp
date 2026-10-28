/**
 * @file llvkdlssnr.cpp
 * @brief DLSS-NR probe (see llvkdlssnr.h and doc/vulkan/PLAN-dlss-nr.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkdlssnr.h"

#if defined(LL_DLSS)
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#endif

#include <cstdio>
#include <string>
#include <vector>

namespace LLVkDlssNr
{

int requested()
{
    static const int value = []() {
        char buf[16] = {};
        return GetEnvironmentVariableA("LL_VK_DLSS_NR", buf, sizeof(buf)) > 0 ? atoi(buf) : 0;
    }();
    return value;
}

int featureId()
{
    return 18;
}

#if !defined(LL_DLSS)

void probe(VkInstance, VkPhysicalDevice, VkDevice, NVSDK_NGX_Parameter*, const NVSDK_NGX_FeatureDiscoveryInfo&)
{
    LL_WARNS("VkDlssNr") << "LL_VK_DLSS_NR is set but this build has no NGX support" << LL_ENDL;
}

#else

namespace
{

const char* result_name(NVSDK_NGX_Result r)
{
    switch (r)
    {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_Fail: return "Fail";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    default: return "other";
    }
}

std::string hex(unsigned int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

// the feature names of the NGX core's capability table, in feature id order (read from the driver's _nvngx.dll, 617.42; 0..13 match the public enum)
const char* feature_name(int id)
{
    static const char* const names[] = { "Denoiser", "SuperSampling", "InPainting", "ImageSuperResolution", "SlowMotion", "VideoSuperResolution", "Colorize", "StyleTransfer",
                                         "VideoDenoiser", "ImageSignalProcessing", "DeepResolve", "FrameGeneration", "DeepDVC", "SuperSamplingDenoising", "TrueHDR", "Latewarp",
                                         "VSR", "FGX", "DLSSNR" };
    return id >= 0 && id < (int)(sizeof(names) / sizeof(names[0])) ? names[id] : "?";
}

std::string support_bits(unsigned int bits)
{
    if (bits == NVSDK_NGX_FeatureSupportResult_Supported) return "Supported";
    std::string s;
    if (bits & NVSDK_NGX_FeatureSupportResult_CheckNotPresent) s += " CheckNotPresent";
    if (bits & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) s += " DriverVersionUnsupported";
    if (bits & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) s += " AdapterUnsupported";
    if (bits & NVSDK_NGX_FeatureSupportResult_OSVersionBelowMinimumSupported) s += " OSVersionBelowMinimum";
    if (bits & NVSDK_NGX_FeatureSupportResult_NotImplemented) s += " NotImplemented";
    if (bits & ~31u) s += " +" + hex(bits & ~31u);
    return s.empty() ? hex(bits) : s.substr(1);
}

void log_int(NVSDK_NGX_Parameter* p, const char* name)
{
    int v = 0;
    const NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetI(p, name, &v);
    if (NVSDK_NGX_SUCCEED(r)) LL_INFOS("VkDlssNr") << name << " = " << v << LL_ENDL;
    else LL_INFOS("VkDlssNr") << name << ": not set (" << result_name(r) << ")" << LL_ENDL;
}

void log_uint(NVSDK_NGX_Parameter* p, const char* name)
{
    unsigned int v = 0;
    const NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetUI(p, name, &v);
    if (NVSDK_NGX_SUCCEED(r)) LL_INFOS("VkDlssNr") << name << " = " << v << LL_ENDL;
    else LL_INFOS("VkDlssNr") << name << ": not set (" << result_name(r) << ")" << LL_ENDL;
}

void* log_pointer(NVSDK_NGX_Parameter* p, const char* name)
{
    void* v = nullptr;
    const NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetVoidPointer(p, name, &v);
    if (NVSDK_NGX_SUCCEED(r)) LL_INFOS("VkDlssNr") << name << " = " << v << (v ? "" : " (null)") << LL_ENDL;
    else LL_INFOS("VkDlssNr") << name << ": not set (" << result_name(r) << ")" << LL_ENDL;
    return NVSDK_NGX_SUCCEED(r) ? v : nullptr;
}

}

void probe(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* caps, const NVSDK_NGX_FeatureDiscoveryInfo& base)
{
    (void)device;
    if (requested() <= 0)
    {
        return;
    }
    LL_INFOS("VkDlssNr") << "DLSS-NR probe (LL_VK_DLSS_NR=" << requested() << "): what the NGX core says about feature " << featureId() << " (" << feature_name(featureId()) << ")" << LL_ENDL;

    // 1. the capability parameters: the names the driver's NGX core has for DLSSNR (set when it found a snippet and judged it)
    if (caps)
    {
        log_int(caps, "DLSSNR.Available");
        log_int(caps, "DLSSNR.NeedsUpdatedDriver");
        log_uint(caps, "DLSSNR.MinDriverVersionMajor");
        log_uint(caps, "DLSSNR.MinDriverVersionMinor");
        log_int(caps, "DLSSNR.FeatureInitResult");
        log_int(caps, "DLSSNR.Enabled");
        log_uint(caps, "DLSSNR.Width");
        log_uint(caps, "DLSSNR.Height");
        log_pointer(caps, "DLSSNRGetStatsCallback");
        void* ratio_cb = log_pointer(caps, "DLSSNRComputeScalingRatioCallback");
        // for comparison: the same reads for the features the viewer uses
        log_int(caps, "SuperSampling.Available");
        log_int(caps, "SuperSamplingDenoising.Available");
        log_int(caps, "FrameGeneration.Available");

        // LL_VK_DLSS_NR_RATIO=1: call the scaling ratio callback with the DLSSOptimalSettingsCallback convention (NVSDK_NGX_Result cb(NVSDK_NGX_Parameter*), inputs and
        // outputs through the parameters). A guess: a wrong convention crashes here, on the CPU, before login. Off unless asked for.
        if (ratio_cb && GetEnvironmentVariableA("LL_VK_DLSS_NR_RATIO", nullptr, 0) > 0)
        {
            NVSDK_NGX_Parameter* p = nullptr;
            if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_AllocateParameters(&p)) && p)
            {
                for (unsigned int quality : { (unsigned int)NVSDK_NGX_PerfQuality_Value_MaxQuality, (unsigned int)NVSDK_NGX_PerfQuality_Value_Balanced, (unsigned int)NVSDK_NGX_PerfQuality_Value_MaxPerf })
                {
                    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Width", 2558);
                    NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Height", 1341);
                    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, 2558);
                    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, 1341);
                    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_PerfQualityValue, quality);
                    typedef NVSDK_NGX_Result(NVSDK_CONV * Callback)(NVSDK_NGX_Parameter*);
                    const NVSDK_NGX_Result r = ((Callback)ratio_cb)(p);
                    float ratio = 0.f;
                    const NVSDK_NGX_Result rr = NVSDK_NGX_Parameter_GetF(p, "DLSSNR.ScalingRatio", &ratio);
                    LL_INFOS("VkDlssNr") << "scaling ratio callback for quality " << quality << ": " << result_name(r) << ", DLSSNR.ScalingRatio "
                                         << (NVSDK_NGX_SUCCEED(rr) ? std::to_string(ratio) : std::string("not set (") + result_name(rr) + ")") << LL_ENDL;
                }
                NVSDK_NGX_VULKAN_DestroyParameters(p);
            }
        }
    }
    else
    {
        LL_WARNS("VkDlssNr") << "no capability parameters to read" << LL_ENDL;
    }

    // 2. the feature requirements of ids 14..18: 18 is the one that matters, 14..17 (TrueHDR, Latewarp, VSR, FGX) check the table reading
    for (int id = 14; id <= 18; ++id)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info = base;
        info.FeatureID = (NVSDK_NGX_Feature)id;
        NVSDK_NGX_FeatureRequirement req = {};
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &info, &req);
        if (NVSDK_NGX_SUCCEED(r))
        {
            LL_INFOS("VkDlssNr") << "feature " << id << " (" << feature_name(id) << ") requirements: " << support_bits((unsigned int)req.FeatureSupported)
                                 << ", min hardware architecture " << hex(req.MinHWArchitecture) << ", min OS " << req.MinOSVersion << LL_ENDL;
        }
        else
        {
            LL_INFOS("VkDlssNr") << "feature " << id << " (" << feature_name(id) << ") requirements query failed: " << result_name(r) << LL_ENDL;
        }
    }

    // 3. the extensions 18 wants (addDeviceExtensions in llvkdlss.cpp enabled the device ones already when the flag was set; this is the list for the log)
    {
        NVSDK_NGX_FeatureDiscoveryInfo info = base;
        info.FeatureID = (NVSDK_NGX_Feature)featureId();
        uint32_t count = 0;
        VkExtensionProperties* props = nullptr;
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props);
        std::string names;
        for (uint32_t i = 0; NVSDK_NGX_SUCCEED(r) && props && i < count; ++i) names += std::string(i ? ", " : "") + props[i].extensionName;
        LL_INFOS("VkDlssNr") << "feature 18 instance extensions: " << result_name(r) << ", " << (NVSDK_NGX_SUCCEED(r) ? count : 0) << (names.empty() ? "" : ": " + names) << LL_ENDL;
        count = 0;
        props = nullptr;
        names.clear();
        r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &info, &count, &props);
        for (uint32_t i = 0; NVSDK_NGX_SUCCEED(r) && props && i < count; ++i) names += std::string(i ? ", " : "") + props[i].extensionName;
        LL_INFOS("VkDlssNr") << "feature 18 device extensions: " << result_name(r) << ", " << (NVSDK_NGX_SUCCEED(r) ? count : 0) << (names.empty() ? "" : ": " + names) << LL_ENDL;
    }

    // 4. the scratch buffer size for a window-sized feature (a question to the core; it may ask the snippet)
    {
        NVSDK_NGX_Parameter* p = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_AllocateParameters(&p)) && p)
        {
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Width", 2558);
            NVSDK_NGX_Parameter_SetUI(p, "DLSSNR.Height", 1341);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Width, 2558);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_Height, 1341);
            NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_PerfQualityValue, (unsigned int)NVSDK_NGX_PerfQuality_Value_Balanced);
            size_t bytes = 0;
            const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetScratchBufferSize((NVSDK_NGX_Feature)featureId(), p, &bytes);
            LL_INFOS("VkDlssNr") << "feature 18 scratch buffer size: " << result_name(r) << ", " << bytes << " bytes" << LL_ENDL;
            NVSDK_NGX_VULKAN_DestroyParameters(p);
        }
    }
    LL_INFOS("VkDlssNr") << "DLSS-NR probe done (nothing created)" << LL_ENDL;
}

#endif // LL_DLSS

}

#endif // LL_VULKAN
