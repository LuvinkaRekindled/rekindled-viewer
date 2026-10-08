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
void LLVkMirror::setMainWindow(void*) {}
bool LLVkMirror::describeDevice(std::string&, std::string&, std::string&, U32&) { return false; }
bool LLVkMirror::inFrame() { return false; }
void LLVkMirror::beginFrame(S32, S32) {}
void LLVkMirror::endFrame() {}
void LLVkMirror::holdFrame(bool) {}
void LLVkMirror::armSnapshotCompare() {}
void LLVkMirror::setPresentPreference(S32, bool) {}
void LLVkMirror::snapshotBoundary(S32) {}
bool LLVkMirror::upscaleAvailable() { return false; }
void LLVkMirror::setUpscaleLodBias(float) {}
bool LLVkMirror::shadowPacketsAvailable() { return false; }
bool LLVkMirror::shadowAlphaPacketsAvailable() { return false; }
void LLVkMirror::shadowPackets(const ShadowPacketsHeader&, const void*, size_t) {}
void LLVkMirror::upscale(const UpscaleArgs&) {}
void LLVkMirror::presentFrame() {}
void LLVkMirror::shutdown() {}
void LLVkMirror::stopAsync() {}

#else // LL_VULKAN

#include "llglheaders.h"
#include <psapi.h>
#include "llvkcontext.h"
#include "llvkdlss.h"
#include "llvkrenderer.h"
#include "llvkverify.h"
#include "llvkasync.h"
#include "llwindow.h"

#include <cstdlib>

namespace LLVkGL { extern bool gActive; extern bool gOwn; extern LLVkMirror::Mutex gMutex; void after_frame(); double take_hook_ms(); std::string resource_summary(); }

namespace LLVkMirror
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

//-----------------------------------------------------------------------------------------------------
// Benchmark mode (LL_VK_MIRROR_BENCH=1): times the same work on the GPU in OpenGL (timestamp queries) and in Vulkan (timestamp
// queries in the mirrored command buffers). OpenGL is finished before each Vulkan frame is submitted and Vulkan is waited for
// before OpenGL continues, so the two never share the GPU while being timed. A loop iteration has two stretches of work:
//   A: display() up to the swap           (GL: stamps 0..1, Vulkan: the present frame)
//   B: probe update, snapshots, the rest  (GL: stamps 2..3, Vulkan: the off-screen frame)
// The CPU cost of recording the work into Vulkan (time inside the draw/clear hooks) is reported alongside.
//-----------------------------------------------------------------------------------------------------

bool bench_enabled()
{
    static const bool enabled = getenv("LL_VK_MIRROR_BENCH") != nullptr;
    return enabled;
}

const char* const PHASE_NAMES[] = { "all hooks", "uniform hooks", "draw hooks", "draw: target", "draw: state", "draw: textures", "draw: uniform blocks", "draw: vertex input", "draw: renderer apply+record", "draw: debug stats",
                                     "apply: texture layouts", "apply: begin pass", "apply: pipeline", "apply: dynamic state", "apply: vertex input", "apply: uniforms", "apply: push textures", "apply: vkCmdDraw" };

// LL_VK_MIRROR_GLONLY=1 (with LL_VK_MIRROR_BENCH=1, without LL_VK_MIRROR): no Vulkan at all; only the GL side is timed, at the viewer's natural pace
bool glonly_mode()
{
    static const bool glonly = getenv("LL_VK_MIRROR_GLONLY") != nullptr && getenv("LL_VK_MIRROR_BENCH") != nullptr;
    return glonly;
}

// kinds of pass the Vulkan GPU time is split into (every fourth iteration is recorded with a timestamp at each change of kind)
const char* const BENCH_KINDS[] = { "setup/other", "shadow", "occlusion", "sky", "gbuffer", "lighting", "alpha", "post", "probes", "hud", "ui" };
const U32 BENCH_KIND_COUNT = (U32)(sizeof(BENCH_KINDS) / sizeof(BENCH_KINDS[0]));

U32 bench_kind_of(const char* n)
{
    const std::string name = n ? n : "";
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("Shadow")) return 1;
    if (has("Occlusion")) return 2;
    if (has("Reflection") || has("Radiance") || has("Irradiance")) return 8;
    if (has("Windlight") || has("Star") || has("Sun Program") || has("Cloud")) return 3;
    if (has("Soften") || has("Light Shader") || has("MultiLight") || has("SpotLight") || has("Sun Shader") || has("Blur Light")) return 5;
    if (has("HUD")) return 9;
    if (has("UI Shader")) return 10;
    if (has("Glow") || has("Tonemap") || has("Exposure") || has("Luminance") || has("SMAA") || has("Sharpening") || has("Post") || has("Copy") || has("Haze")) return 7;
    if (has("Alpha")) return 6;
    return 4;
}

