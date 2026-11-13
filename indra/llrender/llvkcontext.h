/**
 * @file llvkcontext.h
 * @brief Vulkan device layer for the experimental Vulkan backend: loader, instance, device, memory
 *        allocator, per-frame command recording, deferred destruction, staging memory and a swapchain.
 *
 * Compiled only with -DUSE_VULKAN=ON (LL_VULKAN defined). Requires Vulkan 1.3 plus dynamic rendering,
 * synchronization2, extended dynamic state 3 (polygon mode and blend state), vertex input dynamic state and
 * push descriptors, so pipelines only need to be keyed by shader program and attachment formats.
 */

#ifndef LL_LLVKCONTEXT_H
#define LL_LLVKCONTEXT_H

#if defined(LL_VULKAN)

#include "linden_common.h"

#include <atomic>
#include <deque>
#include <map>
#include <string>
#include <functional>
#include <mutex>
#include <vector>

#include "llvkplatform.h"

#define VK_NO_PROTOTYPES
#if LL_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

// the same VMA configuration must be used by every translation unit (llvkvma.cpp holds the implementation)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "thirdparty/vk_mem_alloc.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace LLVk
{

// the size in pixels of the window the renderer presents into (Windows: the client rectangle, Linux: the SDL drawable size)
bool windowClientSize(HWND window, U32& width, U32& height);

// name of the program of the draw being recorded, for validation messages
extern const char* gDebugProgram;

// set when the device reports VK_ERROR_DEVICE_LOST; the mirror then switches itself off
extern bool gDeviceLost;

// Vulkan entry points (loaded by Context::init)
#define LL_VK_GLOBAL_FN(name) extern PFN_##name name;
#define LL_VK_INSTANCE_FN(name) extern PFN_##name name;
#define LL_VK_DEVICE_FN(name) extern PFN_##name name;
#include "llvkfunctions.inl"
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

struct Capabilities
{
    char mDeviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    U32 mApiVersion = 0;
    U32 mDriverVersion = 0;
    U32 mVendorId = 0;
    bool mDiscrete = false;
    bool mMemoryBudget = false;     // VK_EXT_memory_budget available
    bool mCheckpoints = false;      // VK_NV_device_diagnostic_checkpoints available (enabled on the device when it is)
    bool mDeviceFault = false;      // VK_EXT_device_fault available (enabled on the device when it is): after a device loss the faulting address
    bool mWideLines = false;
    bool mDepthClamp = false;       // dynamic depth clamp (the shadow passes need it)
    bool mAnisotropy = false;
    bool mBC = false;               // BC1-7 texture compression
    bool mGeometryShader = false;
    bool mMultiDrawIndirect = false;
    bool mBufferDeviceAddress = false;  // shaders can read buffers by address (render packets)
    bool mDrawIndirectCount = false;            // vkCmdDraw(Indexed)IndirectCount: the draw count is read from a buffer (a GPU cull appends the visible draws and the pass draws only those)
    bool mDrawIndirectFirstInstance = false;    // indirect draws may use a first instance (render packets: the draw number)
    U32 mBindlessSmall = 2048;      // slots of each of the cube, cube array and shadow tables
    bool mBindlessUpdateUnused = false;     // descriptorBindingUpdateUnusedWhilePending: entries of the bindless tables may be written while earlier work that does not use them is pending
    U32 mBindlessSlots = 0;         // sampled images a shader can index at run time (set 2, render packets with textures); 0 = the device cannot
    F32 mMaxAnisotropy = 1.f;
    F32 mTimestampPeriodNs = 1.f;   // nanoseconds per timestamp tick
    U32 mTimestampValidBits = 0;
    VkDeviceSize mUniformAlignment = 256;
    U32 mMaxPushDescriptors = 0;
    // device limits that answer the viewer's GL_MAX_* questions
    U32 mMaxImageDim2D = 0, mMaxImageDimCube = 0, mMaxImageDim3D = 0, mMaxArrayLayers = 0;
    U32 mMaxColorAttachments = 0, mMaxVertexAttribs = 0, mMaxUniformRange = 0, mMaxSampledImagesPerStage = 0;
    U32 mMaxViewport[2] = {};
    F32 mLineWidthRange[2] = { 1.f, 1.f };
    U32 mMaxSampleCount = 1;        // largest framebuffer colour sample count
    // C6: what the viewer's GL-shaped limit queries ask for that Vulkan answers differently (values a GL driver of this class reports; see Context::pickDevice)
    U32 mMaxColorTextureSamples = 1, mMaxDepthTextureSamples = 1, mMaxIntegerSamples = 1;   // sampled multisample images
    U32 mMaxSampleMaskWords = 1;
    U32 mMaxVaryingVectors = 15;
    U32 mRecommendedMaxVertices = 0, mRecommendedMaxIndices = 0;    // GL_MAX_ELEMENTS_*: Vulkan has no such hint (only a hard index limit), see pickDevice
    F32 mMemoryBandwidthGBps = 0.f; // measured device-local copy throughput (read + write), 0 = not measured
    // C7: what the device offers beyond the one queue the renderer uses (family 0, queue 0). Nothing is created on these yet; see PLAN-cleanup-before-rt.md, C7.
    bool mTimelineSemaphore = false;        // Vulkan 1.2 core feature, on the device already (every supported feature of the chain is enabled)
    U32 mGraphicsQueues = 0;                // queues in the renderer's family (graphics + compute + transfer)
    S32 mComputeFamily = -1;                // a family that can compute but not draw: the async compute engine (-1 = none)
    U32 mComputeQueues = 0;
    S32 mTransferFamily = -1;               // a family that can only copy (-1 = none)
    U32 mTransferQueues = 0;

    // C6: ray tracing. mExt* = the device offers the extension, the rest = the device says the feature works. Nothing here is switched on at device creation
    // unless ray tracing was asked for (LL_VK_RT=1): mEnabled tells whether the extensions and features are on the device that was created.
    struct RayTracing
    {
        bool mExtAccel = false, mExtDeferredHost = false, mExtRayQuery = false, mExtPipeline = false, mExtPositionFetch = false, mExtMaintenance1 = false;
        bool mExtReorder = false, mExtOpacityMicromap = false, mExtCluster = false;     // vendor / later: reported, never enabled so far
        bool mAccelStruct = false;          // VK_KHR_acceleration_structure: BLAS / TLAS builds on the GPU
        bool mRayQuery = false;             // inline ray queries from any shader stage (the first ray tracing step needs only this and the above)
        bool mPipeline = false;             // ray tracing pipelines (raygen / hit / miss)
        bool mPositionFetch = false;        // triangle vertex positions from a hit without reading the vertex buffer
        bool mMaintenance1 = false;
        bool mReorder = false;              // VK_NV_ray_tracing_invocation_reorder (shader execution reordering)
        bool mOpacityMicromap = false;
        bool mClusterAccel = false;         // VK_NV_cluster_acceleration_structure
        bool mUsable = false;               // accel structure + ray query + device addresses: ray query shadows / reflections can be built on this device
        bool mEnabled = false;
        U64 mMaxGeometryCount = 0, mMaxInstanceCount = 0, mMaxPrimitiveCount = 0;
        U32 mScratchAlignment = 0, mMaxRecursionDepth = 0;
    } mRt;
};

struct Buffer
{
    VkBuffer mBuffer = VK_NULL_HANDLE;
    VmaAllocation mAllocation = nullptr;
    void* mMapped = nullptr;        // persistently mapped for host-visible buffers
    VkDeviceSize mSize = 0;
    VkDeviceAddress mAddress = 0;   // the address a shader reads the buffer by (0 when the device cannot)
    bool valid() const { return mBuffer != VK_NULL_HANDLE; }
};

// A range of one of the arena's large buffers (PLAN-stable-vertex-buffers.md). The range stays where it is until it is freed, so its address can be handed to
// shaders and acceleration structure builds; it is addressed by buffer + offset in draws.
struct ArenaRef
{
    VkBuffer mBuffer = VK_NULL_HANDLE;      // the block's buffer (shared by many ranges)
    VkDeviceSize mOffset = 0;               // start of the range in that buffer
    VkDeviceSize mSize = 0;
    VkDeviceAddress mAddress = 0;           // device address of the first byte of the range (0 when the device cannot)
    void* mMapped = nullptr;                // the CPU's pointer to the first byte (Host kind only)
    VmaVirtualAllocation mAlloc = VK_NULL_HANDLE;
    U32 mBlock = ~0u;
    bool valid() const { return mBuffer != VK_NULL_HANDLE; }
};

// Device: video memory, written by GPU copies. Shared: the same, CONCURRENT between the graphics and the async compute family (a second queue later; nothing asks yet).
// Host: CPU-written memory the GPU reads (video memory behind resizable BAR when there is room), mapped, written with memcpy; for the many small buffers.
// Accel: device blocks whose buffers can hold acceleration structures (the storage of BLAS / TLAS, ranges aligned to 256 bytes); not counted in the vertex/index statistics.
enum class ArenaKind { Device, Shared, Host, Accel };

struct ArenaStats
{
    U32 mBlocks = 0, mAllocs = 0;
    U32 mHostBlocks = 0, mHostAllocs = 0;
    U64 mHostBlockBytes = 0, mHostUsedBytes = 0;
    U64 mBlockBytes = 0, mUsedBytes = 0, mLargestFree = 0;
    U64 mBlocksCreated = 0, mBlocksReleased = 0;
    // <FS> the acceleration structure storage (ArenaKind::Accel) on its own (S7 measurement): ranges asked for by size class (below 1 MB, 8, 32, 64, 128, more), the blocks made and given back (shared and dedicated),
    // microseconds spent making them (vmaCreateBuffer), how long the dedicated ones lived, and what the blocks that are alive hold now
    U64 mAccelAllocs = 0, mAccelAllocBytes = 0, mAccelBucket[6] = {};
    U64 mAccelBlocksCreated = 0, mAccelBlocksReleased = 0, mAccelDedicatedCreated = 0, mAccelDedicatedReleased = 0, mAccelCreatedBytes = 0, mAccelCreateMicros = 0, mAccelDedicatedLifeFrames = 0;
    U32 mAccelBlocks = 0, mAccelDedicatedAlive = 0;
    U64 mAccelBlockBytes = 0, mAccelUsedBytes = 0, mAccelDedicatedBytes = 0, mAccelLargestFree = 0;
    // <FS> the pool of dedicated acceleration structure blocks that went empty and wait for the next big range (instead of giving 1 GB of video memory a second back to the system and asking for it again)
    U64 mAccelPoolHits = 0, mAccelPoolMisses = 0, mAccelPoolTrimmed = 0;
    U64 mIdleTrimmed = 0;       // empty regular blocks given back after sitting idle (arenaTrimPool)
    U32 mAccelPooled = 0;
    U64 mAccelPooledBytes = 0;
};

struct Image
{
    VkImage mImage = VK_NULL_HANDLE;
    VkImageView mView = VK_NULL_HANDLE;             // 2D view of the whole image (depth formats: depth aspect only)
    VmaAllocation mAllocation = nullptr;            // null for images owned elsewhere (swapchain)
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags mAspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkExtent3D mExtent = { 0, 0, 0 };
    U32 mMipLevels = 1;
    U32 mLayers = 1;
    VkImageViewType mViewType = VK_IMAGE_VIEW_TYPE_2D;
    Image* mOwner = nullptr;            // set on single-layer / single-mip views of another image: layouts live there
    U32 mBaseMip = 0;                   // for such views: which mip level and layer of the owner they show
    U32 mBaseLayer = 0;
    VkImageLayout mLayout = VK_IMAGE_LAYOUT_UNDEFINED;     // tracked by the renderer
    // GL textures that are allocated without data (render targets) read as zero in practice; Vulkan images start undefined.
    // Set on such images: the renderer clears them the first time they leave the undefined layout.
    bool mZeroOnFirstUse = false;
    // <FS> the identity of mView for the bindless table: a Vulkan may hand a destroyed view's handle to the next view, so the table is keyed by this number, never reused. Zero until the
    // table first needs it; whoever replaces mView sets it back to zero.
    U64 mViewId = 0;
    bool valid() const { return mImage != VK_NULL_HANDLE; }
};
inline U64 nextViewId() { static std::atomic<U64> sNext{1}; return sNext.fetch_add(1); }

// the aspect an image of this format is viewed and transitioned with
VkImageAspectFlags aspectForFormat(VkFormat format);

struct StagingAlloc
{
    VkBuffer mBuffer = VK_NULL_HANDLE;
    VkDeviceSize mOffset = 0;
    void* mPtr = nullptr;
    bool valid() const { return mBuffer != VK_NULL_HANDLE; }
};

class Context
{
public:
    // The most frame slots there can be (array bounds). How many are used: frameCount(), 3 unless LL_VK_FRAMES says otherwise (2..6). Every viewer frame uses two
    // slots (the frame and the off-screen frame after its present), so with 3 the thread that records them can only be about 1.5 viewer frames ahead of the GPU.
    static const U32 FRAMES_IN_FLIGHT = 6;
    static U32 frameCount()
    {
        static const U32 count = []() {
            char buf[16] = {};
            const U32 n = GetEnvironmentVariableA("LL_VK_FRAMES", buf, sizeof(buf)) > 0 ? (U32)atoi(buf) : 3;
            return n < 2 ? 2u : (n > FRAMES_IN_FLIGHT ? FRAMES_IN_FLIGHT : n);
        }();
        return count;
    }

    static Context& instance();

    // Creates the instance, picks a device that can present to the window, creates the device, allocator
    // and frame ring, and runs a self test. Returns false (and logs why) if this machine is not suitable.
    bool init(HWND present_window);
    // <FS> why init() returned false, in words for the user (the devices that were found and what each lacks, or the missing Vulkan runtime); empty when it did not fail
    const std::string& failureReason() const { return mFailureReason; }
    void shutdown();
    bool isReady() const { return mReady; }

    // ---- LL_VK_PHASES: GPU time by render pass ----
    // A timestamp is written when each render pass begins; the time up to the next one is charged to that pass's tag. Read back when the frame slot is reused.
    static const U32 MAX_GPU_STAMPS = 4096;
    // A stamp tag with this bit is a DRAW stamp (LL_VK_DRAW_STAMPS, small passes such as avatar impostors): written after the draw it describes, it does not change which pass the time belongs to.
    // When a frame hangs, the first draw stamp that never completed names the draw the GPU is stuck in.
    static const U32 STAMP_DRAW = 0x80000000u;
    static U32 stampTag(const std::string& name);       // the number standing for a pass description (0 is "start of frame")
    static U32 drawStampTag(const std::string& text);   // the same for a draw description, with STAMP_DRAW set (looked up by hash; a full table gives a generic tag)
    static std::string stampTagName(U32 tag);
    void gpuStamp(U32 tag);
    void (*mStampHook)(U32 tag) = nullptr;      // LL_VK_FRAGSTATS: called for every non-draw stamp (the renderer's statistics query changes its tag with the time table's)
    void drawCheckpoint(U32 tag);
    void checkpointOnly(U32 tag);       // an NV checkpoint named by a stamp tag, without a timestamp (work that is not a pass: uploads, copies)
    void serialCheckpoint(U64 serial);  // a checkpoint named by a draw's serial number (bit 62 set): see Renderer::noteDraw
    void setDrawDescriber(std::function<std::string(U64)> fn) { mDrawDescriber = std::move(fn); }
    std::function<std::string(U64)> mDrawDescriber;
    // images destroyed lately (view id, frame number): the bindless audit at a device loss names the slots that still point at one of them
    void noteDestroyedView(U64 view_id);
    // <FS> device lost diagnostics: what an image is (size, format, levels, who made it), keyed by its VkImage handle, so the fault address history can say what a recycled memory range held
    void noteImageTag(U64 image_handle, const char* text);
    // <FS> the guard of the device lost fix (2026-11-09): called with the view id of an image just before its view and memory are freed, so that a bindless handle that still shows that view is pointed at the placeholder
    // instead of at memory that is about to go (set by the renderer; the call runs on the worker thread with the other frame-begin destroys)
    void setViewReleaseHook(std::function<void(U64)> hook) { mViewReleaseHook = std::move(hook); }
    U64 destroyedViewCount() const { return mDestroyedCount.load(); }                                  // views destroyed so far (a sequence number)
    U64 destroyedViewIdAt(U64 seq) const { return mDestroyedIds[seq % DESTROYED_RING]; }                // the view destroyed as number seq (valid for the last DESTROYED_RING)
    static constexpr size_t destroyedRing() { return DESTROYED_RING; }
    bool destroyedViewFrame(U64 view_id, U64& frame) const;
    void setBindlessAudit(std::function<std::string()> fn) { mBindlessAudit = std::move(fn); }
    // S4: after a device lost, what an address that is in no arena block belongs to (scratch rings, overflow chunks, the scene tables): each describer returns "" when the address is not its own
    void addAddressDescriber(std::function<std::string(unsigned long long)> fn) { mAddressDescribers.push_back(std::move(fn)); }
    std::vector<std::function<std::string(unsigned long long)>> mAddressDescribers;
    // S4: the dedicated (non arena) buffers with a device address, alive and recently destroyed: after a device lost, a fault address in one of them says which (size, usage, memory, created and destroyed frame)
    struct DedicatedBuffer { unsigned long long mAddress, mSize; unsigned mUsage, mMemory; unsigned long long mCreated, mDestroyed; bool mAlive; };
    std::mutex mDedicatedMutex;
    std::function<void(U64)> mViewReleaseHook;      // called when an image is really destroyed: no handle may show its view any more
    std::mutex mDeferredNextMutex;
    std::vector<std::function<void()>> mDeferredNext;      // given up while no frame is being recorded: goes with the next frame (see destroyLater)
    std::vector<DedicatedBuffer> mDedicated;
    std::string describeDedicated(unsigned long long at);

    // S4: the flight recorder. A fixed ring of the last events of the GPU side (arena ranges, buffers, writes, flushes, the scene table's prepare / draw, palette segments, frames, snapshot
    // boundaries, ray scene notes), written with a few stores (no lock, no allocation), dumped to the log when a frame is stuck or the device is lost.
    enum FlightKind : unsigned
    {
        FL_ARENA_ALLOC = 1, FL_ARENA_FREE_REQ, FL_ARENA_FREE_DONE, FL_BUF_CREATE, FL_BUF_DESTROY, FL_BUF_WRITE, FL_BUF_RENAME, FL_FLUSH, FL_GEO_PREPARE, FL_GEO_CMDS, FL_GEO_DRAW,
        FL_GEO_RETAIN, FL_GEO_RELEASE, FL_GEO_BUF, FL_PAL_SEG, FL_FRAME_BEGIN, FL_FRAME_SUBMIT, FL_SNAPSHOT, FL_RT_NOTE, FL_RT_SCENE, FL_MISC, FL_KINDS
    };
    struct FlightEvent { unsigned long long mFrame; unsigned mKind, mThread; unsigned long long a, b, c, d; };
    static constexpr size_t FLIGHT_EVENTS = 16384;
    FlightEvent mFlight[FLIGHT_EVENTS] = {};
    std::atomic<unsigned long long> mFlightNext{ 0 };
    void flight(unsigned kind, unsigned long long a = 0, unsigned long long b = 0, unsigned long long c = 0, unsigned long long d = 0)
    {
        const unsigned long long i = mFlightNext.fetch_add(1, std::memory_order_relaxed);
        FlightEvent& e = mFlight[i & (FLIGHT_EVENTS - 1)];
        e.mFrame = (unsigned long long)mFrameNumber;
        e.mKind = kind;
        e.mThread = flightThread();
        e.a = a; e.b = b; e.c = c; e.d = d;
    }
    static unsigned flightThread();
    void dumpFlight(unsigned count, const char* why);
    std::string describeBinding(U64 handle, bool buffer = false) const;      // LL_VK_ADDR_LOG: where the memory of an image (VkImage handle) is
    void setSlotDescriber(std::function<std::string(U32)> fn) { mSlotDescriber = std::move(fn); }
    std::function<std::string(U32)> mSlotDescriber;
    std::function<std::string()> mBindlessAudit;
    static constexpr size_t DESTROYED_RING = 8192;
    U64 mDestroyedIds[DESTROYED_RING] = {};
    U64 mDestroyedFrames[DESTROYED_RING] = {};
    std::atomic<U64> mDestroyedCount{ 0 };
    void reportCheckpoints(const char* when);       // asks the driver for the checkpoints and logs them (also at the watchdog mark: the driver may answer before it resets the device)       // a checkpoint (no timestamp) set BEFORE a draw: after a device loss the latest one that was started names the draw the GPU was in
    void reportDeviceLost();            // logs the last checkpoints (pass names) the GPU reached, when the driver keeps them
    // called (once per hang) when a frame has been stuck for a second and when the device is lost: whatever a subsystem keeps to explain its latest work (the ray scene's last scene calls)
    void setHangReporter(std::function<void()> fn) { mHangReporter = std::move(fn); }
    std::function<void()> mHangReporter;
    struct GpuPassStats
    {
        std::map<U32, U64> mMicros;                     // by tag
        U64 mMainMicros = 0, mOffMicros = 0, mMainFrames = 0, mOffFrames = 0, mMainMax = 0, mOffMax = 0;
    };
    void takeGpuPassStats(GpuPassStats& out) { out = mPassStats; mPassStats = GpuPassStats(); }
    void setNextFrameOffscreen(bool offscreen) { mNextOffscreen = offscreen; }

    VkInstance getInstance() const { return mInstance; }
    VkPhysicalDevice getPhysicalDevice() const { return mPhysical; }
    VkDevice getDevice() const { return mDevice; }
    VkQueue getQueue() const { return mQueue; }
    U32 getQueueFamily() const { return mQueueFamily; }
    VkSurfaceKHR getSurface() const { return mSurface; }
    VmaAllocator getAllocator() const { return mAllocator; }
    const Capabilities& getCaps() const { return mCaps; }

    // --- frames -------------------------------------------------------------------------------------
    // Waits until the frame slot's previous use has finished on the GPU, runs its deferred destroys, resets
    // its command pool and begins recording. Returns the frame's primary command buffer.
    // wait=false: returns VK_NULL_HANDLE immediately if the GPU has not finished with this slot yet
    VkCommandBuffer beginFrame(bool wait = true);
    // Ends recording and submits. wait_semaphore (optional) is waited at the colour-attachment-output stage.
    void endFrame(VkSemaphore wait_semaphore, VkSemaphore signal_semaphore);
    VkCommandBuffer getCommandBuffer() const { return mFrames[mFrameSlot].mCmd; }
    VkSemaphore getAcquireSemaphore() const { return mFrames[mFrameSlot].mAcquire; }
    U64 getFrameNumber() const { return mFrameNumber; }
    // true once the GPU has finished the frame that was the n-th to be submitted (false while it is still being recorded). Right in a frame and between two frames (see mCompletedBelow). The render worker's thread.
    bool frameFinished(U64 n);
    U64 earlyFinishedAvoided() const { return mEarlyFinishedAvoided; }      // times frameFinished was asked between two frames about a frame the old test called finished while it was still on the GPU
    U32 getFrameSlot() const { return mFrameSlot; }
    // GPU microseconds the "rt scene builds" pass took in the frame that last used this slot (read from the pass timestamps when the slot was reused; 0 when it had none): the ray scene paces its builds by it
    U64 rtBuildMicros(U32 slot) const { return slot < FRAMES_IN_FLIGHT ? mRtBuildMicros[slot].load(std::memory_order_relaxed) : 0; }
    bool inFrame() const { return mInFrame; }

    // Upload memory that lives until this frame's GPU work completes
    bool isStagingBuffer(VkBuffer buffer) const;        // a block of some frame slot's staging (a copy that names anything else as its source reads memory that is gone)
    StagingAlloc allocStaging(VkDeviceSize size, VkDeviceSize alignment = 16);

    // Run f after the GPU has finished every frame submitted so far (safe place to destroy resources)
    void destroyLater(std::function<void()> f);

    // --- resources ----------------------------------------------------------------------------------
    // Upload and Readback buffers are persistently mapped (mMapped): Upload is write-combined host memory for
    // the CPU to fill, Readback is cached host memory for the CPU to read. Device buffers are device-local.
    enum class Memory { Device, Upload, UploadDevice, Readback };   // UploadDevice: CPU-written, GPU-read buffers (device-local host-visible memory when the GPU has it)
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory, Buffer& out);
    void destroyBuffer(Buffer& buffer);     // deferred
    // --- buffer arena: vertex / index / acceleration-structure input data, suballocated from a few large device-local buffers ----
    // The range never moves. arenaFree is deferred like destroyBuffer (the range is reusable once the GPU has finished every frame submitted so far).
    bool arenaAlloc(VkDeviceSize size, VkDeviceSize alignment, ArenaKind kind, ArenaRef& out);
    void arenaFree(ArenaRef& ref);
    ArenaStats arenaStats();
    std::string arenaAccelCensus();         // text: how full the acceleration structure blocks are and what draining the sparsest ones would take (measurement only)
    bool createImage2D(U32 width, U32 height, U32 mips, U32 layers, VkFormat format, VkImageUsageFlags usage, Image& out);
    void destroyImage(Image& image);        // deferred
    void setViewForgetHook(void (*fn)(VkImageView)) { mViewForget = fn; }       // called with every sampling view given up (the bindless table: its slots naming the view get the stand-in)
    void forgetView(VkImageView v) { if (mViewForget && v) mViewForget(v); }

    void waitIdle();
    // "heap 0 (device) 3200/15000 MB; heap 1 (host) ..." : usage and budget of every memory heap, for the benchmark log
    std::string memoryBudgetText();
    // video memory as the OS sees this process: what it uses and what it may use before the driver starts moving allocations to system memory (VK_EXT_memory_budget); the largest device-local heap
    bool videoMemory(U64& usage_bytes, U64& budget_bytes);
    // size of the device-local memory heaps in bytes (the GPU's video memory)
    U64 deviceLocalBytes() const;
    size_t pendingDestroys() const;      // resources queued for destruction that no frame has released yet
    // names an object for the validation layer messages (only when validation is on)
    void setObjectName(VkObjectType type, U64 handle, const std::string& name);

    // Records into a one-off command buffer, submits it and waits. For start-up work and rare uploads only.
    bool immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
    void arenaPrepareTick();    // once a frame on the render worker: finished fills make their blocks ready, and a spare host block is made and filled ahead of need (LL_VK_HOST_BLOCK_GATE)

    // Submits what the current frame has recorded so far, waits for the GPU, and carries on recording into the same
    // frame (same slot, same command buffer, swapchain semaphores untouched). Lets a readback see the frame's results.
    bool flushFrameAndWait();

    // <FS> E2 (doc/vulkan/PLAN-async-compute.md, LL_VK_RT_EARLY=3): the ray scene work on the compute queue. A semaphore wait holds a whole submission, so the frame's graphics commands are cut into
    // command buffers at the points where the queues meet. asyncOn() is true when the device has the compute queue and the timeline semaphore and the switch asked for it.
    bool asyncOn() const { return mAsyncOn; }
    // true while the frame being recorded has had a part submitted already (splitFrame: the ray scene built on the compute queue): that part is on the GPU and reads the frame slot's handle map
    bool frameSplit() const { return mAsyncOn && mInFrame && mFrames[mFrameSlot].mCmdIndex > 0; }
    // ends the current command buffer, submits it (signal: it also signals the next timeline value, which is returned in value, else 0) and carries on recording into the next one of the frame; false on failure
    bool splitFrame(bool signal, U64& value);
    VkCommandBuffer beginCompute();                                     // the slot's compute command buffer, begun
    bool submitCompute(VkCommandBuffer cmd, U64 wait_value, U64& signal_value);
    void setNextSubmitWait(U64 value);                                  // the next graphics submit of the frame waits for this timeline value before it starts
    void suppressStamps(bool on) { mStampsOff = on; }                  // GPU stamps write into the graphics command buffer: off while the compute one is recorded

private:
    Context() = default;

    struct Frame
    {
        VkCommandPool mPool = VK_NULL_HANDLE;
        VkCommandBuffer mCmd = VK_NULL_HANDLE;          // the one being recorded (with E2 one of mCmds)
        VkCommandBuffer mCmds[3] = {};                  // E2: the frame's command buffers, used in turn at the splits (mCmds[0] is the first mCmd)
        U32 mCmdIndex = 0;
        VkCommandPool mComputePool = VK_NULL_HANDLE;    // E2
        VkCommandBuffer mComputeCmd = VK_NULL_HANDLE;
        U64 mWaitValue = 0;                             // E2: the next graphics submit waits for this value of mAsyncSem
        U64 mComputeDone = 0;                           // E2: the value the frame's compute work signals (the last submit waits for it when the join was not reached)
        VkFence mFence = VK_NULL_HANDLE;
        VkSemaphore mAcquire = VK_NULL_HANDLE;
        bool mSubmitted = false;
        VkQueryPool mTsPool = VK_NULL_HANDLE;   // timestamps: 0 the start and 1 the end of this frame's command buffer, then one per render pass (LL_VK_PHASES)
        bool mTsPending = false;                // the end timestamp was written and has not been read yet
        bool mOffscreen = false;                // the off-screen frame the mirror opens after the present (not the frame that is presented)
        std::vector<U32> mStampTags;            // what each per-pass timestamp stands for (see Context::stampTag)
        std::vector<std::function<void()>> mDeferred;
        std::vector<std::function<void()>> mDeferredLate;      // what mDeferred held one round of this slot ago: destroyed one more round later (a copy flushed into a later frame can still read what was given up)
        std::vector<Buffer> mStagingBlocks;     // blocks used this frame (the first is kept for reuse)
        VkDeviceSize mStagingUsed = 0;
    };

    bool loadLoader();
    bool createInstance();
    bool pickDevice();
    std::string mFailureReason;
    bool createDevice();
    bool createFrames();
    bool selfTest();
    void measureBandwidth();            // C6: device-local copy throughput into mCaps.mMemoryBandwidthGBps (start-up, single threaded)
    void destroyFrameResources(Frame& f);
    void reportAddressHistory(const std::vector<VkDeviceFaultAddressInfoEXT>& addresses);     // LL_VK_ADDR_LOG: what held the fault page and when it was given up

    GpuPassStats mPassStats;
    bool mNextOffscreen = false;
    bool mReady = false;
    HMODULE mLoader = nullptr;
    HWND mWindow = nullptr;
    VkInstance mInstance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    U32 mQueueFamily = 0;
    VkQueue mComputeQueue = VK_NULL_HANDLE;     // E2
    VkSemaphore mAsyncSem = VK_NULL_HANDLE;     // E2: a timeline semaphore, one value for every signal of the graphics splits and the compute submits
    U64 mAsyncValue = 0;
    bool mAsyncOn = false;
    bool mStampsOff = false;
    VmaAllocator mAllocator = nullptr;
    Capabilities mCaps;
    VkCommandPool mImmediatePool = VK_NULL_HANDLE;
    VkCommandBuffer mImmediateCmd = VK_NULL_HANDLE;
    VkFence mImmediateFence = VK_NULL_HANDLE;

    Frame mFrames[FRAMES_IN_FLIGHT];
    U32 mFrameSlot = 0;
    std::atomic<U64> mRtBuildMicros[FRAMES_IN_FLIGHT] = {};
    U64 mFrameNumber = 0;
    U64 mCompletedBelow = 0;                // every frame with a number below this has been waited for (set in beginFrame after the slot's fence, raised by frameFinished for the next frame in order)
    U64 mEarlyFinishedAvoided = 0;
    void (*mViewForget)(VkImageView) = nullptr;
    bool mInFrame = false;
    std::vector<std::function<void()>> mPendingDeferred;     // destroyed with the next frame slot that completes

    struct ArenaBlock
    {
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VmaAllocation mAllocation = nullptr;
        VmaVirtualBlock mVirtual = VK_NULL_HANDLE;
        VkDeviceSize mSize = 0;
        VkDeviceAddress mAddress = 0;
        void* mMapped = nullptr;
        U32 mAllocs = 0;
        bool mShared = false;
        bool mHost = false;
        bool mAccel = false;
        bool mDedicated = false;            // holds one big range: goes away with it (an acceleration structure one waits in the pool for the next big range)
        bool mPooled = false;               // empty, in the pool of dedicated acceleration structure blocks
        U64 mCreatedFrame = 0;
        U64 mIdleSince = 0;                 // the frame it went into the pool
        U64 mUsed = 0;                      // bytes allocated in it (the placement takes the fullest block that fits; under mArenaMutex)
        U64 mEmptyFrame = 0;                // the frame its last allocation was freed (an empty regular block is given back after a while, see arenaTrimPool)
        bool mReady = true;                 // false while the GPU has not yet written the whole block once (host blocks with LL_VK_HOST_BLOCK_GATE: see arenaStartReadyFill); nothing is placed in it until then
        VkFence mReadyFence = VK_NULL_HANDLE;
        VkCommandPool mReadyPool = VK_NULL_HANDLE;
    };
    void arenaStartReadyFill(ArenaBlock* block);                 // submits the first-use fill of a new host block without waiting (the render worker's thread; mArenaMutex held)
    bool arenaFinishReady(ArenaBlock* block, bool wait);         // true when the block is ready (waits for the fill when asked); frees the fill's fence and pool
    U32 mGateLate = 0, mGatePrepared = 0;                        // blocks that were waited for, blocks that were ready in time
    double mGateLateMs = 0.0;
    U64 mPoolBytes = 0;                     // what the pool holds now (under mArenaMutex)
    U64 mPoolLastTrim = 0;
    void arenaTrimPool();                   // gives back what has waited too long or does not fit (mArenaMutex held)
    ArenaBlock* arenaNewBlock(VkDeviceSize min_size, ArenaKind kind, bool dedicated);
    void arenaDestroyBlock(ArenaBlock* block);
    void arenaFreeNow(ArenaBlock* block, VmaVirtualAllocation alloc);
    void arenaShutdown();
    std::mutex mArenaMutex;
    std::vector<ArenaBlock*> mArenaBlocks;                   // stable addresses; ArenaRef::mBlock indexes it (a destroyed block leaves a null)
    ArenaStats mArenaCounters;
};

