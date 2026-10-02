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
    std::vector<U8> mShadow;
    bool mDirty = true;
    U32 mRingOffset = 0;                // where this program's block was last written in the uniform ring
    U64 mRingFrame = ~0ull;             // frame number that offset belongs to
    std::map<PipelineKey, VkPipeline> mPipelines;
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

struct RasterState
{
    VkCullModeFlags mCull = VK_CULL_MODE_NONE;
    VkFrontFace mFrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPolygonMode mPolygonMode = VK_POLYGON_MODE_FILL;
    F32 mLineWidth = 1.f;
    bool mDepthBias = false;
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
    static const U32 MAX_UNIFORM_RANGE = 16384;        // bytes visible to a draw's loose uniform block

    static Renderer& instance();
    bool init();
    void shutdown();
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
    void setVertexLayout(const VertexLayout& layout) { mLayout = layout; mVertexInputDirty = true; }
    void setVertexBuffer(VkBuffer buffer, VkDeviceSize offset) { mVertexBuffer = buffer; mVertexOffset = offset; }
    void setIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type) { mIndexBuffer = buffer; mIndexOffset = offset; mIndexType = type; }
    void setTexture(U32 unit, Image* image, VkSampler sampler);

    void draw(U32 vertex_count, U32 first_vertex);
    void drawIndexed(U32 index_count, U32 first_index, S32 vertex_offset);

    VkSampler getSampler(const SamplerDesc& desc);
    Image* getWhiteTexture() { return &mWhite; }

    // makes sure an image is in 'layout', ending the current pass if the image is one of its attachments
    void ensureLayout(Image& image, VkImageLayout layout);
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

private:
    friend class Program;
    Renderer() = default;

    bool createLayouts();
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
    VkPipelineCache mPipelineCache = VK_NULL_HANDLE;
    Buffer mRing[Context::FRAMES_IN_FLIGHT];
    Buffer mDummyUbo;
    Image mWhite;
    std::map<SamplerDesc, VkSampler> mSamplers;

    // per frame
    VkCommandBuffer mCmd = VK_NULL_HANDLE;
    U32 mSlot = 0;
    U32 mRingUsed = 0;
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

    // draw state
    Program* mProgram = nullptr;
    VkPrimitiveTopology mTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    RasterState mRaster;
    DepthStencilState mDepthStencil;
    BlendState mBlend;
    VertexLayout mLayout;
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
};

} // namespace LLVk

#endif // LL_VULKAN

#endif // LL_LLVKRENDERER_H
