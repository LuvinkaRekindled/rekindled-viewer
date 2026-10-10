/**
 * @file llvkrenderer.cpp
 * @brief Vulkan draw machinery (see llvkrenderer.h)
 */

#include "linden_common.h"

#include "llvkrenderer.h"

// <FS> Tracy GPU zones from the Vulkan timestamps. Tracy loads its Vulkan functions through the proc-address tables (our Vulkan is loaded at run time).
#if defined(TRACY_ENABLE)
#define TRACY_VK_USE_SYMBOL_TABLE
#include <tracy/TracyVulkan.hpp>
#endif
#include "llvkasync.h"
#include "llvkdlss.h"
#include "llvkmirror.h"
#include "llvkrt.h"

#if defined(LL_VULKAN)

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <cstring>
#include <cstdlib>
#include "lldir.h"
#include "llfile.h"

namespace LLVk
{

namespace
{
// where the driver's pipeline cache lives between runs; empty when switched off
std::string pipeline_cache_path()
{
    static const std::string path = []() {
        const char* off = getenv("LL_VK_PIPELINE_CACHE");
        if (off && off[0] == '0')
        {
            return std::string();
        }
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_pipeline_cache.bin");
    }();
    return path;
}

// the header every Vulkan pipeline cache starts with (spec: header size, version, vendor, device, UUID)
bool pipeline_cache_header_matches(const std::vector<U8>& data)
{
    if (data.size() < 16 + VK_UUID_SIZE)
    {
        return false;
    }
    U32 header_size = 0, version = 0, vendor = 0, device = 0;
    memcpy(&header_size, data.data(), 4);
    memcpy(&version, data.data() + 4, 4);
    memcpy(&vendor, data.data() + 8, 4);
    memcpy(&device, data.data() + 12, 4);
    const Capabilities& caps = Context::instance().getCaps();
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(Context::instance().getPhysicalDevice(), &props);
    return version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header_size >= 16 + VK_UUID_SIZE && vendor == props.vendorID && device == props.deviceID
           && memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0 && caps.mVendorId == props.vendorID;
}
}

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

const U32 RING_BYTES = 128u * 1024 * 1024;     // per frame in flight (a crowd of rigged avatars uploads tens of MB of skinning matrices per frame)
const U32 DUMMY_UBO_BYTES = 64 * 1024;
const U32 VERTEX_RING_BYTES = 8u * 1024 * 1024;

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
        && mDepthBias == o.mDepthBias && mDepthClamp == o.mDepthClamp && mBiasConstant == o.mBiasConstant && mBiasSlope == o.mBiasSlope;
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
    if (mCompareOp != o.mCompareOp) return mCompareOp < o.mCompareOp;
    if (mMinLod != o.mMinLod) return mMinLod < o.mMinLod;
    if (mMaxLod != o.mMaxLod) return mMaxLod < o.mMaxLod;
    return mLodBias < o.mLodBias;
}

//-----------------------------------------------------------------------------------------------------
// C8: pipeline creation off the draw path
//-----------------------------------------------------------------------------------------------------

namespace
{
bool pipeline_async()
{
    static const bool on = []() {
        const char* v = getenv("LL_VK_PIPELINE_ASYNC");
        return !(v && v[0] == '0');
    }();
    return on;
}

// a few low-priority threads that build pipelines; a build that is still queued when the pool stops is called with cancelled = true
class PipelinePool
{
public:
    void post(std::function<void(bool)> fn, bool urgent)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStopped)
        {
            fn(true);
            return;
        }
        if (mThreads.empty())
        {
            const char* v = getenv("LL_VK_PIPELINE_THREADS");
            const unsigned n = v ? (unsigned)std::max(1, atoi(v)) : 2u;
            for (unsigned i = 0; i < n; ++i)
            {
                mThreads.emplace_back([this]() { loop(); });
            }
        }
        if (urgent)
        {
            mQueue.push_front(std::move(fn));       // a draw is waiting for this one: ahead of the pre-warm builds
        }
        else
        {
            mQueue.push_back(std::move(fn));
        }
        mCv.notify_one();
    }
    void stop()
    {
        std::deque<std::function<void(bool)>> dropped;
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopped = true;
            dropped.swap(mQueue);
            threads.swap(mThreads);
            mCv.notify_all();
        }
        for (auto& fn : dropped) fn(true);
        for (auto& t : threads) t.join();
    }
private:
    void loop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            std::function<void(bool)> fn;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCv.wait(lock, [this]() { return mStopped || !mQueue.empty(); });
                if (mQueue.empty())
                {
                    return;
                }
                fn = std::move(mQueue.front());
                mQueue.pop_front();
            }
            fn(false);
        }
    }
    std::mutex mMutex;
    std::condition_variable mCv;
    std::deque<std::function<void(bool)>> mQueue;
    std::vector<std::thread> mThreads;
    bool mStopped = false;
};
PipelinePool gPipePool;

// results of background builds, taken by the worker thread
struct PipeDone
{
    Program* mProgram;
    PipelineKey mKey;
    VkPipeline mPipeline;
    U64 mMicros;
    bool mPrewarm;
};
std::mutex gPipeDoneMutex;
std::vector<PipeDone> gPipeDone;
std::atomic<bool> gPipeHasDone{ false };
std::set<Program*> gLivePrograms;               // guarded by gPipeDoneMutex

// The pipeline variants (target formats and topology) each program was used with in earlier sessions, <cache>/vk_pipeline_variants.txt: when a program is created
// they are built in the background from the driver's cache, so a warm start has no misses and a cold one compiles them before the first draw asks.
std::mutex gVariantMutex;
std::map<std::string, std::set<PipelineKey>> gVariants;
bool gVariantsDirty = false;

std::string variants_path()
{
    const std::string cache = pipeline_cache_path();
    return cache.empty() ? std::string() : cache.substr(0, cache.find_last_of("/\\") + 1) + "vk_pipeline_variants.txt";
}

