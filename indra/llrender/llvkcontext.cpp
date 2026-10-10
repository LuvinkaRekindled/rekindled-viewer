/**
 * @file llvkcontext.cpp
 * @brief Vulkan device layer for the experimental Vulkan backend (see llvkcontext.h)
 */

#include "linden_common.h"

#include "llvkcontext.h"
#include "llvkdlss.h"

#if defined(LL_VULKAN)

#include <algorithm>
#include <chrono>
#include <cstring>

namespace LLVk
{

#define LL_VK_GLOBAL_FN(name) PFN_##name name = nullptr;
#define LL_VK_INSTANCE_FN(name) PFN_##name name = nullptr;
#define LL_VK_DEVICE_FN(name) PFN_##name name = nullptr;
#include "llvkfunctions.inl"
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
const char* gDebugProgram = nullptr;
bool gDeviceLost = false;

namespace
{

bool vk_ok(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
    {
        if (r == VK_ERROR_DEVICE_LOST)
        {
            gDeviceLost = true;
        }
        LL_WARNS("Vulkan") << what << " failed: VkResult " << (S32)r << LL_ENDL;
        return false;
    }
    return true;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const auto& e : list)
    {
        if (strcmp(e.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LL_WARNS("VulkanValidation") << "ERROR [draw: " << (gDebugProgram ? gDebugProgram : "-") << "]: " << data->pMessage << LL_ENDL;
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        LL_WARNS("VulkanValidation") << "[draw: " << (gDebugProgram ? gDebugProgram : "-") << "] " << data->pMessage << LL_ENDL;
    }
    return VK_FALSE;
}

// the feature structures the backend queries and enables, chained together
struct FeatureChain
{
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };

    // C6: ray tracing. Chained after the others only for extensions the device has (a struct of an extension the device lacks must not be in the chain)
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeline = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR posFetch = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR };
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR maint1 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR };
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV reorder = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV };
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT };
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV cluster = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV };

    FeatureChain()
    {
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &eds3;
        eds3.pNext = &vids;
    }
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;

    // appends the ray tracing structs of the extensions in 'rt' (the ones for the vendor / later extensions only when 'all' is set: they are queried, not enabled)
    void addRayTracing(const Capabilities::RayTracing& rt, bool all)
    {
        void** tail = &vids.pNext;
        auto link = [&](bool have, void* s, void** s_next) { if (have) { *tail = s; tail = s_next; } };
        link(rt.mExtAccel, &accel, &accel.pNext);
        link(rt.mExtRayQuery, &rayQuery, &rayQuery.pNext);
        link(rt.mExtPipeline, &rtPipeline, &rtPipeline.pNext);
        link(rt.mExtPositionFetch, &posFetch, &posFetch.pNext);
        link(rt.mExtMaintenance1, &maint1, &maint1.pNext);
        link(all && rt.mExtReorder, &reorder, &reorder.pNext);
        link(all && rt.mExtOpacityMicromap, &omm, &omm.pNext);
        link(all && rt.mExtCluster, &cluster, &cluster.pNext);
    }
};

// ray tracing is switched on at device creation only on request (LL_VK_RT=1): until the renderer uses it, nothing about the device should differ from a build without it
bool rayTracingRequested()
{
    static const bool on = []() { char b[8] = {}; return GetEnvironmentVariableA("LL_VK_RT", b, sizeof(b)) > 0 && b[0] != '0'; }();
    return on;
}

// the largest sample count in a VkSampleCountFlags
U32 max_sample_count(VkSampleCountFlags counts)
{
    U32 best = 1;
    for (U32 c : { 2u, 4u, 8u, 16u, 32u, 64u }) if (counts & c) best = c;
    return best;
}

const char* const REQUIRED_DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

// returns a description of the first missing requirement, or nullptr if the device qualifies
const char* missing_feature(const FeatureChain& c)
{
    if (!c.f13.dynamicRendering) return "dynamicRendering";
    if (!c.f13.synchronization2) return "synchronization2";
    if (!c.f12.hostQueryReset) return "hostQueryReset";
    if (!c.eds3.extendedDynamicState3PolygonMode) return "extendedDynamicState3PolygonMode";
    if (!c.eds3.extendedDynamicState3ColorBlendEnable) return "extendedDynamicState3ColorBlendEnable";
    if (!c.eds3.extendedDynamicState3ColorBlendEquation) return "extendedDynamicState3ColorBlendEquation";
    if (!c.eds3.extendedDynamicState3ColorWriteMask) return "extendedDynamicState3ColorWriteMask";
    if (!c.vids.vertexInputDynamicState) return "vertexInputDynamicState";
    if (!c.f2.features.fillModeNonSolid) return "fillModeNonSolid";
    if (!c.f2.features.independentBlend) return "independentBlend";
    if (!c.f2.features.imageCubeArray) return "imageCubeArray";
    return nullptr;
}

} // anonymous namespace

//-----------------------------------------------------------------------------------------------------
// Context
//-----------------------------------------------------------------------------------------------------

VkImageAspectFlags aspectForFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

size_t Context::pendingDestroys() const
{
    size_t n = 0;
    for (const Frame& f : mFrames) n += f.mDeferred.size();
    return n;
}

U64 Context::deviceLocalBytes() const
{
    if (!mAllocator)
    {
        return 0;
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    U64 total = 0;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            total += props->memoryHeaps[i].size;
        }
    }
    return total;
}

