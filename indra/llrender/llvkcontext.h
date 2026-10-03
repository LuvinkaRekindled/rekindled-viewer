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
    F32 mMaxAnisotropy = 1.f;
    F32 mTimestampPeriodNs = 1.f;   // nanoseconds per timestamp tick
    U32 mTimestampValidBits = 0;
    VkDeviceSize mUniformAlignment = 256;
    U32 mMaxPushDescriptors = 0;
};

struct Buffer
{
    VkBuffer mBuffer = VK_NULL_HANDLE;
    VmaAllocation mAllocation = nullptr;
    void* mMapped = nullptr;        // persistently mapped for host-visible buffers
    VkDeviceSize mSize = 0;
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
    static const U32 FRAMES_IN_FLIGHT = 3;

    static Context& instance();

    // Creates the instance, picks a device that can present to the window, creates the device, allocator
    // and frame ring, and runs a self test. Returns false (and logs why) if this machine is not suitable.
    bool init(HWND present_window);
    void shutdown();
    bool isReady() const { return mReady; }

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

    bool isValid() const { return mSwapchain != VK_NULL_HANDLE; }
    bool needsRecreate() const { return mNeedsRecreate; }
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
};

// helper: image layout transition barrier for a whole colour image (sync2)
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, U32 mips, U32 layers,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKCONTEXT_H
