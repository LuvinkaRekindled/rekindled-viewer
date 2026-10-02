/**
 * @file llvkprobe.cpp
 * @brief Phase 1 Vulkan bring-up probe (see llvkprobe.h)
 */

#include "linden_common.h"

#include "llvkprobe.h"

#if !defined(LL_VULKAN)

bool LLVkProbe::isCompiledIn() { return false; }
void LLVkProbe::setEnabled(bool) {}
void LLVkProbe::frame() {}
void LLVkProbe::shutdown() {}

#else // LL_VULKAN

#include <vector>
#include <cstring>

#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace
{

#define VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateWin32SurfaceKHR) X(vkEnumerateDeviceExtensionProperties)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateImageView) \
    X(vkDestroyImageView)

#define VK_DECLARE(name) PFN_##name name = nullptr;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

const U32 FRAMES_IN_FLIGHT = 2;
const wchar_t* const WINDOW_CLASS = L"LLVkProbeWindow";

struct FrameData
{
    VkCommandPool mPool = VK_NULL_HANDLE;
    VkCommandBuffer mCmd = VK_NULL_HANDLE;
    VkFence mFence = VK_NULL_HANDLE;
    VkSemaphore mAcquire = VK_NULL_HANDLE;
};

struct Probe
{
    HMODULE mLoader = nullptr;
    HWND mWindow = nullptr;
    bool mClassRegistered = false;
    volatile bool mClosed = false;
    volatile bool mResized = false;

    VkInstance mInstance = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    U32 mQueueFamily = 0;

    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D mExtent = { 0, 0 };
    std::vector<VkImage> mImages;
    std::vector<VkImageView> mViews;
    std::vector<VkSemaphore> mRenderDone; // one per swapchain image

    FrameData mFrames[FRAMES_IN_FLIGHT];
    U32 mFrameIndex = 0;
    U64 mFramesPresented = 0;
    bool mReady = false;
};

Probe sProbe;

LRESULT CALLBACK probe_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        sProbe.mClosed = true; // frame() notices and shuts the probe down; the window is destroyed there
        return 0;
    case WM_SIZE:
        sProbe.mResized = true;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool vk_ok(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
    {
        LL_WARNS("VkProbe") << what << " failed: VkResult " << (S32)r << LL_ENDL;
        return false;
    }
    return true;
}

bool load_loader()
{
    sProbe.mLoader = LoadLibraryA("vulkan-1.dll");
    if (!sProbe.mLoader)
    {
        LL_WARNS("VkProbe") << "vulkan-1.dll not found" << LL_ENDL;
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(sProbe.mLoader, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr)
    {
        return false;
    }
#define VK_LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(nullptr, #name);
    VK_GLOBAL_FUNCS(VK_LOAD)
#undef VK_LOAD
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
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

bool create_window()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = probe_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }
    sProbe.mClassRegistered = true;

    sProbe.mWindow = CreateWindowExW(0, WINDOW_CLASS, L"Firestorm Vulkan probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                     100, 100, 640, 360, nullptr, nullptr, inst, nullptr);
    return sProbe.mWindow != nullptr;
}

bool create_instance()
{
    U32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
    {
        vkEnumerateInstanceVersion(&version);
    }
    LL_INFOS("VkProbe") << "Vulkan loader instance version " << VK_API_VERSION_MAJOR(version) << "." << VK_API_VERSION_MINOR(version)
                        << "." << VK_API_VERSION_PATCH(version) << LL_ENDL;
    if (version < VK_API_VERSION_1_3)
    {
        LL_WARNS("VkProbe") << "Vulkan 1.3 is required" << LL_ENDL;
        return false;
    }

    U32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    const char* wanted[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    for (const char* name : wanted)
    {
        if (!has_extension(available, name))
        {
            LL_WARNS("VkProbe") << "missing instance extension " << name << LL_ENDL;
            return false;
        }
    }

    // optional validation, only when asked for (the layer must be findable through VK_LAYER_PATH)
    std::vector<const char*> layers;
    if (GetEnvironmentVariableA("LL_VK_VALIDATION", nullptr, 0) > 0)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Firestorm";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (U32)LL_ARRAY_SIZE(wanted);
    ci.ppEnabledExtensionNames = wanted;
    ci.enabledLayerCount = (U32)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    if (!vk_ok(vkCreateInstance(&ci, nullptr, &sProbe.mInstance), "vkCreateInstance"))
    {
        return false;
    }

#define VK_LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(sProbe.mInstance, #name);
    VK_INSTANCE_FUNCS(VK_LOAD)
#undef VK_LOAD

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = sProbe.mWindow;
    return vk_ok(vkCreateWin32SurfaceKHR(sProbe.mInstance, &sci, nullptr, &sProbe.mSurface), "vkCreateWin32SurfaceKHR");
}

bool create_device()
{
    U32 count = 0;
    vkEnumeratePhysicalDevices(sProbe.mInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(sProbe.mInstance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    U32 best_family = 0;
    S32 best_score = -1;
    for (VkPhysicalDevice dev : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        LL_INFOS("VkProbe") << "Vulkan device: " << props.deviceName << " api " << VK_API_VERSION_MAJOR(props.apiVersion) << "."
                            << VK_API_VERSION_MINOR(props.apiVersion) << "." << VK_API_VERSION_PATCH(props.apiVersion)
                            << " driver 0x" << std::hex << props.driverVersion << std::dec << LL_ENDL;
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        U32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, families.data());
        for (U32 i = 0; i < qcount; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, sProbe.mSurface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                S32 score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (score > best_score)
                {
                    best = dev;
                    best_family = i;
                    best_score = score;
                }
                break;
            }
        }
    }
    if (best == VK_NULL_HANDLE)
    {
        LL_WARNS("VkProbe") << "no Vulkan 1.3 device with graphics+present found" << LL_ENDL;
        return false;
    }
    sProbe.mPhysical = best;
    sProbe.mQueueFamily = best_family;

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    const float priority = 1.f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = best_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = extensions;
    if (!vk_ok(vkCreateDevice(best, &dci, nullptr, &sProbe.mDevice), "vkCreateDevice"))
    {
        return false;
    }

#define VK_LOAD(name) name = (PFN_##name)vkGetDeviceProcAddr(sProbe.mDevice, #name);
    VK_DEVICE_FUNCS(VK_LOAD)
#undef VK_LOAD

    vkGetDeviceQueue(sProbe.mDevice, best_family, 0, &sProbe.mQueue);
    return true;
}

void destroy_swapchain()
{
    if (sProbe.mDevice == VK_NULL_HANDLE)
    {
        return;
    }
    vkDeviceWaitIdle(sProbe.mDevice);
    for (VkSemaphore s : sProbe.mRenderDone)
    {
        vkDestroySemaphore(sProbe.mDevice, s, nullptr);
    }
    sProbe.mRenderDone.clear();
    for (VkImageView v : sProbe.mViews)
    {
        vkDestroyImageView(sProbe.mDevice, v, nullptr);
    }
    sProbe.mViews.clear();
    sProbe.mImages.clear();
    if (sProbe.mSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(sProbe.mDevice, sProbe.mSwapchain, nullptr);
        sProbe.mSwapchain = VK_NULL_HANDLE;
    }
}

// false if the window currently has no drawable area (minimised); not an error
bool create_swapchain(bool& drawable)
{
    drawable = false;
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(sProbe.mPhysical, sProbe.mSurface, &caps), "surface capabilities"))
    {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        RECT rc;
        GetClientRect(sProbe.mWindow, &rc);
        extent.width = (U32)(rc.right - rc.left);
        extent.height = (U32)(rc.bottom - rc.top);
    }
    if (extent.width == 0 || extent.height == 0)
    {
        return true; // minimised
    }

    U32 fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(sProbe.mPhysical, sProbe.mSurface, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(sProbe.mPhysical, sProbe.mSurface, &fcount, formats.data());
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

    destroy_swapchain();

    VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sc.surface = sProbe.mSurface;
    sc.minImageCount = image_count;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    if (!vk_ok(vkCreateSwapchainKHR(sProbe.mDevice, &sc, nullptr, &sProbe.mSwapchain), "vkCreateSwapchainKHR"))
    {
        return false;
    }
    sProbe.mFormat = chosen.format;
    sProbe.mExtent = extent;

    U32 icount = 0;
    vkGetSwapchainImagesKHR(sProbe.mDevice, sProbe.mSwapchain, &icount, nullptr);
    sProbe.mImages.resize(icount);
    vkGetSwapchainImagesKHR(sProbe.mDevice, sProbe.mSwapchain, &icount, sProbe.mImages.data());

    for (VkImage image : sProbe.mImages)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateImageView(sProbe.mDevice, &vi, nullptr, &view), "vkCreateImageView"))
        {
            return false;
        }
        sProbe.mViews.push_back(view);

        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore sem = VK_NULL_HANDLE;
        if (!vk_ok(vkCreateSemaphore(sProbe.mDevice, &si, nullptr, &sem), "vkCreateSemaphore"))
        {
            return false;
        }
        sProbe.mRenderDone.push_back(sem);
    }

    LL_INFOS("VkProbe") << "swapchain " << extent.width << "x" << extent.height << " format " << (S32)chosen.format << " images " << icount << LL_ENDL;
    drawable = true;
    return true;
}

