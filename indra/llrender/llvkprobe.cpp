/**
 * @file llvkprobe.cpp
 * @brief Vulkan bring-up probe (see llvkprobe.h). Uses the Vulkan device layer in llvkcontext.
 */

#include "linden_common.h"

#include "llvkprobe.h"

#if !defined(LL_VULKAN)

bool LLVkProbe::isCompiledIn() { return false; }
void LLVkProbe::setEnabled(bool) {}
void LLVkProbe::frame() {}
void LLVkProbe::shutdown() {}

#else // LL_VULKAN

#include "llvkcontext.h"

namespace
{

const wchar_t* const WINDOW_CLASS = L"LLVkProbeWindow";

struct Probe
{
    HWND mWindow = nullptr;
    volatile bool mClosed = false;
    volatile bool mResized = false;
    LLVk::Swapchain mSwapchain;
    U64 mFramesPresented = 0;
    bool mReady = false;
    bool mFailed = false;       // do not retry every frame after a failure
};

Probe sProbe;

LRESULT CALLBACK probe_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        sProbe.mClosed = true;      // frame() notices and shuts the probe down; the window is destroyed there
        return 0;
    case WM_SIZE:
        sProbe.mResized = true;
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
    wc.lpfnWndProc = probe_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }
    sProbe.mWindow = CreateWindowExW(0, WINDOW_CLASS, L"Firestorm Vulkan probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                     100, 100, 640, 360, nullptr, nullptr, inst, nullptr);
    return sProbe.mWindow != nullptr;
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
    if (sProbe.mReady)
    {
        LL_INFOS("VkProbe") << "Vulkan probe shut down after " << sProbe.mFramesPresented << " frames" << LL_ENDL;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    if (ctx.isReady())
    {
        ctx.waitIdle();
    }
    sProbe.mSwapchain.destroy();
    ctx.shutdown();
    if (sProbe.mWindow)
    {
        DestroyWindow(sProbe.mWindow);
        sProbe.mWindow = nullptr;
    }
    sProbe.mReady = false;
    sProbe.mClosed = false;
    sProbe.mResized = false;
    sProbe.mFramesPresented = 0;
}

void LLVkProbe::setEnabled(bool enabled)
{
    if (!enabled)
    {
        if (sProbe.mReady || sProbe.mWindow)
        {
            shutdown();
        }
        sProbe.mFailed = false;
        return;
    }
    if (sProbe.mReady || sProbe.mClosed || sProbe.mFailed)
    {
        return;
    }

    LLVk::Context& ctx = LLVk::Context::instance();
    if (!create_window() || !ctx.init(sProbe.mWindow) || !sProbe.mSwapchain.create(ctx.getSurface(), sProbe.mWindow))
    {
        LL_WARNS("VkProbe") << "Vulkan probe initialisation failed" << LL_ENDL;
        shutdown();
        sProbe.mFailed = true;
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

    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Swapchain& sc = sProbe.mSwapchain;

    if (sProbe.mResized || sc.needsRecreate() || !sc.isValid())
    {
        sProbe.mResized = false;
        if (!sc.create(ctx.getSurface(), sProbe.mWindow))
        {
            shutdown();
            return;
        }
        if (!sc.isValid())
        {
            return;     // minimised
        }
    }

    // never stall the viewer for the probe: skip the frame if the GPU has not finished this slot yet
    VkCommandBuffer cmd = ctx.beginFrame(false);
    if (cmd == VK_NULL_HANDLE)
    {
        return;
    }

    U32 image_index = 0;
    if (!sc.acquire(ctx.getAcquireSemaphore(), image_index))
    {
        ctx.endFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);   // nothing to draw this time; keep the frame ring moving
        return;
    }

    LLVk::imageBarrier(cmd, sc.getImage(image_index), VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    F32 hue = (F32)(GetTickCount64() % 6000) / 6000.f;
    VkClearValue clear = {};
    hsv_to_rgb(hue, clear.color.float32[0], clear.color.float32[1], clear.color.float32[2]);
    clear.color.float32[3] = 1.f;

    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = sc.getView(image_index);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = clear;

    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, sc.getExtent() };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    LLVk::vkCmdBeginRendering(cmd, &ri);
    LLVk::vkCmdEndRendering(cmd);

    LLVk::imageBarrier(cmd, sc.getImage(image_index), VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_NONE, 0);

    ctx.endFrame(ctx.getAcquireSemaphore(), sc.getRenderDone(image_index));
    sc.present(image_index, sc.getRenderDone(image_index));

    ++sProbe.mFramesPresented;
    if (sProbe.mFramesPresented == 1)
    {
        LL_INFOS("VkProbe") << "first Vulkan frame presented" << LL_ENDL;
    }
}

#endif // LL_VULKAN
