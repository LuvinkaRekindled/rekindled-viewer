/**
 * @file llvkbridge.cpp
 * @brief Vulkan mirror mode (see llvkbridge.h)
 */

#include "linden_common.h"

#include "llvkbridge.h"

#if !defined(LL_VULKAN)

bool LLVkBridge::isReady() { return false; }
void LLVkBridge::setSymbolizer(std::string (*)(const void*)) {}
bool LLVkBridge::videoMemory(U64&, U64&) { return false; }
bool LLVkBridge::hostMemory(U64&, U64&) { return false; }
bool LLVkBridge::memoryByConsumer(MemoryByConsumer&) { return false; }
std::string LLVkBridge::accelCensus() { return std::string(); }
bool LLVkBridge::readbackAffordable() { return true; }
bool LLVkBridge::ensureReady() { return false; }
std::string LLVkBridge::startupFailureMessage() { return std::string(); }
void LLVkBridge::setStartupFailureHandler(void (*)()) {}
void LLVkBridge::setMainWindow(void*) {}
bool LLVkBridge::describeDevice(DeviceInfo&) { return false; }
bool LLVkBridge::inFrame() { return false; }
void LLVkBridge::beginFrame(S32, S32) {}
void LLVkBridge::endFrame() {}
void LLVkBridge::holdFrame(bool) {}
void LLVkBridge::setPresentPreference(S32, bool) {}
void LLVkBridge::snapshotBoundary(S32) {}
bool LLVkBridge::upscaleAvailable() { return false; }
S32 LLVkBridge::neuralRenderMode() { return 0; }
bool LLVkBridge::neuralRenderAvailable() { return false; }
void LLVkBridge::setUpscaleLodBias(float) {}
bool LLVkBridge::shadowPacketsAvailable() { return false; }
bool LLVkBridge::shadowAlphaPacketsAvailable() { return false; }
void LLVkBridge::pipelineBuilds(U64& pending, U64& total) { pending = 0; total = 0; }
bool LLVkBridge::sceneGeoAvailable() { return false; }
bool LLVkBridge::hizAvailable() { return false; }
U32 LLVkBridge::hizSend(const HizBox*, U32, const F32*, const F32*, U32) { return 0; }
S32 LLVkBridge::hizRead(U32, U32, U32*) { return -1; }
void LLVkBridge::hizDone(U32) {}
void LLVkBridge::hizSetResults(const U32*, U32) {}
void LLVkBridge::hizPublish(U32, U32) {}
U32 LLVkBridge::cullSend(const CullParams&, const U32*, const CullRow*, const CullMapRow*, U32, U32, bool) { return 0; }
void LLVkBridge::lodListPublish(U32, U32, const U32*) {}
U32 LLVkBridge::lodListRead(U32, U32&, U32*, U32) { return 0; }
void LLVkBridge::consumerListPublish(std::shared_ptr<const ConsumerList>) {}
std::shared_ptr<const LLVkBridge::ConsumerList> LLVkBridge::consumerListRead() { return nullptr; }
bool LLVkBridge::viewRequest(U32, U32, const CullParams&) { return false; }
std::shared_ptr<const LLVkBridge::ViewSet> LLVkBridge::viewSetRead(U32) { return nullptr; }
void LLVkBridge::viewSetPublish(std::shared_ptr<const ViewSet>) {}
void LLVkBridge::shadowPackets(const ShadowPacketsHeader&, const void*, size_t) {}
bool LLVkBridge::paletteArenaAcquire(U32, PaletteArena&) { return false; }
void LLVkBridge::paletteArenaRegister(U32, F32*, U64, U32) {}
void LLVkBridge::paletteArenaClear() {}
bool LLVkBridge::paletteArenaInfo(U32, PaletteArena&) { return false; }
void LLVkBridge::paletteArenaSent(U32, U64) {}
void LLVkBridge::paletteArenaFree(U64) {}
void LLVkBridge::upscale(const UpscaleArgs&) {}
U32 LLVkBridge::rtShadowMode() { return 0; }
bool LLVkBridge::rtLoadingGates() { return false; }
S32 LLVkBridge::rtDebugView() { return 0; }
S32 LLVkBridge::rtWaterReflections() { return 0; }
S32 LLVkBridge::rtHitQuality() { return -1; }
S32 LLVkBridge::rtHitRays() { return -1; }
S32 LLVkBridge::rtGlossyReflections() { return 0; }
bool LLVkBridge::rtMaterialsAvailable() { return false; }
bool LLVkBridge::rtBodyRename() { return false; }
U32 LLVkBridge::rtSkinPeriod(float) { return 1; }
void LLVkBridge::rtScene(const RtSceneArgs&) {}
void LLVkBridge::presentFrame() {}
long long LLVkBridge::lastPresentTicks() { return 0; }
void LLVkBridge::shutdown() {}
void LLVkBridge::setBindlessPersistent(bool) {}
void LLVkBridge::stopAsync() {}
void LLVkBridge::describeTextures(const std::vector<U32>&, const std::vector<std::string>&, const std::vector<U64>&, const std::vector<std::vector<U64>>&, bool) {}

#else // LL_VULKAN

#include "llrenderheaders.h"
#if LL_WINDOWS
#include <psapi.h>
#endif
#include "llvkcontext.h"
#include "llvknative.h"
#include "llvkupscale.h"
#include "llvkdenoise.h"
#include "llvkdlssnr.h"
#include "llvkrenderer.h"
#include "llvkasync.h"
#include "llvktex.h"
#include "llvkrt.h"
#include "llwindow.h"

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace LLVkWorker { extern bool gActive; extern LLVkBridge::Mutex gMutex; void after_frame(); void before_end_frame(); double take_hook_ms(); std::string resource_summary(); }

namespace LLVkBridge
{
long long gPhaseTicks[PH_COUNT] = {};
bool gPhaseTiming = false;
unsigned long gMainThreadId = 0;
volatile int gLastLap = 0;
volatile unsigned long long gDbg[4] = {};
volatile unsigned gAsyncEpoch = 0;
volatile unsigned long gRendererTids[8] = {};
volatile unsigned gRendererTidCount = 0;
void noteRendererThreadSlow()
{
    const unsigned i = InterlockedIncrement((volatile LONG*)&gRendererTidCount) - 1;
    if (i < 8) gRendererTids[i] = __readgsdword(0x48);
}
long long phaseNow()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
}

namespace
{

const char* const PHASE_NAMES[] = { "all hooks", "uniform hooks", "draw hooks", "draw: target", "draw: state", "draw: textures", "draw: uniform blocks", "draw: vertex input", "draw: renderer apply+record", "draw: debug stats",
                                     "apply: texture layouts", "apply: begin pass", "apply: pipeline", "apply: dynamic state", "apply: vertex input", "apply: sampler slots + ray scene set", "apply: uniforms", "apply: push textures", "apply: vkCmdDraw" };

void* sMainWindowHandle = nullptr;        // the viewer's main window (the window code hands it over once it exists)

struct MirrorState
{
    bool mReady = false;
    bool mFailed = false;
    std::string mFailureMessage;    // <FS> what the user was told when Vulkan could not start
    bool mInFrame = false;
    bool mHold = false;
    bool mOffscreen = false;        // the open frame has no swapchain image (work recorded after the present)
    bool mStandIn = false;          // the open off-screen frame stands in for a frame that could not be presented (the window is minimised or has no size): see begin_stand_in_frame
    U64 mStandInFrames = 0;

    HWND mWindow = nullptr;         // the viewer's main window: Vulkan presents into it
    volatile bool mResized = false;

    LLVk::Swapchain mSwapchain;
    LLVk::Image mBackbuffer;
    LLVk::Image mDepth;
    U32 mBackWidth = 0;
    U32 mBackHeight = 0;