std::string Context::memoryBudgetText()
{
    if (!mAllocator)
    {
        return "no allocator";
    }
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(mAllocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    std::string text;
    for (U32 i = 0; props && i < props->memoryHeapCount; ++i)
    {
        text += "heap " + std::to_string(i) + ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (video) " : " (host) ")
              + std::to_string(budgets[i].usage / (1024 * 1024)) + "/" + std::to_string(budgets[i].budget / (1024 * 1024)) + " MB, allocated by us "
              + std::to_string(budgets[i].statistics.allocationBytes / (1024 * 1024)) + " MB in " + std::to_string(budgets[i].statistics.allocationCount) + " allocations; ";
    }
    return text;
}

void Context::setObjectName(VkObjectType type, U64 handle, const std::string& name)
{
    if (mMessenger == VK_NULL_HANDLE || mDevice == VK_NULL_HANDLE)
    {
        return;
    }
    static PFN_vkSetDebugUtilsObjectNameEXT set_name = nullptr;
    if (!set_name)
    {
        set_name = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(mInstance, "vkSetDebugUtilsObjectNameEXT");
    }
    if (set_name)
    {
        VkDebugUtilsObjectNameInfoEXT info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name.c_str();
        set_name(mDevice, &info);
    }
}

Context& Context::instance()
{
    static Context sContext;
    return sContext;
}

bool Context::loadLoader()
{
    mLoader = LoadLibraryA("vulkan-1.dll");
    if (!mLoader)
    {
        LL_WARNS("Vulkan") << "vulkan-1.dll not found" << LL_ENDL;
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(mLoader, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr)
    {
        return false;
    }
#define LL_VK_GLOBAL_FN(name) name = (PFN_##name)vkGetInstanceProcAddr(nullptr, #name);
#define LL_VK_INSTANCE_FN(name)
#define LL_VK_DEVICE_FN(name)
#include "llvkfunctions.inl"
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}

bool Context::createInstance()
{
    U32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
    {
        vkEnumerateInstanceVersion(&version);
    }
    LL_INFOS("Vulkan") << "Vulkan loader version " << VK_API_VERSION_MAJOR(version) << "." << VK_API_VERSION_MINOR(version) << "."
                       << VK_API_VERSION_PATCH(version) << LL_ENDL;
    if (version < VK_API_VERSION_1_3)
    {
        LL_WARNS("Vulkan") << "Vulkan 1.3 is required" << LL_ENDL;
        return false;
    }

    U32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> wanted = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    if (LLVkDlss::requested())
    {
        LLVkDlss::addInstanceExtensions(wanted);
    }
    for (const char* name : wanted)
    {
        if (!has_extension(available, name))
        {
            LL_WARNS("Vulkan") << "missing instance extension " << name << LL_ENDL;
            return false;
        }
    }
    const bool want_validation = GetEnvironmentVariableA("LL_VK_VALIDATION", nullptr, 0) > 0;
    const bool debug_utils = want_validation && has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug_utils)
    {
        wanted.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // validation only when asked for (the layer must be findable through VK_LAYER_PATH)
    std::vector<const char*> layers;
    if (want_validation)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        LL_INFOS("Vulkan") << "validation layer requested" << LL_ENDL;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Firestorm";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (U32)wanted.size();
    ci.ppEnabledExtensionNames = wanted.data();
    ci.enabledLayerCount = (U32)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    if (!vk_ok(vkCreateInstance(&ci, nullptr, &mInstance), "vkCreateInstance"))
    {
        return false;
    }

#define LL_VK_GLOBAL_FN(name)
#define LL_VK_INSTANCE_FN(name) name = (PFN_##name)vkGetInstanceProcAddr(mInstance, #name);
#define LL_VK_DEVICE_FN(name)
#include "llvkfunctions.inl"

    if (debug_utils)
    {
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger)
        {
            VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            create_messenger(mInstance, &mi, nullptr, &mMessenger);
        }
    }

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = mWindow;
    return vk_ok(vkCreateWin32SurfaceKHR(mInstance, &sci, nullptr, &mSurface), "vkCreateWin32SurfaceKHR");
}

bool Context::pickDevice()
{
    U32 count = 0;
    vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

    S32 best_score = -1;
    for (VkPhysicalDevice dev : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        LL_INFOS("Vulkan") << "device: " << props.deviceName << " (api " << VK_API_VERSION_MAJOR(props.apiVersion) << "."
                           << VK_API_VERSION_MINOR(props.apiVersion) << "." << VK_API_VERSION_PATCH(props.apiVersion) << ")" << LL_ENDL;
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            LL_INFOS("Vulkan") << "  rejected: needs Vulkan 1.3" << LL_ENDL;
            continue;
        }

        U32 ext_count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &ext_count, exts.data());
        const char* missing_ext = nullptr;
        for (const char* name : REQUIRED_DEVICE_EXTENSIONS)
        {
            if (!has_extension(exts, name))
            {
                missing_ext = name;
                break;
            }
        }
        if (missing_ext)
        {
            LL_INFOS("Vulkan") << "  rejected: missing extension " << missing_ext << LL_ENDL;
            continue;
        }

        FeatureChain chain;
        Capabilities::RayTracing rt;
        rt.mExtAccel = has_extension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        rt.mExtDeferredHost = has_extension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        rt.mExtRayQuery = has_extension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        rt.mExtPipeline = has_extension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        rt.mExtPositionFetch = has_extension(exts, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
        rt.mExtMaintenance1 = has_extension(exts, VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
        rt.mExtReorder = has_extension(exts, VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
        rt.mExtOpacityMicromap = has_extension(exts, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        rt.mExtCluster = has_extension(exts, VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        chain.addRayTracing(rt, true);
        vkGetPhysicalDeviceFeatures2(dev, &chain.f2);
        if (const char* missing = missing_feature(chain))
        {
            LL_INFOS("Vulkan") << "  rejected: missing feature " << missing << LL_ENDL;
            continue;
        }

        U32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, families.data());
        for (U32 i = 0; i < qcount; ++i)
        {
            const VkQueueFlags f = families[i].queueFlags;
            LL_INFOS("Vulkan") << "  queue family " << i << ": " << families[i].queueCount << " queue(s)" << ((f & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "") << ((f & VK_QUEUE_COMPUTE_BIT) ? " compute" : "")
                               << ((f & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "") << ((f & VK_QUEUE_SPARSE_BINDING_BIT) ? " sparse" : "") << ((f & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ? " video-decode" : "")
                               << ((f & VK_QUEUE_OPTICAL_FLOW_BIT_NV) ? " optical-flow" : "") << LL_ENDL;
        }
        S32 family = -1;
        for (U32 i = 0; i < qcount; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, mSurface, &present);
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            if ((families[i].queueFlags & need) == need && present)
            {
                family = (S32)i;
                break;
            }
        }
        if (family < 0)
        {
            LL_INFOS("Vulkan") << "  rejected: no graphics+compute+present queue" << LL_ENDL;
            continue;
        }

        S32 score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
        if (score > best_score)
        {
            best_score = score;
            mPhysical = dev;
            mQueueFamily = (U32)family;

            mCaps = Capabilities();
            strncpy(mCaps.mDeviceName, props.deviceName, sizeof(mCaps.mDeviceName) - 1);
            mCaps.mApiVersion = props.apiVersion;
            mCaps.mDriverVersion = props.driverVersion;
            mCaps.mVendorId = props.vendorID;
            mCaps.mDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            mCaps.mMemoryBudget = has_extension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            mCaps.mWideLines = chain.f2.features.wideLines != VK_FALSE;
            mCaps.mDepthClamp = chain.f2.features.depthClamp != VK_FALSE && chain.eds3.extendedDynamicState3DepthClampEnable != VK_FALSE;
            mCaps.mAnisotropy = chain.f2.features.samplerAnisotropy != VK_FALSE;
            mCaps.mBC = chain.f2.features.textureCompressionBC != VK_FALSE;
            mCaps.mGeometryShader = chain.f2.features.geometryShader != VK_FALSE;
            mCaps.mMultiDrawIndirect = chain.f2.features.multiDrawIndirect != VK_FALSE;
            mCaps.mBufferDeviceAddress = chain.f12.bufferDeviceAddress != VK_FALSE;
            mCaps.mDrawIndirectFirstInstance = chain.f2.features.drawIndirectFirstInstance != VK_FALSE;
            mCaps.mMaxAnisotropy = props.limits.maxSamplerAnisotropy;
            mCaps.mTimestampPeriodNs = props.limits.timestampPeriod;
            mCaps.mTimestampValidBits = families[(U32)family].timestampValidBits;
            mCaps.mUniformAlignment = props.limits.minUniformBufferOffsetAlignment;
            mCaps.mMaxImageDim2D = props.limits.maxImageDimension2D;
            mCaps.mMaxImageDimCube = props.limits.maxImageDimensionCube;
            mCaps.mMaxImageDim3D = props.limits.maxImageDimension3D;
            mCaps.mMaxArrayLayers = props.limits.maxImageArrayLayers;
            mCaps.mMaxColorAttachments = props.limits.maxColorAttachments;
            mCaps.mMaxVertexAttribs = props.limits.maxVertexInputAttributes;
            mCaps.mMaxUniformRange = props.limits.maxUniformBufferRange;
            mCaps.mMaxSampledImagesPerStage = props.limits.maxPerStageDescriptorSampledImages;
            mCaps.mLineWidthRange[0] = props.limits.lineWidthRange[0];
            mCaps.mLineWidthRange[1] = props.limits.lineWidthRange[1];
            mCaps.mMaxViewport[0] = props.limits.maxViewportDimensions[0];
            mCaps.mMaxViewport[1] = props.limits.maxViewportDimensions[1];
            mCaps.mTimelineSemaphore = chain.f12.timelineSemaphore != VK_FALSE;
            mCaps.mGraphicsQueues = families[(U32)family].queueCount;
            for (U32 i = 0; i < qcount; ++i)
            {
                const VkQueueFlags f = families[i].queueFlags;
                if (!(f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT) && mCaps.mComputeFamily < 0)
                {
                    mCaps.mComputeFamily = (S32)i;
                    mCaps.mComputeQueues = families[i].queueCount;
                }
                // a transfer-only family: not a graphics / compute one, not video decode or optical flow
                if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_OPTICAL_FLOW_BIT_NV)) && mCaps.mTransferFamily < 0)
                {
                    mCaps.mTransferFamily = (S32)i;
                    mCaps.mTransferQueues = families[i].queueCount;
                }
            }
            LL_INFOS("Vulkan") << "queues: renderer family " << family << " has " << mCaps.mGraphicsQueues << "; async compute family " << mCaps.mComputeFamily << " (" << mCaps.mComputeQueues
                << " queues); transfer-only family " << mCaps.mTransferFamily << " (" << mCaps.mTransferQueues << "); timeline semaphores " << mCaps.mTimelineSemaphore << LL_ENDL;
            mCaps.mMaxSampleCount = max_sample_count(props.limits.framebufferColorSampleCounts);
            // C6: the limits a GL driver reports that have no one-to-one Vulkan limit
            mCaps.mMaxColorTextureSamples = max_sample_count(props.limits.sampledImageColorSampleCounts);
            mCaps.mMaxDepthTextureSamples = max_sample_count(props.limits.sampledImageDepthSampleCounts);
            mCaps.mMaxIntegerSamples = max_sample_count(props.limits.sampledImageIntegerSampleCounts);
            mCaps.mMaxSampleMaskWords = std::max(1u, (mCaps.mMaxSampleCount + 31) / 32);      // one 32 bit word per 32 samples
            // GL counts the vectors a vertex shader hands on without the position (a driver with 128 output components reports 31)
            mCaps.mMaxVaryingVectors = std::min(props.limits.maxVertexOutputComponents, props.limits.maxFragmentInputComponents) / 4 - 1;
            // GL_MAX_ELEMENTS_VERTICES / INDICES are a recommendation for glDrawRangeElements (NVIDIA says 1048575), not a limit; Vulkan only has the largest index value.
            // The viewer batches grass quads up to this size, so it must not read 0.
            mCaps.mRecommendedMaxVertices = mCaps.mRecommendedMaxIndices = (U32)std::min<U64>(1048575u, props.limits.maxDrawIndexedIndexValue);

            VkPhysicalDevicePushDescriptorPropertiesKHR push = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
            VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
            VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_pipe_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
            VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props2.pNext = &push;
            push.pNext = &p12;
            void** props_tail = &p12.pNext;
            if (rt.mExtAccel)
            {
                *props_tail = &accel_props;
                props_tail = &accel_props.pNext;
            }
            if (rt.mExtPipeline)
            {
                *props_tail = &rt_pipe_props;
                props_tail = &rt_pipe_props.pNext;
            }
            vkGetPhysicalDeviceProperties2(dev, &props2);
            mCaps.mMaxPushDescriptors = push.maxPushDescriptors;

            // C6: ray tracing as the device reports it (the feature structs were filled by the query above)
            rt.mAccelStruct = rt.mExtAccel && rt.mExtDeferredHost && chain.accel.accelerationStructure != VK_FALSE;
            rt.mRayQuery = rt.mExtRayQuery && chain.rayQuery.rayQuery != VK_FALSE;
            rt.mPipeline = rt.mExtPipeline && chain.rtPipeline.rayTracingPipeline != VK_FALSE;
            rt.mPositionFetch = rt.mExtPositionFetch && chain.posFetch.rayTracingPositionFetch != VK_FALSE;
            rt.mMaintenance1 = rt.mExtMaintenance1 && chain.maint1.rayTracingMaintenance1 != VK_FALSE;
            rt.mReorder = rt.mExtReorder && chain.reorder.rayTracingInvocationReorder != VK_FALSE;
            rt.mOpacityMicromap = rt.mExtOpacityMicromap && chain.omm.micromap != VK_FALSE;
            rt.mClusterAccel = rt.mExtCluster && chain.cluster.clusterAccelerationStructure != VK_FALSE;
            rt.mUsable = rt.mAccelStruct && rt.mRayQuery && mCaps.mBufferDeviceAddress;
            if (rt.mExtAccel)
            {
                rt.mMaxGeometryCount = accel_props.maxGeometryCount;
                rt.mMaxInstanceCount = accel_props.maxInstanceCount;
                rt.mMaxPrimitiveCount = accel_props.maxPrimitiveCount;
                rt.mScratchAlignment = accel_props.minAccelerationStructureScratchOffsetAlignment;
            }
            if (rt.mExtPipeline)
            {
                rt.mMaxRecursionDepth = rt_pipe_props.maxRayRecursionDepth;
            }
            mCaps.mRt = rt;
            LL_INFOS("Vulkan") << "ray tracing: acceleration structure " << rt.mAccelStruct << ", ray query " << rt.mRayQuery << ", pipeline " << rt.mPipeline
                << ", position fetch " << rt.mPositionFetch << ", maintenance1 " << rt.mMaintenance1 << ", invocation reorder " << rt.mReorder << ", opacity micromap "
                << rt.mOpacityMicromap << ", cluster acceleration structure " << rt.mClusterAccel << " -> usable " << rt.mUsable << "; limits: geometries " << rt.mMaxGeometryCount
                << ", instances " << rt.mMaxInstanceCount << ", primitives " << rt.mMaxPrimitiveCount << ", scratch alignment " << rt.mScratchAlignment
                << ", recursion " << rt.mMaxRecursionDepth << (rayTracingRequested() ? "" : " (not enabled on the device: LL_VK_RT=1 does that)") << LL_ENDL;

            // <FS> bindless sampled images for the render packets: the features, and a table size that leaves room for the push set and the uniform set
            if (chain.f12.runtimeDescriptorArray && chain.f12.descriptorBindingPartiallyBound && chain.f12.descriptorBindingSampledImageUpdateAfterBind
                && chain.f12.shaderSampledImageArrayNonUniformIndexing && mCaps.mBufferDeviceAddress)
            {
                auto room = [](U32 limit, U32 reserve) -> U32 { return limit > reserve ? limit - reserve : 0; };
                const U32 n = std::min<U32>({ 8192u,
                    room(p12.maxPerStageDescriptorUpdateAfterBindSamplers, 96), room(p12.maxPerStageDescriptorUpdateAfterBindSampledImages, 96),
                    room(p12.maxPerStageUpdateAfterBindResources, 160), room(p12.maxDescriptorSetUpdateAfterBindSamplers, 96),
                    room(p12.maxDescriptorSetUpdateAfterBindSampledImages, 96) });
                mCaps.mBindlessSlots = n >= 1024 ? n : 0;
            }
            LL_INFOS("Vulkan") << "descriptor indexing: runtime arrays " << (chain.f12.runtimeDescriptorArray ? 1 : 0) << ", partially bound "
                << (chain.f12.descriptorBindingPartiallyBound ? 1 : 0) << ", sampled image update after bind " << (chain.f12.descriptorBindingSampledImageUpdateAfterBind ? 1 : 0)
                << ", non-uniform sampled image indexing " << (chain.f12.shaderSampledImageArrayNonUniformIndexing ? 1 : 0) << ", update-after-bind limits per stage samplers "
                << p12.maxPerStageDescriptorUpdateAfterBindSamplers << " sampled images " << p12.maxPerStageDescriptorUpdateAfterBindSampledImages << " resources "
                << p12.maxPerStageUpdateAfterBindResources << " -> bindless slots " << mCaps.mBindlessSlots << LL_ENDL;
        }
    }

    if (mPhysical == VK_NULL_HANDLE)
    {
        LL_WARNS("Vulkan") << "no suitable Vulkan device found" << LL_ENDL;
        return false;
    }
    LL_INFOS("Vulkan") << "using " << mCaps.mDeviceName << ", queue family " << mQueueFamily << ", timestamp period "
                       << mCaps.mTimestampPeriodNs << " ns, push descriptors " << mCaps.mMaxPushDescriptors << LL_ENDL;
    return true;
}

bool Context::createDevice()
{
    FeatureChain chain;
    // C6: ray tracing goes onto the device only when asked for and the device can do it. The extension structs of the vendor / later extensions stay out
    // (reported by pickDevice, not enabled): only what the first ray tracing steps use, which is what a device creation check should cover.
    const bool rt_on = rayTracingRequested() && mCaps.mRt.mUsable;
    if (rt_on)
    {
        Capabilities::RayTracing want = mCaps.mRt;
        want.mExtReorder = want.mExtOpacityMicromap = want.mExtCluster = false;
        chain.addRayTracing(want, false);
    }
    else if (rayTracingRequested())
    {
        LL_WARNS("Vulkan") << "LL_VK_RT is set but this device cannot do ray queries (acceleration structure " << mCaps.mRt.mAccelStruct << ", ray query " << mCaps.mRt.mRayQuery << ")" << LL_ENDL;
    }
    vkGetPhysicalDeviceFeatures2(mPhysical, &chain.f2);

    const float priority = 1.f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = mQueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &chain.f2;     // enables every supported feature in the chain (all required ones are present)
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    std::vector<const char*> device_extensions(REQUIRED_DEVICE_EXTENSIONS, REQUIRED_DEVICE_EXTENSIONS + LL_ARRAY_SIZE(REQUIRED_DEVICE_EXTENSIONS));
    if (rt_on)
    {
        const Capabilities::RayTracing& rt = mCaps.mRt;
        device_extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        if (rt.mExtPipeline) device_extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (rt.mExtPositionFetch) device_extensions.push_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
        if (rt.mExtMaintenance1) device_extensions.push_back(VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME);
    }
    if (LLVkDlss::requested())
    {
        LLVkDlss::addDeviceExtensions(mInstance, mPhysical, device_extensions);
    }
    dci.enabledExtensionCount = (U32)device_extensions.size();
    dci.ppEnabledExtensionNames = device_extensions.data();
    if (!vk_ok(vkCreateDevice(mPhysical, &dci, nullptr, &mDevice), "vkCreateDevice"))
    {
        return false;
    }
    mCaps.mRt.mEnabled = rt_on;
    if (rt_on)
    {
        LL_INFOS("Vulkan") << "ray tracing extensions and features are enabled on the device (LL_VK_RT)" << LL_ENDL;
    }

#define LL_VK_GLOBAL_FN(name)
#define LL_VK_INSTANCE_FN(name)
#define LL_VK_DEVICE_FN(name) name = (PFN_##name)vkGetDeviceProcAddr(mDevice, #name);
#include "llvkfunctions.inl"

    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);

    VmaVulkanFunctions funcs = {};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ai = {};
    ai.physicalDevice = mPhysical;
    ai.device = mDevice;
    ai.instance = mInstance;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.pVulkanFunctions = &funcs;
    if (mCaps.mBufferDeviceAddress)
    {
        ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    if (!vk_ok(vmaCreateAllocator(&ai, &mAllocator), "vmaCreateAllocator"))
    {
        return false;
    }
    if (LLVkDlss::requested())
    {
        LLVkDlss::init(mInstance, mPhysical, mDevice);      // not fatal: without it the viewer just does not upscale
    }
    return true;
}

bool Context::createFrames()
{
    LL_INFOS("Vulkan") << "frame slots in flight: " << frameCount() << LL_ENDL;
    for (U32 slot = 0; slot < frameCount(); ++slot)
    {
        Frame& f = mFrames[slot];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        if (!vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &f.mPool), "vkCreateCommandPool"))
        {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.mPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &f.mCmd), "vkAllocateCommandBuffers"))
        {
            return false;
        }
        if (mCaps.mTimestampValidBits > 0)
        {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 + MAX_GPU_STAMPS;
            if (vkCreateQueryPool(mDevice, &qi, nullptr, &f.mTsPool) != VK_SUCCESS)
            {
                f.mTsPool = VK_NULL_HANDLE;
            }
        }
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!vk_ok(vkCreateFence(mDevice, &fi, nullptr, &f.mFence), "vkCreateFence")
            || !vk_ok(vkCreateSemaphore(mDevice, &si, nullptr, &f.mAcquire), "vkCreateSemaphore"))
        {
            return false;
        }
    }
    return true;
}

