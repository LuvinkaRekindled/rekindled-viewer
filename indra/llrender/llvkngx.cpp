/**
 * @file llvkngx.cpp
 * @brief The NVIDIA NGX bootstrap for DLSS-NR, local builds only (see llvkngx.h)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkngx.h"
#include "llvkdlssnr.h"

#if defined(LL_NGX_NR)
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#endif

#include <vector>
#include <deque>
#include <string>

namespace LLVkNgx
{

using namespace LLVk;       // the loaded Vulkan entry points live there

#if !defined(LL_NGX_NR)

void addInstanceExtensions(std::vector<const char*>&) {}
void addDeviceExtensions(VkInstance, VkPhysicalDevice, std::vector<const char*>&) {}
bool init(VkInstance, VkPhysicalDevice, VkDevice)
{
    if (LLVkDlssNr::requested() > 0)
    {
        LL_WARNS("VkNgx") << "LL_VK_DLSS_NR is set but this build has no NGX support (DLSS-NR is a local build option: REKINDLED_LOCAL_NR)" << LL_ENDL;
    }
    return false;
}
bool ready() { return false; }
void shutdown() {}
VkImageLayout inputLayout(const LLVk::Image&) { return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
VkImageLayout outputLayout() { return VK_IMAGE_LAYOUT_GENERAL; }

#else

namespace
{

bool gReady = false;
VkDevice gDevice = VK_NULL_HANDLE;
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

NVSDK_NGX_FeatureDiscoveryInfo discovery_info(NVSDK_NGX_Feature feature)
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

NVSDK_NGX_Feature nr_feature()
{
    return (NVSDK_NGX_Feature)LLVkDlssNr::featureId();
}

bool ok(NVSDK_NGX_Result r, const char* what)
{
    if (NVSDK_NGX_FAILED(r))
    {
        LL_WARNS("VkNgx") << what << " failed: 0x" << std::hex << (U32)r << std::dec << LL_ENDL;
        return false;
    }
    return true;
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
            // NGX owns the strings and may reuse the memory on the next query: keep our own copy
            static std::deque<std::string> sNames;
            sNames.push_back(name);
            out.push_back(sNames.back().c_str());
            LL_INFOS("VkNgx") << "enabling extension " << name << " for DLSS-NR" << LL_ENDL;
        }
        else if (!exists)
        {
            LL_WARNS("VkNgx") << "DLSS-NR wants extension " << name << " which is not available" << LL_ENDL;
        }
    }
}

}

void addInstanceExtensions(std::vector<const char*>& out)
{
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info(nr_feature());
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props);
    if (NVSDK_NGX_FAILED(r))
    {
        // (the instance extensions are the same for every feature: ask for the plain one when the core does not know 18 here)
        info = discovery_info(NVSDK_NGX_Feature_SuperSampling);
        r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props);
    }
    if (ok(r, "NGX instance extension query"))
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
    NVSDK_NGX_FeatureDiscoveryInfo info = discovery_info(nr_feature());
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (ok(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, device, &info, &count, &props), "NGX DLSS-NR extension query"))
    {
        add_all(out, count, props, &available);
    }
}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gReady)
    {
        return true;
    }
    const std::wstring path = data_path();
    if (!ok(NVSDK_NGX_VULKAN_Init_with_ProjectID(PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", path.c_str(), instance, physical, device,
                                                 vkGetInstanceProcAddr, vkGetDeviceProcAddr),
            "NVSDK_NGX_VULKAN_Init"))
    {
        return false;
    }
    gDevice = device;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    if (!ok(NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities), "NGX capability parameters"))
    {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    // DLSS-NR (doc/vulkan/PLAN-dlss-nr.md): whether the feature is usable here (the capability object is owned by NGX); the probe logs everything the core says about feature 18 when LL_VK_DLSS_NR is set
    LLVkDlssNr::queryAvailability(instance, physical, device, capabilities, discovery_info(NVSDK_NGX_Feature_SuperSampling));
    if (LLVkDlssNr::requested() > 0)
    {
        LLVkDlssNr::probe(instance, physical, device, capabilities, discovery_info(NVSDK_NGX_Feature_SuperSampling));
    }
    if (!LLVkDlssNr::available() && LLVkDlssNr::requested() == 0)
    {
        NVSDK_NGX_VULKAN_Shutdown1(device);
        LL_INFOS("VkNgx") << "DLSS-NR is not available here: NGX is shut down again" << LL_ENDL;
        return false;
    }
    gReady = true;
    LL_INFOS("VkNgx") << "NGX is ready for DLSS-NR (local build)" << LL_ENDL;
    return true;
}

bool ready()
{
    return gReady;
}

void shutdown()
{
    if (!gReady)
    {
        return;
    }
    vkDeviceWaitIdle(gDevice);
    LLVkDlssNr::shutdown();
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

#endif // LL_NGX_NR

}

#endif // LL_VULKAN