struct Bench
{
    static const U32 SLOTS = 16;
    static const U32 PHASE_QUERIES = 4096;
    bool mInit = false;
    VkQueryPool mPool = VK_NULL_HANDLE;
    VkQueryPool mPhasePool = VK_NULL_HANDLE;
    bool mPhaseIteration = false;           // this iteration's present frame records phase timestamps
    bool mPhaseActive = false;              // ... and is recording right now
    U32 mPhaseUsed = 0;
    U32 mPhaseKind[PHASE_QUERIES] = {};
    int mCurrentKind = -1;
    double mKindMs[16] = {};
    U32 mPhaseFrames = 0;
    double mThreadCpu = 0;
    ULONGLONG mLastThreadTime = 0;
    U32 mNextSlot = 0;
    U32 mCurSlot = 0;
    GLuint mGlQ[4][4] = {};
    U32 mGlMask[4] = {};
    U32 mIter = 0;
    double mGlA = 0, mGlB = 0, mVkA = 0, mVkB = 0, mHook = 0, mWall = 0, mGlCpu = 0, mDispCpu = 0;
    U32 mGlN = 0, mVkAN = 0, mVkBN = 0, mWallN = 0, mDispN = 0;
    LARGE_INTEGER mLastT0 = {};
    LARGE_INTEGER mDispStart = {};
};
Bench sBench;

void bench_init()
{
    if (!bench_enabled() || sBench.mInit)
    {
        return;
    }
    sBench.mInit = true;
    LLVkMirror::gPhaseTiming = !glonly_mode();
    glGenQueries(16, &sBench.mGlQ[0][0]);
    if (glonly_mode())
    {
        LL_INFOS("VkBench") << "benchmark mode on, OpenGL only: no Vulkan work; GL GPU time and CPU time are measured at the viewer's natural pace" << LL_ENDL;
        return;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = Bench::SLOTS * 2;
    LLVk::vkCreateQueryPool(ctx.getDevice(), &qi, nullptr, &sBench.mPool);
    qi.queryCount = Bench::PHASE_QUERIES;
    LLVk::vkCreateQueryPool(ctx.getDevice(), &qi, nullptr, &sBench.mPhasePool);
    LL_INFOS("VkBench") << "benchmark mode on: GL and Vulkan GPU times are measured with the two APIs serialised" << LL_ENDL;
}

void bench_gl_stamp(U32 k)
{
    if (!sBench.mInit)
    {
        return;
    }
    const U32 s = sBench.mIter % 4;
    glQueryCounter(sBench.mGlQ[s][k], GL_TIMESTAMP);
    glFlush();
    sBench.mGlMask[s] |= 1u << k;
}

void bench_log()
{
    const double n = (double)std::max<U32>(1, sBench.mGlN);
    LL_INFOS("VkBench") << "BENCH over " << sBench.mGlN << " iterations: wall " << (sBench.mWallN ? sBench.mWall / sBench.mWallN : 0.0) << " ms/iter | GPU OpenGL display "
                        << sBench.mGlA / n << " ms + rest " << sBench.mGlB / n << " ms = " << (sBench.mGlA + sBench.mGlB) / n
                        << " ms | GPU Vulkan present frame " << (sBench.mVkAN ? sBench.mVkA / sBench.mVkAN : 0.0) << " ms + off-screen frame "
                        << (sBench.mVkBN ? sBench.mVkB / sBench.mVkBN : 0.0) << " ms | CPU recording into Vulkan " << sBench.mHook / std::max<U32>(1, sBench.mWallN)
                        << " ms/iter | CPU inside OpenGL calls " << sBench.mGlCpu / std::max<U32>(1, sBench.mWallN) << " ms/iter | CPU of display() up to the swap "
                        << (sBench.mDispN ? sBench.mDispCpu / sBench.mDispN : 0.0) << " ms | main thread CPU busy " << sBench.mThreadCpu / std::max<U32>(1, sBench.mWallN) << " ms/iter" << LL_ENDL;
    {
        U32 ring_peak = 0, ring_dropped = 0;
        LLVk::Renderer::instance().getRingStats(ring_peak, ring_dropped);
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        LL_INFOS("VkBench") << "BENCH memory: process working set " << pmc.WorkingSetSize / (1024 * 1024) << " MB, private " << pmc.PrivateUsage / (1024 * 1024)
                            << " MB | uniform ring peak " << ring_peak / 1024 << " KB of " << (128 * 1024) << " KB per frame, " << ring_dropped << " draws dropped | "
                            << LLVk::Context::instance().memoryBudgetText() << LL_ENDL;
        LL_INFOS("VkBench") << "BENCH resources: " << LLVkGL::resource_summary() << LL_ENDL;
    }
    if (LLVkMirror::gPhaseTiming)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        std::string text;
        for (int p = 0; p < LLVkMirror::PH_COUNT; ++p)
        {
            text += std::string(PHASE_NAMES[p]) + " " + std::to_string((double)LLVkMirror::gPhaseTicks[p] * 1000.0 / (double)freq.QuadPart / std::max<U32>(1, sBench.mWallN)).substr(0, 5) + " | ";
            LLVkMirror::gPhaseTicks[p] = 0;
        }
        LL_INFOS("VkBench") << "BENCH translator CPU by phase (ms/iter; hooks include the draw phases, 'all hooks' also the unhooked-uniform and state calls): " << text << LL_ENDL;
    }
    if (sBench.mPhaseFrames)
    {
        std::vector<std::pair<double, U32>> kinds;
        for (U32 k = 0; k < BENCH_KIND_COUNT; ++k) kinds.push_back({ sBench.mKindMs[k] / sBench.mPhaseFrames, k });
        std::sort(kinds.rbegin(), kinds.rend());
        std::string text;
        double total = 0;
        for (auto& kv : kinds)
        {
            total += kv.first;
            if (kv.first > 0.01) text += std::string(BENCH_KINDS[kv.second]) + " " + std::to_string(kv.first).substr(0, 5) + " | ";
        }
        LL_INFOS("VkBench") << "BENCH Vulkan present-frame GPU by pass kind over " << sBench.mPhaseFrames << " frames (timestamps between kinds, a little pessimistic): " << text << "total " << total << " ms" << LL_ENDL;
    }
    sBench.mGlA = sBench.mGlB = sBench.mVkA = sBench.mVkB = sBench.mHook = sBench.mWall = sBench.mGlCpu = sBench.mDispCpu = sBench.mThreadCpu = 0;
    sBench.mGlN = sBench.mVkAN = sBench.mVkBN = sBench.mWallN = sBench.mDispN = 0;
    sBench.mPhaseFrames = 0;
    for (double& v : sBench.mKindMs) v = 0;
}

