/**
 * @file rkvkdlssnr.cpp
 * @brief DLSS-NR probe, creation and evaluation (see rkvkdlssnr.h and doc/vulkan/PLAN-dlss-nr.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "rkvkdlssnr.h"

#if defined(LL_NGX_NR)
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace RKVkDlssNr
{

using namespace RKVk;

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

#if !defined(LL_NGX_NR)

void probe(VkInstance, VkPhysicalDevice, VkDevice, NVSDK_NGX_Parameter*, const NVSDK_NGX_FeatureDiscoveryInfo&)
{
    LL_WARNS("VkDlssNr") << "LL_VK_DLSS_NR is set but this build has no NGX support" << LL_ENDL;
}
void queryAvailability(VkInstance, VkPhysicalDevice, VkDevice, NVSDK_NGX_Parameter*, const NVSDK_NGX_FeatureDiscoveryInfo&) {}
bool available() { return false; }
bool ready() { return false; }
bool evaluate(VkCommandBuffer, const Params&) { return false; }
void release() {}
void shutdown() {}

#else

namespace
{

VkDevice gDevice = VK_NULL_HANDLE;
bool gAvailable = false;            // the core said DLSSNR.Available and the requirements query said supported
bool gFailed = false;               // a creation failed: off for the session
NVSDK_NGX_Parameter* gParams = nullptr;
NVSDK_NGX_Handle* gFeature = nullptr;
U32 gFeatureKey[6] = {};            // output w/h, quality, preset, style (the bundle's config says preset and style are baked into the feature), scaling ratio x 1000
U64 gFeatureFrame = 0;
std::vector<std::pair<NVSDK_NGX_Handle*, U64>> gRetired;     // released when the frame has completed

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

std::string named(NVSDK_NGX_Result r)
{
    return std::string(result_name(r)) + " (" + hex((unsigned int)r) + ")";
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

bool log_int(NVSDK_NGX_Parameter* p, const char* name, int* out = nullptr)
{
    int v = 0;
    const NVSDK_NGX_Result r = NVSDK_NGX_Parameter_GetI(p, name, &v);
    if (NVSDK_NGX_SUCCEED(r)) LL_INFOS("VkDlssNr") << name << " = " << v << LL_ENDL;
    else LL_INFOS("VkDlssNr") << name << ": not set (" << result_name(r) << ")" << LL_ENDL;
    if (out) *out = NVSDK_NGX_SUCCEED(r) ? v : 0;
    return NVSDK_NGX_SUCCEED(r);
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

NVSDK_NGX_Resource_VK resource_of(const RKVk::Image& image, bool write, VkImageAspectFlags aspect)
{
    const VkImageSubresourceRange range = { aspect, 0, 1, 0, 1 };
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.mView, image.mImage, range, image.mFormat, image.mExtent.width, image.mExtent.height, write);
}

void set_subrect(NVSDK_NGX_Parameter* p, const char* image, U32 w, U32 h)
{
    const std::string base = std::string("DLSSNR.") + image + "Subrect";
    NVSDK_NGX_Parameter_SetUI(p, (base + "BaseX").c_str(), 0);
    NVSDK_NGX_Parameter_SetUI(p, (base + "BaseY").c_str(), 0);
    NVSDK_NGX_Parameter_SetUI(p, (base + "Width").c_str(), w);
    NVSDK_NGX_Parameter_SetUI(p, (base + "Height").c_str(), h);
}

void reap_retired()
{
    Context& ctx = Context::instance();
    for (size_t i = 0; i < gRetired.size(); )
    {
        if (ctx.frameFinished(gRetired[i].second))
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(gRetired[i].first);
            gRetired.erase(gRetired.begin() + i);
        }
        else
        {
            ++i;
        }
    }
}

}

void probe(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* caps, const NVSDK_NGX_FeatureDiscoveryInfo& base)
{
    if (requested() <= 0)
    {
        return;
    }
    gDevice = device;
    LL_INFOS("VkDlssNr") << "DLSS-NR probe (LL_VK_DLSS_NR=" << requested() << "): what the NGX core says about feature " << featureId() << " (" << feature_name(featureId()) << ")" << LL_ENDL;

    // 1. the capability parameters: the names the driver's NGX core has for DLSSNR (set when it found a snippet and judged it)
    int available = 0;
    if (caps)
    {
        log_int(caps, "DLSSNR.Available", &available);
        log_int(caps, "DLSSNR.NeedsUpdatedDriver");
        log_uint(caps, "DLSSNR.MinDriverVersionMajor");
        log_uint(caps, "DLSSNR.MinDriverVersionMinor");
        log_int(caps, "DLSSNR.FeatureInitResult");
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
                    LL_INFOS("VkDlssNr") << "scaling ratio callback for quality " << quality << ": " << named(r) << ", DLSSNR.ScalingRatio "
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
    bool supported = false;
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
            if (id == featureId()) supported = req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
        }
        else
        {
            LL_INFOS("VkDlssNr") << "feature " << id << " (" << feature_name(id) << ") requirements query failed: " << named(r) << LL_ENDL;
        }
    }

    // 3. the extensions 18 wants (addDeviceExtensions in rkvkngx.cpp enabled the device ones already when the flag was set; this is the list for the log)
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
    gAvailable = available != 0 && supported;
    LL_INFOS("VkDlssNr") << "DLSS-NR probe done (nothing created); the feature " << (gAvailable ? "can" : "cannot") << " be created here"
                         << (requested() >= 2 && gAvailable ? ", it will be on the first frame" : "") << LL_ENDL;
}

void queryAvailability(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* caps, const NVSDK_NGX_FeatureDiscoveryInfo& base)
{
    gDevice = device;
    int available = 0;
    if (caps)
    {
        NVSDK_NGX_Parameter_GetI(caps, "DLSSNR.Available", &available);
    }
    NVSDK_NGX_FeatureDiscoveryInfo info = base;
    info.FeatureID = (NVSDK_NGX_Feature)featureId();
    NVSDK_NGX_FeatureRequirement req = {};
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &info, &req);
    const bool supported = NVSDK_NGX_SUCCEED(r) && req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
    gAvailable = available != 0 && supported;
    LL_INFOS("VkDlssNr") << "DLSS-NR (neural rendering): " << (gAvailable ? "available" : "not available") << " (DLSSNR.Available " << available << ", feature 18 "
                         << (NVSDK_NGX_SUCCEED(r) ? support_bits((unsigned int)req.FeatureSupported) : "requirements query " + named(r)) << "; needs nvngx_dlssnr.dll next to the viewer)" << LL_ENDL;
}

bool available()
{
    return gAvailable;
}

bool ready()
{
    return gAvailable && !gFailed && gDevice != VK_NULL_HANDLE;
}

void release()
{
    if (gFeature)
    {
        gRetired.emplace_back(gFeature, gFeatureFrame);
        gFeature = nullptr;
        for (U32& k : gFeatureKey) k = 0;
        LL_INFOS("VkDlssNr") << "DLSS-NR feature retired (the mode went off); released once its frame has completed" << LL_ENDL;
    }
    if (!gRetired.empty())
    {
        reap_retired();
    }
}

bool evaluate(VkCommandBuffer cmd, const Params& p)
{
    if (!ready() || !p.mColor || !p.mDepth || !p.mMotion || !p.mOutput)
    {
        return false;
    }
    if (!gParams && !NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_AllocateParameters(&gParams)))
    {
        LL_WARNS("VkDlssNr") << "no parameter object" << LL_ENDL;
        gFailed = true;
        return false;
    }
    reap_retired();
    const U32 key[6] = { p.mOutputWidth, p.mOutputHeight, p.mQuality, (U32)p.mPreset, (U32)(p.mStyle + 0.5f), (U32)(p.mScalingRatio * 1000.f + 0.5f) };
    if (!gFeature || memcmp(key, gFeatureKey, sizeof(key)) != 0)
    {
        if (gFeature)
        {
            gRetired.emplace_back(gFeature, gFeatureFrame);
            gFeature = nullptr;
        }
        // creation: the output size is the feature's size (the plugin creates with "%ux%u preset=%u performanceMode=%u"); the generic names too, in case the core reads them
        NVSDK_NGX_Parameter_SetUI(gParams, "DLSSNR.Width", p.mOutputWidth);
        NVSDK_NGX_Parameter_SetUI(gParams, "DLSSNR.Height", p.mOutputHeight);
        NVSDK_NGX_Parameter_SetUI(gParams, NVSDK_NGX_Parameter_Width, p.mOutputWidth);
        NVSDK_NGX_Parameter_SetUI(gParams, NVSDK_NGX_Parameter_Height, p.mOutputHeight);
        NVSDK_NGX_Parameter_SetUI(gParams, NVSDK_NGX_Parameter_OutWidth, p.mOutputWidth);
        NVSDK_NGX_Parameter_SetUI(gParams, NVSDK_NGX_Parameter_OutHeight, p.mOutputHeight);
        NVSDK_NGX_Parameter_SetUI(gParams, NVSDK_NGX_Parameter_PerfQualityValue, p.mQuality);
        NVSDK_NGX_Parameter_SetUI(gParams, "DLSSNR.Hint.Render.Preset", (unsigned int)p.mPreset);
        NVSDK_NGX_Parameter_SetI(gParams, "DLSSNR.DepthInverted", 0);
        NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.Style", (float)(U32)(p.mStyle + 0.5f));      // 0 standard, 1 natural, 2 cinematic: part of the feature
        if (p.mScalingRatio > 0.f)
        {
            NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.ScalingRatio", p.mScalingRatio);
        }
        const auto t0 = std::chrono::steady_clock::now();
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_CreateFeature1(gDevice, cmd, (NVSDK_NGX_Feature)featureId(), gParams, &gFeature);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int init_result = 0;
        const bool have_init = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(gParams, "DLSSNR.FeatureInitResult", &init_result));
        if (!NVSDK_NGX_SUCCEED(r) || !gFeature)
        {
            LL_WARNS("VkDlssNr") << "creating the DLSS-NR feature failed: " << named(r) << " after " << ms << " ms"
                                 << (have_init ? ", FeatureInitResult " + hex((unsigned int)init_result) : std::string()) << "; off for this session" << LL_ENDL;
            gFeature = nullptr;
            gFailed = true;
            return false;
        }
        memcpy(gFeatureKey, key, sizeof(key));
        LL_INFOS("VkDlssNr") << "DLSS-NR feature created: " << p.mOutputWidth << "x" << p.mOutputHeight << " quality " << p.mQuality << " preset " << p.mPreset << " style " << (U32)(p.mStyle + 0.5f) << " scaling ratio " << p.mScalingRatio << " (0 = not set) in " << ms << " ms"
                             << (have_init ? ", FeatureInitResult " + hex((unsigned int)init_result) : std::string()) << LL_ENDL;
    }
    gFeatureFrame = Context::instance().getFrameNumber();

    NVSDK_NGX_Resource_VK color = resource_of(*p.mColor, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK depth = resource_of(*p.mDepth, false, VK_IMAGE_ASPECT_DEPTH_BIT);
    NVSDK_NGX_Resource_VK motion = resource_of(*p.mMotion, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK output = resource_of(*p.mOutput, true, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Parameter_SetVoidPointer(gParams, "DLSSNR.Color", &color);
    NVSDK_NGX_Parameter_SetVoidPointer(gParams, "DLSSNR.Depth", &depth);
    NVSDK_NGX_Parameter_SetVoidPointer(gParams, "DLSSNR.MVec", &motion);
    NVSDK_NGX_Parameter_SetVoidPointer(gParams, "DLSSNR.Output", &output);
    set_subrect(gParams, "Color", p.mOutputWidth, p.mOutputHeight);
    set_subrect(gParams, "Output", p.mOutputWidth, p.mOutputHeight);
    set_subrect(gParams, "Depth", p.mRenderWidth, p.mRenderHeight);
    set_subrect(gParams, "MVec", p.mRenderWidth, p.mRenderHeight);
    NVSDK_NGX_Parameter_SetUI(gParams, "DLSSNR.Width", p.mOutputWidth);
    NVSDK_NGX_Parameter_SetUI(gParams, "DLSSNR.Height", p.mOutputHeight);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.MVecScaleX", p.mMotionScaleX);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.MVecScaleY", p.mMotionScaleY);
    NVSDK_NGX_Parameter_SetI(gParams, "DLSSNR.DepthInverted", 0);
    NVSDK_NGX_Parameter_SetI(gParams, "DLSSNR.Reset", p.mReset ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(gParams, "DLSSNR.UseAutoMask", p.mAutoMask > 0 ? 1 : 0);      // always: a parameter keeps its last value, so "not set" would mean "as before"
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.Style", p.mStyle);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.Intensity", p.mIntensity);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.GlobalToneStrength", p.mGlobalTone);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.LocalToneStrength", p.mLocalTone);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.LocalStructureStrength", p.mLocalStructure);
    NVSDK_NGX_Parameter_SetF(gParams, "DLSSNR.SkinStructureStrength", p.mSkinStructure);

    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, gFeature, gParams, nullptr);
    static U32 logged = 0, failures = 0;
    if (!NVSDK_NGX_SUCCEED(r))
    {
        if (failures++ < 5)
        {
            LL_WARNS("VkDlssNr") << "evaluating DLSS-NR failed: " << named(r) << LL_ENDL;
        }
        return false;
    }
    if (logged++ < 3)
    {
        LL_INFOS("VkDlssNr") << "evaluate " << logged << ": colour " << p.mOutputWidth << "x" << p.mOutputHeight << " (format " << p.mColor->mFormat << "), output format " << p.mOutput->mFormat
                             << ", depth and motion " << p.mRenderWidth << "x" << p.mRenderHeight << ", motion scale " << p.mMotionScaleX << ", reset " << p.mReset
                             << ", intensity " << p.mIntensity << ", style " << p.mStyle << LL_ENDL;
    }
    return true;
}

void shutdown()
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    if (gFeature || !gRetired.empty())
    {
        vkDeviceWaitIdle(gDevice);
        if (gFeature)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(gFeature);
            gFeature = nullptr;
        }
        for (const auto& r : gRetired)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(r.first);
        }
        gRetired.clear();
    }
    if (gParams)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(gParams);
        gParams = nullptr;
    }
    gDevice = VK_NULL_HANDLE;
}

#endif // LL_NGX_NR

}

#endif // LL_VULKAN
