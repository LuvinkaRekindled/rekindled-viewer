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
#include <map>
#include <string>
#include <functional>
#include <vector>

#include <windows.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// the same VMA configuration must be used by every translation unit (llvkvma.cpp holds the implementation)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000
#pragma warning(push, 0)
#include "thirdparty/vk_mem_alloc.h"
#pragma warning(pop)

namespace LLVk
{

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
    bool mWideLines = false;
    bool mDepthClamp = false;       // dynamic depth clamp (the shadow passes need it)
    bool mAnisotropy = false;
    bool mBC = false;               // BC1-7 texture compression
    bool mGeometryShader = false;
    bool mMultiDrawIndirect = false;
    bool mBufferDeviceAddress = false;  // shaders can read buffers by address (render packets)
    bool mDrawIndirectFirstInstance = false;    // indirect draws may use a first instance (render packets: the draw number)
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
    bool valid() const { return mImage != VK_NULL_HANDLE; }
};

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
    void shutdown();
    bool isReady() const { return mReady; }

    // ---- LL_VK_PHASES: GPU time by render pass ----
    // A timestamp is written when each render pass begins; the time up to the next one is charged to that pass's tag. Read back when the frame slot is reused.
    static const U32 MAX_GPU_STAMPS = 640;
    static U32 stampTag(const std::string& name);       // the number standing for a pass description (0 is "start of frame")
    static std::string stampTagName(U32 tag);
    void gpuStamp(U32 tag);
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
    // true once the GPU has finished the frame that was the n-th to be submitted (false while it is still being recorded)
    bool frameFinished(U64 n);
    U32 getFrameSlot() const { return mFrameSlot; }
    bool inFrame() const { return mInFrame; }

    // Upload memory that lives until this frame's GPU work completes
    StagingAlloc allocStaging(VkDeviceSize size, VkDeviceSize alignment = 16);

    // Run f after the GPU has finished every frame submitted so far (safe place to destroy resources)
    void destroyLater(std::function<void()> f);

    // --- resources ----------------------------------------------------------------------------------
    // Upload and Readback buffers are persistently mapped (mMapped): Upload is write-combined host memory for
    // the CPU to fill, Readback is cached host memory for the CPU to read. Device buffers are device-local.
    enum class Memory { Device, Upload, UploadDevice, Readback };   // UploadDevice: CPU-written, GPU-read buffers (device-local host-visible memory when the GPU has it)
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory, Buffer& out);
    void destroyBuffer(Buffer& buffer);     // deferred
    bool createImage2D(U32 width, U32 height, U32 mips, U32 layers, VkFormat format, VkImageUsageFlags usage, Image& out);
    void destroyImage(Image& image);        // deferred

    void waitIdle();
    // "heap 0 (device) 3200/15000 MB; heap 1 (host) ..." : usage and budget of every memory heap, for the benchmark log
    std::string memoryBudgetText();
    // size of the device-local memory heaps in bytes (the GPU's video memory)
    U64 deviceLocalBytes() const;
    size_t pendingDestroys() const;      // resources queued for destruction that no frame has released yet
    // names an object for the validation layer messages (only when validation is on)
    void setObjectName(VkObjectType type, U64 handle, const std::string& name);

    // Records into a one-off command buffer, submits it and waits. For start-up work and rare uploads only.
    bool immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    // Submits what the current frame has recorded so far, waits for the GPU, and carries on recording into the same
    // frame (same slot, same command buffer, swapchain semaphores untouched). Lets a readback see the frame's results.
    bool flushFrameAndWait();

private:
    Context() = default;

    struct Frame
    {
        VkCommandPool mPool = VK_NULL_HANDLE;
        VkCommandBuffer mCmd = VK_NULL_HANDLE;
        VkFence mFence = VK_NULL_HANDLE;
        VkSemaphore mAcquire = VK_NULL_HANDLE;
        bool mSubmitted = false;
        VkQueryPool mTsPool = VK_NULL_HANDLE;   // timestamps: 0 the start and 1 the end of this frame's command buffer, then one per render pass (LL_VK_PHASES)
        bool mTsPending = false;                // the end timestamp was written and has not been read yet
        bool mOffscreen = false;                // the off-screen frame the mirror opens after the present (not the frame that is presented)
        std::vector<U32> mStampTags;            // what each per-pass timestamp stands for (see Context::stampTag)
        std::vector<std::function<void()>> mDeferred;
        std::vector<Buffer> mStagingBlocks;     // blocks used this frame (the first is kept for reuse)
        VkDeviceSize mStagingUsed = 0;
    };

    bool loadLoader();
    bool createInstance();
    bool pickDevice();
    bool createDevice();
    bool createFrames();
    bool selfTest();
    void destroyFrameResources(Frame& f);

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
    VmaAllocator mAllocator = nullptr;
    Capabilities mCaps;
    VkCommandPool mImmediatePool = VK_NULL_HANDLE;
    VkCommandBuffer mImmediateCmd = VK_NULL_HANDLE;
    VkFence mImmediateFence = VK_NULL_HANDLE;

    Frame mFrames[FRAMES_IN_FLIGHT];
    U32 mFrameSlot = 0;
    U64 mFrameNumber = 0;
    bool mInFrame = false;
    std::vector<std::function<void()>> mPendingDeferred;     // destroyed with the next frame slot that completes
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