// the swap is about to happen: CPU time of display() so far
void bench_mark_present()
{
    if (!sBench.mInit || !sBench.mDispStart.QuadPart)
    {
        return;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    sBench.mDispCpu += (double)(now.QuadPart - sBench.mDispStart.QuadPart) * 1000.0 / (double)freq.QuadPart;
    ++sBench.mDispN;
}

// start of a loop iteration (display() begins)
void bench_begin_iteration()
{
    if (!sBench.mInit)
    {
        return;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (sBench.mLastT0.QuadPart)
    {
        sBench.mWall += (double)(now.QuadPart - sBench.mLastT0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        ++sBench.mWallN;
    }
    sBench.mLastT0 = now;
    sBench.mDispStart = now;
    {
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        {
            const ULONGLONG total = (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) + (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
            if (sBench.mLastThreadTime)
            {
                sBench.mThreadCpu += (double)(total - sBench.mLastThreadTime) / 10000.0;      // 100 ns units
            }
            sBench.mLastThreadTime = total;
        }
    }
    sBench.mPhaseIteration = !glonly_mode() && (sBench.mIter % 4) == 0;
    {
        // GL call time of the previous iteration (this thread's counter, then reset)
        sBench.mGlCpu += (double)LLVkGL::gGlTicks * 1000.0 / (double)freq.QuadPart;
        LLVkGL::gGlTicks = 0;
    }
    if (sBench.mIter >= 2)      // results of the iteration before last are certainly available
    {
        const U32 s = (sBench.mIter - 2) % 4;
        if (sBench.mGlMask[s] == 15)
        {
            GLint available = 0;
            glGetQueryObjectiv(sBench.mGlQ[s][3], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available)
            {
                GLuint64 t[4];
                for (U32 k = 0; k < 4; ++k) glGetQueryObjectui64v(sBench.mGlQ[s][k], GL_QUERY_RESULT, &t[k]);
                sBench.mGlA += (double)(t[1] - t[0]) / 1.0e6;
                sBench.mGlB += (double)(t[3] - t[2]) / 1.0e6;
                ++sBench.mGlN;
                sBench.mGlMask[s] = 0;
            }
        }
    }
    ++sBench.mIter;
    sBench.mGlMask[sBench.mIter % 4] = 0;
    if (sBench.mIter % 150 == 0)
    {
        bench_log();
    }
    bench_gl_stamp(0);
}

void bench_vk_begin(VkCommandBuffer cmd, bool offscreen)
{
    if (!sBench.mInit || glonly_mode())
    {
        return;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    sBench.mCurSlot = sBench.mNextSlot++ % Bench::SLOTS;
    LLVk::vkResetQueryPool(ctx.getDevice(), sBench.mPool, sBench.mCurSlot * 2, 2);
    LLVk::vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, sBench.mPool, sBench.mCurSlot * 2);
    sBench.mPhaseActive = false;
    if (!offscreen && sBench.mPhaseIteration && sBench.mPhasePool)
    {
        LLVk::vkResetQueryPool(ctx.getDevice(), sBench.mPhasePool, 0, Bench::PHASE_QUERIES);
        sBench.mPhaseUsed = 0;
        sBench.mCurrentKind = -1;
        sBench.mPhaseActive = true;
    }
}

// just before the frame's command buffer is submitted: end stamp, and OpenGL is finished first
void bench_vk_end(VkCommandBuffer cmd)
{
    if (!sBench.mInit || glonly_mode())
    {
        return;
    }
    LLVk::vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, sBench.mPool, sBench.mCurSlot * 2 + 1);
    if (sBench.mPhaseActive && sBench.mPhaseUsed > 0)
    {
        LLVk::vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, sBench.mPhasePool, sBench.mPhaseUsed);
    }
    glFinish();
}

// after the frame was submitted: wait for it and read its GPU time
void bench_vk_collect(bool offscreen)
{
    if (!sBench.mInit)
    {
        return;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    ctx.waitIdle();
    U64 t[2] = {};
    LLVk::vkGetQueryPoolResults(ctx.getDevice(), sBench.mPool, sBench.mCurSlot * 2, 2, sizeof(t), t, sizeof(U64), VK_QUERY_RESULT_64_BIT);
    const double ms = (double)(t[1] - t[0]) * (double)ctx.getCaps().mTimestampPeriodNs / 1.0e6;
    if (offscreen)
    {
        sBench.mVkB += ms;
        ++sBench.mVkBN;
    }
    else
    {
        sBench.mVkA += ms;
        ++sBench.mVkAN;
        if (sBench.mPhaseActive && sBench.mPhaseUsed > 0)
        {
            std::vector<U64> stamps(sBench.mPhaseUsed + 1);
            LLVk::vkGetQueryPoolResults(ctx.getDevice(), sBench.mPhasePool, 0, sBench.mPhaseUsed + 1, stamps.size() * sizeof(U64), stamps.data(), sizeof(U64), VK_QUERY_RESULT_64_BIT);
            for (U32 k = 0; k < sBench.mPhaseUsed; ++k)
            {
                sBench.mKindMs[sBench.mPhaseKind[k]] += (double)(stamps[k + 1] - stamps[k]) * (double)ctx.getCaps().mTimestampPeriodNs / 1.0e6;
            }
            ++sBench.mPhaseFrames;
        }
        sBench.mPhaseActive = false;
    }
    sBench.mHook += LLVkGL::take_hook_ms();
}

const wchar_t* const MIRROR_WINDOW_CLASS = L"LLVkMirrorWindow";
void* sMainWindowHandle = nullptr;        // LL_VK_NOGL: the viewer's main window (no GL context to find it from)

// The mirror never drops a frame by default: OpenGL moves on regardless, so a dropped frame would lose incremental work such as a
// reflection probe face. LL_VK_MIRROR_DROP=1 restores the non-blocking behaviour (frames are skipped while the GPU is behind).
bool mirror_blocks()
{
    static const bool blocks = getenv("LL_VK_MIRROR_DROP") == nullptr;
    return blocks;
}

struct MirrorState
{
    bool mRequestChecked = false;
    bool mRequested = false;
    bool mReady = false;
    bool mFailed = false;
    bool mInFrame = false;
    bool mHold = false;
    bool mOffscreen = false;        // the open frame has no swapchain image (work recorded after the present)

    HWND mWindow = nullptr;
    bool mOwnsWindow = true;        // false: the mirror presents into the viewer's main window (LL_VK_EMBED=main)
    HWND mParent = nullptr;         // LL_VK_EMBED: the viewer's main window, which the mirror window covers as a child
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
    U64 mPresentFailed = 0;
};

MirrorState sMirror;

LRESULT CALLBACK mirror_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        if (!sMirror.mParent)
        {
            sMirror.mClosed = true;     // shutdown() runs on the viewer's own thread at the next frame
        }
        return 0;
    case WM_NCHITTEST:
        if (sMirror.mParent)
        {
            return HTTRANSPARENT;       // the mouse belongs to the main window underneath
        }
        break;
    case WM_MOUSEACTIVATE:
        if (sMirror.mParent)
        {
            return MA_NOACTIVATE;
        }
        break;
    case WM_ERASEBKGND:
        if (sMirror.mParent)
        {
            return 1;
        }
        break;
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
    // LL_VK_EMBED=main: present into the viewer's main window (the one the current GL context draws to); needs LL_VK_OWN so OpenGL stops swapping over it.
    // LL_VK_EMBED=child: cover that window with a child window instead (the main window then stops receiving the mouse; not usable).
    HWND parent = nullptr;
    const char* embed = getenv("LL_VK_EMBED");
    if (embed != nullptr)
    {
        if (sMainWindowHandle)
        {
            parent = (HWND)sMainWindowHandle;       // no OpenGL context (LL_VK_NOGL): the window code told us
        }
        else if (HDC dc = wglGetCurrentDC())
        {
            parent = WindowFromDC(dc);
        }
        if (parent && strcmp(embed, "main") == 0)
        {
            if (getenv("LL_VK_OWN") != nullptr)
            {
                sMirror.mWindow = parent;
                sMirror.mOwnsWindow = false;
                LLWindow::sSkipSwapBuffers = true;
                LL_INFOS("VkMirror") << "the Vulkan mirror presents into the viewer's main window (LL_VK_EMBED=main)" << LL_ENDL;
                return true;
            }
            LL_WARNS("VkMirror") << "LL_VK_EMBED=main needs LL_VK_OWN; using a separate mirror window" << LL_ENDL;
            parent = nullptr;
        }
        else if (strcmp(embed, "child") != 0)
        {
            parent = nullptr;
        }
    }
    if (parent)
    {
        RECT rc = {};
        GetClientRect(parent, &rc);
        sMirror.mParent = parent;
        sMirror.mWindow = CreateWindowExW(0, MIRROR_WINDOW_CLASS, L"Firestorm Vulkan mirror", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                          0, 0, std::max<LONG>(rc.right, 64), std::max<LONG>(rc.bottom, 64), parent, nullptr, inst, nullptr);
        if (sMirror.mWindow)
        {
            LL_INFOS("VkMirror") << "the mirror window is a child of the viewer's main window (LL_VK_EMBED)" << LL_ENDL;
            return true;
        }
        sMirror.mParent = nullptr;
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
    ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)sMirror.mBackbuffer.mImage, "mirror backbuffer color");
    ctx.setObjectName(VK_OBJECT_TYPE_IMAGE, (U64)sMirror.mDepth.mImage, "mirror backbuffer depth");
    LL_INFOS("VkMirror") << "mirror backbuffer " << width << "x" << height << LL_ENDL;
    return true;
}

} // anonymous namespace

