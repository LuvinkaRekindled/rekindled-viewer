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

    // LL_VK_NOGL: the viewer has no OpenGL context, so the main window is given here (once it exists) and the picture is presented into it
    void setMainWindow(void* hwnd);
    // What the viewer needs to know about the graphics device, in plain types (C6): identity strings in the form the viewer parses, the limits its feature
    // code reads (values as a GL driver of this class reports them), the measured memory bandwidth and the ray tracing / DLSS capabilities.
    struct DeviceInfo
    {
        std::string mVendor, mRenderer, mVersion;
        U32 mVramMb = 0;
        S32 mMaxTextureSize = 0, mTextureImageUnits = 0, mMaxSamples = 1, mMaxColorTextureSamples = 1, mMaxDepthTextureSamples = 1, mMaxIntegerSamples = 1;
        S32 mMaxSampleMaskWords = 1, mMaxVaryingVectors = 15, mMaxUniformBlockSize = 0, mMaxVertexRange = 0, mMaxIndexRange = 0;
        F32 mMaxAnisotropy = 1.f;
        F32 mMemoryBandwidthGBps = 0.f;     // measured; 0 = not measured
        bool mAccelStruct = false, mRayQuery = false, mRayTracingPipeline = false, mRayTracingUsable = false;
        bool mRayTracingEnabled = false;    // switched on at device creation (LL_VK_RT=1)
        bool mRayReconstruction = false;    // DLSS ray reconstruction can run (needs DLSS on and its DLL)
    };
    // The device the viewer renders with, starting Vulkan if needed. False if Vulkan cannot be started.
    bool describeDevice(DeviceInfo& out);

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
    // the texture mip bias DLSS wants for the current render scale (log2 of it, 0 at 100% or with DLSS off); samplers made afterwards use it
    void setUpscaleLodBias(float bias);

    // ---- shadow packets (render packets) ----
    // One record that draws a whole pass of static, untextured shadow casters. For every draw: the GL names of its vertex and index buffer, its index range and the
    // index of its model matrix. The worker reads the vertices and indices from the buffers by address (shadowPullV.glsl): nothing is bound between the draws.
    struct ShadowPacketsHeader
    {
        U32 mEntries = 0;           // draws, as ShadowPacketEntry after the header
        U32 mMatrices = 0;          // model matrices (16 floats each, column-major) after the entries; matrix 0 is the identity
        F32 mViewProj[16] = {};     // projection * view of the pass, column-major
        U32 mKind = 0;              // 0 = untextured casters (ShadowPacketEntry), 1 = alpha masked casters (ShadowAlphaEntry), 2 = rigged untextured casters (ShadowSkinEntry)
        F32 mTargetWidth = 0.f;     // kind 1: the width of the shadow target (the dithering of semi-transparent casters)
        U32 mPaletteFloats = 0;     // floats of joint matrix palettes (12 per joint) after the model matrices; the rigged entries point into them
        U32 mPass = 0;              // the shadow pass: 0-3 sun cascades, 4-5 spot shadows, 6 other (ray tracing keeps the casters of a cached cascade)
    };
    struct ShadowPacketEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mMatrixId; };      // mMatrixId: stable identity of the model matrix (ray tracing instances)
    // kind 2: a rigged draw: the offset of its weight4 array in the vertex buffer, and its palette (offset in floats from the start of the palettes, joint count)
    // (ray tracing: the vertex range the draw uses, the avatar it belongs to and the identity of its skin, both stable from frame to frame)
    struct ShadowSkinEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mWeightOffset, mPaletteOffset, mJoints, mVertexFirst, mVertexCount, mAvatarId, mSkinId; };
    // kind 1: the same, with what the alpha test needs
    enum ShadowAlphaFlags : U32
    {
        ALPHA_FULLBRIGHT = 1,       // the vertex colour's alpha does not count
        ALPHA_TREE = 2,             // tree style: no vertex alpha, no dithering (grass and the materials' masks)
        ALPHA_NO_INDEX = 4,         // one texture, mTextures[0]: the vertices carry no texture index
        ALPHA_HAS_TEXCOORD = 8,     // mTexcoordOffset is valid
        ALPHA_HAS_COLOR = 16,       // mColorOffset is valid
        ALPHA_RIGGED = 32           // mWeightOffset, mPaletteOffset and mJoints are valid
    };
    struct ShadowAlphaEntry
    {
        U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix;
        U32 mMatrixId;              // stable identity of the model matrix (ray tracing instances)
        U32 mFlags;                 // ShadowAlphaFlags
        U32 mTexcoordOffset;        // byte offset of the texcoord0 array in the vertex buffer
        U32 mColorOffset;           // byte offset of the colour array
        U32 mTextures[4];           // GL names of the textures the vertices' texture index picks from, 0 = none (white)
        F32 mCutoff;                // alpha below this is discarded
        U32 mWeightOffset, mPaletteOffset, mJoints;     // rigged draws, as ShadowSkinEntry
        U32 mVertexFirst, mVertexCount, mAvatarId, mSkinId;     // rigged draws, as ShadowSkinEntry (ray tracing)
    };
    // kind 4 (ray tracing only, nothing is drawn): one mesh of an avatar's own body, as the old path draws it. mMode 1: skinned by the palette (15 joints, 12 floats each, after
    // the entries; one weight float per vertex at mWeightOffset picks joint i and i + 1), 2: rigid under mModel (column-major, the joint's world matrix)
    struct ShadowBodyEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mVertexFirst, mVertexCount, mAvatarId, mMeshId, mWeightOffset, mJoints, mMode; F32 mAvatarPos[3]; F32 mModel[16]; };
    // kind 3: a draw of a deferred geometry pass (the colour passes). The matrices after the entries are 44 floats each: modelview, modelview * projection, and the normal
    // matrix as three vec4 columns (the viewer computes them, once per model matrix). Matrix 0 is the view alone.
    enum GeoFlags : U32
    {
        GEO_ONE_TEXTURE = 1         // one diffuse texture, mTextures[0]: the vertices carry no texture index
    };
    struct GeoEntry
    {
        U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix;
        U32 mFlags;                 // GeoFlags
        U32 mNormalOffset, mTexcoordOffset, mColorOffset;      // byte offsets of the arrays in the vertex buffer
        U32 mTextures[4];           // GL names of the diffuse textures the vertices' texture index picks from, 0 = none (white)
        U32 mNormalMap, mSpecularMap;   // GL names (materials passes), 0 = none
        F32 mSpecular[4];
        F32 mEnvIntensity, mCutoff, mBrightness, mPad;
        U32 mTangentOffset, mTexcoord1Offset, mTexcoord2Offset;     // materials passes: byte offsets of the tangent and the texcoord 1 and 2 arrays
    };
    // ---- ray query shadows (LL_VK_RT_SHADOWS, doc/vulkan/PLAN-rt-shadows.md) ----
    // 0 = off (or the device cannot), 1 = ray query shadows replace the cascades, 2 = split screen, 3 = disagreement map. Fixed for the run; the shader programs are compiled for it.
    U32 rtShadowMode();
    // Builds the scene the shadow passes of this frame named (their casters) for the camera view: bottom-level structures as the budget allows, then the top-level structure in
    // eye space. Call before the lighting that samples the sun shadow. view: the camera's view matrix, column-major.
    struct RtSceneArgs { F32 mView[16] = {}; };
    void rtScene(const RtSceneArgs& args);
    // the device can read buffers by address and the mirror is running
    bool shadowPacketsAvailable();
    // ... and the device can index sampled images at run time (the textured kind)
    bool shadowAlphaPacketsAvailable();
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
    void upscaleImpl(const UpscaleArgs& args);
    void shadowPacketsImpl(const ShadowPacketsHeader& header, const U8* blob);
    void rtSceneImpl(const RtSceneArgs& args);
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

