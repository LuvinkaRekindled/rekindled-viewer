/**
 * @file llvkbridge.cpp
 * @brief Vulkan mirror mode (see llvkbridge.h)
 */

#include "linden_common.h"

#include "llvkbridge.h"

#if !defined(LL_VULKAN)

bool LLVkBridge::isReady() { return false; }
bool LLVkBridge::videoMemory(U64&, U64&) { return false; }
bool LLVkBridge::memoryByConsumer(MemoryByConsumer&) { return false; }
std::string LLVkBridge::accelCensus() { return std::string(); }
bool LLVkBridge::readbackAffordable() { return true; }
bool LLVkBridge::ensureReady() { return false; }
void LLVkBridge::setMainWindow(void*) {}
bool LLVkBridge::describeDevice(DeviceInfo&) { return false; }
bool LLVkBridge::inFrame() { return false; }
void LLVkBridge::beginFrame(S32, S32) {}
void LLVkBridge::endFrame() {}
void LLVkBridge::holdFrame(bool) {}
void LLVkBridge::setPresentPreference(S32, bool) {}
void LLVkBridge::snapshotBoundary(S32) {}
bool LLVkBridge::upscaleAvailable() { return false; }
bool LLVkBridge::rayReconstructionAvailable() { return false; }
S32 LLVkBridge::neuralRenderMode() { return 0; }
bool LLVkBridge::neuralRenderAvailable() { return false; }
void LLVkBridge::setUpscaleLodBias(float) {}
bool LLVkBridge::shadowPacketsAvailable() { return false; }
bool LLVkBridge::shadowAlphaPacketsAvailable() { return false; }
bool LLVkBridge::sceneGeoAvailable() { return false; }
bool LLVkBridge::hizAvailable() { return false; }
U32 LLVkBridge::hizSend(const HizBox*, U32, const F32*, const F32*, U32) { return 0; }
S32 LLVkBridge::hizRead(U32, U32, U32*, bool) { return -1; }
void LLVkBridge::hizDone(U32) {}
void LLVkBridge::hizSetResults(const U32*, U32) {}
void LLVkBridge::hizPublish(U32, U32) {}
bool LLVkBridge::textureNeed(U32, TextureNeed&) { return false; }
void LLVkBridge::textureNeedStats(U64& t, U64& r) { t = r = 0; }
void LLVkBridge::textureNeedReading(U32, F32) {}
void LLVkBridge::textureNeedForget(U32) {}
U32 LLVkBridge::cullSend(const CullParams&, const U32*, const CullRow*, U32, U32, bool) { return 0; }
S32 LLVkBridge::cullRead(U32, U32, U32*) { return -1; }
void LLVkBridge::cullDone(U32) {}
void LLVkBridge::cullSetResults(const U32*, U32) {}
void LLVkBridge::cullPublish(U32, U32) {}
void LLVkBridge::shadowPackets(const ShadowPacketsHeader&, const void*, size_t) {}
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

#else // LL_VULKAN

#include "llrenderheaders.h"
#include <psapi.h>
#include "llvkcontext.h"
#include "llvkupscale.h"
#include "llvkdlssnr.h"
#include "llvkrenderer.h"
#include "llvkasync.h"
#include "llvktex.h"
#include "llvkrt.h"
#include "llwindow.h"

#include <cstdlib>
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
    bool mInFrame = false;
    bool mHold = false;
    bool mOffscreen = false;        // the open frame has no swapchain image (work recorded after the present)

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
    out.mRayReconstruction = LLVkUpscale::rayReconstructionAvailable();
    out.mNeuralRender = LLVkUpscale::ready() && LLVkDlssNr::available();
    return true;
}

bool LLVkBridge::ensureReady()
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
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
        sMirror.mFailed = true;
        shutdown();
        sMirror.mFailed = true;
        return false;
    }
    sMirror.mReady = true;
    LLVkWorker::gActive = true;
    LL_INFOS("VkMirror") << "Vulkan mirror ready" << LL_ENDL;
    return true;
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
        return;
    }

    if (sMirror.mWindow)
    {
        // presenting into the main window: nobody tells the swapchain when it is resized
        RECT rc = {};
        GetClientRect(sMirror.mWindow, &rc);
        const LLVk::Swapchain& swap = sMirror.mSwapchain;
        if (swap.isValid() ? ((U32)rc.right != swap.getExtent().width || (U32)rc.bottom != swap.getExtent().height) : (rc.right > 0 && rc.bottom > 0))
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
            return;     // minimised
        }
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

