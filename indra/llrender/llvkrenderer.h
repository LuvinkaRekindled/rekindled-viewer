/**
 * @file llvkrenderer.h
 * @brief Vulkan draw machinery: compiled programs, pipelines, uniform ring, push-descriptor textures,
 *        lazily begun render passes and GL-style state tracking.
 *
 * The renderer records into the command buffer of the current Context frame. State is set the way the viewer's
 * GL code sets it (bind a program, set uniforms, bind textures, set blend/depth/cull, draw); everything that
 * Vulkan wants up front is created on demand and cached. Conventions follow GL: positive viewport height,
 * counter-clockwise front faces, z remapped in the vertex shader (see llvkshader.h).
 */

#ifndef LL_LLVKRENDERER_H
#define LL_LLVKRENDERER_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"
#include "llvkshader.h"

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace LLVk
{

struct PipelineKey
{
    VkPrimitiveTopology mTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    U32 mColorCount = 0;
    VkFormat mColor[8] = {};
    VkFormat mDepth = VK_FORMAT_UNDEFINED;

    bool operator<(const PipelineKey& o) const;
};

// A compiled GL program in Vulkan form: shader modules, uniform shadow memory and its pipelines.
class Program
{
public:
    bool create(const std::string& name, const ProgramInfo& info);
    void destroy();

    const std::string& getName() const { return mName; }
    const ProgramInfo& getInfo() const { return mInfo; }
    bool isValid() const { return mValid; }

    // loose uniforms (write into the shadow copy; uploaded to the uniform ring on the next draw)
    bool hasUniform(const std::string& name) const { return mUniformIndex.count(name) != 0; }
    // scalar / vector floats or ints; for arrays 'count' elements are written with std140 stride
    bool setFloats(const std::string& name, const F32* data, U32 components, U32 count = 1);
    bool setInts(const std::string& name, const S32* data, U32 components, U32 count = 1);
    // matrices given as tightly packed column-major floats (columns x rows), 'count' of them for arrays
    bool setMatrices(const std::string& name, const F32* data, U32 columns, U32 rows, U32 count = 1);
    // the same, addressing the uniform by its index in getInfo().mUniforms
    void setFloatsAt(size_t uniform_index, const F32* data, U32 components, U32 count);
    void setMatricesAt(size_t uniform_index, const F32* data, U32 columns, U32 rows, U32 count);

    // texture unit each sampler reads (GL numbering; defaults to the sampler's binding)
    void setSamplerUnit(size_t sampler_index, U32 unit) { if (sampler_index < mSamplerUnits.size()) mSamplerUnits[sampler_index] = unit; }
    S32 findSampler(const std::string& name) const;
    U32 getSamplerUnit(size_t sampler_index) const { return sampler_index < mSamplerUnits.size() ? mSamplerUnits[sampler_index] : 0; }
    U32 getSamplerClass(size_t sampler_index) const { return sampler_index < mSamplerClass.size() ? mSamplerClass[sampler_index] : 0; }

    S32 getSamplerBinding(const std::string& name) const;
    bool isDirty() const { return mDirty; }
    const U8* getUniformData() const { return mShadow.data(); }
    U32 getUniformSize() const { return (U32)mShadow.size(); }
    void clearDirty() { mDirty = false; }

    VkPipeline getPipeline(const PipelineKey& key);

private:
    friend class Renderer;
    std::string mName;
    ProgramInfo mInfo;
    bool mValid = false;
    VkShaderModule mModules[3] = {};
    std::unordered_map<std::string, size_t> mUniformIndex;
    std::unordered_map<std::string, U32> mSamplerIndex;
    std::vector<U32> mSamplerUnits;
    std::vector<U8> mSamplerClass;      // 0 2D, 1 cube, 2 cube array, 3 2D array, 4 3D, 5 multisample (index into the GL per-unit texture bindings)
    std::vector<U8> mShadow;
    bool mDirty = true;
    U32 mRingOffset = 0;                // where this program's block was last written in the uniform ring
    U64 mRingFrame = ~0ull;             // frame number that offset belongs to
    std::map<PipelineKey, VkPipeline> mPipelines;
    PipelineKey mLastKey;               // the key of the last lookup, answered without searching the map
    VkPipeline mLastPipeline = VK_NULL_HANDLE;
};

struct VertexAttribute
{
    U32 mLocation = 0;
    VkFormat mFormat = VK_FORMAT_R32G32B32_SFLOAT;
    U32 mOffset = 0;
};

struct VertexLayout
{
    U32 mStride = 0;
    std::vector<VertexAttribute> mAttributes;
};

// General vertex input: several buffers (bindings), each with its own stride, feeding any attribute location
struct VertexInput
{
    struct Binding
    {
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
        U32 mStride = 0;
    };
    struct Attribute
    {
        U32 mLocation = 0;
        U32 mBinding = 0;
        VkFormat mFormat = VK_FORMAT_R32G32B32_SFLOAT;
        U32 mOffset = 0;
    };
    std::vector<Binding> mBindings;
    std::vector<Attribute> mAttributes;
};

struct RasterState
{
    VkCullModeFlags mCull = VK_CULL_MODE_NONE;
    VkFrontFace mFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode mPolygonMode = VK_POLYGON_MODE_FILL;
    F32 mLineWidth = 1.f;
    bool mDepthBias = false;
    bool mDepthClamp = false;
    F32 mBiasConstant = 0.f;
    F32 mBiasSlope = 0.f;
    bool operator==(const RasterState& o) const;
};

struct DepthStencilState
{
    bool mDepthTest = false;
    bool mDepthWrite = true;
    VkCompareOp mDepthOp = VK_COMPARE_OP_LESS;
    bool mStencilTest = false;
    VkCompareOp mStencilOp = VK_COMPARE_OP_ALWAYS;
    VkStencilOp mFail = VK_STENCIL_OP_KEEP;
    VkStencilOp mDepthFail = VK_STENCIL_OP_KEEP;
    VkStencilOp mPass = VK_STENCIL_OP_KEEP;
    U32 mCompareMask = 0xFFFFFFFF;
    U32 mWriteMask = 0xFFFFFFFF;
    U32 mReference = 0;
    bool operator==(const DepthStencilState& o) const;
};

struct BlendState
{
    bool mEnable = false;
    VkBlendFactor mSrcColor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor mDstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp mColorOp = VK_BLEND_OP_ADD;
    VkBlendFactor mSrcAlpha = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor mDstAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkBlendOp mAlphaOp = VK_BLEND_OP_ADD;
    VkColorComponentFlags mWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    bool operator==(const BlendState& o) const;
};

struct SamplerDesc
{
    VkFilter mMag = VK_FILTER_LINEAR;
    VkFilter mMin = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mMip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode mAddressU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode mAddressV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode mAddressW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    F32 mAnisotropy = 1.f;
    bool mCompare = false;
    VkCompareOp mCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    F32 mMinLod = 0.f;
    F32 mMaxLod = 1000.f;
    bool operator<(const SamplerDesc& o) const;
};

// ---- textures ------------------------------------------------------------------------------------------
enum class TexType { Tex2D, Tex2DArray, Cube, CubeArray, Tex3D };

struct TextureDesc
{
    TexType mType = TexType::Tex2D;
    U32 mWidth = 1;
    U32 mHeight = 1;
    U32 mDepth = 1;                 // array layers, cube count (CubeArray) or 3D depth; ignored for Tex2D / Cube
    U32 mMips = 1;
    VkFormat mFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags mUsage = 0;   // extra usage on top of sampled + transfer (e.g. COLOR_ATTACHMENT for render targets)
    VkComponentMapping mSwizzle = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
};

struct FormatInfo
{
    U32 mBlockWidth = 1;
    U32 mBlockHeight = 1;
    U32 mBytesPerBlock = 4;
    bool mCompressed = false;
    bool mKnown = false;
};

FormatInfo formatInfo(VkFormat format);
U32 mipLevelCount(U32 width, U32 height);

// clear selection bits
enum ClearBits : U32 { CLEAR_COLOR = 1, CLEAR_DEPTH = 2, CLEAR_STENCIL = 4 };

class Renderer
{
public:
    static const U32 MAX_TEXTURE_UNITS = 32;
    static const U32 UBO_SET_POOL = 64;
    static const VkDeviceSize MAX_UNIFORM_RANGE_NAMED = 65536;
    static const U32 MAX_UNIFORM_RANGE = 16384;        // bytes visible to a draw's loose uniform block

    static Renderer& instance();
    // peak uniform ring use (bytes) and number of dropped draws since the last call
    void getRingStats(U32& peak, U32& exhausted) { peak = std::max(mRingPeak, mRingUsed); exhausted = mRingExhausted; mRingPeak = 0; mRingExhausted = 0; }
    bool init();
    void shutdown();
    // The driver's compiled pipelines, kept between runs (<cache>/vk_pipeline_cache.bin): loaded in init(), written at shutdown and every few minutes
    // while new pipelines keep being built, so a crash does not lose a whole session of them. LL_VK_PIPELINE_CACHE=0 turns it off.
    void savePipelineCache();
    U32 mPipelinesSinceSave = 0;
    U64 mPipelineCacheLastSaveFrame = 0;
    bool isReady() const { return mReady; }

    // frame bracketing: 'cmd' and 'frame_slot' come from Context::beginFrame / the frame ring
    void beginFrame(VkCommandBuffer cmd, U32 frame_slot);
    void endFrame();

    // render target (color attachments + optional depth); changing it ends the current pass
    void setRenderTarget(Image* const* colors, U32 color_count, Image* depth);
    void setViewport(S32 x, S32 y, S32 width, S32 height);
    void setScissor(S32 x, S32 y, S32 width, S32 height);
    void setScissorEnabled(bool enabled);
    void clear(U32 bits, const F32 color[4], F32 depth, U32 stencil);

    // pipeline state
    void setProgram(Program* program);
    void setRasterState(const RasterState& s) { mRaster = s; }
    void setDepthStencilState(const DepthStencilState& s) { mDepthStencil = s; }
    void setBlendState(const BlendState& s) { mBlend = s; }
    void setTopology(VkPrimitiveTopology t) { mTopology = t; }
    void setVertexLayout(const VertexLayout& layout) { mLayout = layout; mVertexInputDirty = true; mUseVertexInput = false; }
    void setVertexBuffer(VkBuffer buffer, VkDeviceSize offset) { mVertexBuffer = buffer; mVertexOffset = offset; }
    // takes the vectors' storage (the caller's input is left empty)
    void setVertexInput(VertexInput& input)
    {
        mVertexInput.mBindings.swap(input.mBindings);
        mVertexInput.mAttributes.swap(input.mAttributes);
        mVertexInputDirty = true;
        mUseVertexInput = true;
    }

    // per-frame host-visible scratch memory usable as vertex or index data (reset every frame)
    struct ScratchAlloc { VkBuffer mBuffer = VK_NULL_HANDLE; VkDeviceSize mOffset = 0; void* mPtr = nullptr; VkDeviceAddress mAddress = 0; };
    ScratchAlloc allocVertexScratch(VkDeviceSize size);
    void setIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type) { mIndexBuffer = buffer; mIndexOffset = offset; mIndexType = type; }
    void setTexture(U32 unit, Image* image, VkSampler sampler);

    // named uniform blocks (ReflectionProbes, GLTFJoints, GLTFNodes, GLTFMaterials = indices 0..3); a null buffer unbinds
    void setUniformBuffer(U32 index, VkBuffer buffer, VkDeviceSize range);
    bool draw(U32 vertex_count, U32 first_vertex);
    bool drawIndexed(U32 index_count, U32 first_index, S32 vertex_offset);
    bool drawPackets(const U32* vertex_counts, U32 draws, VkBuffer indirect = VK_NULL_HANDLE, VkDeviceSize indirect_offset = 0);      // <FS> render packets

    VkSampler getSampler(const SamplerDesc& desc);
    Image* getWhiteTexture() { return &mWhite; }
    Program* getProgram() const { return mProgram; }
    bool inPass() const { return mRendering; }
    // LL_VK_PHASES: a short description of the current render target ("1483x777 c4 d" = size, colour attachments, depth) with the depth image's identity, so
    // that passes of the same size (the shadow cascades) stay apart
    std::string describeTarget() const
    {
        const Image* ref = mColorCount ? mColorTargets[0] : mDepthTarget;
        if (!ref)
        {
            return "no target";
        }
        const Image* depth = mDepthTarget ? (mDepthTarget->mOwner ? mDepthTarget->mOwner : mDepthTarget) : nullptr;
        char buf[96];
        snprintf(buf, sizeof(buf), "%ux%u c%u%s #%04x", ref->mExtent.width, ref->mExtent.height, mColorCount, depth ? " d" : "", (unsigned)(((size_t)depth >> 4) & 0xFFFF));
        return buf;
    }

    // Reads one mip level / layer of an image into tightly packed texels of its own format. Only valid outside a frame
    // (it waits for the GPU); used by the mirror's debug dump.
    bool readbackImage(Image& image, U32 mip, U32 layer, std::vector<U8>& texels);
    // Submits what the frame has recorded so far, waits, and keeps recording (render targets are kept; the next pass
    // loads rather than clears). Use before a readback that must see this frame's results.
    bool flushAndRestart();
    // counters the readback cache and the emulated fences use: anything that changes image contents bumps the first, every flushAndRestart the second
    U64 modificationCount() const { return mModCount; }
    U64 flushCount() const { return mFlushCount; }
    // makes every query recorded so far readable: finishes the current frame's work and collects the finished frames' results
    void resolveQueries();
    // The rectangle (x, y, w, h) of one mip level and layer, tightly packed. Valid right after flushAndRestart().
    bool readbackRegion(Image& image, U32 mip, U32 layer, S32 x, S32 y, U32 w, U32 h, std::vector<U8>& texels);
    // one whole mip level of a block-compressed image, the blocks exactly as stored (what glGetCompressedTexImage returns)
    bool readbackCompressedMip(Image& image, U32 mip, U32 layer, std::vector<U8>& blocks);
    // ---- device-local buffers (large vertex / index buffers) ----
    // The CPU never touches these. A write goes into staging memory now (or, outside a frame, into a private copy until the next frame
    // starts) and becomes a GPU copy of just the changed bytes when flushBufferUpdates() records it: before a draw that reads the buffer, and
    // at the end of the frame. Copies are ordered against the draws recorded before them by barriers, as GL promises.
    bool updateBuffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void* data);
    // records every pending copy (ends the current render pass if there is something to record); false while no frame is being recorded
    bool flushBufferUpdates();
    // texture uploads since the last call: how many, how many bytes, how many ended a render pass that was in progress
    struct UploadStats { U32 mCount = 0; U64 mBytes = 0; U32 mPassSplits = 0; };
    UploadStats takeUploadStats() { UploadStats s = mUploadStats; mUploadStats = UploadStats(); return s; }
    static U32 compressedMipBytes(const Image& image, U32 mip);
    // debug: records copies of the given pixel rows of a colour image (level/layer of the view) into a host buffer, tightly packed, in the current frame
    bool recordRowCopies(Image& view, const std::vector<U32>& rows, VkBuffer buffer);

    // copies a rectangle between images (blit, so formats may differ); ends the current pass
    bool copyRegion(Image& src, S32 sx, S32 sy, Image& dst, U32 dst_mip, U32 dst_layer, S32 dx, S32 dy, S32 width, S32 height);

    // makes sure an image is in 'layout', ending the current pass if the image is one of its attachments
    void ensureLayout(Image& image, VkImageLayout layout);
    // Records work that is not a draw (an NGX call) into the current frame: puts each image in the layout it needs, ends the render pass, then runs
    // 'record'. Afterwards the images are in the layouts the callback leaves them in, which must be the ones asked for. False outside a frame.
    bool recordExternal(const std::vector<std::pair<Image*, VkImageLayout>>& images, const std::function<void(VkCommandBuffer)>& record);
    // a clear with nothing drawn after it still has to happen: begins (and leaves open) a pass for it
    void flushPendingClear();

    // ---- textures ----
    bool createTexture(const TextureDesc& desc, Image& out);
    // Copies tightly packed texel rows (or compressed blocks) into one mip level of one layer (or 3D slice range
    // starting at 'layer' with 'depth' slices). Recorded into the current frame, or submitted immediately when
    // no frame is active. The image ends in SHADER_READ_ONLY_OPTIMAL.
    bool uploadTexture(Image& image, U32 mip, U32 layer, U32 x, U32 y, U32 width, U32 height, U32 depth, const void* data, VkDeviceSize bytes);
    // fills mips 1..n from mip 0 with linear blits (the image must have been created with mips > 1)
    bool generateMipmaps(Image& image);

    U32 getDrawCount() const { return mDrawCount; }

    // ---- GPU queries: occlusion and timestamps. Results become readable when the frame that recorded them has finished
    // (several frames later); takeQueryResults() hands over what has arrived since the last call. ----
    struct QueryResult { U64 mSerial; U32 mKind; U64 mValue; };     // mKind: 0 = samples passed, 1 = timestamp (start of an elapsed-time query), 2 = timestamp (end)
    // an occlusion query that starts with the next draw (it covers the draws up to queryEnd); false if no room
    bool queryBegin(U64 serial);
    void queryEnd();
    // writes a timestamp now; 'end' tells the harvest which half of an elapsed-time query it is
    bool queryTimestamp(U64 serial, bool end);
    void takeQueryResults(std::vector<QueryResult>& out);