    U32 mImageIndex = 0;
    LLVk::Image mTarget;            // wrapper of the acquired swapchain image for this frame
    U64 mFrames = 0;
    U64 mSkipped = 0;
    U64 mPresentFailed = 0;
};

MirrorState sMirror;

bool create_window()
{
    if (!sMainWindowHandle)
    {
        LL_WARNS("VkMirror") << "Vulkan cannot present: the window code has not handed over the main window" << LL_ENDL;
        return false;
    }
    sMirror.mWindow = (HWND)sMainWindowHandle;
    LLWindow::sSkipSwapBuffers = true;
    LL_INFOS("VkMirror") << "Vulkan presents into the viewer's main window" << LL_ENDL;
    return true;
}

void destroy_backbuffer()
{
    LLVk::Context& ctx = LLVk::Context::instance();
    ctx.destroyImage(sMirror.mBackbuffer);
    ctx.destroyImage(sMirror.mDepth);
    sMirror.mBackWidth = sMirror.mBackHeight = 0;
}

bool create_backbuffer(U32 width, U32 height)
{
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    destroy_backbuffer();

    LLVk::TextureDesc color;
    color.mWidth = width;
    color.mHeight = height;
    color.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    LLVk::TextureDesc depth;
    depth.mWidth = width;
    depth.mHeight = height;
    depth.mFormat = VK_FORMAT_D32_SFLOAT;
    depth.mUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!renderer.createTexture(color, sMirror.mBackbuffer) || !renderer.createTexture(depth, sMirror.mDepth))
    {
        LL_WARNS("VkMirror") << "could not create the mirror backbuffer " << width << "x" << height << LL_ENDL;
        return false;
    }
    sMirror.mBackWidth = width;
    sMirror.mBackHeight = height;
    ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)sMirror.mBackbuffer.mImage, "mirror backbuffer color");
    ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)sMirror.mDepth.mImage, "mirror backbuffer depth");
    LL_INFOS("VkMirror") << "mirror backbuffer " << width << "x" << height << LL_ENDL;
    return true;
}

} // anonymous namespace

void LLVkBridge::setPresentPreference(S32 mode, bool vsync_on)
{
    LLVk::setPresentPreference((int)mode, vsync_on);
}

LLVkBridge::Mutex& LLVkBridge::lock()
{
    return LLVkWorker::gMutex;
}

void LLVkBridge::setSymbolizer(std::string (*fn)(const void*))
{
    LLVkAsync::setSymbolizer(fn);
}

bool LLVkBridge::isReady()
{
    return sMirror.mReady;
}

bool LLVkBridge::readbackAffordable()
{
    return LLVkTex::readbackAffordable();
}

bool LLVkBridge::videoMemory(U64& usage_bytes, U64& budget_bytes)
{
    return sMirror.mReady && !sMirror.mFailed && LLVk::Context::instance().videoMemory(usage_bytes, budget_bytes);
}

bool LLVkBridge::hostMemory(U64& usage_bytes, U64& allocated_by_us)
{
    return sMirror.mReady && !sMirror.mFailed && LLVk::Context::instance().hostMemory(usage_bytes, allocated_by_us);
}

std::string LLVkBridge::accelCensus()
{
    return sMirror.mReady && !sMirror.mFailed ? LLVk::Context::instance().arenaAccelCensus() : std::string();
}

bool LLVkBridge::memoryByConsumer(MemoryByConsumer& out)
{
    if (!sMirror.mReady || sMirror.mFailed)
    {
        return false;
    }
    const LLVk::ArenaStats s = LLVk::Context::instance().arenaStats();
    out.mBlocks = s.mBlocks;
    out.mHostBlocks = s.mHostBlocks;
    out.mAccelBlocks = s.mAccelBlocks;
    out.mAccelPooled = s.mAccelPooled;
    out.mBlockBytes = s.mBlockBytes;
    out.mBlockUsed = s.mUsedBytes;
    out.mHostBytes = s.mHostBlockBytes;
    out.mHostUsed = s.mHostUsedBytes;
    out.mAccelBytes = s.mAccelBlockBytes;
    out.mAccelUsed = s.mAccelUsedBytes;
    out.mAccelPooledBytes = s.mAccelPooledBytes;
    out.mRtStructureBytes = LLVk::Rt::structureBytes();
    return true;
}

LLVk::Image* LLVkBridge::backbufferColor()
{
    return (sMirror.mReady && sMirror.mBackbuffer.valid()) ? &sMirror.mBackbuffer : nullptr;
}

LLVk::Image* LLVkBridge::backbufferDepth()
{
    return (sMirror.mReady && sMirror.mDepth.valid()) ? &sMirror.mDepth : nullptr;
}

bool LLVkBridge::inFrame()
{
    return sMirror.mReady && sMirror.mInFrame;
}

void LLVkBridge::setMainWindow(void* hwnd)
{
    sMainWindowHandle = hwnd;
}

bool LLVkBridge::describeDevice(DeviceInfo& out)
{
    if (!ensureReady())
    {
        return false;
    }
    const LLVk::Capabilities& caps = LLVk::Context::instance().getCaps();
    std::string& vendor = out.mVendor;
    std::string& renderer = out.mRenderer;
    std::string& version = out.mVersion;
    switch (caps.mVendorId)
    {
    case 0x10DE: vendor = "NVIDIA Corporation"; break;
    case 0x1002: vendor = "ATI Technologies Inc."; break;
    case 0x8086: vendor = "Intel"; break;
    default:     vendor = "Unknown vendor"; break;
    }
    renderer = caps.mDeviceName;
    if (caps.mVendorId == 0x10DE)
    {
        renderer += "/PCIe/SSE2";        // what the NVIDIA driver's GL_RENDERER said: the viewer compares this string with the one it saved to notice a GPU change
    }
    // the driver version is packed by the vendor; NVIDIA uses 10.8.8.6 bits
    const U32 d = caps.mDriverVersion;
    char buf[96];
    if (caps.mVendorId == 0x10DE)
    {
        snprintf(buf, sizeof(buf), "4.6.0 NVIDIA %u.%u (Vulkan %u.%u)", d >> 22, (d >> 14) & 0xFF, VK_API_VERSION_MAJOR(caps.mApiVersion), VK_API_VERSION_MINOR(caps.mApiVersion));
    }
    else
    {
        snprintf(buf, sizeof(buf), "4.6.0 Vulkan %u.%u", VK_API_VERSION_MAJOR(caps.mApiVersion), VK_API_VERSION_MINOR(caps.mApiVersion));
    }
    version = buf;
    out.mVramMb = (U32)(LLVk::Context::instance().deviceLocalBytes() / (1024 * 1024));

    // limits: what the GL queries used to be answered with (see emulate_limit in llvkworker.cpp, which reads the same capabilities)
    out.mMaxTextureSize = (S32)caps.mMaxImageDim2D;
    out.mTextureImageUnits = (S32)std::min<U32>(caps.mMaxSampledImagesPerStage, 32u);     // not the descriptor limit (a million): display() loops over every unit
    out.mMaxSamples = (S32)caps.mMaxSampleCount;
    out.mMaxColorTextureSamples = (S32)caps.mMaxColorTextureSamples;
    out.mMaxDepthTextureSamples = (S32)caps.mMaxDepthTextureSamples;
    out.mMaxIntegerSamples = (S32)caps.mMaxIntegerSamples;
    out.mMaxSampleMaskWords = (S32)caps.mMaxSampleMaskWords;
    out.mMaxVaryingVectors = (S32)caps.mMaxVaryingVectors;
    out.mMaxUniformBlockSize = (S32)caps.mMaxUniformRange;
    out.mMaxVertexRange = (S32)caps.mRecommendedMaxVertices;
    out.mMaxIndexRange = (S32)caps.mRecommendedMaxIndices;
    out.mMaxAnisotropy = caps.mMaxAnisotropy;
    out.mMemoryBandwidthGBps = caps.mMemoryBandwidthGBps;
    out.mAccelStruct = caps.mRt.mAccelStruct;
    out.mRayQuery = caps.mRt.mRayQuery;
    out.mRayTracingPipeline = caps.mRt.mPipeline;
    out.mRayTracingUsable = caps.mRt.mUsable;
    out.mRayTracingEnabled = caps.mRt.mEnabled;
    out.mNeuralRender = LLVkUpscale::ready() && LLVkDlssNr::available();
    return true;
}

static void (*sStartupFailureHandler)() = nullptr;

void LLVkBridge::setStartupFailureHandler(void (*handler)())
{
    sStartupFailureHandler = handler;
}

// <FS> what the user is told when Vulkan cannot start: the reasons the Vulkan context collected, and what Rekindled needs
static std::string startup_failure_text(const std::string& detail)
{
    return "Rekindled cannot start: the graphics of this computer could not be set up.\n\n" + detail + "\n"
        "Rekindled needs a graphics card and driver with Vulkan 1.3 and these features: dynamic rendering, descriptor indexing (bindless textures), multi-draw indirect and buffer device "
        "addresses. Graphics cards from about 2016 on usually have them (for example GeForce GTX 900 series and newer, Radeon RX 400 series and newer, Intel graphics of the 6th generation "
        "Core and newer), with an up to date graphics driver.\n\n"
        "If your card is that recent, please update the graphics driver and start again. The same details are in the log file.";
}

std::string LLVkBridge::startupFailureMessage()
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    return sMirror.mFailureMessage;
}