void load_variants()
{
    const std::string path = variants_path();
    FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!f)
    {
        return;
    }
    char line[1024];
    U32 loaded = 0;
    std::lock_guard<std::mutex> lock(gVariantMutex);
    while (fgets(line, sizeof(line), f))
    {
        // name \t topology \t color count \t formats (space separated) \t depth format
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        std::vector<std::string> parts;
        for (size_t p = 0;;)
        {
            const size_t t = text.find('\t', p);
            parts.push_back(text.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (parts.size() < 5)
        {
            continue;
        }
        PipelineKey key;
        key.mTopology = (VkPrimitiveTopology)atoi(parts[1].c_str());
        key.mColorCount = (U32)atoi(parts[2].c_str());
        if (key.mColorCount > 8)
        {
            continue;
        }
        const char* c = parts[3].c_str();
        for (U32 i = 0; i < key.mColorCount; ++i)
        {
            char* end = nullptr;
            key.mColor[i] = (VkFormat)strtol(c, &end, 10);
            c = end;
        }
        key.mDepth = (VkFormat)atoi(parts[4].c_str());
        gVariants[parts[0]].insert(key);
        ++loaded;
    }
    fclose(f);
    LL_INFOS("Vulkan") << "pipeline variants recorded by earlier sessions: " << loaded << " for " << gVariants.size() << " programs" << LL_ENDL;
}

void save_variants()
{
    const std::string path = variants_path();
    std::lock_guard<std::mutex> lock(gVariantMutex);
    if (path.empty() || !gVariantsDirty)
    {
        return;
    }
    const std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb"))
    {
        for (const auto& kv : gVariants)
        {
            for (const PipelineKey& k : kv.second)
            {
                fprintf(f, "%s\t%d\t%u\t", kv.first.c_str(), (int)k.mTopology, k.mColorCount);
                for (U32 i = 0; i < k.mColorCount; ++i) fprintf(f, "%s%d", i ? " " : "", (int)k.mColor[i]);
                fprintf(f, "\t%d\n", (int)k.mDepth);
            }
        }
        fclose(f);
        remove(path.c_str());
        rename(tmp.c_str(), path.c_str());
        gVariantsDirty = false;
    }
}

void note_variant(const std::string& name, const PipelineKey& key)
{
    std::lock_guard<std::mutex> lock(gVariantMutex);
    if (gVariants[name].insert(key).second)
    {
        gVariantsDirty = true;
    }
}
} // anonymous namespace

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
    mBindless = info.mBindless;
    mUsesRt = info.mUsesRt;
    for (const SamplerInfo& s : mInfo.mSamplers)
    {
        if (mBindless)
        {
            const bool ok = s.mIndexUniform >= 0 && (size_t)s.mIndexUniform < mInfo.mUniforms.size();
            mSlotOffset.push_back(ok ? mInfo.mUniforms[s.mIndexUniform].mOffset : 0);
            mSamplerTable.push_back(s.mType == "samplerCube" ? 1 : s.mType == "samplerCubeArray" ? 2 : s.mType == "sampler2DShadow" ? 3 : 0);
            if (!ok)
            {
                mBindless = false;
            }
        }
        mSamplerIndex[s.mName] = s.mBinding;
        mSamplerUnits.push_back(s.mBinding);
        {
            U8 cls = 0;
            if (s.mType.find("CubeArray") != std::string::npos) cls = 2;
            else if (s.mType.find("Cube") != std::string::npos) cls = 1;
            else if (s.mType.find("2DArray") != std::string::npos) cls = 3;
            else if (s.mType.find("3D") != std::string::npos) cls = 4;
            else if (s.mType.find("MS") != std::string::npos) cls = 5;
            mSamplerClass.push_back(cls);
        }
    }
    {
        // <FS> how many programs read their textures through the bindless tables (the rest keep the push descriptor path)
        static U32 sPrograms = 0, sBindlessPrograms = 0, sNoSamplers = 0;
        ++sPrograms;
        if (mBindless) ++sBindlessPrograms;
        if (mInfo.mSamplers.empty()) ++sNoSamplers;
        if (sPrograms % 50 == 0)
        {
            LL_INFOS("VkShader") << "programs: " << sPrograms << " created, " << sBindlessPrograms << " with bindless textures, " << sNoSamplers << " without samplers" << LL_ENDL;
        }
    }

    // numeric initializers from the GLSL ("uniform float x = 0.5;") become the starting values
    for (const LooseUniform& u : mInfo.mUniforms)
    {
        if (u.mInit.empty())
        {
            continue;
        }
        if (u.mArraySize != 0 || u.mType == "vec2" || u.mType == "vec3" || u.mType == "vec4")
        {
            // constructor initializers ("vec3[2](vec3(0.1, 0.2, 0.3), ...)"): take the numbers in order, skipping type names and array sizes
            const U32 comps = u.mType == "vec2" ? 2 : u.mType == "vec3" ? 3 : u.mType == "vec4" ? 4 : u.mType == "float" ? 1 : 0;
            if (!comps)
            {
                continue;
            }
            std::vector<F32> values;
            const std::string& text = u.mInit;
            for (size_t p = 0; p < text.size();)
            {
                const char c = text[p];
                if (isalpha((unsigned char)c) || c == '_')
                {
                    while (p < text.size() && (isalnum((unsigned char)text[p]) || text[p] == '_')) ++p;
                }
                else if (c == '[')
                {
                    while (p < text.size() && text[p] != ']') ++p;
                    ++p;
                }
                else if (isdigit((unsigned char)c) || c == '.' || ((c == '-' || c == '+') && p + 1 < text.size() && (isdigit((unsigned char)text[p + 1]) || text[p + 1] == '.')))
                {
                    char* end = nullptr;
                    const double v = strtod(text.c_str() + p, &end);
                    if (end == text.c_str() + p)
                    {
                        ++p;
                        continue;
                    }
                    values.push_back((F32)v);
                    p = (size_t)(end - text.c_str());
                    while (p < text.size() && (text[p] == 'f' || text[p] == 'F')) ++p;
                }
                else
                {
                    ++p;
                }
            }
            const U32 elements = std::max<U32>(1, u.mArraySize);
            const U32 stride = u.mArraySize ? u.mSize / u.mArraySize : 0;
            for (U32 e = 0; e < elements && (size_t)(e + 1) * comps <= values.size(); ++e)
            {
                const U32 offset = u.mOffset + e * stride;
                if (offset + comps * 4 <= mShadow.size())
                {
                    memcpy(&mShadow[offset], &values[(size_t)e * comps], comps * 4);
                }
            }
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
    {
        std::lock_guard<std::mutex> lock(gPipeDoneMutex);
        gLivePrograms.insert(this);
    }
    prewarm();
    return true;
}

void Program::destroy()
{
    // background builds read this program's shader modules: wait for them, then drop what they delivered
    while (mJobs.load(std::memory_order_acquire) > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lock(gPipeDoneMutex);
        gLivePrograms.erase(this);
        for (size_t i = 0; i < gPipeDone.size();)
        {
            if (gPipeDone[i].mProgram == this)
            {
                if (gPipeDone[i].mPipeline && Context::instance().getDevice() != VK_NULL_HANDLE)
                {
                    vkDestroyPipeline(Context::instance().getDevice(), gPipeDone[i].mPipeline, nullptr);     // never bound: nothing in flight uses it
                }
                gPipeDone.erase(gPipeDone.begin() + (std::ptrdiff_t)i);
            }
            else
            {
                ++i;
            }
        }
    }
    mPending.clear();
    mFailed.clear();
    mSkips.clear();
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
    mLastPipeline = VK_NULL_HANDLE;
    for (auto& m : mModules) m = VK_NULL_HANDLE;
    mUniformIndex.clear();
    mSamplerIndex.clear();
    mSamplerUnits.clear();
    mShadow.clear();
    mValid = false;
}

// LL_VK_PHASES: per uniform name, how often it is written and how often the value really changes (classifies uniforms as per pass, per material or per draw)
struct UniformChangeStat { U64 mWrites = 0, mChanges = 0, mBytes = 0; U32 mSize = 0; };
static std::unordered_map<std::string, UniformChangeStat> gUniformChangeStats;
static void note_uniform_write(const LooseUniform& u, bool changed, U32 bytes)
{
    UniformChangeStat& s = gUniformChangeStats[u.mName];
    ++s.mWrites;
    if (changed) { ++s.mChanges; s.mBytes += bytes; }
    s.mSize = std::max<U32>(s.mSize, bytes);
}

bool Program::setFloats(const std::string& name, const F32* data, U32 components, U32 count)
{
    auto it = mUniformIndex.find(name);
    if (it == mUniformIndex.end() || components < 1 || components > 4)
    {
        return false;
    }
    setFloatsAt(it->second, data, components, count);
    return true;
}

void Program::setFloatsAt(size_t uniform_index, const F32* data, U32 components, U32 count)
{
    const LooseUniform& u = mInfo.mUniforms[uniform_index];
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = u.mArraySize > 0 ? 16 : 4 * components;
    bool changed = false;
    for (U32 i = 0; i < count; ++i)
    {
        U8* dst = &mShadow[u.mOffset + i * stride];
        if (memcmp(dst, data + i * components, components * 4) != 0)
        {
            memcpy(dst, data + i * components, components * 4);
            changed = true;
        }
    }
    if (changed)
    {
        mDirty = true;      // the viewer re-sends the same values before most draws: only real changes cost an upload
    }
    if (LLVkMirror::gPhaseTiming) note_uniform_write(u, changed, count * (u.mArraySize > 0 ? 16 : 4 * components));
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
    setMatricesAt(it->second, data, columns, rows, count);
    return true;
}

void Program::setMatricesAt(size_t uniform_index, const F32* data, U32 columns, U32 rows, U32 count)
{
    const LooseUniform& u = mInfo.mUniforms[uniform_index];
    const U32 max_count = std::max<U32>(1, u.mArraySize);
    count = std::min(count, max_count);
    const U32 stride = columns * 16;    // std140: every column occupies 16 bytes
    bool changed = false;
    for (U32 m = 0; m < count; ++m)
    {
        for (U32 c = 0; c < columns; ++c)
        {
            U8* dst = &mShadow[u.mOffset + m * stride + c * 16];
            const F32* src = data + (m * columns + c) * rows;
            if (memcmp(dst, src, rows * 4) != 0)
            {
                memcpy(dst, src, rows * 4);
                changed = true;
            }
        }
    }
    if (changed)
    {
        mDirty = true;
    }
    if (LLVkMirror::gPhaseTiming) note_uniform_write(u, changed, count * columns * 16);
}

S32 Program::findSampler(const std::string& name) const
{
    for (size_t i = 0; i < mInfo.mSamplers.size(); ++i)
    {
        if (mInfo.mSamplers[i].mName == name)
        {
            return (S32)i;
        }
    }
    return -1;
}

S32 Program::getSamplerBinding(const std::string& name) const
{
    auto it = mSamplerIndex.find(name);
    return it == mSamplerIndex.end() ? -1 : (S32)it->second;
}

VkResult Program::createPipelineObject(const PipelineKey& key, VkPipelineCreateFlags flags, VkPipeline& out) const
{
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

    VkDynamicState dynamic[21] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
    };
    U32 dynamic_count = 20;
    if (Context::instance().getCaps().mDepthClamp)
    {
        dynamic[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = dynamic_count;
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

    gi.flags = flags;
    return vkCreateGraphicsPipelines(ctx.getDevice(), r.mPipelineCache, 1, &gi, nullptr, &out);
}

void Program::requestBuild(const PipelineKey& key, bool prewarm)
{
    mPending.insert(key);
    mJobs.fetch_add(1, std::memory_order_acq_rel);
    if (prewarm)
    {
        gSyncStats.mPipePrewarmQueued.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        gSyncStats.mPipeMisses.fetch_add(1, std::memory_order_relaxed);
    }
    Program* self = this;
    gPipePool.post([self, key, prewarm](bool cancelled) {
        if (!cancelled)
        {
            const auto t0 = std::chrono::steady_clock::now();
            VkPipeline p = VK_NULL_HANDLE;
            const VkResult res = self->createPipelineObject(key, 0, p);
            const U64 micros = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> lock(gPipeDoneMutex);
                gPipeDone.push_back({ self, key, res == VK_SUCCESS ? p : VK_NULL_HANDLE, micros, prewarm });
            }
            gPipeHasDone.store(true, std::memory_order_release);
        }
        self->mJobs.fetch_sub(1, std::memory_order_acq_rel);
    }, !prewarm);
}

void Program::prewarm()
{
    if (!pipeline_async() || Renderer::instance().mPipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }
    std::vector<PipelineKey> keys;
    {
        std::lock_guard<std::mutex> lock(gVariantMutex);
        auto it = gVariants.find(mName);
        if (it != gVariants.end())
        {
            keys.assign(it->second.begin(), it->second.end());
        }
    }
    for (const PipelineKey& k : keys)
    {
        if (!mPipelines.count(k) && !mPending.count(k))
        {
            requestBuild(k, true);
        }
    }
}

void Program::drainCompletedPipelines()
{
    if (!gPipeHasDone.load(std::memory_order_acquire))
    {
        return;
    }
    std::vector<PipeDone> done;
    {
        std::lock_guard<std::mutex> lock(gPipeDoneMutex);
        done.swap(gPipeDone);
        gPipeHasDone.store(false, std::memory_order_release);
    }
    for (PipeDone& d : done)
    {
        bool live;
        {
            std::lock_guard<std::mutex> lock(gPipeDoneMutex);
            live = gLivePrograms.count(d.mProgram) != 0;
        }
        if (!live)
        {
            if (d.mPipeline) vkDestroyPipeline(Context::instance().getDevice(), d.mPipeline, nullptr);
            continue;
        }
        Program& p = *d.mProgram;
        p.mPending.erase(d.mKey);
        U32 skipped = 0;
        auto sk = p.mSkips.find(d.mKey);
        if (sk != p.mSkips.end())
        {
            skipped = sk->second;
            p.mSkips.erase(sk);
        }
        if (!d.mPipeline)
        {
            p.mFailed.insert(d.mKey);
            LL_WARNS("Vulkan") << "pipeline creation failed in the background for program " << p.mName << LL_ENDL;
            continue;
        }
        p.mPipelines[d.mKey] = d.mPipeline;
        Renderer::instance().mPipelinesSinceSave++;
        if (d.mPrewarm)
        {
            gSyncStats.mPipePrewarmDone.fetch_add(1, std::memory_order_relaxed);
            gSyncStats.mPipePrewarmMicros.fetch_add(d.mMicros, std::memory_order_relaxed);
        }
        else
        {
            gSyncStats.mPipeBgDone.fetch_add(1, std::memory_order_relaxed);
            gSyncStats.mPipeBgMicros.fetch_add(d.mMicros, std::memory_order_relaxed);
            U64 prev = gSyncStats.mPipeBgMaxMicros.load(std::memory_order_relaxed);
            while (d.mMicros > prev && !gSyncStats.mPipeBgMaxMicros.compare_exchange_weak(prev, d.mMicros, std::memory_order_relaxed)) {}
            note_variant(p.mName, d.mKey);
            LL_INFOS("VkPipeline") << "pipeline built in the background: " << p.mName << " (" << d.mKey.mColorCount << " colour targets) took " << (double)d.mMicros / 1000.0
                                   << " ms; draws dropped meanwhile: " << skipped << LL_ENDL;
        }
    }
}

void Program::stopPipelineThreads()
{
    gPipePool.stop();
}

VkPipeline Program::getPipeline(const PipelineKey& key)
{
    if (mLastPipeline && mLastKey.mTopology == key.mTopology && mLastKey.mColorCount == key.mColorCount && mLastKey.mDepth == key.mDepth
        && memcmp(mLastKey.mColor, key.mColor, sizeof(VkFormat) * key.mColorCount) == 0)
    {
        return mLastPipeline;
    }
    auto it = mPipelines.find(key);
    if (it == mPipelines.end() && pipeline_async())
    {
        drainCompletedPipelines();
        it = mPipelines.find(key);
    }
    if (it != mPipelines.end())
    {
        mLastKey = key;
        mLastPipeline = it->second;
        return it->second;
    }

    const bool async = pipeline_async();
    if (async)
    {
        if (mFailed.count(key))
        {
            return VK_NULL_HANDLE;
        }
        if (mPending.count(key))
        {
            ++mSkips[key];
            gSyncStats.mPipeSkippedDraws.fetch_add(1, std::memory_order_relaxed);
            return VK_NULL_HANDLE;
        }
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto build_start = std::chrono::steady_clock::now();
    // The driver builds from its cache without compiling when it can (the flag makes it say so instead of compiling when it cannot)
    const VkResult res = createPipelineObject(key, async ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0, pipeline);
    if (async && res == VK_PIPELINE_COMPILE_REQUIRED)
    {
        requestBuild(key, false);
        ++mSkips[key];
        gSyncStats.mPipeSkippedDraws.fetch_add(1, std::memory_order_relaxed);
        return VK_NULL_HANDLE;
    }
    if (!vk_ok(res, "vkCreateGraphicsPipelines"))
    {
        LL_WARNS("Vulkan") << "pipeline creation failed for program " << mName << LL_ENDL;
        if (async)
        {
            mFailed.insert(key);
        }
        return VK_NULL_HANDLE;
    }
    {
        const U64 micros = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_start).count();
        gSyncStats.mPipelinesBuilt.fetch_add(1, std::memory_order_relaxed);
        gSyncStats.mPipelineBuildMicros.fetch_add(micros, std::memory_order_relaxed);
        U64 prev = gSyncStats.mPipelineBuildMaxMicros.load(std::memory_order_relaxed);
        while (micros > prev && !gSyncStats.mPipelineBuildMaxMicros.compare_exchange_weak(prev, micros, std::memory_order_relaxed)) {}
        Renderer::instance().mPipelinesSinceSave++;
    }
    note_variant(mName, key);
    mPipelines[key] = pipeline;
    mLastKey = key;
    mLastPipeline = pipeline;
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

    // <FS> set 2: a table of sampled images that shaders index at run time, written as a frame needs them (render packets with textures). Not needed to run.
    if (const U32 slots = ctx.getCaps().mBindlessSlots)
    {
        VkDescriptorSetLayoutBinding b2[BINDLESS_TABLES] = {};
        VkDescriptorBindingFlags binding_flags[BINDLESS_TABLES];
        for (U32 t = 0; t < BINDLESS_TABLES; ++t)
        {
            b2[t].binding = t;      // 0: 2D (render packets and bindless programs), 1: cube, 2: cube array, 3: 2D shadow
            b2[t].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b2[t].descriptorCount = slots;
            b2[t].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            binding_flags[t] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        fi.bindingCount = BINDLESS_TABLES;
        fi.pBindingFlags = binding_flags;
        VkDescriptorSetLayoutCreateInfo l2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        l2.pNext = &fi;
        l2.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        l2.bindingCount = BINDLESS_TABLES;
        l2.pBindings = b2;
        bool ok = vk_ok(vkCreateDescriptorSetLayout(dev, &l2, nullptr, &mSet2Layout), "set 2 layout");
        if (ok)
        {
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slots * BINDLESS_TABLES * Context::frameCount() };
            VkDescriptorPoolCreateInfo bi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            bi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            bi.maxSets = Context::frameCount();
            bi.poolSizeCount = 1;
            bi.pPoolSizes = &ps;
            ok = vk_ok(vkCreateDescriptorPool(dev, &bi, nullptr, &mBindlessPool), "bindless pool");
        }
        if (ok)
        {
            VkDescriptorSetLayout set_layouts[Context::FRAMES_IN_FLIGHT];
            for (auto& l : set_layouts) l = mSet2Layout;
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = mBindlessPool;
            ai.descriptorSetCount = Context::frameCount();
            ai.pSetLayouts = set_layouts;
            ok = vk_ok(vkAllocateDescriptorSets(dev, &ai, mBindlessSet), "allocate bindless sets");
        }
        if (ok)
        {
            mBindlessCapacity = slots;
            LL_INFOS("Vulkan") << "bindless sampled image tables: " << BINDLESS_TABLES << " x " << slots << " slots per frame (2D, cube, cube array, 2D shadow)" << LL_ENDL;
        }
        else
        {
            if (mBindlessPool) vkDestroyDescriptorPool(dev, mBindlessPool, nullptr);
            if (mSet2Layout) vkDestroyDescriptorSetLayout(dev, mSet2Layout, nullptr);
            mBindlessPool = VK_NULL_HANDLE;
            mSet2Layout = VK_NULL_HANDLE;
            LL_WARNS("Vulkan") << "the bindless sampled image table could not be made: render packets with textures stay off" << LL_ENDL;
        }
    }

    // <FS> set 3: the ray tracing scene (ray query shadows, LL_VK_RT_SHADOWS); without it the layout is what it was
    {
        const Capabilities& caps = ctx.getCaps();
        if (mSet2Layout && Rt::shadowMode() && caps.mRt.mEnabled && caps.mRt.mUsable)
        {
            // binding 0: the acceleration structure; binding 1: the geometry records of the masked structures (a dynamic offset picks the region of the scene call)
            VkDescriptorSetLayoutBinding b3[2] = {};
            b3[0].binding = 0;
            b3[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            b3[0].descriptorCount = 1;
            b3[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            b3[1].binding = 1;
            b3[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            b3[1].descriptorCount = 1;
            b3[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo l3 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            l3.bindingCount = 2;
            l3.pBindings = b3;
            if (!vk_ok(vkCreateDescriptorSetLayout(dev, &l3, nullptr, &mSet3Layout), "set 3 layout"))
            {
                mSet3Layout = VK_NULL_HANDLE;
            }
        }
    }
    VkDescriptorSetLayout layouts[4] = { mSet0Layout, mSet1Layout, mSet2Layout, mSet3Layout };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = mSet3Layout ? 4 : mSet2Layout ? 3 : 2;
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
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        // LL_VK_RING_MEM=host (write-combined system memory) or cached (CPU-cached system memory) puts the uniform ring there instead of video memory behind the BAR
        static const char* const ring_mem = getenv("LL_VK_RING_MEM");
        const Context::Memory ring_memory = ring_mem && strcmp(ring_mem, "host") == 0 ? Context::Memory::Upload : (ring_mem && strcmp(ring_mem, "cached") == 0 ? Context::Memory::Readback : Context::Memory::UploadDevice);
        if (!ctx.createBuffer(RING_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ring_memory, mRing[i]))
        {
            return false;
        }
    }
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        if (!ctx.createBuffer(VERTEX_RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Context::Memory::UploadDevice, mVertexRing[i]))
        {
            return false;
        }
    }
    if (!ctx.createBuffer(DUMMY_UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Context::Memory::UploadDevice, mDummyUbo))
    {
        return false;
    }
    memset(mDummyUbo.mMapped, 0, DUMMY_UBO_BYTES);

    // descriptor sets for set 0
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = Context::frameCount();
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 4 * Context::frameCount();
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = Context::frameCount();
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
    ai.descriptorSetCount = Context::frameCount();
    ai.pSetLayouts = set_layouts;
    if (!vk_ok(vkAllocateDescriptorSets(dev, &ai, mSet0), "allocate set 0"))
    {
        return false;
    }
    for (U32 i = 0; i < Context::frameCount(); ++i)
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

    // <FS> the ray tracing scene (an empty structure per frame slot until the viewer asks for one)
    if (mSet3Layout && !Rt::init(mSet3Layout))
    {
        LL_WARNS("Vulkan") << "ray query shadows: the scene could not be made; programs that trace will have no set 3" << LL_ENDL;
    }

    // descriptor sets for the combinations of named uniform blocks bound during a frame (reset every frame)
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        VkDescriptorPoolSize usizes[2] = {};
        usizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        usizes[0].descriptorCount = UBO_SET_POOL;
        usizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        usizes[1].descriptorCount = 4 * UBO_SET_POOL;
        VkDescriptorPoolCreateInfo upi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        upi.maxSets = UBO_SET_POOL;
        upi.poolSizeCount = 2;
        upi.pPoolSizes = usizes;
        if (!vk_ok(vkCreateDescriptorPool(dev, &upi, nullptr, &mUboPool[i]), "uniform block descriptor pool"))
        {
            return false;
        }
    }

    VkPipelineCacheCreateInfo pc = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    std::vector<U8> cache_data;
    {
        const std::string path = pipeline_cache_path();
        if (!path.empty())
        {
            if (FILE* f = fopen(path.c_str(), "rb"))
            {
                fseek(f, 0, SEEK_END);
                const long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0 && size < (256l << 20))
                {
                    cache_data.resize((size_t)size);
                    if (fread(cache_data.data(), 1, cache_data.size(), f) != cache_data.size())
                    {
                        cache_data.clear();
                    }
                }
                fclose(f);
            }
            // only data written by this driver on this device is handed over (the header carries the vendor, device and cache UUID)
            if (!pipeline_cache_header_matches(cache_data))
            {
                if (!cache_data.empty())
                {
                    LL_INFOS("Vulkan") << "the pipeline cache on disk is from another driver or device and is ignored" << LL_ENDL;
                }
                cache_data.clear();
            }
        }
    }
    pc.initialDataSize = cache_data.size();
    pc.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    if (!vk_ok(vkCreatePipelineCache(dev, &pc, nullptr, &mPipelineCache), "pipeline cache"))
    {
        return false;
    }
    LL_INFOS("Vulkan") << "pipeline cache: " << cache_data.size() / 1024 << " KB loaded from disk" << LL_ENDL;
    if (pipeline_async())
    {
        load_variants();
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

    // white stand-ins for samplers of other kinds (an unbound cube-array sampler must still see a cube-array view)
    {
        const TexType types[4] = { TexType::Tex2DArray, TexType::Cube, TexType::CubeArray, TexType::Tex3D };
        const U32 layers[4] = { 1, 6, 6, 1 };
        const U8 white[4] = { 255, 255, 255, 255 };
        for (U32 i = 0; i < 4; ++i)
        {
            TextureDesc td;
            td.mType = types[i];
            td.mWidth = td.mHeight = 1;
            td.mDepth = 1;
            if (createTexture(td, mFallback[i]))
            {
                for (U32 l = 0; l < layers[i]; ++l)
                {
                    uploadTexture(mFallback[i], 0, l, 0, 0, 1, 1, 1, white, 4);
                }
            }
        }
    }

    createQueryPools();     // optional: without them queries just report nothing

    mReady = true;
    tracyInit();
    LL_INFOS("Vulkan") << "Vulkan renderer ready" << LL_ENDL;
    return true;
}

void Renderer::gpuZone(const char* name, bool begin)
{
    if (!LLVkMirror::gPhaseTiming)
    {
        return;
    }
    if (begin)
    {
        // the shadow zones also name the render target, so that the cascades show up separately in the report
        mZoneStack.push_back(Context::stampTag(std::string("zone: ") + name + (strncmp(name, "shadow", 6) == 0 ? " " + describeTarget() : std::string())));
        Context::instance().gpuStamp(mZoneStack.back());
    }
    else if (!mZoneStack.empty())
    {
        mZoneStack.pop_back();
        Context::instance().gpuStamp(mZoneStack.empty() ? mPassTag : mZoneStack.back());      // (inside a pass that spans the zone the pass keeps its own entry)
    }
}

void Renderer::tracyInit()
{
#if defined(TRACY_ENABLE)
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    // Tracy records and submits a command buffer of its own to line the GPU clock up with the CPU clock: give it a throw-away one
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = ctx.getQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev, &pi, nullptr, &pool) != VK_SUCCESS)
    {
        return;
    }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) == VK_SUCCESS)
    {
        mTracyCtx = TracyVkContext(ctx.getInstance(), ctx.getPhysicalDevice(), dev, ctx.getQueue(), cmd, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
        if (mTracyCtx)
        {
            tracy::VkCtx* tctx = (tracy::VkCtx*)mTracyCtx;
            TracyVkContextName(tctx, "Vulkan", 6);
            LL_INFOS("Vulkan") << "Tracy GPU zones from Vulkan timestamps are available (one zone per render pass while a Tracy client is connected)" << LL_ENDL;
        }
    }
    vkDestroyCommandPool(dev, pool, nullptr);
#endif
}

