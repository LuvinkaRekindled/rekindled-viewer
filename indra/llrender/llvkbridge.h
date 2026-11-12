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
#include "llvkplatform.h"
namespace LLVk { struct Image; }
#endif

namespace LLVkBridge
{
    // the deferred geometry pass types the scene table serves (kGeoTypes in pipeline.cpp, the viewer's list of them, has this many): item type t is a static pass type, kGeoPassTypes + t its rigged variant,
    // 2 * kGeoPassTypes the static alpha items and 2 * kGeoPassTypes + 1 the rigged alpha items
    constexpr unsigned kGeoPassTypes = 26;       // (J1 K4: + fullbright shiny, bump, post bump; J1 K6 S4: + grass, which has no rigged variant; J1 K6 B1: + the invisible alpha draws, drawn by the Highlight Transparent view only)
    // true once Vulkan is initialised and the mirror has not been switched off
    bool isReady();
    // <FS> LL_VK_SYNC_LOG: names an address in the report of the main thread's waits on the worker (the viewer's symbol lookup)
    void setSymbolizer(std::string (*fn)(const void*));
    // video memory as the OS sees this process (what it uses, what it may use before the driver pages allocations out), for the viewer's texture budget; false when no Vulkan device
    bool videoMemory(U64& usage_bytes, U64& budget_bytes);
    // where the video memory goes besides the textures (doc/vulkan/EXPIRY-design.md): the arena blocks (geometry and other device blocks, host-written blocks, acceleration structure blocks: bytes alive and bytes in use),
    // the idle dedicated acceleration structure blocks the pool holds, and the bytes of the ray scene's structures; false until Vulkan is up
    struct MemoryByConsumer
    {
        U32 mBlocks = 0, mHostBlocks = 0, mAccelBlocks = 0, mAccelPooled = 0;
        U64 mBlockBytes = 0, mBlockUsed = 0, mHostBytes = 0, mHostUsed = 0, mAccelBytes = 0, mAccelUsed = 0, mAccelPooledBytes = 0, mRtStructureBytes = 0;
    };
    bool memoryByConsumer(MemoryByConsumer& out);
    std::string accelCensus();      // the AS census text of Context::arenaAccelCensus (empty when Vulkan is not up)
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
        bool mNeuralRender = false;         // <FS> DLSS-NR (neural rendering) can run (DLSS on, NGX says the feature is available and supported, nvngx_dlssnr.dll next to the viewer)
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
    long long lastPresentTicks();       // QueryPerformanceCounter ticks of the worker's last present (0 before the first): the hitch watcher's second clock

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
        F32 mCameraNear = 0.1f, mCameraFar = 512.f, mCameraFovY = 1.f;      // <FS> the FSR backend wants them (metres, radians)
        F32 mFrameTimeMs = 16.6f;
        // <FS> DLSS-NR (doc/vulkan/PLAN-dlss-nr.md): mNeuralRender set means this is not an upscale but the neural renderer on the finished window-size colour (mColor and mOutput at
        // the output size, mDepth and mMotion at the render size); mPreset, mMotionScaleX/Y and mReset are shared with the upscale
        S32 mNeuralRender = 0;              // 1 evaluate, 2 release the feature (the mode went off; nothing else in the args is read)
        S32 mNrQuality = 1, mNrAutoMask = 0;
        F32 mNrScalingRatio = 0.f;          // > 0: the network's size as a fraction of the output (DLSSNR.ScalingRatio at creation)
        F32 mNrStyle = 0.f, mNrIntensity = 1.f, mNrGlobalTone = 0.5f, mNrLocalTone = 0.5f, mNrLocalStructure = 0.5f, mNrSkinStructure = 0.5f;
    };
    // DLSS is initialised and usable (decided when the Vulkan device was created)
    bool upscaleAvailable();
    // DLSS ray reconstruction can run here (DLSS is ready and NGX reports the feature)
    bool rayReconstructionAvailable();
    // <FS> the value of LL_VK_DLSS_NR (0 unset, 1 probe, 2 evaluate, 3 show), and whether the feature can run (probe said yes, nothing failed since)
    S32 neuralRenderMode();
    bool neuralRenderAvailable();
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
        U32 mFragment = 0;          // kind 11: the id of the fragment retained (S1, below); kind 0: 1 = drawn only, not named to the ray scene (S6.6c)
        U64 mProgram = ~0ull;       // S6.3: the program (an LLVk::Program*) the record draws with, written by pushData from the producer's draw context; ~0: not named, the worker keeps the one it has
        U32 mParticleFloats = 0;    // kind 36 (the particle sort): floats of the source table after the jobs: a row (now high, now low, 0, 0), then a source is 8 floats (position, 0, target, 0)
        U32 mTexmats = 0;           // kind 1: texture matrices (16 floats each) after the palettes; an entry names its matrix by mTexMatrix
        U32 mFrame = 0;             // S6.3: the producer's frame number (0: unknown); kinds 22 and 29 draw what the frame's kind 21 prepared and are checked against it
        U32 mViewerFrame = 0;       // P3: the viewer's frame (gFrameCount) a record that names palettes by id is made in: the id table it is resolved in (kind 8: mFragment == 1)
        U32 mEntryBytes = 0;        // J1 K3b: kind 2: 0 = ShadowSkinEntry entries, sizeof(ShadowSkinSlim) = the slim ones (the fields the shadow skin pull program reads; palettes by id, no ray scene fields)
        U32 mDrawOnly = 0;          // J1 K6 step 3d: kind 1: 1 = drawn only, not named to the ray scene (the glTF and texture matrix casters of a raster pass in the ray tracing modes: the viewer names them from the fragments' items)
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
    // S2 alpha (ray shadows, LL_VK_SCENE_RAYALPHA=1): kind 18 RETAIN_ALPHA: the alpha masked static casters of a resident fragment: mFragment, mEntries ShadowAlphaEntry (mMatrix = region matrix slot,
    //   mTextures = GL names, ALPHA_NOT_READY set when a texture has no data), mViewProj[0..3] = the fragment's bounding sphere (the kind 14 one); merges with the fragment's kind 14 record.
    //   kind 19 TEXTURES: mFragment, mEntries updates { entry index, 4 GL names, ready } (6 U32 each) for entries whose textures changed name or readiness.
    // mMatrixId: stable identity of the model matrix (ray tracing instances). The last four are for the ray scene only: the material of the draw (RtMaterialDef id, 0 = none) and the byte
    // offsets of the texcoord0, colour and normal arrays in the vertex buffer (0 = the buffer has no such array)
    struct ShadowPacketEntry { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mMatrixId, mMaterial, mTexcoordOffset, mColorOffset, mNormalOffset; };
    // kind 2: a rigged draw: the offset of its weight4 array in the vertex buffer, and its palette (offset in floats from the start of the palettes, joint count)
    // (ray tracing: the vertex range the draw uses, the avatar it belongs to and the identity of its skin, both stable from frame to frame)
    // ray scene materials (LL_VK_RT_MATERIALS): mMaterial = RtMaterialDef id of the draw (0 = none), mNormalOffset / mTexcoordOffset / mColorOffset = byte offsets of those arrays in the vertex
    // buffer (0 = none)
    // <FS> J1 K3b: what a cascade's rigged record needs per draw when no ray scene reads it (a third of ShadowSkinEntry): the buffers, the index range, the model matrix number, the weights' offset, the palette (an id of the
    // frame's shared list) and its joints. The worker expands it to a ShadowSkinEntry (mEntryBytes in the header says which a record carries).
    struct ShadowSkinSlim { U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix, mWeightOffset, mPaletteOffset, mJoints; };
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
        ALPHA_RAY_ONLY = 128,       // ray tracing: an opaque caster (no alpha test); kind 5 records only
        ALPHA_NOT_READY = 256,      // kind 18 / 19 only: a texture of the entry has no data yet (ray scene loading gate): the entry stays out of the scene
        ALPHA_BLENDED = 512,        // ray tracing: an alpha BLENDED draw (the alpha pool, KIND_ALPHA): the soft sun ray takes its alpha as its coverage (rtAlphaCover), not the masked ramp
        ALPHA_DOUBLE_SIDED = 1024,  // kind 1 (raster), C5 PBR shadow rows: a double sided glTF caster: the viewer sends it in a record of its own with culling off (the shader ignores the bit)
        ALPHA_OPAQUE = 2048         // kind 1 (raster): a caster with no alpha test (opaque glTF): the fragment shader samples nothing. ALPHA_HARD (64) in the raster pass: texture alpha times vertex alpha against mCutoff, no dithering
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
        U32 mTexMatrix;             // kind 1 (raster): 1 + the number of the draw's texture matrix among the record's mTexmats (after the palettes), 0 = none
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
    // S5f F3 kind 32 (alpha transient): the draws the scene table does not keep (particle groups, ...) of an alpha pass, drawn with the pull program the viewer bound, in the pass's order. Blob: mEntries GeoEntry
    //   (mMatrix = number of its model matrix, mPad3 = number of its texture matrix, 0 = none, mPbrRow = its PBR row, textures as GL names), then mMatrices x 44 floats as in kind 3 (matrix 0: the view alone), then
    //   mFragment texture matrices (16 floats each, numbered from 1), then mPaletteFloats PBR rows (40 floats each, numbered from 0). The pull shaders read the same row layout as for fragment items.
    // kind 3: a draw of a deferred geometry pass (the colour passes). The matrices after the entries are 44 floats each: modelview, modelview * projection, and the normal
    // matrix as three vec4 columns (the viewer computes them, once per model matrix). Matrix 0 is the view alone.
    enum GeoFlags : U32
    {
        GEO_ONE_TEXTURE = 1,        // one diffuse texture, mTextures[0]: the vertices carry no texture index
        GEO_RIGGED = 2,             // A1 (kind 25): a rigged draw, mWeightOffset / mPaletteOffset / mJoints are valid
        GEO_NO_INDEX = 4,           // S6.6: no index buffer: mFirstIndex is the first vertex, mIndexCount the vertex count (a drawArrays)
        GEO_STRIP = 8,              // S6.6: the draw is a triangle strip (a record of one entry)
        GEO_PARTICLE_ROWS = 16,     // GPU particles stage 1: the arrays hold one row a particle (position, velocity, half size, colour, glow); mFirstIndex / mIndexCount count 6 vertices a particle (the pull shaders make the quad)
        GEO_FULLBRIGHT = 32,        // item D1 (alpha runs): a fullbright alpha draw: the merged lit alpha program (ALPHA_UBER) skips the lighting for it
        GEO_MATERIAL = 64,          // item D2 (alpha runs): a material alpha draw: the merged program (ALPHA_UBER_MAT) shades it as the materials blend shader does
        GEO_NORMAL_MAP = 128,       // ... with a normal map (the row's mapSlots low half)
        GEO_SPEC_MAP = 256,         // ... with a specular map (the row's mapSlots high half)
        GEO_DOUBLE_SIDED = 512      // P3: a double sided glTF draw of the table's G-buffer passes: the worker keeps these commands apart (after the single sided ones of the type) and a kind 22 with mFragment = 1 draws them, culling off
    };
    struct GeoEntry
    {
        U32 mVertexBuffer, mIndexBuffer, mFirstIndex, mIndexCount, mMatrix;
        U32 mFlags;                 // GeoFlags
        U32 mNormalOffset, mTexcoordOffset, mColorOffset;      // byte offsets of the arrays in the vertex buffer
        U32 mTextures[4];           // GL names of the diffuse textures the vertices' texture index picks from, 0 = none (white)
        U32 mNormalMap, mSpecularMap;   // GL names (materials passes), 0 = none
        F32 mSpecular[4];
        F32 mEnvIntensity, mCutoff, mBrightness;
        U32 mEmissiveOffset;        // S5e: byte offset of the emissive array (alpha items; 0 = none)
        U32 mPbrRow;                // S5f F3 (kind 32): the number of the entry's PBR row in the record (glTF alpha items)
        U32 mTangentOffset, mTexcoord1Offset, mTexcoord2Offset;     // materials passes: byte offsets of the tangent and the texcoord 1 and 2 arrays
        U32 mWeightOffset, mPaletteOffset, mJoints, mPad3;          // A1 rigged draws: byte offset of the weight4 array, the palette's offset in floats (12 per joint) and its joint count
        U32 mDebugColor;            // K6 B1: the Batch Size debug view's colour of the draw (LLDrawInfo::getDebugColor, made when the entry is built; 0 = none), carried in a spare word of the table row
    };
    // S4 A2 kind 27 PALETTES: the viewer's per-frame palette list grows by mPaletteFloats floats (12 per joint, whole palettes) at float offset mFragment; mPass = the viewer's frame; the blob is the floats. The worker keeps
    //   them in scratch memory of the frame (segments by offset); entries of kinds 25 / 26 and of kinds 2 / 6 with mFragment = 1 name a palette by its absolute offset in this list instead of carrying copies.
    // S4 A1 (doc/vulkan/S4-design.md) kind 25: the same record as kind 3 for rigged draws (GEO_RIGGED, entries 160 bytes in the table, drawn with the skinned pull program): after the matrices (44 floats each)
    //   come mPaletteFloats floats of joint matrix palettes (12 per joint), the entries point into them.
    // S3 G2 / G3 (doc/vulkan/S3-design.md, LL_VK_SCENE_GEO=2): the deferred geometry entries kept by the worker in a persistent table, drawn from the list of fragments the viewer says are visible.
    // The fragments are named by the viewer's dense HANDLE (LLSceneFragment::mGeoHandle, reused only after the release was sent).
    // kind 20 RETAIN_GEO: mFragment = handle, mEntries x GeoWire (mE.mMatrix = region matrix slot, 0 = the view alone; its texture fields are 1 + the number in the fragment's texture list, 0 = none),
    //   then mPaletteFloats GL names, the fragment's textures in list order (the worker pins them: permanent bindless slots).
    // kind 21 PREPARE_GEO (once a frame, before the passes): mEntries = handles of the visible fragments (U32 each), then mMatrices x GeoSlotMatrix (all region matrices in use). The worker looks at the
    //   fragments that something named (buffer storage changed, a texture's name changed, a pin got another slot), and writes the draw commands of the frame, by pass type.
    // kind 22 DRAW_GEO: mPass = pass type number (0..12, the viewer's kGeoTypes order), blob = 32 floats: the view and the projection matrix (column-major) of the pass. The viewer has bound the pass's
    //   pull program; the worker draws that type's commands with it.
    // kind 23 RELEASE_GEO: mEntries handles (U32). kind 24 GEO_TEX: mFragment = handle, mEntries x { local texture number, GL name } (the textures whose name changed).
    struct GeoWire { GeoEntry mE; U32 mType; };
    // kind 33 (S6 LV1): the light volumes of a deferred lighting pass in one record. The viewer has bound the pull light program (pointLightV with LL_LIGHT_PULL) and written the pass's uniforms and
    // matrices; blob = mEntries x LightVolumeRow. The worker draws each as one 8 vertex triangle fan (the fan of the box as seen from the camera side mCypher), the light read from the table by instance.
    struct LightVolumeRow { F32 mCenter[3]; F32 mSize; F32 mColor[3]; F32 mFalloff; U32 mCypher; U32 mClassic; U32 mPad[2]; };
    // kind 33 with mPass 1 (LV2): the projectors: blob = mEntries x SpotVolumeRow (11 vec4: the light, the projector's matrix and values, the GL name of its image in mTexture, which the worker replaces by the
    // bindless slot of the frame), drawn with the pull projector program (spotLightF with LL_PROJECTOR_PULL)
    struct SpotVolumeRow { F32 mCenter[3]; F32 mSize; F32 mColor[3]; F32 mFalloff; U32 mCypher; U32 mClassic; U32 mTexture; S32 mShadowIndex; F32 mN[3]; F32 mNear; F32 mP[3]; F32 mRange; F32 mOrigin[3]; F32 mAmbiance; F32 mFocus; F32 mLod; F32 mAmbientLod; F32 mShadowFade; F32 mMat[16]; };
    // kind 41 (2026-11-10): the avatar impostors of a deferred pass in one record, instead of one immediate mode quad (and three texture binds) a avatar. The viewer has bound the program "Deferred Impostor Shader"
    // (its native stand-in is the only one that draws: it declares ll_impostor_table) and written the pass's matrices; blob = mEntries x ImpostorQuadRow. The worker replaces the three GL names by this
    // frame's bindless slots and draws each row as 6 vertices (two triangles: the corners are centre -/+ left -/+ up, the order of LLVOAvatar::renderImpostor), the row read from the table by instance.
    struct ImpostorQuadRow { F32 mCenter[3]; U32 mDiffuse; F32 mLeft[3]; U32 mNormal; F32 mUp[3]; U32 mSpecular; };
    struct GeoFragRef { U32 mId, mNameOffset, mNameCount; };
    struct GeoSlotMatrix { U32 mSlot; F32 mM[16]; };
    // S7 H1 (doc/vulkan/S7-design.md section 4): Hi-Z occlusion. hizSend queues kind 34 (the boxes of the occlusion groups and the view-projection of the pass) and returns its serial (0: not sent: no
    // Vulkan, too many boxes, or too many serials not yet read). The worker tests the boxes against a depth pyramid of the depth target bound at that point of the frame and writes one word a box (bit 0 =
    // occluded, bits 8-15 = pyramid level, bit 16 = behind the near plane, bit 17 = off screen, bit 18 = beyond the far plane) into a ring slot of cached host memory; the slot is published when the GPU has
    // finished the frame. hizRead copies the 3 words a box (result bits, nearest depth, largest depth under it; out holds 3 * count) of a serial: 1 = copied, 0 = not published yet, -1 = lost (the slot holds a later serial). hizDone(serial) frees the serial's slot for reuse.
    struct HizBox { F32 mCenter[3]; U32 mFlags; F32 mHalf[3]; F32 mPad; };      // mFlags bit 0 = water (depth clamped: no near / far rejection)
    bool hizAvailable();
    // S7 B0 (LL_VK_VISCENSUS): with prev_view_proj the boxes from census_first on are tested a second time against the pyramid of the previous record (with its view-projection); hizRead(.., true) copies those verdicts (bit 31 = valid)
    U32 hizSend(const HizBox* boxes, U32 count, const F32* view_proj, const F32* prev_view_proj = nullptr, U32 census_first = 0);
    S32 hizRead(U32 serial, U32 count, U32* out, bool census_half = false);
    void hizDone(U32 serial);
    void hizSetResults(const U32* results, U32 capacity);       // (worker) the mapped ring
    void hizPublish(U32 slot, U32 serial);                      // (worker) the frame of this serial has finished
    // H6 (doc/vulkan/PLAN-texture-handles.md): what the draws measured of the need of a texture. The table draws record, per texture, the smallest area of texture coordinates one pixel covered (so the
    // size of the whole texture on the screen is 1 / area pixels); the worker reads that back when the GPU has finished the frame (3 or so frames later) and keeps it by texture id.
    // mWindow: the smallest area over the last one to two windows (half a second each) in which the texture was drawn (INF: it has not been drawn in them, its windows are older than that); mKnown: the same, kept
    // when the texture is not drawn any more (INF: never drawn); mAge: seconds since the texture was last drawn. Main thread; false when nothing was ever measured for the id.
    struct TextureNeed { F32 mWindow = 0.f; F32 mKnown = 0.f; F32 mAge = 0.f; };
    bool textureNeed(U32 name, TextureNeed& out);
    void textureNeedStats(U64& textures, U64& readings);
    void textureNeedReading(U32 name, F32 area);                 // (worker) a reading of one finished frame
    void textureNeedForget(U32 name);                            // (worker) the texture is gone
    // S7 C1 (doc/vulkan/S7-design.md section 14, LL_VK_CULL_SHADOW=1): kind 35, the flat group table (CullRow, 64 bytes) tested against the camera frustum, shadow mode only. blob = CullParams then
    // count rows; mViewProj of the header is unused, mEntries = rows, mFragment = the serial. The worker writes cullVariants bit sets of ceil(rows / 32) words (bit i = row i passes) into a ring slot of cached
    // host memory; cullRead copies them (variants * ceil(count / 32) words): 1 = copied, 0 = not published yet, -1 = lost.
    constexpr U32 cullVariants = 4;
    struct CullParams { F32 mPlanes[7][4]; F32 mOrigin[3]; F32 mCornerDist; F32 mMotion[4]; F32 mFactors[4]; U32 mPlaneCount; U32 mPad[7]; };     // 192 bytes; mMotion = translation bound, rotation bound
    struct CullRow { F32 mCenter[3]; U32 mParent; F32 mHalf[3]; U32 mFlags; F32 mExtMin[3]; F32 mPad0; F32 mExtMax[3]; F32 mPad1; };       // mFlags: 1 root, 2 skip frustum check, 4 occluded, 8 sphere test
    static_assert(sizeof(CullParams) == 192 && sizeof(CullRow) == 64, "cull record layout");
    // C1c: the record carries delta rows: slots[n] (U32 each, padded to 16 bytes) then rows[n]; table_size = the rows of the whole table after the delta; reset = the worker's copy is replaced (first record, or after a record
    // was refused). The worker keeps the table and uploads all of it for the test. cullRead takes the table size of the record (the words of a variant are ceil(table_size / 32)).
    U32 cullSend(const CullParams& params, const U32* slots, const CullRow* rows, U32 count, U32 table_size, bool reset);
    S32 cullRead(U32 serial, U32 table_size, U32* out);
    void cullDone(U32 serial);
    void cullSetResults(const U32* results, U32 capacity_words);    // (worker) the mapped ring: capacity_words per slot
    void cullPublish(U32 slot, U32 serial);                         // (worker)
    bool sceneGeoAvailable();       // the worker can keep and draw them (buffer addresses, indirect draws with a first instance, bindless textures)
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
    S32 rtSnapTest();              // rt-debug.txt snaptest=N (0 = not set): every change of N above 0 takes a snapshot as the exit does
    S32 rtCullUse();               // rt-debug.txt culluse=N (-1 = not set): LL_VK_CULL_USE live, a same-lap A/B of the main view's cull walk against the GPU verdict
    S32 rtSunSamples();            // rt-debug.txt sunsamples=N (0 = not set: RenderRTShadowSamples decides), a same-lap cost A/B of the sun rays per pixel
    S32 rtTexAnim();               // rt-debug.txt texanim=0/1/2 (-1 = not set: the setting FSTexAnimOnGPU decides), a same-lap A/B of the texture animation by the CPU against the vertex shader
    S32 rtSunNoise();              // rt-debug.txt sunnoise=0/1/2 (default 1): how the noise of the ray traced sun shadow moves with the jitter: 1 every frame, 2 every 4th frame, 0 not at all
    S32 rtEarly();                 // rt-debug.txt rtearly=0/1/3 (default -1: the environment variable LL_VK_RT_EARLY decides): where the ray scene is built, a same-session A/B
    void nativeGlobalDefines(const std::vector<std::pair<std::string, std::string>>& defines);     // <FS> the viewer's global shader defines (llvknative.h), pushed each time the viewer's shaders are set up (main thread)
    S32 nativeKey(const char* key, S32 def);     // <FS> a live key of rt-debug.txt (native shaders, llvknative.h): `key=N`, or 'def' when the line is missing; any thread
    S32 rtSunFilter();           // rt-debug.txt sunfilter=0/1 (default 1): the penumbra filter sized by the blocker distance runs on the centre ray's shadow (rtShadowFilterF.glsl)
    S32 rtSunPenMax();             // rt-debug.txt penmax=N (default 8): that filter's widest penumbra half-width in render pixels
    S32 rtSunPenLod();             // rt-debug.txt penlod=N (default 1): a bias in mip levels on that filter's taps (1 = each tap averages twice the footprint)
    S32 rtSunStrength();           // rt-debug.txt sunstrength=N percent (default 65): with the centre ray, the most light an avatar's own close surfaces block on that avatar
    S32 rtSunSelfShadow();         // rt-debug.txt sunself=0/1 (default 1): the translucent, soft avatar self shadows of the sun; 0 = off (an avatar blocks fully, no start disc), a same-lap cost A/B
    S32 rtSunPenRays();            // rt-debug.txt penrays=N (default 0 = the shader's 8): the rays of a pixel whose first sun ray met a close skinned surface
    S32 rtDebugView();      // rt-debug.txt view=N: the ray scene material debug view mode to show, 0 = the setting RenderRTMaterialDebug decides
    bool rtBodyRename();
    U32 rtSkinPeriod(float distance);      // viewer frames between two poses the ray scene takes of an avatar this far from the eye (1 when ray tracing is off)
    // Builds the scene the shadow passes of this frame named (their casters) for the camera view: bottom-level structures as the budget allows, then the top-level structure in
    // eye space. Call before the lighting that samples the sun shadow. view: the camera's view matrix, column-major.
    struct RtSceneArgs { F32 mView[16] = {}; F32 mDistanceScale = 1.f; U32 mSnapshot = 0; U32 mPhase = 0; U32 mAsync = 0; };       // (mPhase: 0 the whole call, 1 skin + builds + top level with no barrier after them, 2 only that barrier: LL_VK_RT_EARLY, doc/vulkan/PLAN-async-compute.md; mAsync: phase 1 on the compute queue, E2, LL_VK_RT_EARLY=3)       // (mDistanceScale: what the far avatar rules multiply a distance by, less than 1 when the camera zooms in; mSnapshot: a render of a snapshot, where the avatars' structures are built again and not refitted, see llvkrt.cpp)
    void rtScene(const RtSceneArgs& args);
    // the device can read buffers by address and the mirror is running
    bool shadowPacketsAvailable();
    // ... and the device can index sampled images at run time (the textured kind)
    bool shadowAlphaPacketsAvailable();
    // J1 (Adam, 2026-11-07: "there has to be a way to get it to refresh after a shader loads"): the GPU pipelines the renderer builds in the background at start-up (the variants earlier sessions recorded) or for a draw
    // that asked: how many are still being built, and how many there are (pre-warm queued + draw misses). After a shader change the heavy ones (ray queries inlined in the lit alpha programs) take minutes, and every
    // draw that needs one is dropped until it is done: the world is lit wrong or not at all. The start-up waits for them (llstartup.cpp).
    void pipelineBuilds(U64& pending, U64& total);
    // blob: mEntries ShadowPacketEntry, then mMatrices * 16 floats
    void shadowPackets(const ShadowPacketsHeader& header, const void* blob, size_t blob_bytes);
    // <FS> PLAN-avatars.md, rigged draws as rows, P1 (LL_VK_PALARENA): the frame's palettes are written by the viewer straight into memory the renderer owns (a host visible, mapped buffer with a device address),
    // not copied into the frame list and then twice more (kind 27: the record, the worker's scratch). paletteArenaAcquire gives the arena of a viewer frame (false: none is free, the old copies are used);
    // a kind 39 record (mPass = the viewer frame, mEntries = the arena index, mFragment = the first float, mPaletteFloats = how many) names a written part as a segment of the frame's list; the renderer frees
    // an arena frameCount() + 3 renderer frames after the last record that named it (the ring rule of the palette scratch: the ray scene reads palettes up to two frames later).
    struct PaletteArena { F32* mPtr = nullptr; U64 mAddress = 0; U32 mFloats = 0; U32 mIndex = 0; };
    bool paletteArenaAcquire(U32 frame, PaletteArena& out);              // any thread
    void paletteArenaRegister(U32 index, F32* ptr, U64 address, U32 floats);       // the renderer, once the buffers exist
    void paletteArenaClear();                                            // the renderer, before the buffers are destroyed
    bool paletteArenaInfo(U32 index, PaletteArena& out);                 // the worker
    void paletteArenaSent(U32 index, U64 free_at_renderer_frame);        // the worker, for a kind 39 record
    void paletteArenaFree(U64 renderer_frame);                           // the renderer, at the start of each frame
    // <FS> P2 (PLAN-avatars.md): the number of kind 21 records the worker did not play (not in a frame, no scene table). A rigged item's palette binding is persistent: the viewer sends it when it changes, so a record
    // that was lost makes it send every binding again
    static inline std::atomic<U32> sGeoPrepareDropped{ 0 };
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
    // <FS> what the ray scene's alpha test will read of a texture (the census of the alpha casters' textures in the ray scene log line): its size now, its levels and how many hold data
    struct TextureInfo { unsigned mWidth = 0, mHeight = 0, mLevels = 0, mValidLevels = 0; bool mCompressed = false, mHasImage = false; };
    bool getTextureInfo(unsigned int name, TextureInfo& out);
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