bool LLVkBridge::ensureReady()
{
    std::unique_lock<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    if (sMirror.mReady)
    {
        return true;
    }
    if (sMirror.mFailed)
    {
        return false;
    }

    LLVk::Context& ctx = LLVk::Context::instance();
    if (!create_window() || !ctx.init(sMirror.mWindow) || !LLVk::Renderer::instance().init()
        || !sMirror.mSwapchain.create(ctx.getSurface(), sMirror.mWindow))
    {
        LL_WARNS("VkMirror") << "Vulkan initialisation failed: there is no other renderer" << LL_ENDL;
        const std::string detail = ctx.failureReason();
        sMirror.mFailed = true;
        shutdown();
        sMirror.mFailed = true;
        if (sMirror.mFailureMessage.empty())
        {
            // <FS> the first failure: tell the user why (once; the app leaves through its normal failure path when it gets back to init(), see LLAppViewer::init)
            sMirror.mFailureMessage = startup_failure_text(detail.empty() ? std::string("The graphics device could not be set up (the details are in the log file).\n") : detail);
            LL_WARNS("VkMirror") << sMirror.mFailureMessage << LL_ENDL;
            const std::string message = sMirror.mFailureMessage;
            lock.unlock();
            static const bool no_box = getenv("LL_VK_NO_MESSAGE_BOX") != nullptr;      // (for tests that must not wait for a person)
            if (!no_box)
            {
                OSMessageBox(message, "Rekindled cannot start", OSMB_OK);
            }
            if (sStartupFailureHandler)
            {
                sStartupFailureHandler();       // (the app leaves: it does not return)
            }
        }
        return false;
    }
    sMirror.mReady = true;
    LLVkWorker::gActive = true;
    LL_INFOS("VkMirror") << "Vulkan mirror ready" << LL_ENDL;
    return true;
}

void LLVkBridge::describeTextures(const std::vector<U32>& names, const std::vector<std::string>& labels, const std::vector<U64>& finest, const std::vector<std::vector<U64>>& level_hashes, bool quiet)
{
    LLVkAsync::postCall([names, labels, finest, level_hashes, quiet]() { LLVkWorker::describe_textures(names, labels, finest, level_hashes, quiet); });
}

void LLVkBridge::stopAsync()
{
    LLVkAsync::stop();
    // Renderer::shutdown() is not reached on the normal exit path: stop the pipeline build threads and write the pipeline cache and the variant list here
    LLVk::Program::stopPipelineThreads();
    if (LLVk::Renderer::instance().isReady())
    {
        LLVk::Renderer::instance().savePipelineCache();
    }
}

void LLVkBridge::shutdown()
{
    LLVkAsync::stop();
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::gActive = false;
    LLVk::Context& ctx = LLVk::Context::instance();
    if (ctx.isReady())
    {
        ctx.waitIdle();
    }
    if (sMirror.mReady)
    {
        LL_INFOS("VkMirror") << "mirror shut down after " << sMirror.mFrames << " mirrored frames (" << sMirror.mSkipped << " skipped)" << LL_ENDL;
    }
    sMirror.mInFrame = false;
    sMirror.mReady = false;
    destroy_backbuffer();
    sMirror.mSwapchain.destroy();
    LLVk::Renderer::instance().shutdown();
    ctx.shutdown();
    if (sMirror.mWindow)
    {
        sMirror.mWindow = nullptr;
        LLWindow::sSkipSwapBuffers = false;
    }
}

namespace
{
bool begin_offscreen_frame();

// <FS> The window cannot be presented into (minimised, or it has no size): a frame is recorded all the same, without a swapchain image. Nearly everything the renderer gives back, it gives back
// when a frame begins (deferred destroys, the scratch rings and staging of the slot, retired handles, buffer writes that wait for a frame), and the viewer keeps sending work while it is minimised (the
// reflection probe update, texture discards). With no frame at all, a viewer left minimised for 85 minutes (2026-11-09) kept every buffer it gave up: all the video memory in 18 minutes, then 58 GB of
// system memory, 13 million scratch allocations refused, and 58 seconds to free it all at the restore. The frame is closed by the next endFrame / beginFrame like any off-screen frame.
void begin_stand_in_frame(const char* why)
{
    if (!sMirror.mBackbuffer.valid())
    {
        return;         // (nothing was ever drawn: there is nothing to record into, and nothing to give back yet)
    }
    if (begin_offscreen_frame())
    {
        sMirror.mStandIn = true;
        if (sMirror.mStandInFrames++ == 0)
        {
            LL_INFOS("VkMirror") << "no picture can be shown (" << why << "): frames are recorded without a present until one can" << LL_ENDL;
        }
    }
}
}

void LLVkBridge::beginFrameImpl(S32 width, S32 height)
{
    gMainThreadId = __readgsdword(0x48);
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    if (LLVk::gDeviceLost && !sMirror.mFailed)
    {
        // <FS> there is no OpenGL to fall back to: carrying on would leave a blank window. Stop with a message the crash report carries
        LL_ERRS("VkMirror") << "The graphics device was lost (VK_ERROR_DEVICE_LOST): the GPU or its driver stopped answering. The viewer cannot continue; please restart it. "
                                   "If this repeats, the log lines before this one name what was being drawn." << LL_ENDL;
    }
    if (sMirror.mInFrame)
    {
        if (sMirror.mHold)
        {
            return;     // the main loop holds the frame open across display() and what follows it
        }
        endFrameImpl();     // the previous frame never reached its swap (early return, snapshot): present what it drew
    }
    sMirror.mInFrame = false;
    if (!ensureReady())
    {
        return;
    }
    if (width <= 0 || height <= 0)
    {
        begin_stand_in_frame("the window has no size");
        return;
    }

    if (sMirror.mWindow)
    {
        // presenting into the main window: nobody tells the swapchain when it is resized
        U32 client_w = 0, client_h = 0;
        LLVk::windowClientSize(sMirror.mWindow, client_w, client_h);
        const LLVk::Swapchain& swap = sMirror.mSwapchain;
        if (swap.isValid() ? (client_w != swap.getExtent().width || client_h != swap.getExtent().height) : (client_w > 0 && client_h > 0))
        {
            sMirror.mResized = true;
        }
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    LLVk::Swapchain& sc = sMirror.mSwapchain;

    if (sMirror.mResized || sc.needsRecreate() || !sc.isValid())
    {
        sMirror.mResized = false;
        if (!sc.create(ctx.getSurface(), sMirror.mWindow))
        {
            shutdown();
            sMirror.mFailed = true;
            return;
        }
        if (!sc.isValid())
        {
            begin_stand_in_frame("the window is minimised");
            return;     // minimised
        }
    }
    if (sMirror.mStandInFrames)
    {
        LL_INFOS("VkMirror") << "a picture can be shown again after " << sMirror.mStandInFrames << " frames without a present" << LL_ENDL;
        sMirror.mStandInFrames = 0;
    }

    if ((U32)width != sMirror.mBackWidth || (U32)height != sMirror.mBackHeight)
    {
        ctx.waitIdle();
        if (!create_backbuffer((U32)width, (U32)height))
        {
            shutdown();
            sMirror.mFailed = true;
            return;
        }
    }

    // wait for the GPU when it is behind: a dropped frame would lose incremental work such as a reflection probe face
    VkCommandBuffer cmd = ctx.beginFrame(true);
    if (cmd == VK_NULL_HANDLE)
    {
        ++sMirror.mSkipped;
        return;
    }
    // While the mirror owns the resources a skipped frame leaves the window showing the old picture until the viewer happens to draw again,
    // so wait for an image (a swapchain image is normally free within a vsync) and recreate the swapchain if the surface changed under us.
    U32 image_index = 0;
    bool acquired = false;
    for (int attempt = 0; attempt < 3 && !acquired; ++attempt)
    {
        acquired = sc.acquire(ctx.getAcquireSemaphore(), image_index, 250ull * 1000 * 1000);
        if (!acquired)
        {
            if (!sc.needsRecreate() || !sc.create(ctx.getSurface(), sMirror.mWindow) || !sc.isValid())
            {
                break;
            }
        }
    }
    if (!acquired)
    {
        ctx.endFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        ++sMirror.mSkipped;
        return;
    }
    sMirror.mImageIndex = image_index;

    renderer.beginFrame(cmd, ctx.getFrameSlot());
    LLVk::Image* colors[1] = { &sMirror.mBackbuffer };
    renderer.setRenderTarget(colors, 1, &sMirror.mDepth);
    sMirror.mInFrame = true;
}

void LLVkBridge::holdFrameImpl(bool hold)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    sMirror.mHold = hold;
}

