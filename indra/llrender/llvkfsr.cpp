/**
 * @file llvkfsr.cpp
 * @brief AMD FSR 3.1 upscaler for the Vulkan mirror (see llvkfsr.h)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkfsr.h"

#if defined(LL_FSR)
#include <vulkan/vulkan.h>
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/vk/ffx_api_vk.h"
#endif

#include <vector>
#include <cmath>

namespace LLVkFsr
{

using namespace LLVk;       // the loaded Vulkan entry points live there

VkImageLayout inputLayout(const LLVk::Image&)
{
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;       // (the depth image too: it is only sampled; the backend is told COMPUTE_READ for every input)
}

VkImageLayout outputLayout()
{
    return VK_IMAGE_LAYOUT_GENERAL;
}

void addDeviceExtensions(VkPhysicalDevice device, std::vector<const char*>& out)
{
    std::vector<VkExtensionProperties> available;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &n, nullptr);
    available.resize(n);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &n, available.data());
    for (const char* wanted : { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_AMD_BUFFER_MARKER_EXTENSION_NAME })
    {
        bool have = false, exists = false;
        for (const char* e : out) have = have || strcmp(e, wanted) == 0;
        for (const auto& a : available) exists = exists || strcmp(a.extensionName, wanted) == 0;
        if (exists && !have)
        {
            out.push_back(wanted);          // (the macros are string literals: they outlive the device creation)
            LL_INFOS("VkFsr") << "enabling extension " << wanted << " for FSR" << LL_ENDL;
        }
    }
}

#if !defined(LL_FSR)

bool init(VkInstance, VkPhysicalDevice, VkDevice)
{
    LL_WARNS("VkFsr") << "this build has no FSR support (the FidelityFX SDK was not found when it was configured)" << LL_ENDL;
    return false;
}
bool ready() { return false; }
void shutdown() {}
bool evaluate(VkCommandBuffer, const LLVkDlss::Params&) { return false; }

#else

namespace
{

bool gReady = false;
VkInstance gInstance = VK_NULL_HANDLE;
VkPhysicalDevice gPhysical = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
HMODULE gModule = nullptr;
ffxFunctions gFn = {};

// A context is made for a maximum render and output size (a little over the first request) and serves every request that fits inside it and is not much smaller (the dispatch carries the
// real sizes, the context is made with FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION): the live view at 1713x898 and the snapshot at 1713x911 share one, where a context per exact size made a new one
// (memory the viewer's own accounting does not see) for every snapshot, at the exit one with the video memory at 93% (device lost, 2026-10-27). Contexts of clearly different sizes (a hi-res
// snapshot) are kept for when the size comes back and given back once the frame that last used them has completed (the same scheme as the DLSS features).
struct Ctx
{
    U32 mFlags = 0;                                  // the creation flags the caller asked for (without the dynamic resolution bit)
    U32 mMaxRenderW = 0, mMaxRenderH = 0, mMaxOutW = 0, mMaxOutH = 0;
    ffxContext mContext = nullptr;
    U64 mLastFrame = 0;
};
std::vector<Ctx> gContexts;                                  // [0..n): all live contexts; the last one used is gCurrent
std::vector<std::pair<ffxContext, U64>> gRetired;            // pushed out: destroyed once the frame that last used them has completed
size_t gCurrent = ~(size_t)0;
const size_t CONTEXTS_MAX = 4;
const U64 CONTEXT_IDLE_FRAMES = 360;

float env_float(const char* name)
{
    char buf[32] = {};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 ? (float)atof(buf) : 1.f;
}

bool fsr_ok(ffxReturnCode_t r, const char* what)
{
    if (r != FFX_API_RETURN_OK)
    {
        LL_WARNS("VkFsr") << what << " failed: " << r << LL_ENDL;
        return false;
    }
    return true;
}

void destroy_context(ffxContext& context)
{
    if (context)
    {
        gFn.DestroyContext(&context, nullptr);
        context = nullptr;
    }
}

void reap()
{
    Context& ctx = Context::instance();
    const U64 frame_now = ctx.getFrameNumber();
    for (size_t i = 0; i < gContexts.size(); )
    {
        if (i != gCurrent && frame_now > gContexts[i].mLastFrame + CONTEXT_IDLE_FRAMES)
        {
            gRetired.emplace_back(gContexts[i].mContext, gContexts[i].mLastFrame);
            gContexts.erase(gContexts.begin() + i);
            if (gCurrent != ~(size_t)0 && gCurrent > i)
            {
                --gCurrent;
            }
        }
        else
        {
            ++i;
        }
    }
    for (size_t i = 0; i < gRetired.size(); )
    {
        if (ctx.frameFinished(gRetired[i].second))
        {
            destroy_context(gRetired[i].first);
            gRetired.erase(gRetired.begin() + i);
        }
        else
        {
            ++i;
        }
    }
}

// a context serves a request when it was made for at least these sizes and is not much bigger (within 1.6 times the pixels: a context made for a hi-res snapshot is not used for the window)
bool fits(const Ctx& c, U32 flags, const LLVkDlss::Params& p)
{
    if (c.mFlags != flags || p.mRenderWidth > c.mMaxRenderW || p.mRenderHeight > c.mMaxRenderH || p.mOutputWidth > c.mMaxOutW || p.mOutputHeight > c.mMaxOutH)
    {
        return false;
    }
    const double render_ratio = (double)c.mMaxRenderW * c.mMaxRenderH / ((double)p.mRenderWidth * p.mRenderHeight);
    const double out_ratio = (double)c.mMaxOutW * c.mMaxOutH / ((double)p.mOutputWidth * p.mOutputHeight);
    return render_ratio <= 1.6 && out_ratio <= 1.6;
}

// the context for this request: one that fits, or a new one (the oldest other context is pushed out when there are too many)
ffxContext context_for(U32 flags, const LLVkDlss::Params& p)
{
    reap();
    for (size_t i = 0; i < gContexts.size(); ++i)
    {
        if (fits(gContexts[i], flags, p))
        {
            gCurrent = i;
            return gContexts[i].mContext;
        }
    }
    while (gContexts.size() >= CONTEXTS_MAX)
    {
        size_t oldest = 0;
        for (size_t i = 1; i < gContexts.size(); ++i)
        {
            if (gContexts[i].mLastFrame < gContexts[oldest].mLastFrame) oldest = i;
        }
        gRetired.emplace_back(gContexts[oldest].mContext, gContexts[oldest].mLastFrame);
        gContexts.erase(gContexts.begin() + oldest);
    }

    ffxCreateBackendVKDesc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = gDevice;
    backend.vkPhysicalDevice = gPhysical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &backend.header;
    // 4 % over the request in each direction: the next snapshot or window resize of a few pixels fits in this context
    const U32 max_render_w = (U32)ceilf(p.mRenderWidth * 1.04f), max_render_h = (U32)ceilf(p.mRenderHeight * 1.04f);
    const U32 max_out_w = (U32)ceilf(p.mOutputWidth * 1.04f), max_out_h = (U32)ceilf(p.mOutputHeight * 1.04f);
    create.flags = flags | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    create.maxRenderSize = { max_render_w, max_render_h };
    create.maxUpscaleSize = { max_out_w, max_out_h };
    create.fpMessage = [](uint32_t type, const wchar_t* message)
    {
        char text[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, message, -1, text, (int)sizeof(text) - 1, nullptr, nullptr);
        LL_INFOS("VkFsr") << "FSR " << (type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "warning: ") << text << LL_ENDL;
    };

    ffxContext context = nullptr;
    if (!fsr_ok(gFn.CreateContext(&context, &create.header, nullptr), "ffxCreateContext (upscale)"))
    {
        return nullptr;
    }
    Ctx c;
    c.mFlags = flags;
    c.mMaxRenderW = max_render_w;
    c.mMaxRenderH = max_render_h;
    c.mMaxOutW = max_out_w;
    c.mMaxOutH = max_out_h;
    c.mContext = context;
    gContexts.push_back(c);
    gCurrent = gContexts.size() - 1;
    LL_INFOS("VkFsr") << "FSR upscale context created for " << p.mRenderWidth << "x" << p.mRenderHeight << " -> " << p.mOutputWidth << "x" << p.mOutputHeight << " (made for up to "
                      << max_render_w << "x" << max_render_h << " -> " << max_out_w << "x" << max_out_h << "), flags 0x" << std::hex << create.flags << std::dec
                      << ", " << (gContexts.size()) << " contexts" << LL_ENDL;
    return context;
}

FfxApiResource resource_of(const LLVk::Image& image, bool write)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.mFormat;
    info.extent = { image.mExtent.width, image.mExtent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    FfxApiResource r = {};
    r.resource = (void*)image.mImage;
    r.description = ffxApiGetImageResourceDescriptionVK(image.mImage, info, 0);
    r.state = write ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gReady)
    {
        return true;
    }
    gModule = LoadLibraryA("amd_fidelityfx_vk.dll");
    if (!gModule)
    {
        LL_WARNS("VkFsr") << "FSR is not available: amd_fidelityfx_vk.dll was not found next to the viewer (error " << GetLastError() << ")" << LL_ENDL;
        return false;
    }
    ffxLoadFunctions(&gFn, gModule);
    if (!gFn.CreateContext || !gFn.DestroyContext || !gFn.Dispatch || !gFn.Query)
    {
        LL_WARNS("VkFsr") << "FSR is not available: amd_fidelityfx_vk.dll does not export the ffx-api" << LL_ENDL;
        FreeLibrary(gModule);
        gModule = nullptr;
        return false;
    }
    gInstance = instance;
    gPhysical = physical;
    gDevice = device;
    gReady = true;
    LL_INFOS("VkFsr") << "FSR is ready (the contexts are made when the first frame is upscaled)" << LL_ENDL;
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
    for (Ctx& c : gContexts)
    {
        destroy_context(c.mContext);
    }
    gContexts.clear();
    for (auto& r : gRetired)
    {
        destroy_context(r.first);
    }
    gRetired.clear();
    gCurrent = ~(size_t)0;
    FreeLibrary(gModule);
    gModule = nullptr;
    gReady = false;
}

bool evaluate(VkCommandBuffer cmd, const LLVkDlss::Params& p)
{
    if (!gReady || !p.mColor || !p.mDepth || !p.mMotion || !p.mOutput || p.mRayReconstruction)
    {
        return false;           // (no ray reconstruction here: the caller does not ask for it when rayReconstructionAvailable() is false)
    }
    // the colour is display-referred (gamma encoded) unless the caller says it is linear HDR; the motion vectors are in render pixels, and carry no jitter
    U32 flags = 0;
    if (p.mHdr)
    {
        flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    ffxContext context = context_for(flags, p);
    if (!context)
    {
        return false;
    }
    gContexts[gCurrent].mLastFrame = Context::instance().getFrameNumber();

    // LL_VK_FSR_MVX / MVY / JX / JY (default 1): multipliers for the signs, to find out which way round FSR wants them without rebuilding
    static const float mv_x = env_float("LL_VK_FSR_MVX"), mv_y = env_float("LL_VK_FSR_MVY"), j_x = env_float("LL_VK_FSR_JX"), j_y = env_float("LL_VK_FSR_JY");
    static U32 logged = 0;
    if (logged++ < 3)
    {
        LL_INFOS("VkFsr") << "evaluate " << logged << ": jitter " << p.mJitterX << "," << p.mJitterY << " reset " << p.mReset << " camera near " << p.mCameraNear << " far " << p.mCameraFar
                          << " fov " << p.mCameraFovY << " frame " << p.mFrameTimeMs << " ms, formats colour " << p.mColor->mFormat << " depth " << p.mDepth->mFormat << " motion " << p.mMotion->mFormat
                          << " output " << p.mOutput->mFormat << ", sign multipliers mv " << mv_x << "," << mv_y << " jitter " << j_x << "," << j_y << LL_ENDL;
    }

    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void*)cmd;
    d.color = resource_of(*p.mColor, false);
    d.depth = resource_of(*p.mDepth, false);
    d.motionVectors = resource_of(*p.mMotion, false);
    d.output = resource_of(*p.mOutput, true);
    d.jitterOffset = { p.mJitterX * j_x, p.mJitterY * j_y };
    d.motionVectorScale = { p.mMotionScaleX * mv_x, p.mMotionScaleY * mv_y };
    d.renderSize = { p.mRenderWidth, p.mRenderHeight };
    d.upscaleSize = { p.mOutputWidth, p.mOutputHeight };
    d.enableSharpening = false;             // (the viewer's own CAS pass follows)
    d.sharpness = 0.f;
    d.frameTimeDelta = p.mFrameTimeMs;
    d.preExposure = 1.f;
    d.reset = p.mReset;
    d.cameraNear = p.mCameraNear;
    d.cameraFar = p.mCameraFar;
    d.cameraFovAngleVertical = p.mCameraFovY;
    d.viewSpaceToMetersFactor = 1.f;        // (view space is metres)
    d.flags = p.mHdr ? 0u : (U32)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    return fsr_ok(gFn.Dispatch(&context, &d.header), "ffxDispatch (upscale)");
}

#endif // LL_FSR

}

#endif // LL_VULKAN
