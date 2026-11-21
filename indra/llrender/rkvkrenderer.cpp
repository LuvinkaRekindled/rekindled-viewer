/**
 * @file rkvkrenderer.cpp
 * @brief Vulkan draw machinery (see rkvkrenderer.h)
 */

#include "linden_common.h"

#include "rkvkrenderer.h"
#include <unordered_set>
#include <set>
#include <cmath>
#include <xmmintrin.h>

// <FS> Tracy GPU zones from the Vulkan timestamps. Tracy loads its Vulkan functions through the proc-address tables (our Vulkan is loaded at run time).
#if defined(TRACY_ENABLE)
#define TRACY_VK_USE_SYMBOL_TABLE
#include <tracy/TracyVulkan.hpp>
#endif
#include "rkvkasync.h"
#include "rkvkworker.h"
#include "rkvkupscale.h"
#include "rkvkbridge.h"
#include "rkvkrt.h"

#if defined(RK_VULKAN)

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
#include "rkdiag.h"        // <FS> RK_DIAG: the statistics lines are off unless RK_VK_DIAG is set

namespace RKVk
{

namespace
{
// pipelines the driver had to COMPILE (its cache did not hold them: VkPipelineCreationFeedback) since the cache was last written. A pipeline served from the cache adds nothing to it, so a run whose pipelines
// all come from the cache has nothing to save: the 220 MB write on the render thread (190 ms) every 18,000 frames and again at exit was for nothing (AUDIT-shaders-2026-11-18.md section 1 item 1).
// A driver that does not report the feedback counts every pipeline, as before.
std::atomic<U32> gPipelineCacheMisses{ 0 };

// where the driver's pipeline cache lives between runs; empty when switched off
std::string pipeline_cache_path()
{
    static const std::string path = []() {
        const char* off = getenv("RK_VK_PIPELINE_CACHE");
        if (off && off[0] == '0')
        {
            return std::string();
        }
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_pipeline_cache.bin");
    }();
    return path;
}

// programs whose pipelines the world does not wait for (the draw is dropped until the pipeline is built, and what it adds is optional): their pre-warm builds are not counted by the start-up wait
bool is_optional_program(const std::string& name)
{
    return name == "Deferred RT Reflection Shader";
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
const U32 PALETTE_RING_BYTES = 8u * 1024 * 1024;        // <FS> a slot of the palette ring (a frame's rigged palettes: a few MB in a crowd)
const U32 VERTEX_OVERFLOW_CHUNK_BYTES = 32u * 1024 * 1024;      // F3.1: see mVertexOverflow
const U32 VERTEX_OVERFLOW_MAX_CHUNKS = 8;

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
        const char* v = getenv("RK_VK_PIPELINE_ASYNC");
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
            const char* v = getenv("RK_VK_PIPELINE_THREADS");
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

// the variants of one file (the user's, recorded by this machine's earlier sessions, or the list that ships with the viewer): returns how many lines were read; false = no such file
bool load_variants_file(const std::string& path, U32& loaded)
{
    FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    if (!f)
    {
        return false;
    }
    char line[1024];
    loaded = 0;
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
    return true;
}

void load_variants()
{
    U32 loaded = 0, shipped = 0;
    const bool had_user_file = load_variants_file(variants_path(), loaded);
    // J1 (the user, 2026-11-07: a first start must not be two minutes of darkness): the viewer ships a list of the variants it uses (app_settings/vk_pipeline_variants_default.txt, made with
    // RK_VK_VARIANTS_EXPORT), so the very first start on a machine, whose own list does not exist yet, builds them all at start-up (behind the progress screen, see llstartup.cpp) instead of when the
    // first draw asks and is dropped
    load_variants_file(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "vk_pipeline_variants_default.txt"), shipped);
    LL_INFOS("Vulkan") << "pipeline variants recorded by earlier sessions: " << loaded << (had_user_file ? "" : " (no list of this machine yet)") << ", shipped list: " << shipped << ", " << gVariants.size() << " programs" << LL_ENDL;
}

// every variant known, to a file (RK_VK_VARIANTS_EXPORT=<path>: the shipped list is made from a session that used everything)
void export_variants()
{
    const char* path = getenv("RK_VK_VARIANTS_EXPORT");
    if (!path || !path[0])
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gVariantMutex);
    if (FILE* f = fopen(path, "wb"))
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
    }
}

void save_variants()
{
    export_variants();
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
    mPacketU = PacketUniforms();
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
        // RK_VK_COLD_SALT=<n> (a test of cold start times, 2026-11-12): the id bound in the SPIR-V header is raised by n, which changes the module's bytes and nothing it does, so the driver's own on disk shader
        // cache (NVIDIA's, which this program cannot switch off and which sits outside the program's directories) holds no pipeline for it and the driver compiles every pipeline again. A different n for each cold run.
        static const U32 cold_salt = []() { const char* e = getenv("RK_VK_COLD_SALT"); return e ? (U32)std::max(0, atoi(e)) : 0u; }();
        std::vector<U32> salted;
        if (cold_salt && info.mSpirv[s].size() > 5)
        {
            salted = info.mSpirv[s];
            salted[3] += cold_salt;
            mi.pCode = salted.data();
        }
        if (!vk_ok(vkCreateShaderModule(ctx.getDevice(), &mi, nullptr, &mModules[s]), "vkCreateShaderModule"))
        {
            destroy();
            return false;
        }
    }

    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        mShadow[k].assign(info.mBlockSizes[k], 0);
    }
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
            const bool ok = s.mIndexUniform >= 0 && (size_t)s.mIndexUniform < mInfo.mUniforms.size() && mInfo.mUniforms[s.mIndexUniform].mClass == UC_DRAW;
            mSlotOffset.push_back(ok ? mInfo.mUniforms[s.mIndexUniform].mOffset : 0);
            mSamplerTable.push_back(s.mType == "samplerCube" ? 1 : s.mType == "samplerCubeArray" ? 2 : s.mType == "sampler2DShadow" ? 3 : 0);
            if (!ok)
            {
                LL_WARNS("VkShader") << "program '" << mName << "': the sampler " << s.mName << " has no texture id uniform: it draws without its textures" << LL_ENDL;
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
        // <FS> how many programs read their textures through the texture handles
        static U32 sPrograms = 0, sBindlessPrograms = 0, sNoSamplers = 0;
        ++sPrograms;
        if (mBindless) ++sBindlessPrograms;
        if (mInfo.mSamplers.empty()) ++sNoSamplers;
        if (!mInfo.mSamplers.empty())
        {
            std::string outputs;
            for (const std::string& o : mInfo.mOutputs)
            {
                outputs += (outputs.empty() ? "" : ", ") + o;
            }
            LL_INFOS("VkShader") << "program '" << mName << "': " << (mBindless ? "texture handles" : "NO TEXTURES") << ", " << mInfo.mSamplers.size() << " samplers, outputs: " << outputs << LL_ENDL;
        }
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
                if (offset + comps * 4 <= mShadow[u.mClass].size())
                {
                    memcpy(&mShadow[u.mClass][offset], &values[(size_t)e * comps], comps * 4);
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
            memcpy(&mShadow[u.mClass][u.mOffset], &f, 4);
        }
        else if (u.mType == "int" || u.mType == "uint" || u.mType == "bool")
        {
            const S32 i = (S32)v;
            memcpy(&mShadow[u.mClass][u.mOffset], &i, 4);
        }
    }

    {
        // RK_VK_UNIFORM_DUMP=1: every program's loose uniforms and samplers, for the uniform class table (doc/vulkan/F0-inventory.md)
        static FILE* dump = getenv("RK_VK_UNIFORM_DUMP") ? fopen("D:/claude/run-logs/program-uniforms.txt", "wb") : nullptr;
        if (dump)
        {
            fprintf(dump, "PROGRAM\t%s\tbindless=%d\tblock=%zu\n", mName.c_str(), mBindless ? 1 : 0, mShadow[0].size() + mShadow[1].size() + mShadow[2].size() + mShadow[3].size() + mShadow[4].size());
            for (const LooseUniform& u : mInfo.mUniforms)
            {
                fprintf(dump, "U\t%s\t%s\t%u\t%u\n", u.mName.c_str(), u.mType.c_str(), u.mArraySize, u.mSize);
            }
            for (const SamplerInfo& sm : mInfo.mSamplers)
            {
                fprintf(dump, "S\t%s\n", sm.mName.c_str());
            }
            fflush(dump);
        }
    }
    mValid = true;
    for (bool& d : mDirty) d = true;
    {
        std::lock_guard<std::mutex> lock(gPipeDoneMutex);
        gLivePrograms.insert(this);
    }
    prewarm();
    return true;
}

bool Program::isLive(const Program* p)
{
    std::lock_guard<std::mutex> lock(gPipeDoneMutex);
    return gLivePrograms.count(const_cast<Program*>(p)) != 0;
}

void Program::poisonAll()
{
    std::lock_guard<std::mutex> lock(gPipeDoneMutex);
    for (Program* p : gLivePrograms)
    {
        for (U32 k = 0; k < UC_COUNT; ++k)
        {
            if (!p->mShadow[k].empty())
            {
                memset(p->mShadow[k].data(), 0xCD, p->mShadow[k].size());
                p->mDirty[k] = true;
            }
        }
    }
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
    for (auto& sh : mShadow) sh.clear();
    mValid = false;
}

// RK_VK_PHASES: per uniform name, how often it is written and how often the value really changes (classifies uniforms as per pass, per material or per draw)
struct UniformChangeStat { U64 mWrites = 0, mChanges = 0, mBytes = 0; U32 mSize = 0; };
static std::unordered_map<std::string, UniformChangeStat> gUniformChangeStats;
static void note_uniform_write(const std::string& program, const LooseUniform& u, bool changed, U32 bytes)
{
    UniformChangeStat& s = gUniformChangeStats[program + "|" + u.mName];
    ++s.mWrites;
    if (changed) { ++s.mChanges; s.mBytes += bytes; }
    s.mSize = std::max<U32>(s.mSize, bytes);
}

const Program::PacketUniforms& Program::packetUniforms()
{
    if (!mPacketU.mResolved)
    {
        auto find = [&](const char* name) -> U32
        {
            auto it = mUniformIndex.find(name);
            return it == mUniformIndex.end() ? ~0u : (U32)it->second;
        };
        mPacketU.mTable = find("rk_packet_table");
        mPacketU.mModels = find("rk_packet_models");
        mPacketU.mTexmats = find("rk_packet_texmats");
        mPacketU.mPbr = find("rk_packet_pbr");
        mPacketU.mLights = find("rk_light_table");
        mPacketU.mImpostors = find("rk_impostor_table");
        mPacketU.mMotion = find("rk_packet_motion");
        mPacketU.mPalIds = find("rk_packet_palids");        // D5 step 4: the frame's palette table of a rigged program
        mPacketU.mProj = find("rk_packet_proj");            // the pass's projection of a rigged program (a rigged vertex is projected from its eye space position, as the stock programs do)
        mPacketU.mResolved = true;
    }
    return mPacketU;
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
        U8* dst = &mShadow[u.mClass][u.mOffset + i * stride];
        if (memcmp(dst, data + i * components, components * 4) != 0)
        {
            memcpy(dst, data + i * components, components * 4);
            changed = true;
        }
    }
    if (changed)
    {
        mDirty[u.mClass] = true;      // the viewer re-sends the same values before most draws: only real changes cost an upload
    }
    if (RKVkBridge::gPhaseTiming) note_uniform_write(mName, u, changed, count * (u.mArraySize > 0 ? 16 : 4 * components));
}

void Program::writeRange(U32 cls, U32 offset, U32 size, const U8* data)
{
    if (cls >= UC_COUNT || (size_t)offset + size > mShadow[cls].size())
    {
        return;
    }
    U8* dst = &mShadow[cls][offset];
    if (memcmp(dst, data, size) != 0)
    {
        memcpy(dst, data, size);
        mDirty[cls] = true;
    }
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
            U8* dst = &mShadow[u.mClass][u.mOffset + m * stride + c * 16];
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
        mDirty[u.mClass] = true;
    }
    if (RKVkBridge::gPhaseTiming) note_uniform_write(mName, u, changed, count * columns * 16);
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
    // native programs: specialization constants
    VkSpecializationMapEntry spec_entries[16] = {};
    U32 spec_data[16] = {};
    VkSpecializationInfo spec_info = {};
    if (!mInfo.mSpec.empty())
    {
        const U32 n = (U32)std::min<size_t>(mInfo.mSpec.size(), 16);
        for (U32 i = 0; i < n; ++i)
        {
            spec_entries[i] = { mInfo.mSpec[i].first, i * 4, 4 };
            spec_data[i] = mInfo.mSpec[i].second;
        }
        spec_info.mapEntryCount = n;
        spec_info.pMapEntries = spec_entries;
        spec_info.dataSize = n * 4;
        spec_info.pData = spec_data;
    }
    for (U32 s = 0; s < 3; ++s)
    {
        if (mModules[s])
        {
            stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[stage_count].stage = bits[s];
            stages[stage_count].module = mModules[s];
            stages[stage_count].pName = "main";
            stages[stage_count].pSpecializationInfo = mInfo.mSpec.empty() ? nullptr : &spec_info;
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

    // what the driver says about this build: served from its pipeline cache or compiled (the stage entries are required to match the stage count)
    VkPipelineCreationFeedback feedback = {};
    VkPipelineCreationFeedback stage_feedback[4] = {};
    VkPipelineCreationFeedbackCreateInfo fi = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO };
    fi.pPipelineCreationFeedback = &feedback;
    fi.pipelineStageCreationFeedbackCount = stage_count;
    fi.pPipelineStageCreationFeedbacks = stage_feedback;
    ri.pNext = &fi;         // (stage_count is at most 3: vertex, fragment, geometry)
    auto note_cache = [&](VkResult res)
    {
        const bool hit = (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
        if (res == VK_SUCCESS && !hit)
        {
            gPipelineCacheMisses.fetch_add(1, std::memory_order_relaxed);
        }
        return res;
    };

    gi.flags = flags;
    gDebugProgram = getName().c_str();      // (validation messages about this pipeline name its program)
    // <FS> compile time baseline (PLAN-rt-reflection-pass.md step 0): RK_VK_PIPEDIAG=1 logs, for every pipeline the driver needed more than a second for, a hash of what it was given
    // and the size of the driver's pipeline cache before and after (a cache hit leaves the size as it was; a miss adds the pipeline)
    static const bool pipe_diag = []() { const char* e = getenv("RK_VK_PIPEDIAG"); return e && e[0] == '1'; }();
    if (pipe_diag)
    {
        auto cache_size = [&]() { size_t sz = 0; vkGetPipelineCacheData(ctx.getDevice(), r.mPipelineCache, &sz, nullptr); return sz; };
        const size_t before = cache_size();
        const auto t0 = std::chrono::steady_clock::now();
        const VkResult res = note_cache(vkCreateGraphicsPipelines(ctx.getDevice(), r.mPipelineCache, 1, &gi, nullptr, &out));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 1000.0)
        {
            auto fnv = [](const void* d, size_t n, U64 h) { const U8* p = (const U8*)d; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; };
            U64 h[3] = { 1469598103934665603ull, 1469598103934665603ull, 1469598103934665603ull };
            for (U32 s = 0; s < 3; ++s)
            {
                if (!mInfo.mSpirv[s].empty()) h[s] = fnv(mInfo.mSpirv[s].data(), mInfo.mSpirv[s].size() * 4, h[s]);
            }
            U64 hs = fnv(spec_data, mInfo.mSpec.size() * 4, 1469598103934665603ull);
            U64 hk = fnv(key.mColor, sizeof(key.mColor), 1469598103934665603ull);
            hk = fnv(&key.mDepth, sizeof(key.mDepth), hk);
            hk = fnv(&key.mColorCount, sizeof(key.mColorCount), hk);
            hk = fnv(&key.mTopology, sizeof(key.mTopology), hk);
            LL_INFOS("VkPipeline") << "PIPEDIAG " << mName << " took " << ms << " ms; vert " << std::hex << h[0] << " frag " << h[1] << " geom " << h[2] << " spec " << hs << " key " << hk << std::dec
                                   << " layout " << (void*)r.mPipelineLayout << " cache before " << before << " after " << cache_size() << " bytes" << LL_ENDL;
        }
        return res;
    }
    return note_cache(vkCreateGraphicsPipelines(ctx.getDevice(), r.mPipelineCache, 1, &gi, nullptr, &out));
}