// How the swapchain presents (setting FSVkPresentMode with the Vertical Sync preference; the environment variable LL_VK_PRESENT overrides both).
// mode: 0 follow the Vertical Sync preference (on: FIFO, off: IMMEDIATE), 1 vsync (FIFO), 2 uncapped (IMMEDIATE, can tear), 3 uncapped without tearing (MAILBOX).
// Takes effect at once: the swapchain is recreated at the start of the next frame.
void setPresentPreference(int mode, bool vsync_on);

// A swapchain for a presentation surface. Recreated on resize or when the surface becomes out of date.
class Swapchain
{
public:
    bool create(VkSurfaceKHR surface, HWND window);
    void destroy();

    // false: not drawable right now (minimised) or out of date (call create again); 'index' valid only if true
    bool acquire(VkSemaphore signal, U32& index, U64 timeout_ns = 0);
    // returns false if the swapchain needs recreating
    bool present(U32 index, VkSemaphore wait);
    // true if the last present was rejected as out of date (the window changed size before it could be shown)
    bool lastPresentOutOfDate() const { return mPresentOutOfDate; }
    // Acquire an image and wait for it on the CPU, and present one that has no semaphore to wait for (everything was waited for already).
    // Used to show the picture again right after the swapchain had to be recreated.
    bool acquireBlocking(U32& index);
    bool presentNoWait(U32 index);