private:
    friend class Program;
    Renderer() = default;

    bool createLayouts();
    Image* resolveTexture(size_t sampler_index, bool depth_ro, Image* depth_root);
    void transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout);
    void withCommandBuffer(const std::function<void(VkCommandBuffer)>& record);
    void applyState();
    void beginRenderingIfNeeded();
    void endRendering();
    U32 allocUniform(U32 size);

    bool mReady = false;
    VkDescriptorSetLayout mSet0Layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSet1Layout = VK_NULL_HANDLE;
    VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkDescriptorSet mSet0[Context::FRAMES_IN_FLIGHT] = {};
    VkDescriptorPool mUboPool[Context::FRAMES_IN_FLIGHT] = {};
    std::map<std::array<U64, 8>, VkDescriptorSet> mUboSets[Context::FRAMES_IN_FLIGHT];
    VkBuffer mUbo[4] = {};
    VkDeviceSize mUboRange[4] = {};
    VkDescriptorSet mAppliedSet0 = VK_NULL_HANDLE;
    VkDescriptorSet uboSet();
    VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
    Buffer mRing[Context::FRAMES_IN_FLIGHT];
    Buffer mDummyUbo;
    Image mWhite;
    Image mFallback[4];             // white stand-ins: 2D array, cube, cube array, 3D (mWhite is the 2D one)
    std::map<SamplerDesc, VkSampler> mSamplers;

    // per frame
    VkCommandBuffer mCmd = VK_NULL_HANDLE;
    U32 mSlot = 0;
    U32 mRingUsed = 0;
    U32 mRingPeak = 0;                  // largest ring use of any frame since the last getRingStats()
    U32 mRingExhausted = 0;             // draws dropped because the ring was full
    UploadStats mUploadStats;
    struct BufferCopy { VkBuffer mDst; VkDeviceSize mDstOffset; VkBuffer mSrc; VkDeviceSize mSrcOffset; VkDeviceSize mSize; };
    struct DeferredCopy { VkBuffer mDst; VkDeviceSize mDstOffset; std::vector<U8> mData; };
    std::vector<BufferCopy> mPendingCopies;         // in staging memory, to be recorded
    std::vector<DeferredCopy> mDeferredCopies;      // written outside a frame: staged when the next frame begins