namespace
{
// opens a frame that has no swapchain image: the work the viewer does after the present (probe updates, snapshots) is recorded into it
bool begin_offscreen_frame()
{
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    ctx.setNextFrameOffscreen(true);
    VkCommandBuffer cmd = ctx.beginFrame(true);
    if (cmd == VK_NULL_HANDLE)
    {
        ++sMirror.mSkipped;
        ctx.setNextFrameOffscreen(false);
        return false;
    }
    renderer.beginFrame(cmd, ctx.getFrameSlot());
    LLVk::Image* colors[1] = { &sMirror.mBackbuffer };
    renderer.setRenderTarget(colors, 1, &sMirror.mDepth);
    sMirror.mInFrame = true;
    sMirror.mOffscreen = true;
    return true;
}
}

void LLVkBridge::Mutex::lockSlow(unsigned long me)
{
    for (U32 attempt = 0;; ++attempt)
    {
        unsigned long expected = 0;
        if (mOwner.compare_exchange_weak(expected, me, std::memory_order_acquire))
        {
            return;
        }
        if (attempt < 200) YieldProcessor();
        else if (attempt < 400) SwitchToThread();
        else if (attempt < 1000) Sleep(0);
        else Sleep(1);
    }
}

const char* LLVkBridge::phaseName(int id)
{
    return id >= 0 && id < PH_COUNT ? PHASE_NAMES[id] : "?";
}

void LLVkBridge::snapshotBoundaryImpl(S32 mode)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    if (mode <= 0 || !sMirror.mReady || !sMirror.mInFrame)
    {
        return;
    }
    LL_INFOS("VkSnap") << "snapshot boundary, variant " << mode << (sMirror.mOffscreen ? " (off-screen frame)" : " (present frame)") << LL_ENDL;
    LLVk::Context::instance().flight(LLVk::Context::FL_SNAPSHOT, (unsigned long long)mode, sMirror.mOffscreen ? 1ull : 0ull);
    if (mode == 1)
    {
        LLVk::Renderer::instance().flushAndRestart(4);
        return;
    }
    if (sMirror.mOffscreen)
    {
        const bool held = sMirror.mHold;
        sMirror.mHold = false;
        endFrameImpl();             // closes the off-screen frame
        if (mode >= 3)
        {
            LLVk::Context::instance().waitIdle();
        }
        sMirror.mHold = held;
        if (sMirror.mReady && !sMirror.mInFrame)
        {
            begin_offscreen_frame();
        }
    }
}

void LLVkBridge::presentFrameImpl()
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    if (!sMirror.mReady || !sMirror.mInFrame || sMirror.mOffscreen)
    {
        return;
    }
    const bool held = sMirror.mHold;
    sMirror.mHold = false;
    endFrameImpl();
    sMirror.mHold = held;
    if (!held || sMirror.mInFrame || !sMirror.mReady)
    {
        return;
    }

    // keep recording what the viewer does until the frame is released, without a swapchain image
    begin_offscreen_frame();
}

namespace
{

// The window changed size between drawing the frame and showing it, so the present was refused. Without this the old picture stays on
// screen until the viewer happens to draw again. Recreate the swapchain at the new size and show the finished backbuffer on it now.
bool represent_after_resize()
{
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Swapchain& sc = sMirror.mSwapchain;
    LLVk::Image& back = sMirror.mBackbuffer;
    if (!back.valid() || !sc.create(ctx.getSurface(), sMirror.mWindow) || !sc.isValid())
    {
        return false;
    }
    U32 index = 0;
    if (!sc.acquireBlocking(index))
    {
        return false;
    }
    const VkExtent2D extent = sc.getExtent();
    const VkImage target = sc.getImage(index);
    const U32 src_w = sMirror.mBackWidth, src_h = sMirror.mBackHeight;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        // the backbuffer was left in TRANSFER_SRC_OPTIMAL by the frame's own present blit
        LLVk::imageBarrier(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (S32)src_w, (S32)src_h, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0] = { 0, (S32)extent.height, 0 };      // the picture is stored bottom row first
        blit.dstOffsets[1] = { (S32)extent.width, 0, 1 };
        LLVk::vkCmdBlitImage(cmd, back.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        LLVk::imageBarrier(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    });
    return sc.presentNoWait(index);
}

}


// ---- <FS> blink capture (LL_VK_BLINK_CAPTURE=1, off by default) ----------------------------------------------------------------------------------------------------------------------------
// A diagnostic for a frame that is wrong for one frame (a patch of the ground black, a green flash): every finished frame is also blitted down to a small image and copied into a ring of host
// buffers, read back when the GPU has finished with it (no wait, so the timing is that of a normal run apart from two small copies). Each new frame is compared with the two before it: where a block
// is much darker (or much greener) in the middle frame and the frames on both sides agree with each other, the three frames and a difference picture are written as BMP files into
// <exe folder>\blink-captures (LL_VK_BLINK_DIR) and a VkBlink line goes to the log.
namespace
{
struct BlinkCapture
{
    static constexpr U32 RING = 8;
    static constexpr U32 CAP_W = 320;
    static constexpr U32 CELL = 8;
    struct Slot
    {
        LLVk::Image mImage;
        LLVk::Buffer mBuffer;
        U64 mFrame = 0;
        U64 mSeq = 0;       // the number of the capture (the frame numbers skip: the viewer closes an off-screen frame after most presented ones)
        bool mPending = false;
    };
    struct Done
    {
        U64 mFrame = 0;
        U64 mSeq = 0;
        std::vector<U8> mRGBA;
        std::vector<F32> mLuma;     // a mean a block
        std::vector<F32> mGreen;    // green minus the mean of red and blue, a mean a block
        std::vector<F32> mRgb;      // the mean colour of a block (3 floats)
    };
    bool mOn = false, mChecked = false;
    U32 mW = 0, mH = 0, mSrcW = 0, mSrcH = 0;
    Slot mSlots[RING];
    std::deque<Done> mHistory;
    U64 mCaptured = 0, mSkippedBusy = 0, mTriggers = 0, mSaved = 0, mLastSaveFrame = 0, mChecks = 0, mGap = 0;
    F32 mDrop = 0.25f;          // LL_VK_BLINK_DROP: how much darker (a share) a block must be for one frame
    U32 mBlocks = 3;            // LL_VK_BLINK_BLOCKS: how many blocks must do it
    U32 mMaxSaved = 300;        // LL_VK_BLINK_MAX: how many blinks are written out
    F32 mChange = 0.f;          // LL_VK_BLINK_CHANGE (0 = off): a block whose colour differs from the mean of the frames before and after by more than this (0 to 1, in any channel) for ONE frame, while those two agree, counts: a surface that shows another texture for a frame changes colour more than its brightness
    U32 mMaxChanged = 0;
    U32 mCapW = CAP_W;          // LL_VK_BLINK_WIDTH: the width of the copy (320 to 1280, a multiple of the block size): a flash of a small surface needs more than 320
    U32 mMaxDark = 0, mMaxGreen = 0, mMaxBright = 0;       // the strongest changes seen since the last log line (how far the thresholds are from what happens)
    std::string mDir;

    bool enabled()
    {
        if (!mChecked)
        {
            mChecked = true;
            const char* e = getenv("LL_VK_BLINK_CAPTURE");
            mOn = e && e[0] && e[0] != '0';
            if (mOn)
            {
                const char* d = getenv("LL_VK_BLINK_DIR");
                if (d && d[0])
                {
                    mDir = d;
                }
                else
                {
                    char path[MAX_PATH] = {};
                    GetModuleFileNameA(nullptr, path, MAX_PATH);
                    std::string exe = path;
                    const size_t slash = exe.find_last_of("\\/");
                    mDir = (slash == std::string::npos ? std::string() : exe.substr(0, slash)) + "\\blink-captures";
                }
                if (const char* v = getenv("LL_VK_BLINK_DROP")) mDrop = std::max(0.05f, std::min(0.9f, (F32)atof(v)));
                if (const char* v = getenv("LL_VK_BLINK_BLOCKS")) mBlocks = (U32)std::max(1, atoi(v));
                if (const char* v = getenv("LL_VK_BLINK_MAX")) mMaxSaved = (U32)std::max(1, atoi(v));
                if (const char* v = getenv("LL_VK_BLINK_CHANGE")) mChange = std::max(0.f, std::min(1.f, (F32)atof(v)));
                if (const char* v = getenv("LL_VK_BLINK_WIDTH")) mCapW = (U32)std::max<S32>(320, std::min<S32>(1280, atoi(v))) / CELL * CELL;
                CreateDirectoryA(mDir.c_str(), nullptr);
                LL_INFOS("VkBlink") << "blink capture is ON (LL_VK_BLINK_CAPTURE): small copies of every frame, bad frames saved to " << mDir << " (a block " << (S32)(mDrop * 100.f) << "% darker for one frame, " << mBlocks << " blocks)" << LL_ENDL;
            }
        }
        return mOn;
    }