bool Context::init(HWND present_window)
{
    if (mReady)
    {
        return true;
    }
    mWindow = present_window;

    if (!loadLoader() || !createInstance() || !pickDevice() || !createDevice() || !createFrames() || !selfTest())
    {
        LL_WARNS("Vulkan") << "Vulkan context initialisation failed" << LL_ENDL;
        shutdown();
        return false;
    }
    mReady = true;
    measureBandwidth();
    LL_INFOS("Vulkan") << "Vulkan context ready" << LL_ENDL;
    return true;
}

void Context::destroyFrameResources(Frame& f)
{
    for (auto& fn : f.mDeferred)
    {
        fn();
    }
    f.mDeferred.clear();
    for (Buffer& b : f.mStagingBlocks)
    {
        if (b.valid())
        {
            vmaDestroyBuffer(mAllocator, b.mBuffer, b.mAllocation);
        }
    }
    f.mStagingBlocks.clear();
    if (f.mTsPool) vkDestroyQueryPool(mDevice, f.mTsPool, nullptr);
    if (f.mAcquire) vkDestroySemaphore(mDevice, f.mAcquire, nullptr);
    if (f.mFence) vkDestroyFence(mDevice, f.mFence, nullptr);
    if (f.mPool) vkDestroyCommandPool(mDevice, f.mPool, nullptr);
    f = Frame();
}

