/**
 * @file llvkrenderer.cpp
 * @brief Vulkan draw machinery (see llvkrenderer.h)
 */

#include "linden_common.h"

#include "llvkrenderer.h"

#if defined(LL_VULKAN)

#include <cstring>
#include <cstdlib>

namespace LLVk
{

namespace
{

bool vk_ok(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << what << " failed: VkResult " << (S32)r << LL_ENDL;
        return false;
    }
    return true;
}

const U32 RING_BYTES = 8u * 1024 * 1024;       // per frame in flight
const U32 DUMMY_UBO_BYTES = 64 * 1024;

VkShaderStageFlags all_graphics_stages()
{
    return VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
}

} // anonymous namespace

//-----------------------------------------------------------------------------------------------------
// small state comparisons
//-----------------------------------------------------------------------------------------------------

bool PipelineKey::operator<(const PipelineKey& o) const
{
    if (mTopology != o.mTopology) return mTopology < o.mTopology;
    if (mColorCount != o.mColorCount) return mColorCount < o.mColorCount;
    for (U32 i = 0; i < mColorCount; ++i)
    {
        if (mColor[i] != o.mColor[i]) return mColor[i] < o.mColor[i];
    }
    return mDepth < o.mDepth;
}

bool RasterState::operator==(const RasterState& o) const
{
    return mCull == o.mCull && mFrontFace == o.mFrontFace && mPolygonMode == o.mPolygonMode && mLineWidth == o.mLineWidth
        && mDepthBias == o.mDepthBias && mBiasConstant == o.mBiasConstant && mBiasSlope == o.mBiasSlope;
}

bool DepthStencilState::operator==(const DepthStencilState& o) const
{
    return mDepthTest == o.mDepthTest && mDepthWrite == o.mDepthWrite && mDepthOp == o.mDepthOp && mStencilTest == o.mStencilTest
        && mStencilOp == o.mStencilOp && mFail == o.mFail && mDepthFail == o.mDepthFail && mPass == o.mPass
        && mCompareMask == o.mCompareMask && mWriteMask == o.mWriteMask && mReference == o.mReference;
}

bool BlendState::operator==(const BlendState& o) const
{
    return mEnable == o.mEnable && mSrcColor == o.mSrcColor && mDstColor == o.mDstColor && mColorOp == o.mColorOp
        && mSrcAlpha == o.mSrcAlpha && mDstAlpha == o.mDstAlpha && mAlphaOp == o.mAlphaOp && mWriteMask == o.mWriteMask;
}

bool SamplerDesc::operator<(const SamplerDesc& o) const
{
    if (mMag != o.mMag) return mMag < o.mMag;
    if (mMin != o.mMin) return mMin < o.mMin;
    if (mMip != o.mMip) return mMip < o.mMip;
    if (mAddressU != o.mAddressU) return mAddressU < o.mAddressU;
    if (mAddressV != o.mAddressV) return mAddressV < o.mAddressV;
    if (mAddressW != o.mAddressW) return mAddressW < o.mAddressW;
    if (mAnisotropy != o.mAnisotropy) return mAnisotropy < o.mAnisotropy;
    if (mCompare != o.mCompare) return mCompare < o.mCompare;
    return mCompareOp < o.mCompareOp;
}

//-----------------------------------------------------------------------------------------------------
// Program
//-----------------------------------------------------------------------------------------------------

bool Program::create(const std::string& name, const ProgramInfo& info)
{
    destroy();
    mName = name;
    mInfo = info;
    Context& ctx = Context::instance();

    for (U32 s = 0; s < 3; ++s)
    {
        if (info.mSpirv[s].empty())
        {
            continue;
        }
        VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = info.mSpirv[s].size() * sizeof(U32);
        mi.pCode = info.mSpirv[s].data();
        if (!vk_ok(vkCreateShaderModule(ctx.getDevice(), &mi, nullptr, &mModules[s]), "vkCreateShaderModule"))
        {
            destroy();
            return false;
        }
    }

    mShadow.assign(std::max<U32>(info.mBlockSize, 16), 0);
    for (size_t i = 0; i < mInfo.mUniforms.size(); ++i)
    {
        mUniformIndex[mInfo.mUniforms[i].mName] = i;
    }
    for (const SamplerInfo& s : mInfo.mSamplers)
    {
        mSamplerIndex[s.mName] = s.mBinding;
    }

    // numeric initializers from the GLSL ("uniform float x = 0.5;") become the starting values
    for (const LooseUniform& u : mInfo.mUniforms)
    {
        if (u.mInit.empty() || u.mArraySize != 0)
        {
            continue;
        }
        char* end = nullptr;
        const double v = strtod(u.mInit.c_str(), &end);
        if (end == u.mInit.c_str())
        {
            continue;
        }
        if (u.mType == "float")
        {
            const F32 f = (F32)v;
            memcpy(&mShadow[u.mOffset], &f, 4);
        }
        else if (u.mType == "int" || u.mType == "uint" || u.mType == "bool")
        {
            const S32 i = (S32)v;
            memcpy(&mShadow[u.mOffset], &i, 4);
        }
    }

    mValid = true;
    mDirty = true;
    return true;
}

