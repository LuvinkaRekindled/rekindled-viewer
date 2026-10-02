/**
 * @file llvkmirror.cpp
 * @brief Vulkan mirror mode (see llvkmirror.h)
 */

#include "linden_common.h"

#include "llvkmirror.h"

#if !defined(LL_VULKAN)

bool LLVkMirror::isRequested() { return false; }
bool LLVkMirror::isReady() { return false; }
bool LLVkMirror::ensureReady() { return false; }
bool LLVkMirror::inFrame() { return false; }
void LLVkMirror::beginFrame(S32, S32) {}
void LLVkMirror::endFrame() {}
void LLVkMirror::shutdown() {}

#else // LL_VULKAN

#include "llvkcontext.h"
#include "llvkrenderer.h"

#include <cstdlib>

namespace LLVkGL { extern bool gActive; extern std::recursive_mutex gMutex; }

namespace
{

const wchar_t* const MIRROR_WINDOW_CLASS = L"LLVkMirrorWindow";

struct MirrorState
{
    bool mRequestChecked = false;
    bool mRequested = false;
    bool mReady = false;
    bool mFailed = false;
    bool mInFrame = false;

    HWND mWindow = nullptr;
    volatile bool mClosed = false;
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
};

MirrorState sMirror;

LRESULT CALLBACK mirror_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        sMirror.mClosed = true;     // shutdown() runs on the viewer's own thread at the next frame
        return 0;
    case WM_SIZE:
        sMirror.mResized = true;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool create_window()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mirror_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = MIRROR_WINDOW_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }
    sMirror.mWindow = CreateWindowExW(0, MIRROR_WINDOW_CLASS, L"Firestorm Vulkan mirror", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                      80, 80, 1280, 720, nullptr, nullptr, inst, nullptr);
    return sMirror.mWindow != nullptr;
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
    LL_INFOS("VkMirror") << "mirror backbuffer " << width << "x" << height << LL_ENDL;
    (void)ctx;
    return true;
}

} // anonymous namespace

bool LLVkMirror::isRequested()
{
    if (!sMirror.mRequestChecked)
    {
        sMirror.mRequestChecked = true;
        sMirror.mRequested = getenv("LL_VK_MIRROR") != nullptr;
        if (sMirror.mRequested)
        {
            LL_INFOS("VkMirror") << "LL_VK_MIRROR set: Vulkan mirror mode requested" << LL_ENDL;
        }
    }
    return sMirror.mRequested;
}

std::recursive_mutex& LLVkMirror::lock()
{
    return LLVkGL::gMutex;
}

bool LLVkMirror::isReady()
{
    return sMirror.mReady;
}

LLVk::Image* LLVkMirror::backbufferColor()
{
    return (sMirror.mReady && sMirror.mBackbuffer.valid()) ? &sMirror.mBackbuffer : nullptr;
}

LLVk::Image* LLVkMirror::backbufferDepth()
{
    return (sMirror.mReady && sMirror.mDepth.valid()) ? &sMirror.mDepth : nullptr;
}

bool LLVkMirror::inFrame()
{
    return sMirror.mReady && sMirror.mInFrame;
}

bool LLVkMirror::ensureReady()
{
    std::lock_guard<std::recursive_mutex> lock(LLVkGL::gMutex);
    if (sMirror.mReady)
    {
        return true;
    }
    if (!isRequested() || sMirror.mFailed)
    {
        return false;
    }

    LLVk::Context& ctx = LLVk::Context::instance();
    if (!create_window() || !ctx.init(sMirror.mWindow) || !LLVk::Renderer::instance().init()
        || !sMirror.mSwapchain.create(ctx.getSurface(), sMirror.mWindow))
    {
        LL_WARNS("VkMirror") << "Vulkan mirror initialisation failed; continuing with OpenGL only" << LL_ENDL;
        sMirror.mFailed = true;
        shutdown();
        sMirror.mFailed = true;
        return false;
    }
    sMirror.mReady = true;
    LLVkGL::gActive = true;
    LL_INFOS("VkMirror") << "Vulkan mirror ready" << LL_ENDL;
    return true;
}

void LLVkMirror::shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(LLVkGL::gMutex);
    LLVkGL::gActive = false;
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
        DestroyWindow(sMirror.mWindow);
        sMirror.mWindow = nullptr;
    }
    sMirror.mClosed = false;
}

void LLVkMirror::beginFrame(S32 width, S32 height)
{
    std::lock_guard<std::recursive_mutex> lock(LLVkGL::gMutex);
    if (sMirror.mInFrame)
    {
        endFrame();     // the previous frame never reached its swap (early return, snapshot): present what it drew
    }
    sMirror.mInFrame = false;
    if (!ensureReady())
    {
        return;
    }
    if (sMirror.mClosed)
    {
        shutdown();
        sMirror.mFailed = true;      // the user closed the mirror window: stay off
        return;
    }
    if (width <= 0 || height <= 0)
    {
        return;
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

    // never stall the viewer: if the GPU is behind, this frame simply is not mirrored
    VkCommandBuffer cmd = ctx.beginFrame(false);
    if (cmd == VK_NULL_HANDLE)
    {
        ++sMirror.mSkipped;
        return;
    }
    U32 image_index = 0;
    if (!sc.acquire(ctx.getAcquireSemaphore(), image_index))
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

void LLVkMirror::endFrame()
{
    std::lock_guard<std::recursive_mutex> lock(LLVkGL::gMutex);
    if (!sMirror.mReady || !sMirror.mInFrame)
    {
        return;
    }
    sMirror.mInFrame = false;

    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    LLVk::Swapchain& sc = sMirror.mSwapchain;
    const U32 index = sMirror.mImageIndex;

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
    sc.present(index, sc.getRenderDone(index));
    ++sMirror.mFrames;
}

#endif // LL_VULKAN