void Context::shutdown()
{
    if (mDevice != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(mDevice);
        LLVkDlss::shutdown();
        for (Frame& f : mFrames)
        {
            destroyFrameResources(f);
        }
        if (mImmediateFence) vkDestroyFence(mDevice, mImmediateFence, nullptr);
        if (mImmediatePool) vkDestroyCommandPool(mDevice, mImmediatePool, nullptr);
        mImmediateFence = VK_NULL_HANDLE;
        mImmediatePool = VK_NULL_HANDLE;
        mImmediateCmd = VK_NULL_HANDLE;
        arenaShutdown();
        if (mAllocator)
        {
            vmaDestroyAllocator(mAllocator);
            mAllocator = nullptr;
        }
        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }
    if (mInstance != VK_NULL_HANDLE)
    {
        if (mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }
        if (mMessenger != VK_NULL_HANDLE)
        {
            auto destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT");
            if (destroy_messenger)
            {
                destroy_messenger(mInstance, mMessenger, nullptr);
            }
            mMessenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    mPhysical = VK_NULL_HANDLE;
    mQueue = VK_NULL_HANDLE;
    mReady = false;
    mInFrame = false;
    mFrameSlot = 0;
    mFrameNumber = 0;
}

void Context::waitIdle()
{
    if (mDevice != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(mDevice);
    }
}

namespace
{
std::vector<std::string>& stamp_names()
{
    static std::vector<std::string> names = { "(uploads and copies before the first pass)" };
    return names;
}
}

U32 Context::stampTag(const std::string& name)
{
    auto& names = stamp_names();
    for (U32 i = 0; i < (U32)names.size(); ++i)
    {
        if (names[i] == name) return i;
    }
    names.push_back(name);
    return (U32)names.size() - 1;
}

std::string Context::stampTagName(U32 tag)
{
    auto& names = stamp_names();
    return tag < names.size() ? names[tag] : "?";
}

void Context::gpuStamp(U32 tag)
{
    if (!mInFrame)
    {
        return;
    }
    Frame& f = mFrames[mFrameSlot];
    const U32 n = (U32)f.mStampTags.size();
    if (!f.mTsPool || n >= MAX_GPU_STAMPS)
    {
        return;
    }
    vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 2 + n);
    f.mStampTags.push_back(tag);
}

VkCommandBuffer Context::beginFrame(bool wait)
{
    llassert(!mInFrame);
    Frame& f = mFrames[mFrameSlot];

    if (!wait && vkGetFenceStatus(mDevice, f.mFence) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    vkWaitForFences(mDevice, 1, &f.mFence, VK_TRUE, UINT64_MAX);
    gSyncStats.mFenceWaitMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count(), std::memory_order_relaxed);
    vkResetFences(mDevice, 1, &f.mFence);

    if (f.mTsPending && f.mTsPool)
    {
        f.mTsPending = false;
        const U32 n = (U32)f.mStampTags.size();
        std::vector<U64> r((size_t)(2 + n) * 2);        // per query: the value and its availability
        const VkResult res = vkGetQueryPoolResults(mDevice, f.mTsPool, 0, 2 + n, r.size() * sizeof(U64), r.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        bool all = res == VK_SUCCESS;
        for (U32 q = 0; all && q < 2 + n; ++q) all = r[q * 2 + 1] != 0;
        if (all)
        {
            const U32 bits = mCaps.mTimestampValidBits;
            const U64 mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            auto to_micros = [&](U64 from, U64 to) { return (U64)((double)((to - from) & mask) * (double)mCaps.mTimestampPeriodNs / 1000.0); };
            const U64 start = r[0], end = r[2];
            const U64 micros = to_micros(start, end);
            gSyncStats.mGpuFrames.fetch_add(1, std::memory_order_relaxed);
            gSyncStats.mGpuMicros.fetch_add(micros, std::memory_order_relaxed);
            U64 prev = gSyncStats.mGpuMaxMicros.load(std::memory_order_relaxed);
            while (micros > prev && !gSyncStats.mGpuMaxMicros.compare_exchange_weak(prev, micros, std::memory_order_relaxed)) {}
            if (f.mOffscreen) { mPassStats.mOffMicros += micros; ++mPassStats.mOffFrames; mPassStats.mOffMax = std::max(mPassStats.mOffMax, micros); }
            else { mPassStats.mMainMicros += micros; ++mPassStats.mMainFrames; mPassStats.mMainMax = std::max(mPassStats.mMainMax, micros); }
            if (n)
            {
                // the time from one pass's stamp to the next is that pass's; the first stretch (uploads, copies) and the last pass run to the frame's own start and end
                U64 last = start;
                U32 last_tag = 0;
                for (U32 i = 0; i < n; ++i)
                {
                    const U64 stamp = r[(2 + i) * 2];
                    mPassStats.mMicros[last_tag] += to_micros(last, stamp);
                    last = stamp;
                    last_tag = f.mStampTags[i];
                }
                mPassStats.mMicros[last_tag] += to_micros(last, end);
            }
        }
    }
    f.mStampTags.clear();
    f.mOffscreen = mNextOffscreen;
    mNextOffscreen = false;

    // everything queued while this slot was last current is now safe to destroy
    for (auto& fn : f.mDeferred)
    {
        fn();
    }
    f.mDeferred.clear();

    // keep the first staging block, free any extras that were needed
    for (size_t i = 1; i < f.mStagingBlocks.size(); ++i)
    {
        vmaDestroyBuffer(mAllocator, f.mStagingBlocks[i].mBuffer, f.mStagingBlocks[i].mAllocation);
    }
    if (f.mStagingBlocks.size() > 1)
    {
        f.mStagingBlocks.resize(1);
    }
    f.mStagingUsed = 0;

    vkResetCommandPool(mDevice, f.mPool, 0);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);
    if (f.mTsPool)
    {
        vkCmdResetQueryPool(f.mCmd, f.mTsPool, 0, 2 + MAX_GPU_STAMPS);
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 0);
    }
    mInFrame = true;
    return f.mCmd;
}