void Program::destroy()
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    if (dev != VK_NULL_HANDLE)
    {
        std::vector<VkPipeline> pipes;
        for (auto& kv : mPipelines)
        {
            pipes.push_back(kv.second);
        }
        VkShaderModule modules[3] = { mModules[0], mModules[1], mModules[2] };
        ctx.destroyLater([=]() {
            for (VkPipeline p : pipes) vkDestroyPipeline(dev, p, nullptr);
            for (VkShaderModule m : modules) if (m) vkDestroyShaderModule(dev, m, nullptr);
        });
    }
    mPipelines.clear();
    for (auto& m : mModules) m = VK_NULL_HANDLE;
    mUniformIndex.clear();
    mSamplerIndex.clear();
    mShadow.clear();
    mValid = false;
}

bool Program::setFloats(const std::string& name, const F32* data, U32 components, U32 count)
{
    auto it = mUniformIndex.find(name);
    if (it == mUniformIndex.end() || components < 1 || components > 4)
    {
        return false;
    }
    const LooseUniform& u = mInfo.mUniforms[it->second];
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = u.mArraySize > 0 ? 16 : 4 * components;
    for (U32 i = 0; i < count; ++i)
    {
        memcpy(&mShadow[u.mOffset + i * stride], data + i * components, components * 4);
    }
    mDirty = true;
    return true;
}

bool Program::setInts(const std::string& name, const S32* data, U32 components, U32 count)
{
    return setFloats(name, reinterpret_cast<const F32*>(data), components, count);
}

bool Program::setMatrices(const std::string& name, const F32* data, U32 columns, U32 rows, U32 count)
{
    auto it = mUniformIndex.find(name);
    if (it == mUniformIndex.end() || columns < 2 || columns > 4 || rows < 2 || rows > 4)
    {
        return false;
    }
    const LooseUniform& u = mInfo.mUniforms[it->second];
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = columns * 16;    // std140: every column occupies 16 bytes
    for (U32 m = 0; m < count; ++m)
    {
        for (U32 c = 0; c < columns; ++c)
        {
            memcpy(&mShadow[u.mOffset + m * stride + c * 16], data + (m * columns + c) * rows, rows * 4);
        }
    }
    mDirty = true;
    return true;
}

S32 Program::getSamplerBinding(const std::string& name) const
{
    auto it = mSamplerIndex.find(name);
    return it == mSamplerIndex.end() ? -1 : (S32)it->second;
}

VkPipeline Program::getPipeline(const PipelineKey& key)
{
    auto it = mPipelines.find(key);
    if (it != mPipelines.end())
    {
        return it->second;
    }

    Renderer& r = Renderer::instance();
    Context& ctx = Context::instance();

    VkPipelineShaderStageCreateInfo stages[3] = {};
    U32 stage_count = 0;
    const VkShaderStageFlagBits bits[3] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT };
    for (U32 s = 0; s < 3; ++s)
    {
        if (mModules[s])
        {
            stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[stage_count].stage = bits[s];
            stages[stage_count].module = mModules[s];
            stages[stage_count].pName = "main";
            ++stage_count;
        }
    }

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };   // dynamic
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = key.mTopology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState attachments[8] = {};
    for (U32 i = 0; i < key.mColorCount; ++i)
    {
        attachments[i].colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = key.mColorCount;
    cb.pAttachments = attachments;

    const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
    };
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = (U32)LL_ARRAY_SIZE(dynamic);
    dyn.pDynamicStates = dynamic;

    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = key.mColorCount;
    ri.pColorAttachmentFormats = key.mColor;
    ri.depthAttachmentFormat = key.mDepth;

    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gi.pNext = &ri;
    gi.stageCount = stage_count;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dyn;
    gi.layout = r.mPipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateGraphicsPipelines(ctx.getDevice(), r.mPipelineCache, 1, &gi, nullptr, &pipeline), "vkCreateGraphicsPipelines"))
    {
        LL_WARNS("Vulkan") << "pipeline creation failed for program " << mName << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    mPipelines[key] = pipeline;
    return pipeline;
}

//-----------------------------------------------------------------------------------------------------
// Renderer: setup
//-----------------------------------------------------------------------------------------------------

Renderer& Renderer::instance()
{
    static Renderer sRenderer;
    return sRenderer;
}