    void release()
    {
        LLVk::Context& ctx = LLVk::Context::instance();
        for (Slot& s : mSlots)
        {
            if (s.mImage.valid()) ctx.destroyImage(s.mImage);
            if (s.mBuffer.valid()) ctx.destroyBuffer(s.mBuffer);
            s = Slot();
        }
        mHistory.clear();
    }

    bool setup(U32 src_w, U32 src_h)
    {
        if (mW && src_w == mSrcW && src_h == mSrcH)
        {
            return true;
        }
        release();
        LLVk::Context& ctx = LLVk::Context::instance();
        mSrcW = src_w;
        mSrcH = src_h;
        mW = mCapW;
        mH = std::max<U32>(CELL, ((U64)mCapW * src_h / std::max<U32>(1, src_w) + CELL / 2) / CELL * CELL);
        for (Slot& s : mSlots)
        {
            if (!ctx.createImage2D(mW, mH, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s.mImage)
                || !ctx.createBuffer((VkDeviceSize)mW * mH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, LLVk::Context::Memory::Readback, s.mBuffer))
            {
                LL_WARNS("VkBlink") << "blink capture: could not create the capture images; off" << LL_ENDL;
                release();
                mOn = false;
                return false;
            }
        }
        LL_INFOS("VkBlink") << "blink capture: frames of " << src_w << "x" << src_h << " are copied to " << mW << "x" << mH << " (" << mW / CELL << "x" << mH / CELL << " blocks)" << LL_ENDL;
        return true;
    }

    // BMP, 24 bit, rows bottom up (the capture is already flipped to top-down: write the rows reversed)
    void writeBmp(const std::string& path, const U8* rgba, U32 w, U32 h)
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f)
        {
            return;
        }
        const U32 row = (w * 3 + 3) / 4 * 4, size = row * h;
        U8 header[54] = { 'B', 'M' };
        auto put32 = [&](int at, U32 v) { for (int i = 0; i < 4; ++i) header[at + i] = (U8)(v >> (8 * i)); };
        put32(2, 54 + size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
        header[26] = 1; header[28] = 24; put32(34, size);
        fwrite(header, 1, 54, f);
        std::vector<U8> line(row, 0);
        for (U32 y = 0; y < h; ++y)
        {
            const U8* src = rgba + (size_t)(h - 1 - y) * w * 4;
            for (U32 x = 0; x < w; ++x)
            {
                line[x * 3 + 0] = src[x * 4 + 2];
                line[x * 3 + 1] = src[x * 4 + 1];
                line[x * 3 + 2] = src[x * 4 + 0];
            }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }

    void analyse(Done& d)
    {
        const U32 bw = mW / CELL, bh = mH / CELL;
        d.mLuma.assign((size_t)bw * bh, 0.f);
        d.mGreen.assign((size_t)bw * bh, 0.f);
        d.mRgb.assign((size_t)bw * bh * 3, 0.f);
        for (U32 by = 0; by < bh; ++by)
        {
            for (U32 bx = 0; bx < bw; ++bx)
            {
                F32 l = 0.f, g = 0.f, cr = 0.f, cg = 0.f, cb = 0.f;
                for (U32 y = 0; y < CELL; ++y)
                {
                    const U8* px = d.mRGBA.data() + ((size_t)(by * CELL + y) * mW + bx * CELL) * 4;
                    for (U32 x = 0; x < CELL; ++x, px += 4)
                    {
                        const F32 r = px[0] / 255.f, gr = px[1] / 255.f, b = px[2] / 255.f;
                        l += 0.2126f * r + 0.7152f * gr + 0.0722f * b;
                        cr += r; cg += gr; cb += b;
                        g += gr - 0.5f * (r + b);
                    }
                }
                d.mLuma[(size_t)by * bw + bx] = l / (CELL * CELL);
                d.mGreen[(size_t)by * bw + bx] = g / (CELL * CELL);
                d.mRgb[((size_t)by * bw + bx) * 3 + 0] = cr / (CELL * CELL);
                d.mRgb[((size_t)by * bw + bx) * 3 + 1] = cg / (CELL * CELL);
                d.mRgb[((size_t)by * bw + bx) * 3 + 2] = cb / (CELL * CELL);
            }
        }
    }

    // the oldest three in the history: a, b (the frame under test), c
    void check()
    {
        if (mHistory.size() < 3)
        {
            return;
        }
        const Done& a = mHistory[mHistory.size() - 3];
        const Done& b = mHistory[mHistory.size() - 2];
        const Done& c = mHistory[mHistory.size() - 1];
        if (b.mSeq != a.mSeq + 1 || c.mSeq != b.mSeq + 1)
        {
            ++mGap;
            return;
        }
        ++mChecks;
        U32 dark = 0, green = 0, bright = 0, changed = 0;
        const size_t n = b.mLuma.size();
        for (size_t k = 0; k < n; ++k)
        {
            const F32 la = a.mLuma[k], lb = b.mLuma[k], lc = c.mLuma[k];
            const F32 lo = std::min(la, lc), hi = std::max(la, lc);
            if (hi > 0.05f && hi - lo < 0.15f * hi && lb < (1.f - mDrop) * lo)
            {
                ++dark;
            }
            else if (hi > 0.05f && hi - lo < 0.15f * hi && lb > (1.f + 2.f * mDrop) * hi + 0.05f)
            {
                ++bright;
            }
            if (mChange > 0.f)
            {
                F32 worst = 0.f, apart = 0.f;
                for (int ch = 0; ch < 3; ++ch)
                {
                    const F32 xa = a.mRgb[k * 3 + ch], xb = b.mRgb[k * 3 + ch], xc = c.mRgb[k * 3 + ch];
                    worst = std::max(worst, std::fabs(xb - 0.5f * (xa + xc)));
                    apart = std::max(apart, std::fabs(xa - xc));
                }
                if (worst > mChange && apart < 0.08f)
                {
                    ++changed;
                }
            }
            const F32 ga = a.mGreen[k], gb = b.mGreen[k], gc = c.mGreen[k];
            if (gb - std::max(ga, gc) > 0.12f && std::fabs(ga - gc) < 0.08f)
            {
                ++green;
            }
        }
        mMaxDark = std::max(mMaxDark, dark);
        mMaxGreen = std::max(mMaxGreen, green);
        mMaxBright = std::max(mMaxBright, bright);
        mMaxChanged = std::max(mMaxChanged, changed);
        if (dark < mBlocks && green < 4 && bright < mBlocks && changed < mBlocks)
        {
            return;
        }
        ++mTriggers;
        if (mSaved >= mMaxSaved || b.mSeq < mLastSaveFrame + 6)
        {
            return;
        }
        mLastSaveFrame = b.mSeq;
        ++mSaved;
        char base[64];
        snprintf(base, sizeof(base), "blink-%03llu-frame%llu-d%u-b%u-g%u-c%u", (unsigned long long)mSaved, (unsigned long long)b.mFrame, dark, bright, green, changed);
        const std::string stem = mDir + "\\" + base;
        writeBmp(stem + "-a-before.bmp", a.mRGBA.data(), mW, mH);
        writeBmp(stem + "-b-bad.bmp", b.mRGBA.data(), mW, mH);
        writeBmp(stem + "-c-after.bmp", c.mRGBA.data(), mW, mH);
        std::vector<U8> diff(b.mRGBA.size());
        for (size_t i = 0; i < b.mRGBA.size(); i += 4)
        {
            for (int ch = 0; ch < 3; ++ch)
            {
                const int mean = ((int)a.mRGBA[i + ch] + (int)c.mRGBA[i + ch]) / 2;
                diff[i + ch] = (U8)std::min(255, std::abs((int)b.mRGBA[i + ch] - mean) * 4);
            }
            diff[i + 3] = 255;
        }
        writeBmp(stem + "-d-diff.bmp", diff.data(), mW, mH);
        LL_INFOS("VkBlink") << "blink " << mSaved << " at frame " << b.mFrame << ": " << dark << " blocks dark, " << green << " blocks green, " << bright << " blocks bright, " << changed << " blocks changed colour (of " << n << "), saved " << base << LL_ENDL;
    }

    // frames the GPU has finished are read, oldest first
    void collect()
    {
        LLVk::Context& ctx = LLVk::Context::instance();
        for (;;)
        {
            Slot* best = nullptr;
            for (Slot& s : mSlots)
            {
                if (s.mPending && ctx.frameFinished(s.mFrame) && (!best || s.mFrame < best->mFrame))
                {
                    best = &s;
                }
            }
            if (!best)
            {
                return;
            }
            Done d;
            d.mFrame = best->mFrame;
            d.mSeq = best->mSeq;
            d.mRGBA.assign((const U8*)best->mBuffer.mMapped, (const U8*)best->mBuffer.mMapped + (size_t)mW * mH * 4);
            best->mPending = false;
            analyse(d);
            mHistory.push_back(std::move(d));
            while (mHistory.size() > 4)
            {
                mHistory.pop_front();
            }
            check();
        }
    }

    // recorded after the blit to the swapchain: the backbuffer is in TRANSFER_SRC layout
    void record(LLVk::Context& ctx, LLVk::Renderer& renderer, LLVk::Image& backbuffer, U32 w, U32 h)
    {
        if (!setup(w, h))
        {
            return;
        }
        collect();
        Slot* slot = nullptr;
        for (U32 i = 0; i < RING && !slot; ++i)
        {
            Slot& s = mSlots[(mCaptured + i) % RING];
            if (!s.mPending)
            {
                slot = &s;
            }
        }
        if (!slot)
        {
            ++mSkippedBusy;
            return;
        }
        renderer.ensureLayout(slot->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { (S32)w, (S32)h, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0] = { 0, (S32)mH, 0 };      // (rows flipped like the blit to the window: the viewer's images have row 0 at the bottom)
        blit.dstOffsets[1] = { (S32)mW, 0, 1 };
        LLVk::vkCmdBlitImage(ctx.getCommandBuffer(), backbuffer.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->mImage.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        renderer.ensureLayout(slot->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { mW, mH, 1 };
        LLVk::vkCmdCopyImageToBuffer(ctx.getCommandBuffer(), slot->mImage.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->mBuffer.mBuffer, 1, &copy);
        slot->mFrame = ctx.getFrameNumber();
        slot->mSeq = mCaptured;
        slot->mPending = true;
        ++mCaptured;
        if (mCaptured % 3000 == 0)
        {
            LL_INFOS("VkBlink") << "blink capture: " << mCaptured << " frames copied, " << mSkippedBusy << " skipped (ring busy), " << mChecks << " checks (" << mGap << " skipped for a gap), the strongest block counts in the last 3000: dark " << mMaxDark << ", green " << mMaxGreen << ", bright " << mMaxBright << ", changed colour " << mMaxChanged << "; " << mTriggers << " blinks seen, " << mSaved << " saved" << LL_ENDL;
            mMaxDark = mMaxGreen = mMaxBright = mMaxChanged = 0;
        }
    }
};
BlinkCapture sBlink;
}

void LLVkBridge::endFrameImpl()
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    if (!sMirror.mReady || !sMirror.mInFrame || sMirror.mHold)
    {
        return;
    }
    sMirror.mInFrame = false;

    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    LLVk::Swapchain& sc = sMirror.mSwapchain;
    const U32 index = sMirror.mImageIndex;

    LLVkWorker::before_end_frame();
    if (sMirror.mOffscreen)
    {
        sMirror.mOffscreen = false;
        renderer.endFrame();
        ctx.endFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (sMirror.mStandIn)
        {
            // <FS> this frame took the place of a presented one: what a presented frame's end does for the worker's own upkeep runs here too (the readings and results of finished frames are taken, not left to pile up)
            sMirror.mStandIn = false;
            LLVkWorker::after_frame();
        }
        return;
    }

    LLVk::Image& target = sMirror.mTarget;
    target = LLVk::Image();
    target.mImage = sc.getImage(index);
    target.mView = sc.getView(index);
    target.mFormat = sc.getFormat();
    target.mAspect = VK_IMAGE_ASPECT_COLOR_BIT;
    target.mExtent = { sc.getExtent().width, sc.getExtent().height, 1 };
    target.mLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The viewer's images use GL conventions (row 0 = bottom). The window wants row 0 = top: flip while blitting.
    renderer.ensureLayout(sMirror.mBackbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    renderer.ensureLayout(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (S32)sMirror.mBackWidth, (S32)sMirror.mBackHeight, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, (S32)target.mExtent.height, 0 };
    blit.dstOffsets[1] = { (S32)target.mExtent.width, 0, 1 };
    LLVk::vkCmdBlitImage(ctx.getCommandBuffer(), sMirror.mBackbuffer.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.mImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    if (sBlink.enabled())
    {
        sBlink.record(ctx, renderer, sMirror.mBackbuffer, sMirror.mBackWidth, sMirror.mBackHeight);        // <FS> LL_VK_BLINK_CAPTURE
    }

    renderer.ensureLayout(target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    renderer.endFrame();
    ctx.endFrame(ctx.getAcquireSemaphore(), sc.getRenderDone(index));
    if (!sc.present(index, sc.getRenderDone(index)))
    {
        ++sMirror.mPresentFailed;
        if (sc.lastPresentOutOfDate())
        {
            ctx.waitIdle();
            represent_after_resize();
        }
    }
    ++sMirror.mFrames;
    LLVkWorker::after_frame();
}

// ---- the public frame calls: queued for the worker thread in async mode (LL_VK_ASYNC), inline otherwise ----
namespace
{
// The worker takes over between two frames: once a frame has been closed in the usual way and the mirror is ready.
void maybe_start_async()
{
    if (!LLVkAsync::gEnabled && LLVkAsync::requested() && sMirror.mReady && !sMirror.mInFrame && !sMirror.mHold)
    {
        LLVkAsync::start();
    }
}
}

void LLVkBridge::beginFrame(S32 width, S32 height)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameBegin(width, height);
        return;
    }
    beginFrameImpl(width, height);
}

void LLVkBridge::endFrame()
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameEnd();
        return;
    }
    endFrameImpl();
    maybe_start_async();
}

long long LLVkBridge::lastPresentTicks()
{
    return LLVkAsync::lastPresentTicks();
}

void LLVkBridge::presentFrame()
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::framePresent();
        return;
    }
    presentFrameImpl();
}