void LLVkMirror::setPresentPreference(S32 mode, bool vsync_on)
{
    LLVk::setPresentPreference((int)mode, vsync_on);
}

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

LLVkMirror::Mutex& LLVkMirror::lock()
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

void LLVkMirror::setMainWindow(void* hwnd)
{
    sMainWindowHandle = hwnd;
}

bool LLVkMirror::describeDevice(std::string& vendor, std::string& renderer, std::string& version, U32& vram_mb)
{
    if (!ensureReady())
    {
        return false;
    }
    const LLVk::Capabilities& caps = LLVk::Context::instance().getCaps();
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
    vram_mb = (U32)(LLVk::Context::instance().deviceLocalBytes() / (1024 * 1024));
    return true;
}

bool LLVkMirror::ensureReady()
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
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
    LLVkGL::gOwn = getenv("LL_VK_OWN") != nullptr;
    if (LLVkGL::gOwn)
    {
        LL_INFOS("VkMirror") << "LL_VK_OWN: the mirror owns textures, buffers, framebuffers, vertex arrays, queries and fences; OpenGL no longer executes them or the draws. The main window stays blank, the picture is in the mirror window." << LL_ENDL;
    }
    bench_init();
    LL_INFOS("VkMirror") << "Vulkan mirror ready" << LL_ENDL;
    return true;
}