bool Renderer::createLayouts()
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();

    // set 0: binding 0 = loose uniforms (dynamic offset into the ring), bindings 1..4 = the named uniform blocks
    VkDescriptorSetLayoutBinding b0[5] = {};
    b0[0].binding = 0;
    b0[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b0[0].descriptorCount = 1;
    b0[0].stageFlags = all_graphics_stages();
    for (U32 i = 1; i < 5; ++i)
    {
        b0[i].binding = i;
        b0[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = all_graphics_stages();
    }
    VkDescriptorSetLayoutCreateInfo l0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l0.bindingCount = 5;
    l0.pBindings = b0;
    if (!vk_ok(vkCreateDescriptorSetLayout(dev, &l0, nullptr, &mSet0Layout), "set 0 layout"))
    {
        return false;
    }

    // set 1: samplers, pushed with the draw
    VkDescriptorSetLayoutBinding b1[MAX_TEXTURE_UNITS] = {};
    for (U32 i = 0; i < MAX_TEXTURE_UNITS; ++i)
    {
        b1[i].binding = i;
        b1[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b1[i].descriptorCount = 1;
        b1[i].stageFlags = all_graphics_stages();
    }
    VkDescriptorSetLayoutCreateInfo l1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l1.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    l1.bindingCount = MAX_TEXTURE_UNITS;
    l1.pBindings = b1;
    if (!vk_ok(vkCreateDescriptorSetLayout(dev, &l1, nullptr, &mSet1Layout), "set 1 layout"))
    {
        return false;
    }

    VkDescriptorSetLayout layouts[2] = { mSet0Layout, mSet1Layout };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 2;
    pl.pSetLayouts = layouts;
    return vk_ok(vkCreatePipelineLayout(dev, &pl, nullptr, &mPipelineLayout), "pipeline layout");
}

bool Renderer::init()
{
    if (mReady)
    {
        return true;
    }
    Context& ctx = Context::instance();
    if (!ctx.isReady() || !createLayouts())
    {
        return false;
    }
    VkDevice dev = ctx.getDevice();

    // uniform rings (one per frame in flight) and a stand-in for named uniform blocks nobody has bound
    for (U32 i = 0; i < Context::FRAMES_IN_FLIGHT; ++i)
    {
        if (!ctx.createBuffer(RING_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Context::Memory::Upload, mRing[i]))
        {
            return false;
        }
    }
    if (!ctx.createBuffer(DUMMY_UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Context::Memory::Upload, mDummyUbo))
    {
        return false;
    }
    memset(mDummyUbo.mMapped, 0, DUMMY_UBO_BYTES);

    // descriptor sets for set 0
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = Context::FRAMES_IN_FLIGHT;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 4 * Context::FRAMES_IN_FLIGHT;
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = Context::FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (!vk_ok(vkCreateDescriptorPool(dev, &pi, nullptr, &mPool), "descriptor pool"))
    {
        return false;
    }
    VkDescriptorSetLayout set_layouts[Context::FRAMES_IN_FLIGHT];
    for (auto& l : set_layouts) l = mSet0Layout;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = mPool;
    ai.descriptorSetCount = Context::FRAMES_IN_FLIGHT;
    ai.pSetLayouts = set_layouts;
    if (!vk_ok(vkAllocateDescriptorSets(dev, &ai, mSet0), "allocate set 0"))
    {
        return false;
    }
    for (U32 i = 0; i < Context::FRAMES_IN_FLIGHT; ++i)
    {
        VkDescriptorBufferInfo ring = { mRing[i].mBuffer, 0, MAX_UNIFORM_RANGE };
        VkDescriptorBufferInfo dummy = { mDummyUbo.mBuffer, 0, DUMMY_UBO_BYTES };
        VkWriteDescriptorSet w[5] = {};
        for (U32 b = 0; b < 5; ++b)
        {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = mSet0[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[b].pBufferInfo = b == 0 ? &ring : &dummy;
        }
        vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
    }

    VkPipelineCacheCreateInfo pc = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (!vk_ok(vkCreatePipelineCache(dev, &pc, nullptr, &mPipelineCache), "pipeline cache"))
    {
        return false;
    }

    // 1x1 white texture: what an unbound texture unit samples
    if (!ctx.createImage2D(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mWhite))
    {
        return false;
    }
    Buffer staging;
    if (!ctx.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Context::Memory::Upload, staging))
    {
        return false;
    }
    memset(staging.mMapped, 0xFF, 4);
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, mWhite.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { 1, 1, 1 };
        vkCmdCopyBufferToImage(cmd, staging.mBuffer, mWhite.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        imageBarrier(cmd, mWhite.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    mWhite.mLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ctx.waitIdle();
    vmaDestroyBuffer(ctx.getAllocator(), staging.mBuffer, staging.mAllocation);

    mReady = true;
    LL_INFOS("Vulkan") << "Vulkan renderer ready" << LL_ENDL;
    return true;
}

void Renderer::shutdown()
{
    Context& ctx = Context::instance();
    if (!mReady && mPipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }
    VkDevice dev = ctx.getDevice();
    if (dev != VK_NULL_HANDLE)
    {
        ctx.waitIdle();
        for (auto& kv : mSamplers) vkDestroySampler(dev, kv.second, nullptr);
        mSamplers.clear();
        if (mPipelineCache) vkDestroyPipelineCache(dev, mPipelineCache, nullptr);
        if (mPool) vkDestroyDescriptorPool(dev, mPool, nullptr);
        if (mPipelineLayout) vkDestroyPipelineLayout(dev, mPipelineLayout, nullptr);
        if (mSet0Layout) vkDestroyDescriptorSetLayout(dev, mSet0Layout, nullptr);
        if (mSet1Layout) vkDestroyDescriptorSetLayout(dev, mSet1Layout, nullptr);
        for (Buffer& b : mRing)
        {
            if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        if (mDummyUbo.valid()) vmaDestroyBuffer(ctx.getAllocator(), mDummyUbo.mBuffer, mDummyUbo.mAllocation);
        mDummyUbo = Buffer();
        if (mWhite.valid())
        {
            vkDestroyImageView(dev, mWhite.mView, nullptr);
            vmaDestroyImage(ctx.getAllocator(), mWhite.mImage, mWhite.mAllocation);
        }
        mWhite = Image();
    }
    mPipelineCache = VK_NULL_HANDLE;
    mPool = VK_NULL_HANDLE;
    mPipelineLayout = VK_NULL_HANDLE;
    mSet0Layout = VK_NULL_HANDLE;
    mSet1Layout = VK_NULL_HANDLE;
    mProgram = nullptr;
    mReady = false;
}

VkSampler Renderer::getSampler(const SamplerDesc& desc)
{
    auto it = mSamplers.find(desc);
    if (it != mSamplers.end())
    {
        return it->second;
    }
    Context& ctx = Context::instance();
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = desc.mMag;
    si.minFilter = desc.mMin;
    si.mipmapMode = desc.mMip;
    si.addressModeU = desc.mAddressU;
    si.addressModeV = desc.mAddressV;
    si.addressModeW = desc.mAddressW;
    si.anisotropyEnable = desc.mAnisotropy > 1.f && ctx.getCaps().mAnisotropy ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = std::min(desc.mAnisotropy, ctx.getCaps().mMaxAnisotropy);
    si.compareEnable = desc.mCompare ? VK_TRUE : VK_FALSE;
    si.compareOp = desc.mCompareOp;
    si.minLod = 0.f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (!vk_ok(vkCreateSampler(ctx.getDevice(), &si, nullptr, &s), "vkCreateSampler"))
    {
        return VK_NULL_HANDLE;
    }
    mSamplers[desc] = s;
    return s;
}

//-----------------------------------------------------------------------------------------------------
// Renderer: frame, targets, clears, layouts
//-----------------------------------------------------------------------------------------------------

void Renderer::beginFrame(VkCommandBuffer cmd, U32 frame_slot)
{
    mCmd = cmd;
    mSlot = frame_slot;
    mRingUsed = 0;
    mDrawCount = 0;
    mRendering = false;
    mPendingClear = 0;
    mStateApplied = false;
    mBoundPipeline = VK_NULL_HANDLE;
    mVertexInputDirty = true;
    mSet0Bound = false;
    mAppliedDynamicOffset = 0xFFFFFFFF;
    for (S32& v : mAppliedViewport) v = -1;
    for (S32& v : mAppliedScissor) v = -1;
    mColorCount = 0;
    mDepthTarget = nullptr;
    if (mProgram)
    {
        mProgram->mDirty = true;     // uniforms must be re-uploaded into this frame's ring
    }
}

void Renderer::endFrame()
{
    flushPendingClear();
    endRendering();
    mCmd = VK_NULL_HANDLE;
}

void Renderer::flushPendingClear()
{
    if (!mRendering && mPendingClear && (mColorCount || mDepthTarget))
    {
        beginRenderingIfNeeded();
    }
}

void Renderer::endRendering()
{
    if (mRendering)
    {
        vkCmdEndRendering(mCmd);
        mRendering = false;
    }
}

void Renderer::setRenderTarget(Image* const* colors, U32 color_count, Image* depth)
{
    bool same = color_count == mColorCount && depth == mDepthTarget;
    for (U32 i = 0; same && i < color_count; ++i)
    {
        same = colors[i] == mColorTargets[i];
    }
    if (same)
    {
        return;
    }
    flushPendingClear();
    endRendering();
    mColorCount = std::min<U32>(color_count, 8);
    for (U32 i = 0; i < mColorCount; ++i)
    {
        mColorTargets[i] = colors[i];
    }
    mDepthTarget = depth;
}

void Renderer::setViewport(S32 x, S32 y, S32 width, S32 height)
{
    mViewport[0] = x; mViewport[1] = y; mViewport[2] = width; mViewport[3] = height;
    mViewportSet = true;
}

void Renderer::setScissor(S32 x, S32 y, S32 width, S32 height)
{
    mScissor[0] = x; mScissor[1] = y; mScissor[2] = width; mScissor[3] = height;
}

void Renderer::setScissorEnabled(bool enabled)
{
    mScissorEnabled = enabled;
}

void Renderer::transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout)
{
    if (image.mLayout == layout)
    {
        return;
    }
    imageBarrier(cmd, image.mImage, image.mAspect, image.mMipLevels, image.mLayers, image.mLayout, layout,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    image.mLayout = layout;
}

void Renderer::ensureLayout(Image& image, VkImageLayout layout)
{
    if (image.mLayout == layout)
    {
        return;
    }
    // a pass that has this image attached must end before the image changes layout
    if (mRendering)
    {
        bool attached = &image == mDepthTarget;
        for (U32 i = 0; i < mColorCount; ++i)
        {
            attached = attached || &image == mColorTargets[i];
        }
        if (attached)
        {
            endRendering();
        }
    }
    else if (mPendingClear && !mInBegin)
    {
        bool attached = &image == mDepthTarget;
        for (U32 i = 0; i < mColorCount; ++i)
        {
            attached = attached || &image == mColorTargets[i];
        }
        if (attached)
        {
            flushPendingClear();     // the clear is recorded before the image changes layout
            endRendering();
        }
    }
    transition(mCmd, image, layout);
}

void Renderer::beginRenderingIfNeeded()
{
    if (mRendering)
    {
        return;
    }
    mInBegin = true;
    VkExtent2D extent = { 0, 0 };
    VkRenderingAttachmentInfo colors[8] = {};
    for (U32 i = 0; i < mColorCount; ++i)
    {
        Image* img = mColorTargets[i];
        ensureLayout(*img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = img->mView;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = (mPendingClear & CLEAR_COLOR) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        memcpy(colors[i].clearValue.color.float32, mClearColor, sizeof(mClearColor));
        extent = { img->mExtent.width, img->mExtent.height };
    }
    VkRenderingAttachmentInfo depth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    if (mDepthTarget)
    {
        ensureLayout(*mDepthTarget, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth.imageView = mDepthTarget->mView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = (mPendingClear & CLEAR_DEPTH) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = { mClearDepth, mClearStencil };
        if (extent.width == 0)
        {
            extent = { mDepthTarget->mExtent.width, mDepthTarget->mExtent.height };
        }
    }

    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = mColorCount;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = mDepthTarget ? &depth : nullptr;
    vkCmdBeginRendering(mCmd, &ri);
    mRendering = true;
    mPendingClear = 0;
    mInBegin = false;
}

void Renderer::clear(U32 bits, const F32 color[4], F32 depth, U32 stencil)
{
    if (bits & CLEAR_COLOR)
    {
        memcpy(mClearColor, color, sizeof(mClearColor));
    }
    if (bits & CLEAR_DEPTH)
    {
        mClearDepth = depth;
    }
    mClearStencil = stencil;

    if (!mRendering)
    {
        mPendingClear |= bits;      // becomes the load op of the next pass
        return;
    }

    // a pass is already running: clear inside it
    VkClearAttachment atts[9] = {};
    U32 n = 0;
    if (bits & CLEAR_COLOR)
    {
        for (U32 i = 0; i < mColorCount; ++i)
        {
            atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            atts[n].colorAttachment = i;
            memcpy(atts[n].clearValue.color.float32, mClearColor, sizeof(mClearColor));
            ++n;
        }
    }
    if ((bits & CLEAR_DEPTH) && mDepthTarget)
    {
        atts[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        atts[n].clearValue.depthStencil = { mClearDepth, mClearStencil };
        ++n;
    }
    if (n == 0)
    {
        return;
    }
    const Image* ref = mColorCount ? mColorTargets[0] : mDepthTarget;
    VkClearRect rect = {};
    rect.rect = { { 0, 0 }, { ref->mExtent.width, ref->mExtent.height } };
    rect.layerCount = 1;
    vkCmdClearAttachments(mCmd, n, atts, 1, &rect);
}

//-----------------------------------------------------------------------------------------------------
// Renderer: state, uniforms, textures, draws
//-----------------------------------------------------------------------------------------------------

void Renderer::setProgram(Program* program)
{
    mProgram = program;
}

void Renderer::setTexture(U32 unit, Image* image, VkSampler sampler)
{
    if (unit < MAX_TEXTURE_UNITS)
    {
        mTextures[unit] = image;
        mTextureSamplers[unit] = sampler;
    }
}

U32 Renderer::allocUniform(U32 size)
{
    const U32 align = (U32)Context::instance().getCaps().mUniformAlignment;
    const U32 start = (mRingUsed + align - 1) / align * align;
    // every dynamic descriptor must be able to see MAX_UNIFORM_RANGE bytes from its offset
    if (start + MAX_UNIFORM_RANGE > RING_BYTES)
    {
        LL_WARNS_ONCE("Vulkan") << "uniform ring exhausted; reusing the start of the ring" << LL_ENDL;
        mRingUsed = 0;
        return allocUniform(size);
    }
    mRingUsed = start + size;
    return start;
}

void Renderer::applyState()
{
    // 1. textures must be in a sampling layout before a pass is (re)started
    if (mProgram)
    {
        for (const SamplerInfo& s : mProgram->mInfo.mSamplers)
        {
            Image* img = s.mBinding < MAX_TEXTURE_UNITS ? mTextures[s.mBinding] : nullptr;
            if (img)
            {
                ensureLayout(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    // 2. pass
    beginRenderingIfNeeded();

    // 3. pipeline
    PipelineKey key;
    key.mTopology = mTopology;
    key.mColorCount = mColorCount;
    for (U32 i = 0; i < mColorCount; ++i)
    {
        key.mColor[i] = mColorTargets[i]->mFormat;
    }
    key.mDepth = mDepthTarget ? mDepthTarget->mFormat : VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = mProgram->getPipeline(key);
    if (pipeline != mBoundPipeline)
    {
        vkCmdBindPipeline(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        mBoundPipeline = pipeline;
    }

    // 4. dynamic state (only what changed since it was last recorded)
    const Image* ref = mColorCount ? mColorTargets[0] : mDepthTarget;
    S32 vp[4] = { mViewport[0], mViewport[1], mViewport[2], mViewport[3] };
    if (!mViewportSet)
    {
        vp[0] = 0; vp[1] = 0; vp[2] = (S32)ref->mExtent.width; vp[3] = (S32)ref->mExtent.height;
    }
    if (memcmp(vp, mAppliedViewport, sizeof(vp)) != 0)
    {
        VkViewport v = { (F32)vp[0], (F32)vp[1], (F32)vp[2], (F32)vp[3], 0.f, 1.f };
        vkCmdSetViewport(mCmd, 0, 1, &v);
        memcpy(mAppliedViewport, vp, sizeof(vp));
    }
    S32 sc[4] = { 0, 0, (S32)ref->mExtent.width, (S32)ref->mExtent.height };
    if (mScissorEnabled)
    {
        memcpy(sc, mScissor, sizeof(sc));
    }
    // scissor must stay inside the target
    sc[0] = std::max(sc[0], 0);
    sc[1] = std::max(sc[1], 0);
    sc[2] = std::max(0, std::min(sc[2], (S32)ref->mExtent.width - sc[0]));
    sc[3] = std::max(0, std::min(sc[3], (S32)ref->mExtent.height - sc[1]));
    if (memcmp(sc, mAppliedScissor, sizeof(sc)) != 0)
    {
        VkRect2D r = { { sc[0], sc[1] }, { (U32)sc[2], (U32)sc[3] } };
        vkCmdSetScissor(mCmd, 0, 1, &r);
        memcpy(mAppliedScissor, sc, sizeof(sc));
    }

    if (!mStateApplied || !(mRaster == mAppliedRaster))
    {
        vkCmdSetCullMode(mCmd, mRaster.mCull);
        vkCmdSetFrontFace(mCmd, mRaster.mFrontFace);
        vkCmdSetPolygonModeEXT(mCmd, mRaster.mPolygonMode);
        vkCmdSetLineWidth(mCmd, Context::instance().getCaps().mWideLines ? mRaster.mLineWidth : 1.f);
        vkCmdSetDepthBiasEnable(mCmd, mRaster.mDepthBias ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBias(mCmd, mRaster.mBiasConstant, 0.f, mRaster.mBiasSlope);
        mAppliedRaster = mRaster;
    }
    if (!mStateApplied || !(mDepthStencil == mAppliedDepthStencil))
    {
        vkCmdSetDepthTestEnable(mCmd, mDepthStencil.mDepthTest ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(mCmd, mDepthStencil.mDepthWrite ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(mCmd, mDepthStencil.mDepthOp);
        vkCmdSetStencilTestEnable(mCmd, mDepthStencil.mStencilTest ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(mCmd, VK_STENCIL_FACE_FRONT_AND_BACK, mDepthStencil.mFail, mDepthStencil.mPass, mDepthStencil.mDepthFail, mDepthStencil.mStencilOp);
        vkCmdSetStencilCompareMask(mCmd, VK_STENCIL_FACE_FRONT_AND_BACK, mDepthStencil.mCompareMask);
        vkCmdSetStencilWriteMask(mCmd, VK_STENCIL_FACE_FRONT_AND_BACK, mDepthStencil.mWriteMask);
        vkCmdSetStencilReference(mCmd, VK_STENCIL_FACE_FRONT_AND_BACK, mDepthStencil.mReference);
        mAppliedDepthStencil = mDepthStencil;
    }
    if (!mStateApplied || !(mBlend == mAppliedBlend))
    {
        const VkBool32 enable = mBlend.mEnable ? VK_TRUE : VK_FALSE;
        VkBool32 enables[8];
        VkColorBlendEquationEXT eqs[8];
        VkColorComponentFlags masks[8];
        for (U32 i = 0; i < 8; ++i)
        {
            enables[i] = enable;
            eqs[i] = { mBlend.mSrcColor, mBlend.mDstColor, mBlend.mColorOp, mBlend.mSrcAlpha, mBlend.mDstAlpha, mBlend.mAlphaOp };
            masks[i] = mBlend.mWriteMask;
        }
        if (mColorCount)
        {
            vkCmdSetColorBlendEnableEXT(mCmd, 0, mColorCount, enables);
            vkCmdSetColorBlendEquationEXT(mCmd, 0, mColorCount, eqs);
            vkCmdSetColorWriteMaskEXT(mCmd, 0, mColorCount, masks);
        }
        mAppliedBlend = mBlend;
    }
    mStateApplied = true;

    // 5. vertex input
    if (mVertexInputDirty)
    {
        VkVertexInputBindingDescription2EXT binding = { VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT };
        binding.binding = 0;
        binding.stride = mLayout.mStride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        binding.divisor = 1;
        VkVertexInputAttributeDescription2EXT attrs[16] = {};
        const U32 n = std::min<U32>((U32)mLayout.mAttributes.size(), 16);
        for (U32 i = 0; i < n; ++i)
        {
            attrs[i].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
            attrs[i].location = mLayout.mAttributes[i].mLocation;
            attrs[i].binding = 0;
            attrs[i].format = mLayout.mAttributes[i].mFormat;
            attrs[i].offset = mLayout.mAttributes[i].mOffset;
        }
        vkCmdSetVertexInputEXT(mCmd, n ? 1 : 0, n ? &binding : nullptr, n, attrs);
        mVertexInputDirty = false;
    }
    if (mVertexBuffer)
    {
        vkCmdBindVertexBuffers(mCmd, 0, 1, &mVertexBuffer, &mVertexOffset);
    }

    // 6. loose uniforms: copy this program's shadow into the ring when it changed (or on its first use this frame)
    if (mProgram->mDirty || mProgram->mRingFrame != Context::instance().getFrameNumber())
    {
        mProgram->mRingOffset = allocUniform((U32)mProgram->mShadow.size());
        memcpy((U8*)mRing[mSlot].mMapped + mProgram->mRingOffset, mProgram->mShadow.data(), mProgram->mShadow.size());
        mProgram->mRingFrame = Context::instance().getFrameNumber();
        mProgram->mDirty = false;
    }
    if (!mSet0Bound || mAppliedDynamicOffset != mProgram->mRingOffset)
    {
        vkCmdBindDescriptorSets(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &mSet0[mSlot], 1, &mProgram->mRingOffset);
        mAppliedDynamicOffset = mProgram->mRingOffset;
        mSet0Bound = true;
    }

    // 7. textures, pushed for exactly the samplers the program declares
    if (!mProgram->mInfo.mSamplers.empty())
    {
        VkDescriptorImageInfo infos[MAX_TEXTURE_UNITS];
        VkWriteDescriptorSet writes[MAX_TEXTURE_UNITS];
        U32 n = 0;
        VkSampler fallback = getSampler(SamplerDesc());
        for (const SamplerInfo& s : mProgram->mInfo.mSamplers)
        {
            if (s.mBinding >= MAX_TEXTURE_UNITS)
            {
                continue;
            }
            Image* img = mTextures[s.mBinding] ? mTextures[s.mBinding] : &mWhite;
            infos[n] = { mTextureSamplers[s.mBinding] ? mTextureSamplers[s.mBinding] : fallback, img->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstBinding = s.mBinding;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkCmdPushDescriptorSetKHR(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 1, n, writes);
    }
}

void Renderer::draw(U32 vertex_count, U32 first_vertex)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget))
    {
        return;
    }
    applyState();
    vkCmdDraw(mCmd, vertex_count, 1, first_vertex, 0);
    ++mDrawCount;
}

void Renderer::drawIndexed(U32 index_count, U32 first_index, S32 vertex_offset)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget) || !mIndexBuffer)
    {
        return;
    }
    applyState();
    vkCmdBindIndexBuffer(mCmd, mIndexBuffer, mIndexOffset, mIndexType);
    vkCmdDrawIndexed(mCmd, index_count, 1, first_index, vertex_offset, 0);
    ++mDrawCount;
}

//-----------------------------------------------------------------------------------------------------
// Textures
//-----------------------------------------------------------------------------------------------------

FormatInfo formatInfo(VkFormat f)
{
    FormatInfo fi;
    fi.mKnown = true;
    switch (f)
    {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: fi.mBytesPerBlock = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: fi.mBytesPerBlock = 2; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_UINT:
        fi.mBytesPerBlock = 4; break;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: fi.mBytesPerBlock = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: fi.mBytesPerBlock = 16; break;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
        fi.mBlockWidth = fi.mBlockHeight = 4; fi.mBytesPerBlock = 8; fi.mCompressed = true; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        fi.mBlockWidth = fi.mBlockHeight = 4; fi.mBytesPerBlock = 16; fi.mCompressed = true; break;
    default:
        fi.mKnown = false;
        break;
    }
    return fi;
}

U32 mipLevelCount(U32 width, U32 height)
{
    U32 n = 1;
    U32 m = std::max(width, height);
    while (m > 1)
    {
        m >>= 1;
        ++n;
    }
    return n;
}

void Renderer::withCommandBuffer(const std::function<void(VkCommandBuffer)>& record)
{
    if (mCmd != VK_NULL_HANDLE)
    {
        endRendering();     // copies and blits are not allowed inside a render pass; the next draw resumes it
        record(mCmd);
        return;
    }
    Context::instance().immediateSubmit(record);
}

bool Renderer::createTexture(const TextureDesc& d, Image& out)
{
    Context& ctx = Context::instance();
    const FormatInfo fi = formatInfo(d.mFormat);
    if (!fi.mKnown && d.mFormat != VK_FORMAT_UNDEFINED)
    {
        // formats outside the table still work for render targets; uploads need the table
        LL_INFOS("Vulkan") << "createTexture: format " << (S32)d.mFormat << " is not in the format table" << LL_ENDL;
    }

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = d.mType == TexType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = d.mFormat;
    ii.mipLevels = d.mMips;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | d.mUsage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    U32 layers = 1;
    U32 depth = 1;
    switch (d.mType)
    {
    case TexType::Tex2D:
        break;
    case TexType::Tex2DArray:
        layers = std::max<U32>(1, d.mDepth);
        view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case TexType::Cube:
        layers = 6;
        view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        break;
    case TexType::CubeArray:
        layers = 6 * std::max<U32>(1, d.mDepth);
        view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        break;
    case TexType::Tex3D:
        depth = std::max<U32>(1, d.mDepth);
        view_type = VK_IMAGE_VIEW_TYPE_3D;
        break;
    }
    ii.extent = { d.mWidth, d.mHeight, depth };
    ii.arrayLayers = layers;

    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    out = Image();
    if (!vk_ok(vmaCreateImage(ctx.getAllocator(), &ii, &ai, &out.mImage, &out.mAllocation, nullptr), "vmaCreateImage (texture)"))
    {
        out = Image();
        return false;
    }
    out.mFormat = d.mFormat;
    out.mAspect = aspectForFormat(d.mFormat);
    out.mExtent = { d.mWidth, d.mHeight, depth };
    out.mMipLevels = d.mMips;
    out.mLayers = layers;
    out.mViewType = view_type;
    out.mLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = out.mImage;
    vi.viewType = view_type;
    vi.format = d.mFormat;
    vi.components = d.mSwizzle;
    vi.subresourceRange = { out.mAspect, 0, d.mMips, 0, layers };
    if (!vk_ok(vkCreateImageView(ctx.getDevice(), &vi, nullptr, &out.mView), "vkCreateImageView (texture)"))
    {
        vmaDestroyImage(ctx.getAllocator(), out.mImage, out.mAllocation);
        out = Image();
        return false;
    }
    return true;
}

bool Renderer::uploadTexture(Image& image, U32 mip, U32 layer, U32 x, U32 y, U32 width, U32 height, U32 depth, const void* data, VkDeviceSize bytes)
{
    Context& ctx = Context::instance();
    const FormatInfo fi = formatInfo(image.mFormat);
    if (!fi.mKnown || mip >= image.mMipLevels)
    {
        return false;
    }

    // tightly packed size of the region
    const U32 bw = (width + fi.mBlockWidth - 1) / fi.mBlockWidth;
    const U32 bh = (height + fi.mBlockHeight - 1) / fi.mBlockHeight;
    const VkDeviceSize expected = (VkDeviceSize)bw * bh * fi.mBytesPerBlock * std::max<U32>(depth, 1);
    if (bytes < expected)
    {
        LL_WARNS("Vulkan") << "uploadTexture: " << bytes << " bytes supplied, " << expected << " needed" << LL_ENDL;
        return false;
    }

    Buffer own;                 // used only when no frame is active
    StagingAlloc stage;
    if (mCmd != VK_NULL_HANDLE)
    {
        stage = ctx.allocStaging(expected, std::max<VkDeviceSize>(4, fi.mBytesPerBlock));
    }
    if (!stage.valid())
    {
        if (!ctx.createBuffer(expected, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Context::Memory::Upload, own))
        {
            return false;
        }
        stage = { own.mBuffer, 0, own.mMapped };
    }
    memcpy(stage.mPtr, data, (size_t)expected);

    const bool is_3d = image.mViewType == VK_IMAGE_VIEW_TYPE_3D;
    withCommandBuffer([&](VkCommandBuffer cmd) {
        transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = {};
        c.bufferOffset = stage.mOffset;
        c.imageSubresource = { image.mAspect, mip, is_3d ? 0 : layer, 1 };
        c.imageOffset = { (S32)x, (S32)y, is_3d ? (S32)layer : 0 };
        c.imageExtent = { width, height, std::max<U32>(depth, 1) };
        vkCmdCopyBufferToImage(cmd, stage.mBuffer, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    if (own.valid())
    {
        ctx.waitIdle();     // the immediate path already waited; this just makes the free unconditional
        vmaDestroyBuffer(ctx.getAllocator(), own.mBuffer, own.mAllocation);
    }
    return true;
}

bool Renderer::generateMipmaps(Image& image)
{
    if (image.mMipLevels < 2)
    {
        return true;
    }
    withCommandBuffer([&](VkCommandBuffer cmd) {
        // level 0 is the source of everything: bring the whole image to TRANSFER_DST first, then walk the chain
        transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const bool is_3d = image.mViewType == VK_IMAGE_VIEW_TYPE_3D;
        S32 w = (S32)image.mExtent.width, h = (S32)image.mExtent.height, dp = (S32)image.mExtent.depth;
        auto level_barrier = [&](U32 level, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image.mImage;
            b.subresourceRange = { image.mAspect, level, 1, 0, image.mLayers };
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        for (U32 i = 1; i < image.mMipLevels; ++i)
        {
            level_barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            const S32 nw = std::max(1, w / 2), nh = std::max(1, h / 2), nd = is_3d ? std::max(1, dp / 2) : 1;
            VkImageBlit blit = {};
            blit.srcSubresource = { image.mAspect, i - 1, 0, image.mLayers };
            blit.srcOffsets[1] = { w, h, is_3d ? dp : 1 };
            blit.dstSubresource = { image.mAspect, i, 0, image.mLayers };
            blit.dstOffsets[1] = { nw, nh, nd };
            vkCmdBlitImage(cmd, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            w = nw; h = nh; dp = is_3d ? nd : dp;
        }
        // every level except the last is SRC, the last is DST: bring them all to SHADER_READ_ONLY
        for (U32 i = 0; i + 1 < image.mMipLevels; ++i)
        {
            level_barrier(i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        level_barrier(image.mMipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image.mLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    });
    return true;
}

} // namespace LLVk

#endif // LL_VULKAN
