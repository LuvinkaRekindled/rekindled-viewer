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
#include <vector>

namespace
{

const wchar_t* const WINDOW_CLASS = L"LLVkProbeWindow";

// the test scene: a textured, vertex-coloured quad drawn with the viewer's own UI shader
struct TestScene
{
    LLVk::Program mProgram;
    LLVk::Image mTexMip;        // 256x256 checker with a GPU-generated mip chain
    LLVk::Image mTexNoMip;      // the same checker, one level
    LLVk::Image mArray, mCube, mVolume, mCompressed;     // creation + upload tests, never sampled
    LLVk::Buffer mVertices;
    VkSampler mSamplerMip = VK_NULL_HANDLE;
    VkSampler mSamplerNoMip = VK_NULL_HANDLE;
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

    // 256x256 checker with 2 pixel cells: far finer than the screen can show, so mips matter
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    const U32 N = 256;
    std::vector<U8> px(N * N * 4);
    for (U32 y = 0; y < N; ++y)
    {
        for (U32 x = 0; x < N; ++x)
        {
            const bool on = ((x / 2) + (y / 2)) % 2 == 0;
            U8* p = &px[(y * N + x) * 4];
            p[0] = on ? 255 : 20; p[1] = on ? 255 : 60; p[2] = on ? 255 : 200; p[3] = 255;
        }
    }
    LLVk::TextureDesc d;
    d.mWidth = N;
    d.mHeight = N;
    d.mMips = LLVk::mipLevelCount(N, N);
    bool ok = renderer.createTexture(d, scene.mTexMip)
        && renderer.uploadTexture(scene.mTexMip, 0, 0, 0, 0, N, N, 1, px.data(), px.size())
        && renderer.generateMipmaps(scene.mTexMip);
    d.mMips = 1;
    ok = ok && renderer.createTexture(d, scene.mTexNoMip)
        && renderer.uploadTexture(scene.mTexNoMip, 0, 0, 0, 0, N, N, 1, px.data(), px.size());
    LL_INFOS("VkProbe") << "texture test: mip chain " << (ok ? "OK" : "FAILED") << " (" << LLVk::mipLevelCount(N, N) << " levels)" << LL_ENDL;
    if (!ok)
    {
        return false;
    }