void LLVkBridge::holdFrame(bool hold)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameHold(hold);
        return;
    }
    holdFrameImpl(hold);
}

void LLVkBridge::snapshotBoundary(S32 mode)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::snapshotBoundary(mode);
        return;
    }
    snapshotBoundaryImpl(mode);
}

void LLVkBridge::setUpscaleLodBias(float bias)
{
    LLVkUpscale::setLodBias(bias);
}

bool LLVkBridge::upscaleAvailable()
{
    return sMirror.mReady && LLVkUpscale::ready();
}

bool LLVkBridge::denoiseAvailable()
{
    return sMirror.mReady && LLVkDenoise::ready();
}

bool LLVkBridge::upscalerIsFsr()
{
    return sMirror.mReady && LLVkUpscale::backendIsFsr();
}

void LLVkBridge::denoiseReflections(const DenoiseArgs& args)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::push(LLVkAsync::CMD_DENOISE, &args, sizeof(args));
        return;
    }
    denoiseImpl(args);
}

void LLVkBridge::denoiseImpl(const DenoiseArgs& args)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::on_denoise_reflections(args);
}

S32 LLVkBridge::neuralRenderMode()
{
    return LLVkDlssNr::requested();
}

bool LLVkBridge::neuralRenderAvailable()
{
    return sMirror.mReady && LLVkDlssNr::ready();
}

void LLVkBridge::upscale(const UpscaleArgs& args)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::push(LLVkAsync::CMD_UPSCALE, &args, sizeof(args));
        return;
    }
    upscaleImpl(args);
}

void LLVkBridge::upscaleImpl(const UpscaleArgs& args)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::on_upscale(args);
}

namespace LLVkWorker
{
    void on_shadow_packets(const LLVkBridge::ShadowPacketsHeader& header, const U8* blob);
    void on_rt_scene(const LLVkBridge::RtSceneArgs& args);
}

void LLVkBridge::pipelineBuilds(U64& pending, U64& total)
{
    const LLVk::SyncStats& s = LLVk::gSyncStats;
    const U64 queued = s.mPipePrewarmQueued.load(std::memory_order_relaxed) + s.mPipeMisses.load(std::memory_order_relaxed);
    const U64 done = s.mPipePrewarmDone.load(std::memory_order_relaxed) + s.mPipeBgDone.load(std::memory_order_relaxed);
    total = queued;
    pending = queued > done ? queued - done : 0;
}