bool create_frames()
{
    for (FrameData& f : sProbe.mFrames)
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = sProbe.mQueueFamily;
        if (!vk_ok(vkCreateCommandPool(sProbe.mDevice, &pi, nullptr, &f.mPool), "vkCreateCommandPool"))
        {
            return false;
        }
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f.mPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(sProbe.mDevice, &ai, &f.mCmd), "vkAllocateCommandBuffers"))
        {
            return false;
        }
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!vk_ok(vkCreateFence(sProbe.mDevice, &fi, nullptr, &f.mFence), "vkCreateFence")
            || !vk_ok(vkCreateSemaphore(sProbe.mDevice, &si, nullptr, &f.mAcquire), "vkCreateSemaphore"))
        {
            return false;
        }
    }
    return true;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
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
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void hsv_to_rgb(F32 h, F32& r, F32& g, F32& b)
{
    const F32 x = h * 6.f;
    const S32 i = (S32)x % 6;
    const F32 f = x - floorf(x);
    const F32 q = 1.f - f;
    switch (i)
    {
    case 0: r = 1; g = f; b = 0; break;
    case 1: r = q; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = f; break;
    case 3: r = 0; g = q; b = 1; break;
    case 4: r = f; g = 0; b = 1; break;
    default: r = 1; g = 0; b = q; break;
    }
}

} // anonymous namespace

