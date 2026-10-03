/**
 * @file llvkcontext.cpp
 * @brief Vulkan device layer for the experimental Vulkan backend (see llvkcontext.h)
 */

#include "linden_common.h"

#include "llvkcontext.h"

#if defined(LL_VULKAN)

#include <algorithm>
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
};

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
            {
                const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
                U32 best = 1;
                for (U32 c : { 2u, 4u, 8u, 16u, 32u, 64u }) if (counts & c) best = c;
                mCaps.mMaxSampleCount = best;
            }

            VkPhysicalDevicePushDescriptorPropertiesKHR push = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
            VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props2.pNext = &push;
            vkGetPhysicalDeviceProperties2(dev, &props2);
            mCaps.mMaxPushDescriptors = push.maxPushDescriptors;
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
    dci.enabledExtensionCount = (U32)LL_ARRAY_SIZE(REQUIRED_DEVICE_EXTENSIONS);
    dci.ppEnabledExtensionNames = REQUIRED_DEVICE_EXTENSIONS;
    if (!vk_ok(vkCreateDevice(mPhysical, &dci, nullptr, &mDevice), "vkCreateDevice"))
    {
        return false;
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
    return vk_ok(vmaCreateAllocator(&ai, &mAllocator), "vmaCreateAllocator");
}

bool Context::createFrames()
{
    for (Frame& f : mFrames)
    {
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
        for (Frame& f : mFrames)
        {
            destroyFrameResources(f);
        }
        if (mImmediateFence) vkDestroyFence(mDevice, mImmediateFence, nullptr);
        if (mImmediatePool) vkDestroyCommandPool(mDevice, mImmediatePool, nullptr);
        mImmediateFence = VK_NULL_HANDLE;
        mImmediatePool = VK_NULL_HANDLE;
        mImmediateCmd = VK_NULL_HANDLE;
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

VkCommandBuffer Context::beginFrame(bool wait)
{
    llassert(!mInFrame);
    Frame& f = mFrames[mFrameSlot];

    if (!wait && vkGetFenceStatus(mDevice, f.mFence) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    vkWaitForFences(mDevice, 1, &f.mFence, VK_TRUE, UINT64_MAX);
    vkResetFences(mDevice, 1, &f.mFence);

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
    mInFrame = true;
    return f.mCmd;
}

void Context::endFrame(VkSemaphore wait_semaphore, VkSemaphore signal_semaphore)
{
    llassert(mInFrame);
    Frame& f = mFrames[mFrameSlot];
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
    mFrameSlot = (mFrameSlot + 1) % FRAMES_IN_FLIGHT;
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
    return ok;
}

bool Context::frameFinished(U64 n)
{
    if (n >= mFrameNumber)
    {
        return false;
    }
    if (mFrameNumber - n >= FRAMES_IN_FLIGHT)
    {
        return true;        // its slot has been reused, which waited for it
    }
    return vkGetFenceStatus(mDevice, mFrames[n % FRAMES_IN_FLIGHT].mFence) == VK_SUCCESS;
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
    const U32 slot = mInFrame ? mFrameSlot : (mFrameSlot + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT;
    mFrames[slot].mDeferred.push_back(std::move(fn));
}

bool Context::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory, Buffer& out)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
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
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    {
        image_count = caps.maxImageCount;
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
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
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
    VkResult r = vkAcquireNextImageKHR(Context::instance().getDevice(), mSwapchain, timeout_ns, signal, VK_NULL_HANDLE, &index);
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
    VkResult r = vkQueuePresentKHR(Context::instance().getQueue(), &pi);
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

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, U32 mips, U32 layers,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
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