bool LLVkBridge::rayReconstructionAvailable()
{
    return sMirror.mReady && LLVkUpscale::rayReconstructionAvailable();
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

S32 LLVkBridge::rtCullUse()
{
    return rtShadowMode() != 0 ? LLVk::Rt::cullUse() : -1;
}

S32 LLVkBridge::rtSunSamples()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunSamples() : 0;
}

S32 LLVkBridge::rtTexAnim()
{
    return rtShadowMode() != 0 ? LLVk::Rt::texAnim() : -1;
}

S32 LLVkBridge::rtSunSelfShadow()
{
    return rtShadowMode() != 0 ? LLVk::Rt::sunSelfShadow() : 1;
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

U32 LLVkBridge::hizSend(const HizBox* boxes, U32 count, const F32* view_proj, const F32* prev_view_proj, U32 census_first)
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
    if (prev_view_proj && census_first < count)
    {
        header.mMatrices = 1;                   // 64 bytes after the boxes: the previous view-projection
        header.mPaletteFloats = census_first;
        std::vector<U8> blob((size_t)count * sizeof(HizBox) + 64);
        memcpy(blob.data(), boxes, (size_t)count * sizeof(HizBox));
        memcpy(blob.data() + (size_t)count * sizeof(HizBox), prev_view_proj, 64);
        shadowPackets(header, blob.data(), blob.size());
        return serial;
    }
    shadowPackets(header, boxes, (size_t)count * sizeof(HizBox));
    return serial;
}

S32 LLVkBridge::hizRead(U32 serial, U32 count, U32* out, bool census_half)
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
        memcpy(out, results + ((size_t)slot + (census_half ? 8 : 0)) * capacity * 3, (size_t)count * 12);
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