void Renderer::tracyShutdown()
{
#if defined(TRACY_ENABLE)
    LL_INFOS("Vulkan") << "Tracy: " << mTracyZones << " GPU zones recorded while a client was connected" << LL_ENDL;
    tracyEndPass();
    if (mTracyCtx)
    {
        TracyVkDestroy((tracy::VkCtx*)mTracyCtx);
        mTracyCtx = nullptr;
    }
#endif
}

void Renderer::tracyBeginPass()
{
#if defined(TRACY_ENABLE)
    static_assert(sizeof(tracy::VkCtxScope) <= sizeof(Renderer::mTracyScope), "grow mTracyScope");
    if (!mTracyCtx || mTracyScopeLive)
    {
        return;
    }
#if defined(TRACY_ON_DEMAND)
    if (!tracy::GetProfiler().IsConnected())
    {
        return;
    }
#endif
    const std::string name = describeTarget();
    new (mTracyScope) tracy::VkCtxScope((tracy::VkCtx*)mTracyCtx, __LINE__, __FILE__, strlen(__FILE__), "render pass", 11, name.c_str(), name.size(), mCmd, true);
    mTracyScopeLive = true;
    if (++mTracyZones == 1)
    {
        LL_INFOS("Vulkan") << "Tracy: first GPU zone recorded (\"" << name << "\")" << LL_ENDL;
    }
#endif
}

void Renderer::tracyEndPass()
{
#if defined(TRACY_ENABLE)
    if (mTracyScopeLive)
    {
        ((tracy::VkCtxScope*)mTracyScope)->~VkCtxScope();
        mTracyScopeLive = false;
    }
#endif
}

bool Renderer::createQueryPools()
{
    VkDevice dev = Context::instance().getDevice();
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = MAX_OCC_QUERIES;
        if (vkCreateQueryPool(dev, &qi, nullptr, &mOccPool[i]) != VK_SUCCESS)
        {
            mOccPool[i] = VK_NULL_HANDLE;
            return false;
        }
        vkResetQueryPool(dev, mOccPool[i], 0, MAX_OCC_QUERIES);
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = MAX_TS_QUERIES;
        if (vkCreateQueryPool(dev, &qi, nullptr, &mTsPool[i]) != VK_SUCCESS)
        {
            mTsPool[i] = VK_NULL_HANDLE;
            return false;
        }
        vkResetQueryPool(dev, mTsPool[i], 0, MAX_TS_QUERIES);
        mOccSerial[i].reserve(MAX_OCC_QUERIES);
        mOccBegun[i].reserve(MAX_OCC_QUERIES);
    }
    return true;
}

void Renderer::destroyQueryPools()
{
    VkDevice dev = Context::instance().getDevice();
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        if (mOccPool[i]) vkDestroyQueryPool(dev, mOccPool[i], nullptr);
        if (mTsPool[i]) vkDestroyQueryPool(dev, mTsPool[i], nullptr);
        mOccPool[i] = VK_NULL_HANDLE;
        mTsPool[i] = VK_NULL_HANDLE;
        mOccSerial[i].clear(); mOccBegun[i].clear(); mTsSerial[i].clear(); mTsIsEnd[i].clear();
    }
    mOccPending = mOccActive = -1;
}