U32 LLVkBridge::rtShadowMode()
{
    // known once the device exists (the shaders are compiled after that); the scene is built only when the device was created with ray queries
    const LLVk::Capabilities& caps = LLVk::Context::instance().getCaps();
    return sMirror.mReady && caps.mRt.mEnabled && caps.mRt.mUsable ? LLVk::Rt::shadowMode() : 0;
}

bool LLVkBridge::rtMaterialsAvailable()
{
    return rtShadowMode() != 0 && LLVk::Rt::materialsOn();
}

S32 LLVkBridge::rtGlossyReflections()
{
    return rtShadowMode() != 0 ? LLVk::Rt::glossyReflections() : 0;
}

S32 LLVkBridge::rtHitQuality()
{
    return rtShadowMode() != 0 ? LLVk::Rt::hitQuality() : -1;
}

S32 LLVkBridge::rtSnapTest()
{
    return rtShadowMode() != 0 ? LLVk::Rt::snapTest() : 0;
}

S32 LLVkBridge::rtSunSamples()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunSamples() : 0;
}

S32 LLVkBridge::rtTexAnim()
{
    return rtShadowMode() != 0 ? LLVk::Rt::texAnim() : -1;
}

S32 LLVkBridge::rtSunNoise()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunNoise() : 1;
}

S32 LLVkBridge::rtEarly()
{
    return rtShadowMode() != 0 ? LLVk::Rt::rtEarly() : -1;
}

S32 LLVkBridge::nativeKey(const char* key, S32 def)
{
    return LLVk::nativeKey(key, def);
}

void LLVkBridge::nativeGlobalDefines(const std::vector<std::pair<std::string, std::string>>& defines)
{
    LLVk::nativeSetGlobalDefines(defines);
}

void LLVkBridge::nativeRtSession(bool on)
{
    LLVk::nativeSetRtSession(on);
}

S32 LLVkBridge::rtSunFilter()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunFilter() : 0;
}

S32 LLVkBridge::rtSunPenMax()
{
    return LLVk::Rt::sunPenMax();
}

S32 LLVkBridge::rtSunPenLod()
{
    return LLVk::Rt::sunPenLod();
}

S32 LLVkBridge::rtSunStrength()
{
    return LLVk::Rt::sunStrength();
}

S32 LLVkBridge::rtSunSelfShadow()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunSelfShadow() : 1;
}

S32 LLVkBridge::rtSunPenRays()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunPenRays() : 0;
}

S32 LLVkBridge::rtHitRays()
{
    return rtShadowMode() != 0 ? LLVk::Rt::hitRays() : -1;
}

S32 LLVkBridge::rtWaterReflections()
{
    return rtShadowMode() != 0 ? LLVk::Rt::waterReflections() : 0;
}

S32 LLVkBridge::rtDebugView()
{
    return rtShadowMode() != 0 ? LLVk::Rt::debugView() : 0;
}

bool LLVkBridge::rtLoadingGates()
{
    return rtShadowMode() != 0 && LLVk::Rt::loadingGates();
}

bool LLVkBridge::rtBodyRename()
{
    return rtShadowMode() != 0 && LLVk::Rt::bodyRename();
}

U32 LLVkBridge::rtSkinPeriod(float distance)
{
    return rtShadowMode() != 0 ? LLVk::Rt::skinPeriod(distance) : 1u;
}

void LLVkBridge::rtScene(const RtSceneArgs& args)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::push(LLVkAsync::CMD_RT_SCENE, &args, sizeof(args));
        return;
    }
    rtSceneImpl(args);
}

void LLVkBridge::rtSceneImpl(const RtSceneArgs& args)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::on_rt_scene(args);
}

bool LLVkBridge::shadowPacketsAvailable()
{
    return sMirror.mReady && LLVk::Context::instance().getCaps().mBufferDeviceAddress;
}

bool LLVkBridge::shadowAlphaPacketsAvailable()
{
    return shadowPacketsAvailable() && LLVk::Context::instance().getCaps().mBindlessSlots != 0;
}

bool LLVkBridge::sceneGeoAvailable()
{
    const auto& caps = LLVk::Context::instance().getCaps();
    return shadowAlphaPacketsAvailable() && caps.mMultiDrawIndirect && caps.mDrawIndirectFirstInstance;
}

void LLVkBridge::shadowPackets(const ShadowPacketsHeader& header, const void* blob, size_t blob_bytes)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::pushData(LLVkAsync::CMD_SHADOW_PACKETS, &header, sizeof(header), blob, blob_bytes);
        return;
    }
    shadowPacketsImpl(header, (const U8*)blob);
}

// <FS> the palette arenas (see llvkbridge.h): the arena of a viewer frame is index frame % count; it is busy from the first acquire until the renderer frees it
namespace
{
constexpr U32 kPaletteArenasMax = 16;
struct PaletteArenaSlot
{
    std::atomic<F32*> mPtr{ nullptr };
    std::atomic<U64> mAddress{ 0 };
    std::atomic<U32> mFloats{ 0 };
    std::atomic<U32> mBusy{ 0 };            // 0 = free, else the viewer frame + 1 that holds it
    std::atomic<U64> mFreeAt{ 0 };          // the renderer frame from which it is free again; 0 = no record has named it yet
};
PaletteArenaSlot gPaletteArena[kPaletteArenasMax];
std::atomic<U32> gPaletteArenaCount{ 0 };
}

void LLVkBridge::paletteArenaRegister(U32 index, F32* ptr, U64 address, U32 floats)
{
    if (index >= kPaletteArenasMax || !ptr || !address)
    {
        return;
    }
    PaletteArenaSlot& s = gPaletteArena[index];
    s.mFloats.store(floats, std::memory_order_relaxed);
    s.mAddress.store(address, std::memory_order_relaxed);
    s.mBusy.store(0, std::memory_order_relaxed);
    s.mFreeAt.store(0, std::memory_order_relaxed);
    s.mPtr.store(ptr, std::memory_order_release);
    if (index + 1 > gPaletteArenaCount.load(std::memory_order_relaxed))
    {
        gPaletteArenaCount.store(index + 1, std::memory_order_release);
    }
}

void LLVkBridge::paletteArenaClear()
{
    gPaletteArenaCount.store(0, std::memory_order_release);
    for (PaletteArenaSlot& s : gPaletteArena)
    {
        s.mPtr.store(nullptr, std::memory_order_release);
    }
}

bool LLVkBridge::paletteArenaAcquire(U32 frame, PaletteArena& out)
{
    const U32 count = gPaletteArenaCount.load(std::memory_order_acquire);
    if (!count)
    {
        return false;
    }
    const U32 index = frame % count;
    PaletteArenaSlot& s = gPaletteArena[index];
    F32* ptr = s.mPtr.load(std::memory_order_acquire);
    if (!ptr)
    {
        return false;
    }
    const U32 want = frame + 1;
    U32 busy = s.mBusy.load(std::memory_order_acquire);
    if (busy != want)
    {
        if (busy != 0)
        {
            // taken by an earlier frame: free only once the renderer has freed it, or when that frame never sent a record (an aborted frame: nobody can read it) and this is the next use of the index
            if (s.mFreeAt.load(std::memory_order_acquire) != 0 || (U32)(want - busy) < count)
            {
                return false;
            }
            // GPU-LIFETIMES.md finding 5.7: taken over from a frame that never freed it. Right for an aborted frame (no record was sent); wrong when the worker is `count` viewer frames behind with
            // that frame's record still queued (the arena would be rewritten before it is played). Counted: it is expected only after an aborted frame (healthy = 0 in a lap).
            static std::atomic<U32> takeovers{ 0 };
            const U32 n = takeovers.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5 || (n & (n - 1)) == 0)        // (the first 5, then at every power of two: the last line says the order of the total)
            {
                LL_WARNS("PalArena") << "palette arena " << index << " taken over from viewer frame " << (busy - 1) << " (now " << frame << ", " << count << " arenas): " << n << " so far" << LL_ENDL;
            }
        }
        if (!s.mBusy.compare_exchange_strong(busy, want, std::memory_order_acq_rel))
        {
            return false;
        }
        s.mFreeAt.store(0, std::memory_order_release);
    }
    out.mPtr = ptr;
    out.mAddress = s.mAddress.load(std::memory_order_relaxed);
    out.mFloats = s.mFloats.load(std::memory_order_relaxed);
    out.mIndex = index;
    return true;
}