    bool isValid() const { return mSwapchain != VK_NULL_HANDLE; }
    bool needsRecreate() const;
    VkImage getImage(U32 i) const { return mImages[i]; }
    VkImageView getView(U32 i) const { return mViews[i]; }
    VkSemaphore getRenderDone(U32 i) const { return mRenderDone[i]; }
    VkFormat getFormat() const { return mFormat; }
    VkExtent2D getExtent() const { return mExtent; }

private:
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    HWND mWindow = nullptr;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D mExtent = { 0, 0 };
    std::vector<VkImage> mImages;
    std::vector<VkImageView> mViews;
    std::vector<VkSemaphore> mRenderDone;
    bool mNeedsRecreate = false;
    VkPresentModeKHR mRequestedMode = VK_PRESENT_MODE_FIFO_KHR;   // what was asked for when the swapchain was made (the device may not offer it)
    bool mPresentOutOfDate = false;
};

// LL_VK_PHASES statistics: image barriers recorded, graphics pipelines built (and how long the driver took)
struct SyncStats
{
    std::atomic<U64> mImageBarriers{ 0 };
    std::atomic<U64> mPipelinesBuilt{ 0 };
    std::atomic<U64> mPipelineBuildMicros{ 0 };
    std::atomic<U64> mPipelineBuildMaxMicros{ 0 };
    // C8: pipelines the worker never waits for (see Program::getPipeline)
    std::atomic<U64> mPipeMisses{ 0 };              // lookups the driver could only answer by compiling: queued for the background threads
    std::atomic<U64> mPipeSkippedDraws{ 0 };        // draws dropped because their pipeline was still being built
    std::atomic<U64> mPipeBgDone{ 0 };              // background builds finished for a draw that asked (not pre-warm)
    std::atomic<U64> mPipeBgMicros{ 0 };
    std::atomic<U64> mPipeBgMaxMicros{ 0 };
    std::atomic<U64> mPipePrewarmQueued{ 0 };       // builds queued at program creation from the variants recorded in earlier sessions
    std::atomic<U64> mPipePrewarmDone{ 0 };
    std::atomic<U64> mPipePrewarmMicros{ 0 };
    // GPU time of each frame's command buffer (first to last command; harvested a few frames later) and how long the thread that records the frames
    // is blocked on the GPU (the frame fence), on the swapchain (acquire) and in present
    std::atomic<U64> mGpuFrames{ 0 };
    std::atomic<U64> mGpuMicros{ 0 };
    std::atomic<U64> mGpuMaxMicros{ 0 };
    std::atomic<U64> mFenceWaitMicros{ 0 };
    std::atomic<U64> mAcquireWaitMicros{ 0 };
    std::atomic<U64> mPresentMicros{ 0 };
};
extern SyncStats gSyncStats;

// helper: image layout transition barrier for a whole colour image (sync2)
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, U32 mips, U32 layers,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKCONTEXT_H