// H6: the measured need of each texture, by id. The worker adds the readings of the frames the GPU has finished; the main thread reads when it looks at a texture (a few hundred a frame): a short lock.
namespace
{
struct NeedState
{
    F32 mCur = 3.0e38f, mPrev = 3.0e38f;        // the smallest area of the current window and of the one before it
    double mWindowStart = 0.0;
    double mLastSeen = 0.0;
};
std::mutex gNeedMutex;
std::unordered_map<U32, NeedState> gNeed;
U64 gNeedReadings = 0;
constexpr double kNeedWindow = 0.5;             // seconds
double need_now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

void LLVkBridge::textureNeedReading(U32 name, F32 area)
{
    const double now = need_now();
    std::lock_guard<std::mutex> lock(gNeedMutex);
    NeedState& s = gNeed[name];
    if (s.mWindowStart == 0.0)
    {
        s.mWindowStart = now;
    }
    else if (now - s.mWindowStart >= kNeedWindow)
    {
        // a new window; the one that closed is kept as the previous (two windows old data is dropped: a texture that was drawn in the closed window only keeps that one)
        s.mPrev = now - s.mWindowStart < 2.0 * kNeedWindow ? s.mCur : 3.0e38f;
        s.mCur = 3.0e38f;
        s.mWindowStart = now;
    }
    s.mCur = std::min(s.mCur, area);
    s.mLastSeen = now;
    ++gNeedReadings;
}

void LLVkBridge::textureNeedForget(U32 name)
{
    std::lock_guard<std::mutex> lock(gNeedMutex);
    gNeed.erase(name);
}

bool LLVkBridge::textureNeed(U32 name, TextureNeed& out)
{
    const double now = need_now();
    std::lock_guard<std::mutex> lock(gNeedMutex);
    auto it = gNeed.find(name);
    if (it == gNeed.end())
    {
        return false;
    }
    const NeedState& s = it->second;
    const F32 known = std::min(s.mCur, s.mPrev);
    out.mKnown = known;
    out.mAge = (F32)(now - s.mLastSeen);
    // the window values count only while they are recent: seen within the last two windows
    out.mWindow = out.mAge < 2.0 * kNeedWindow ? known : 3.0e38f;
    return true;
}

void LLVkBridge::textureNeedStats(U64& textures, U64& readings)
{
    std::lock_guard<std::mutex> lock(gNeedMutex);
    textures = gNeed.size();
    readings = gNeedReadings;
}

// S7 C1: the cull shadow ring, like the Hi-Z one
namespace
{
std::atomic<U32> gCullPublished[8];
std::atomic<const U32*> gCullResults{ nullptr };
std::atomic<U32> gCullCapacity{ 0 };
std::atomic<U32> gCullDone{ 0 };
U32 gCullSerial = 0;        // main thread only
}

U32 LLVkBridge::cullSend(const CullParams& params, const U32* slots, const CullRow* rows, U32 count, U32 table_size, bool reset)
{
    if (!table_size || !hizAvailable() || count > table_size)
    {
        return 0;
    }
    const U32 capacity = gCullCapacity.load(std::memory_order_relaxed);
    if (capacity && (size_t)cullVariants * ((table_size + 31) / 32) > capacity)
    {
        return 0;       // more rows than a slot holds
    }
    const U32 serial = gCullSerial + 1;
    if (serial - gCullDone.load(std::memory_order_acquire) > 6)
    {
        return 0;       // the ring would wrap onto a serial that has not been read
    }
    gCullSerial = serial;
    ShadowPacketsHeader header;
    header.mKind = 35;
    header.mEntries = count;
    header.mMatrices = table_size;
    header.mPaletteFloats = reset ? 1u : 0u;
    header.mPass = serial % 8;
    header.mFragment = serial;
    const size_t slot_bytes = ((size_t)count * 4 + 15) & ~(size_t)15;
    std::vector<U8> blob(sizeof(CullParams) + slot_bytes + (size_t)count * sizeof(CullRow), 0);
    memcpy(blob.data(), &params, sizeof(CullParams));
    if (count)
    {
        memcpy(blob.data() + sizeof(CullParams), slots, (size_t)count * 4);
        memcpy(blob.data() + sizeof(CullParams) + slot_bytes, rows, (size_t)count * sizeof(CullRow));
    }
    shadowPackets(header, blob.data(), blob.size());
    return serial;
}

S32 LLVkBridge::cullRead(U32 serial, U32 table_size, U32* out)
{
    const U32* results = gCullResults.load(std::memory_order_acquire);
    const U32 capacity = gCullCapacity.load(std::memory_order_relaxed);
    const size_t words = (size_t)cullVariants * ((table_size + 31) / 32);
    if (!results || !capacity || words > capacity)
    {
        return 0;
    }
    const U32 slot = serial % 8;
    const U32 published = gCullPublished[slot].load(std::memory_order_acquire);
    if (published == serial)
    {
        memcpy(out, results + (size_t)slot * capacity, words * 4);
        std::atomic_thread_fence(std::memory_order_acquire);
        return gCullPublished[slot].load(std::memory_order_relaxed) == serial ? 1 : -1;
    }
    return published > serial ? -1 : 0;
}

void LLVkBridge::cullDone(U32 serial)
{
    U32 cur = gCullDone.load(std::memory_order_relaxed);
    while (serial > cur && !gCullDone.compare_exchange_weak(cur, serial, std::memory_order_release))
    {
    }
}

void LLVkBridge::cullSetResults(const U32* results, U32 capacity_words)
{
    gCullCapacity.store(capacity_words, std::memory_order_relaxed);
    gCullResults.store(results, std::memory_order_release);
}

void LLVkBridge::cullPublish(U32 slot, U32 serial)
{
    gCullPublished[slot % 8].store(serial, std::memory_order_release);
}

void LLVkBridge::shadowPacketsImpl(const ShadowPacketsHeader& header, const U8* blob)
{
    std::lock_guard<LLVkBridge::Mutex> lock(LLVkWorker::gMutex);
    LLVkWorker::on_shadow_packets(header, blob);
}

#endif // LL_VULKAN