#if defined(LL_VULKAN)
namespace LLVkGL
{
    // Arena phase 3: where a GL buffer name lives. The address stays valid while epoch is unchanged; version moves on every write. Worker thread only (it owns the buffer table).
    struct BufferInfo
    {
        unsigned long long mAddress = 0;     // device address of the first byte (0: no storage, or not in the arena)
        unsigned long long mSize = 0;        // bytes of data (the viewer's pool may make the buffer up to 12.5% larger than its content)
        unsigned long long mEpoch = 0;       // changes when the name gets other contents or another owner (re-specified, deleted, recycled by the vertex buffer pool)
        unsigned long long mVersion = 0;     // changes on every write
        unsigned long long mMoves = 0;       // changes when the same contents move to another range (the address changed, nothing else)
        bool mDevice = false;                // video memory written by GPU copies (false: written by the CPU)
        const void* mMapped = nullptr;       // the mapped video memory range itself (slow to read from the CPU; debugging only)
        const void* mCpu = nullptr;          // the CPU's pointer to the data when it lives in host-written memory (valid until the buffer is written or freed), else null
    };
    bool getBufferInfo(unsigned int name, BufferInfo& out);
    // the GPU reads this buffer in the current frame (an acceleration structure build, a ray query shader): a CPU rewrite of a host-written range has to rename it first
    void markBufferUsed(unsigned int name);
}
#endif

#endif // LL_LLVKMIRROR_H