void Context::endFrame(VkSemaphore wait_semaphore, VkSemaphore signal_semaphore)
{
    llassert(mInFrame);
    Frame& f = mFrames[mFrameSlot];
    if (f.mTsPool)
    {
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 1);
        f.mTsPending = true;
    }
    vkEndCommandBuffer(f.mCmd);

    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = f.mCmd;

    VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = wait_semaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = signal_semaphore;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = wait_semaphore != VK_NULL_HANDLE ? 1 : 0;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signal_semaphore != VK_NULL_HANDLE ? 1 : 0;
    si.pSignalSemaphoreInfos = &signal;
    vk_ok(vkQueueSubmit2(mQueue, 1, &si, f.mFence), "vkQueueSubmit2");

    mInFrame = false;
    mFrameSlot = (mFrameSlot + 1) % frameCount();
    ++mFrameNumber;
}

bool Context::flushFrameAndWait()
{
    if (!mInFrame || !mImmediateFence)
    {
        if (!mImmediateFence)
        {
            immediateSubmit([](VkCommandBuffer) {});     // creates the immediate pool and fence
        }
        if (!mInFrame || !mImmediateFence)
        {
            return false;
        }
    }
    Frame& f = mFrames[mFrameSlot];
    vkEndCommandBuffer(f.mCmd);

    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = f.mCmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    vkResetFences(mDevice, 1, &mImmediateFence);
    bool ok = vk_ok(vkQueueSubmit2(mQueue, 1, &si, mImmediateFence), "flush submit")
        && vk_ok(vkWaitForFences(mDevice, 1, &mImmediateFence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "flush wait");

    // the work above has finished: the pool can be reset and the same command buffer recorded again
    vkResetCommandPool(mDevice, f.mPool, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);
    if (f.mTsPool)
    {
        f.mStampTags.clear();       // the reset below wipes the stamps of the first part
        vkCmdResetQueryPool(f.mCmd, f.mTsPool, 0, 2 + MAX_GPU_STAMPS);
        vkCmdWriteTimestamp2(f.mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.mTsPool, 0);
    }
    return ok;
}

bool Context::frameFinished(U64 n)
{
    if (n >= mFrameNumber)
    {
        return false;
    }
    if (mFrameNumber - n >= frameCount())
    {
        return true;        // its slot has been reused, which waited for it
    }
    return vkGetFenceStatus(mDevice, mFrames[n % frameCount()].mFence) == VK_SUCCESS;
}

StagingAlloc Context::allocStaging(VkDeviceSize size, VkDeviceSize alignment)
{
    Frame& f = mFrames[mFrameSlot];
    if (alignment < 1)
    {
        alignment = 1;
    }

    auto fits = [&]() {
        if (f.mStagingBlocks.empty())
        {
            return false;
        }
        VkDeviceSize start = (f.mStagingUsed + alignment - 1) / alignment * alignment;
        return start + size <= f.mStagingBlocks.back().mSize;
    };

    if (!fits())
    {
        Buffer block;
        const VkDeviceSize block_size = std::max<VkDeviceSize>(size + alignment, 32ull * 1024 * 1024);
        if (!createBuffer(block_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, block))
        {
            return StagingAlloc();
        }
        f.mStagingBlocks.push_back(block);
        f.mStagingUsed = 0;
    }

    Buffer& block = f.mStagingBlocks.back();
    VkDeviceSize start = (f.mStagingUsed + alignment - 1) / alignment * alignment;
    f.mStagingUsed = start + size;

    StagingAlloc a;
    a.mBuffer = block.mBuffer;
    a.mOffset = start;
    a.mPtr = (U8*)block.mMapped + start;
    return a;
}

void Context::destroyLater(std::function<void()> fn)
{
    const U32 slot = mInFrame ? mFrameSlot : (mFrameSlot + frameCount() - 1) % frameCount();
    mFrames[slot].mDeferred.push_back(std::move(fn));
}

bool Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory, Buffer& out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    if (mCaps.mBufferDeviceAddress)
    {
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;      // every buffer can be read by address (render packets)
    }
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai = {};
    switch (memory)
    {
    case Memory::Upload:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case Memory::UploadDevice:
        // written by the CPU, read by the GPU every frame: video memory the CPU can map (resizable BAR) when there is room
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case Memory::Readback:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    default:
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo info = {};
    out = Buffer();
    if (!vk_ok(vmaCreateBuffer(mAllocator, &bi, &ai, &out.mBuffer, &out.mAllocation, &info), "vmaCreateBuffer"))
    {
        out = Buffer();
        return false;
    }
    out.mMapped = info.pMappedData;
    out.mSize = size;
    if (mCaps.mBufferDeviceAddress)
    {
        VkBufferDeviceAddressInfo address_info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address_info.buffer = out.mBuffer;
        out.mAddress = vkGetBufferDeviceAddress(mDevice, &address_info);
    }
    if (memory == Memory::UploadDevice)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            VkMemoryPropertyFlags flags = 0;
            vmaGetAllocationMemoryProperties(mAllocator, out.mAllocation, &flags);
            LL_INFOS("Vulkan") << "CPU-written GPU buffers live in " << ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local (video) memory" : "host memory")
                               << " (memory property flags 0x" << std::hex << flags << std::dec << ")" << LL_ENDL;
        }
    }
    return true;
}

void Context::destroyBuffer(Buffer& buffer)
{
    if (!buffer.valid())
    {
        return;
    }
    VkBuffer b = buffer.mBuffer;
    VmaAllocation a = buffer.mAllocation;
    VmaAllocator alloc = mAllocator;
    destroyLater([=]() { vmaDestroyBuffer(alloc, b, a); });
    buffer = Buffer();
}

//-----------------------------------------------------------------------------------------------------
// buffer arena
//-----------------------------------------------------------------------------------------------------

namespace
{
// regular blocks (a range bigger than a quarter of one gets a block of its own)
VkDeviceSize arena_block_bytes()
{
    static const VkDeviceSize v = []() {
        char b[16] = {};
        const U32 mb = GetEnvironmentVariableA("LL_VK_ARENA_BLOCK_MB", b, sizeof(b)) > 0 ? (U32)atoi(b) : 256;
        return (VkDeviceSize)std::max<U32>(mb, 16) * 1024 * 1024;
    }();
    return v;
}
}