void LLVkMirror::stopAsync()
{
    LLVkAsync::stop();
}

void LLVkMirror::shutdown()
{
    LLVkAsync::stop();
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    if (LLVkGL::gOwn)
    {
        LL_WARNS("VkMirror") << "the mirror shut down while it owned the resources: nothing can be drawn any more" << LL_ENDL;
    }
    LLVkGL::gActive = false;
    LLVkGL::gOwn = false;
    LLVkVerify::gEnabled = false;
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
        if (sMirror.mOwnsWindow)
        {
            DestroyWindow(sMirror.mWindow);
        }
        sMirror.mWindow = nullptr;
        sMirror.mParent = nullptr;
        sMirror.mOwnsWindow = true;
        LLWindow::sSkipSwapBuffers = false;
    }
    sMirror.mClosed = false;
}

void LLVkMirror::beginFrameImpl(S32 width, S32 height)
{
    gMainThreadId = __readgsdword(0x48);
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    if (glonly_mode())
    {
        bench_init();
        bench_begin_iteration();
        return;
    }
    if (LLVk::gDeviceLost && !sMirror.mFailed)
    {
        LL_WARNS("VkMirror") << "the Vulkan device was lost; switching the mirror off, the viewer carries on with OpenGL" << LL_ENDL;
        shutdown();
        sMirror.mFailed = true;
        return;
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

    if (!sMirror.mOwnsWindow && sMirror.mWindow)
    {
        // presenting into the main window: nobody tells the mirror when it is resized
        RECT rc = {};
        GetClientRect(sMirror.mWindow, &rc);
        {
            // LL_VK_NUDGE (experiment): once the size has stopped changing, poke the compositor: r = repaint the window, c = recreate the swapchain, d = DwmFlush
            static const char* nudge = getenv("LL_VK_NUDGE");
            static S32 prev_w = -1, prev_h = -1;
            static ULONGLONG changed_at = 0;
            static bool pending = false;
            const ULONGLONG now_ms = GetTickCount64();
            if (rc.right != prev_w || rc.bottom != prev_h)
            {
                prev_w = rc.right; prev_h = rc.bottom;
                changed_at = now_ms;
                pending = true;
            }
            else if (nudge && pending && now_ms - changed_at >= 200)
            {
                pending = false;
                if (strchr(nudge, 'r'))
                {
                    RedrawWindow(sMirror.mWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
                }
                if (strchr(nudge, 'c'))
                {
                    sMirror.mResized = true;
                }
                if (strchr(nudge, 'd'))
                {
                    typedef HRESULT(WINAPI * DwmFlushFn)();
                    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
                    {
                        if (DwmFlushFn flush = (DwmFlushFn)GetProcAddress(dwm, "DwmFlush")) flush();
                    }
                }
                LL_INFOS("VkSize") << "nudged the window after the resize settled (" << nudge << ")" << LL_ENDL;
            }
        }
        static const bool size_log = getenv("LL_VK_SIZELOG") != nullptr;
        if (size_log)
        {
            static ULONGLONG next_stats = 0;
            const ULONGLONG now_ms = GetTickCount64();
            if (now_ms >= next_stats)
            {
                next_stats = now_ms + 5000;
                LL_INFOS("VkSize") << "mirror frames " << sMirror.mFrames << ", skipped " << sMirror.mSkipped << ", presents refused " << sMirror.mPresentFailed << LL_ENDL;
            }
            static S32 last_sizes[6] = {};
            const S32 now[6] = { width, height, (S32)rc.right, (S32)rc.bottom, (S32)sMirror.mSwapchain.getExtent().width, (S32)sMirror.mSwapchain.getExtent().height };
            if (memcmp(now, last_sizes, sizeof(now)) != 0)
            {
                memcpy(last_sizes, now, sizeof(now));
                LL_INFOS("VkSize") << "beginFrame: viewer window " << width << "x" << height << ", client area " << rc.right << "x" << rc.bottom << ", swapchain "
                                   << now[4] << "x" << now[5] << ", backbuffer " << sMirror.mBackWidth << "x" << sMirror.mBackHeight << LL_ENDL;
            }
        }
        const LLVk::Swapchain& swap = sMirror.mSwapchain;
        if (swap.isValid() ? ((U32)rc.right != swap.getExtent().width || (U32)rc.bottom != swap.getExtent().height) : (rc.right > 0 && rc.bottom > 0))
        {
            sMirror.mResized = true;
        }
    }
    if (sMirror.mParent && sMirror.mWindow)
    {
        // an embedded mirror window follows the size of the main window
        RECT parent_rc = {}, mine = {};
        GetClientRect(sMirror.mParent, &parent_rc);
        GetClientRect(sMirror.mWindow, &mine);
        if (parent_rc.right > 0 && parent_rc.bottom > 0 && (parent_rc.right != mine.right || parent_rc.bottom != mine.bottom))
        {
            SetWindowPos(sMirror.mWindow, HWND_TOP, 0, 0, parent_rc.right, parent_rc.bottom, SWP_NOACTIVATE);
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

    bench_begin_iteration();

    // never stall the viewer: if the GPU is behind, this frame simply is not mirrored
    VkCommandBuffer cmd = ctx.beginFrame(mirror_blocks() ? true : false);
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
        acquired = sc.acquire(ctx.getAcquireSemaphore(), image_index, LLVkGL::gOwn ? 250ull * 1000 * 1000 : 0ull);
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
    bench_vk_begin(cmd, false);
    LLVk::Image* colors[1] = { &sMirror.mBackbuffer };
    renderer.setRenderTarget(colors, 1, &sMirror.mDepth);
    sMirror.mInFrame = true;
}

void LLVkMirror::armSnapshotCompareImpl()
{
    // the GL-versus-Vulkan snapshot comparison went with OpenGL
}

void LLVkMirror::holdFrameImpl(bool hold)
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
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
    VkCommandBuffer cmd = ctx.beginFrame(mirror_blocks() ? true : false);
    if (cmd == VK_NULL_HANDLE)
    {
        ++sMirror.mSkipped;
        ctx.setNextFrameOffscreen(false);
        return false;
    }
    renderer.beginFrame(cmd, ctx.getFrameSlot());
    bench_vk_begin(cmd, true);
    LLVk::Image* colors[1] = { &sMirror.mBackbuffer };
    renderer.setRenderTarget(colors, 1, &sMirror.mDepth);
    sMirror.mInFrame = true;
    sMirror.mOffscreen = true;
    return true;
}
}

void LLVkMirror::Mutex::lockSlow(unsigned long me)
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

const char* LLVkMirror::phaseName(int id)
{
    return id >= 0 && id < PH_COUNT ? PHASE_NAMES[id] : "?";
}

void LLVkMirror::snapshotBoundaryImpl(S32 mode)
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    if (mode <= 0 || !sMirror.mReady || !sMirror.mInFrame)
    {
        return;
    }
    LL_INFOS("VkSnap") << "snapshot boundary, variant " << mode << (sMirror.mOffscreen ? " (off-screen frame)" : " (present frame)") << LL_ENDL;
    if (mode == 1)
    {
        LLVk::Renderer::instance().flushAndRestart();
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

void LLVkMirror::presentFrameImpl()
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    if (glonly_mode())
    {
        bench_mark_present();
        bench_gl_stamp(1);
        bench_gl_stamp(2);
        return;
    }
    if (!sMirror.mReady || !sMirror.mInFrame || sMirror.mOffscreen)
    {
        return;
    }
    bench_mark_present();
    bench_gl_stamp(1);
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

void LLVkMirror::endFrameImpl()
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    if (glonly_mode())
    {
        const U32 s = sBench.mIter % 4;
        if (!sMirror.mHold && (sBench.mGlMask[s] & 4) && !(sBench.mGlMask[s] & 8))
        {
            bench_gl_stamp(3);
        }
        return;
    }
    if (!sMirror.mReady || !sMirror.mInFrame || sMirror.mHold)
    {
        return;
    }
    sMirror.mInFrame = false;

    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::Renderer& renderer = LLVk::Renderer::instance();
    LLVk::Swapchain& sc = sMirror.mSwapchain;
    const U32 index = sMirror.mImageIndex;

    if (sMirror.mOffscreen)
    {
        sMirror.mOffscreen = false;
        renderer.endFrame();
        bench_gl_stamp(3);
        bench_vk_end(ctx.getCommandBuffer());
        ctx.endFrame(VK_NULL_HANDLE, VK_NULL_HANDLE);
        bench_vk_collect(true);
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
    bench_vk_end(ctx.getCommandBuffer());
    ctx.endFrame(ctx.getAcquireSemaphore(), sc.getRenderDone(index));
    if (!sc.present(index, sc.getRenderDone(index)))
    {
        ++sMirror.mPresentFailed;
        if (sc.lastPresentOutOfDate() && !sMirror.mOwnsWindow)
        {
            ctx.waitIdle();
            represent_after_resize();
        }
    }
    bench_vk_collect(false);
    bench_gl_stamp(2);
    ++sMirror.mFrames;
    LLVkGL::after_frame();
}

bool LLVkMirror::benchPhasesActive()
{
    return sBench.mPhaseActive;
}

void LLVkMirror::benchPass(const char* name)
{
    const U32 kind = bench_kind_of(name);
    if ((int)kind == sBench.mCurrentKind || sBench.mPhaseUsed + 2 >= Bench::PHASE_QUERIES)
    {
        return;
    }
    LLVk::Context& ctx = LLVk::Context::instance();
    LLVk::vkCmdWriteTimestamp2(ctx.getCommandBuffer(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, sBench.mPhasePool, sBench.mPhaseUsed);
    sBench.mPhaseKind[sBench.mPhaseUsed] = kind;
    ++sBench.mPhaseUsed;
    sBench.mCurrentKind = (int)kind;
}

// ---- the public frame calls: queued for the worker thread in async mode (LL_VK_ASYNC), inline otherwise ----
namespace
{
// The worker takes over between two frames: once a frame has been closed in the usual way and the mirror is ready.
void maybe_start_async()
{
    if (!LLVkAsync::gEnabled && LLVkAsync::requested() && sMirror.mReady && !sMirror.mInFrame && !sMirror.mHold && LLVkGL::gOwn && !LLVkVerify::gEnabled)
    {
        LLVkAsync::start();
    }
}
}

void LLVkMirror::beginFrame(S32 width, S32 height)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameBegin(width, height);
        return;
    }
    beginFrameImpl(width, height);
}

void LLVkMirror::endFrame()
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameEnd();
        return;
    }
    endFrameImpl();
    maybe_start_async();
}

