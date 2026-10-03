/**
 * @file llvkmirror.h
 * @brief Vulkan "mirror" mode: OpenGL stays the renderer of record while the llrender classes replay what they do
 *        into the Vulkan backend, which renders the same frame into its own window.
 *
 * Enabled by the environment variable LL_VK_MIRROR=1 in a build made with -DUSE_VULKAN=ON. Everything here is a
 * no-op otherwise. Mirror calls made outside a mirrored frame are ignored (uploads still work), and any failure
 * switches the mirror off without affecting the OpenGL viewer.
 */

#ifndef LL_LLVKMIRROR_H
#define LL_LLVKMIRROR_H

#include "linden_common.h"

#if defined(LL_VULKAN)
#include <mutex>
namespace LLVk { struct Image; }
#endif

namespace LLVkMirror
{
    // mirror mode was requested (environment) and this build supports it
    bool isRequested();

    // true once Vulkan is initialised and the mirror has not been switched off
    bool isReady();

    // initialises Vulkan lazily on first use; returns isReady()
    bool ensureReady();

    // true between beginFrame() and endFrame() of a frame that is being recorded
    bool inFrame();

    // Call once at the start of each viewer frame with the size of the viewer's window
    void beginFrame(S32 width, S32 height);

    // Call just before the viewer swaps buffers: blits the mirrored backbuffer to the mirror window and presents
    void endFrame();

    // Call where the viewer swaps buffers. Presents the mirrored backbuffer now even while the frame is held; whatever the
    // viewer still renders until the frame is released (probe updates clear and reuse the default framebuffer) is recorded in
    // a second, off-screen frame so it cannot disturb the image that was just presented.
    void presentFrame();

    // While held, endFrame() does nothing and a nested beginFrame() joins the open frame. The main loop holds the frame
    // across display() and the work that follows it (reflection probe updates, snapshots), then releases it and ends it.
    void holdFrame(bool hold);

    // A snapshot is about to render: with LL_VK_VERIFY set, compare each of its passes between GL and Vulkan and report on it afterwards
    void armSnapshotCompare();
    // Snapshot experiment (setting FSVkSnapVariant): called before each display() a snapshot makes.
    // 1 = flush what has been recorded and carry on, 2 = close the off-screen frame and open a new one, 3 = like 2 but also wait for the GPU.
    void snapshotBoundary(S32 mode);

    // switch the mirror off (window closed, error); the viewer carries on with OpenGL only
    void shutdown();

#if defined(LL_VULKAN)
    // the lock every mirror entry point takes (the viewer calls GL from more than one thread)
    std::recursive_mutex& lock();

    // the image the viewer's default framebuffer maps to (valid while ready)
    LLVk::Image* backbufferColor();
    LLVk::Image* backbufferDepth();

    // CPU phase timers of the translation layer (benchmark mode only; PhaseTimer costs one branch when off)
    enum Phase { PH_ALL_HOOKS, PH_UNIFORM, PH_DRAW, PH_D_TARGET, PH_D_STATE, PH_D_TEX, PH_D_UBO, PH_D_VERTEX, PH_D_APPLY, PH_D_STATS, PH_COUNT };
    extern long long gPhaseTicks[PH_COUNT];
    extern bool gPhaseTiming;
    long long phaseNow();       // performance counter ticks
    struct PhaseTimer
    {
        int mId;
        long long mStart = 0;
        explicit PhaseTimer(int id) : mId(gPhaseTiming ? id : -1) { if (mId >= 0) mStart = phaseNow(); }
        ~PhaseTimer()
        {
            if (mId >= 0)
            {
                gPhaseTicks[mId] += phaseNow() - mStart;
            }
        }
    };

    // benchmark (LL_VK_MIRROR_BENCH): true while the present frame is recording per-pass-kind GPU timestamps
    bool benchPhasesActive();
    // called before each mirrored draw while benchPhasesActive(): notes which kind of pass the draw belongs to
    void benchPass(const char* program_name);
#endif
}

#endif // LL_LLVKMIRROR_H