bool LLVkProbe::isCompiledIn() { return true; }

void LLVkProbe::shutdown()
{
    if (sProbe.mDevice != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(sProbe.mDevice);
        destroy_swapchain();
        for (FrameData& f : sProbe.mFrames)
        {
            if (f.mAcquire) vkDestroySemaphore(sProbe.mDevice, f.mAcquire, nullptr);
            if (f.mFence) vkDestroyFence(sProbe.mDevice, f.mFence, nullptr);
            if (f.mPool) vkDestroyCommandPool(sProbe.mDevice, f.mPool, nullptr);
            f = FrameData();
        }
        vkDestroyDevice(sProbe.mDevice, nullptr);
        sProbe.mDevice = VK_NULL_HANDLE;
    }
    if (sProbe.mInstance != VK_NULL_HANDLE)
    {
        if (sProbe.mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(sProbe.mInstance, sProbe.mSurface, nullptr);
            sProbe.mSurface = VK_NULL_HANDLE;
        }
        vkDestroyInstance(sProbe.mInstance, nullptr);
        sProbe.mInstance = VK_NULL_HANDLE;
    }
    if (sProbe.mWindow)
    {
        DestroyWindow(sProbe.mWindow);
        sProbe.mWindow = nullptr;
    }
    if (sProbe.mReady)
    {
        LL_INFOS("VkProbe") << "Vulkan probe shut down after " << sProbe.mFramesPresented << " frames" << LL_ENDL;
    }
    sProbe.mReady = false;
    sProbe.mClosed = false;
    sProbe.mResized = false;
    sProbe.mFramesPresented = 0;
    sProbe.mFrameIndex = 0;
}