bool LLVkBridge::paletteArenaInfo(U32 index, PaletteArena& out)
{
    if (index >= kPaletteArenasMax)
    {
        return false;
    }
    PaletteArenaSlot& s = gPaletteArena[index];
    out.mPtr = s.mPtr.load(std::memory_order_acquire);
    out.mAddress = s.mAddress.load(std::memory_order_relaxed);
    out.mFloats = s.mFloats.load(std::memory_order_relaxed);
    out.mIndex = index;
    return out.mPtr && out.mAddress;
}

void LLVkBridge::paletteArenaSent(U32 index, U64 free_at_renderer_frame)
{
    if (index >= kPaletteArenasMax)
    {
        return;
    }
    PaletteArenaSlot& s = gPaletteArena[index];
    U64 cur = s.mFreeAt.load(std::memory_order_relaxed);
    while (cur < free_at_renderer_frame && !s.mFreeAt.compare_exchange_weak(cur, free_at_renderer_frame, std::memory_order_acq_rel))
    {
    }
}

void LLVkBridge::paletteArenaFree(U64 renderer_frame)
{
    const U32 count = gPaletteArenaCount.load(std::memory_order_acquire);
    for (U32 i = 0; i < count; ++i)
    {
        PaletteArenaSlot& s = gPaletteArena[i];
        const U64 at = s.mFreeAt.load(std::memory_order_acquire);
        if (at != 0 && renderer_frame >= at)
        {
            s.mFreeAt.store(0, std::memory_order_relaxed);
            s.mBusy.store(0, std::memory_order_release);
        }
    }
}

// S7 H1: the Hi-Z ring (the worker writes the results, the main thread reads them once published)
namespace
{
std::atomic<U32> gHizPublished[8];
std::atomic<const U32*> gHizResults{ nullptr };
std::atomic<U32> gHizCapacity{ 0 };
std::atomic<U32> gHizDone{ 0 };
U32 gHizSerial = 0;         // main thread only
}

bool LLVkBridge::hizAvailable()
{
    return LLVkBridge::sceneGeoAvailable();
}

U32 LLVkBridge::hizSend(const HizBox* boxes, U32 count, const F32* view_proj)
{
    if (!count || count > 32768 || !hizAvailable())
    {
        return 0;
    }
    const U32 serial = gHizSerial + 1;
    if (serial - gHizDone.load(std::memory_order_acquire) > 6)
    {
        return 0;       // the ring would wrap onto a serial that has not been read
    }
    gHizSerial = serial;
    ShadowPacketsHeader header;
    header.mKind = 34;
    header.mEntries = count;
    header.mPass = serial % 8;
    header.mFragment = serial;
    memcpy(header.mViewProj, view_proj, sizeof(header.mViewProj));
    shadowPackets(header, boxes, (size_t)count * sizeof(HizBox));
    return serial;
}

S32 LLVkBridge::hizRead(U32 serial, U32 count, U32* out)
{
    const U32* results = gHizResults.load(std::memory_order_acquire);
    const U32 capacity = gHizCapacity.load(std::memory_order_relaxed);
    if (!results || !capacity || count > capacity)
    {
        return 0;
    }
    // three words a box: result bits, nearest depth of the box, largest depth of the pyramid under it
    const U32 slot = serial % 8;
    const U32 published = gHizPublished[slot].load(std::memory_order_acquire);
    if (published == serial)
    {
        memcpy(out, results + (size_t)slot * capacity * 3, (size_t)count * 12);
        std::atomic_thread_fence(std::memory_order_acquire);
        return gHizPublished[slot].load(std::memory_order_relaxed) == serial ? 1 : -1;
    }
    return published > serial ? -1 : 0;
}

void LLVkBridge::hizDone(U32 serial)
{
    U32 cur = gHizDone.load(std::memory_order_relaxed);
    while (serial > cur && !gHizDone.compare_exchange_weak(cur, serial, std::memory_order_release))
    {
    }
}

void LLVkBridge::hizSetResults(const U32* results, U32 capacity)
{
    gHizCapacity.store(capacity, std::memory_order_relaxed);
    gHizResults.store(results, std::memory_order_release);
}

void LLVkBridge::hizPublish(U32 slot, U32 serial)
{
    gHizPublished[slot % 8].store(serial, std::memory_order_release);
}

namespace
{
U32 gCullSerial = 0;        // main thread only
}

U32 LLVkBridge::cullSend(const CullParams& params, const U32* slots, const CullRow* rows, const CullMapRow* map_rows, U32 count, U32 table_size, bool reset)
{
    if (!table_size || !hizAvailable() || count > table_size)
    {
        return 0;
    }
    const U32 serial = ++gCullSerial;
    ShadowPacketsHeader header;
    header.mKind = 35;
    header.mEntries = count;
    header.mMatrices = table_size;
    header.mPaletteFloats = reset ? 1u : 0u;
    header.mPass = serial % 8;
    header.mFragment = serial;
    const size_t slot_bytes = ((size_t)count * 4 + 15) & ~(size_t)15;
    std::vector<U8> blob(sizeof(CullParams) + slot_bytes + (size_t)count * sizeof(CullRow) + (size_t)count * sizeof(CullMapRow), 0);
    memcpy(blob.data(), &params, sizeof(CullParams));
    if (count)
    {
        memcpy(blob.data() + sizeof(CullParams), slots, (size_t)count * 4);
        memcpy(blob.data() + sizeof(CullParams) + slot_bytes, rows, (size_t)count * sizeof(CullRow));
        memcpy(blob.data() + sizeof(CullParams) + slot_bytes + (size_t)count * sizeof(CullRow), map_rows, (size_t)count * sizeof(CullMapRow));
    }
    shadowPackets(header, blob.data(), blob.size());
    return serial;
}

namespace
{
std::mutex gLodMutex;
std::vector<U32> gLodChange;
U32 gLodFrame = 0, gLodTable = 0;
}

void LLVkBridge::lodListPublish(U32 frame, U32 table_size, const U32* change_bits)
{
    std::lock_guard<std::mutex> lock(gLodMutex);
    gLodChange.assign(change_bits, change_bits + (table_size + 31) / 32);
    gLodFrame = frame;
    gLodTable = table_size;
}

U32 LLVkBridge::lodListRead(U32 newer_than, U32& frame, U32* change_bits, U32 capacity_rows)
{
    std::lock_guard<std::mutex> lock(gLodMutex);
    if (!gLodTable || gLodFrame <= newer_than || gLodTable > capacity_rows)
    {
        return 0;
    }
    if (change_bits)
    {
        memcpy(change_bits, gLodChange.data(), gLodChange.size() * 4);
    }
    frame = gLodFrame;
    return gLodTable;
}

namespace
{
std::mutex gConsMutex;
std::shared_ptr<const LLVkBridge::ConsumerList> gCons;
}

void LLVkBridge::consumerListPublish(std::shared_ptr<const ConsumerList> list)
{
    std::lock_guard<std::mutex> lock(gConsMutex);
    gCons = std::move(list);
}

std::shared_ptr<const LLVkBridge::ConsumerList> LLVkBridge::consumerListRead()
{
    std::lock_guard<std::mutex> lock(gConsMutex);
    return gCons;
}

namespace
{
std::mutex gViewSetMutex;
std::unordered_map<U32, std::shared_ptr<const LLVkBridge::ViewSet>> gViewSets;       // the newest answer for each id (a few dozen ids at most: the probe faces)
}

bool LLVkBridge::viewRequest(U32 id, U32 frame, const CullParams& params)
{
    if (!id || !hizAvailable())
    {
        return false;
    }
    ShadowPacketsHeader header;
    header.mKind = 43;
    header.mFragment = id;
    header.mPass = frame;
    shadowPackets(header, &params, sizeof(params));
    return true;
}

std::shared_ptr<const LLVkBridge::ViewSet> LLVkBridge::viewSetRead(U32 id)
{
    std::lock_guard<std::mutex> lock(gViewSetMutex);
    const auto it = gViewSets.find(id);
    return it == gViewSets.end() ? nullptr : it->second;
}

void LLVkBridge::viewSetPublish(std::shared_ptr<const ViewSet> set)
{
    std::lock_guard<std::mutex> lock(gViewSetMutex);
    if (gViewSets.size() > 256)
    {
        gViewSets.clear();      // (ids that are no longer asked for: the probes come and go)
    }
    gViewSets[set->mId] = std::move(set);
}

void LLVkBridge::shadowPacketsImpl(const ShadowPacketsHeader& header, const U8* blob)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::on_shadow_packets(header, blob);
}

#endif // LL_VULKAN