Context::ArenaBlock* Context::arenaNewBlock(VkDeviceSize min_size, ArenaKind kind, bool dedicated)
{
    const bool shared = kind == ArenaKind::Shared, host = kind == ArenaKind::Host;
    ArenaBlock* block = new ArenaBlock();
    block->mSize = dedicated ? (min_size + 255) / 256 * 256 : std::max(arena_block_bytes(), min_size);
    block->mShared = shared;
    block->mHost = host;
    block->mDedicated = dedicated;

    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = block->mSize;
    // vertex / index reads, copies in, shaders reading by address (packets, skinning) and, once the device has it, acceleration structure builds reading the same ranges
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (mCaps.mBufferDeviceAddress)
    {
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (mCaps.mRt.mEnabled)
    {
        bi.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    const uint32_t families[2] = { mQueueFamily, (uint32_t)std::max<S32>(mCaps.mComputeFamily, 0) };
    if (shared && mCaps.mComputeFamily >= 0)
    {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = 2;
        bi.pQueueFamilyIndices = families;
    }
    else
    {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (host)
    {
        // as Memory::UploadDevice: written by the CPU, read by the GPU every frame; video memory the CPU can map (resizable BAR) when there is room
        ai.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        ai.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    VmaAllocationInfo block_info = {};
    if (!vk_ok(vmaCreateBuffer(mAllocator, &bi, &ai, &block->mBuffer, &block->mAllocation, &block_info), "arena block"))
    {
        delete block;
        return nullptr;
    }
    block->mMapped = block_info.pMappedData;
    VmaVirtualBlockCreateInfo vi = {};
    vi.size = block->mSize;
    if (vmaCreateVirtualBlock(&vi, &block->mVirtual) != VK_SUCCESS)
    {
        vmaDestroyBuffer(mAllocator, block->mBuffer, block->mAllocation);
        delete block;
        return nullptr;
    }
    if (mCaps.mBufferDeviceAddress)
    {
        VkBufferDeviceAddressInfo address_info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        address_info.buffer = block->mBuffer;
        block->mAddress = vkGetBufferDeviceAddress(mDevice, &address_info);
    }
    bool placed = false;
    for (ArenaBlock*& slot : mArenaBlocks)
    {
        if (!slot) { slot = block; placed = true; break; }
    }
    if (!placed)
    {
        mArenaBlocks.push_back(block);
    }
    ++mArenaCounters.mBlocksCreated;
    if (host)
    {
        VkMemoryPropertyFlags flags = 0;
        vmaGetAllocationMemoryProperties(mAllocator, block->mAllocation, &flags);
        LL_INFOS("Vulkan") << "buffer arena: host-written block lives in " << ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device-local (video) memory" : "host memory") << LL_ENDL;
    }
    LL_INFOS("Vulkan") << "buffer arena: new " << (dedicated ? "dedicated " : "") << (shared ? "shared " : "") << (host ? "host-written " : "") << "block of " << (block->mSize >> 20) << " MB ("
                       << (U32)std::count_if(mArenaBlocks.begin(), mArenaBlocks.end(), [](ArenaBlock* b) { return b != nullptr; }) << " blocks now)" << LL_ENDL;
    return block;
}

void Context::arenaDestroyBlock(ArenaBlock* block)
{
    for (ArenaBlock*& slot : mArenaBlocks)
    {
        if (slot == block) slot = nullptr;
    }
    vmaDestroyVirtualBlock(block->mVirtual);
    vmaDestroyBuffer(mAllocator, block->mBuffer, block->mAllocation);
    ++mArenaCounters.mBlocksReleased;
    delete block;
}

bool Context::arenaAlloc(VkDeviceSize size, VkDeviceSize alignment, ArenaKind kind, ArenaRef& out)
{
    out = ArenaRef();
    if (size == 0 || mAllocator == nullptr)
    {
        return false;
    }
    alignment = std::max<VkDeviceSize>(alignment, 4);
    if (kind == ArenaKind::Shared && mCaps.mComputeFamily < 0)
    {
        kind = ArenaKind::Device;
    }
    const bool shared = kind == ArenaKind::Shared, host = kind == ArenaKind::Host;
    std::lock_guard<std::mutex> lock(mArenaMutex);
    const bool dedicated = size > arena_block_bytes() / 4;
    auto place = [&](ArenaBlock* b) {
        VmaVirtualAllocationCreateInfo ci = {};
        ci.size = size;
        ci.alignment = alignment;
        VmaVirtualAllocation alloc = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        if (vmaVirtualAllocate(b->mVirtual, &ci, &alloc, &offset) != VK_SUCCESS)
        {
            return false;
        }
        ++b->mAllocs;
        out.mBuffer = b->mBuffer;
        out.mOffset = offset;
        out.mSize = size;
        out.mAddress = b->mAddress ? b->mAddress + offset : 0;
        out.mMapped = b->mMapped ? (U8*)b->mMapped + offset : nullptr;
        out.mAlloc = alloc;
        for (size_t i = 0; i < mArenaBlocks.size(); ++i)
        {
            if (mArenaBlocks[i] == b) { out.mBlock = (U32)i; break; }
        }
        return true;
    };
    if (!dedicated)
    {
        for (ArenaBlock* b : mArenaBlocks)
        {
            if (b && !b->mDedicated && b->mShared == shared && b->mHost == host && place(b))
            {
                return true;
            }
        }
    }
    ArenaBlock* fresh = arenaNewBlock(size + alignment, kind, dedicated);
    if (!fresh || !place(fresh))
    {
        LL_WARNS("Vulkan") << "buffer arena: no room for " << size << " bytes" << LL_ENDL;
        return false;
    }
    return true;
}

void Context::arenaFreeNow(ArenaBlock* block, VmaVirtualAllocation alloc)
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    vmaVirtualFree(block->mVirtual, alloc);
    if (--block->mAllocs != 0)
    {
        return;
    }
    // an empty block goes when it is a block of its own, or when another empty one of its kind is already waiting for the next allocation
    bool release = block->mDedicated;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (b && b != block && !b->mDedicated && b->mShared == block->mShared && b->mHost == block->mHost && b->mAllocs == 0)
        {
            release = true;
            break;
        }
    }
    if (release)
    {
        arenaDestroyBlock(block);
    }
}

void Context::arenaFree(ArenaRef& ref)
{
    if (!ref.valid())
    {
        return;
    }
    ArenaBlock* block = nullptr;
    {
        std::lock_guard<std::mutex> lock(mArenaMutex);
        block = ref.mBlock < mArenaBlocks.size() ? mArenaBlocks[ref.mBlock] : nullptr;
    }
    const VmaVirtualAllocation alloc = ref.mAlloc;
    ref = ArenaRef();
    if (block)
    {
        destroyLater([this, block, alloc]() { arenaFreeNow(block, alloc); });
    }
}

ArenaStats Context::arenaStats()
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    ArenaStats s = mArenaCounters;
    s.mBlocks = s.mAllocs = s.mHostBlocks = s.mHostAllocs = 0;
    s.mHostBlockBytes = s.mHostUsedBytes = 0;
    s.mBlockBytes = s.mUsedBytes = s.mLargestFree = 0;
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (!b) continue;
        VmaDetailedStatistics st = {};
        vmaCalculateVirtualBlockStatistics(b->mVirtual, &st);
        if (b->mHost)
        {
            ++s.mHostBlocks;
            s.mHostAllocs += (U32)st.statistics.allocationCount;
            s.mHostBlockBytes += b->mSize;
            s.mHostUsedBytes += st.statistics.allocationBytes;
            continue;
        }
        ++s.mBlocks;
        s.mAllocs += (U32)st.statistics.allocationCount;
        s.mBlockBytes += b->mSize;
        s.mUsedBytes += st.statistics.allocationBytes;
        s.mLargestFree = std::max<U64>(s.mLargestFree, st.unusedRangeSizeMax);
    }
    return s;
}

void Context::arenaShutdown()
{
    std::lock_guard<std::mutex> lock(mArenaMutex);
    for (ArenaBlock* b : mArenaBlocks)
    {
        if (b)
        {
            vmaDestroyVirtualBlock(b->mVirtual);
            vmaDestroyBuffer(mAllocator, b->mBuffer, b->mAllocation);
            delete b;
        }
    }
    mArenaBlocks.clear();
}

bool Context::createImage2D(U32 width, U32 height, U32 mips, U32 layers, VkFormat format, VkImageUsageFlags usage, Image& out)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = { width, height, 1 };
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    out = Image();
    if (!vk_ok(vmaCreateImage(mAllocator, &ii, &ai, &out.mImage, &out.mAllocation, nullptr), "vmaCreateImage"))
    {
        out = Image();
        return false;
    }
    out.mFormat = format;
    out.mAspect = aspectForFormat(format);
    out.mExtent = { width, height, 1 };
    out.mMipLevels = mips;
    out.mLayers = layers;
    out.mLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkImageUsageFlags view_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
        | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (usage & view_usage)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out.mImage;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { out.mAspect, 0, mips, 0, layers };
        if (!vk_ok(vkCreateImageView(mDevice, &vi, nullptr, &out.mView), "vkCreateImageView (image)"))
        {
            vmaDestroyImage(mAllocator, out.mImage, out.mAllocation);
            out = Image();
            return false;
        }
    }
    return true;
}

