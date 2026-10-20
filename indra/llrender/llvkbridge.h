/**
 * @file llvkbridge.h
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
#include <vector>
#include <intrin.h>
namespace LLVk { struct Image; }
#endif

namespace LLVkBridge
{
    // true once Vulkan is initialised and the mirror has not been switched off
    bool isReady();
    // video memory as the OS sees this process (what it uses, what it may use before the driver pages allocations out), for the viewer's texture budget; false when no Vulkan device
    bool videoMemory(U64& usage_bytes, U64& budget_bytes);
    // false: reading a texture back now would make the main thread wait for a render worker that is far behind (or too many were made this frame): ask again in a later frame. True without the worker.
    bool readbackAffordable();

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
        // ray reconstruction instead of super resolution: the GL names of the guide textures (render size) and the model preset (0 default, 4 D, 5 E, 6 F)
        S32 mRayReconstruction = 0;
        U32 mDiffuseAlbedo = 0, mSpecularAlbedo = 0, mNormals = 0, mExposure = 0;
        U32 mReflectionMotion = 0;                      // render size RG16F: motion vectors of what the traced reflections show (pixels, previous minus current); 0 = none
        U32 mSpecularHitDistance = 0;                   // render size R16F: metres from the surface to what its reflection ray hit (0: not traced); 0 here = no such guide
        F32 mWorldToView[16] = {}, mViewToClip[16] = {};    // column-major, no jitter: only used with the hit distance
        S32 mPreset = 0;
    };
    // DLSS is initialised and usable (decided when the Vulkan device was created)
    bool upscaleAvailable();
    // DLSS ray reconstruction can run here (DLSS is ready and NGX reports the feature)
    bool rayReconstructionAvailable();
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
        U32 mFragment = 0;          // kind 11: the id of the fragment retained (S1, below)
    };
    // S1 retained fragments (doc/vulkan/S1-handoff.md): the untextured static casters of one spatial group, kept by the worker under an id until released.
    // kind 11 RETAIN: mFragment = id, mEntries ShadowPacketEntry in the blob; mMatrix of an entry is 0 (no matrix) or 1 + the fragment's own matrix number.
    // kind 12 RELEASE: mEntries ids (U32) in the blob.
    // kind 13 retained pass: like kind 0, but the entries are named by fragment: mEntries = fragments, mPaletteFloats = words of the matrix map; the blob is
    //   mEntries x { id, offset into the map }, the map (per fragment its own matrix number -> the number in this record's matrices), then mMatrices x 16 floats.
    // S2 resident fragments (the worker keeps their entries in a GPU table and culls them on the GPU; only fragments whose matrices are all region matrices):
    // kind 14 RETAIN_RESIDENT: mFragment = id, mEntries ShadowPacketEntry in the blob with mMatrix = the region matrix slot (0 = none), mViewProj[0..3] = bounding sphere (centre in region
    //   space, radius). Kind 12 RELEASE frees resident ones as well. kind 15 MATRICES: mMatrices x 16 floats, the region matrices by slot (slot 0 = identity). kind 16 DRAW_RESIDENT: one pass:
    //   mViewProj, mPass; the worker culls every resident entry against the pass's frustum on the GPU and draws the survivors; the blob is the 6 frustum planes (6 x 4 floats). Kind 17 NOTE_RESIDENT: the same pass for the ray scene (ray shadows only: nothing is drawn): the worker tests the resident fragments against the planes
    //   on the CPU and names the survivors to the ray scene (Rt::noteCaster) as a kind 5 record would; mPass, the blob as kind 16.
    // mMatrixId: stable identity of the model matrix (ray tracing instances). The last four are for the ray scene only: the material of the draw (RtMaterialDef id, 0 = none) and the byte
    // offsets of the texcoord0, colour and normal arrays in the vertex buffer (0 = the buffer has no such array)
    struct ShadowPacketEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mMatrixId, mMaterial, mTexcoordOffset, mColorOffset, mNormalOffset; };
    // kind 2: a rigged draw: the offset of its weight4 array in the vertex buffer, and its palette (offset in floats from the start of the palettes, joint count)
    // (ray tracing: the vertex range the draw uses, the avatar it belongs to and the identity of its skin, both stable from frame to frame)
    // ray scene materials (LL_VK_RT_MATERIALS): mMaterial = RtMaterialDef id of the draw (0 = none), mNormalOffset / mTexcoordOffset / mColorOffset = byte offsets of those arrays in the vertex
    // buffer (0 = none)
    struct ShadowSkinEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mWeightOffset, mPaletteOffset, mJoints, mVertexFirst, mVertexCount, mAvatarId, mSkinId, mAvatarKind; F32 mAvatarPos[3]; U32 mMaterial, mNormalOffset, mTexcoordOffset, mColorOffset; };
    // kind 1: the same, with what the alpha test needs
    enum ShadowAlphaFlags : U32
    {
        ALPHA_FULLBRIGHT = 1,       // the vertex colour's alpha does not count
        ALPHA_TREE = 2,             // tree style: no vertex alpha, no dithering (grass and the materials' masks)
        ALPHA_NO_INDEX = 4,         // one texture, mTextures[0]: the vertices carry no texture index
        ALPHA_HAS_TEXCOORD = 8,     // mTexcoordOffset is valid
        ALPHA_HAS_COLOR = 16,       // mColorOffset is valid
        ALPHA_RIGGED = 32,          // mWeightOffset, mPaletteOffset and mJoints are valid
        ALPHA_HARD = 64,            // ray tracing: a plain cut at mCutoff on texture alpha times vertex alpha (glTF alpha mask): no dithering
        ALPHA_RAY_ONLY = 128        // ray tracing: an opaque caster (no alpha test); kind 5 records only
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
        U32 mAvatarKind;            // rigged draws: what the avatar is (bit 1 animated object, 2 impostor, 4 the viewer's own, 8 jelly doll, 16 invisible, 32 too slow)
        F32 mAvatarPos[3];          // rigged draws: where the avatar is (ray tracing ranks and limits avatars by distance)
        F32 mUv[6];                 // kind 5: the texture coordinate transform as the ray query applies it: u' = mUv[0] * u + mUv[2] * v + mUv[4], v' = mUv[1] * u + mUv[3] * v + mUv[5]
        U32 mMaterial;              // ray tracing: RtMaterialDef id of the draw, 0 = none
        U32 mNormalOffset;          // ray tracing: byte offset of the normal array, 0 = none
    };
    // kind 10 (ray tracing only, nothing is drawn): mEntries RtMaterialDef rows to store (new materials, or ones whose texture names changed); the ids are the viewer's, stable for as long as a
    // material is in use. The draws name their material by id (mMaterial of the entries above). Texture names are GL names, resolved by the viewer; the worker turns them into bindless slots.
    enum RtMaterialFlags : U32
    {
        RTM_FULLBRIGHT = 1,         // no lighting: the colour is the surface
        RTM_GLTF = 2,               // the glTF fields are valid (factors, normal / metallic-roughness / emissive maps)
        RTM_MASK = 4,               // alpha masked: mCutoff
        RTM_BLEND = 8,              // alpha blended
        RTM_BATCH = 16,             // the vertex's texture index (position.w & 3) picks one of mTextures
        RTM_EMISSIVE_LEGACY = 32,   // legacy material with an emissive mask in the diffuse alpha
        RTM_NO_VERTEX_ALPHA = 64    // the alpha mask test uses the texture alpha alone (the legacy materials passes do not multiply the vertex colour alpha in, materialF.glsl)
    };
    struct RtMaterialDef
    {
        U32 mId, mFlags;
        U32 mTextures[4];           // diffuse / base colour textures (GL names, 0 = none = white)
        U32 mNormalMap, mSpecularMap, mEmissiveMap, mPad;      // GL names: normal; legacy specular or glTF metallic-roughness; glTF emissive
        F32 mColor[4];              // glTF base colour factor (legacy: 1, the colour is in the vertices)
        F32 mEmissive[3];           // glTF emissive factor
        F32 mCutoff;
        F32 mMetallic, mRoughness;
        F32 mUv[6];                 // texture coordinate transform, as ShadowAlphaEntry::mUv
        F32 mSpecular[4];           // legacy specular colour rgb and exponent
    };
    // kind 5 (ray tracing only, nothing is drawn): ShadowAlphaEntry entries (static draws) of casters that the old path draws, named to the ray scene; flags ALPHA_RAY_ONLY = opaque

    // kind 4 (ray tracing only, nothing is drawn): one mesh of an avatar's own body, as the old path draws it. mMode 1: skinned by the palette (15 joints, 12 floats each, after
    // the entries; one weight float per vertex at mWeightOffset picks joint i and i + 1), 2: rigid under mModel (column-major, the joint's world matrix)
    // mAlphaFlags: 0 = opaque, else an alpha masked part (hair, eyelashes, skirt) with the diffuse texture mTexture (GL name, 0 = none), the byte offset of its texcoord0 array and the cutoff: the same as ShadowAlphaFlags, tree style (no vertex alpha)
    // ray scene materials (LL_VK_RT_MATERIALS): mTexcoordOffset is set for every mesh that has texcoords (mAlphaFlags decides alone whether the alpha test runs), mNormalOffset = byte offset of
    // the normal array (0 = none), mMaterial = RtMaterialDef id of the mesh (0 = none)
    struct ShadowBodyEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mVertexFirst, mVertexCount, mAvatarId, mMeshId, mWeightOffset, mJoints, mMode, mSelf; F32 mAvatarPos[3]; F32 mModel[16]; U32 mAlphaFlags, mTexture, mTexcoordOffset; F32 mCutoff; U32 mNormalOffset, mMaterial; };
    // kinds 6 and 7 (ray tracing only, nothing is drawn): the rigged draws of the MAIN view named to the ray scene every frame, as kind 2 (ShadowSkinEntry) and kind 1 with
    // ALPHA_RIGGED (ShadowAlphaEntry): entries, model matrices, palettes, the layout of the kinds they copy.    // kind 8: rigged draws of the avatar velocity pass (avatarVelocityPullV.glsl): ShadowSkinEntry entries (no model matrices), then the palettes of this frame (mPaletteFloats floats, 12 per joint), then
    // the palettes of the frame before, same layout and offsets. mViewProj is the jittered view-projection (the pixels of the G-buffer); the unjittered ones come as ordinary uniforms
    // kind 9: draws that move as a whole, for the velocity pass (objectVelocityPullV.glsl): ShadowPacketEntry entries, then mMatrices model matrices of this frame, then mMatrices of the frame before
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
    // live switches (rt-debug.txt gate=, bodyname=), 1 by default: do not trace avatars still loading and masked casters whose texture has no data; name the body meshes of the last frames again
    bool rtLoadingGates();
    bool rtMaterialsAvailable();       // ray tracing is on and has the material table (the traced glossy mirror and the traced water reflections can run: the FSVkGlossyMirror / FSVkRTWater settings ask for them)
    S32 rtGlossyReflections();      // rt-debug.txt glossyrt=N (needs LL_VK_RT_MATERIALS=1): ray traced reflections on glossy surfaces of the lighting pass
    S32 rtWaterReflections();      // rt-debug.txt waterrt=1 (needs LL_VK_RT_MATERIALS=1): the water traces its reflection into the ray scene
    S32 rtHitRays();               // rt-debug.txt hitrays=N (-1 = not set): shadow rays per local light at a traced hit
    S32 rtHitQuality();            // rt-debug.txt hitq=N: how a hit of the traced glossy mirror is lit (0 flat diffuse, 1 the main pass lighting at the hit)
    S32 rtDebugView();      // rt-debug.txt view=N: the ray scene material debug view mode to show, 0 = the setting RenderRTMaterialDebug decides
    bool rtBodyRename();
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

    void setBindlessPersistent(bool on);        // live switch (FSBindlessPersistent): off gives every slot not used this frame back at once
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
                 PH_A_TEX, PH_A_PASS, PH_A_PIPE, PH_A_DYN, PH_A_VI, PH_A_SAMP, PH_A_UNI, PH_A_PUSH, PH_A_CMD, PH_COUNT };
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

#endif
}

#if defined(LL_VULKAN)
namespace LLVkWorker
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
    // records the queued writes of video-memory buffers (GPU copies) into the command stream now: the ray scene reads buffers by address (builds, skinning, alpha test), nothing else would flush them first
    void flushPendingBufferWrites();
    // counts the changes of where a watched buffer lives (the ray scene's alpha casters: their records hold the buffers' addresses): a new storage, a release, a rename
    unsigned long long bufferTopologyVersion();
    // the names of the watched buffers whose storage changed since the last call (what bufferTopologyVersion counts); false: too many to list, look at all of them
    bool takeTopologyChanges(std::vector<unsigned>& names);
    // Pinned textures (doc/vulkan/PLAN-rt-materials.md, "persistent bindless table"): the ray scene's rows name a texture by a PIN INDEX; the pin owns a bindless slot for as long as the
    // texture is in use (no per-frame lookup), the GPU reads index -> slot from a table (Rt::setPinSlot keeps it). Acquire / release count references of one GL name.
    unsigned int rtPinAcquire(unsigned int gl_name);
    void rtPinRelease(unsigned int pin_index);
    // once per scene call: the pins whose texture changed (image replaced, parameters, levels), the ones still waiting for their texture, and a slice of all of them checked again
    void rtPinUpdate();
    void rtPinCounts(unsigned long long& pins, unsigned long long& resolves, unsigned long long& events);
}
#endif

#endif // LL_LLVKMIRROR_H