void LLVkProbe::setEnabled(bool enabled)
{
    if (!enabled)
    {
        if (sProbe.mReady || sProbe.mWindow)
        {
            shutdown();
        }
        return;
    }
    if (sProbe.mReady || sProbe.mClosed)
    {
        return;
    }

    static bool failed = false; // do not retry every frame after a failure
    if (failed)
    {
        return;
    }

    bool drawable = false;
    if (!load_loader() || !create_window() || !create_instance() || !create_device() || !create_frames()
        || !create_swapchain(drawable))
    {
        LL_WARNS("VkProbe") << "Vulkan probe initialisation failed" << LL_ENDL;
        shutdown();
        failed = true;
        return;
    }
    sProbe.mReady = true;
    LL_INFOS("VkProbe") << "Vulkan probe ready" << LL_ENDL;
}

void LLVkProbe::frame()
{
    if (sProbe.mClosed)
    {
        shutdown();
        return;
    }
    if (!sProbe.mReady)
    {
        return;
    }

    if (sProbe.mResized || sProbe.mSwapchain == VK_NULL_HANDLE)
    {
        sProbe.mResized = false;
        bool drawable = false;
        if (!create_swapchain(drawable))
        {
            shutdown();
            return;
        }
        if (!drawable)
        {
            return;
        }
    }

    FrameData& f = sProbe.mFrames[sProbe.mFrameIndex];
    // do not block the viewer's frame if the GPU is behind; skip this probe frame instead
    if (vkWaitForFences(sProbe.mDevice, 1, &f.mFence, VK_TRUE, 0) != VK_SUCCESS)
    {
        return;
    }

    U32 image_index = 0;
    VkResult r = vkAcquireNextImageKHR(sProbe.mDevice, sProbe.mSwapchain, 0, f.mAcquire, VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        sProbe.mResized = true;
        return;
    }
    if (r == VK_NOT_READY || r == VK_TIMEOUT)
    {
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        vk_ok(r, "vkAcquireNextImageKHR");
        shutdown();
        return;
    }

    vkResetFences(sProbe.mDevice, 1, &f.mFence);
    vkResetCommandBuffer(f.mCmd, 0);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.mCmd, &bi);

    VkImage image = sProbe.mImages[image_index];
    image_barrier(f.mCmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    F32 hue = (F32)(GetTickCount64() % 6000) / 6000.f;
    VkClearValue clear = {};
    hsv_to_rgb(hue, clear.color.float32[0], clear.color.float32[1], clear.color.float32[2]);
    clear.color.float32[3] = 1.f;

    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = sProbe.mViews[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = clear;

    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, sProbe.mExtent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(f.mCmd, &ri);
    vkCmdEndRendering(f.mCmd);

    image_barrier(f.mCmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_NONE, 0);
    vkEndCommandBuffer(f.mCmd);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.mAcquire;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.mCmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sProbe.mRenderDone[image_index];
    if (!vk_ok(vkQueueSubmit(sProbe.mQueue, 1, &si, f.mFence), "vkQueueSubmit"))
    {
        shutdown();
        return;
    }

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sProbe.mRenderDone[image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sProbe.mSwapchain;
    pi.pImageIndices = &image_index;
    r = vkQueuePresentKHR(sProbe.mQueue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        sProbe.mResized = true;
    }
    else if (r != VK_SUCCESS)
    {
        vk_ok(r, "vkQueuePresentKHR");
        shutdown();
        return;
    }

    ++sProbe.mFramesPresented;
    sProbe.mFrameIndex = (sProbe.mFrameIndex + 1) % FRAMES_IN_FLIGHT;
    if (sProbe.mFramesPresented == 1)
    {
        LL_INFOS("VkProbe") << "first Vulkan frame presented" << LL_ENDL;
    }
}

#endif // LL_VULKAN