void Context::destroyImage(Image& image)
{
    if (!image.valid())
    {
        return;
    }
    VkImage i = image.mImage;
    VkImageView v = image.mView;
    VmaAllocation a = image.mAllocation;
    VmaAllocator alloc = mAllocator;
    VkDevice dev = mDevice;
    destroyLater([=]() {
        if (v) vkDestroyImageView(dev, v, nullptr);
        if (a) vmaDestroyImage(alloc, i, a);
    });
    image = Image();
}

// Exercises the allocator and the transfer path end to end: buffer -> device buffer -> readback, and
// buffer -> image -> readback, comparing the bytes that come back.
bool Context::immediateSubmit(const std::function<void(VkCommandBuffer)>& record)
{
    if (!mImmediatePool)
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &mImmediatePool), "immediate pool")
            || !vk_ok(vkCreateFence(mDevice, &fi, nullptr, &mImmediateFence), "immediate fence"))
        {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = mImmediatePool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(mDevice, &ai, &mImmediateCmd), "immediate command buffer"))
        {
            return false;
        }
    }

    vkResetCommandBuffer(mImmediateCmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(mImmediateCmd, &bi);
    record(mImmediateCmd);
    vkEndCommandBuffer(mImmediateCmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &mImmediateCmd;
    vkResetFences(mDevice, 1, &mImmediateFence);
    return vk_ok(vkQueueSubmit(mQueue, 1, &si, mImmediateFence), "immediate submit")
        && vk_ok(vkWaitForFences(mDevice, 1, &mImmediateFence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "immediate wait");
}

bool Context::selfTest()
{
    const U32 W = 64, H = 64;
    const VkDeviceSize image_bytes = W * H * 4;
    const VkDeviceSize buffer_bytes = 1024 * 1024;

    Buffer upload, device, readback;
    Image image;
    bool ok = createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload, upload)
        && createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Device, device)
        && createBuffer(buffer_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback, readback)
        && createImage2D(W, H, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image);

    VkCommandPool pool = VK_NULL_HANDLE;
    if (ok)
    {
        U32* src = (U32*)upload.mMapped;
        for (U32 i = 0; i < buffer_bytes / 4; ++i)
        {
            src[i] = 0x9E3779B1u * (i + 1);
        }
        memset(readback.mMapped, 0, (size_t)buffer_bytes);

        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = mQueueFamily;
        ok = vk_ok(vkCreateCommandPool(mDevice, &pi, nullptr, &pool), "self test pool");
    }

    if (ok)
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(mDevice, &ai, &cmd);

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        auto memory_barrier = [&]() {
            VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        // buffer round trip: upload [0, 512K) -> device -> readback [0, 512K)
        VkBufferCopy region = { 0, 0, buffer_bytes / 2 };
        vkCmdCopyBuffer(cmd, upload.mBuffer, device.mBuffer, 1, &region);
        memory_barrier();
        vkCmdCopyBuffer(cmd, device.mBuffer, readback.mBuffer, 1, &region);

        // image round trip: upload [512K, 512K + 16K) -> image -> readback [512K, ...)
        imageBarrier(cmd, image.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy up = {};
        up.bufferOffset = buffer_bytes / 2;
        up.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        up.imageExtent = { W, H, 1 };
        vkCmdCopyBufferToImage(cmd, upload.mBuffer, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        imageBarrier(cmd, image.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy down = up;
        vkCmdCopyImageToBuffer(cmd, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.mBuffer, 1, &down);

        vkEndCommandBuffer(cmd);

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(mDevice, &fi, nullptr, &fence);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        ok = vk_ok(vkQueueSubmit(mQueue, 1, &si, fence), "self test submit")
            && vk_ok(vkWaitForFences(mDevice, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "self test wait");
        vkDestroyFence(mDevice, fence, nullptr);
    }

    if (ok)
    {
        const U8* sent = (const U8*)upload.mMapped;
        const U8* got = (const U8*)readback.mMapped;
        const bool buffer_ok = memcmp(sent, got, (size_t)(buffer_bytes / 2)) == 0;
        const bool image_ok = memcmp(sent + buffer_bytes / 2, got + buffer_bytes / 2, (size_t)image_bytes) == 0;
        LL_INFOS("Vulkan") << "self test: buffer round trip " << (buffer_ok ? "OK" : "FAILED") << ", image round trip "
                           << (image_ok ? "OK" : "FAILED") << LL_ENDL;
        ok = buffer_ok && image_ok;
    }

    if (pool) vkDestroyCommandPool(mDevice, pool, nullptr);
    if (upload.valid()) vmaDestroyBuffer(mAllocator, upload.mBuffer, upload.mAllocation);
    if (device.valid()) vmaDestroyBuffer(mAllocator, device.mBuffer, device.mAllocation);
    if (readback.valid()) vmaDestroyBuffer(mAllocator, readback.mBuffer, readback.mAllocation);
    if (image.valid()) vmaDestroyImage(mAllocator, image.mImage, image.mAllocation);
    return ok;
}

// C6: the viewer sorts GPUs into classes by memory bandwidth (LLFeatureManager::loadGPUClass; thresholds 48 / 96 / 192 / 384 GB/s times the CPU bias). The GL
// benchmark that used to feed it is gone, so this one measures what the device really does: large device-local copies, bytes read plus bytes written over the
// wall time of the submit. The first run wakes the memory clock up, the best of the following runs counts.
void Context::measureBandwidth()
{
    const U64 vram = deviceLocalBytes();
    VkDeviceSize chunk = 256ull * 1024 * 1024;
    while (chunk > 16ull * 1024 * 1024 && (vram == 0 || chunk * 8 > vram))
    {
        chunk /= 2;
    }
    Buffer a, b;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!createBuffer(chunk, usage, Memory::Device, a) || !createBuffer(chunk, usage, Memory::Device, b))
    {
        LL_WARNS("Vulkan") << "bandwidth measurement skipped: no memory for the test buffers" << LL_ENDL;
        if (a.valid()) vmaDestroyBuffer(mAllocator, a.mBuffer, a.mAllocation);
        return;
    }

    auto barrier = [](VkCommandBuffer cmd) {
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const U32 copies_per_run = 8;
    auto run = [&]() -> double {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const bool ok = immediateSubmit([&](VkCommandBuffer cmd) {
            const VkBufferCopy region = { 0, 0, chunk };
            for (U32 i = 0; i < copies_per_run; ++i)
            {
                vkCmdCopyBuffer(cmd, (i & 1) ? b.mBuffer : a.mBuffer, (i & 1) ? a.mBuffer : b.mBuffer, 1, &region);
                barrier(cmd);
            }
        });
        QueryPerformanceCounter(&t1);
        const double seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        return ok && seconds > 0.0 ? (double)copies_per_run * 2.0 * (double)chunk / seconds / 1.0e9 : 0.0;
    };

    // the buffers start out undefined; write them once so the copies read real data
    bool ok = immediateSubmit([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, a.mBuffer, 0, chunk, 0x3F800000u); vkCmdFillBuffer(cmd, b.mBuffer, 0, chunk, 0u); barrier(cmd); });
    double best = 0.0, first = 0.0;
    for (U32 i = 0; ok && i < 4; ++i)
    {
        const double gbps = run();
        if (i == 0) first = gbps;
        else best = std::max(best, gbps);
    }
    vmaDestroyBuffer(mAllocator, a.mBuffer, a.mAllocation);
    vmaDestroyBuffer(mAllocator, b.mBuffer, b.mAllocation);
    if (!ok || best <= 0.0)
    {
        LL_WARNS("Vulkan") << "bandwidth measurement failed" << LL_ENDL;
        return;
    }
    // A copy reaches about 85% of what the memory can do (RTX 5060 Ti: 379.7 measured, 448 in the data sheet), and the class thresholds are in peak terms
    // (384 GB/s is the Class 5 edge), so the raw figure would put a card that is Class 5 on paper into Class 4. Scaled to the estimated peak.
    const double COPY_EFFICIENCY = 0.85;
    mCaps.mMemoryBandwidthGBps = (F32)(best / COPY_EFFICIENCY);
    LL_INFOS("Vulkan") << "memory bandwidth (device-local copy, read + write): " << best << " GB/s measured (first run " << first << ", buffers " << (chunk >> 20) << " MB each), "
                       << mCaps.mMemoryBandwidthGBps << " GB/s estimated peak (copy efficiency " << COPY_EFFICIENCY << ")" << LL_ENDL;
}

//-----------------------------------------------------------------------------------------------------
// Swapchain
//-----------------------------------------------------------------------------------------------------

void Swapchain::destroy()
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    if (dev == VK_NULL_HANDLE)
    {
        return;
    }
    vkDeviceWaitIdle(dev);
    for (VkSemaphore s : mRenderDone)
    {
        vkDestroySemaphore(dev, s, nullptr);
    }
    mRenderDone.clear();
    for (VkImageView v : mViews)
    {
        vkDestroyImageView(dev, v, nullptr);
    }
    mViews.clear();
    mImages.clear();
    if (mSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(dev, mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

namespace
{
std::atomic<int> gPresentModePref{ 0 };
std::atomic<bool> gVsyncPref{ true };

VkPresentModeKHR wanted_present_mode()
{
    static const char* env = getenv("LL_VK_PRESENT");        // experiment: fifo, mailbox, immediate, relaxed; wins over the settings
    if (env)
    {
        return strcmp(env, "mailbox") == 0 ? VK_PRESENT_MODE_MAILBOX_KHR : strcmp(env, "immediate") == 0 ? VK_PRESENT_MODE_IMMEDIATE_KHR
             : strcmp(env, "relaxed") == 0 ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    switch (gPresentModePref.load(std::memory_order_relaxed))
    {
    case 1: return VK_PRESENT_MODE_FIFO_KHR;
    case 2: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case 3: return VK_PRESENT_MODE_MAILBOX_KHR;
    default: return gVsyncPref.load(std::memory_order_relaxed) ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
}
}

void setPresentPreference(int mode, bool vsync_on)
{
    gPresentModePref.store(mode, std::memory_order_relaxed);
    gVsyncPref.store(vsync_on, std::memory_order_relaxed);
}

bool Swapchain::needsRecreate() const
{
    return mNeedsRecreate || (mSwapchain != VK_NULL_HANDLE && mRequestedMode != wanted_present_mode());
}

bool Swapchain::create(VkSurfaceKHR surface, HWND window)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkPhysicalDevice phys = ctx.getPhysicalDevice();
    mSurface = surface;
    mWindow = window;
    mNeedsRecreate = false;

    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps), "surface capabilities"))
    {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        RECT rc;
        GetClientRect(window, &rc);
        extent.width = (U32)(rc.right - rc.left);
        extent.height = (U32)(rc.bottom - rc.top);
    }
    // keep the old swapchain until the new one exists and hand it over (oldSwapchain): the compositor then switches over without showing a stale picture
    VkSwapchainKHR old_swapchain = mSwapchain;
    vkDeviceWaitIdle(dev);
    for (VkSemaphore s : mRenderDone) vkDestroySemaphore(dev, s, nullptr);
    mRenderDone.clear();
    for (VkImageView v : mViews) vkDestroyImageView(dev, v, nullptr);
    mViews.clear();
    mImages.clear();
    mSwapchain = VK_NULL_HANDLE;
    if (extent.width == 0 || extent.height == 0)
    {
        if (old_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(dev, old_swapchain, nullptr);
        }
        return true;     // minimised: no swapchain until the window has an area again
    }

    U32 fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fcount, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = f;
        }
    }

    U32 image_count = caps.minImageCount + 1;
    if (const char* n = getenv("LL_VK_SC_IMAGES"))
    {
        image_count = std::max<U32>(caps.minImageCount, (U32)atoi(n));      // experiment: swapchain image count
    }
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    {
        image_count = caps.maxImageCount;
    }
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    {
        const VkPresentModeKHR want = wanted_present_mode();
        mRequestedMode = want;
        U32 mcount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mcount, nullptr);
        std::vector<VkPresentModeKHR> modes(mcount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mcount, modes.data());
        for (VkPresentModeKHR pm : modes) if (pm == want) present_mode = want;
    }

    VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc.surface = surface;
    sc.minImageCount = image_count;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = present_mode;
    sc.clipped = VK_TRUE;
    sc.oldSwapchain = old_swapchain;
    const bool created = vk_ok(vkCreateSwapchainKHR(dev, &sc, nullptr, &mSwapchain), "vkCreateSwapchainKHR");
    if (old_swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(dev, old_swapchain, nullptr);
    }
    if (!created)
    {
        mSwapchain = VK_NULL_HANDLE;
        return false;
    }
    mFormat = chosen.format;
    mExtent = extent;

    U32 icount = 0;
    vkGetSwapchainImagesKHR(dev, mSwapchain, &icount, nullptr);
    mImages.resize(icount);
    vkGetSwapchainImagesKHR(dev, mSwapchain, &icount, mImages.data());

    for (VkImage image : mImages)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateImageView(dev, &vi, nullptr, &view), "vkCreateImageView"))
        {
            return false;
        }
        mViews.push_back(view);

        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore sem = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSemaphore(dev, &si, nullptr, &sem), "vkCreateSemaphore"))
        {
            return false;
        }
        mRenderDone.push_back(sem);
    }

    LL_INFOS("Vulkan") << "swapchain " << extent.width << "x" << extent.height << " format " << (S32)chosen.format << " images " << icount << LL_ENDL;
    return true;
}

