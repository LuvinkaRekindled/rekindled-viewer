/**
 * @file rkvkfsr.cpp
 * @brief AMD FSR 3.1 upscaler for the Vulkan mirror (see rkvkfsr.h)
 *
 * Built from the FidelityFX SDK v1.1.4 source (MIT) into static libraries (ffx_fsr3upscaler, ffx_backend_vk: llrender/CMakeLists.txt, doc/vulkan/PLAN-amd-denoiser.md): the native
 * FidelityFX API, no amd_fidelityfx_vk.dll. It follows what the SDK's ffx-api provider does with it (ffx-api/src/ffx_provider_fsr3upscale.cpp): a backend interface with its own scratch memory
 * for each context, the three shared resources made through the backend after the context and given to every dispatch.
 */

#include "linden_common.h"

#if defined(RK_VULKAN)

#include "rkvkfsr.h"

#if defined(RK_FSR)
#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#endif

#include <vector>
#include <memory>
#include <cmath>

namespace RKVkFsr
{

using namespace RKVk;       // the loaded Vulkan entry points live there

VkImageLayout inputLayout(const RKVk::Image&)
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

#if !defined(RK_FSR)

bool init(VkInstance, VkPhysicalDevice, VkDevice)
{
    LL_WARNS("VkFsr") << "this build has no FSR support (the FidelityFX SDK libraries were not found when it was configured)" << LL_ENDL;
    return false;
}
bool ready() { return false; }
bool failed() { return false; }
void shutdown() {}
bool evaluate(VkCommandBuffer, const RKVkUpscale::Params&) { return false; }

#else

namespace
{

bool gReady = false;
bool gFailed = false;       // a context could not be made (a pipeline the driver refuses, ...): the viewer is told, it draws at the window size from then on
VkInstance gInstance = VK_NULL_HANDLE;
VkPhysicalDevice gPhysical = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
VkDeviceContext gDeviceContext = {};        // (the FidelityFX device points at it: it has to stay)

// A context is made for a maximum render and output size (a little over the first request) and serves every request that fits inside it and is not much smaller (the dispatch carries the
// real sizes, the context is made with FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION): the live view at 1713x898 and the snapshot at 1713x911 share one, where a context per exact size made a new one
// (memory the viewer's own accounting does not see) for every snapshot, at the exit one with the video memory at 93% (device lost, 2026-10-27). Contexts of clearly different sizes (a hi-res
// snapshot) are kept for when the size comes back and given back once the frame that last used them has completed (the same scheme as the DLSS features).
struct Ctx
{
    U32 mFlags = 0;                                  // the creation flags the caller asked for (without the dynamic resolution bit)
    U32 mMaxRenderW = 0, mMaxRenderH = 0, mMaxOutW = 0, mMaxOutH = 0;
    bool mMade = false;
    FfxInterface mBackend = {};                      // its own backend interface and scratch memory (one context a interface, as the SDK's ffx-api provider does)
    void* mScratch = nullptr;
    FfxFsr3UpscalerContext mContext = {};
    FfxResourceInternal mShared[3] = {};             // dilated depth, dilated motion vectors, reconstructed previous nearest depth
    U64 mLastFrame = 0;
};
std::vector<Ctx*> gContexts;                                 // [0..n): all live contexts (on the heap: the FidelityFX context must not move); the last one used is gCurrent
std::vector<Ctx*> gRetired;                                  // pushed out: destroyed once the frame that last used them has completed
size_t gCurrent = ~(size_t)0;
const size_t CONTEXTS_MAX = 4;
const U64 CONTEXT_IDLE_FRAMES = 360;

float env_float(const char* name)
{
    char buf[32] = {};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 ? (float)atof(buf) : 1.f;
}

bool fsr_ok(FfxErrorCode r, const char* what)
{
    if (r != FFX_OK)
    {
        LL_WARNS("VkFsr") << what << " failed: " << (S32)r << LL_ENDL;
        return false;
    }
    return true;
}

void destroy_context(Ctx& c)
{
    if (c.mMade)
    {
        for (FfxResourceInternal& r : c.mShared)
        {
            c.mBackend.fpDestroyResource(&c.mBackend, r, 0);
        }
        ffxFsr3UpscalerContextDestroy(&c.mContext);
        c.mMade = false;
    }
    free(c.mScratch);
    c.mScratch = nullptr;
}

void reap()
{
    Context& ctx = Context::instance();
    const U64 frame_now = ctx.getFrameNumber();
    for (size_t i = 0; i < gContexts.size(); )
    {
        if (i != gCurrent && frame_now > gContexts[i]->mLastFrame + CONTEXT_IDLE_FRAMES)
        {
            gRetired.push_back(gContexts[i]);
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
        if (ctx.frameFinished(gRetired[i]->mLastFrame))
        {
            destroy_context(*gRetired[i]);
            delete gRetired[i];
            gRetired.erase(gRetired.begin() + i);
        }
        else
        {
            ++i;
        }
    }
}

// a context serves a request when it was made for at least these sizes and is not much bigger (within 1.6 times the pixels: a context made for a hi-res snapshot is not used for the window)
bool fits(const Ctx& c, U32 flags, const RKVkUpscale::Params& p)
{
    if (c.mFlags != flags || p.mRenderWidth > c.mMaxRenderW || p.mRenderHeight > c.mMaxRenderH || p.mOutputWidth > c.mMaxOutW || p.mOutputHeight > c.mMaxOutH)
    {
        return false;
    }
    const double render_ratio = (double)c.mMaxRenderW * c.mMaxRenderH / ((double)p.mRenderWidth * p.mRenderHeight);
    const double out_ratio = (double)c.mMaxOutW * c.mMaxOutH / ((double)p.mOutputWidth * p.mOutputHeight);
    return render_ratio <= 1.6 && out_ratio <= 1.6;
}

// the context for this request: one that fits, or a new one (the oldest other context is pushed out when there are too many); null when it could not be made
Ctx* context_for(U32 flags, const RKVkUpscale::Params& p)
{
    reap();
    for (size_t i = 0; i < gContexts.size(); ++i)
    {
        if (fits(*gContexts[i], flags, p))
        {
            gCurrent = i;
            return gContexts[i];
        }
    }
    while (gContexts.size() >= CONTEXTS_MAX)
    {
        size_t oldest = 0;
        for (size_t i = 1; i < gContexts.size(); ++i)
        {
            if (gContexts[i]->mLastFrame < gContexts[oldest]->mLastFrame) oldest = i;
        }
        gRetired.push_back(gContexts[oldest]);
        gContexts.erase(gContexts.begin() + oldest);
        gCurrent = ~(size_t)0;
    }

    std::unique_ptr<Ctx> owner(new Ctx);
    Ctx& c = *owner;
    const size_t scratch_size = ffxGetScratchMemorySizeVK(gPhysical, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    c.mScratch = calloc(1, scratch_size);
    if (!c.mScratch)
    {
        return nullptr;
    }
    const FfxDevice ffx_device = ffxGetDeviceVK(&gDeviceContext);
    if (!fsr_ok(ffxGetInterfaceVK(&c.mBackend, ffx_device, c.mScratch, scratch_size, FFX_FSR3UPSCALER_CONTEXT_COUNT), "ffxGetInterfaceVK (FSR)"))
    {
        destroy_context(c);
        return nullptr;
    }

    // 4 % over the request in each direction: the next snapshot or window resize of a few pixels fits in this context
    const U32 max_render_w = (U32)ceilf(p.mRenderWidth * 1.04f), max_render_h = (U32)ceilf(p.mRenderHeight * 1.04f);
    const U32 max_out_w = (U32)ceilf(p.mOutputWidth * 1.04f), max_out_h = (U32)ceilf(p.mOutputHeight * 1.04f);
    FfxFsr3UpscalerContextDescription create = {};
    create.flags = flags | FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
    create.maxRenderSize = { max_render_w, max_render_h };
    create.maxUpscaleSize = { max_out_w, max_out_h };
    create.backendInterface = c.mBackend;
    create.fpMessage = [](FfxMsgType type, const wchar_t* message)
    {
        char text[512] = {};
#if LL_WINDOWS
        WideCharToMultiByte(CP_UTF8, 0, message, -1, text, (int)sizeof(text) - 1, nullptr, nullptr);
#else
        // (the SDK's messages are ASCII: the characters of the 32 bit wchar_t below 128 as they are, the others as '?')
        for (size_t i = 0; message && message[i] && i + 1 < sizeof(text); ++i)
        {
            text[i] = message[i] < 128 ? (char)message[i] : '?';
        }
#endif
        LL_INFOS("VkFsr") << "FSR " << (type == FFX_MESSAGE_TYPE_ERROR ? "error: " : "warning: ") << text << LL_ENDL;
    };

    if (!fsr_ok(ffxFsr3UpscalerContextCreate(&c.mContext, &create), "ffxFsr3UpscalerContextCreate"))
    {
        destroy_context(c);
        return nullptr;
    }
    c.mMade = true;

    // the viewer's reactive mask (where blended surfaces changed the pixel) at full strength made the upscaler throw its history away there and the blended screen jittered (the user 2026-11-21): the
    // motion pass gives those pixels the camera's motion so the history is good, and the mask only helps a little. RK_VK_FSR_REACTIVE_SCALE (default 0.3) scales it (the SDK's fReactivenessScale)
    {
        char buf[32] = {};
        float scale = GetEnvironmentVariableA("RK_VK_FSR_REACTIVE_SCALE", buf, sizeof(buf)) > 0 ? (float)atof(buf) : 0.3f;
        fsr_ok(ffxFsr3UpscalerSetConstant(&c.mContext, FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, &scale), "ffxFsr3UpscalerSetConstant (reactiveness scale)");
    }

    // the three shared resources the dispatch writes (the SDK's ffx-api provider makes them here too, through the backend)
    FfxFsr3UpscalerSharedResourceDescriptions shared = {};
    if (!fsr_ok(ffxFsr3UpscalerGetSharedResourceDescriptions(&c.mContext, &shared), "ffxFsr3UpscalerGetSharedResourceDescriptions"))
    {
        destroy_context(c);
        return nullptr;
    }
    const FfxCreateResourceDescription* descs[3] = { &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth };
    for (int i = 0; i < 3; ++i)
    {
        if (!fsr_ok(c.mBackend.fpCreateResource(&c.mBackend, descs[i], 0, &c.mShared[i]), "creating a shared resource"))
        {
            // the ones made so far are destroyed with the context: the rest are zero (the backend ignores them)
            destroy_context(c);
            return nullptr;
        }
    }

    c.mFlags = flags;
    c.mMaxRenderW = max_render_w;
    c.mMaxRenderH = max_render_h;
    c.mMaxOutW = max_out_w;
    c.mMaxOutH = max_out_h;
    gContexts.push_back(owner.release());
    gCurrent = gContexts.size() - 1;
    LL_INFOS("VkFsr") << "FSR upscale context created for " << p.mRenderWidth << "x" << p.mRenderHeight << " -> " << p.mOutputWidth << "x" << p.mOutputHeight << " (made for up to "
                      << max_render_w << "x" << max_render_h << " -> " << max_out_w << "x" << max_out_h << "), flags 0x" << std::hex << create.flags << std::dec
                      << ", " << (gContexts.size()) << " contexts" << LL_ENDL;
    return gContexts[gCurrent];
}

FfxResource resource_of(const RKVk::Image& image, bool write, const wchar_t* name)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.mFormat;
    info.extent = { image.mExtent.width, image.mExtent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(image.mImage, info, write ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY);
    return ffxGetResourceVK((void*)image.mImage, desc, name, write ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}

}

bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device)
{
    if (gReady)
    {
        return true;
    }
    if (!vkGetDeviceProcAddr)
    {
        LL_WARNS("VkFsr") << "FSR is not available: the device's function loader is missing" << LL_ENDL;
        return false;
    }
    gInstance = instance;
    gPhysical = physical;
    gDevice = device;
    gDeviceContext.vkDevice = device;
    gDeviceContext.vkPhysicalDevice = physical;
    gDeviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    gReady = true;
    LL_INFOS("VkFsr") << "FSR is ready (FidelityFX SDK " << FFX_FSR3UPSCALER_VERSION_MAJOR << "." << FFX_FSR3UPSCALER_VERSION_MINOR << "." << FFX_FSR3UPSCALER_VERSION_PATCH
                      << " built into the viewer; the contexts are made when the first frame is upscaled)" << LL_ENDL;
    return true;
}

bool ready()
{
    return gReady;
}

bool failed()
{
    return gFailed;
}

void shutdown()
{
    if (!gReady)
    {
        return;
    }
    vkDeviceWaitIdle(gDevice);
    for (Ctx* c : gContexts)
    {
        destroy_context(*c);
        delete c;
    }
    gContexts.clear();
    for (Ctx* c : gRetired)
    {
        destroy_context(*c);
        delete c;
    }
    gRetired.clear();
    gCurrent = ~(size_t)0;
    gReady = false;
}

bool evaluate(VkCommandBuffer cmd, const RKVkUpscale::Params& p)
{
    if (!gReady || !p.mColor || !p.mDepth || !p.mMotion || !p.mOutput)
    {
        return false;
    }
    // the colour is display-referred (gamma encoded) unless the caller says it is linear HDR; the motion vectors are in render pixels, and carry no jitter
    U32 flags = 0;
    if (p.mHdr)
    {
        flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    Ctx* context = context_for(flags, p);
    if (!context)
    {
        if (!gFailed)
        {
            gFailed = true;
            LL_WARNS("VkFsr") << "FSR cannot make its context on this device: the upscaler is given up, the world is drawn at the window size instead" << LL_ENDL;
        }
        return false;
    }
    context->mLastFrame = Context::instance().getFrameNumber();

    // RK_VK_FSR_MVX / MVY / JX / JY (default 1): multipliers for the signs, to find out which way round FSR wants them without rebuilding
    static const float mv_x = env_float("RK_VK_FSR_MVX"), mv_y = env_float("RK_VK_FSR_MVY"), j_x = env_float("RK_VK_FSR_JX"), j_y = env_float("RK_VK_FSR_JY");
    static U32 logged = 0;
    if (logged++ < 3)
    {
        LL_INFOS("VkFsr") << "evaluate " << logged << ": jitter " << p.mJitterX << "," << p.mJitterY << " reset " << p.mReset << " camera near " << p.mCameraNear << " far " << p.mCameraFar
                          << " fov " << p.mCameraFovY << " frame " << p.mFrameTimeMs << " ms, formats colour " << p.mColor->mFormat << " depth " << p.mDepth->mFormat << " motion " << p.mMotion->mFormat
                          << " output " << p.mOutput->mFormat << ", sign multipliers mv " << mv_x << "," << mv_y << " jitter " << j_x << "," << j_y << LL_ENDL;
    }

    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = resource_of(*p.mColor, false, L"fsr_color");
    d.depth = resource_of(*p.mDepth, false, L"fsr_depth");
    d.motionVectors = resource_of(*p.mMotion, false, L"fsr_motion");
    d.output = resource_of(*p.mOutput, true, L"fsr_output");
    // (exposure and transparency masks stay empty: null resources; the reactive mask is the viewer's own, from the blended draws: pipeline.cpp generateReactiveMask)
    if (p.mReactive)
    {
        d.reactive = resource_of(*p.mReactive, false, L"fsr_reactive");
    }
    d.dilatedDepth = context->mBackend.fpGetResource(&context->mBackend, context->mShared[0]);
    d.dilatedMotionVectors = context->mBackend.fpGetResource(&context->mBackend, context->mShared[1]);
    d.reconstructedPrevNearestDepth = context->mBackend.fpGetResource(&context->mBackend, context->mShared[2]);
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
    d.flags = 0;
    return fsr_ok(ffxFsr3UpscalerContextDispatch(&context->mContext, &d), "ffxFsr3UpscalerContextDispatch");
}

#endif // RK_FSR

}

#endif // RK_VULKAN