    // other texture kinds: create and upload one slice/face/block each
    {
        LLVk::TextureDesc a;
        a.mType = LLVk::TexType::Tex2DArray; a.mWidth = 64; a.mHeight = 64; a.mDepth = 4; a.mMips = 1;
        LLVk::TextureDesc cu;
        cu.mType = LLVk::TexType::Cube; cu.mWidth = 32; cu.mHeight = 32; cu.mMips = 1;
        LLVk::TextureDesc v;
        v.mType = LLVk::TexType::Tex3D; v.mWidth = 16; v.mHeight = 16; v.mDepth = 8; v.mMips = 1;
        LLVk::TextureDesc bc;
        bc.mWidth = 16; bc.mHeight = 16; bc.mMips = 1; bc.mFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        std::vector<U8> buf(64 * 64 * 4, 0x7F);
        const bool arr_ok = renderer.createTexture(a, scene.mArray) && renderer.uploadTexture(scene.mArray, 0, 2, 0, 0, 64, 64, 1, buf.data(), buf.size());
        const bool cube_ok = renderer.createTexture(cu, scene.mCube) && renderer.uploadTexture(scene.mCube, 0, 5, 0, 0, 32, 32, 1, buf.data(), buf.size());
        const bool vol_ok = renderer.createTexture(v, scene.mVolume) && renderer.uploadTexture(scene.mVolume, 0, 3, 0, 0, 16, 16, 1, buf.data(), buf.size());
        const bool bc_ok = renderer.createTexture(bc, scene.mCompressed) && renderer.uploadTexture(scene.mCompressed, 0, 0, 0, 0, 16, 16, 1, buf.data(), 16 * 8);
        LL_INFOS("VkProbe") << "texture test: array " << (arr_ok ? "OK" : "FAILED") << ", cube " << (cube_ok ? "OK" : "FAILED") << ", 3D "
                            << (vol_ok ? "OK" : "FAILED") << ", BC1 " << (bc_ok ? "OK" : "FAILED") << LL_ENDL;
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
    sd.mMag = VK_FILTER_LINEAR;
    sd.mMin = VK_FILTER_LINEAR;
    sd.mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    scene.mSamplerMip = renderer.getSampler(sd);
    sd.mMip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sd.mMag = VK_FILTER_NEAREST;
    sd.mMin = VK_FILTER_NEAREST;
    scene.mSamplerNoMip = renderer.getSampler(sd);

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
        LLVk::Image* images[] = { &scene.mTexMip, &scene.mTexNoMip, &scene.mArray, &scene.mCube, &scene.mVolume, &scene.mCompressed };
        for (LLVk::Image* img : images)
        {
            if (img->valid())
            {
                LLVk::vkDestroyImageView(ctx.getDevice(), img->mView, nullptr);
                vmaDestroyImage(ctx.getAllocator(), img->mImage, img->mAllocation);
            }
        }
        scene.mProgram.destroy();
    }
    scene = TestScene();
}

void make_mvp(F32* m, F32 aspect, F32 time_seconds, F32 scale, F32 tx, F32 ty)
{
    // rotation about z, scaled, translated; column-major. y is negated because the final image is not flipped
    // yet (the real renderer flips at presentation), so the quad shows the right way up.
    const F32 c = cosf(time_seconds) * scale, s = sinf(time_seconds) * scale;
    const F32 v[16] = {
        c / aspect, -s,  0.f, 0.f,
        -s / aspect, -c, 0.f, 0.f,
        0.f,         0.f, 1.f, 0.f,
        tx,         -ty,  0.f, 1.f,
    };
    memcpy(m, v, sizeof(v));
}

void draw_scene(TestScene& scene, LLVk::Image& target, F32 time_seconds)
{
    LLVk::Renderer& r = LLVk::Renderer::instance();
    const F32 aspect = (F32)target.mExtent.width / (F32)target.mExtent.height;
    const F32 identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

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
    scene.mProgram.setMatrices("texture_matrix0", identity, 4, 4);
    const S32 binding = scene.mProgram.getSamplerBinding("diffuseMap");

    // left: no mips (aliases), right: mip chain (smooth). Two draws with different uniforms and textures.
    struct { LLVk::Image* tex; VkSampler sampler; F32 tx; } quads[2] = {
        { &scene.mTexNoMip, scene.mSamplerNoMip, -0.5f },
        { &scene.mTexMip, scene.mSamplerMip, 0.5f },
    };
    for (auto& q : quads)
    {
        F32 mvp[16];
        make_mvp(mvp, aspect, time_seconds, 0.4f, q.tx, 0.f);
        scene.mProgram.setMatrices("modelview_projection_matrix", mvp, 4, 4);
        if (binding >= 0)
        {
            r.setTexture((U32)binding, q.tex, q.sampler);
        }
        r.draw(4, 0);
    }
}

// Draws the test scene into an off-screen image and reads it back, so the draw path can be judged from numbers
// instead of from what a window happens to show.
void verify_render(TestScene& scene)
{
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    const U32 W = 512, H = 256;

    LLVk::TextureDesc td;
    td.mWidth = W;
    td.mHeight = H;
    td.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    LLVk::Image target;
    LLVk::Buffer readback;
    if (!renderer.createTexture(td, target)
        || !ctx.createBuffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, LLVk::Context::Memory::Readback, readback))
    {
        LL_WARNS("VkProbe") << "render verification: could not create targets" << LL_ENDL;
        return;
    }
    memset(readback.mMapped, 0, W * H * 4);

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        renderer.beginFrame(cmd, ctx.getFrameSlot());
        LLVk::Image* targets[1] = { &target };
        renderer.setRenderTarget(targets, 1, nullptr);
        const F32 bg[4] = { 0.2f, 0.4f, 0.6f, 1.f };
        renderer.clear(LLVk::CLEAR_COLOR, bg, 1.f, 0);
        draw_scene(scene, target, 0.f);                  // t = 0: quads unrotated
        renderer.ensureLayout(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        renderer.endFrame();
        VkBufferImageCopy c = {};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { W, H, 1 };
        LLVk::vkCmdCopyImageToBuffer(cmd, target.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.mBuffer, 1, &c);
    });

    const U8* px = (const U8*)readback.mMapped;
    auto pixel = [&](U32 x, U32 y) { return px + (y * W + x) * 4; };
    const U8* b = pixel(2, 2);

    // statistics of the green channel inside each quad (centres at +-0.5 ndc, half size 0.24 ndc vertically)
    auto region_stats = [&](U32 cx, U32 cy, U32 half, F32& mean, F32& variance, U32& non_bg) {
        F64 sum = 0, sum2 = 0;
        U32 n = 0;
        non_bg = 0;
        for (U32 y = cy - half; y < cy + half; ++y)
        {
            for (U32 x = cx - half; x < cx + half; ++x)
            {
                const U8* q = pixel(x, y);
                sum += q[1]; sum2 += (F64)q[1] * q[1]; ++n;
                if (abs((S32)q[0] - b[0]) > 8 || abs((S32)q[1] - b[1]) > 8 || abs((S32)q[2] - b[2]) > 8) ++non_bg;
            }
        }
        mean = (F32)(sum / n);
        variance = (F32)(sum2 / n - (sum / n) * (sum / n));
    };
    F32 mean_l, var_l, mean_r, var_r;
    U32 nb_l, nb_r;
    region_stats(W / 4, H / 2, 20, mean_l, var_l, nb_l);
    region_stats(W * 3 / 4, H / 2, 20, mean_r, var_r, nb_r);

    LL_INFOS("VkProbe") << "render verification (offscreen " << W << "x" << H << "): background pixel (" << (S32)b[0] << "," << (S32)b[1] << ","
                        << (S32)b[2] << ") expected (51,102,153); left quad (no mips) mean " << mean_l << " variance " << var_l << " covered " << nb_l
                        << "/1600; right quad (mips) mean " << mean_r << " variance " << var_r << " covered " << nb_r << "/1600" << LL_ENDL;

    ctx.waitIdle();
    vmaDestroyBuffer(ctx.getAllocator(), readback.mBuffer, readback.mAllocation);
    ctx.destroyImage(target);
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
        if (create_scene(sProbe.mScene))
        {
            verify_render(sProbe.mScene);
        }
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
    if (sProbe.mFramesPresented < 3)
    {
        LL_INFOS("VkProbe") << "frame " << sProbe.mFramesPresented << ": " << renderer.getDrawCount() << " draws, swapchain image " << image_index
                            << " " << sc.getExtent().width << "x" << sc.getExtent().height << LL_ENDL;
    }
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
