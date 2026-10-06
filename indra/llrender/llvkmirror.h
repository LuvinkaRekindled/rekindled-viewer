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
#include <atomic>
#include <intrin.h>
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

    // Present mode (setting FSVkPresentMode with the Vertical Sync preference): 0 follow Vertical Sync, 1 vsync, 2 uncapped, 3 uncapped without tearing.
    // Safe to call at any time; the swapchain follows at the start of the next frame.
    void setPresentPreference(S32 mode, bool vsync_on);

    // A snapshot is about to render: with LL_VK_VERIFY set, compare each of its passes between GL and Vulkan and report on it afterwards
    void armSnapshotCompare();
    // Snapshot experiment (setting FSVkSnapVariant): called before each display() a snapshot makes.
    // 1 = flush what has been recorded and carry on, 2 = close the off-screen frame and open a new one, 3 = like 2 but also wait for the GPU.
    void snapshotBoundary(S32 mode);

    // ---- DLSS upscaling (setting FSVkDLSS; see llvkdlss.h) ----
    // The GL names of four textures and what the upscaler needs to know about them; sizes are in pixels.
    struct UpscaleArgs
    {
        U32 mColor = 0, mDepth = 0, mMotion = 0, mOutput = 0;
        U32 mRenderWidth = 0, mRenderHeight = 0, mOutputWidth = 0, mOutputHeight = 0;
        F32 mJitterX = 0.f, mJitterY = 0.f;             // this frame's jitter in render pixels (+x right, +y down)
        F32 mMotionScaleX = 1.f, mMotionScaleY = 1.f;
        S32 mReset = 0;                                 // the history is useless (camera cut)
        S32 mHdr = 0;                                   // the colour is linear HDR
    };
    // DLSS is initialised and usable (decided when the Vulkan device was created)
    bool upscaleAvailable();

    // ---- shadow packets (render packets) ----
    // One record that draws a whole pass of static, untextured shadow casters. For every draw: the GL names of its vertex and index buffer, its index range and the
    // index of its model matrix. The worker reads the vertices and indices from the buffers by address (shadowPullV.glsl): nothing is bound between the draws.
    struct ShadowPacketsHeader
    {
        U32 mEntries = 0;           // draws, as ShadowPacketEntry after the header
        U32 mMatrices = 0;          // model matrices (16 floats each, column-major) after the entries; matrix 0 is the identity
        F32 mViewProj[16] = {};     // projection * view of the pass, column-major
    };
    struct ShadowPacketEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mPad; };
    // the device can read buffers by address and the mirror is running
    bool shadowPacketsAvailable();
    // blob: mEntries ShadowPacketEntry, then mMatrices * 16 floats
    void shadowPackets(const ShadowPacketsHeader& header, const void* blob, size_t blob_bytes);
    // queues (async) or runs the upscale: colour + depth + motion -> output, all textures the mirror owns
    void upscale(const UpscaleArgs& args);

    // switch the mirror off (window closed, error); the viewer carries on with OpenGL only
    void shutdown();
    // plays what is queued for the mirror thread and stops it (the viewer is quitting); GL calls after this run inline
    void stopAsync();

#if defined(LL_VULKAN)
    // the inline implementations of the frame calls (the worker thread runs these in async mode)
    void beginFrameImpl(S32 width, S32 height);
    void endFrameImpl();
    void presentFrameImpl();
    void holdFrameImpl(bool hold);
    void snapshotBoundaryImpl(S32 mode);
    void armSnapshotCompareImpl();
    void upscaleImpl(const UpscaleArgs& args);
    void shadowPacketsImpl(const ShadowPacketsHeader& header, const U8* blob);
    // the lock every mirror entry point takes (the viewer calls GL from more than one thread)
    class Mutex;
    Mutex& lock();

    // The lock every hook runs under. The same thread takes it again and again (tens of thousands of times per frame), so the
    // uncontended path is a compare-exchange; a thread that has to wait spins briefly and then yields.
    class Mutex
    {
    public:
        void lock()
        {
            const unsigned long me = __readgsdword(0x48);       // the thread id, read from the thread environment block
            if (mOwner.load(std::memory_order_relaxed) == me)
            {
                ++mDepth;
                return;
            }
            unsigned long expected = 0;
            if (!mOwner.compare_exchange_strong(expected, me, std::memory_order_acquire))
            {
                lockSlow(me);
            }
            mDepth = 1;
        }
        void unlock()
        {
            if (--mDepth == 0)
            {
                mOwner.store(0, std::memory_order_release);
            }
        }
        // gives the lock up completely (whatever the nesting) and takes it back at the same depth later
        unsigned releaseAll()
        {
            const unsigned depth = mDepth;
            mDepth = 0;
            mOwner.store(0, std::memory_order_release);
            return depth;
        }
        void reacquire(unsigned depth)
        {
            lock();
            mDepth = depth;
        }
    private:
        void lockSlow(unsigned long me);
        std::atomic<unsigned long> mOwner{ 0 };
        unsigned mDepth = 0;
    };

    // the image the viewer's default framebuffer maps to (valid while ready)
    LLVk::Image* backbufferColor();
    LLVk::Image* backbufferDepth();

    // CPU phase timers of the translation layer (benchmark mode only; PhaseTimer costs one branch when off)
    enum Phase { PH_ALL_HOOKS, PH_UNIFORM, PH_DRAW, PH_D_TARGET, PH_D_STATE, PH_D_TEX, PH_D_UBO, PH_D_VERTEX, PH_D_APPLY, PH_D_STATS,
                 PH_A_TEX, PH_A_PASS, PH_A_PIPE, PH_A_DYN, PH_A_VI, PH_A_UNI, PH_A_PUSH, PH_A_CMD, PH_COUNT };
    extern long long gPhaseTicks[PH_COUNT];
    extern bool gPhaseTiming;
    extern unsigned long gMainThreadId;
    extern volatile int gLastLap;
    extern volatile unsigned long long gDbg[4];
    // which threads have used the Vulkan layer since the async worker started (printed by the worker's crash report)
    extern volatile unsigned gAsyncEpoch;
    extern volatile unsigned long gRendererTids[8];
    extern volatile unsigned gRendererTidCount;
    void noteRendererThreadSlow();
    inline void noteRendererThread()
    {
        static thread_local unsigned seen_epoch = 0;
        if (seen_epoch != gAsyncEpoch)
        {
            seen_epoch = gAsyncEpoch;
            noteRendererThreadSlow();
        }
    }               // where the draw path last got to (printed by the async worker's crash report)         // set by beginFrame: the thread that draws the viewer's frames
    inline bool onMainThread() { return __readgsdword(0x48) == gMainThreadId; }
    long long phaseNow();       // performance counter ticks
    const char* phaseName(int id);
    // adds the time since 't' to a phase and restarts the lap
    inline void phaseLap(int id, long long& t)
    {
        if (gPhaseTiming)
        {
            const long long n = phaseNow();
            gPhaseTicks[id] += n - t;
            t = n;
            gLastLap = id;
        }
    }
    struct PhaseTimer
    {
        int mId;
        long long mStart = 0;
        explicit PhaseTimer(int id) : mId(gPhaseTiming && onMainThread() ? id : -1) { if (mId >= 0) mStart = phaseNow(); }
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