bool Swapchain::acquire(VkSemaphore signal, U32& index, U64 timeout_ns)
{
    if (mSwapchain == VK_NULL_HANDLE)
    {
        return false;
    }
    const auto acquire_start = std::chrono::steady_clock::now();
    VkResult r = vkAcquireNextImageKHR(Context::instance().getDevice(), mSwapchain, timeout_ns, signal, VK_NULL_HANDLE, &index);
    gSyncStats.mAcquireWaitMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acquire_start).count(), std::memory_order_relaxed);
    static U32 sLogged = 0;
    if (r != VK_SUCCESS && r != VK_NOT_READY && r != VK_TIMEOUT && sLogged++ < 20)
    {
        LL_WARNS("Vulkan") << "vkAcquireNextImageKHR returned " << (S32)r << LL_ENDL;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        mNeedsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
        return true;
    }
    return r == VK_SUCCESS;
}

bool Swapchain::present(U32 index, VkSemaphore wait)
{
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &wait;
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &index;
    const auto present_start = std::chrono::steady_clock::now();
    VkResult r = vkQueuePresentKHR(Context::instance().getQueue(), &pi);
    gSyncStats.mPresentMicros.fetch_add((U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - present_start).count(), std::memory_order_relaxed);
    static U32 sLogged = 0;
    if (r != VK_SUCCESS && sLogged++ < 20)
    {
        LL_WARNS("Vulkan") << "vkQueuePresentKHR returned " << (S32)r << LL_ENDL;
    }
    mPresentOutOfDate = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
        return false;
    }
    return r == VK_SUCCESS;
}

bool Swapchain::acquireBlocking(U32& index)
{
    if (mSwapchain == VK_NULL_HANDLE)
    {
        return false;
    }
    VkDevice dev = Context::instance().getDevice();
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS)
    {
        return false;
    }
    VkResult r = vkAcquireNextImageKHR(dev, mSwapchain, 1000ull * 1000 * 1000, VK_NULL_HANDLE, fence, &index);
    bool ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    if (ok)
    {
        ok = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000ull * 1000 * 1000) == VK_SUCCESS;
    }
    vkDestroyFence(dev, fence, nullptr);
    return ok;
}

bool Swapchain::presentNoWait(U32 index)
{
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(Context::instance().getQueue(), &pi);
    mPresentOutOfDate = r == VK_ERROR_OUT_OF_DATE_KHR;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        mNeedsRecreate = true;
    }
    return r == VK_SUCCESS;
}

SyncStats gSyncStats;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, U32 mips, U32 layers,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    gSyncStats.mImageBarriers.fetch_add(1, std::memory_order_relaxed);
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, mips, 0, layers };
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace LLVk

#endif // LL_VULKAN
