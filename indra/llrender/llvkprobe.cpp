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
#include "llvkrenderer.h"

#include <cmath>

namespace
{

const wchar_t* const WINDOW_CLASS = L"LLVkProbeWindow";

// the test scene: a textured, vertex-coloured quad drawn with the viewer's own UI shader
struct TestScene
{
    LLVk::Program mProgram;
    LLVk::Image mTexture;
    LLVk::Buffer mVertices;
    VkSampler mSampler = VK_NULL_HANDLE;
    bool mTried = false;
    bool mReady = false;
};

struct Probe
{
    TestScene mScene;
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

struct Vertex
{
    F32 mPosition[3];
    F32 mUV[2];
    U8 mColor[4];
};

bool create_scene(TestScene& scene)
{
    scene.mTried = true;
    const LLVk::ProgramInfo* info = LLVk::findCompiledProgram("UI Shader");
    if (!info)
    {
        LL_INFOS("VkProbe") << "no compiled UI Shader available (start the viewer with LL_VK_SHADER_CHECK=1); clear-only probe" << LL_ENDL;
        return false;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    if (!scene.mProgram.create("UI Shader", *info))
    {
        return false;
    }
    for (const LLVk::LooseUniform& u : info->mUniforms)
    {
        LL_INFOS("VkProbe") << "UI Shader uniform " << u.mType << " " << u.mName << " @" << u.mOffset << LL_ENDL;
    }

    // 64x64 blue/white checkerboard, row 0 first
    const U32 N = 64;
    {
        LLVk::Buffer staging;
        if (!ctx.createBuffer(N * N * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, LLVk::Context::Memory::Upload, staging)
            || !ctx.createImage2D(N, N, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, scene.mTexture))
        {
            return false;
        }
        U8* px = (U8*)staging.mMapped;
        for (U32 y = 0; y < N; ++y)
        {
            for (U32 x = 0; x < N; ++x)
            {
                const bool on = ((x / 8) + (y / 8)) % 2 == 0;
                U8* p = px + (y * N + x) * 4;
                p[0] = on ? 255 : 40; p[1] = on ? 255 : 90; p[2] = 255; p[3] = 255;
            }
        }
        ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            LLVk::imageBarrier(cmd, scene.mTexture.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy c = {};
            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.imageExtent = { N, N, 1 };
            LLVk::vkCmdCopyBufferToImage(cmd, staging.mBuffer, scene.mTexture.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            LLVk::imageBarrier(cmd, scene.mTexture.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        scene.mTexture.mLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ctx.waitIdle();
        vmaDestroyBuffer(ctx.getAllocator(), staging.mBuffer, staging.mAllocation);
    }

    // a quad as a triangle strip; position in clip space (z 0), uv 0..4 (repeated), corner colours
    const Vertex quad[4] = {
        { { -0.6f, -0.6f, 0.f }, { 0.f, 0.f }, { 255, 80, 80, 255 } },
        { {  0.6f, -0.6f, 0.f }, { 4.f, 0.f }, { 80, 255, 80, 255 } },
        { { -0.6f,  0.6f, 0.f }, { 0.f, 4.f }, { 80, 80, 255, 255 } },
        { {  0.6f,  0.6f, 0.f }, { 4.f, 4.f }, { 255, 255, 255, 255 } },
    };
    if (!ctx.createBuffer(sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, LLVk::Context::Memory::Upload, scene.mVertices))
    {
        return false;
    }
    memcpy(scene.mVertices.mMapped, quad, sizeof(quad));

    LLVk::SamplerDesc sd;
    sd.mMag = VK_FILTER_NEAREST;
    sd.mMin = VK_FILTER_NEAREST;
    sd.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    scene.mSampler = LLVk::Renderer::instance().getSampler(sd);

    scene.mReady = true;
    LL_INFOS("VkProbe") << "test scene ready" << LL_ENDL;
    return true;
}

void destroy_scene(TestScene& scene)
{
    LLVk::Context& ctx = LLVk::Context::instance();
    if (ctx.getDevice() != VK_NULL_HANDLE)
    {
        ctx.waitIdle();
        if (scene.mVertices.valid())
        {
            vmaDestroyBuffer(ctx.getAllocator(), scene.mVertices.mBuffer, scene.mVertices.mAllocation);
        }
        if (scene.mTexture.valid())
        {
            LLVk::vkDestroyImageView(ctx.getDevice(), scene.mTexture.mView, nullptr);
            vmaDestroyImage(ctx.getAllocator(), scene.mTexture.mImage, scene.mTexture.mAllocation);
        }
        scene.mProgram.destroy();
    }
    scene = TestScene();
}

void draw_scene(TestScene& scene, LLVk::Image& target, F32 time_seconds)
{
    LLVk::Renderer& r = LLVk::Renderer::instance();

    // rotation about z with the aspect ratio folded in. The final image is not flipped yet (that happens at
    // presentation in the real renderer), so y is negated here to show the quad the right way up.
    const F32 aspect = (F32)target.mExtent.width / (F32)target.mExtent.height;
    const F32 c = cosf(time_seconds), s = sinf(time_seconds);
    const F32 mvp[16] = {                       // column-major mat4
        c / aspect, -s,        0.f, 0.f,
        -s / aspect, -c,       0.f, 0.f,
        0.f,         0.f,      1.f, 0.f,
        0.f,         0.f,      0.f, 1.f,
    };
    const F32 identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    scene.mProgram.setMatrices("modelview_projection_matrix", mvp, 4, 4);
    scene.mProgram.setMatrices("texture_matrix0", identity, 4, 4);

    LLVk::VertexLayout layout;
    layout.mStride = sizeof(Vertex);
    layout.mAttributes = {
        { 0, VK_FORMAT_R32G32B32_SFLOAT, (U32)offsetof(Vertex, mPosition) },    // position
        { 2, VK_FORMAT_R32G32_SFLOAT, (U32)offsetof(Vertex, mUV) },             // texcoord0
        { 6, VK_FORMAT_R8G8B8A8_UNORM, (U32)offsetof(Vertex, mColor) },         // diffuse_color
    };

    r.setProgram(&scene.mProgram);
    r.setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    r.setVertexLayout(layout);
    r.setVertexBuffer(scene.mVertices.mBuffer, 0);
    const S32 binding = scene.mProgram.getSamplerBinding("diffuseMap");
    if (binding >= 0)
    {
        r.setTexture((U32)binding, &scene.mTexture, scene.mSampler);
    }
    r.draw(4, 0);
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
    destroy_scene(sProbe.mScene);
    LLVk::Renderer::instance().shutdown();
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
    if (!create_window() || !ctx.init(sProbe.mWindow) || !LLVk::Renderer::instance().init()
        || !sProbe.mSwapchain.create(ctx.getSurface(), sProbe.mWindow))
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

    if (!sProbe.mScene.mTried)
    {
        create_scene(sProbe.mScene);
    }

    // wrap the swapchain image so the renderer can track its layout; its contents are undefined after acquire
    LLVk::Image target;
    target.mImage = sc.getImage(image_index);
    target.mView = sc.getView(image_index);
    target.mFormat = sc.getFormat();
    target.mAspect = VK_IMAGE_ASPECT_COLOR_BIT;
    target.mExtent = { sc.getExtent().width, sc.getExtent().height, 1 };
    target.mLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    renderer.beginFrame(cmd, ctx.getFrameSlot());
    LLVk::Image* targets[1] = { &target };
    renderer.setRenderTarget(targets, 1, nullptr);

    const F32 time_seconds = (F32)(GetTickCount64() % 60000) / 1000.f;
    const F32 hue = (F32)(GetTickCount64() % 6000) / 6000.f;
    F32 clear_color[4] = { 0, 0, 0, 1 };
    hsv_to_rgb(hue, clear_color[0], clear_color[1], clear_color[2]);
    for (int i = 0; i < 3; ++i) clear_color[i] *= 0.35f;      // dim background so the quad stands out
    renderer.clear(LLVk::CLEAR_COLOR, clear_color, 1.f, 0);

    if (sProbe.mScene.mReady)
    {
        draw_scene(sProbe.mScene, target, time_seconds);
    }

    renderer.ensureLayout(target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    renderer.endFrame();

    ctx.endFrame(ctx.getAcquireSemaphore(), sc.getRenderDone(image_index));
    sc.present(image_index, sc.getRenderDone(image_index));

    ++sProbe.mFramesPresented;
    if (sProbe.mFramesPresented == 1)
    {
        LL_INFOS("VkProbe") << "first Vulkan frame presented" << LL_ENDL;
    }
}

#endif // LL_VULKAN