void Program::requestBuild(const PipelineKey& key, bool prewarm)
{
    mPending.insert(key);
    mJobs.fetch_add(1, std::memory_order_acq_rel);
    if (prewarm)
    {
        // <FS> the RT Reflection pass is optional: the world draws without it (the surfaces keep the probe result) until it is built, so the start-up does not wait for it (PLAN-rt-reflection-pass.md step 3)
        if (!is_optional_program(mName))
        {
            gSyncStats.mPipePrewarmQueued.fetch_add(1, std::memory_order_relaxed);
        }
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
        if (d.mPrewarm)
        {
            if (!is_optional_program(p.mName))
            {
                gSyncStats.mPipePrewarmDone.fetch_add(1, std::memory_order_relaxed);
            }
            gSyncStats.mPipePrewarmMicros.fetch_add(d.mMicros, std::memory_order_relaxed);
            if (d.mMicros > 1000000)
            {
                // (a pre-warm build the driver had to compile: after a shader change the lit alpha programs take minutes; the draws that need it are dropped until it is done)
                LL_INFOS("VkPipeline") << "pre-warm pipeline built: " << p.mName << " (" << d.mKey.mColorCount << " colour targets) took " << (double)d.mMicros / 1000.0
                                       << " ms; draws dropped meanwhile: " << skipped << LL_ENDL;
            }
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
    static Renderer* sRenderer = new Renderer;      // never destroyed (see sGL in rkvkworker.cpp): late releases at process exit still reach it
    return *sRenderer;
}

bool Renderer::createLayouts()
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();

    // set 0: bindings 0 and 5..8 = the loose uniform blocks of the classes (dynamic offsets into the ring), bindings 1..4 = the named uniform blocks
    VkDescriptorSetLayoutBinding b0[9] = {};
    for (U32 i = 0; i < 9; ++i)
    {
        b0[i].binding = i;
        b0[i].descriptorType = (i == 0 || i >= 5) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = all_graphics_stages();
    }
    VkDescriptorSetLayoutCreateInfo l0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l0.bindingCount = 9;
    l0.pBindings = b0;
    if (!vk_ok(vkCreateDescriptorSetLayout(dev, &l0, nullptr, &mSet0Layout), "set 0 layout"))
    {
        return false;
    }

    // set 1: nothing any more (every texture is read through the handle map of set 2); the empty set stays in the pipeline layouts so that the numbers of sets 2 and 3 do not change
    VkDescriptorSetLayoutCreateInfo l1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    l1.bindingCount = 0;
    l1.pBindings = nullptr;
    if (!vk_ok(vkCreateDescriptorSetLayout(dev, &l1, nullptr, &mSet1Layout), "set 1 layout"))
    {
        return false;
    }

    // <FS> set 2: a table of sampled images that shaders index at run time, written as a frame needs them (render packets with textures). Not needed to run.
    // (required: every texture is read through the handle map and the tables of this set)
    const U32 slots = ctx.getCaps().mBindlessSlots;
    if (!slots || !ctx.getCaps().mBindlessUpdateUnused)
    {
        LL_WARNS("Vulkan") << "this device cannot index sampled images at run time with updates while in use (slots " << slots << ", update unused while pending "
                           << (int)ctx.getCaps().mBindlessUpdateUnused << "): the texture handles need both" << LL_ENDL;
        return false;
    }
    {
        // the 2D table is the one a scene fills (a heavy sim names thousands of textures in a frame); cubes, cube arrays and shadow maps are a handful
        U32 cap[BINDLESS_TABLES];
        cap[0] = slots;
        for (U32 t = 1; t < BINDLESS_TABLES; ++t)
        {
            cap[t] = ctx.getCaps().mBindlessSmall;
        }
        VkDescriptorSetLayoutBinding b2[BINDLESS_TABLES + 1] = {};
        VkDescriptorBindingFlags binding_flags[BINDLESS_TABLES + 1] = {};
        for (U32 t = 0; t < BINDLESS_TABLES; ++t)
        {
            b2[t].binding = t;      // 0: 2D (render packets and bindless programs), 1: cube, 2: cube array, 3: 2D shadow
            b2[t].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b2[t].descriptorCount = cap[t];
            b2[t].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;       // (compute: the bindless probe, RKVkWorker::describe_textures)
            binding_flags[t] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            // Entries are written in the middle of recording a frame, while work recorded earlier may still be pending: without this bit that is undefined for EVERY entry of the set
            binding_flags[t] |= VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        }
        {
            b2[BINDLESS_TABLES].binding = BINDLESS_TABLES;      // 4: the handle map (a storage buffer of uint, id -> descriptor index)
            b2[BINDLESS_TABLES].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b2[BINDLESS_TABLES].descriptorCount = 1;
            b2[BINDLESS_TABLES].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        const U32 binding_count = BINDLESS_TABLES + 1;
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        fi.bindingCount = binding_count;
        fi.pBindingFlags = binding_flags;
        VkDescriptorSetLayoutCreateInfo l2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        l2.pNext = &fi;
        l2.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        l2.bindingCount = binding_count;
        l2.pBindings = b2;
        bool ok = vk_ok(vkCreateDescriptorSetLayout(dev, &l2, nullptr, &mSet2Layout), "set 2 layout");
        if (ok)
        {
            U32 total = 0;
            for (U32 t = 0; t < BINDLESS_TABLES; ++t) total += cap[t];
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, total * Context::frameCount() }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Context::frameCount() } };
            VkDescriptorPoolCreateInfo bi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            bi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            bi.maxSets = Context::frameCount();
            bi.poolSizeCount = 2;
            bi.pPoolSizes = ps;
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
            // one map per frame in flight (host visible: a frame is seen as a whole through its own copy); in H1 every entry is its own slot
            const VkDeviceSize map_bytes = (VkDeviceSize)BINDLESS_TABLES * HANDLE_MAP_STRIDE * sizeof(U32);
            for (U32 f = 0; f < Context::frameCount() && ok; ++f)
            {
                ok = ctx.createBuffer(map_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Upload, mHandleMap[f]) && mHandleMap[f].mMapped;
                if (ok)
                {
                    U32* entries = (U32*)mHandleMap[f].mMapped;
                    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
                    {
                        for (U32 i = 0; i < HANDLE_MAP_STRIDE; ++i)
                        {
                            entries[t * HANDLE_MAP_STRIDE + i] = 0;
                        }
                    }
                    VkDescriptorBufferInfo bufinfo = { mHandleMap[f].mBuffer, 0, map_bytes };
                    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w.dstSet = mBindlessSet[f];
                    w.dstBinding = BINDLESS_TABLES;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w.pBufferInfo = &bufinfo;
                    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
                }
            }
            if (ok)
            {
                LL_INFOS("Vulkan") << "texture handles: a map of " << BINDLESS_TABLES * HANDLE_MAP_STRIDE << " handles per frame in flight" << LL_ENDL;
            }
        }
        if (ok)
        {
            for (U32 t = 0; t < BINDLESS_TABLES; ++t) mBindlessCap[t] = cap[t];
            LL_INFOS("Vulkan") << "bindless sampled image tables: " << cap[0] << " 2D slots and " << cap[1] << " each for cube, cube array and 2D shadow, per frame" << LL_ENDL;
        }
        else
        {
            if (mBindlessPool) vkDestroyDescriptorPool(dev, mBindlessPool, nullptr);
            if (mSet2Layout) vkDestroyDescriptorSetLayout(dev, mSet2Layout, nullptr);
            mBindlessPool = VK_NULL_HANDLE;
            mSet2Layout = VK_NULL_HANDLE;
            LL_WARNS("Vulkan") << "the texture handle tables could not be made" << LL_ENDL;
            return false;
        }
    }

    // <FS> set 3: the ray tracing scene (ray query shadows, RK_VK_RT_SHADOWS); without it the layout is what it was
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
        // RK_VK_RING_MEM=host (write-combined system memory) or cached (CPU-cached system memory) puts the uniform ring there instead of video memory behind the BAR
        static const char* const ring_mem = getenv("RK_VK_RING_MEM");
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
    for (U32 i = 0; i < Context::frameCount() + 2 && i < PALETTE_RING_SLOTS; ++i)
    {
        // <FS> the palette ring (see the header): read by address from the skinning pass and the rigged draws
        if (!ctx.createBuffer(PALETTE_RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, mPaletteRing[i]))
        {
            return false;
        }
    }
    // <FS> the palette arenas (rkvkbridge.h): an arena that cannot be made is not an error, the viewer copies the palettes as before
    for (U32 i = 0; i < PALETTE_ARENAS; ++i)
    {
        if (!ctx.createBuffer(PALETTE_ARENA_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, mPaletteArena[i]) || !mPaletteArena[i].mAddress || !mPaletteArena[i].mMapped)
        {
            break;
        }
        RKVkBridge::paletteArenaRegister(i, (F32*)mPaletteArena[i].mMapped, (U64)mPaletteArena[i].mAddress, (U32)(PALETTE_ARENA_BYTES / 4));
    }
    ctx.addAddressDescriber([this](unsigned long long at) -> std::string
    {
        auto close_to = [at](const Buffer& b, unsigned long long& offset) -> bool
        {
            const unsigned long long base = (unsigned long long)b.mAddress;
            if (!base || at + 0x10000 < base || at > base + (unsigned long long)b.mSize + 0x100000)
            {
                return false;
            }
            offset = at - base;
            return true;
        };
        unsigned long long offset = 0;
        for (U32 i = 0; i < Context::frameCount(); ++i)
        {
            if (close_to(mVertexRing[i], offset))
            {
                return "the vertex scratch ring of frame slot " + std::to_string(i) + ", offset " + std::to_string((long long)offset) + " of " + std::to_string((unsigned long long)mVertexRing[i].mSize) + (offset >= (unsigned long long)mVertexRing[i].mSize ? " (PAST THE END)" : "");
            }
            for (size_t k = 0; k < mVertexOverflow[i].size(); ++k)
            {
                if (close_to(mVertexOverflow[i][k], offset))
                {
                    return "overflow chunk " + std::to_string(k) + " of frame slot " + std::to_string(i) + ", offset " + std::to_string((long long)offset) + " of " + std::to_string((unsigned long long)mVertexOverflow[i][k].mSize) + (offset >= (unsigned long long)mVertexOverflow[i][k].mSize ? " (PAST THE END)" : "");
                }
            }
            if (close_to(mRing[i], offset))
            {
                return "the uniform ring of frame slot " + std::to_string(i) + ", offset " + std::to_string((long long)offset);
            }
        }
        return std::string();
    });
    if (!ctx.createBuffer(DUMMY_UBO_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Context::Memory::UploadDevice, mDummyUbo))
    {
        return false;
    }
    memset(mDummyUbo.mMapped, 0, DUMMY_UBO_BYTES);

    // descriptor sets for set 0
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[0].descriptorCount = UC_COUNT * Context::frameCount();
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
        VkWriteDescriptorSet w[9] = {};
        for (U32 b = 0; b < 9; ++b)
        {
            const bool dynamic = b == 0 || b >= 5;
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = mSet0[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[b].pBufferInfo = dynamic ? &ring : &dummy;
        }
        vkUpdateDescriptorSets(dev, 9, w, 0, nullptr);
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
        usizes[0].descriptorCount = UC_COUNT * UBO_SET_POOL;
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

    if (getenv("RK_VK_STANDIN_MAGENTA") && ctx.createImage2D(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mMagenta))
    {
        Buffer mstaging;
        if (ctx.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Context::Memory::Upload, mstaging))
        {
            const U8 magenta[4] = { 255, 0, 255, 255 };
            memcpy(mstaging.mMapped, magenta, 4);
            ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                imageBarrier(cmd, mMagenta.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy c = {};
                c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                c.imageExtent = { 1, 1, 1 };
                vkCmdCopyBufferToImage(cmd, mstaging.mBuffer, mMagenta.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                imageBarrier(cmd, mMagenta.mImage, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            });
            mMagenta.mLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ctx.waitIdle();
            vmaDestroyBuffer(ctx.getAllocator(), mstaging.mBuffer, mstaging.mAllocation);
        }
    }

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
    Context::instance().setDrawDescriber([](U64 serial) { return Renderer::instance().describeDraw(serial); });
    Context::instance().setBindlessAudit([]() { return Renderer::instance().auditBindless(); });
    Context::instance().setSlotDescriber([](U32 slot) { return Renderer::instance().describeSlot(slot); });
    LL_INFOS("Vulkan") << "Vulkan renderer ready" << LL_ENDL;
    return true;
}