public:
    // LL_VK_PHASES diagnostics: how many indexed draws changed nothing but the draw command, and how many of those continued the previous draw's index range
    struct DrawDiag { U32 mDraws = 0, mUnchanged = 0, mMergeable = 0; };
    DrawDiag takeDrawDiag() { DrawDiag d = mDrawDiag; mDrawDiag = DrawDiag(); return d; }
private:
    DrawDiag mDrawDiag;
    // <FS> what is bound in the command buffer already, so a draw does not record it again (reset where mBoundPipeline is)
    VkBuffer mBoundIndexBuffer = VK_NULL_HANDLE;
    VkDeviceSize mBoundIndexOffset = 0;
    VkIndexType mBoundIndexType = VK_INDEX_TYPE_UINT16;
    bool mUboKeyDirty = true;               // the bound uniform buffers changed since uboSet() last looked its set up
    VkDescriptorSet mLastUboSet = VK_NULL_HANDLE;
    bool mDiagChanged = false;
    VkBuffer mDiagLastIndexBuffer = VK_NULL_HANDLE;
    VkDeviceSize mDiagLastIndexOffset = 0;
    U64 mDiagLastIndexEnd = ~0ull;
    S32 mDiagLastVertexOffset = 0;
    bool mDrawBlocked = false;          // applyState could not prepare the draw (ring full)
    U32 mDrawCount = 0;
    bool mStateApplied = false;     // dynamic state has been set at least once in this command buffer

    // render target / pass
    Image* mColorTargets[8] = {};
    U32 mColorCount = 0;
    Image* mDepthTarget = nullptr;
    bool mRendering = false;
    U32 mPendingClear = 0;
    F32 mClearColor[4] = {};
    F32 mClearDepth = 1.f;
    U32 mClearStencil = 0;
    S32 mViewport[4] = {};
    S32 mScissor[4] = {};
    bool mScissorEnabled = false;
    bool mViewportSet = false;
    bool mInBegin = false;
    bool mDepthRO = false;          // depth attachment is also sampled: bound read-only
    U32 mAppliedColorCount = 0xFFFF;

    // draw state
    Program* mProgram = nullptr;
    VkPrimitiveTopology mTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    RasterState mRaster;
    DepthStencilState mDepthStencil;
    BlendState mBlend;
    VertexLayout mLayout;
    VertexInput mVertexInput;
    bool mUseVertexInput = false;
    VertexInput mAppliedVertexInput;        // what the command buffer's dynamic vertex input state currently holds
    bool mAppliedVertexInputValid = false;
    struct PushedBinding { VkImageView mView; VkSampler mSampler; VkImageLayout mLayout; };
    PushedBinding mPushed[MAX_TEXTURE_UNITS] = {};      // what each push-descriptor binding currently holds
    Buffer mVertexRing[Context::FRAMES_IN_FLIGHT];
    U32 mVertexRingUsed = 0;
    VkBuffer mVertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize mVertexOffset = 0;
    VkBuffer mIndexBuffer = VK_NULL_HANDLE;
    VkDeviceSize mIndexOffset = 0;
    VkIndexType mIndexType = VK_INDEX_TYPE_UINT16;
    Image* mTextures[MAX_TEXTURE_UNITS] = {};
    VkSampler mTextureSamplers[MAX_TEXTURE_UNITS] = {};

    // what has already been recorded
    VkPipeline mBoundPipeline = VK_NULL_HANDLE;
    RasterState mAppliedRaster;
    DepthStencilState mAppliedDepthStencil;
    BlendState mAppliedBlend;
    bool mVertexInputDirty = true;
    S32 mAppliedViewport[4] = { -1, -1, -1, -1 };
    S32 mAppliedScissor[4] = { -1, -1, -1, -1 };
    U32 mAppliedDynamicOffset = 0xFFFFFFFF;
    bool mSet0Bound = false;

    // queries (one pool of each kind per frame in flight)
    static constexpr U32 MAX_OCC_QUERIES = 8192;
    static constexpr U32 MAX_TS_QUERIES = 2048;
    VkQueryPool mOccPool[Context::FRAMES_IN_FLIGHT] = {};
    VkQueryPool mTsPool[Context::FRAMES_IN_FLIGHT] = {};
    std::vector<U64> mOccSerial[Context::FRAMES_IN_FLIGHT];
    std::vector<U8> mOccBegun[Context::FRAMES_IN_FLIGHT];
    std::vector<U64> mTsSerial[Context::FRAMES_IN_FLIGHT];
    std::vector<U8> mTsIsEnd[Context::FRAMES_IN_FLIGHT];
    U64 mModCount = 0, mFlushCount = 0;
    S32 mOccPending = -1;           // allocated, starts with the next draw
    S32 mOccActive = -1;            // begun inside the current pass
    std::vector<QueryResult> mQueryResults;
    bool createQueryPools();
    void destroyQueryPools();
    void harvestQueries(U32 slot);
    void startPendingQuery();
};

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKRENDERER_H