// the work recorded the last time this frame slot was used has finished: collect its query results, then reset the pools
void Renderer::harvestQueries(U32 slot)
{
    VkDevice dev = Context::instance().getDevice();
    if (mOccPool[slot] && !mOccSerial[slot].empty())
    {
        const U32 n = (U32)mOccSerial[slot].size();
        std::vector<U64> data((size_t)n * 2, 0);
        vkGetQueryPoolResults(dev, mOccPool[slot], 0, n, data.size() * sizeof(U64), data.data(), 2 * sizeof(U64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);      // NOT_READY just leaves some entries unavailable
        for (U32 i = 0; i < n; ++i)
        {
            if (data[(size_t)i * 2 + 1])
            {
                mQueryResults.push_back({ mOccSerial[slot][i], 0, data[(size_t)i * 2] });
            }
            else if (!mOccBegun[slot][i])
            {
                mQueryResults.push_back({ mOccSerial[slot][i], 0, 0 });        // no draw happened inside it: nothing passed
            }
        }
        vkResetQueryPool(dev, mOccPool[slot], 0, n);
        mOccSerial[slot].clear();
        mOccBegun[slot].clear();
    }
    if (mTsPool[slot] && !mTsSerial[slot].empty())
    {
        const U32 n = (U32)mTsSerial[slot].size();
        std::vector<U64> data((size_t)n * 2, 0);
        vkGetQueryPoolResults(dev, mTsPool[slot], 0, n, data.size() * sizeof(U64), data.data(), 2 * sizeof(U64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (U32 i = 0; i < n; ++i)
        {
            if (data[(size_t)i * 2 + 1])
            {
                mQueryResults.push_back({ mTsSerial[slot][i], mTsIsEnd[slot][i] ? 2u : 1u, data[(size_t)i * 2] });
            }
        }
        vkResetQueryPool(dev, mTsPool[slot], 0, n);
        mTsSerial[slot].clear();
        mTsIsEnd[slot].clear();
    }
}

void Renderer::resolveQueries()
{
    if (mCmd != VK_NULL_HANDLE)
    {
        flushAndRestart();
    }
    Context& ctx = Context::instance();
    ctx.waitIdle();
    for (U32 slot = 0; slot < Context::frameCount(); ++slot)
    {
        if (slot != mSlot || mCmd == VK_NULL_HANDLE)
        {
            harvestQueries(slot);       // the work that used these pools has finished
        }
    }
}

bool Renderer::queryBegin(U64 serial)
{
    if (mCmd == VK_NULL_HANDLE || !mOccPool[mSlot] || mOccSerial[mSlot].size() >= MAX_OCC_QUERIES)
    {
        return false;
    }
    if (mOccActive >= 0 && mRendering)
    {
        vkCmdEndQuery(mCmd, mOccPool[mSlot], (U32)mOccActive);      // GL does not nest these; do not leave one open
    }
    mOccActive = -1;
    mOccPending = (S32)mOccSerial[mSlot].size();
    mOccSerial[mSlot].push_back(serial);
    mOccBegun[mSlot].push_back(0);
    return true;
}

void Renderer::startPendingQuery()
{
    if (mOccPending >= 0 && mRendering)
    {
        vkCmdBeginQuery(mCmd, mOccPool[mSlot], (U32)mOccPending, 0);
        mOccBegun[mSlot][(size_t)mOccPending] = 1;
        mOccActive = mOccPending;
        mOccPending = -1;
    }
}

void Renderer::queryEnd()
{
    if (mOccActive >= 0 && mRendering)
    {
        vkCmdEndQuery(mCmd, mOccPool[mSlot], (U32)mOccActive);
    }
    mOccActive = -1;
    mOccPending = -1;
}

bool Renderer::queryTimestamp(U64 serial, bool end)
{
    if (mCmd == VK_NULL_HANDLE || !mTsPool[mSlot] || mTsSerial[mSlot].size() >= MAX_TS_QUERIES)
    {
        return false;
    }
    const U32 index = (U32)mTsSerial[mSlot].size();
    mTsSerial[mSlot].push_back(serial);
    mTsIsEnd[mSlot].push_back(end ? 1 : 0);
    vkCmdWriteTimestamp2(mCmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mTsPool[mSlot], index);
    return true;
}

void Renderer::takeQueryResults(std::vector<QueryResult>& out)
{
    if (mQueryResults.size() > 400000)
    {
        mQueryResults.erase(mQueryResults.begin(), mQueryResults.begin() + 200000);
    }
    out.insert(out.end(), mQueryResults.begin(), mQueryResults.end());
    mQueryResults.clear();
}

void Renderer::shutdown()
{
    Context& ctx = Context::instance();
    if (!mReady && mPipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }
    VkDevice dev = ctx.getDevice();
    Program::stopPipelineThreads();
    if (dev != VK_NULL_HANDLE)
    {
        ctx.waitIdle();
        Rt::shutdown();
        tracyShutdown();
        destroyQueryPools();
        for (auto& kv : mSamplers) vkDestroySampler(dev, kv.second, nullptr);
        mSamplers.clear();
        savePipelineCache();
        if (mPipelineCache) vkDestroyPipelineCache(dev, mPipelineCache, nullptr);
        if (mPool) vkDestroyDescriptorPool(dev, mPool, nullptr);
        for (VkDescriptorPool& p : mUboPool)
        {
            if (p) vkDestroyDescriptorPool(dev, p, nullptr);
            p = VK_NULL_HANDLE;
        }
        if (mPipelineLayout) vkDestroyPipelineLayout(dev, mPipelineLayout, nullptr);
        if (mSet0Layout) vkDestroyDescriptorSetLayout(dev, mSet0Layout, nullptr);
        if (mSet1Layout) vkDestroyDescriptorSetLayout(dev, mSet1Layout, nullptr);
        if (mBindlessPool) vkDestroyDescriptorPool(dev, mBindlessPool, nullptr);
        if (mSet2Layout) vkDestroyDescriptorSetLayout(dev, mSet2Layout, nullptr);
        if (mSet3Layout) vkDestroyDescriptorSetLayout(dev, mSet3Layout, nullptr);
        for (Buffer& b : mRing)
        {
            if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        for (Buffer& b : mVertexRing)
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
        for (Image& f : mFallback)
        {
            if (f.valid())
            {
                vkDestroyImageView(dev, f.mView, nullptr);
                vmaDestroyImage(ctx.getAllocator(), f.mImage, f.mAllocation);
            }
            f = Image();
        }
    }
    mPipelineCache = VK_NULL_HANDLE;
    mPool = VK_NULL_HANDLE;
    mPipelineLayout = VK_NULL_HANDLE;
    mSet0Layout = VK_NULL_HANDLE;
    mSet1Layout = VK_NULL_HANDLE;
    mSet2Layout = VK_NULL_HANDLE;
    mSet3Layout = VK_NULL_HANDLE;
    mRtBoundSet = VK_NULL_HANDLE;
    mBindlessPool = VK_NULL_HANDLE;
    for (BindlessTable& t : mBindlessTbl) { t.mMap.clear(); t.mInfos.clear(); t.mWritten = 0; }
    mProgram = nullptr;
    mReady = false;
}

void Renderer::savePipelineCache()
{
    const std::string path = pipeline_cache_path();
    if (path.empty() || !mPipelineCache)
    {
        return;
    }
    VkDevice dev = Context::instance().getDevice();
    size_t size = 0;
    if (vkGetPipelineCacheData(dev, mPipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
    {
        return;
    }
    std::vector<U8> data(size);
    if (vkGetPipelineCacheData(dev, mPipelineCache, &size, data.data()) != VK_SUCCESS)
    {
        return;
    }
    data.resize(size);
    const std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb"))
    {
        const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        if (ok)
        {
            remove(path.c_str());
            rename(tmp.c_str(), path.c_str());
            LL_INFOS("Vulkan") << "pipeline cache saved: " << data.size() / 1024 << " KB (" << mPipelinesSinceSave << " pipelines built since the last save)" << LL_ENDL;
            mPipelinesSinceSave = 0;
        }
    }
    save_variants();
}

VkSampler Renderer::getSampler(const SamplerDesc& wanted)
{
    SamplerDesc desc = wanted;
    desc.mLodBias = (!desc.mCompare && desc.mMip == VK_SAMPLER_MIPMAP_MODE_LINEAR) ? LLVkDlss::lodBias() : 0.f;      // DLSS: sample textures sharper to make up for the smaller render size
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
    si.minLod = desc.mMinLod;
    si.maxLod = desc.mMaxLod;
    si.mipLodBias = desc.mLodBias;
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
    mZoneStack.clear();         // GPU zones do not span command buffers
    Program::drainCompletedPipelines();
    if (mPipelinesSinceSave > 0 && ++mPipelineCacheLastSaveFrame >= 18000)
    {
        mPipelineCacheLastSaveFrame = 0;
        savePipelineCache();
    }
    harvestQueries(frame_slot);
    mOccPending = mOccActive = -1;
    mCmd = cmd;
    mSlot = frame_slot;
    // buffer writes made between frames get their staging space now
    if (!mDeferredCopies.empty())
    {
        std::vector<DeferredCopy> deferred;
        deferred.swap(mDeferredCopies);
        for (DeferredCopy& d : deferred)
        {
            if (d.mFillSize)
            {
                fillBuffer(d.mDst, d.mDstOffset, d.mFillSize, d.mFillValue);
            }
            else
            {
                updateBuffer(d.mDst, d.mDstOffset, (VkDeviceSize)d.mData.size(), d.mData.data());
            }
        }
    }
    mRingPeak = std::max(mRingPeak, mRingUsed);
    mRingUsed = 0;
    mVertexRingUsed = 0;
    mDrawCount = 0;
    mRendering = false;
    mPendingClear = 0;
    mStateApplied = false;
    mDepthRO = false;
    mAppliedColorCount = 0xFFFF;
    mBoundPipeline = VK_NULL_HANDLE;
    mBoundIndexBuffer = VK_NULL_HANDLE;
    mUboKeyDirty = true;
    mLastUboSet = VK_NULL_HANDLE;
    mVertexInputDirty = true;
    mSet0Bound = false;
    if (mSet2Layout)
    {
        // the table starts over: this frame's slots are handed out again from 1
        ++mBindlessEpoch;
        for (BindlessTable& t : mBindlessTbl) { t.mMap.clear(); t.mInfos.clear(); t.mWritten = 0; }
        mBindlessBound = false;
    }
    mRtBoundSet = VK_NULL_HANDLE;
    mAppliedDynamicOffset = 0xFFFFFFFF;
    mAppliedSet0 = VK_NULL_HANDLE;
    mAppliedVertexInputValid = false;
    memset(mPushed, 0, sizeof(mPushed));
    mUboSets[frame_slot].clear();
    if (mUboPool[frame_slot])
    {
        vkResetDescriptorPool(Context::instance().getDevice(), mUboPool[frame_slot], 0);
    }
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
#if defined(TRACY_ENABLE)
    if (mTracyCtx && mCmd != VK_NULL_HANDLE)
    {
        tracy::VkCtx* tctx = (tracy::VkCtx*)mTracyCtx;
        TracyVkCollect(tctx, mCmd);      // hands the finished timestamps to Tracy and resets the queries that were read
    }
#endif
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
        if (mOccActive >= 0)
        {
            vkCmdEndQuery(mCmd, mOccPool[mSlot], (U32)mOccActive);      // a query cannot outlive its render pass instance
            mOccActive = -1;
        }
        vkCmdEndRendering(mCmd);
        mRendering = false;
        tracyEndPass();
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
    ++mStateGen;
}

void Renderer::setScissor(S32 x, S32 y, S32 width, S32 height)
{
    mScissor[0] = x; mScissor[1] = y; mScissor[2] = width; mScissor[3] = height;
    ++mStateGen;
}

void Renderer::setScissorEnabled(bool enabled)
{
    mScissorEnabled = enabled;
    ++mStateGen;
}

static Image* root_of(Image* i)
{
    return i->mOwner ? i->mOwner : i;
}

// What a layout transition has to wait for and to block, by the layout the image leaves and the one it enters. Waiting for "all commands" at every
// transition makes the GPU drain completely between passes; naming the stages that really touch the image lets unrelated work overlap.
// LL_VK_COARSE_BARRIERS=1 goes back to the old all-commands barrier.
namespace
{
struct LayoutSync
{
    VkPipelineStageFlags2 mStage;
    VkAccessFlags2 mAccess;
};

constexpr VkPipelineStageFlags2 SHADER_STAGES = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags2 DEPTH_STAGES = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr LayoutSync CONSERVATIVE = { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };

bool coarse_barriers()
{
    static const bool coarse = getenv("LL_VK_COARSE_BARRIERS") != nullptr;
    return coarse;
}

// the work that used the image in 'layout' and has to be finished (and its writes made available)
LayoutSync sync_leaving(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return { DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return { DEPTH_STAGES | SHADER_STAGES, 0 };         // only reads: an execution dependency is enough
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return { SHADER_STAGES, 0 };
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT };
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0 };
    default:
        return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT };     // general (DLSS), present, anything unforeseen
    }
}

// the work that will use the image in 'layout' and has to wait
LayoutSync sync_entering(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return { DEPTH_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return { DEPTH_STAGES | SHADER_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT };
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return { SHADER_STAGES, VK_ACCESS_2_SHADER_READ_BIT };
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT };
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return { VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT };
    default:
        return CONSERVATIVE;
    }
}
}

void Renderer::transition(VkCommandBuffer cmd, Image& view_or_image, VkImageLayout layout)
{
    Image& image = *root_of(&view_or_image);     // views of one layer/mip share the owner's layout (whole image)
    if (image.mLayout == layout)
    {
        view_or_image.mLayout = layout;
        return;
    }
    noteBarrier(image, image.mLayout, layout);
    if (image.mLayout == VK_IMAGE_LAYOUT_UNDEFINED && image.mZeroOnFirstUse)
    {
        image.mZeroOnFirstUse = false;
        imageBarrier(cmd, image.mImage, image.mAspect, image.mMipLevels, image.mLayers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        VkImageSubresourceRange range = { image.mAspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        if (image.mAspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            VkClearDepthStencilValue zero = { 1.f, 0 };
            vkCmdClearDepthStencilImage(cmd, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        else
        {
            VkClearColorValue zero = {};
            vkCmdClearColorImage(cmd, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        image.mLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            // the layout stays, so no transition follows: whatever is written next (the first upload into a new texture) must still come after the clear
            VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
            view_or_image.mLayout = layout;
            return;
        }
    }
    if (coarse_barriers())
    {
        imageBarrier(cmd, image.mImage, image.mAspect, image.mMipLevels, image.mLayers, image.mLayout, layout,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    else
    {
        const LayoutSync before = sync_leaving(image.mLayout);
        const LayoutSync after = sync_entering(layout);
        imageBarrier(cmd, image.mImage, image.mAspect, image.mMipLevels, image.mLayers, image.mLayout, layout, before.mStage, before.mAccess, after.mStage, after.mAccess);
    }
    image.mLayout = layout;
    view_or_image.mLayout = layout;
}

static const char* layout_name(VkImageLayout l)
{
    switch (l)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undef";
    case VK_IMAGE_LAYOUT_GENERAL: return "general";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "color";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "depth";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "depthRO";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "sample";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "xferSrc";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "xferDst";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "present";
    default: return "other";
    }
}

static std::string image_kind(const Image& im)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%ux%u f%d m%u l%u", im.mExtent.width, im.mExtent.height, (int)im.mFormat, im.mMipLevels, im.mLayers);
    return buf;
}

void Renderer::noteBarrier(const Image& image, VkImageLayout from, VkImageLayout to)
{
    static const bool on = getenv("LL_VK_PHASES") != nullptr;
    if (!on)
    {
        mBarrierEndedPass = false;
        return;
    }
    const std::string key = std::string(mBarrierCause) + " | " + image_kind(image) + " | " + layout_name(from) + ">" + layout_name(to);
    BarrierStat& st = mBarrierStats[key];
    ++st.mCount;
    if (mBarrierEndedPass)
    {
        ++st.mEndedPass;
    }
    ++mBarrierPerImage[&image];
    mBarrierEndedPass = false;
}

std::string Renderer::takeBarrierReport(size_t frames)
{
    const double f = (double)std::max<size_t>(1, frames);
    std::vector<std::pair<std::string, BarrierStat>> rows(mBarrierStats.begin(), mBarrierStats.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.mCount > b.second.mCount; });
    U64 total = 0, ended = 0;
    std::map<std::string, U64> by_cause;
    for (const auto& r : rows)
    {
        total += r.second.mCount;
        ended += r.second.mEndedPass;
        by_cause[r.first.substr(0, r.first.find(" | "))] += r.second.mCount;
    }
    std::string out;
    char buf[256];
    snprintf(buf, sizeof(buf), "layout transitions per frame %.1f (%.1f of them ended a render pass); by cause:", (double)total / f, (double)ended / f);
    out = buf;
    for (const auto& c : by_cause)
    {
        snprintf(buf, sizeof(buf), " %s %.1f,", c.first.c_str(), (double)c.second / f);
        out += buf;
    }
    out += " | top changes (cause | image | layouts: per frame, ended a pass):";
    for (size_t i = 0; i < rows.size() && i < 14; ++i)
    {
        snprintf(buf, sizeof(buf), " [%s: %.1f, %.1f]", rows[i].first.c_str(), (double)rows[i].second.mCount / f, (double)rows[i].second.mEndedPass / f);
        out += buf;
    }
    std::vector<std::pair<const Image*, U32>> imgs(mBarrierPerImage.begin(), mBarrierPerImage.end());
    std::sort(imgs.begin(), imgs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    out += " | images with the most transitions per frame (of " + std::to_string(imgs.size()) + " distinct):";
    for (size_t i = 0; i < imgs.size() && i < 8; ++i)
    {
        snprintf(buf, sizeof(buf), " [#%04x %s: %.1f]", (unsigned)(((size_t)imgs[i].first >> 4) & 0xFFFF), image_kind(*imgs[i].first).c_str(), (double)imgs[i].second / f);
        out += buf;
    }
    {
        snprintf(buf, sizeof(buf), " | draws by depth use per frame: plain %.1f, samples %.1f, writes %.1f, writes+samples %.1f; changes of use (per frame):",
                 (double)mDepthUseCounts[0] / f, (double)mDepthUseCounts[1] / f, (double)mDepthUseCounts[2] / f, (double)mDepthUseCounts[3] / f);
        out += buf;
        std::vector<std::pair<std::string, U32>> ch(mDepthUseChanges.begin(), mDepthUseChanges.end());
        std::sort(ch.begin(), ch.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < ch.size() && i < 12; ++i)
        {
            snprintf(buf, sizeof(buf), " [%s: %.1f]", ch[i].first.c_str(), (double)ch[i].second / f);
            out += buf;
        }
        for (U32& c : mDepthUseCounts) c = 0;
        mDepthUseChanges.clear();
    }
    {
        // the loose uniform blocks copied into the ring: a draw whose program has a changed value (or the first use in a frame) copies the whole block
        U64 copies = 0, bytes = 0, dirty = 0, same = 0;
        std::vector<std::pair<U64, const Program*>> by_bytes;
        for (const auto& kv : mUniformCopies)
        {
            copies += kv.second.mCopies;
            bytes += kv.second.mBytes;
            dirty += kv.second.mDirty;
            same += kv.second.mSame;
            by_bytes.push_back({ kv.second.mBytes, kv.first });
        }
        std::sort(by_bytes.rbegin(), by_bytes.rend());
        snprintf(buf, sizeof(buf), " | uniform ring copies per frame %.0f (%.0f because a value changed, %.0f identical to an earlier copy of the frame), %.0f KB per frame; most bytes:", (double)copies / f, (double)dirty / f, (double)same / f, (double)bytes / 1024.0 / f);
        out += buf;
        for (size_t i = 0; i < by_bytes.size() && i < 8; ++i)
        {
            const UniformCopyStat& us = mUniformCopies[by_bytes[i].second];
            snprintf(buf, sizeof(buf), " [%s: %.0f copies of %.1f KB]", by_bytes[i].second->mName.c_str(), (double)us.mCopies / f, us.mCopies ? (double)us.mBytes / 1024.0 / (double)us.mCopies : 0.0);
            out += buf;
        }
        mUniformCopies.clear();
    }
    {
        // uniforms by how often their value really changes (per frame): about the number of draws = per draw, a few per pass = per pass
        std::vector<std::pair<U64, std::string>> by_changes;
        for (const auto& kv : gUniformChangeStats) by_changes.push_back({ kv.second.mChanges, kv.first });
        std::sort(by_changes.rbegin(), by_changes.rend());
        out += " | uniform value changes per frame (writes, bytes):";
        for (size_t i = 0; i < by_changes.size() && i < 40; ++i)
        {
            const UniformChangeStat& us = gUniformChangeStats[by_changes[i].second];
            snprintf(buf, sizeof(buf), " [%s: %.0f of %.0f writes, %u B]", by_changes[i].second.c_str(), (double)us.mChanges / f, (double)us.mWrites / f, us.mSize);
            out += buf;
        }
        out += " | all distinct uniforms written: " + std::to_string(gUniformChangeStats.size());
        gUniformChangeStats.clear();
    }
    mBarrierStats.clear();
    mBarrierPerImage.clear();
    return out;
}

void Renderer::ensureLayout(Image& image, VkImageLayout layout)
{
    Image* root = root_of(&image);
    if (root->mLayout == layout)
    {
        return;
    }
    auto is_attached = [&]() {
        bool attached = mDepthTarget && root_of(mDepthTarget) == root;
        for (U32 i = 0; i < mColorCount; ++i)
        {
            attached = attached || root_of(mColorTargets[i]) == root;
        }
        return attached;
    };
    // barriers are not allowed inside a render pass: any layout change ends the current one
    if (mRendering)
    {
        endRendering();
        mBarrierEndedPass = true;
    }
    else if (mPendingClear && !mInBegin)
    {
        if (is_attached())
        {
            flushPendingClear();     // the clear is recorded before the image changes layout
            endRendering();
            mBarrierEndedPass = true;
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
    if (LLVkMirror::gPhaseTiming)
    {
        mPassTag = Context::stampTag(describeTarget());
        mBeginTag = mZoneStack.empty() ? mPassTag : mZoneStack.back();
        // a pass that starts with a clear: the layout changes, the begin and the clear get their own entry, so that they are not counted as the first draws' time
        mBeginSplit = mPendingClear != 0;
        Context::instance().gpuStamp(mBeginSplit ? Context::stampTag("pass begin + clear " + describeTarget()) : mBeginTag);      // LL_VK_PHASES: GPU time by render pass (or by the viewer's zone that spans it)
    }
    tracyBeginPass();
    VkExtent2D extent = { 0, 0 };
    VkRenderingAttachmentInfo colors[8] = {};
    for (U32 i = 0; i < mColorCount; ++i)
    {
        Image* img = mColorTargets[i];
        const char* prev_cause = setBarrierCause("attach color");
        ensureLayout(*img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        setBarrierCause(prev_cause);
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
        const VkImageLayout depth_layout = mDepthRO ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const char* prev_cause = setBarrierCause(mDepthRO ? "attach depthRO" : "attach depth");
        ensureLayout(*mDepthTarget, depth_layout);
        setBarrierCause(prev_cause);
        depth.imageView = mDepthTarget->mView;
        depth.imageLayout = depth_layout;
        depth.loadOp = ((mPendingClear & CLEAR_DEPTH) && !mDepthRO) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = mDepthRO ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
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
    if (mBeginSplit)
    {
        mBeginSplit = false;
        Context::instance().gpuStamp(mBeginTag);
    }
}

void Renderer::clear(U32 bits, const F32 color[4], F32 depth, U32 stencil)
{
    ++mModCount;
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

// slot 0 of every frame's tables: a stand-in image of the table's kind, what an unbound texture unit samples
void Renderer::bindlessStart()
{
    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
    {
        if (mBindlessTbl[t].mInfos.empty())
        {
            const Image* stand_in = &mWhite;
            if (t == 1 && mFallback[1].valid()) stand_in = &mFallback[1];
            if (t == 2 && mFallback[2].valid()) stand_in = &mFallback[2];
            VkDescriptorImageInfo white = { getSampler(SamplerDesc()), stand_in->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            mBindlessTbl[t].mInfos.push_back(white);
        }
    }
}

U32 Renderer::bindlessSlot(Image* image, VkSampler sampler)
{
    return bindlessSlotIn(0, image, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

U32 Renderer::bindlessSlotIn(U32 table, Image* image, VkSampler sampler, VkImageLayout layout)
{
    if (!mSet2Layout || table >= BINDLESS_TABLES || !image || !image->valid() || !sampler)
    {
        return 0;
    }
    bindlessStart();
    BindlessTable& tbl = mBindlessTbl[table];
    const BindlessKey key = { image->mView, sampler, layout };
    auto it = tbl.mMap.find(key);
    if (it != tbl.mMap.end())
    {
        return it->second;
    }
    if (tbl.mInfos.size() >= mBindlessCapacity)
    {
        LL_WARNS_ONCE("Vulkan") << "bindless table full: the textures after the first " << mBindlessCapacity << " of a frame are drawn white" << LL_ENDL;
        return 0;
    }
    const U32 slot = (U32)tbl.mInfos.size();
    VkDescriptorImageInfo info = { sampler, image->mView, layout };
    tbl.mInfos.push_back(info);
    tbl.mMap.emplace(key, slot);
    return slot;
}

void Renderer::bindlessFlush()
{
    if (!mSet2Layout || mCmd == VK_NULL_HANDLE)
    {
        return;
    }
    bindlessStart();
    VkWriteDescriptorSet writes[BINDLESS_TABLES];
    U32 n = 0;
    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
    {
        BindlessTable& tbl = mBindlessTbl[t];
        if (tbl.mWritten < tbl.mInfos.size())
        {
            // the slots taken since the last flush are contiguous: one write per table
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet = mBindlessSet[mSlot];
            writes[n].dstBinding = t;
            writes[n].dstArrayElement = tbl.mWritten;
            writes[n].descriptorCount = (U32)tbl.mInfos.size() - tbl.mWritten;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo = tbl.mInfos.data() + tbl.mWritten;
            tbl.mWritten = (U32)tbl.mInfos.size();
            ++n;
        }
    }
    if (n)
    {
        vkUpdateDescriptorSets(Context::instance().getDevice(), n, writes, 0, nullptr);
    }
    if (!mBindlessBound)
    {
        vkCmdBindDescriptorSets(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 2, 1, &mBindlessSet[mSlot], 0, nullptr);
        mBindlessBound = true;
    }
}

Renderer::ScratchAlloc Renderer::allocVertexScratch(VkDeviceSize size)
{
    ScratchAlloc a;
    const U32 start = (mVertexRingUsed + 15) / 16 * 16;
    if (start + size > VERTEX_RING_BYTES || !mVertexRing[mSlot].valid())
    {
        LL_WARNS_ONCE("Vulkan") << "vertex scratch ring exhausted" << LL_ENDL;
        return a;
    }
    mVertexRingUsed = start + (U32)size;
    a.mBuffer = mVertexRing[mSlot].mBuffer;
    a.mOffset = start;
    a.mPtr = (U8*)mVertexRing[mSlot].mMapped + start;
    a.mAddress = mVertexRing[mSlot].mAddress ? mVertexRing[mSlot].mAddress + start : 0;
    return a;
}

U32 Renderer::allocUniform(U32 size)
{
    const U32 align = (U32)Context::instance().getCaps().mUniformAlignment;
    const U32 start = (mRingUsed + align - 1) / align * align;
    // every dynamic descriptor must be able to see MAX_UNIFORM_RANGE bytes from its offset
    if (start + MAX_UNIFORM_RANGE > RING_BYTES)
    {
        // wrapping would overwrite data the GPU has yet to read: the draw is dropped instead
        LL_WARNS_ONCE("Vulkan") << "uniform ring exhausted; draws are dropped until the next frame" << LL_ENDL;
        ++mRingExhausted;
        return 0xFFFFFFFFu;
    }
    mRingUsed = start + size;
    return start;
}

// The image a sampler reads: what is bound, unless it is of the wrong kind for the shader's sampler type, or
// is the depth attachment being written (a feedback loop), in which case a white stand-in is used.
Image* Renderer::resolveTexture(size_t k, bool depth_ro, Image* depth_root)
{
    const U32 unit = mProgram->mSamplerUnits[k];
    Image* img = unit < MAX_TEXTURE_UNITS ? mTextures[unit] : nullptr;

    VkImageViewType need = VK_IMAGE_VIEW_TYPE_2D;
    switch (k < mProgram->mSamplerClass.size() ? mProgram->mSamplerClass[k] : 0)      // classified once when the program was created
    {
    case 1: need = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case 2: need = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
    case 3: need = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case 4: need = VK_IMAGE_VIEW_TYPE_3D; break;
    default: break;
    }

    if (img && depth_root && root_of(img) == depth_root && !depth_ro)
    {
        img = nullptr;          // sampling the depth buffer while writing it
    }
    for (U32 i = 0; img && i < mColorCount; ++i)
    {
        if (root_of(mColorTargets[i]) == root_of(img))
        {
            img = nullptr;      // a stale binding of a colour target being rendered to (unused by the shader in practice)
        }
    }
    if (img && img->mViewType != need)
    {
        img = nullptr;
    }
    if (img)
    {
        return img;
    }
    switch (need)
    {
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return mFallback[0].valid() ? &mFallback[0] : &mWhite;
    case VK_IMAGE_VIEW_TYPE_CUBE: return mFallback[1].valid() ? &mFallback[1] : &mWhite;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return mFallback[2].valid() ? &mFallback[2] : &mWhite;
    case VK_IMAGE_VIEW_TYPE_3D: return mFallback[3].valid() ? &mFallback[3] : &mWhite;
    default: return &mWhite;
    }
}

void Renderer::applyState()
{
    LLVkMirror::noteRendererThread();
    long long lap_t = LLVkMirror::gPhaseTiming ? LLVkMirror::phaseNow() : 0;
    mDiagChanged = false;
    // 1. A texture that is also the depth attachment is a feedback loop. Reading it while depth writes are off is
    //    legal with a read-only depth layout (the viewer's lighting passes do this); with writes on it cannot work.
    Image* depth_root = mDepthTarget ? root_of(mDepthTarget) : nullptr;
    bool depth_sampled = false;
    if (mProgram && depth_root)
    {
        for (size_t k = 0; k < mProgram->mInfo.mSamplers.size(); ++k)
        {
            const U32 unit = mProgram->mSamplerUnits[k];
            Image* img = unit < MAX_TEXTURE_UNITS ? mTextures[unit] : nullptr;
            if (img && root_of(img) == depth_root)
            {
                depth_sampled = true;
            }
        }
    }
    {
        static const bool diag = getenv("LL_VK_PHASES") != nullptr;
        if (diag && depth_root && mProgram)
        {
            static const char* const use_name[4] = { "plain", "samples", "writes", "writes+samples" };       // plain: no write, not sampled
            const int use = (mDepthStencil.mDepthWrite ? 2 : 0) + (depth_sampled ? 1 : 0);
            ++mDepthUseCounts[use];
            if (mDepthUseRoot != depth_root)
            {
                mDepthUseRoot = depth_root;
                mDepthUsePrev = -1;
            }
            if (mDepthUsePrev >= 0 && mDepthUsePrev != use)
            {
                ++mDepthUseChanges[std::string(use_name[mDepthUsePrev]) + ">" + use_name[use] + " " + mProgram->mName];
            }
            mDepthUsePrev = use;
        }
    }
    bool depth_ro = depth_sampled && !mDepthStencil.mDepthWrite;
    {
        // C9: a draw that neither writes depth nor reads the depth buffer works in either layout, so it keeps the one the pass is in (it used to force the
        // writable layout, which ended the pass and turned the layout back and forth around every draw that samples the depth: about 50 times a frame in a busy scene)
        static const bool sticky = getenv("LL_VK_STICKY_DEPTH") == nullptr || getenv("LL_VK_STICKY_DEPTH")[0] != '0';
        const DepthStencilState& ds = mDepthStencil;
        const bool stencil_writes = ds.mStencilTest && ds.mWriteMask && (ds.mFail != VK_STENCIL_OP_KEEP || ds.mDepthFail != VK_STENCIL_OP_KEEP || ds.mPass != VK_STENCIL_OP_KEEP);
        if (sticky && depth_root && !depth_sampled && !ds.mDepthWrite && !stencil_writes && mDepthRO && mDepthRORoot == depth_root && !(mPendingClear & CLEAR_DEPTH))
        {
            depth_ro = true;
        }
    }
    if (depth_ro != mDepthRO)
    {
        flushPendingClear();
        endRendering();
        mDepthRO = depth_ro;
    }
    if (depth_ro)
    {
        mDepthRORoot = depth_root;
    }

    // 2. textures must be in a sampling layout before a pass is (re)started
    Image* resolved[MAX_TEXTURE_UNITS] = {};
    if (mProgram)
    {
        for (size_t k = 0; k < mProgram->mInfo.mSamplers.size() && k < MAX_TEXTURE_UNITS; ++k)
        {
            Image* img = resolveTexture(k, depth_ro, depth_root);
            resolved[k] = img;
            if (img && img != &mWhite && !(depth_root && root_of(img) == depth_root) && img->mLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                const char* prev_cause = setBarrierCause("draw samples");
                ensureLayout(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                setBarrierCause(prev_cause);
            }
        }
    }

    LLVkMirror::phaseLap(LLVkMirror::PH_A_TEX, lap_t);
    // 3. pass
    beginRenderingIfNeeded();
    LLVkMirror::phaseLap(LLVkMirror::PH_A_PASS, lap_t);

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
    if (pipeline == VK_NULL_HANDLE)
    {
        mDrawBlocked = true;        // being built in the background (or failed): the draw is dropped, the next one asks again
        return;
    }
    if (pipeline != mBoundPipeline)
    {
        vkCmdBindPipeline(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        mBoundPipeline = pipeline;
        mDiagChanged = true;
    }
    LLVkMirror::phaseLap(LLVkMirror::PH_A_PIPE, lap_t);

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
        mDiagChanged = true;
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
        mDiagChanged = true;
    }

    if (!mStateApplied || !(mRaster == mAppliedRaster))
    {
        mDiagChanged = true;
        vkCmdSetCullMode(mCmd, mRaster.mCull);
        vkCmdSetFrontFace(mCmd, mRaster.mFrontFace);
        vkCmdSetPolygonModeEXT(mCmd, mRaster.mPolygonMode);
        if (Context::instance().getCaps().mDepthClamp)
        {
            vkCmdSetDepthClampEnableEXT(mCmd, mRaster.mDepthClamp ? VK_TRUE : VK_FALSE);
        }
        vkCmdSetLineWidth(mCmd, Context::instance().getCaps().mWideLines ? mRaster.mLineWidth : 1.f);
        vkCmdSetDepthBiasEnable(mCmd, mRaster.mDepthBias ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthBias(mCmd, mRaster.mBiasConstant, 0.f, mRaster.mBiasSlope);
        mAppliedRaster = mRaster;
    }
    if (!mStateApplied || !(mDepthStencil == mAppliedDepthStencil))
    {
        mDiagChanged = true;
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
    if (!mStateApplied || !(mBlend == mAppliedBlend) || mColorCount != mAppliedColorCount)
    {
        mDiagChanged = true;
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
        mAppliedColorCount = mColorCount;
    }
    mStateApplied = true;
    LLVkMirror::phaseLap(LLVkMirror::PH_A_DYN, lap_t);

    // 5. vertex input
    if (mUseVertexInput)
    {
        // general case: any number of bindings and attributes, compared with what was last recorded on every draw (the buffers can change)
        const U32 nb = std::min<U32>((U32)mVertexInput.mBindings.size(), 16);
        const U32 na = std::min<U32>((U32)mVertexInput.mAttributes.size(), 16);
        bool same_format = mAppliedVertexInputValid && mAppliedVertexInput.mBindings.size() == mVertexInput.mBindings.size()
            && mAppliedVertexInput.mAttributes.size() == mVertexInput.mAttributes.size();
        bool same_buffers = same_format;
        for (U32 i = 0; same_format && i < nb; ++i)
        {
            const VertexInput::Binding& a = mAppliedVertexInput.mBindings[i];
            const VertexInput::Binding& b = mVertexInput.mBindings[i];
            same_format = a.mStride == b.mStride;
            same_buffers = same_buffers && a.mBuffer == b.mBuffer && a.mOffset == b.mOffset;
        }
        for (U32 i = 0; same_format && i < na; ++i)
        {
            const VertexInput::Attribute& a = mAppliedVertexInput.mAttributes[i];
            const VertexInput::Attribute& b = mVertexInput.mAttributes[i];
            same_format = a.mLocation == b.mLocation && a.mBinding == b.mBinding && a.mFormat == b.mFormat && a.mOffset == b.mOffset;
        }
        if (!same_format || !same_buffers)
        {
            mDiagChanged = true;
        }
        if (!same_format)
        {
            VkVertexInputBindingDescription2EXT bindings[16];
            VkVertexInputAttributeDescription2EXT attrs[16];
            for (U32 i = 0; i < nb; ++i)
            {
                bindings[i] = { VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT };
                bindings[i].binding = i;
                bindings[i].stride = mVertexInput.mBindings[i].mStride;
                bindings[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                bindings[i].divisor = 1;
            }
            for (U32 i = 0; i < na; ++i)
            {
                attrs[i] = { VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT };
                attrs[i].location = mVertexInput.mAttributes[i].mLocation;
                attrs[i].binding = mVertexInput.mAttributes[i].mBinding;
                attrs[i].format = mVertexInput.mAttributes[i].mFormat;
                attrs[i].offset = mVertexInput.mAttributes[i].mOffset;
            }
            vkCmdSetVertexInputEXT(mCmd, nb, nb ? bindings : nullptr, na, attrs);
        }
        if (nb && !(same_format && same_buffers))
        {
            VkBuffer buffers[16];
            VkDeviceSize offsets[16];
            for (U32 i = 0; i < nb; ++i)
            {
                buffers[i] = mVertexInput.mBindings[i].mBuffer;
                offsets[i] = mVertexInput.mBindings[i].mOffset;
            }
            vkCmdBindVertexBuffers(mCmd, 0, nb, buffers, offsets);
        }
        if (!(same_format && same_buffers))
        {
            mAppliedVertexInput = mVertexInput;
        }
        mAppliedVertexInputValid = true;
    }
    else
    {
        if (mVertexInputDirty)
        {
            mAppliedVertexInputValid = false;       // the layout path overwrites what the general path recorded
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
    }

    LLVkMirror::phaseLap(LLVkMirror::PH_A_VI, lap_t);
    LLVkMirror::gLastLap = 301;
    LLVkMirror::gDbg[0] = (unsigned long long)mProgram;
    LLVkMirror::gDbg[1] = mSlot;
    LLVkMirror::gDbg[2] = (unsigned long long)mCmd;
    LLVkMirror::gDbg[3] = (unsigned long long)mRing[mSlot].mMapped;
    // 5b. <FS> bindless programs: the slot of each sampler's texture is written into the loose uniform block (copied to the ring below); no descriptors are pushed
    if (mProgram->mBindless)
    {
        VkSampler fallback_sampler = getSampler(SamplerDesc());
        for (size_t k = 0; k < mProgram->mInfo.mSamplers.size(); ++k)
        {
            const U32 unit = mProgram->mSamplerUnits[k];
            if (unit >= MAX_TEXTURE_UNITS)
            {
                continue;
            }
            Image* img = k < MAX_TEXTURE_UNITS && resolved[k] ? resolved[k] : resolveTexture(k, depth_ro, depth_root);
            const bool is_depth_target = depth_root && root_of(img) == depth_root;
            const VkSampler sampler = mTextureSamplers[unit] ? mTextureSamplers[unit] : fallback_sampler;
            const VkImageLayout layout = is_depth_target ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const U32 table = mProgram->mSamplerTable[k];
            UnitSlot& cached = mUnitSlot[unit];
            U32 slot;
            if (cached.mEpoch == mBindlessEpoch && cached.mView == img->mView && cached.mSampler == sampler && cached.mLayout == layout && cached.mTable == table)
            {
                slot = cached.mSlot;
            }
            else
            {
                slot = bindlessSlotIn(table, img, sampler, layout);
                cached = { img->mView, sampler, layout, table, mBindlessEpoch, slot };
            }
            U32* dst = (U32*)(mProgram->mShadow.data() + mProgram->mSlotOffset[k]);
            if (*dst != slot)
            {
                *dst = slot;
                mProgram->mDirty = true;
            }
        }
        bindlessFlush();
    }
    // 5c. <FS> programs that trace rays: the frame slot's ray tracing scene is set 3
    if (mProgram->mUsesRt && mSet3Layout)
    {
        const VkDescriptorSet rt_set = Rt::descriptorSet(mSlot);
        const U32 rt_offset = Rt::tableOffset();
        if (!mProgram->mBindless)
        {
            bindlessFlush();            // the masked casters' textures are read through the frame's table (set 2) whatever the program does with its own samplers
        }
        if (rt_set != VK_NULL_HANDLE && (rt_set != mRtBoundSet || rt_offset != mRtBoundOffset))
        {
            vkCmdBindDescriptorSets(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 3, 1, &rt_set, 1, &rt_offset);
            mRtBoundSet = rt_set;
            mRtBoundOffset = rt_offset;
        }
    }
    LLVkMirror::phaseLap(LLVkMirror::PH_A_SAMP, lap_t);
    // 6. loose uniforms: copy this program's shadow into the ring when it changed (or on its first use this frame)
    if (mProgram->mDirty || mProgram->mRingFrame != Context::instance().getFrameNumber())
    {
        LLVkMirror::gLastLap = 302;
        const U32 ring_offset = allocUniform((U32)mProgram->mShadow.size());
        LLVkMirror::gLastLap = 303;
        if (ring_offset == 0xFFFFFFFFu)
        {
            mDrawBlocked = true;
            return;
        }
        mProgram->mRingOffset = ring_offset;
        mDiagChanged = true;
        if (LLVkMirror::gPhaseTiming)
        {
            UniformCopyStat& us = mUniformCopies[mProgram];
            ++us.mCopies;
            us.mBytes += mProgram->mShadow.size();
            if (mProgram->mDirty) ++us.mDirty;
            {
                // would the block be the same as one already copied in this frame for this program?
                const U64* w = (const U64*)mProgram->mShadow.data();
                U64 h = 1469598103934665603ull ^ (U64)(size_t)mProgram;
                for (size_t k = 0; k < mProgram->mShadow.size() / 8; ++k)
                {
                    h = (h ^ w[k]) * 1099511628211ull;
                    h ^= h >> 29;
                }
                if (mUniformSeenFrame != Context::instance().getFrameNumber())
                {
                    mUniformSeenFrame = Context::instance().getFrameNumber();
                    mUniformSeen.clear();
                }
                if (!mUniformSeen.insert(h).second) ++us.mSame;
            }
        }
        memcpy((U8*)mRing[mSlot].mMapped + mProgram->mRingOffset, mProgram->mShadow.data(), mProgram->mShadow.size());
        mProgram->mRingFrame = Context::instance().getFrameNumber();
        mProgram->mDirty = false;
    }
    LLVkMirror::gLastLap = 304;
    VkDescriptorSet set0 = mSet0[mSlot];
    if (mUbo[0] || mUbo[1] || mUbo[2] || mUbo[3])
    {
        set0 = uboSet();
    }
    LLVkMirror::gLastLap = 305;
    if (!mSet0Bound || mAppliedDynamicOffset != mProgram->mRingOffset || mAppliedSet0 != set0)
    {
        mDiagChanged = true;
        vkCmdBindDescriptorSets(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &set0, 1, &mProgram->mRingOffset);
        mAppliedDynamicOffset = mProgram->mRingOffset;
        mAppliedSet0 = set0;
        mSet0Bound = true;
    }

    LLVkMirror::phaseLap(LLVkMirror::PH_A_UNI, lap_t);
    // 7. textures, pushed for exactly the samplers the program declares
    if (!mProgram->mBindless && !mProgram->mInfo.mSamplers.empty())
    {
        VkDescriptorImageInfo infos[MAX_TEXTURE_UNITS];
        VkWriteDescriptorSet writes[MAX_TEXTURE_UNITS];
        U32 n = 0;
        VkSampler fallback = getSampler(SamplerDesc());
        for (size_t k = 0; k < mProgram->mInfo.mSamplers.size(); ++k)
        {
            const SamplerInfo& s = mProgram->mInfo.mSamplers[k];
            const U32 unit = mProgram->mSamplerUnits[k];
            if (s.mBinding >= MAX_TEXTURE_UNITS || unit >= MAX_TEXTURE_UNITS)
            {
                continue;
            }
            Image* img = k < MAX_TEXTURE_UNITS && resolved[k] ? resolved[k] : resolveTexture(k, depth_ro, depth_root);
            const bool is_depth_target = depth_root && root_of(img) == depth_root;
            const VkSampler sampler = mTextureSamplers[unit] ? mTextureSamplers[unit] : fallback;
            const VkImageLayout layout = is_depth_target ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            PushedBinding& pushed = mPushed[s.mBinding];
            if (pushed.mView == img->mView && pushed.mSampler == sampler && pushed.mLayout == layout)
            {
                continue;       // this binding already holds exactly that
            }
            pushed = { img->mView, sampler, layout };
            infos[n] = { sampler, img->mView, layout };
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstBinding = s.mBinding;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        if (n)
        {
            mDiagChanged = true;
            vkCmdPushDescriptorSetKHR(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 1, n, writes);
        }
    }
    LLVkMirror::phaseLap(LLVkMirror::PH_A_PUSH, lap_t);
}

void Renderer::setUniformBuffer(U32 index, VkBuffer buffer, VkDeviceSize range)
{
    if (index < 4)
    {
        const VkDeviceSize used_range = buffer ? std::min<VkDeviceSize>(range, MAX_UNIFORM_RANGE_NAMED) : 0;
        if (mUbo[index] != buffer || mUboRange[index] != used_range)
        {
            mUbo[index] = buffer;
            mUboRange[index] = used_range;
            mUboKeyDirty = true;
        }
    }
}

// the set 0 variant for the named uniform blocks currently bound (built once per combination per frame)
VkDescriptorSet Renderer::uboSet()
{
    if (!mUboKeyDirty && mLastUboSet != VK_NULL_HANDLE)
    {
        return mLastUboSet;         // <FS> same uniform buffers as the last draw of this frame: no key, no map lookup
    }
    std::array<U64, 8> key = {};
    for (U32 i = 0; i < 4; ++i)
    {
        key[i] = (U64)mUbo[i];
        key[4 + i] = (U64)mUboRange[i];
    }
    auto& sets = mUboSets[mSlot];
    auto it = sets.find(key);
    if (it != sets.end())
    {
        mLastUboSet = it->second;
        mUboKeyDirty = false;
        return it->second;
    }
    Context& ctx = Context::instance();
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = mUboPool[mSlot];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &mSet0Layout;
    if (!mUboPool[mSlot] || vkAllocateDescriptorSets(ctx.getDevice(), &ai, &set) != VK_SUCCESS)
    {
        return mSet0[mSlot];
    }
    VkDescriptorBufferInfo infos[5];
    infos[0] = { mRing[mSlot].mBuffer, 0, MAX_UNIFORM_RANGE };
    VkWriteDescriptorSet w[5] = {};
    for (U32 b = 0; b < 5; ++b)
    {
        if (b > 0)
        {
            infos[b] = mUbo[b - 1] ? VkDescriptorBufferInfo{ mUbo[b - 1], 0, mUboRange[b - 1] } : VkDescriptorBufferInfo{ mDummyUbo.mBuffer, 0, DUMMY_UBO_BYTES };
        }
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set;
        w[b].dstBinding = b;
        w[b].descriptorCount = 1;
        w[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(ctx.getDevice(), 5, w, 0, nullptr);
    sets[key] = set;
    mLastUboSet = set;
    mUboKeyDirty = false;
    return set;
}

bool Renderer::draw(U32 vertex_count, U32 first_vertex)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget))
    {
        return false;
    }
    gDebugProgram = mProgram->getName().c_str();
    applyState();
    if (mDrawBlocked)
    {
        mDrawBlocked = false;
        return false;
    }
    long long cmd_t = LLVkMirror::gPhaseTiming ? LLVkMirror::phaseNow() : 0;
    startPendingQuery();
    vkCmdDraw(mCmd, vertex_count, 1, first_vertex, 0);
    LLVkMirror::phaseLap(LLVkMirror::PH_A_CMD, cmd_t);
    ++mDrawCount;
    ++mModCount;
    return true;
}

// <FS> Render packets: the draws of one record share everything but their vertex count and their instance index (the shader finds the rest by it): the state is
// applied once and every draw is one command
bool Renderer::drawPackets(const U32* vertex_counts, U32 draws, VkBuffer indirect, VkDeviceSize indirect_offset)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget) || draws == 0)
    {
        return false;
    }
    gDebugProgram = mProgram->getName().c_str();
    applyState();
    if (mDrawBlocked)
    {
        mDrawBlocked = false;
        return false;
    }
    startPendingQuery();
    if (indirect != VK_NULL_HANDLE)
    {
        // the arguments (vertex count, one instance, first vertex 0, first instance = the draw number) are in the buffer: one command for the whole record
        for (U32 done = 0; done < draws; )
        {
            const U32 n = std::min<U32>(draws - done, 65535u);
            vkCmdDrawIndirect(mCmd, indirect, indirect_offset + (VkDeviceSize)done * sizeof(VkDrawIndirectCommand), n, (U32)sizeof(VkDrawIndirectCommand));
            done += n;
        }
        mDrawCount += draws;
    }
    else
    {
        for (U32 i = 0; i < draws; ++i)
        {
            if (vertex_counts[i])
            {
                vkCmdDraw(mCmd, vertex_counts[i], 1, 0, i);
                ++mDrawCount;
            }
        }
    }
    ++mModCount;
    return true;
}

bool Renderer::drawIndexed(U32 index_count, U32 first_index, S32 vertex_offset)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget) || !mIndexBuffer)
    {
        return false;
    }
    gDebugProgram = mProgram->getName().c_str();
    applyState();
    if (mDrawBlocked)
    {
        mDrawBlocked = false;
        return false;
    }
    long long cmd_t = LLVkMirror::gPhaseTiming ? LLVkMirror::phaseNow() : 0;
    if (LLVkMirror::gPhaseTiming && LLVkMirror::onMainThread())
    {
        ++mDrawDiag.mDraws;
        if (!mDiagChanged)
        {
            ++mDrawDiag.mUnchanged;
            if (mIndexBuffer == mDiagLastIndexBuffer && mIndexOffset == mDiagLastIndexOffset && (U64)first_index == mDiagLastIndexEnd && vertex_offset == mDiagLastVertexOffset)
            {
                ++mDrawDiag.mMergeable;
            }
        }
        mDiagLastIndexBuffer = mIndexBuffer;
        mDiagLastIndexOffset = mIndexOffset;
        mDiagLastIndexEnd = (U64)first_index + index_count;
        mDiagLastVertexOffset = vertex_offset;
    }
    if (mIndexBuffer != mBoundIndexBuffer || mIndexOffset != mBoundIndexOffset || mIndexType != mBoundIndexType)
    {
        vkCmdBindIndexBuffer(mCmd, mIndexBuffer, mIndexOffset, mIndexType);       // <FS> only when it differs from what the command buffer holds
        mBoundIndexBuffer = mIndexBuffer;
        mBoundIndexOffset = mIndexOffset;
        mBoundIndexType = mIndexType;
    }
    startPendingQuery();
    vkCmdDrawIndexed(mCmd, index_count, 1, first_index, vertex_offset, 0);
    LLVkMirror::phaseLap(LLVkMirror::PH_A_CMD, cmd_t);
    ++mDrawCount;
    ++mModCount;
    return true;
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

bool Renderer::recordExternal(const std::vector<std::pair<Image*, VkImageLayout>>& images, const std::function<void(VkCommandBuffer)>& record)
{
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    if (LLVkMirror::gPhaseTiming)
    {
        Context::instance().gpuStamp(Context::stampTag("external (DLSS)"));
    }
    for (const auto& i : images)
    {
        const char* prev_cause = setBarrierCause("external");
        ensureLayout(*i.first, i.second);
        setBarrierCause(prev_cause);
    }
    endRendering();
    record(mCmd);
    ++mModCount;
    // whatever the callback bound or set, the next draw starts from scratch
    mStateApplied = false;
    mBoundPipeline = VK_NULL_HANDLE;
    mBoundIndexBuffer = VK_NULL_HANDLE;
    mUboKeyDirty = true;
    mVertexInputDirty = true;
    mAppliedVertexInputValid = false;
    mSet0Bound = false;
    mRtBoundSet = VK_NULL_HANDLE;
    mAppliedDynamicOffset = 0xFFFFFFFF;
    mAppliedSet0 = VK_NULL_HANDLE;
    for (S32& v : mAppliedViewport) v = -1;
    for (S32& v : mAppliedScissor) v = -1;
    return true;
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
    ++mModCount;
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

    // GL textures start out zeroed; Vulkan images hold whatever the memory had. Render targets and arrays are only partly written
    // (a probe face at a time), so they start cleared. Compressed formats cannot be cleared and are always fully uploaded.
    const bool attachable = (d.mUsage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    const bool array_like = d.mType == TexType::Tex2DArray || d.mType == TexType::CubeArray || d.mType == TexType::Cube;
    if ((attachable || array_like) && !(fi.mKnown && fi.mCompressed) && out.mMipLevels > 0)
    {
        // cleared inside the frame the first time the image is used (Renderer::transition): no blocking submit for every new texture
        out.mZeroOnFirstUse = true;
        return true;
    }
    return true;
}

bool Renderer::uploadTexture(Image& image, U32 mip, U32 layer, U32 x, U32 y, U32 width, U32 height, U32 depth, const void* data, VkDeviceSize bytes)
{
    struct Src { const void* mData; } src = { data };
    return uploadTextureDirect(image, mip, layer, x, y, width, height, depth, bytes, [](void* ctx, U8* dst, VkDeviceSize n) { memcpy(dst, ((const Src*)ctx)->mData, (size_t)n); }, &src);
}

bool Renderer::uploadTextureDirect(Image& image, U32 mip, U32 layer, U32 x, U32 y, U32 width, U32 height, U32 depth, VkDeviceSize bytes, void (*fill)(void* ctx, U8* dst, VkDeviceSize bytes), void* fill_ctx)
{
    const auto t_start = std::chrono::steady_clock::now();
    LLVkMirror::noteRendererThread();
    ++mModCount;
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
    const auto t_fill = std::chrono::steady_clock::now();
    fill(fill_ctx, (U8*)stage.mPtr, expected);
    const U64 fill_us = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_fill).count();

    ++mUploadStats.mCount;
    mUploadStats.mBytes += expected;
    if (mCmd != VK_NULL_HANDLE && mRendering)
    {
        ++mUploadStats.mPassSplits;
    }
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
    LLVkAsync::noteTexWorker(0, fill_us, (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start).count(), 1);
    return true;
}

bool Renderer::generateMipmaps(Image& image)
{
    ++mModCount;
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

bool Renderer::readbackImage(Image& view_or_image, U32 mip, U32 layer, std::vector<U8>& texels)
{
    Context& ctx = Context::instance();
    Image* image = root_of(&view_or_image);
    const FormatInfo fi = formatInfo(image->mFormat);
    if (mCmd != VK_NULL_HANDLE || !fi.mKnown || fi.mCompressed || mip >= image->mMipLevels)
    {
        return false;
    }
    const U32 w = std::max<U32>(1, image->mExtent.width >> mip), h = std::max<U32>(1, image->mExtent.height >> mip);
    const VkDeviceSize bytes = (VkDeviceSize)w * h * fi.mBytesPerBlock;
    Buffer rb;
    if (!ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Readback, rb))
    {
        return false;
    }
    const VkImageLayout old_layout = image->mLayout;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {};
        c.imageSubresource = { image->mAspect, mip, layer, 1 };
        c.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, image->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.mBuffer, 1, &c);
        transition(cmd, *image, old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : old_layout);
    });
    texels.assign((U8*)rb.mMapped, (U8*)rb.mMapped + bytes);
    ctx.waitIdle();
    vmaDestroyBuffer(ctx.getAllocator(), rb.mBuffer, rb.mAllocation);
    return true;
}

bool Renderer::updateBuffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, const void* data)
{
    if (dst == VK_NULL_HANDLE || !data || size == 0)
    {
        return false;
    }
    ++mModCount;
    if (mCmd == VK_NULL_HANDLE)
    {
        DeferredCopy d;
        d.mDst = dst;
        d.mDstOffset = offset;
        d.mData.assign((const U8*)data, (const U8*)data + size);
        mDeferredCopies.push_back(std::move(d));
        return true;
    }
    StagingAlloc stage = Context::instance().allocStaging(size, 16);
    if (!stage.valid())
    {
        LL_WARNS("Vulkan") << "updateBuffer: no staging space for " << size << " bytes" << LL_ENDL;
        return false;
    }
    memcpy(stage.mPtr, data, (size_t)size);
    mPendingCopies.push_back({ dst, offset, stage.mBuffer, stage.mOffset, size });
    return true;
}

bool Renderer::fillBuffer(VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, U32 value)
{
    if (dst == VK_NULL_HANDLE || size == 0)
    {
        return false;
    }
    ++mModCount;
    if (mCmd == VK_NULL_HANDLE)
    {
        DeferredCopy d;
        d.mDst = dst;
        d.mDstOffset = offset;
        d.mFillSize = size;
        d.mFillValue = value;
        mDeferredCopies.push_back(std::move(d));
        return true;
    }
    mPendingCopies.push_back({ dst, offset, VK_NULL_HANDLE, value, size });
    return true;
}

bool Renderer::flushBufferUpdates()
{
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    if (mPendingCopies.empty())
    {
        return true;
    }
    ++mModCount;
    withCommandBuffer([&](VkCommandBuffer cmd) {
        // everything recorded so far (and earlier frames still on the GPU) finishes reading before the copies overwrite
        VkMemoryBarrier2 before = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &before;
        vkCmdPipelineBarrier2(cmd, &dep);
        for (const BufferCopy& c : mPendingCopies)
        {
            if (c.mSrc == VK_NULL_HANDLE)
            {
                vkCmdFillBuffer(cmd, c.mDst, c.mDstOffset, c.mSize, (U32)c.mSrcOffset);
                continue;
            }
            VkBufferCopy region = { c.mSrcOffset, c.mDstOffset, c.mSize };
            vkCmdCopyBuffer(cmd, c.mSrc, c.mDst, 1, &region);
        }
        VkMemoryBarrier2 after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        dep.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    mPendingCopies.clear();
    return true;
}

bool Renderer::flushAndRestart()
{
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    flushPendingClear();
    endRendering();
    Image* colors[8] = {};
    const U32 color_count = mColorCount;
    for (U32 i = 0; i < color_count; ++i)
    {
        colors[i] = mColorTargets[i];
    }
    Image* depth = mDepthTarget;
    const VkCommandBuffer cmd = mCmd;
    const U32 slot = mSlot;
    if (!Context::instance().flushFrameAndWait())
    {
        return false;
    }
    ++mFlushCount;
    beginFrame(cmd, slot);      // forgets everything cached about the old recording
    mColorCount = color_count;
    for (U32 i = 0; i < color_count; ++i)
    {
        mColorTargets[i] = colors[i];
    }
    mDepthTarget = depth;
    return true;
}

bool Renderer::readbackRegion(Image& view_or_image, U32 mip, U32 layer, S32 x, S32 y, U32 w, U32 h, std::vector<U8>& texels)
{
    Context& ctx = Context::instance();
    Image* image = root_of(&view_or_image);
    const FormatInfo fi = formatInfo(image->mFormat);
    if (!fi.mKnown || fi.mCompressed || mip >= image->mMipLevels || w == 0 || h == 0 || x < 0 || y < 0)
    {
        return false;
    }
    const U32 mw = std::max<U32>(1, image->mExtent.width >> mip), mh = std::max<U32>(1, image->mExtent.height >> mip);
    if ((U32)x + w > mw || (U32)y + h > mh)
    {
        return false;
    }
    const VkDeviceSize bytes = (VkDeviceSize)w * h * fi.mBytesPerBlock;
    Buffer rb;
    if (!ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Readback, rb))
    {
        return false;
    }
    const VkImageLayout old_layout = image->mLayout;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {};
        c.imageSubresource = { image->mAspect, mip, layer, 1 };
        c.imageOffset = { x, y, 0 };
        c.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, image->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.mBuffer, 1, &c);
        transition(cmd, *image, old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : old_layout);
    });
    texels.assign((U8*)rb.mMapped, (U8*)rb.mMapped + bytes);
    vmaDestroyBuffer(ctx.getAllocator(), rb.mBuffer, rb.mAllocation);
    return true;
}

U32 Renderer::compressedMipBytes(const Image& image, U32 mip)
{
    const FormatInfo fi = formatInfo(image.mFormat);
    if (!fi.mKnown || !fi.mCompressed || mip >= image.mMipLevels)
    {
        return 0;
    }
    const U32 mw = std::max<U32>(1, image.mExtent.width >> mip), mh = std::max<U32>(1, image.mExtent.height >> mip);
    return ((mw + fi.mBlockWidth - 1) / fi.mBlockWidth) * ((mh + fi.mBlockHeight - 1) / fi.mBlockHeight) * fi.mBytesPerBlock;
}

bool Renderer::readbackCompressedMip(Image& view_or_image, U32 mip, U32 layer, std::vector<U8>& blocks)
{
    Context& ctx = Context::instance();
    Image* image = root_of(&view_or_image);
    const U32 bytes = compressedMipBytes(*image, mip);
    if (bytes == 0)
    {
        return false;
    }
    const U32 mw = std::max<U32>(1, image->mExtent.width >> mip), mh = std::max<U32>(1, image->mExtent.height >> mip);
    Buffer rb;
    if (!ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Readback, rb))
    {
        return false;
    }
    const VkImageLayout old_layout = image->mLayout;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {};
        c.imageSubresource = { image->mAspect, mip, layer, 1 };
        c.imageExtent = { mw, mh, 1 };      // a whole level; the buffer is tightly packed blocks
        vkCmdCopyImageToBuffer(cmd, image->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.mBuffer, 1, &c);
        transition(cmd, *image, old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : old_layout);
    });
    blocks.assign((U8*)rb.mMapped, (U8*)rb.mMapped + bytes);
    vmaDestroyBuffer(ctx.getAllocator(), rb.mBuffer, rb.mAllocation);
    return true;
}

bool Renderer::recordRowCopies(Image& view, const std::vector<U32>& rows, VkBuffer buffer)
{
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    Image* root = root_of(&view);
    const FormatInfo fi = formatInfo(root->mFormat);
    if (!fi.mKnown || fi.mCompressed || root->mAspect != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        return false;
    }
    flushPendingClear();
    endRendering();
    const VkImageLayout old_layout = root->mLayout;
    transition(mCmd, view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const U32 w = view.mExtent.width;
    std::vector<VkBufferImageCopy> regions(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        regions[i] = {};
        regions[i].bufferOffset = (VkDeviceSize)i * w * fi.mBytesPerBlock;
        regions[i].imageSubresource = { root->mAspect, view.mBaseMip, view.mBaseLayer, 1 };
        regions[i].imageOffset = { 0, (S32)rows[i], 0 };
        regions[i].imageExtent = { w, 1, 1 };
    }
    vkCmdCopyImageToBuffer(mCmd, root->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, (U32)regions.size(), regions.data());
    transition(mCmd, view, old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : old_layout);
    return true;
}

bool Renderer::copyRegion(Image& src, S32 sx, S32 sy, Image& dst, U32 dst_mip, U32 dst_layer, S32 dx, S32 dy, S32 width, S32 height)
{
    ++mModCount;
    if (mCmd == VK_NULL_HANDLE || width <= 0 || height <= 0)
    {
        return false;
    }
    Image* src_root = src.mOwner ? src.mOwner : &src;
    Image* dst_root = dst.mOwner ? dst.mOwner : &dst;
    const bool same_image = src_root == dst_root;
    flushPendingClear();
    endRendering();

    if (same_image)
    {
        return false;       // a copy onto itself is not needed by the viewer
    }
    transition(mCmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(mCmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const S32 max_w = (S32)src.mExtent.width, max_h = (S32)src.mExtent.height;
    const S32 w = std::min(width, std::min(max_w - sx, (S32)std::max<U32>(1, dst_root->mExtent.width >> dst_mip) - dx));
    const S32 h = std::min(height, std::min(max_h - sy, (S32)std::max<U32>(1, dst_root->mExtent.height >> dst_mip) - dy));
    if (w > 0 && h > 0 && sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0)
    {
        VkImageBlit blit = {};
        blit.srcSubresource = { src_root->mAspect, src.mBaseMip, src.mBaseLayer, 1 };
        blit.srcOffsets[0] = { sx, sy, 0 };
        blit.srcOffsets[1] = { sx + w, sy + h, 1 };
        blit.dstSubresource = { dst_root->mAspect, dst_mip, dst_layer, 1 };
        blit.dstOffsets[0] = { dx, dy, 0 };
        blit.dstOffsets[1] = { dx + w, dy + h, 1 };
        vkCmdBlitImage(mCmd, src_root->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_root->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_NEAREST);
    }
    transition(mCmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

} // namespace LLVk

#endif // LL_VULKAN
