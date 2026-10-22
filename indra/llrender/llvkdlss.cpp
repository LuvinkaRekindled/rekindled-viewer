/**
 * @file llvkdlss.cpp
 * @brief NVIDIA DLSS for the Vulkan mirror (see llvkdlss.h)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkdlss.h"

#if defined(LL_DLSS)
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_defs_dlssd.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"
#endif

#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

namespace LLVkDlss
{

using namespace LLVk;       // the loaded Vulkan entry points live there

bool requested()
{
    static const bool on = GetEnvironmentVariableA("LL_VK_DLSS", nullptr, 0) > 0;
    return on;
}

namespace
{
    std::atomic<float> sLodBias(0.f);       // read by the Vulkan thread when it makes a sampler, written when the render scale changes
    bool sLodBiasLocked = false;            // LL_VK_DLSS_LODBIAS is set: it stays

    // the value at start: the environment (the scale the viewer started with, or an explicit bias)
    void initLodBias()
    {
        static const bool done = []() {
            char buf[32] = {};
            if (GetEnvironmentVariableA("LL_VK_DLSS_LODBIAS", buf, sizeof(buf)) > 0)
            {
                sLodBias.store((float)atof(buf));
                sLodBiasLocked = true;
            }
            else if (requested() && GetEnvironmentVariableA("LL_VK_DLSS_SCALE", buf, sizeof(buf)) > 0)
            {
                const float scale = (float)atof(buf);
                if (scale > 0.1f && scale < 1.f)
                {
                    sLodBias.store(log2f(scale));
                }
            }
            return true;
        }();
        (void)done;
    }
}

float lodBias()
{
    initLodBias();
    return sLodBias.load();
}

void setLodBias(float bias)
{
    initLodBias();
    if (!sLodBiasLocked)
    {
        sLodBias.store(bias);
    }
}

#if !defined(LL_DLSS)

void addInstanceExtensions(std::vector<const char*>&) {}
void addDeviceExtensions(VkInstance, VkPhysicalDevice, std::vector<const char*>&) {}
bool init(VkInstance, VkPhysicalDevice, VkDevice)
{
    LL_WARNS("VkDlss") << "this build has no DLSS support (the NVIDIA SDK was not found when it was configured)" << LL_ENDL;
    return false;
}
bool ready() { return false; }
bool rayReconstructionAvailable() { return false; }
void shutdown() {}
bool evaluate(VkCommandBuffer, const Params&) { return false; }
VkImageLayout inputLayout(const LLVk::Image&) { return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
VkImageLayout outputLayout() { return VK_IMAGE_LAYOUT_GENERAL; }

#else

namespace
{

bool gReady = false;
bool gRayReconstruction = false;    // NGX reports the ray reconstruction feature as available
VkDevice gDevice = VK_NULL_HANDLE;
NVSDK_NGX_Parameter* gParams = nullptr;
NVSDK_NGX_Handle* gFeature = nullptr;
U32 gFeatureKey[6] = {};            // render w/h, output w/h, flags, kind (super resolution 0 / ray reconstruction 1 + preset) the feature was created with
std::wstring gDataPath;

// not an NVIDIA-assigned id: NGX takes a custom project id for applications without one
const char* const PROJECT_ID = "5b3a9a0e-6f0e-4c52-9d0f-7b0b3a6f1c42";

std::wstring data_path()
{
    if (gDataPath.empty())
    {
        wchar_t base[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        gDataPath = n ? std::wstring(base) + L"\\FirestormDLSS" : L".";
        CreateDirectoryW(gDataPath.c_str(), nullptr);
    }
    return gDataPath;
}

NVSDK_NGX_FeatureDiscoveryInfo discovery_info(NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling)
{
    NVSDK_NGX_FeatureDiscoveryInfo info = {};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = feature;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = PROJECT_ID;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    static std::wstring path;
    path = data_path();
    info.ApplicationDataPath = path.c_str();
    info.FeatureInfo = nullptr;
    return info;
}

bool ok(NVSDK_NGX_Result r, const char* what)
{
    if (NVSDK_NGX_FAILED(r))
    {
        LL_WARNS("VkDlss") << what << " failed: 0x" << std::hex << (U32)r << std::dec << LL_ENDL;
        return false;
    }
    return true;
}

// Features. A snapshot renders at three or four sizes within a few seconds (the preview, the window, the hi-res image, back to the window): a feature per size is created, and the big ones
// (the 4097x2178 output) cost most of a gigabyte each. Releasing a feature the moment the size changes put the creation (and a wait for the GPU) into every switch; releasing late kept half a dozen
// of them alive at once, and the video memory went over the budget (2026-10-21: 14.4 GB of 12.8, the machine locked up). So: the feature of the size in use, plus up to two of other sizes kept
// for when the size comes back; anything pushed out is released as soon as the frame that last used it has completed (a snapshot records several sizes into one frame: the first size's
// evaluate is still ahead of the GPU when the feature for the second is created).
struct ParkedFeature { U32 mKey[6]; NVSDK_NGX_Handle* mHandle; U64 mLastFrame; };
std::vector<ParkedFeature> gParked;                 // other sizes, kept
std::vector<std::pair<NVSDK_NGX_Handle*, U64>> gRetired;     // pushed out: released when the frame (second) has completed
U64 gFeatureFrame = 0;                              // the frame the current feature last evaluated in
const size_t PARKED_MAX = 3;                        // a snapshot cycles through four sizes (the window, the preview, its upscale, the hi-res image): all of them stay for the burst
const U64 PARKED_IDLE_FRAMES = 360;                 // ... and a parked feature that has not been used for this long (the snapshot is over) is given back

void reap_retired()
{
    Context& ctx = Context::instance();
    const U64 frame_now = ctx.getFrameNumber();
    for (size_t i = 0; i < gParked.size(); )
    {
        if (frame_now > gParked[i].mLastFrame + PARKED_IDLE_FRAMES)
        {
            gRetired.emplace_back(gParked[i].mHandle, gParked[i].mLastFrame);
            gParked.erase(gParked.begin() + i);
        }
        else
        {
            ++i;
        }
    }
    if (gRetired.empty())
    {
        return;
    }
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

// the feature in use goes to the parked ones (the oldest parked one is retired when there are too many)
void park_feature()
{
    if (!gFeature)
    {
        return;
    }
    ParkedFeature pf;
    memcpy(pf.mKey, gFeatureKey, sizeof(pf.mKey));
    pf.mHandle = gFeature;
    pf.mLastFrame = gFeatureFrame;
    gParked.push_back(pf);
    gFeature = nullptr;
    for (U32& k : gFeatureKey) k = 0;
    while (gParked.size() > PARKED_MAX)
    {
        size_t oldest = 0;
        for (size_t i = 1; i < gParked.size(); ++i)
        {
            if (gParked[i].mLastFrame < gParked[oldest].mLastFrame) oldest = i;
        }
        gRetired.emplace_back(gParked[oldest].mHandle, gParked[oldest].mLastFrame);
        gParked.erase(gParked.begin() + oldest);
    }
}

// a parked feature of this size becomes the feature in use
bool take_parked(const U32* key)
{
    for (size_t i = 0; i < gParked.size(); ++i)
    {
        if (memcmp(gParked[i].mKey, key, sizeof(gParked[i].mKey)) == 0)
        {
            gFeature = gParked[i].mHandle;
            memcpy(gFeatureKey, key, sizeof(gFeatureKey));
            gParked.erase(gParked.begin() + i);
            return true;
        }
    }
    return false;
}

// shutdown: everything goes, after the GPU is idle
void release_feature()
{
    vkDeviceWaitIdle(gDevice);
    if (gFeature)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(gFeature);
        gFeature = nullptr;
    }
    for (const ParkedFeature& pf : gParked)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(pf.mHandle);
    }
    gParked.clear();
    for (const auto& r : gRetired)
    {
        NVSDK_NGX_VULKAN_ReleaseFeature(r.first);
    }
    gRetired.clear();
    for (U32& k : gFeatureKey) k = 0;
}

NVSDK_NGX_PerfQuality_Value quality_for(U32 render_w, U32 out_w)
{
    const float scale = out_w ? (float)render_w / (float)out_w : 1.f;
    if (scale >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (scale >= 0.64f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (scale >= 0.55f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (scale >= 0.40f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

void add_all(std::vector<const char*>& out, uint32_t count, VkExtensionProperties* props, const std::vector<VkExtensionProperties>* available)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const char* name = props[i].extensionName;
        bool have = false;
        for (const char* e : out) have = have || strcmp(e, name) == 0;
        bool exists = true;
        if (available)
        {
            exists = false;
            for (const auto& a : *available) exists = exists || strcmp(a.extensionName, name) == 0;
        }
        if (!have && exists)
        {
            // NGX owns the strings and may reuse the memory on the next query (C6 asks twice: super resolution and ray reconstruction): keep our own copy
            static std::deque<std::string> sNames;
            sNames.push_back(name);
            out.push_back(sNames.back().c_str());
            LL_INFOS("VkDlss") << "enabling extension " << name << " for DLSS" << LL_ENDL;
        }
        else if (!exists)
        {
            LL_WARNS("VkDlss") << "DLSS wants extension " << name << " which is not available" << LL_ENDL;
        }
    }
}

}

void addInstanceExtensions(std::vector<const char*>& out)
{
    NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info();
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (ok(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props), "NGX instance extension query"))
    {
        std::vector<VkExtensionProperties> available;
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        available.resize(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
        add_all(out, count, props, &available);
    }
}

void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out)
{
    std::vector<VkExtensionProperties> available;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &n, nullptr);
    available.resize(n);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &n, available.data());
    // C6: super resolution first, then ray reconstruction (its extensions are enabled too so that the capability query in init() can say yes)
    for (NVSDK_NGX_Feature feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_RayReconstruction })
    {
        NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info(feature);
        uint32_t count = 0;
        VkExtensionProperties* props = nullptr;
        if (ok(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, device, &info, &count, &props), feature == NVSDK_NGX_Feature_SuperSampling ? "NGX device extension query" : "NGX ray reconstruction extension query"))
        {
            add_all(out, count, props, &available);
        }
    }
}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gReady)
    {
        return true;
    }
    NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info();
    NVSDK_NGX_FeatureRequirement req = {};
    if (ok(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &info, &req), "NGX feature requirements"))
    {
        if (req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported)
        {
            LL_WARNS("VkDlss") << "DLSS is not supported on this system (reasons 0x" << std::hex << (U32)req.FeatureSupported << std::dec
                               << ", needs hardware architecture " << req.MinHWArchitecture << ", OS " << req.MinOSVersion << ")" << LL_ENDL;
            return false;
        }
    }

    const std::wstring path = data_path();
    if (!ok(NVSDK_NGX_VULKAN_Init_with_ProjectID(PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", path.c_str(), instance, physical, device,
                                                 vkGetInstanceProcAddr, vkGetDeviceProcAddr),
            "NVSDK_NGX_VULKAN_Init"))
    {
        return false;
    }
    gDevice = device;
    if (!ok(NVSDK_NGX_VULKAN_GetCapabilityParameters(&gParams), "NGX capability parameters"))
    {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(gParams, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(gParams, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (!available)
    {
        unsigned int major = 0, minor = 0;
        NVSDK_NGX_Parameter_GetUI(gParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        NVSDK_NGX_Parameter_GetUI(gParams, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        LL_WARNS("VkDlss") << "DLSS is not available" << (needs_driver ? ": the driver is too old, needs " : "") << (needs_driver ? std::to_string(major) + "." + std::to_string(minor) : std::string())
                           << " (is nvngx_dlss.dll next to the viewer?)" << LL_ENDL;
        NVSDK_NGX_VULKAN_Shutdown1(device);
        gParams = nullptr;
        return false;
    }
    // C6: ray reconstruction (a separate NGX feature with its own DLL, nvngx_dlssd.dll); nothing uses it yet, the answer goes into the capabilities
    {
        int rr = 0, rr_needs_driver = 0;
        NVSDK_NGX_Parameter_GetI(gParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rr);
        NVSDK_NGX_Parameter_GetI(gParams, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rr_needs_driver);
        NVSDK_NGX_FeatureDiscoveryInfo rr_info = discovery_info(NVSDK_NGX_Feature_RayReconstruction);
        NVSDK_NGX_FeatureRequirement rr_req = {};
        const bool rr_req_ok = NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &rr_info, &rr_req));
        gRayReconstruction = rr != 0;
        LL_INFOS("VkDlss") << "ray reconstruction: " << (gRayReconstruction ? "available" : "not available") << (rr_needs_driver ? " (needs a newer driver)" : "")
                           << (rr_req_ok ? (rr_req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported ? ", hardware and system supported" : ", not supported by the system: reasons 0x") : ", requirement query failed")
                           << (rr_req_ok && rr_req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported ? std::to_string((U32)rr_req.FeatureSupported) : std::string()) << LL_ENDL;
    }
    // the capability object is owned by NGX; evaluation needs one of our own
    gParams = nullptr;
    if (!ok(NVSDK_NGX_VULKAN_AllocateParameters(&gParams), "NGX parameter allocation"))
    {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    gReady = true;
    LL_INFOS("VkDlss") << "DLSS is ready" << LL_ENDL;
    return true;
}

bool ready()
{
    return gReady;
}

bool rayReconstructionAvailable()
{
    return gReady && gRayReconstruction;
}

void shutdown()
{
    if (!gReady)
    {
        return;
    }
    release_feature();
    if (gParams)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(gParams);
        gParams = nullptr;
    }
    NVSDK_NGX_VULKAN_Shutdown1(gDevice);
    gReady = false;
}

VkImageLayout inputLayout(const LLVk::Image& image)
{
    return (image.mAspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageLayout outputLayout()
{
    return VK_IMAGE_LAYOUT_GENERAL;
}

namespace
{
float env_float(const char* name)
{
    char buf[32] = {};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 ? (float)atof(buf) : 1.f;
}

NVSDK_NGX_Resource_VK resource_of(const LLVk::Image& image, bool write, VkImageAspectFlags aspect)
{
    const VkImageSubresourceRange range = { aspect, 0, 1, 0, 1 };
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.mView, image.mImage, range, image.mFormat, image.mExtent.width, image.mExtent.height, write);
}
}

bool evaluate(VkCommandBuffer cmd, const Params& p)
{
    if (!gReady || !p.mColor || !p.mDepth || !p.mMotion || !p.mOutput)
    {
        return false;
    }
    const bool rr = p.mRayReconstruction;
    if (rr && (!gRayReconstruction || !p.mDiffuseAlbedo || !p.mSpecularAlbedo || !p.mNormals))
    {
        return false;
    }
    const U32 flags = (p.mHdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const U32 key[6] = { p.mRenderWidth, p.mRenderHeight, p.mOutputWidth, p.mOutputHeight, flags, rr ? (1u | ((U32)p.mPreset << 8)) : 0u };
    reap_retired();
    if (!gFeature || memcmp(key, gFeatureKey, sizeof(key)) != 0)
    {
        park_feature();
        if (!take_parked(key))
        {
        const NVSDK_NGX_PerfQuality_Value quality = quality_for(p.mRenderWidth, p.mOutputWidth);
        if (rr)
        {
            // the model the network runs: one hint per quality mode (the SDK picks the one of the mode the feature is created with)
            const U32 preset = (U32)p.mPreset;
            for (const char* hint : { NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                                      NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                                      NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality })
            {
                NVSDK_NGX_Parameter_SetUI(gParams, hint, preset);
            }
            NVSDK_NGX_DLSSD_Create_Params create = {};
            create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;      // the roughness travels in the w of the normals
            create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
            create.InWidth = p.mRenderWidth;
            create.InHeight = p.mRenderHeight;
            create.InTargetWidth = p.mOutputWidth;
            create.InTargetHeight = p.mOutputHeight;
            create.InPerfQualityValue = quality;
            create.InFeatureCreateFlags = (int)flags;
            create.InEnableOutputSubrects = false;
            if (!ok(NGX_VULKAN_CREATE_DLSSD_EXT1(gDevice, cmd, 1, 1, &gFeature, gParams, &create), "creating the ray reconstruction feature"))
            {
                gFeature = nullptr;
                return false;
            }
            LL_INFOS("VkDlss") << "ray reconstruction feature created: " << p.mRenderWidth << "x" << p.mRenderHeight << " -> " << p.mOutputWidth << "x" << p.mOutputHeight
                               << " quality " << (S32)quality << " preset " << p.mPreset << LL_ENDL;
        }
        else
        {
            NVSDK_NGX_DLSS_Create_Params create = {};
            create.Feature.InWidth = p.mRenderWidth;
            create.Feature.InHeight = p.mRenderHeight;
            create.Feature.InTargetWidth = p.mOutputWidth;
            create.Feature.InTargetHeight = p.mOutputHeight;
            create.Feature.InPerfQualityValue = quality;
            create.InFeatureCreateFlags = (int)flags;
            if (!ok(NGX_VULKAN_CREATE_DLSS_EXT1(gDevice, cmd, 1, 1, &gFeature, gParams, &create), "creating the DLSS feature"))
            {
                gFeature = nullptr;
                return false;
            }
            LL_INFOS("VkDlss") << "DLSS feature created: " << p.mRenderWidth << "x" << p.mRenderHeight << " -> " << p.mOutputWidth << "x" << p.mOutputHeight
                               << " quality " << (S32)quality << LL_ENDL;
        }
        memcpy(gFeatureKey, key, sizeof(key));
        }
    }
    gFeatureFrame = Context::instance().getFrameNumber();

    NVSDK_NGX_Resource_VK color = resource_of(*p.mColor, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK depth = resource_of(*p.mDepth, false, VK_IMAGE_ASPECT_DEPTH_BIT);
    NVSDK_NGX_Resource_VK motion = resource_of(*p.mMotion, false, VK_IMAGE_ASPECT_COLOR_BIT);
    NVSDK_NGX_Resource_VK output = resource_of(*p.mOutput, true, VK_IMAGE_ASPECT_COLOR_BIT);

    // LL_VK_DLSS_MVX / MVY / JX / JY (default 1): multipliers for the motion vector and jitter signs, to find out which way round DLSS wants them without rebuilding
    static const float mv_x = env_float("LL_VK_DLSS_MVX"), mv_y = env_float("LL_VK_DLSS_MVY"), j_x = env_float("LL_VK_DLSS_JX"), j_y = env_float("LL_VK_DLSS_JY");
    static U32 logged = 0;
    if (logged++ < 3)
    {
        LL_INFOS("VkDlss") << "evaluate " << logged << ": jitter " << p.mJitterX << "," << p.mJitterY << " reset " << p.mReset << " sign multipliers mv " << mv_x << "," << mv_y
                           << " jitter " << j_x << "," << j_y << LL_ENDL;
    }

    if (rr)
    {
        NVSDK_NGX_Resource_VK diffuse = resource_of(*p.mDiffuseAlbedo, false, VK_IMAGE_ASPECT_COLOR_BIT);
        NVSDK_NGX_Resource_VK specular = resource_of(*p.mSpecularAlbedo, false, VK_IMAGE_ASPECT_COLOR_BIT);
        NVSDK_NGX_Resource_VK normals = resource_of(*p.mNormals, false, VK_IMAGE_ASPECT_COLOR_BIT);
        NVSDK_NGX_Resource_VK exposure = {};
        NVSDK_NGX_VK_DLSSD_Eval_Params eval = {};
        if (p.mExposure)
        {
            exposure = resource_of(*p.mExposure, false, VK_IMAGE_ASPECT_COLOR_BIT);
            eval.pInExposureTexture = &exposure;
        }
        NVSDK_NGX_Resource_VK refl_motion = {};
        if (p.mReflectionMotion)
        {
            refl_motion = resource_of(*p.mReflectionMotion, false, VK_IMAGE_ASPECT_COLOR_BIT);
            eval.pInMotionVectorsReflections = &refl_motion;
        }
        NVSDK_NGX_Resource_VK hit_distance = {};
        float world_to_view[16], view_to_clip[16];
        if (p.mHitDistance)
        {
            hit_distance = resource_of(*p.mHitDistance, false, VK_IMAGE_ASPECT_COLOR_BIT);
            memcpy(world_to_view, p.mWorldToView, sizeof(world_to_view));
            memcpy(view_to_clip, p.mViewToClip, sizeof(view_to_clip));
            eval.pInSpecularHitDistance = &hit_distance;
            eval.pInWorldToViewMatrix = world_to_view;
            eval.pInViewToClipMatrix = view_to_clip;
        }
        eval.pInDiffuseAlbedo = &diffuse;
        eval.pInSpecularAlbedo = &specular;
        eval.pInNormals = &normals;
        eval.pInColor = &color;
        eval.pInOutput = &output;
        eval.pInDepth = &depth;
        eval.pInMotionVectors = &motion;
        eval.InJitterOffsetX = p.mJitterX * j_x;
        eval.InJitterOffsetY = p.mJitterY * j_y;
        eval.InRenderSubrectDimensions = { p.mRenderWidth, p.mRenderHeight };
        eval.InReset = p.mReset ? 1 : 0;
        eval.InMVScaleX = p.mMotionScaleX * mv_x;
        eval.InMVScaleY = p.mMotionScaleY * mv_y;
        return ok(NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, gFeature, gParams, &eval), "evaluating ray reconstruction");
    }

    NVSDK_NGX_VK_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = &color;
    eval.Feature.pInOutput = &output;
    eval.pInDepth = &depth;
    eval.pInMotionVectors = &motion;
    eval.InJitterOffsetX = p.mJitterX * j_x;
    eval.InJitterOffsetY = p.mJitterY * j_y;
    eval.InRenderSubrectDimensions = { p.mRenderWidth, p.mRenderHeight };
    eval.InReset = p.mReset ? 1 : 0;
    eval.InMVScaleX = p.mMotionScaleX * mv_x;
    eval.InMVScaleY = p.mMotionScaleY * mv_y;
    return ok(NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, gFeature, gParams, &eval), "evaluating DLSS");
}

#endif // LL_DLSS

}

#endif // LL_VULKAN