void LLVkMirror::presentFrame()
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::framePresent();
        return;
    }
    presentFrameImpl();
}

void LLVkMirror::holdFrame(bool hold)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::frameHold(hold);
        return;
    }
    holdFrameImpl(hold);
}

void LLVkMirror::snapshotBoundary(S32 mode)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::snapshotBoundary(mode);
        return;
    }
    snapshotBoundaryImpl(mode);
}

void LLVkMirror::armSnapshotCompare()
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::armSnapshot();
        return;
    }
    armSnapshotCompareImpl();
}

void LLVkMirror::setUpscaleLodBias(float bias)
{
    LLVkDlss::setLodBias(bias);
}

bool LLVkMirror::upscaleAvailable()
{
    return sMirror.mReady && LLVkDlss::ready();
}

void LLVkMirror::upscale(const UpscaleArgs& args)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::push(LLVkAsync::CMD_UPSCALE, &args, sizeof(args));
        return;
    }
    upscaleImpl(args);
}

void LLVkMirror::upscaleImpl(const UpscaleArgs& args)
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    LLVkGL::on_upscale(args);
}

namespace LLVkGL
{
    void on_shadow_packets(const LLVkMirror::ShadowPacketsHeader& header, const U8* blob);
}

bool LLVkMirror::shadowPacketsAvailable()
{
    return sMirror.mReady && LLVk::Context::instance().getCaps().mBufferDeviceAddress;
}

bool LLVkMirror::shadowAlphaPacketsAvailable()
{
    return shadowPacketsAvailable() && LLVk::Context::instance().getCaps().mBindlessSlots != 0;
}

void LLVkMirror::shadowPackets(const ShadowPacketsHeader& header, const void* blob, size_t blob_bytes)
{
    if (LLVkAsync::gEnabled)
    {
        LLVkAsync::pushData(LLVkAsync::CMD_SHADOW_PACKETS, &header, sizeof(header), blob, blob_bytes);
        return;
    }
    shadowPacketsImpl(header, (const U8*)blob);
}

void LLVkMirror::shadowPacketsImpl(const ShadowPacketsHeader& header, const U8* blob)
{
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    LLVkGL::on_shadow_packets(header, blob);
}

#endif // LL_VULKAN