void Renderer::gpuZone(const char* name, bool begin)
{
    if (!RKVkBridge::gPhaseTiming)
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
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;       // (Tracy begins this buffer twice: the second begin resets it)
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

static bool frag_stats()
{
    static const bool on = getenv("RK_VK_FRAGSTATS") != nullptr;
    return on;
}

// RK_VK_FRAGSTATS: the GPU time table switches its tag (a zone, a table draw type) inside a pass: the statistics query follows it, so the counts have the same rows as the times
void Renderer::statSegment(U32 tag)
{
    if (!mRendering || mStatOpen < 0 || !mStatPool[mSlot] || mStatTag[mSlot].size() >= MAX_STAT_QUERIES || mStatTag[mSlot][(size_t)mStatOpen] == tag)
    {
        return;
    }
    const U64 pixels = mStatPixels[mSlot][(size_t)mStatOpen];
    vkCmdEndQuery(mCmd, mStatPool[mSlot], (U32)mStatOpen);
    mStatOpen = (S32)mStatTag[mSlot].size();
    mStatTag[mSlot].push_back(tag);
    mStatPixels[mSlot].push_back(pixels);
    vkCmdBeginQuery(mCmd, mStatPool[mSlot], (U32)mStatOpen, 0);
}

static void stat_stamp_hook(U32 tag)
{
    Renderer::instance().statSegment(tag & ~Context::STAMP_DRAW);
}

bool Renderer::createQueryPools()
{
    if (frag_stats())
    {
        Context::instance().mStampHook = &stat_stamp_hook;
    }
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
        if (frag_stats())
        {
            qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            qi.queryCount = MAX_STAT_QUERIES;
            // the six values come back in the order of the bits: input vertices, input primitives (triangles), vertex shader runs, triangles into the clipper, triangles out of it
            // (what the clipper and the fixed-function cull let through), fragment shader runs
            qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
                                  | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT
                                  | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
            if (vkCreateQueryPool(dev, &qi, nullptr, &mStatPool[i]) != VK_SUCCESS)
            {
                mStatPool[i] = VK_NULL_HANDLE;
                LL_WARNS("Vulkan") << "RK_VK_FRAGSTATS: the device has no pipeline statistics queries" << LL_ENDL;
            }
            else
            {
                vkResetQueryPool(dev, mStatPool[i], 0, MAX_STAT_QUERIES);
            }
            qi.pipelineStatistics = 0;
        }
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
        if (mStatPool[i]) vkDestroyQueryPool(dev, mStatPool[i], nullptr);
        mOccPool[i] = VK_NULL_HANDLE;
        mTsPool[i] = VK_NULL_HANDLE;
        mStatPool[i] = VK_NULL_HANDLE;
        mStatTag[i].clear(); mStatPixels[i].clear();
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
                ++(data[(size_t)i * 2] ? mOccPassed : mOccNone);
            }
            else if (!mOccBegun[slot][i])
            {
                mQueryResults.push_back({ mOccSerial[slot][i], 0, 0 });        // no draw happened inside it: nothing passed
                ++mOccNeverDrawn;
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
    if (mStatPool[slot] && !mStatTag[slot].empty())
    {
        const U32 n = (U32)mStatTag[slot].size();
        std::vector<U64> data((size_t)n * 7, 0);       // per query: the six values and the availability
        vkGetQueryPoolResults(dev, mStatPool[slot], 0, n, data.size() * sizeof(U64), data.data(), 7 * sizeof(U64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (U32 i = 0; i < n; ++i)
        {
            if (data[(size_t)i * 7 + 6])
            {
                StatSum& s = mStatSums[mStatTag[slot][i]];
                s.mVertices += data[(size_t)i * 7];
                s.mPrimitives += data[(size_t)i * 7 + 1];
                s.mVsRuns += data[(size_t)i * 7 + 2];
                s.mClipIn += data[(size_t)i * 7 + 3];
                s.mClipOut += data[(size_t)i * 7 + 4];
                s.mInvocations += data[(size_t)i * 7 + 5];
                s.mPixels += mStatPixels[slot][i];
                ++s.mPasses;
            }
        }
        vkResetQueryPool(dev, mStatPool[slot], 0, n);
        mStatTag[slot].clear();
        mStatPixels[slot].clear();
        if (++mStatFrames >= 600)
        {
            // one row per pass target or zone (the tag the GPU time table uses): segments a frame, input vertices, triangles, vertex shader runs (all a frame), vertex shader runs per triangle
            // (the vertex cache: about 0.6 is good, 1.5 and up is poor), fragment shader runs a frame and per pixel of the pass
            std::vector<std::pair<double, U32>> order;
            for (const auto& kv : mStatSums) order.push_back({ (double)(kv.second.mVsRuns + kv.second.mInvocations), kv.first });
            std::sort(order.rbegin(), order.rend());
            LL_INFOS("VkFragStats") << "vertex and fragment work of " << mStatFrames << " frames by pass or zone (segments | input vertices | triangles | vertex runs | vertex runs per triangle | fragment runs | fragment runs per pixel | triangles into the clipper | share the clipper and cull dropped):" << LL_ENDL;
            for (size_t k = 0; k < order.size() && k < 32; ++k)
            {
                const StatSum& s = mStatSums[order[k].second];
                const double fr = (double)mStatFrames;
                LL_INFOS("VkFragStats") << "  [" << Context::stampTagName(order[k].second) << "] " << (double)s.mPasses / fr << " | " << (U64)((double)s.mVertices / fr) << " | " << (U64)((double)s.mPrimitives / fr)
                    << " | " << (U64)((double)s.mVsRuns / fr) << " | " << (s.mPrimitives ? (double)s.mVsRuns / (double)s.mPrimitives : 0.0) << " | " << (U64)((double)s.mInvocations / fr)
                    << " | " << (s.mPixels ? (double)s.mInvocations / (double)s.mPixels : 0.0)
                    << " | " << (U64)((double)s.mClipIn / fr) << " | " << (s.mClipIn ? 1.0 - (double)s.mClipOut / (double)s.mClipIn : 0.0) << LL_ENDL;
            }
            mStatSums.clear();
            mStatFrames = 0;
        }
    }
}

void Renderer::resolveQueries()
{
    if (mCmd != VK_NULL_HANDLE)
    {
        flushAndRestart(5);
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
        for (Buffer& b : mHandleMap)
        {
            if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        for (Buffer& b : mRing)
        {
            if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        for (Buffer& b : mPaletteRing)
        {
            if (b.valid()) vmaDestroyBuffer(Context::instance().getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        RKVkBridge::paletteArenaClear();        // (the viewer must not write into them any more)
        for (Buffer& b : mPaletteArena)
        {
            if (b.valid()) vmaDestroyBuffer(Context::instance().getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        for (Buffer& b : mVertexRing)
        {
            if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            b = Buffer();
        }
        for (std::vector<Buffer>& chunks : mVertexOverflow)
        {
            for (Buffer& b : chunks)
            {
                if (b.valid()) vmaDestroyBuffer(ctx.getAllocator(), b.mBuffer, b.mAllocation);
            }
            chunks.clear();
        }
        if (mDummyUbo.valid()) vmaDestroyBuffer(ctx.getAllocator(), mDummyUbo.mBuffer, mDummyUbo.mAllocation);
        mDummyUbo = Buffer();
        if (mWhite.valid())
        {
            vkDestroyImageView(dev, mWhite.mView, nullptr);
            vmaDestroyImage(ctx.getAllocator(), mWhite.mImage, mWhite.mAllocation);
        }
        mWhite = Image();
        if (mMagenta.valid())
        {
            vkDestroyImageView(dev, mMagenta.mView, nullptr);
            vmaDestroyImage(ctx.getAllocator(), mMagenta.mImage, mMagenta.mAllocation);
        }
        mMagenta = Image();
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
    mHandles = HandleTable();
    mPendingWrites.clear();
    mKeyedHandles.clear();
    mViewHandles.clear();
    for (auto& v : mHandleInfo) v.clear();
    mBindlessFilled = false;
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
    const U32 misses = gPipelineCacheMisses.load(std::memory_order_relaxed);
    if (misses == 0)
    {
        save_variants();        // (the variant list is small and follows what was drawn; the driver's cache holds nothing new)
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
            LL_INFOS("Vulkan") << "pipeline cache saved: " << data.size() / 1024 << " KB (" << misses << " pipelines compiled since the last save)" << LL_ENDL;
            gPipelineCacheMisses.fetch_sub(misses, std::memory_order_relaxed);
        }
    }
    save_variants();
}

VkSampler Renderer::getSampler(const SamplerDesc& wanted)
{
    SamplerDesc desc = wanted;
    desc.mLodBias = (!desc.mCompare && desc.mMip == VK_SAMPLER_MIPMAP_MODE_LINEAR) ? RKVkUpscale::lodBias() : 0.f;      // DLSS: sample textures sharper to make up for the smaller render size
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
    if (gPipelineCacheMisses.load(std::memory_order_relaxed) > 0 && ++mPipelineCacheLastSaveFrame >= 18000)
    {
        mPipelineCacheLastSaveFrame = 0;
        savePipelineCache();
    }
    harvestQueries(frame_slot);
    mCmd = cmd;
    handlesBeginFrame(frame_slot);          // what the finished frames gave back is free again; this frame's map catches up with the changes since it last ran
    mOccPending = mOccActive = -1;
    mStatOpen = -1;
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
    flushDeferredUploads(cmd);      // <FS> the texture uploads made between frames, before anything draws
    mRingPeak = std::max(mRingPeak, mRingUsed);
    mRingUsed = 0;
    ++mRingEpoch;       // every program's cached ring offset is void: the ring starts again from 0 (a mid-frame flush keeps the frame number, so the frame number cannot tell)
    if (!mRestarting)
    {
        mVertexRingUsed = 0;        // (a mid-frame flush keeps it going: the ray scene reads palettes and tables written earlier in the frame by address, and the new start would overwrite them)
        mPaletteSlot = (U32)(Context::instance().getFrameNumber() % (U64)(Context::frameCount() + 2));      // <FS> the palette ring: one slot per frame, round frameCount() + 2 of them
        mPaletteRingUsed = 0;
        RKVkBridge::paletteArenaFree(Context::instance().getFrameNumber());      // <FS> the palette arenas whose last reader frame is over
        if (mOverflowChunk || mOverflowUsed)
        {
            mOverflowIdle = 0;
        }
        else if (++mOverflowIdle >= 600)
        {
            // no frame used the overflow for 600 frames: nothing on the GPU reads it any more
            for (std::vector<Buffer>& chunks : mVertexOverflow)
            {
                for (Buffer& b : chunks)
                {
                    if (b.valid()) vmaDestroyBuffer(Context::instance().getAllocator(), b.mBuffer, b.mAllocation);
                }
                chunks.clear();
            }
            mOverflowIdle = 0;
        }
        mOverflowChunk = 0;
        mOverflowUsed = 0;
    }
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
        mBindlessBound = false;         // (what was queued and not flushed stays queued: the descriptors are written at the next flush, before the submit of the frame that maps to them)
    }
    mRtBoundSet = VK_NULL_HANDLE;
    for (U32& o : mAppliedDynamicOffset) o = 0xFFFFFFFF;
    mAppliedSet0 = VK_NULL_HANDLE;
    mAppliedVertexInputValid = false;
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
        for (bool& d : mProgram->mDirty) d = true;     // uniforms must be re-uploaded into this frame's ring
    }
}

void Renderer::endFrame()
{
    finishPassUse();
    flushPendingClear();
    endRendering(PE_FRAME);
    // descriptors queued during this frame are written before the command buffer is submitted
    bindlessFlush();
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

void Renderer::endRendering(PassEnd why)
{
    if (mRendering)
    {
        ++mPassEnds[why];
        {
            // <FS> RK_VK_PROBE=1: what each pass of one frame in 120 recorded (the draws, the colour targets, the size of the first one): finds a pass that draws nothing
            static const bool probe = getenv("RK_VK_PROBE") != nullptr;
            static U32 multi_logged = 0;
            if (probe && ((Context::instance().getFrameNumber() % 120) == 0 || (mColorCount >= 3 && ++multi_logged <= 400)))
            {
                const Image* first = mColorCount > 0 && mColorTargets[0] ? mColorTargets[0] : nullptr;
                LL_INFOS("VkProbe") << "pass ended (" << (why < PE_COUNT ? why : 0) << "): " << (mDrawCount - mPassDrawStart) << " draws, " << mColorCount << " colour target(s)"
                                    << (first ? (std::string(", first ") + std::to_string(first->mExtent.width) + "x" + std::to_string(first->mExtent.height)) : std::string())
                                    << (mDepthTarget ? ", with depth" : ", no depth") << LL_ENDL;
            }
        }
        mPassDrawsTotal += mDrawCount - mPassDrawStart;
        if (mDrawCount == mPassDrawStart)
        {
            ++mPassesEmpty;         // F3.5a: a pass that drew nothing (a clear, or a pass begun for a layout)
        }
        mLastPassEnd = why;
        if (mOccActive >= 0)
        {
            vkCmdEndQuery(mCmd, mOccPool[mSlot], (U32)mOccActive);      // a query cannot outlive its render pass instance
            mOccActive = -1;
        }
        if (mStatOpen >= 0)
        {
            vkCmdEndQuery(mCmd, mStatPool[mSlot], (U32)mStatOpen);
            mStatOpen = -1;
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
    finishPassUse();
    flushPendingClear();
    endRendering(PE_TARGET);
    // <FS> (mouselook dark state 2026-11-12) the read-only depth mode belongs to the depth image it was chosen for: a new depth image starts writable. It used to carry over from the last pass of the
    // previous target (the planar mirror view's lighting ends read-only), and the first pass after that took the old mode: a pending clear of the new depth went into a read-only pass (load, no clear,
    // no store) and was gone, so the depth of the main view held whatever it held before.
    {
        const Image* old_root = mDepthTarget ? (mDepthTarget->mOwner ? mDepthTarget->mOwner : mDepthTarget) : nullptr;
        const Image* new_root = depth ? (depth->mOwner ? depth->mOwner : depth) : nullptr;
        if (old_root != new_root)
        {
            mDepthRO = false;
        }
    }
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
// RK_VK_COARSE_BARRIERS=1 goes back to the old all-commands barrier.
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
    static const bool coarse = getenv("RK_VK_COARSE_BARRIERS") != nullptr;
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
    static const bool on = getenv("RK_VK_PHASES") != nullptr;
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
    {
        BarrierImageStat& pi = mBarrierPerImage[&image];
        ++pi.mCount;
        pi.mKind = image_kind(image);
    }
    {
        // F3.5c': who needed this transition: the draw's program, the viewer's GPU zone (when RKGpuZoneTimers is on) and the target of the pass that is being set up
        std::string who = image_kind(image) + " | " + layout_name(from) + ">" + layout_name(to) + " | " + mBarrierCause + " | " + (mProgram ? mProgram->mName : std::string("(no program)"));
        who += mZoneStack.empty() ? std::string(" | (no zone)") : " | " + Context::stampTagName(mZoneStack.back());
        who += " | into " + describeTarget();
        ++mBarrierWho[who];
    }
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
    std::vector<std::pair<const Image*, BarrierImageStat>> imgs(mBarrierPerImage.begin(), mBarrierPerImage.end());
    std::sort(imgs.begin(), imgs.end(), [](const auto& a, const auto& b) { return a.second.mCount > b.second.mCount; });
    out += " | images with the most transitions per frame (of " + std::to_string(imgs.size()) + " distinct):";
    for (size_t i = 0; i < imgs.size() && i < 8; ++i)
    {
        snprintf(buf, sizeof(buf), " [#%04x %s: %.1f]", (unsigned)(((size_t)imgs[i].first >> 4) & 0xFFFF), imgs[i].second.mKind.c_str(), (double)imgs[i].second.mCount / f);
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
            snprintf(buf, sizeof(buf), " [%s: %.0f copies of %.1f KB]", us.mName.c_str(), (double)us.mCopies / f, us.mCopies ? (double)us.mBytes / 1024.0 / (double)us.mCopies : 0.0);
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
        {
            // every name, appended to a file that keeps growing across sessions (the union over the scenes visited: doc/vulkan/uniform-classes.tsv is merged from it)
            static FILE* all = fopen("D:/claude/run-logs/uniform-stats-all.tsv", "ab");
            if (all)
            {
                for (const auto& kv : gUniformChangeStats)
                {
                    fprintf(all, "%s\t%.1f\t%.1f\t%u\t%zu\n", kv.first.c_str(), (double)kv.second.mChanges / f, (double)kv.second.mWrites / f, kv.second.mSize, (size_t)frames);
                }
                fflush(all);
            }
        }
        gUniformChangeStats.clear();
    }
    {
        // F3.5c': the transitions of the images with the most of them (the scene colour, the main depth) by the pass that needed them, per frame
        std::vector<std::pair<std::string, U32>> who(mBarrierWho.begin(), mBarrierWho.end());
        std::sort(who.begin(), who.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out += " | transitions by who needed them (image | layouts | cause | program | zone | the pass's target: per frame):";
        for (size_t i = 0; i < who.size() && i < 40; ++i)
        {
            snprintf(buf, sizeof(buf), " [%.1f] ", (double)who[i].second / f);
            out += buf + who[i].first + ";";
        }
        // the same by program only (the fourth field), all rows
        std::map<std::string, U32> by_program;
        for (const auto& kv : mBarrierWho)
        {
            size_t pos = 0;
            for (int k = 0; k < 3 && pos != std::string::npos; ++k)
            {
                pos = kv.first.find(" | ", pos);
                pos = pos == std::string::npos ? pos : pos + 3;
            }
            if (pos != std::string::npos)
            {
                const size_t end = kv.first.find(" | ", pos);
                by_program[kv.first.substr(pos, end == std::string::npos ? std::string::npos : end - pos)] += kv.second;
            }
        }
        std::vector<std::pair<std::string, U32>> bp(by_program.begin(), by_program.end());
        std::sort(bp.begin(), bp.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out += " | transitions by program (all rows, per frame):";
        for (size_t i = 0; i < bp.size() && i < 25; ++i)
        {
            snprintf(buf, sizeof(buf), " [%s: %.1f]", bp[i].first.c_str(), (double)bp[i].second / f);
            out += buf;
        }
        mBarrierWho.clear();
        // F3.5c: producer pass segments with a depth attachment: how often per frame, how many worker passes they took, draws that write / are read-only / plain, segments that were mixed (these are the ones that split)
        std::vector<std::pair<std::string, PassUse>> pu(mPassUse.begin(), mPassUse.end());
        std::sort(pu.begin(), pu.end(), [](const auto& a, const auto& b) { return a.second.mSegments > b.second.mSegments; });
        out += " | depth use per producer pass segment (target | first program: segments, worker passes, draws writing / read-only / plain, mixed segments; per frame):";
        for (size_t i = 0; i < pu.size() && i < 40; ++i)
        {
            const PassUse& u = pu[i].second;
            snprintf(buf, sizeof(buf), " [%.2f seg, %.2f passes, %.0f/%.0f/%.0f draws, %.2f mixed] ", (double)u.mSegments / f, (double)u.mWorkerPasses / f, (double)u.mWrites / f, (double)u.mRO / f, (double)u.mPlain / f, (double)u.mMixed / f);
            out += pu[i].first;
            out += buf;
            out += ";";
        }
        mPassUse.clear();
    }
    mBarrierStats.clear();
    mBarrierPerImage.clear();
    return out;
}

void Renderer::finishPassUse()
{
    if (mPUKey.empty())
    {
        return;
    }
    PassUse& u = mPassUse[mPUKey];
    ++u.mSegments;
    u.mWorkerPasses += mPassBeginsTotal - mPUStartBegins;
    u.mWrites += mPUWrites;
    u.mRO += mPURO;
    u.mPlain += mPUPlain;
    if (mPUWrites && mPURO)
    {
        ++u.mMixed;
    }
    mPUKey.clear();
    mPUWrites = mPURO = mPUPlain = 0;
}

// F3.5a: the render pass counters of the worker since the last call, per frame (always counted: plain increments), appended to the VkOps line
std::string Renderer::takePassReport(size_t frames)
{
    static const char* const kEnd[PE_COUNT] = { "frame end", "target change", "layout barrier", "depth read-only flip", "external (DLSS)", "command buffer", "mid-frame flush", "row copy", "copy" };
    const double f = (double)std::max<size_t>(1, frames);
    char buf[512];
    std::string out;
    snprintf(buf, sizeof(buf), "render passes (worker) per frame: %.1f begun (%.1f began with a clear, %.1f depth only, %.1f with read-only depth, %.1f drew nothing), %.1f draws per pass; clears: %.1f before a pass (its load op), %.1f inside a pass; passes began after (what ended the pass before):",
             (double)mPassBegins / f, (double)mPassBeginsClear / f, (double)mPassBeginsDepthOnly / f, (double)mPassBeginsDepthRO / f, (double)mPassesEmpty / f,
             mPassBegins ? (double)mPassDrawsTotal / (double)mPassBegins : 0.0,(double)mClearsPending / f, (double)mClearsInPass / f);
    out = buf;
    for (int i = 0; i < PE_COUNT; ++i)
    {
        snprintf(buf, sizeof(buf), " %s %.1f,", kEnd[i], (double)mPassBeginAfter[i] / f);
        out += buf;
    }
    snprintf(buf, sizeof(buf), " | depth read-only flips per frame: to read-only %.1f, back to writable %.1f (%.1f of them ended a pass) | ", (double)mDepthROFlips[1] / f, (double)mDepthROFlips[0] / f, (double)mDepthROFlipEnded / f);
    out += buf;
    mPassBegins = mPassBeginsClear = mPassBeginsDepthOnly = mPassBeginsDepthRO = mPassesEmpty = mClearsPending = mClearsInPass = mDepthROFlipEnded = mPassDrawsTotal = 0;
    for (int i = 0; i < PE_COUNT; ++i)
    {
        mPassEnds[i] = 0;
        mPassBeginAfter[i] = 0;
    }
    mDepthROFlips[0] = mDepthROFlips[1] = 0;
    if (!mDeclStats.empty())
    {
        // F3.5c: declared read-only depth against the inference, by program (per frame): agree (declared and the draw samples depth), declared only (declared, the draw does not sample it), VIOLATION (declared and the draw writes depth: the declaration is ignored), MISSING (the draw samples depth, nothing declared)
        std::vector<std::pair<U64, const Program*>> rows;
        U64 agree = 0, only = 0, viol = 0, miss = 0;
        for (const auto& kv : mDeclStats)
        {
            agree += kv.second.mAgree;
            only += kv.second.mDeclaredOnly;
            viol += kv.second.mViolation;
            miss += kv.second.mMissing;
            rows.push_back({ kv.second.mViolation * 1000000ull + kv.second.mMissing * 1000ull + kv.second.mAgree + kv.second.mDeclaredOnly, kv.first });
        }
        std::sort(rows.rbegin(), rows.rend());
        snprintf(buf, sizeof(buf), "declared read-only depth per frame: agree %.1f, declared only %.1f, VIOLATIONS %.1f, MISSING %.1f; by program (agree/only/violations/missing):", (double)agree / f, (double)only / f, (double)viol / f, (double)miss / f);
        out += buf;
        for (size_t i = 0; i < rows.size() && i < 24; ++i)
        {
            const DeclStat& d = mDeclStats[rows[i].second];
            snprintf(buf, sizeof(buf), " [%s: %.1f/%.1f/%.1f/%.1f]", d.mName.c_str(), (double)d.mAgree / f, (double)d.mDeclaredOnly / f, (double)d.mViolation / f, (double)d.mMissing / f);
            out += buf;
        }
        out += " | ";
        mDeclStats.clear();
    }
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
        endRendering(PE_BARRIER);
        mBarrierEndedPass = true;
    }
    else if (mPendingClear && !mInBegin)
    {
        if (is_attached())
        {
            flushPendingClear();     // the clear is recorded before the image changes layout
            endRendering(PE_BARRIER);
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
    if (RKVkPassCensus::sOn && RKVkPassCensus::sMode == 2)
    {
        const std::string target = describeTarget();
        RKVkPassCensus::renderPassBegin((std::string(mDepthRO ? "ro-depth " : "") + target + " | " + (mProgram ? mProgram->getName() : std::string("no program"))).c_str());
    }
    ++mPassBegins;
    ++mPassBeginsTotal;
    ++mPassBeginAfter[mLastPassEnd];
    mPassDrawStart = mDrawCount;
    if (mPendingClear)
    {
        ++mPassBeginsClear;
    }
    if (!mColorCount)
    {
        ++mPassBeginsDepthOnly;
    }
    if (mDepthRO)
    {
        ++mPassBeginsDepthRO;
    }
    mDrawStamps = false;
    mDrawStampCount = 0;
    if (RKVkBridge::gPhaseTiming)
    {
        // RK_VK_DRAW_STAMPS: 0 none, 1 (default) the passes of avatar impostors (four colour attachments and depth, up to 1024 per side, not the 343x343 probe faces), 2 every pass
        static const U64 draw_stamp_mode = []() { const char* e = getenv("RK_VK_DRAW_STAMPS"); return e && e[0] ? (U64)atoi(e) : 1ull; }();
        const Image* ref = mColorCount ? mColorTargets[0] : mDepthTarget;
        if (ref && draw_stamp_mode)
        {
            const U32 w = ref->mExtent.width, h = ref->mExtent.height;
            // (the reflection probe faces are 343x343 with the same attachments: told apart by that size. An impostor target can be any size from a few texels to 512x512, square or not: hangs and
            // faults have been in 128x256, 64x64 and 512x512)
            mDrawStamps = draw_stamp_mode >= 2 || (mColorCount == 4 && mDepthTarget && w <= 1024 && h <= 1024 && !(w == 343 && h == 343));
        }
        mPassTag = Context::stampTag(describeTarget());
        mBeginTag = mZoneStack.empty() ? mPassTag : mZoneStack.back();
        // a pass that starts with a clear: the layout changes, the begin and the clear get their own entry, so that they are not counted as the first draws' time
        mBeginSplit = mPendingClear != 0;
        Context::instance().gpuStamp(mBeginSplit ? Context::stampTag("pass begin + clear " + describeTarget()) : mBeginTag);      // RK_VK_PHASES: GPU time by render pass (or by the viewer's zone that spans it)
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
    if (mStatPool[mSlot] && mStatTag[mSlot].size() < MAX_STAT_QUERIES)
    {
        mStatOpen = (S32)mStatTag[mSlot].size();
        mStatTag[mSlot].push_back(RKVkBridge::gPhaseTiming ? mBeginTag : mPassTag);       // (the tag the GPU time table charges the pass's first draws to)
        mStatPixels[mSlot].push_back((U64)extent.width * extent.height);
        vkCmdBeginQuery(mCmd, mStatPool[mSlot], (U32)mStatOpen, 0);
    }
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
        ++mClearsPending;
        mPendingClear |= bits;      // becomes the load op of the next pass
        return;
    }
    ++mClearsInPass;

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
//-----------------------------------------------------------------------------------------------------
// Texture handles (doc/vulkan/PLAN-texture-handles.md, H3)
//-----------------------------------------------------------------------------------------------------
// A handle is a dense number in a table; what it shows is a descriptor index, written once, never changed (HandleTable has the bookkeeping, this has the Vulkan side). Every frame in flight has its
// own copy of the map (handle -> descriptor index) that is brought up to date from the log when the frame begins, so a frame sees one mapping from its first draw to its last. A descriptor is
// written into the sets of all frames at once: its index is not in the map of any frame that is running (it was free), and the binding is "update unused while pending".

void Renderer::bindlessStart()
{
    if (mBindlessFilled || mBindlessSet[0] == VK_NULL_HANDLE || !mWhite.valid())
    {
        return;
    }
    {
        static bool hooked = false;
        if (!hooked)
        {
            hooked = true;
            Context::instance().setViewForgetHook([](VkImageView v) { Renderer::instance().forgetViewHandles(v); });
        }
    }
    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
    {
        const Image* stand_in = &mWhite;
        if (t == 0 && mMagenta.valid()) stand_in = &mMagenta;       // (debug: RK_VK_STANDIN_MAGENTA)
        if (t == 1 && mFallback[1].valid()) stand_in = &mFallback[1];
        if (t == 2 && mFallback[2].valid()) stand_in = &mFallback[2];
        mPlaceholder[t] = { getSampler(SamplerDesc()), stand_in->mView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        mHandleInfo[t].assign(1, HandleInfo());
    }
    // every descriptor of every set holds the placeholder: an index nothing has written yet is read as the placeholder, not as an undefined descriptor (GPU assisted validation found that crash on 2026-10-22)
    std::vector<VkDescriptorImageInfo> infos[BINDLESS_TABLES];
    std::vector<VkWriteDescriptorSet> writes;
    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
    {
        infos[t].assign(mBindlessCap[t], mPlaceholder[t]);
    }
    for (U32 set = 0; set < Context::frameCount(); ++set)
    {
        for (U32 t = 0; t < BINDLESS_TABLES; ++t)
        {
            if (!mBindlessCap[t])
            {
                continue;
            }
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = mBindlessSet[set];
            w.dstBinding = t;
            w.dstArrayElement = 0;
            w.descriptorCount = mBindlessCap[t];
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = infos[t].data();
            writes.push_back(w);
        }
    }
    vkUpdateDescriptorSets(Context::instance().getDevice(), (U32)writes.size(), writes.data(), 0, nullptr);
    mHandles.init(mBindlessCap, Context::frameCount());
    mBindlessFilled = true;
    Context::instance().setImageDestroyHook([this](VkImage handle) { dropDeferredUploads(handle); });
    Context::instance().setViewReleaseHook([this](U64 view_id) { releaseViewHandles(view_id); });       // <FS> the guard: no handle shows a view whose image is being freed
    LL_INFOS("Vulkan") << "texture handles: every descriptor of the " << Context::frameCount() << " frame sets holds the placeholder (" << mBindlessCap[0] << " 2D descriptors)" << LL_ENDL;
}

void Renderer::handlesBeginFrame(U32 frame_slot)
{
    if (!mHandles.ready())
    {
        return;
    }
    Context& ctx = Context::instance();
    mHandles.retire([&ctx](U64 frame) { return ctx.frameFinished(frame); });
    if (frame_slot < Context::FRAMES_IN_FLIGHT && mHandleMap[frame_slot].valid() && mHandleMap[frame_slot].mMapped)
    {
        if (ctx.frameSplit())
        {
            // (GPU-LIFETIMES.md step 5: the apply belongs to the start of a frame; a frame that is already split must not take the whole log into its map: refused and counted, healthy = 0)
            static U64 refused = 0;
            if (++refused <= 5 || refused % 1000 == 0)
            {
                LL_WARNS("VkTex") << "handle map: the apply of the log was asked in a split frame: refused (" << refused << " so far)" << LL_ENDL;
            }
            return;
        }
        mHandles.applyTo(frame_slot, (U32*)mHandleMap[frame_slot].mMapped);
    }
}

U32 Renderer::handleNew(U32 table)
{
    if (!mSet2Layout || table >= BINDLESS_TABLES)
    {
        return 0;
    }
    bindlessStart();
    const U32 h = mHandles.newHandle(table);
    if (!h)
    {
        ++mStatBindlessRefused;
        return 0;
    }
    if (mHandleInfo[table].size() <= h)
    {
        mHandleInfo[table].resize(h + 1);
    }
    {
        HandleInfo keep = mHandleInfo[table][h];            // (the history stays across the handle being given to another texture)
        mHandleInfo[table][h] = HandleInfo();
        for (U32 k = 0; k < 6; ++k) mHandleInfo[table][h].mHist[k] = keep.mHist[k];
        mHandleInfo[table][h].mHistCount = keep.mHistCount;
        mHandleInfo[table][h].note(0, Context::instance().getFrameNumber(), 0, 0);
    }
    const U32 live = mHandles.liveHandles(table);
    if (table == 0 && live > mStatBindlessPeak.load(std::memory_order_relaxed))
    {
        mStatBindlessPeak.store(live, std::memory_order_relaxed);
    }
    return h;
}

bool Renderer::handleSet(U32 table, U32 handle, Image* image, VkSampler sampler, VkImageLayout layout)
{
    if (!mHandles.ready() || table >= BINDLESS_TABLES || !handle || !image || !image->valid() || !sampler)
    {
        return false;
    }
    const U32 d = mHandles.setContent(table, handle, Context::instance().getFrameNumber());
    if (!d)
    {
        ++mStatBindlessRefused;
        return false;
    }
    mPendingWrites.push_back({ table, d, { sampler, image->mView, layout } });
    if (mDescView[table].size() <= d)
    {
        mDescView[table].resize((size_t)d + 1024, 0);
    }
    mDescView[table][d] = image->mViewId;
    if (image->mViewId)
    {
        if (mViewUsers.size() > 40000)
        {
            // (views that were never destroyed through destroyImage: a bound, the guard misses what it dropped) GPU-LIFETIMES.md finding 5.6: counted
            static U64 cleared = 0;
            ++cleared;
            LL_WARNS("Vulkan") << "view release guard: " << mViewUsers.size() << " views noted, the list is cleared (" << cleared << " times so far): views noted before this are not guarded any more" << LL_ENDL;
            mViewUsers.clear();
        }
        mViewUsers[image->mViewId].push_back(((U64)table << 48) | ((U64)handle << 32) | (U64)d);
    }
    // (not in a split frame, except for a handle's first content: an earlier part of the frame is already submitted and reads this very map while it runs. An entry set here would reach it BEFORE
    // the descriptor is written (the write waits for the next flush, and the index is a recycled one that still holds the view of an image destroyed lately): a shadow alpha draw of that part
    // sampled freed memory, device lost 2026-11-09. The frame's later parts show the handle's old content until the next frame, whose map takes the entry from the log at its start.
    // A handle that the frame's map shows as the placeholder (never set, or its image was cleared in an earlier frame) has no old content to keep: left out, the draws of the later parts that
    // use it (a glyph page or a UI image first drawn after the join, a texture first shown by the G-buffer, a texture that came back after its image was dropped) sampled the white placeholder
    // for the rest of the frame (new glyph pages alone: 1 to 6 times in 1200 frames in a steady scene, the handle log of 2026-11-10). So its descriptor is written FIRST
    // (the index is free: no frame's map names it) and only then the map entry: the running part can reach the entry only through a row that still carries the number from the handle's previous
    // owner, and then finds a written descriptor, as the non-split path has it within one command buffer.)
    handleMapSet(table, handle, d);
    if (handle < mHandleInfo[table].size())
    {
        HandleInfo& hi = mHandleInfo[table][handle];
        hi.mViewId = image->mViewId;
        hi.mImage = (U64)image->mImage;
        hi.mView = image->mView;
        hi.mW = image->mExtent.width; hi.mH = image->mExtent.height; hi.mMips = image->mMipLevels; hi.mLayers = image->mLayers; hi.mFormat = (U32)image->mFormat; hi.mLayout = (U32)layout;
        hi.mFrame = Context::instance().getFrameNumber();
        ++hi.mSets;
        hi.note(1, hi.mFrame, d, image->mViewId);
    }
    return true;
}

// GPU-LIFETIMES.md step 5: the one write into the map of the frame being recorded (set 2 binding 4) that is not the apply at the start of the frame (handlesBeginFrame).
// Split frames (E2): the first part of the frame is already submitted and reads this very map while it runs, so a handle that shows something waits for the next frame, whose map takes the
// entry from the log at its start (refused and counted below). A handle that the frame's map shows as the placeholder (never set, or its image was cleared) has no old content to keep and
// takes its entry at once, descriptor first (see the note in handleSet). Outside a split frame the entry goes in as before.
void Renderer::handleMapSet(U32 table, U32 handle, U32 d)
{
    if (mCmd == VK_NULL_HANDLE || mSlot >= Context::FRAMES_IN_FLIGHT || !mHandleMap[mSlot].valid() || !mHandleMap[mSlot].mMapped)
    {
        return;
    }
    U32& entry = ((U32*)mHandleMap[mSlot].mMapped)[(table << 16) | handle];
    if (Context::instance().frameSplit())
    {
        if (entry != 0)
        {
            static std::atomic<U64> sSplitMapSkips{ 0 };
            const U64 skips = ++sSplitMapSkips;
            if (skips == 1 || skips % 1000 == 0)
            {
                LL_INFOS("VkTex") << "handle map: " << skips << " entries not written into the map of a split frame so far (the first part of the frame is on the GPU; the next frame takes them from the log)" << LL_ENDL;
            }
            return;
        }
        flushPendingWrites();
    }
    // the frame that is being recorded applied its map when it began: a handle first shown (or changed) in this frame reaches the frame's own copy now, so that its first draw is not the placeholder.
    // Its descriptor is written (bindlessFlush) before this frame is submitted; the maps of the frames already submitted are not touched (the log gives the next ones the entry).
    entry = d;
}


void Renderer::handleClear(U32 table, U32 handle)
{
    if (!mHandles.ready() || table >= BINDLESS_TABLES || !handle)
    {
        return;
    }
    mHandles.clearContent(table, handle, Context::instance().getFrameNumber());
    if (handle < mHandleInfo[table].size())
    {
        HandleInfo& hi = mHandleInfo[table][handle];
        hi.mView = VK_NULL_HANDLE;
        hi.mImage = 0;
        hi.note(2, Context::instance().getFrameNumber(), 0, 0);
    }
}

U32 Renderer::auditHandleState(U32 table, U32 handle) const
{
    if (!mHandles.ready() || table >= BINDLESS_TABLES || !handle)
    {
        return 0;
    }
    if (!mHandles.live(table, handle))
    {
        return 1;
    }
    const U32 d = mHandles.descriptorOf(table, handle);
    if (!d)
    {
        return 0;
    }
    const U64 view = d < mDescView[table].size() ? mDescView[table][d] : 0;
    U64 at = 0;
    return (view && Context::instance().destroyedViewFrame(view, at)) ? 2u : 0u;
}

void Renderer::releaseViewHandles(U64 view_id)
{
    if (!view_id || !mHandles.ready())
    {
        return;
    }
    // (looked up by the view, not scanned for: a destroyed image used to cost a pass over every handle and every descriptor, thousands of times while a world loads, and stalled the worker for seconds)
    auto found = mViewUsers.find(view_id);
    if (found == mViewUsers.end())
    {
        return;
    }
    const std::vector<U64> users = std::move(found->second);
    mViewUsers.erase(found);
    {
        // (what the finished frames gave back is back before the descriptors are judged: this runs at the start of a frame, before handlesBeginFrame has done it)
        Context& ctx = Context::instance();
        mHandles.retire([&ctx](U64 frame) { return ctx.frameFinished(frame); });
    }
    for (const U64 packed : users)
    {
        const U32 t = (U32)(packed >> 48), h = (U32)((packed >> 32) & 0xFFFFu), d = (U32)(packed & 0xFFFFFFFFu);
        if (t >= BINDLESS_TABLES)
        {
            continue;
        }
        // a handle that still shows this view shows the placeholder from now on
        std::vector<HandleInfo>& infos = mHandleInfo[t];
        if (h < infos.size() && infos[h].mViewId == view_id && mHandles.live(t, h))
        {
            static U32 said = 0;
            if (said++ < 40)
            {
                LL_WARNS("VkTex") << "handle guard: handle " << h << " of table " << t << " still showed the view of an image that is being freed (" << infos[h].mW << "x" << infos[h].mH << ", set "
                                  << (Context::instance().getFrameNumber() - infos[h].mFrame) << " frames ago): it shows the placeholder now" << LL_ENDL;
            }
            ++mViewGuardClears;
            handleClear(t, h);
            infos[h].mViewId = 0;
        }
        // the descriptor that was written for it: still holding the view, and still shown by a handle or waiting for its frames to finish (what a frame in flight reads through its map)
        std::vector<U64>& dv = mDescView[t];
        if (d < dv.size() && dv[d] == view_id)
        {
            if (mHandles.descriptorReachable(t, d))
            {
                static U32 said_d = 0;
                if (said_d++ < 60)
                {
                    LL_WARNS("VkTex") << "descriptor guard: table " << t << " index " << d << " holds the view of an image that is being freed and is still reachable (shown by a handle or waiting to be given back)" << LL_ENDL;
                }
                mPendingWrites.push_back({ t, d, mPlaceholder[t] });        // (the placeholder from now on: never memory that is about to go)
                ++mViewGuardClears;
            }
            dv[d] = 0;
        }
    }
}

void Renderer::handleRelease(U32 table, U32 handle)
{
    if (!mHandles.ready() || table >= BINDLESS_TABLES || !handle || !mHandles.live(table, handle))
    {
        return;
    }
    if (handle < mHandleInfo[table].size())
    {
        HandleInfo& hi = mHandleInfo[table][handle];
        if (hi.mKeyed)
        {
            mKeyedHandles.erase(hi.mKey);
            ++mHandleEpoch;
        }
        HandleInfo::Event hist[6];
        for (U32 k = 0; k < 6; ++k) hist[k] = hi.mHist[k];
        const U32 hist_count = hi.mHistCount;
        hi = HandleInfo();
        for (U32 k = 0; k < 6; ++k) hi.mHist[k] = hist[k];
        hi.mHistCount = hist_count;
        hi.note(3, Context::instance().getFrameNumber(), 0, 0);
    }
    mHandles.release(table, handle, Context::instance().getFrameNumber());
}

U32 Renderer::handleForImage(U32 table, Image* image, VkSampler sampler, VkImageLayout layout)
{
    if (!mSet2Layout || table >= BINDLESS_TABLES || !image || !image->valid() || !sampler)
    {
        return 0;
    }
    bindlessStart();
    if (!image->mViewId)
    {
        image->mViewId = nextViewId();
    }
    BindlessKey key;
    key.mViewId = image->mViewId;
    key.mSampler = sampler;
    key.mLayout = layout;
    key.mTable = table;
    auto it = mKeyedHandles.find(key);
    if (it != mKeyedHandles.end())
    {
        return it->second;
    }
    const U32 h = handleNew(table);
    if (!h)
    {
        return 0;
    }
    if (!handleSet(table, h, image, sampler, layout))
    {
        handleRelease(table, h);
        return 0;
    }
    mHandleInfo[table][h].mKeyed = true;
    mHandleInfo[table][h].mKey = key;
    mKeyedHandles.emplace(key, h);
    mViewHandles[image->mView].push_back({ table, h });
    return h;
}

void Renderer::forgetViewHandles(VkImageView view)
{
    auto it = mViewHandles.find(view);
    if (it == mViewHandles.end())
    {
        return;
    }
    const std::vector<std::pair<U32, U32>> list = it->second;
    mViewHandles.erase(it);
    for (const auto& p : list)
    {
        if (p.second < mHandleInfo[p.first].size() && mHandleInfo[p.first][p.second].mKeyed && mHandleInfo[p.first][p.second].mView == view)
        {
            handleRelease(p.first, p.second);
        }
    }
}

void Renderer::flushPendingWrites()
{
    if (mPendingWrites.empty())
    {
        return;
    }
    std::stable_sort(mPendingWrites.begin(), mPendingWrites.end(), [](const PendingWrite& a, const PendingWrite& b) { return a.mTable != b.mTable ? a.mTable < b.mTable : a.mDesc < b.mDesc; });
    // one entry per descriptor (a later write of the same index wins), runs of consecutive indices are one write
    std::vector<PendingWrite> list;
    list.reserve(mPendingWrites.size());
    for (const PendingWrite& w : mPendingWrites)
    {
        if (!list.empty() && list.back().mTable == w.mTable && list.back().mDesc == w.mDesc)
        {
            list.back() = w;
        }
        else
        {
            list.push_back(w);
        }
    }
    std::vector<VkDescriptorImageInfo> infos(list.size());
    for (size_t i = 0; i < list.size(); ++i)
    {
        infos[i] = list[i].mInfo;
    }
    std::vector<VkWriteDescriptorSet> writes;
    for (size_t i = 0; i < list.size();)
    {
        size_t j = i + 1;
        while (j < list.size() && list[j].mTable == list[i].mTable && list[j].mDesc == list[j - 1].mDesc + 1)
        {
            ++j;
        }
        for (U32 f = 0; f < Context::frameCount(); ++f)
        {
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = mBindlessSet[f];
            w.dstBinding = list[i].mTable;
            w.dstArrayElement = list[i].mDesc;
            w.descriptorCount = (U32)(j - i);
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &infos[i];
            writes.push_back(w);
        }
        i = j;
    }
    vkUpdateDescriptorSets(Context::instance().getDevice(), (U32)writes.size(), writes.data(), 0, nullptr);
    mStatBindlessWrites += list.size();
    mPendingWrites.clear();
}

void Renderer::bindlessFlush()
{
    if (!mSet2Layout)
    {
        return;
    }
    bindlessStart();
    flushPendingWrites();
    if (mCmd != VK_NULL_HANDLE && !mBindlessBound)
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
        if (!mVertexRing[mSlot].valid())
        {
            LL_WARNS_ONCE("Vulkan") << "vertex scratch ring exhausted" << LL_ENDL;
            return a;
        }
        // the ring is full: the scratch continues in the overflow chunks of this slot (made when first needed)
        std::vector<Buffer>& chunks = mVertexOverflow[mSlot];
        for (;;)
        {
            if (mOverflowChunk < chunks.size())
            {
                const Buffer& c = chunks[mOverflowChunk];
                const VkDeviceSize s = (mOverflowUsed + 15) / 16 * 16;
                if (s + size <= c.mSize)
                {
                    mOverflowUsed = (U32)(s + size);
                    a.mBuffer = c.mBuffer;
                    a.mOffset = s;
                    a.mPtr = (U8*)c.mMapped + s;
                    a.mAddress = c.mAddress ? c.mAddress + s : 0;
                    return a;
                }
                ++mOverflowChunk;
                mOverflowUsed = 0;
                continue;
            }
            Buffer b;
            if (size > VERTEX_OVERFLOW_CHUNK_BYTES || chunks.size() >= VERTEX_OVERFLOW_MAX_CHUNKS ||
                !Context::instance().createBuffer(VERTEX_OVERFLOW_CHUNK_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, Context::Memory::UploadDevice, b))
            {
                LL_WARNS_ONCE("Vulkan") << "vertex scratch ring and overflow chunks exhausted" << LL_ENDL;
                return a;
            }
            chunks.push_back(b);
        }
    }
    mVertexRingUsed = start + (U32)size;
    a.mBuffer = mVertexRing[mSlot].mBuffer;
    a.mOffset = start;
    a.mPtr = (U8*)mVertexRing[mSlot].mMapped + start;
    a.mAddress = mVertexRing[mSlot].mAddress ? mVertexRing[mSlot].mAddress + start : 0;
    return a;
}

// <FS> the palette ring (see the header): a slot's memory is not written again until the frame that could last read it is done. Full (or missing): the vertex ring as before, counted
Renderer::ScratchAlloc Renderer::allocPaletteScratch(VkDeviceSize size)
{
    const U32 slot = mPaletteSlot;
    const U32 start = (mPaletteRingUsed + 15) / 16 * 16;
    if (slot < PALETTE_RING_SLOTS && mPaletteRing[slot].valid() && start + size <= PALETTE_RING_BYTES)
    {
        mPaletteRingUsed = start + (U32)size;
        ScratchAlloc a;
        a.mBuffer = mPaletteRing[slot].mBuffer;
        a.mOffset = start;
        a.mPtr = (U8*)mPaletteRing[slot].mMapped + start;
        a.mAddress = mPaletteRing[slot].mAddress ? mPaletteRing[slot].mAddress + start : 0;
        return a;
    }
    ++mPaletteFallbacks;
    if (mPaletteFallbacks <= 20 || mPaletteFallbacks % 1000 == 0)
    {
        LL_WARNS("Vulkan") << "palette scratch ring full or missing: a palette of " << size << " bytes went to the vertex ring, whose memory is only safe for this frame (" << mPaletteFallbacks << " so far)" << LL_ENDL;
    }
    return allocVertexScratch(size);
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
    RKVkBridge::noteRendererThread();
    long long lap_t = RKVkBridge::gPhaseTiming ? RKVkBridge::phaseNow() : 0;
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
        static const bool diag = getenv("RK_VK_PHASES") != nullptr;
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
        // F3.5c: the viewer's declaration (DepthReadOnlyScope) against the inference
        static const bool decl_verify = getenv("RK_VK_PHASES") != nullptr || getenv("RK_VK_DEPTHRO_VERIFY") != nullptr;
        if (decl_verify && depth_root && mProgram)
        {
            DeclStat& ds = mDeclStats[mProgram];
            if (ds.mName.empty())
            {
                ds.mName = mProgram->mName;
            }
            if (mDepthStencil.mDeclaredRO)
            {
                if (mDepthStencil.mDepthWrite) { ++ds.mViolation; } else if (depth_ro) { ++ds.mAgree; } else { ++ds.mDeclaredOnly; }
            }
            else if (depth_ro)
            {
                ++ds.mMissing;
            }
        }
        if (depth_root && mProgram && (mDepthStencil.mDeclaredRO ? mDepthStencil.mDepthWrite : depth_ro))
        {
            // always on (a rare path): a draw that samples the depth it is attached to with no declaration (the inference still decides, F3.5c), or that writes depth under a declaration (the declaration is ignored)
            static std::unordered_set<const Program*> warned_missing, warned_violation;
            if ((mDepthStencil.mDeclaredRO ? warned_violation : warned_missing).insert(mProgram).second)
            {
                LL_WARNS("DepthRO") << (mDepthStencil.mDeclaredRO ? "a draw writes depth under a DepthReadOnlyScope (declaration ignored): program " : "a draw samples the depth attachment with no DepthReadOnlyScope (inferred): program ")
                    << mProgram->mName << ", pass target " << describeTarget() << LL_ENDL;
            }
        }
        if (mDepthStencil.mDeclaredRO && !mDepthStencil.mDepthWrite)
        {
            depth_ro = true;        // declared: the pass takes the read-only layout whether or not this draw samples the depth
        }
    }
    {
        // C9: a draw that neither writes depth nor reads the depth buffer works in either layout, so it keeps the one the pass is in (it used to force the
        // writable layout, which ended the pass and turned the layout back and forth around every draw that samples the depth: about 50 times a frame in a busy scene)
        static const bool sticky = getenv("RK_VK_STICKY_DEPTH") == nullptr || getenv("RK_VK_STICKY_DEPTH")[0] != '0';
        const DepthStencilState& ds = mDepthStencil;
        const bool stencil_writes = ds.mStencilTest && ds.mWriteMask && (ds.mFail != VK_STENCIL_OP_KEEP || ds.mDepthFail != VK_STENCIL_OP_KEEP || ds.mPass != VK_STENCIL_OP_KEEP);
        if (sticky && depth_root && !depth_sampled && !ds.mDepthWrite && !stencil_writes && mDepthRO && mDepthRORoot == depth_root && !(mPendingClear & CLEAR_DEPTH))
        {
            depth_ro = true;
        }
    }
    if (depth_ro != mDepthRO)
    {
        ++mDepthROFlips[depth_ro ? 1 : 0];
        if (mRendering)
        {
            ++mDepthROFlipEnded;
        }
        if (!mRendering && (mPendingClear & CLEAR_DEPTH))
        {
            mDepthRO = depth_ro;        // <FS> no pass is open: a pending depth clear starts the pass in the mode this draw needs (in the old read-only mode the clear was lost)
        }
        flushPendingClear();
        endRendering(PE_DEPTH_FLIP);
        mDepthRO = depth_ro;
    }
    if (depth_ro)
    {
        mDepthRORoot = depth_root;
    }
    {
        static const bool pu_diag = getenv("RK_VK_PHASES") != nullptr;
        if (pu_diag && depth_root && mProgram)
        {
            if (mPUKey.empty())
            {
                mPUKey = describeTarget() + " | " + mProgram->mName;
                mPUStartBegins = mPassBeginsTotal;
            }
            if (mDepthStencil.mDepthWrite) { ++mPUWrites; } else if (depth_ro) { ++mPURO; } else { ++mPUPlain; }
        }
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

    RKVkBridge::phaseLap(RKVkBridge::PH_A_TEX, lap_t);
    // 3. pass
    beginRenderingIfNeeded();
    RKVkBridge::phaseLap(RKVkBridge::PH_A_PASS, lap_t);

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
    RKVkBridge::phaseLap(RKVkBridge::PH_A_PIPE, lap_t);

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
    RKVkBridge::phaseLap(RKVkBridge::PH_A_DYN, lap_t);

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

    RKVkBridge::phaseLap(RKVkBridge::PH_A_VI, lap_t);
    RKVkBridge::gLastLap = 301;
    RKVkBridge::gDbg[0] = (unsigned long long)mProgram;
    RKVkBridge::gDbg[1] = mSlot;
    RKVkBridge::gDbg[2] = (unsigned long long)mCmd;
    RKVkBridge::gDbg[3] = (unsigned long long)mRing[mSlot].mMapped;
    // 5b. <FS> the handle of each sampler's texture is written into the loose uniform block (copied to the ring below); nothing is pushed
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
            U32 handle;
            if (img->mViewId && cached.mViewId == img->mViewId && cached.mSampler == sampler && cached.mLayout == layout && cached.mTable == table && cached.mEpoch == mHandleEpoch)
            {
                handle = cached.mHandle;           // (the same texture drawn again: no lookup)
            }
            else
            {
                handle = handleForImage(table, img, sampler, layout);
                cached = { img->mViewId, sampler, layout, table, mHandleEpoch, handle };
            }
            U32* dst = (U32*)(mProgram->mShadow[UC_DRAW].data() + mProgram->mSlotOffset[k]);
            const U32 id = (table << 16) | handle;      // the id of a handle in its table (the shader reads the frame's map with it)
            if (*dst != id)
            {
                *dst = id;
                mProgram->mDirty[UC_DRAW] = true;
            }
        }
        bindlessFlush();
    }
    // 5c. <FS> programs that trace rays: the frame slot's ray tracing scene is set 3
    if (mProgram->mUsesRt && mSet3Layout)
    {
        const VkDescriptorSet rt_set = Rt::descriptorSet(mSlot);
        const U32 rt_offset = Rt::tableOffset(mSlot);
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
    RKVkBridge::phaseLap(RKVkBridge::PH_A_SAMP, lap_t);
    // 6. loose uniforms, class by class: copy a class's shadow into the ring when it changed (or on its first use this frame)
    const U64 frame_number = Context::instance().getFrameNumber();
    const U64 ring_key = mRingEpoch;
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        std::vector<U8>& shadow = mProgram->mShadow[k];
        if (RKVkPassCensus::sOn && !shadow.empty()) RKVkPassCensus::classRead(mProgram->mCensus, k, (U32)shadow.size(), mProgram->mName.c_str());
        if (shadow.empty() || (!mProgram->mDirty[k] && mProgram->mRingFrame[k] == ring_key))
        {
            continue;
        }
        RKVkBridge::gLastLap = 302;
        const U32 ring_offset = allocUniform((U32)shadow.size());
        RKVkBridge::gLastLap = 303;
        if (ring_offset == 0xFFFFFFFFu)
        {
            mDrawBlocked = true;
            return;
        }
        mProgram->mRingOffset[k] = ring_offset;
        mDiagChanged = true;
        if (RKVkBridge::gPhaseTiming)
        {
            UniformCopyStat& us = mUniformCopies[mProgram];
            if (us.mName.empty())
            {
                us.mName = mProgram->mName;
            }
            ++us.mCopies;
            us.mBytes += shadow.size();
            if (mProgram->mDirty[k]) ++us.mDirty;
            {
                // would the block be the same as one already copied in this frame for this program and class?
                const U64* w = (const U64*)shadow.data();
                U64 h = 1469598103934665603ull ^ (U64)(size_t)mProgram ^ ((U64)k << 56);
                for (size_t m = 0; m < shadow.size() / 8; ++m)
                {
                    h = (h ^ w[m]) * 1099511628211ull;
                    h ^= h >> 29;
                }
                if (mUniformSeenFrame != frame_number)
                {
                    mUniformSeenFrame = frame_number;
                    mUniformSeen.clear();
                }
                if (!mUniformSeen.insert(h).second) ++us.mSame;
            }
        }
        memcpy((U8*)mRing[mSlot].mMapped + ring_offset, shadow.data(), shadow.size());
        mProgram->mRingFrame[k] = ring_key;
        mProgram->mDirty[k] = false;
    }
    RKVkBridge::gLastLap = 304;
    VkDescriptorSet set0 = mSet0[mSlot];
    if (mUbo[0] || mUbo[1] || mUbo[2] || mUbo[3])
    {
        set0 = uboSet();
    }
    RKVkBridge::gLastLap = 305;
    bool offsets_changed = !mSet0Bound || mAppliedSet0 != set0;
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        offsets_changed = offsets_changed || mAppliedDynamicOffset[k] != mProgram->mRingOffset[k];
    }
    if (offsets_changed)
    {
        mDiagChanged = true;
        vkCmdBindDescriptorSets(mCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &set0, UC_COUNT, mProgram->mRingOffset);       // the dynamic bindings 0, 5, 6, 7, 8 in this order
        for (U32 k = 0; k < UC_COUNT; ++k)
        {
            mAppliedDynamicOffset[k] = mProgram->mRingOffset[k];
        }
        mAppliedSet0 = set0;
        mSet0Bound = true;
    }

    RKVkBridge::phaseLap(RKVkBridge::PH_A_UNI, lap_t);
    RKVkBridge::phaseLap(RKVkBridge::PH_A_PUSH, lap_t);
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
    VkDescriptorBufferInfo infos[9];
    VkWriteDescriptorSet w[9] = {};
    for (U32 b = 0; b < 9; ++b)
    {
        const bool dynamic = b == 0 || b >= 5;
        if (dynamic)
        {
            infos[b] = { mRing[mSlot].mBuffer, 0, MAX_UNIFORM_RANGE };
        }
        else
        {
            infos[b] = mUbo[b - 1] ? VkDescriptorBufferInfo{ mUbo[b - 1], 0, mUboRange[b - 1] } : VkDescriptorBufferInfo{ mDummyUbo.mBuffer, 0, DUMMY_UBO_BYTES };
        }
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = set;
        w[b].dstBinding = b;
        w[b].descriptorCount = 1;
        w[b].descriptorType = dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(ctx.getDevice(), 9, w, 0, nullptr);
    sets[key] = set;
    mLastUboSet = set;
    mUboKeyDirty = false;
    return set;
}

std::string Renderer::describeSlot(U32 id) const
{
    // (a texture id is (table << 16) | handle)
    const U32 table = id >> 16, handle = id & 0xFFFF;
    char buf[420];
    if (table >= BINDLESS_TABLES || handle >= mHandleInfo[table].size())
    {
        snprintf(buf, sizeof(buf), "handle %u of table %u: beyond what was ever handed out (%zu)", handle, table, table < BINDLESS_TABLES ? mHandleInfo[table].size() : (size_t)0);
        return buf;
    }
    const HandleInfo& s = mHandleInfo[table][handle];
    const U64 frame = Context::instance().getFrameNumber();
    U64 destroyed_at = 0;
    const bool dead = s.mViewId && Context::instance().destroyedViewFrame(s.mViewId, destroyed_at);
    snprintf(buf, sizeof(buf), "handle %u of table %u: %s, descriptor index %u, last set %lld frames ago (%u sets), image %ux%u format %u mips %u layers %u layout %u, view id %llu, %s",
             handle, table, mHandles.live(table, handle) ? "live" : "FREE", mHandles.descriptorOf(table, handle), (long long)(frame - s.mFrame), s.mSets, s.mW, s.mH, s.mFormat, s.mMips, s.mLayers, s.mLayout,
             (unsigned long long)s.mViewId, dead ? "image DESTROYED" : "image alive");
    // <FS> 2026-11-14 (the user's eyes and ears bake slots show another texture's image while the texture object, its image and its handle are all right): what each frame slot's handle map holds for this
    // handle (the descriptor index the draws of that slot read it through) against the descriptor index the handle table says it has; a slot that differs shows another texture's image
    std::string maps;
    {
        // the handle's last events, newest first: made / set (descriptor index, the view it showed) / cleared / released, with the frames ago
        maps = " | history:";
        const U32 n = std::min<U32>(s.mHistCount, 6);
        for (U32 k = 0; k < n; ++k)
        {
            const HandleInfo::Event& e = s.mHist[(s.mHistCount - 1 - k) % 6];
            static const char* const kinds[4] = { "made", "set", "cleared", "released" };
            maps += std::string(" [") + kinds[e.mKind & 3] + (e.mKind == 1 ? " desc " + std::to_string(e.mDesc) + " view " + std::to_string((unsigned long long)e.mView) : std::string()) + ", " + std::to_string((long long)(frame - e.mFrame)) + " frames ago]";
        }
    }
    maps += " | handle map by frame slot (expected descriptor " + std::to_string(mHandles.descriptorOf(table, handle)) + "):";
    for (U32 k = 0; k < Context::frameCount() && k < Context::FRAMES_IN_FLIGHT; ++k)
    {
        if (mHandleMap[k].valid() && mHandleMap[k].mMapped)
        {
            const U32 value = ((const U32*)mHandleMap[k].mMapped)[(table << 16) | handle];
            maps += " slot" + std::to_string(k) + "=" + std::to_string(value) + (value == mHandles.descriptorOf(table, handle) ? "" : " DIFFERS");
        }
    }
    return std::string(buf) + ", image handle " + std::to_string((unsigned long long)s.mImage) + ": " + Context::instance().describeBinding(s.mImage) + " | " + RKVkWorker::describe_texture_of_view(s.mViewId) + maps;
}

std::string Renderer::auditBindless() const
{
    // what the handle tables hold at this moment (a live handle that shows an image destroyed lately is normal until its owner sets it again)
    const Context& ctx = Context::instance();
    unsigned stale = 0;
    for (U32 t = 0; t < BINDLESS_TABLES; ++t)
    {
        for (U32 h = 1; h < (U32)mHandleInfo[t].size(); ++h)
        {
            const HandleInfo& s = mHandleInfo[t][h];
            U64 destroyed_at = 0;
            if (mHandles.live(t, h) && s.mViewId && ctx.destroyedViewFrame(s.mViewId, destroyed_at))
            {
                ++stale;
            }
        }
    }
    char buf[400];
    snprintf(buf, sizeof(buf), "handles live: %u (2D) %u %u %u, descriptors in use %u (2D), %u of the live handles show an image destroyed lately, %llu descriptor sets so far, refused: %llu handles, %llu descriptors, %zu retirements waiting",
             mHandles.liveHandles(0), mHandles.liveHandles(1), mHandles.liveHandles(2), mHandles.liveHandles(3), mHandles.descriptorsUsed(0), stale, (unsigned long long)mHandles.sets(),
             (unsigned long long)mHandles.refusedHandles(), (unsigned long long)mHandles.refusedDescriptors(), mHandles.pendingRetirements());
    return buf;
}

// One GPU timestamp after a draw, described well enough to find the draw again: when a frame hangs the watchdog (Context::beginFrame) names the first draw whose stamp never
// completed. The program, what was drawn, and the buffers (handle and offset: the arena buffers are shared, the offset tells which range).
// RK_VK_ADDR_LOG is read once (getenv on every draw stamp made the render thread the bottleneck)
static bool addr_log_on()
{
    static const bool on = getenv("RK_VK_ADDR_LOG") != nullptr;
    return on;
}

U32 Renderer::drawTag(const char* what, U32 count, U32 first, S32 vertex_offset)
{
    char text[3200];
    int n = snprintf(text, sizeof(text), "draw %u: %s, %s %u from %u, vertex offset %d, vertex buffer %llx+%llu, index buffer %llx+%llu", ++mDrawStampCount, mProgram ? mProgram->getName().c_str() : "(no program)", what, count, first,
                     vertex_offset, (unsigned long long)(uintptr_t)mVertexBuffer, (unsigned long long)mVertexOffset, (unsigned long long)(uintptr_t)mIndexBuffer, (unsigned long long)mIndexOffset);
    if (mProgram && n > 0 && n < (int)sizeof(text) - 80)
    {
        // the bindless texture slots this draw samples (0 is the stand-in): a stale or recycled slot is one more way a draw can read memory that is not its own
        n += snprintf(text + n, sizeof(text) - n, ", texture slots");
        const std::vector<U8>& draw_class = mProgram->mShadow[UC_DRAW];
        for (size_t k = 0; k < mProgram->mSlotOffset.size() && k < 16 && n < (int)sizeof(text) - 12; ++k)
        {
            const U32 offset = mProgram->mSlotOffset[k];
            if (offset + sizeof(U32) <= draw_class.size())
            {
                U32 slot = 0;
                memcpy(&slot, draw_class.data() + offset, sizeof(U32));
                n += snprintf(text + n, sizeof(text) - n, " %u", slot);
            }
        }
    }
    const bool avatar_draw = mProgram && mProgram->getName().find("Avatar") != std::string::npos;       // the draws that fault: detail without the address log (a handful a frame)
    if (mProgram && n > 0 && n < (int)sizeof(text) - 400 && (addr_log_on() || avatar_draw))
    {
        // where the vertex and index data of this draw are (the memory ranges the address log saw), for a fault that reads somewhere none of them is
        const Context& ctx = Context::instance();
        static thread_local std::unordered_map<U64, std::string> cache;       // (the scan of the event ring is slow: once a frame per buffer)
        static thread_local U64 cache_frame = ~0ull;
        if (cache_frame != ctx.getFrameNumber())
        {
            cache.clear();
            cache_frame = ctx.getFrameNumber();
        }
        auto where = [&](U64 handle) -> const std::string& {
            auto it = cache.find(handle);
            if (it == cache.end())
            {
                it = cache.emplace(handle, ctx.describeBinding(handle, true)).first;
            }
            return it->second;
        };
        if (mUseVertexInput)
        {
            for (size_t b = 0; b < mVertexInput.mBindings.size() && b < 4 && n < (int)sizeof(text) - 250; ++b)
            {
                const VertexInput::Binding& vb = mVertexInput.mBindings[b];
                n += snprintf(text + n, sizeof(text) - n, " | vertex binding %zu: %llx+%llu stride %u, %s", b, (unsigned long long)(uintptr_t)vb.mBuffer, (unsigned long long)vb.mOffset, vb.mStride,
                              where((U64)(uintptr_t)vb.mBuffer).c_str());
            }
        }
        if (n < (int)sizeof(text) - 250)
        {
            n += snprintf(text + n, sizeof(text) - n, " | index buffer %s", where((U64)(uintptr_t)mIndexBuffer).c_str());
        }
    }
    if (mProgram && n > 0 && n < (int)sizeof(text) - 1200 && avatar_draw)
    {
        // the avatar draws are the ones that fault: what the pipeline and the uniforms were (draw class bytes in hex, the bone palette checked for values no matrix has)
        n += snprintf(text + n, sizeof(text) - n, " | pipeline %llx", (unsigned long long)(uintptr_t)mBoundPipeline);
        const std::vector<U8>& dc = mProgram->mShadow[UC_DRAW];
        n += snprintf(text + n, sizeof(text) - n, " | draw uniforms %zu bytes:", dc.size());
        for (size_t k = 0; k < dc.size() && k < 192 && n < (int)sizeof(text) - 400; ++k)
        {
            n += snprintf(text + n, sizeof(text) - n, "%s%02x", (k % 4 == 0) ? " " : "", dc[k]);
        }
        const std::vector<U8>& sk = mProgram->mShadow[UC_SKIN];
        U32 nonfinite = 0;
        float max_abs = 0.f;
        for (size_t k = 0; k + 4 <= sk.size(); k += 4)
        {
            float f;
            memcpy(&f, sk.data() + k, 4);
            if (!std::isfinite(f)) ++nonfinite;
            else max_abs = std::max(max_abs, std::fabs(f));
        }
        n += snprintf(text + n, sizeof(text) - n, " | skin uniforms %zu bytes, %u non-finite floats, largest magnitude %g", sk.size(), nonfinite, (double)max_abs);
        const std::vector<U8>& mc = mProgram->mShadow[UC_MATERIAL];
        n += snprintf(text + n, sizeof(text) - n, " | material uniforms %zu bytes:", mc.size());
        for (size_t k = 0; k < mc.size() && k < 96 && n < (int)sizeof(text) - 100; ++k)
        {
            n += snprintf(text + n, sizeof(text) - n, "%s%02x", (k % 4 == 0) ? " " : "", mc[k]);
        }
    }
    return Context::drawStampTag(text);
}

// One record per draw in the ring (see DrawRec) and one checkpoint before it. Costs a few tens of nanoseconds a draw; on by default, RK_VK_DRAW_RING=0 turns it off.
void Renderer::noteDraw(U8 kind, U32 count, U32 first, S32 vertex_offset)
{
    static const bool on = getenv("RK_VK_DRAW_RING") ? atoi(getenv("RK_VK_DRAW_RING")) != 0 : true;       // (on unless RK_VK_DRAW_RING=0: after a device lost the report names the draw the GPU had started)
    if (!on || !Context::instance().getCaps().mCheckpoints)
    {
        return;
    }
    constexpr size_t RING = 16384;
    if (mDrawRing.empty())
    {
        mDrawRing.resize(RING);
    }
    const U64 serial = ++mDrawSerial;
    DrawRec& r = mDrawRing[serial & (RING - 1)];
    r.mSerial = serial;
    r.mPassTag = mPassTag;
    if (mProgram)
    {
        if (mProgram->mNameTag == ~0u)
        {
            mProgram->mNameTag = Context::drawStampTag(mProgram->getName());
        }
        r.mNameTag = mProgram->mNameTag;
    }
    r.mKind = kind;
    r.mCount = count;
    r.mFirst = first;
    r.mVertexOffset = vertex_offset;
    r.mIndexBuffer = (U64)(uintptr_t)mIndexBuffer;
    r.mIndexOffset = (U64)mIndexOffset;
    r.mSlotCount = 0;
    if (mProgram)
    {
        const std::vector<U8>& draw_class = mProgram->mShadow[UC_DRAW];
        for (size_t k = 0; k < mProgram->mSlotOffset.size() && r.mSlotCount < 12; ++k)
        {
            const U32 offset = mProgram->mSlotOffset[k];
            if (offset + sizeof(U32) <= draw_class.size())
            {
                U32 slot = 0;
                memcpy(&slot, draw_class.data() + offset, sizeof(U32));
                r.mSlots[r.mSlotCount++] = (U16)slot;
            }
        }
    }
    Context::instance().serialCheckpoint(serial);
}

std::string Renderer::describeDraw(U64 serial) const
{
    constexpr size_t RING = 16384;
    char text[96];
    snprintf(text, sizeof(text), "draw #%llu", (unsigned long long)serial);
    if (mDrawRing.empty() || mDrawRing[serial & (RING - 1)].mSerial != serial)
    {
        return std::string(text) + " (no longer in the ring)";
    }
    const DrawRec& r = mDrawRing[serial & (RING - 1)];
    std::string out = text;
    out += std::string(" in pass [") + Context::stampTagName(r.mPassTag) + "], program [" + Context::stampTagName(r.mNameTag) + "], " + (r.mKind == 0 ? "indices " : (r.mKind == 1 ? "vertices " : "packet draws "))
         + std::to_string(r.mCount) + " from " + std::to_string(r.mFirst) + ", vertex offset " + std::to_string(r.mVertexOffset);
    char buf[80];
    snprintf(buf, sizeof(buf), ", index buffer %llx+%llu, texture slots", (unsigned long long)r.mIndexBuffer, (unsigned long long)r.mIndexOffset);
    out += buf;
    for (U32 k = 0; k < r.mSlotCount; ++k)
    {
        out += " " + std::to_string(r.mSlots[k]);
    }
    return out;
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
    long long cmd_t = RKVkBridge::gPhaseTiming ? RKVkBridge::phaseNow() : 0;
    startPendingQuery();
    noteDraw(1, vertex_count, first_vertex, 0);
    const U32 stamp_tag = mDrawStamps ? drawTag("vertices", vertex_count, first_vertex, 0) : 0u;
    if (stamp_tag)
    {
        Context::instance().drawCheckpoint(stamp_tag);       // (before the draw: a lost device names the draw the GPU had started)
    }
    vkCmdDraw(mCmd, vertex_count, 1, first_vertex, 0);
    if (stamp_tag)
    {
        Context::instance().gpuStamp(stamp_tag);
    }
    RKVkBridge::phaseLap(RKVkBridge::PH_A_CMD, cmd_t);
    ++mDrawCount;
    ++mModCount;
    return true;
}

// <FS> Render packets: the draws of one record share everything but their vertex count and their instance index (the shader finds the rest by it): the state is
// applied once and every draw is one command
bool Renderer::drawPackets(const U32* vertex_counts, U32 draws, VkBuffer indirect, VkDeviceSize indirect_offset, VkBuffer count_buffer, VkDeviceSize count_offset)
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
    noteDraw(2, draws, 0, 0);
    if (indirect != VK_NULL_HANDLE && count_buffer != VK_NULL_HANDLE)
    {
        // the cull appended the visible draws and counted them in the count buffer ('draws' is the most there can be): the hardware reads the count, the culled draws cost nothing
        vkCmdDrawIndirectCount(mCmd, indirect, indirect_offset, count_buffer, count_offset, draws, (U32)sizeof(VkDrawIndirectCommand));
        mDrawCount += draws;
    }
    else if (indirect != VK_NULL_HANDLE)
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
    if (mDrawStamps)
    {
        Context::instance().gpuStamp(drawTag("packet draws", draws, 0, 0));
    }
    ++mModCount;
    return true;
}

bool Renderer::drawPacketsIndexed(const VkDrawIndexedIndirectCommand* commands, U32 draws, VkBuffer indirect, VkDeviceSize indirect_offset, VkBuffer count_buffer, VkDeviceSize count_offset)
{
    if (!mProgram || !mProgram->isValid() || (!mColorCount && !mDepthTarget) || !mIndexBuffer || draws == 0)
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
    if (mIndexBuffer != mBoundIndexBuffer || mIndexOffset != mBoundIndexOffset || mIndexType != mBoundIndexType)
    {
        vkCmdBindIndexBuffer(mCmd, mIndexBuffer, mIndexOffset, mIndexType);
        mBoundIndexBuffer = mIndexBuffer;
        mBoundIndexOffset = mIndexOffset;
        mBoundIndexType = mIndexType;
    }
    startPendingQuery();
    noteDraw(2, draws, 0, 0);
    if (indirect != VK_NULL_HANDLE && count_buffer != VK_NULL_HANDLE)
    {
        vkCmdDrawIndexedIndirectCount(mCmd, indirect, indirect_offset, count_buffer, count_offset, draws, (U32)sizeof(VkDrawIndexedIndirectCommand));
        mDrawCount += draws;
    }
    else if (indirect != VK_NULL_HANDLE)
    {
        vkCmdDrawIndexedIndirect(mCmd, indirect, indirect_offset, draws, (U32)sizeof(VkDrawIndexedIndirectCommand));
        mDrawCount += draws;
    }
    else
    {
        for (U32 i = 0; i < draws; ++i)
        {
            if (commands[i].indexCount)
            {
                vkCmdDrawIndexed(mCmd, commands[i].indexCount, 1, commands[i].firstIndex, commands[i].vertexOffset, commands[i].firstInstance);
                ++mDrawCount;
            }
        }
    }
    if (mDrawStamps)
    {
        Context::instance().gpuStamp(drawTag("packet draws", draws, 0, 0));
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
    long long cmd_t = RKVkBridge::gPhaseTiming ? RKVkBridge::phaseNow() : 0;
    if (RKVkBridge::gPhaseTiming && RKVkBridge::onMainThread())
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
    noteDraw(0, index_count, first_index, vertex_offset);
    const U32 stamp_tag = mDrawStamps ? drawTag("indices", index_count, first_index, vertex_offset) : 0u;
    if (stamp_tag)
    {
        Context::instance().drawCheckpoint(stamp_tag);       // (before the draw: a lost device names the draw the GPU had started)
    }
    vkCmdDrawIndexed(mCmd, index_count, 1, first_index, vertex_offset, 0);
    if (stamp_tag)
    {
        Context::instance().gpuStamp(stamp_tag);
    }
    RKVkBridge::phaseLap(RKVkBridge::PH_A_CMD, cmd_t);
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
    if (mComputeRedirect != VK_NULL_HANDLE)
    {
        record(mComputeRedirect);       // E2: the compute queue's command buffer: nothing of the graphics state is touched
        return true;
    }
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    if (RKVkBridge::gPhaseTiming)
    {
        Context::instance().gpuStamp(Context::stampTag("external (DLSS)"));
    }
    for (const auto& i : images)
    {
        const char* prev_cause = setBarrierCause("external");
        ensureLayout(*i.first, i.second);
        setBarrierCause(prev_cause);
    }
    endRendering(PE_EXTERNAL);
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
    for (U32& o : mAppliedDynamicOffset) o = 0xFFFFFFFF;
    mAppliedSet0 = VK_NULL_HANDLE;
    for (S32& v : mAppliedViewport) v = -1;
    for (S32& v : mAppliedScissor) v = -1;
    return true;
}

bool Renderer::splitFrame(bool signal, U64& value)
{
    value = 0;
    Context& ctx = Context::instance();
    if (!ctx.asyncOn() || mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    finishPassUse();
    flushPendingClear();
    endRendering(PE_EXTERNAL);
    bindlessFlush();            // the descriptors queued so far are written before this part is submitted (the layout allows updating the unused ones while it runs)
    if (!ctx.splitFrame(signal, value))
    {
        return false;
    }
    // a new command buffer: nothing is bound in it (what beginFrame and recordExternal clear for the same reason)
    mCmd = ctx.getCommandBuffer();
    mOccPending = mOccActive = -1;
    mRendering = false;
    mPendingClear = 0;
    mStateApplied = false;
    mAppliedColorCount = 0xFFFF;
    mBoundPipeline = VK_NULL_HANDLE;
    mBoundIndexBuffer = VK_NULL_HANDLE;
    mUboKeyDirty = true;
    mLastUboSet = VK_NULL_HANDLE;
    mVertexInputDirty = true;
    mSet0Bound = false;
    if (mSet2Layout)
    {
        mBindlessBound = false;
    }
    mRtBoundSet = VK_NULL_HANDLE;
    for (U32& o : mAppliedDynamicOffset) o = 0xFFFFFFFF;
    mAppliedSet0 = VK_NULL_HANDLE;
    mAppliedVertexInputValid = false;
    for (S32& v : mAppliedViewport) v = -1;
    for (S32& v : mAppliedScissor) v = -1;
    return true;
}

void Renderer::withCommandBuffer(const std::function<void(VkCommandBuffer)>& record)
{
    if (mCmd != VK_NULL_HANDLE)
    {
        endRendering(PE_COMMAND_BUFFER);     // copies and blits are not allowed inside a render pass; the next draw resumes it
        record(mCmd);
        return;
    }
    flushDeferredUploadsNow();      // <FS> what was uploaded before this must be there first
    Context::instance().immediateSubmit(record);
}

Renderer::TexStats Renderer::takeTexStats()
{
    TexStats s;
    s.mBindlessPeak = mStatBindlessPeak.exchange(0);
    s.mBindlessWrites = mStatBindlessWrites.exchange(0);
    s.mBindlessRefused = mStatBindlessRefused.exchange(0);
    s.mAllocFailed = mStatAllocFailed.exchange(0);
    s.mAllocFailedBytes = mStatAllocFailedBytes.exchange(0);
    s.mAllocFailedLast[0] = mStatAllocW.load();
    s.mAllocFailedLast[1] = mStatAllocH.load();
    return s;
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
        // a texture without an image is drawn as the white stand-in: say so, with the memory the device has left
        ++mStatAllocFailed;
        mStatAllocFailedBytes += (U64)d.mWidth * d.mHeight * 4;
        mStatAllocW = d.mWidth;
        mStatAllocH = d.mHeight;
        static U32 said = 0;
        if (said++ < 20)
        {
            LL_WARNS("VkTex") << "no memory for a texture of " << d.mWidth << "x" << d.mHeight << " (" << d.mMips << " levels, format " << (S32)d.mFormat << "): " << ctx.memoryBudgetText() << LL_ENDL;
        }
        out = Image();
        return false;
    }
    {
        char tag[96];
        snprintf(tag, sizeof(tag), "texture %ux%ux%u fmt %u mips %u layers %u", d.mWidth, d.mHeight, depth, (unsigned)ii.format, (unsigned)ii.mipLevels, (unsigned)layers);
        ctx.noteImageTag((U64)out.mImage, tag);
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

// <FS> RK_VK_UPLOADLOG=1 (also on with the hitch watcher): what the worker spends on texture uploads, every 2 s (doc/vulkan/PLAN-hitchless-loading.md, texture uploads): the number, the bytes, the time
// filling the staging memory and the time of the whole call (fill + recording the copies and their barriers), the longest single upload, how many ended a render pass, how many had a buffer of their
// own (the old path outside a frame) and how many were kept for the next frame
static void note_upload_log(VkDeviceSize bytes, U64 fill_us, U64 total_us, U32 split, U32 own, U32 deferred)
{
    static const bool log_on = getenv("RK_VK_UPLOADLOG") != nullptr || getenv("RK_VK_HITCHWATCH") != nullptr;
    if (!log_on)
    {
        return;
    }
    struct Acc { U64 mN = 0, mBytes = 0, mFillUs = 0, mTotalUs = 0, mMaxUs = 0, mSplits = 0, mOwn = 0, mDeferred = 0; std::chrono::steady_clock::time_point mT0 = std::chrono::steady_clock::now(); };
    static Acc a;
    ++a.mN;
    a.mBytes += bytes;
    a.mFillUs += fill_us;
    a.mTotalUs += total_us;
    a.mMaxUs = std::max(a.mMaxUs, total_us);
    a.mSplits += split;
    a.mOwn += own;
    a.mDeferred += deferred;
    const auto now = std::chrono::steady_clock::now();
    if (now - a.mT0 >= std::chrono::seconds(2))
    {
        RK_DIAG("VkUpload") << "texture uploads in " << std::chrono::duration_cast<std::chrono::milliseconds>(now - a.mT0).count() << " ms: " << a.mN << " uploads, "
                             << (double)a.mBytes / (1024.0 * 1024.0) << " MB, fill " << (double)a.mFillUs / 1000.0 << " ms, whole calls " << (double)a.mTotalUs / 1000.0
                             << " ms, longest " << (double)a.mMaxUs / 1000.0 << " ms, " << a.mSplits << " ended a render pass, " << a.mOwn << " with a buffer of their own, "
                             << a.mDeferred << " kept for the next frame" << LL_ENDL;
        a = Acc();
    }
}

bool Renderer::uploadTextureDirect(Image& image, U32 mip, U32 layer, U32 x, U32 y, U32 width, U32 height, U32 depth, VkDeviceSize bytes, void (*fill)(void* ctx, U8* dst, VkDeviceSize bytes), void* fill_ctx)
{
    const auto t_start = std::chrono::steady_clock::now();
    RKVkBridge::noteRendererThread();
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

    // <FS> between frames: keep the upload for the next frame instead of a submit and a wait for each (see mDeferredUploads)
    static const bool batch = getenv("RK_VK_UPLOAD_BATCH") == nullptr || atoi(getenv("RK_VK_UPLOAD_BATCH")) != 0;
    if (mCmd == VK_NULL_HANDLE && batch && expected <= 64ull * 1024 * 1024)
    {
        const VkDeviceSize align = std::max<VkDeviceSize>(4, fi.mBytesPerBlock);
        U64 fill_us_d = 0;
        {
            std::lock_guard<std::mutex> lock(mDeferredMutex);
            VkDeviceSize start = 0;
            if (!mDeferredBlocks.empty())
            {
                start = (mDeferredBlockUsed + align - 1) / align * align;
            }
            if (mDeferredBlocks.empty() || start + expected > mDeferredBlocks.back().mSize)
            {
                Buffer block;
                for (size_t r = 0; r < mRetiredBlocks.size(); ++r)
                {
                    if (mRetiredBlocks[r].mBuffer.mSize >= expected + align && ctx.frameFinished(mRetiredBlocks[r].mFrame))
                    {
                        block = mRetiredBlocks[r].mBuffer;      // (a block the GPU is done with: its pages are already in memory)
                        mRetiredBlocks.erase(mRetiredBlocks.begin() + r);
                        break;
                    }
                }
                if (block.mBuffer == VK_NULL_HANDLE
                    && (!ctx.createBuffer(std::max<VkDeviceSize>(expected + align, 32ull * 1024 * 1024), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Context::Memory::Upload, block) || !block.mMapped))
                {
                    block = Buffer();
                }
                if (block.mBuffer == VK_NULL_HANDLE)
                {
                    goto old_path;
                }
                mDeferredBlocks.push_back(block);
                start = 0;
            }
            const auto t_fill_d = std::chrono::steady_clock::now();
            fill(fill_ctx, (U8*)mDeferredBlocks.back().mMapped + start, expected);
            fill_us_d = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_fill_d).count();
            mDeferredBlockUsed = start + expected;
            mDeferredBytes += expected;
            mDeferredUploads.push_back({ &image, image.mImage, mip, layer, x, y, width, height, std::max<U32>(depth, 1), start, (U32)(mDeferredBlocks.size() - 1) });
            ++mDeferredByHandle[image.mImage];
            ++mDeferredByImage[&image];
        }
        ++mUploadStats.mCount;
        mUploadStats.mBytes += expected;
        const U64 total_d = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start).count();
        RKVkAsync::noteTexWorker(0, fill_us_d, total_d, 1);
        note_upload_log(expected, fill_us_d, total_d, 0, 0, 1);
        if (mDeferredBytes > 256ull * 1024 * 1024)
        {
            flushDeferredUploadsNow();      // (bounds the staging memory held between frames)
        }
        return true;
    }
old_path:
    Buffer own;                 // used only when no frame is active
    StagingAlloc stage;
    if (mCmd != VK_NULL_HANDLE)
    {
        stage = ctx.allocStaging(expected, std::max<VkDeviceSize>(4, fi.mBytesPerBlock));
    }
    if (!stage.valid())
    {
        if (mCmd != VK_NULL_HANDLE)
        {
            static U32 said = 0;
            if (said++ < 10)
            {
                LL_WARNS("Vulkan") << "uploadTexture: no staging space in the frame for " << expected << " bytes, a buffer of its own is used (frame " << ctx.getFrameNumber() << ")" << LL_ENDL;
            }
        }
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
        {
            static const U32 tag = Context::stampTag("texture upload copies");
            static U64 stamped_frame = ~0ull;
            if (stamped_frame != Context::instance().getFrameNumber())      // (one a frame: there can be hundreds)
            {
                stamped_frame = Context::instance().getFrameNumber();
                Context::instance().gpuStamp(tag);
            }
        }
        transition(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = {};
        c.bufferOffset = stage.mOffset;
        c.imageSubresource = { image.mAspect, mip, is_3d ? 0 : layer, 1 };
        c.imageOffset = { (S32)x, (S32)y, is_3d ? (S32)layer : 0 };
        c.imageExtent = { width, height, std::max<U32>(depth, 1) };
        vkCmdCopyBufferToImage(cmd, stage.mBuffer, image.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    const bool used_own_buffer = own.valid();       // <FS> (the upload log below)
    if (own.valid())
    {
        if (mCmd != VK_NULL_HANDLE)
        {
            // the copy was recorded into the frame that is being recorded: it runs when that frame does, long after this call (waitIdle cannot wait for a command that has not been submitted;
            // the buffer used to be destroyed here, and the GPU copied from memory that belonged to something else: the device lost after snapshots, 2026-10-21, "inside [texture upload copies]")
            ctx.destroyBuffer(own);
        }
        else
        {
            ctx.waitIdle();     // the immediate path already waited; this just makes the free unconditional
            vmaDestroyBuffer(ctx.getAllocator(), own.mBuffer, own.mAllocation);
        }
    }
    const U64 total_us = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start).count();
    RKVkAsync::noteTexWorker(0, fill_us, total_us, 1);
    note_upload_log(expected, fill_us, total_us, (mCmd != VK_NULL_HANDLE && mRendering) ? 1 : 0, used_own_buffer ? 1 : 0, 0);
    return true;
}

// <FS> the staging blocks of a flush are kept (up to 6 of 40 MB or less) for the next uploads between frames; a block nobody asked for in 1800 frames is given back (freed once its frame has completed)
void Renderer::retireDeferredBlocks()
{
    Context& ctx = Context::instance();
    const U64 now = ctx.getFrameNumber();
    for (size_t r = 0; r < mRetiredBlocks.size(); )
    {
        if (now > mRetiredBlocks[r].mFrame + 1800 && ctx.frameFinished(mRetiredBlocks[r].mFrame))
        {
            ctx.destroyBuffer(mRetiredBlocks[r].mBuffer);
            mRetiredBlocks.erase(mRetiredBlocks.begin() + r);
        }
        else
        {
            ++r;
        }
    }
    for (Buffer& b : mDeferredBlocks)
    {
        if (b.mSize <= 40ull * 1024 * 1024 && mRetiredBlocks.size() < 6)
        {
            mRetiredBlocks.push_back({ b, now });
        }
        else if (mFlushSync)
        {
            mDoomedBlocks.push_back(b);     // (destroyed by flushDeferredUploadsNow as soon as its submit has been waited for)
        }
        else
        {
            ctx.destroyBuffer(b);       // (freed once the frame that copies from it has completed)
        }
    }
}

void Renderer::flushDeferredUploads(VkCommandBuffer cmd)
{
    std::lock_guard<std::mutex> lock(mDeferredMutex);
    if (mDeferredUploads.empty())
    {
        retireDeferredBlocks();
        mDeferredBlocks.clear();
        mDeferredBlockUsed = 0;
        mDeferredBytes = 0;
        return;
    }
    Context& ctx = Context::instance();
    // one barrier pair a texture: the uploads of one image together (a stable sort keeps the order of the levels), the regions of one staging block in one copy
    std::stable_sort(mDeferredUploads.begin(), mDeferredUploads.end(), [](const DeferredUpload& a, const DeferredUpload& b) { return a.mHandle < b.mHandle; });
    size_t i = 0;
    U32 images = 0, copies = 0;
    while (i < mDeferredUploads.size())
    {
        size_t j = i;
        while (j < mDeferredUploads.size() && mDeferredUploads[j].mHandle == mDeferredUploads[i].mHandle)
        {
            ++j;
        }
        Image* image = mDeferredUploads[i].mImage;
        if (image && image->valid() && image->mImage == mDeferredUploads[i].mHandle)
        {
            const bool is_3d = image->mViewType == VK_IMAGE_VIEW_TYPE_3D;
            transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            for (size_t k = i; k < j; )
            {
                size_t m = k;
                std::vector<VkBufferImageCopy> regions;
                while (m < j && mDeferredUploads[m].mBlock == mDeferredUploads[k].mBlock)
                {
                    const DeferredUpload& u = mDeferredUploads[m];
                    VkBufferImageCopy c = {};
                    c.bufferOffset = u.mOffset;
                    c.imageSubresource = { image->mAspect, u.mMip, is_3d ? 0 : u.mLayer, 1 };
                    c.imageOffset = { (S32)u.mX, (S32)u.mY, is_3d ? (S32)u.mLayer : 0 };
                    c.imageExtent = { u.mW, u.mH, u.mD };
                    regions.push_back(c);
                    ++m;
                }
                vkCmdCopyBufferToImage(cmd, mDeferredBlocks[mDeferredUploads[k].mBlock].mBuffer, image->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (U32)regions.size(), regions.data());
                ++copies;
                k = m;
            }
            transition(cmd, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            ++images;
        }
        else
        {
            // uploads kept for an image that is not where they were queued (or is gone): they cannot be written. Counted and the first few logged (healthy: none)
            static U64 lost = 0;
            lost += j - i;
            if (lost <= 20)
            {
                LL_WARNS("VkUpload") << "deferred uploads skipped at the flush: " << (j - i) << " for the image " << (unsigned long long)mDeferredUploads[i].mHandle << ", first mip " << mDeferredUploads[i].mMip
                                     << (image ? (image->valid() ? ", the struct holds another image" : ", the struct holds no image") : ", no struct") << " (" << lost << " so far)" << LL_ENDL;
            }
        }
        i = j;
    }
    static const bool log_on = getenv("RK_VK_UPLOADLOG") != nullptr || getenv("RK_VK_HITCHWATCH") != nullptr;
    if (log_on && mDeferredUploads.size() >= 200)
    {
        RK_DIAG("VkUpload") << "recorded " << mDeferredUploads.size() << " uploads kept from between frames: " << images << " images, " << copies << " copies, "
                             << (double)mDeferredBytes / (1024.0 * 1024.0) << " MB" << LL_ENDL;
    }
    retireDeferredBlocks();
    mDeferredBlocks.clear();
    mDeferredUploads.clear();
    mDeferredByHandle.clear();
    mDeferredByImage.clear();
    mDeferredBlockUsed = 0;
    mDeferredBytes = 0;
}

void Renderer::flushDeferredUploadsNow()
{
    {
        std::lock_guard<std::mutex> lock(mDeferredMutex);
        if (mDeferredUploads.empty())
        {
            return;
        }
    }
    if (mCmd != VK_NULL_HANDLE)
    {
        endRendering(PE_COMMAND_BUFFER);
        flushDeferredUploads(mCmd);
        return;
    }
    // <FS> (2026-11-14, the user: system memory at 33 GB for a few seconds at the loading screen, shared GPU memory, the host heap of our own allocations in the log) a flush made here waits for the GPU, so the blocks
    // it does not keep are destroyed at once. Their destroy used to be queued for a frame slot to come round, and at a loading screen no frame does: every flush added its staging memory to a queue that was
    // only emptied when frames started again
    mFlushSync = true;
    const bool submitted = Context::instance().immediateSubmit([&](VkCommandBuffer cmd) { flushDeferredUploads(cmd); });
    mFlushSync = false;
    std::vector<Buffer> doomed;
    {
        std::lock_guard<std::mutex> lock(mDeferredMutex);
        doomed.swap(mDoomedBlocks);
    }
    for (Buffer& b : doomed)
    {
        Context::instance().destroyBuffer(b, submitted);       // (a failed or timed out wait: the normal, deferred destroy)
    }
}

void Renderer::retargetDeferredUploads(Image* from, Image* to)
{
    // (2026-11-14, the ears: an upload kept between frames named the Image struct of the staged texture record, which tex_commit_image resets and erases when it moves the image into the texture it replaces: at the next
    // flush the pointer led to a reset or freed struct, the entry was skipped without a word, and levels 1 to 5 of the image stayed empty while level 0, flushed earlier, was there)
    std::lock_guard<std::mutex> lock(mDeferredMutex);
    static U64 asks = 0, hits = 0;      // (the proof line of the index: how many calls found something queued, how many the index let through at once)
    ++asks;
    const auto it = mDeferredByImage.find(from);
    if (it != mDeferredByImage.end() && from != to)
    {
        ++hits;
        const U32 n = it->second;
        for (DeferredUpload& u : mDeferredUploads)
        {
            if (u.mImage == from)
            {
                u.mImage = to;
            }
        }
        mDeferredByImage.erase(it);
        mDeferredByImage[to] += n;
    }
    if (asks % 2000 == 0)
    {
        RK_DIAG("VkUpload") << "deferred upload index: " << asks << " retargets (" << hits << " with uploads queued), " << mDeferredUploads.size() << " queued now" << LL_ENDL;
    }
}

void Renderer::dropDeferredUploads(VkImage handle)
{
    std::lock_guard<std::mutex> lock(mDeferredMutex);
    static U64 asks = 0, hits = 0;
    ++asks;
    const auto it = mDeferredByHandle.find(handle);
    if (it == mDeferredByHandle.end())
    {
        if (asks % 2000 == 0)
        {
            RK_DIAG("VkUpload") << "deferred upload index: " << asks << " drops (" << hits << " with uploads queued), " << mDeferredUploads.size() << " queued now" << LL_ENDL;
        }
        return;         // nothing is queued for this image
    }
    ++hits;
    mDeferredByHandle.erase(it);
    size_t keep = 0;
    for (size_t r = 0; r < mDeferredUploads.size(); ++r)
    {
        if (mDeferredUploads[r].mHandle == handle)
        {
            const auto bi = mDeferredByImage.find(mDeferredUploads[r].mImage);
            if (bi != mDeferredByImage.end() && --bi->second == 0)
            {
                mDeferredByImage.erase(bi);
            }
        }
        else
        {
            if (keep != r)
            {
                mDeferredUploads[keep] = mDeferredUploads[r];
            }
            ++keep;
        }
    }
    mDeferredUploads.resize(keep);          // (one pass, the order of the rest kept: the flush's stable sort needs it)
}

bool Renderer::copyBaseLevel(Image& src, Image& dst)
{
    ++mModCount;
    if (!src.valid() || !dst.valid() || src.mExtent.width != dst.mExtent.width || src.mExtent.height != dst.mExtent.height)
    {
        return false;
    }
    withCommandBuffer([&](VkCommandBuffer cmd) {
        transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region = {};
        region.srcSubresource = { src.mAspect, 0, 0, std::min<U32>(src.mLayers, dst.mLayers) };
        region.dstSubresource = { dst.mAspect, 0, 0, std::min<U32>(src.mLayers, dst.mLayers) };
        region.extent = { src.mExtent.width, src.mExtent.height, 1 };
        vkCmdCopyImage(cmd, src.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    return true;
}

bool Renderer::copyMipLevels(Image& src, U32 first, Image& dst)
{
    ++mModCount;
    if (!src.valid() || !dst.valid() || first >= src.mMipLevels || src.mLayers != dst.mLayers)
    {
        return false;
    }
    const U32 count = std::min<U32>(dst.mMipLevels, src.mMipLevels - first);
    // every level must be the size of the level it goes to (the chain of a smaller image is the chain of the larger one, shifted)
    std::vector<VkImageCopy> regions(count);
    for (U32 m = 0; m < count; ++m)
    {
        const U32 sw = std::max(1u, src.mExtent.width >> (first + m)), sh = std::max(1u, src.mExtent.height >> (first + m));
        const U32 dw = std::max(1u, dst.mExtent.width >> m), dh = std::max(1u, dst.mExtent.height >> m);
        if (sw != dw || sh != dh)
        {
            return false;
        }
        regions[m] = {};
        regions[m].srcSubresource = { src.mAspect, first + m, 0, src.mLayers };
        regions[m].dstSubresource = { dst.mAspect, m, 0, dst.mLayers };
        regions[m].extent = { sw, sh, 1 };
    }
    withCommandBuffer([&](VkCommandBuffer cmd) {
        transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cmd, src.mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions.data());
        transition(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
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
    flushDeferredUploadsNow();      // <FS> uploads made between frames are in the image before it is read
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
    {
        // every copy's source must be a staging block that still exists (a block given up reads memory that belongs to something else by now)
        Context& vctx = Context::instance();
        static U32 said = 0;
        U32 bad = 0;
        for (const BufferCopy& c : mPendingCopies)
        {
            if (c.mSrc != VK_NULL_HANDLE && !vctx.isStagingBuffer(c.mSrc))
            {
                ++bad;
                if (said++ < 10)
                {
                    LL_WARNS("Vulkan") << "buffer update: a pending copy of " << c.mSize << " bytes into buffer " << (unsigned long long)(uintptr_t)c.mDst << " offset " << c.mDstOffset << " reads staging buffer "
                                       << (unsigned long long)(uintptr_t)c.mSrc << " offset " << c.mSrcOffset << " which is no staging block of any frame slot (frame " << vctx.getFrameNumber() << ")" << LL_ENDL;
                }
            }
        }
        vctx.flight(Context::FL_MISC, 4ull, (unsigned long long)mPendingCopies.size(), (unsigned long long)bad, mPendingCopies.empty() ? 0ull : (unsigned long long)(uintptr_t)mPendingCopies.front().mSrc);
    }
    withCommandBuffer([&](VkCommandBuffer cmd) {
        {
            // (timestamp stamps: the device lost report names the stage the GPU never left; the work before the first pass was one lump)
            static const U32 tag = Context::stampTag("buffer update copies");
            Context::instance().gpuStamp(tag);
        }
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
        // <FS> two copies of one batch that write overlapping ranges of a buffer must not run in either order (the validation layer's WRITE_AFTER_WRITE, 2026-11-03: the later update has to win).
        // A barrier between them cost 6 ms of GPU time a frame (about 10 a frame), so the later copy is kept whole and the earlier one is cut to what the later ones do not write, on the CPU:
        // the pieces never overlap and need no barrier. A batch with a fill in it keeps the barrier (a fill has to stay a multiple of 4 bytes).
        bool batch_has_fill = false;
        for (const BufferCopy& c : mPendingCopies)
        {
            batch_has_fill = batch_has_fill || c.mSrc == VK_NULL_HANDLE;
        }
        // RK_VK_COPY_TRIM=0: the copies go in as they were queued, with nothing ordering overlapping ones (how it was before 2026-11-03: a same-session A/B of the trimming)
        static const bool trim_on = getenv("RK_VK_COPY_TRIM") == nullptr || atoi(getenv("RK_VK_COPY_TRIM")) != 0;
        if (!trim_on)
        {
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
            batch_has_fill = false;
        }
        U32 trimmed = 0, dropped = 0;
        if (trim_on && !batch_has_fill)
        {
            struct Piece { VkBuffer mDst, mSrc; VkDeviceSize mDstOffset, mSrcOffset, mSize; };
            std::vector<Piece> pieces;
            pieces.reserve(mPendingCopies.size());
            std::unordered_map<VkBuffer, std::map<VkDeviceSize, VkDeviceSize>> covered;       // by destination: the ranges the later copies write (disjoint, sorted by start)
            for (size_t i = mPendingCopies.size(); i-- > 0;)
            {
                const BufferCopy& c = mPendingCopies[i];
                std::map<VkDeviceSize, VkDeviceSize>& cov = covered[c.mDst];
                const VkDeviceSize lo = c.mDstOffset, hi = c.mDstOffset + c.mSize;
                VkDeviceSize pos = lo;
                {
                    auto after = cov.upper_bound(lo);
                    if (after != cov.begin())
                    {
                        auto before = std::prev(after);
                        pos = std::max(pos, before->second);
                    }
                }
                U32 emitted = 0;
                while (pos < hi)
                {
                    auto next = cov.lower_bound(pos);
                    const bool open = next == cov.end() || next->first >= hi;
                    const VkDeviceSize gap_end = open ? hi : next->first;
                    if (gap_end > pos)
                    {
                        pieces.push_back({ c.mDst, c.mSrc, pos, c.mSrcOffset + (pos - lo), gap_end - pos });
                        ++emitted;
                    }
                    if (open)
                    {
                        break;
                    }
                    pos = std::max(gap_end, next->second);
                }
                if (!emitted)
                {
                    ++dropped;
                }
                else if (emitted > 1 || pieces.back().mSize != c.mSize)
                {
                    ++trimmed;
                }
                // this copy's range joins the covered ones (merged with what it touches)
                VkDeviceSize mlo = lo, mhi = hi;
                auto at = cov.upper_bound(mlo);
                if (at != cov.begin())
                {
                    auto before = std::prev(at);
                    if (before->second >= mlo)
                    {
                        mlo = before->first;
                        mhi = std::max(mhi, before->second);
                        at = cov.erase(before);
                    }
                }
                while (at != cov.end() && at->first <= mhi)
                {
                    mhi = std::max(mhi, at->second);
                    at = cov.erase(at);
                }
                cov[mlo] = mhi;
            }
            for (const Piece& p : pieces)
            {
                VkBufferCopy region = { p.mSrcOffset, p.mDstOffset, p.mSize };
                vkCmdCopyBuffer(cmd, p.mSrc, p.mDst, 1, &region);
            }
            if (trimmed || dropped)
            {
                static U64 total_trimmed = 0, total_dropped = 0;
                static U32 said = 0;
                total_trimmed += trimmed;
                total_dropped += dropped;
                if (said++ < 20 || said % 2000 == 0)
                {
                    LL_INFOS("Vulkan") << "buffer updates: " << trimmed << " copies cut and " << dropped << " left out because later copies of the batch write the same bytes (batch of " << mPendingCopies.size() << "; "
                                       << total_trimmed << " and " << total_dropped << " so far)" << LL_ENDL;
                }
            }
        }
        std::unordered_map<VkBuffer, std::map<VkDeviceSize, VkDeviceSize>> written;
        U32 waw_barriers = 0;
        for (const BufferCopy& c : mPendingCopies)
        {
            if (!trim_on || !batch_has_fill)
            {
                break;          // (the pieces above were recorded, or the copies went in as queued)
            }
            {
                std::map<VkDeviceSize, VkDeviceSize>& iv = written[c.mDst];
                const VkDeviceSize lo = c.mDstOffset, hi = c.mDstOffset + c.mSize;
                bool overlap = false;
                auto next = iv.lower_bound(lo);
                if (next != iv.end() && next->first < hi)
                {
                    overlap = true;
                }
                if (!overlap && next != iv.begin())
                {
                    auto prev = std::prev(next);
                    overlap = prev->second > lo;
                }
                if (overlap)
                {
                    VkMemoryBarrier2 copy_after_copy = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                    copy_after_copy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    copy_after_copy.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    copy_after_copy.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    copy_after_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkDependencyInfo waw = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                    waw.memoryBarrierCount = 1;
                    waw.pMemoryBarriers = &copy_after_copy;
                    vkCmdPipelineBarrier2(cmd, &waw);
                    ++waw_barriers;
                    written.clear();            // (the barrier orders everything written so far)
                }
                written[c.mDst][lo] = hi;
            }
            if (c.mSrc == VK_NULL_HANDLE)
            {
                vkCmdFillBuffer(cmd, c.mDst, c.mDstOffset, c.mSize, (U32)c.mSrcOffset);
                continue;
            }
            VkBufferCopy region = { c.mSrcOffset, c.mDstOffset, c.mSize };
            vkCmdCopyBuffer(cmd, c.mSrc, c.mDst, 1, &region);
        }
        if (waw_barriers)
        {
            static U64 total = 0;
            static U32 said = 0;
            total += waw_barriers;
            if (said++ < 20 || said % 1000 == 0)
            {
                LL_INFOS("Vulkan") << "buffer updates: " << waw_barriers << " copy to copy barrier(s) for overlapping writes of one buffer in a batch of " << mPendingCopies.size() << " (" << total << " so far)" << LL_ENDL;
            }
        }
        VkMemoryBarrier2 after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        after.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        dep.pMemoryBarriers = &after;
        vkCmdPipelineBarrier2(cmd, &dep);
        {
            static const U32 tag = Context::stampTag("buffer update copies done");
            Context::instance().gpuStamp(tag);
        }
    });
    mPendingCopies.clear();
    return true;
}

bool Renderer::flushAndRestart(U32 why)
{
    if (mCmd == VK_NULL_HANDLE)
    {
        return false;
    }
    {
        static int logged = 0;
        if (logged < 60)
        {
            ++logged;
            LL_INFOS("Vulkan") << "mid-frame flush #" << (mFlushCount + 1) << " at frame " << Context::instance().getFrameNumber() << ", " << mDrawCount << " draws recorded so far" << LL_ENDL;
        }
    }
    flushPendingClear();
    endRendering(PE_FLUSH_RESTART);
    bindlessFlush();        // the draws recorded so far run before the partial frame returns: their descriptors must be written now (beginFrame below drops the queue)
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
    ++mFlushWhy[why < 6 ? why : 0];
    mRestarting = true;
    beginFrame(cmd, slot);      // forgets everything cached about the old recording
    mRestarting = false;
    mColorCount = color_count;
    for (U32 i = 0; i < color_count; ++i)
    {
        mColorTargets[i] = colors[i];
    }
    mDepthTarget = depth;
    return true;
}

std::string Renderer::takeFlushReport(double frames)
{
    static const char* const names[6] = { "other", "pixel readback", "glGetTexImage", "compressed readback", "snapshot boundary", "occlusion results" };
    U64 total = 0;
    for (U64 n : mFlushWhy) total += n;
    char buf[400];
    int len = snprintf(buf, sizeof(buf), "mid-frame flushes (each waits for the GPU): %llu over %.0f frames (%.2f per 100 frames)", (unsigned long long)total, frames, frames > 0 ? 100.0 * (double)total / frames : 0.0);
    for (U32 i = 0; i < 6 && len > 0 && len < (int)sizeof(buf) - 60; ++i)
    {
        if (mFlushWhy[i])
        {
            len += snprintf(buf + len, sizeof(buf) - len, ", %s %llu", names[i], (unsigned long long)mFlushWhy[i]);
        }
        mFlushWhy[i] = 0;
    }
    return buf;
}

bool Renderer::readbackRegion(Image& view_or_image, U32 mip, U32 layer, S32 x, S32 y, U32 w, U32 h, std::vector<U8>& texels)
{
    flushDeferredUploadsNow();      // <FS> uploads made between frames are in the image before it is read
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
    flushDeferredUploadsNow();      // <FS> uploads made between frames are in the image before it is read
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
    endRendering(PE_ROW_COPY);
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
    endRendering(PE_COPY);

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
        if (src_root->mFormat == dst_root->mFormat && src_root->mAspect == dst_root->mAspect)
        {
            // the same format and the same size: a plain copy. A blit would need the BLIT_DST feature of the format, which B10G11R11 (the reflection probe) does not have on Mesa (llvmpipe, RADV): the copy was refused and the probe was never filled
            VkImageCopy copy = {};
            copy.srcSubresource = { src_root->mAspect, src.mBaseMip, src.mBaseLayer, 1 };
            copy.srcOffset = { sx, sy, 0 };
            copy.dstSubresource = { dst_root->mAspect, dst_mip, dst_layer, 1 };
            copy.dstOffset = { dx, dy, 0 };
            copy.extent = { (U32)w, (U32)h, 1 };
            vkCmdCopyImage(mCmd, src_root->mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_root->mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        else
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
    }
    transition(mCmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

} // namespace RKVk

#endif // RK_VULKAN
