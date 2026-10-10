/**
 * @file llvkrt.cpp
 * @brief Ray tracing scene: bottom-level structures per vertex/index buffer pair, a top-level structure per frame (see llvkrt.h and doc/vulkan/PLAN-rt-shadows.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkrt.h"
#include "llvkrenderer.h"
#include "llvkmirror.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace LLVk
{
namespace Rt
{

namespace
{

U64 env_u64(const char* name, U64 fallback)
{
    const char* e = getenv(name);
    return e && e[0] ? (U64)atoll(e) : fallback;
}

// Several scene calls can happen within one frame (the main view, probes, snapshots). The top-level structure is rebuilt in place by each (the barrier before the build orders it
// after the earlier lighting), but what the build and the shaders read from memory the CPU writes has to be separate per call: the instances and the geometry table.
constexpr U32 kCalls = 6;
constexpr U32 kInstanceCapacity = 32768;                    // per call
constexpr VkDeviceSize kTableBytes = 2u << 20;              // per call: the geometry records of the alpha masked structures

// one geometry (index range) of an alpha masked structure as the ray query shader reads it (llvkshader.cpp declares the same struct, LLRtGeom)
struct GpuGeom
{
    U32 mPositions[2];
    U32 mIndices[2];
    U32 mTexcoords[2];
    U32 mColors[2];
    U32 mSlots[2];          // four 16 bit texture slots
    U32 mFlags;
    F32 mCutoff;
    U32 mFirst;             // first index of the range
    U32 mPad;
};
static_assert(sizeof(GpuGeom) == 56, "geometry record");
constexpr U32 kMaxRecords = (U32)(kTableBytes / sizeof(GpuGeom));
constexpr U32 kPasses = 8;                                  // shadow passes tracked: 0-3 sun cascades, 4-5 spot shadows, 6 other

struct Range
{
    U32 mFirst = 0, mCount = 0;
    bool operator<(const Range& o) const { return mFirst != o.mFirst ? mFirst < o.mFirst : mCount < o.mCount; }
    bool operator==(const Range& o) const { return mFirst == o.mFirst && mCount == o.mCount; }
    U64 id() const { return ((U64)mFirst << 32) | mCount; }
};

struct Key
{
    U32 mVb = 0, mIb = 0, mMatrixId = 0, mKind = 0;
    bool operator==(const Key& o) const { return mVb == o.mVb && mIb == o.mIb && mMatrixId == o.mMatrixId && mKind == o.mKind; }
};

struct KeyHash
{
    size_t operator()(const Key& k) const
    {
        U64 h = ((U64)k.mVb << 32) | k.mIb;
        h ^= ((U64)k.mMatrixId * 0x9E3779B97F4A7C15ull) + ((U64)k.mKind << 1) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
        return (size_t)(h ^ (h >> 33));
    }
};

struct Blas;

// what the shadow passes of a frame drew from one buffer pair with one matrix
struct Group
{
    Key mKey;
    F32 mModel[16] = {};
    std::vector<Range> mRanges;
    std::vector<AlphaInfo> mAlpha;      // kind 1: parallel to mRanges
    U64 mPassSeen[kPasses] = {};        // per shadow pass: the frame number + 1 of the latest note (0 = never)
    bool mSkip = false;                 // left out of this scene call
    Blas* mBlas = nullptr;              // its structure (sBlas nodes keep their place)
    LLVkGL::BufferInfo mVi, mIi;        // the buffers as of the latest note
    U64 mNotedTag = 0;                  // frame tag of the latest note
    F32 mPrevModel[16] = {};            // the matrix of the note before
    bool mMoving = false;               // the matrix changed between the last two notes
    bool mDynamic = false;              // its vertex data was rewritten since the structure was last built or refitted (this call)
    float mDistance = 0.f;              // eye space distance of the object's origin (this call)
};

struct Job;

struct Blas
{
    U64 mVbEpoch = 0, mIbEpoch = 0;
    U64 mVbVersion = ~0ull, mIbVersion = ~0ull;     // contents at the last build or refit
    std::vector<Range> mBuilt;                      // sorted: what the structure covers (the geometry index is the position in this list)
    std::unordered_map<U64, AlphaInfo> mAlpha;      // kind 1: the alpha test data of each range, as last drawn
    std::vector<U64> mBad;                          // ranges that failed the data check, valid for the buffer versions below (checked again when the buffers are written)
    U64 mBadVbVersion = ~0ull, mBadIbVersion = ~0ull;
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    VkDeviceAddress mAddress = 0;
    ArenaRef mStorage;
    VkDeviceSize mUpdateScratch = 0;
    U32 mPrims = 0;
    U32 mRefits = 0;                                // since the last build (a refit keeps the tree: after a number of them, build again)
    U64 mLastSeen = 0;                              // scene call number
    U64 mJobCall = 0;                               // the scene call a job for this structure belongs to (mJob is valid only then)
    Job* mJob = nullptr;
};

struct Tlas
{
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    Buffer mStorage;
    Buffer mInstances;                              // kCalls regions of mCapacity instances
    Buffer mTable;                                  // kCalls regions of kTableBytes
    U32 mCapacity = 0;
    VkDescriptorSet mSet = VK_NULL_HANDLE;
    U64 mCallFrame = ~0ull;                         // frame number of the calls counted in mCalls
    U32 mCalls = 0;
};

struct Stats
{
    U64 mScenes = 0, mBuilds = 0, mRefits = 0, mPrimsBuilt = 0, mPrimsRefit = 0, mDeferred = 0, mNoData = 0, mAlphaStructs = 0, mTableFull = 0, mFailed = 0, mEvicted = 0, mInstances = 0, mCasters = 0;
    U64 mOverCap = 0, mExtraCalls = 0, mBadVertices = 0, mBadMatrices = 0, mChecked = 0, mSingular = 0, mCompared = 0, mMismatched = 0;
    F32 mMaxScale = 0.f;
    Key mMaxScaleKey;
    U64 mWhyNew = 0, mWhyEpoch = 0, mWhyGrew = 0, mWhyIb = 0, mWhyRefitLimit = 0, mNewGroups = 0, mNewMasked = 0;      // why structures were built (and how many casters were new)
    double mCpuMs = 0, mCpuMsMax = 0;
    double mPhaseMs[6] = {};        // expire, order, scan (with sort), instances, recording, the rest
};

// Switches that can be changed while the viewer runs: the file named by LL_VK_RT_DEBUG_FILE (default <work>/rt-debug.txt) is read again every half second (key=value lines:
// single, norefit, keep, common_only, alpha, check, skipbad). Used to bisect picture problems without restarts.
struct DebugSwitches
{
    bool mSingle = false;           // only the first scene call of a frame builds; later calls keep that scene
    bool mNoRefit = false;          // a structure whose vertices changed is rebuilt, not refitted
    U64 mKeep = 8;                  // frames the casters of a cached pass are kept
    bool mCommonOnly = false;       // only casters that use the most common matrix (the region's): no moving objects
    bool mAlpha = true;             // alpha masked casters in the scene
    bool mCheck = true;             // data checks on built ranges
    bool mSkipBad = true;
    S32 mOnly = 0;                  // 0 all, 1 region matrix only, 2 region + null matrix, 3 region + non-null other matrices
    S32 mMoving = 0;                // 0 all, 1 leave out moving casters, 2 moving casters only
    S32 mDynamic = 0;               // 0 all, 1 leave out buffers rewritten since the build, 2 only those
    S32 mKind = 0;                  // 0 both, 1 opaque only, 2 masked only
    F32 mMinDist = 0.f, mMaxDist = 0.f;     // non-region casters only: eye space distance range (max 0 = no limit)
    bool mDump = false;             // log the non-region groups once per call while set (the first 80)
    bool mSingular = true;          // leave out instances whose matrix cannot be inverted (zero scale): the hardware inverts it to transform rays into the object
    U32 mMaxInstances = 0;          // 0 = no limit
    U32 mBuildMax = 0;              // 0 = the default per call
    bool mSerial = false;           // every build of a call is its own command with a barrier after it
    U32 mPad = 0;                   // bytes of scratch left unused after every job
    S32 mSkipBuf = 0;               // 0 all, 1 leave out groups with a buffer in video memory (device path, not checkable by the CPU), 2 leave out groups with only host-written buffers
    U32 mMaxVtx = 0;                // 0 = the buffer size / 16 - 1; otherwise this value is the maxVertex of every geometry (65535: any 16 bit index)
    bool mCompare = false;          // compare the system-memory copy of host-written buffers with the mapped video memory (a few per call), log mismatches
    bool mDynCasters = false;       // include casters with a matrix of their own (avatar attachments, moving linksets): they go wrong in the ray scene so far (see PLAN-rt-shadows.md)
    bool mCallLog = false;          // one line per scene call into <work>/rt-calls.log (wall clock, frame, call, jobs, instances)
};
DebugSwitches sDbg;
std::chrono::steady_clock::time_point sDbgRead;

void read_debug_switches()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - sDbgRead < std::chrono::milliseconds(500))
    {
        return;
    }
    sDbgRead = now;
    static const char* path = []() { const char* e = getenv("LL_VK_RT_DEBUG_FILE"); return e && e[0] ? e : "<work>/rt-debug.txt"; }();
    DebugSwitches d;
    d.mKeep = env_u64("LL_VK_RT_KEEP_FRAMES", 8);
    d.mAlpha = env_u64("LL_VK_RT_ALPHA", 1) != 0;
    d.mCheck = env_u64("LL_VK_RT_CHECK", 0) != 0;
    d.mSkipBad = env_u64("LL_VK_RT_SKIPBAD", 1) != 0;
    d.mDynCasters = env_u64("LL_VK_RT_DYNAMIC", 0) != 0;
    if (FILE* f = fopen(path, "rb"))
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            char key[64];
            long long v = 0;
            if (sscanf(line, "%63[^=]=%lld", key, &v) == 2)
            {
                const std::string k = key;
                if (k == "single") d.mSingle = v != 0;
                else if (k == "norefit") d.mNoRefit = v != 0;
                else if (k == "keep") d.mKeep = (U64)v;
                else if (k == "common_only") d.mCommonOnly = v != 0;
                else if (k == "alpha") d.mAlpha = v != 0;
                else if (k == "check") d.mCheck = v != 0;
                else if (k == "skipbad") d.mSkipBad = v != 0;
                else if (k == "only") d.mOnly = (S32)v;
                else if (k == "moving") d.mMoving = (S32)v;
                else if (k == "dynamic") d.mDynamic = (S32)v;
                else if (k == "kind") d.mKind = (S32)v;
                else if (k == "min_dist") d.mMinDist = (F32)v;
                else if (k == "max_dist") d.mMaxDist = (F32)v;
                else if (k == "dump") d.mDump = v != 0;
                else if (k == "singular") d.mSingular = v != 0;
                else if (k == "max_instances") d.mMaxInstances = (U32)v;
                else if (k == "build_max") d.mBuildMax = (U32)v;
                else if (k == "serial") d.mSerial = v != 0;
                else if (k == "pad") d.mPad = (U32)v;
                else if (k == "calllog") d.mCallLog = v != 0;
                else if (k == "dyncasters") d.mDynCasters = v != 0;
                else if (k == "compare") d.mCompare = v != 0;
                else if (k == "maxvtx") d.mMaxVtx = (U32)v;
                else if (k == "skipbuf") d.mSkipBuf = (S32)v;
            }
        }
        fclose(f);
    }
    if (d.mSingle != sDbg.mSingle || d.mNoRefit != sDbg.mNoRefit || d.mKeep != sDbg.mKeep || d.mCommonOnly != sDbg.mCommonOnly || d.mAlpha != sDbg.mAlpha || d.mCheck != sDbg.mCheck
        || d.mSkipBad != sDbg.mSkipBad || d.mOnly != sDbg.mOnly || d.mMoving != sDbg.mMoving || d.mDynamic != sDbg.mDynamic || d.mKind != sDbg.mKind || d.mMinDist != sDbg.mMinDist
        || d.mMaxDist != sDbg.mMaxDist || d.mSingular != sDbg.mSingular || d.mMaxInstances != sDbg.mMaxInstances || d.mBuildMax != sDbg.mBuildMax || d.mSerial != sDbg.mSerial || d.mPad != sDbg.mPad || d.mCallLog != sDbg.mCallLog || d.mDynCasters != sDbg.mDynCasters || d.mCompare != sDbg.mCompare || d.mMaxVtx != sDbg.mMaxVtx || d.mSkipBuf != sDbg.mSkipBuf)
    {
        LL_INFOS("VkRt") << "debug switches: single " << d.mSingle << ", norefit " << d.mNoRefit << ", keep " << d.mKeep << ", common_only " << d.mCommonOnly << ", alpha " << d.mAlpha
                         << ", check " << d.mCheck << ", skipbad " << d.mSkipBad << ", only " << d.mOnly << ", moving " << d.mMoving << ", dynamic " << d.mDynamic << ", kind " << d.mKind
                         << ", dist " << d.mMinDist << ".." << d.mMaxDist << ", dump " << d.mDump << ", dynamic casters " << d.mDynCasters << ", singular " << d.mSingular << ", max instances " << d.mMaxInstances << ", build max " << d.mBuildMax << ", serial " << d.mSerial << ", pad " << d.mPad << LL_ENDL;
    }
    sDbg = d;
}

bool sInit = false;
bool sFailed = false;
VkDescriptorPool sPool = VK_NULL_HANDLE;
Tlas sTlas[Context::FRAMES_IN_FLIGHT];
Buffer sScratch;
// The casters of the shadow passes. Far cascades are rendered only every few frames (FSShadowFarCascadeInterval) and their shadow map is reused in between, so the casters a
// frame names are not all there is. A pass that rendered this frame contributes exactly the casters it named (a moving object must not leave copies of its earlier positions);
// the casters of a pass that did not render stay, as its cached shadow map stays, until it renders again or for a few frames at most.
std::unordered_map<Key, Group, KeyHash> sGroups;
U64 sPassFrame[kPasses] = {};                           // per pass: the frame number + 1 of its latest note
std::unordered_map<Key, Blas, KeyHash> sBlas;
U64 sSceneCalls = 0;
U64 sBlasStorageBytes = 0;
U32 sLastInstances = 0;
U32 sLastBuilt = 0;
U32 sTableOffset = 0;
U32 sSlotCacheName[2048] = {};
U32 sSlotCacheSlot[2048] = {};
U64 sSlotCacheTag = 0;
Stats sStats;

// column-major 4x4 product
void mat_mul(const F32* a, const F32* b, F32* out)
{
    F32 r[16];
    for (int c = 0; c < 4; ++c)
    {
        for (int row = 0; row < 4; ++row)
        {
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] + a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

const F32 kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

bool device_ready()
{
    const Capabilities& caps = Context::instance().getCaps();
    return caps.mRt.mEnabled && caps.mRt.mUsable && vkCmdBuildAccelerationStructuresKHR && vkCreateAccelerationStructureKHR && vkGetAccelerationStructureDeviceAddressKHR;
}

void drop_blas(Blas& b)
{
    if (b.mAs != VK_NULL_HANDLE)
    {
        Context& ctx = Context::instance();
        VkDevice dev = ctx.getDevice();
        VkAccelerationStructureKHR as = b.mAs;
        ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });     // before its storage: deferred calls run in order
        sBlasStorageBytes -= std::min<U64>(sBlasStorageBytes, b.mStorage.mSize);
        ctx.arenaFree(b.mStorage);
    }
    b.mAs = VK_NULL_HANDLE;
    b.mAddress = 0;
    b.mStorage = ArenaRef();
    b.mBuilt.clear();
    b.mVbVersion = b.mIbVersion = ~0ull;
    b.mPrims = 0;
    b.mRefits = 0;
}

void tlas_sizes(U32 capacity, VkAccelerationStructureBuildSizesInfoKHR& sizes)
{
    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geo;
    sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(Context::instance().getDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
}

// a top-level structure for up to `capacity` instances, the instance and table memory of its frame slot, and the set pointing at them (made once at start-up: a set that
// recorded commands refer to must not be rewritten)
bool make_tlas(Tlas& t, U32 capacity)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    tlas_sizes(capacity, sizes);
    if (!ctx.createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Context::Memory::Device, t.mStorage)
        || !ctx.createBuffer(64ull * capacity * kCalls, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, Context::Memory::UploadDevice, t.mInstances)
        || !ctx.createBuffer(kTableBytes * kCalls, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, t.mTable))
    {
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        ctx.destroyBuffer(t.mTable);
        return false;
    }
    memset(t.mTable.mMapped, 0, (size_t)(kTableBytes * kCalls));
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = t.mStorage.mBuffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &t.mAs) != VK_SUCCESS)
    {
        t.mAs = VK_NULL_HANDLE;
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        ctx.destroyBuffer(t.mTable);
        return false;
    }
    t.mCapacity = capacity;
    VkWriteDescriptorSetAccelerationStructureKHR as_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &t.mAs;
    VkDescriptorBufferInfo table = { t.mTable.mBuffer, 0, kTableBytes };
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].pNext = &as_write;
    w[0].dstSet = t.mSet;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = t.mSet;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    w[1].pBufferInfo = &table;
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    return true;
}

bool ensure_scratch(VkDeviceSize bytes)
{
    Context& ctx = Context::instance();
    if (sScratch.valid() && sScratch.mSize >= bytes)
    {
        return true;
    }
    ctx.destroyBuffer(sScratch);
    return ctx.createBuffer(bytes * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Device, sScratch);
}

void full_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

struct Job
{
    Key mKey;
    Blas* mBlas = nullptr;
    bool mUpdate = false;
    std::vector<Range> mSet;
    std::vector<VkAccelerationStructureGeometryKHR> mGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> mRanges;
    std::vector<U32> mCounts;
    VkAccelerationStructureBuildGeometryInfoKHR mInfo = {};
    VkDeviceSize mScratchSize = 0, mScratchOffset = 0, mUpdateScratch = 0;
    VkAccelerationStructureKHR mNewAs = VK_NULL_HANDLE;
    ArenaRef mNewStorage;
    VkDeviceAddress mNewAddress = 0;
    U32 mPrims = 0;
    U64 mVbVersion = 0, mIbVersion = 0;
    bool mFresh = false;            // there was no structure before (the others replace one or refit it)
};

} // anonymous namespace

U32 shadowMode()
{
    static const U32 mode = []() {
        const U64 v = env_u64("LL_VK_RT_SHADOWS", 0);
        return v >= 1 && v <= 3 ? (U32)v : 0u;
    }();
    return mode;
}

bool active()
{
    return sInit && !sFailed;
}

bool init(VkDescriptorSetLayout set_layout)
{
    if (sInit)
    {
        return true;
    }
    if (!shadowMode() || set_layout == VK_NULL_HANDLE || !device_ready())
    {
        return false;
    }
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Context::frameCount() }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, Context::frameCount() } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = Context::frameCount();
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &sPool) != VK_SUCCESS)
    {
        sFailed = true;
        return false;
    }
    VkDescriptorSetLayout layouts[Context::FRAMES_IN_FLIGHT];
    for (auto& l : layouts) l = set_layout;
    VkDescriptorSet sets[Context::FRAMES_IN_FLIGHT] = {};
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = sPool;
    ai.descriptorSetCount = Context::frameCount();
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &ai, sets) != VK_SUCCESS)
    {
        vkDestroyDescriptorPool(dev, sPool, nullptr);
        sPool = VK_NULL_HANDLE;
        sFailed = true;
        return false;
    }
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    tlas_sizes(kInstanceCapacity, sizes);
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        sTlas[i].mSet = sets[i];
        if (!make_tlas(sTlas[i], kInstanceCapacity))
        {
            sFailed = true;
            return false;
        }
    }
    // an empty structure in every slot, so a draw before the first scene call has something valid to trace against
    Buffer scratch;
    if (!ctx.createBuffer(sizes.buildScratchSize + 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::Device, scratch))
    {
        sFailed = true;
        return false;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(ctx.getCaps().mRt.mScratchAlignment, 128);
    const VkDeviceAddress scratch_address = (scratch.mAddress + align - 1) / align * align;
    const bool ok = ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        for (U32 i = 0; i < Context::frameCount(); ++i)
        {
            VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geo.geometry.instances.data.deviceAddress = sTlas[i].mInstances.mAddress;
            VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geo;
            info.dstAccelerationStructure = sTlas[i].mAs;
            info.scratchData.deviceAddress = scratch_address;
            VkAccelerationStructureBuildRangeInfoKHR range = {};
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = { &range };
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, ranges);
            full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        }
    });
    ctx.destroyBuffer(scratch);
    if (!ok)
    {
        sFailed = true;
        return false;
    }
    sInit = true;
    LL_INFOS("VkRt") << "ray tracing scene ready: mode " << shadowMode() << ", " << Context::frameCount() << " frame slots, " << kCalls << " scene calls per frame, instance capacity "
                     << kInstanceCapacity << ", geometry table " << (kTableBytes >> 10) << " KB per call" << LL_ENDL;
    return true;
}

void shutdown()
{
    if (!sInit)
    {
        return;
    }
    Context& ctx = Context::instance();
    ctx.waitIdle();
    VkDevice dev = ctx.getDevice();
    for (auto& kv : sBlas)
    {
        drop_blas(kv.second);
    }
    sBlas.clear();
    for (Tlas& t : sTlas)
    {
        if (t.mAs != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(dev, t.mAs, nullptr);
        }
        t = Tlas();
    }
    if (sPool)
    {
        vkDestroyDescriptorPool(dev, sPool, nullptr);
        sPool = VK_NULL_HANDLE;
    }
    sInit = false;
}

void noteCaster(const Caster& c)
{
    if (!sInit || sFailed || c.mCount < 3)
    {
        return;
    }
    ++sStats.mCasters;
    Key key;
    key.mVb = c.mVb;
    key.mIb = c.mIb;
    key.mMatrixId = c.mMatrixId;
    key.mKind = c.mKind;
    auto it = sGroups.find(key);
    if (it == sGroups.end())
    {
        it = sGroups.emplace(key, Group()).first;
        it->second.mKey = key;
        it->second.mBlas = &sBlas[key];
        ++sStats.mNewGroups;
    }
    Group& g = it->second;
    const U32 pass = std::min<U32>(c.mPass, kPasses - 1);
    const U64 frame_tag = Context::instance().getFrameNumber() + 1;
    g.mPassSeen[pass] = frame_tag;
    sPassFrame[pass] = frame_tag;
    if (g.mNotedTag != frame_tag)
    {
        // first note of this frame: the matrix of the object now, and where its buffers live now
        const F32* now_model = c.mModel ? c.mModel : kIdentity;
        F32 diff = 0.f;
        for (int k = 0; k < 16; ++k)
        {
            diff = std::max(diff, fabsf(now_model[k] - g.mModel[k]));
        }
        memcpy(g.mPrevModel, g.mModel, sizeof(g.mModel));
        g.mMoving = diff > 1e-5f;
        memcpy(g.mModel, now_model, sizeof(g.mModel));
        g.mVi = c.mVi;
        g.mIi = c.mIi;
        g.mNotedTag = frame_tag;
        g.mBlas->mLastSeen = sSceneCalls;
    }
    std::vector<Range>& ranges = g.mRanges;
    const Range r = { c.mFirst, c.mCount };
    for (size_t k = 0; k < ranges.size(); ++k)
    {
        if (ranges[k] == r)
        {
            if (c.mAlpha && memcmp(&g.mAlpha[k], c.mAlpha, sizeof(AlphaInfo)) != 0)
            {
                g.mAlpha[k] = *c.mAlpha;                // the textures may have changed
                g.mBlas->mAlpha[r.id()] = *c.mAlpha;
            }
            return;
        }
    }
    if (ranges.size() < 512)
    {
        ranges.push_back(r);
        g.mAlpha.push_back(c.mAlpha ? *c.mAlpha : AlphaInfo());
        if (c.mKind == 1)
        {
            g.mBlas->mAlpha[r.id()] = g.mAlpha.back();     // the alpha test data of each range of a masked structure, as last drawn
        }
    }
}

VkDescriptorSet descriptorSet(U32 frame_slot)
{
    return frame_slot < Context::FRAMES_IN_FLIGHT ? sTlas[frame_slot].mSet : VK_NULL_HANDLE;
}

U32 tableOffset()
{
    return sTableOffset;
}

void buildScene(const F32* view, TextureSlotFn slot_of)
{
    if (!sInit || sFailed)
    {
        return;
    }
    Context& ctx = Context::instance();
    if (!ctx.inFrame())
    {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkDevice dev = ctx.getDevice();
    const Capabilities& caps = ctx.getCaps();
    const VkDeviceSize align = std::max<VkDeviceSize>(caps.mRt.mScratchAlignment, 128);
    read_debug_switches();
    if (sDbg.mSingle)
    {
        const Tlas& t0 = sTlas[ctx.getFrameSlot()];
        if (t0.mCallFrame == ctx.getFrameNumber() && t0.mCalls > 0)
        {
            return;                             // later scene calls of a frame keep the first one's scene (debug switch)
        }
    }
    static const U64 max_builds_default = env_u64("LL_VK_RT_BUILD_MAX", 256);
    const U64 max_builds = sDbg.mBuildMax ? sDbg.mBuildMax : max_builds_default;
    static const U64 max_tris = env_u64("LL_VK_RT_BUILD_TRIS", 800000);
    const bool alpha_on = sDbg.mAlpha;
    const bool check_on = sDbg.mCheck;
    const bool skip_bad = sDbg.mSkipBad;
    static const U64 rebuild_after_refits = 64;
    ++sSceneCalls;
    const U64 keep = sDbg.mKeep;

    auto lap = [&](int phase, std::chrono::steady_clock::time_point& last) {
        const auto now = std::chrono::steady_clock::now();
        sStats.mPhaseMs[phase] += std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
    };
    auto t_lap = t0;

    // forget structures that were not named for a while, and casters nothing has named for a few frames
    if (sSceneCalls % 120 == 0)
    {
        for (auto it = sBlas.begin(); it != sBlas.end();)
        {
            if (sSceneCalls - it->second.mLastSeen > 900)
            {
                drop_blas(it->second);
                it = sBlas.erase(it);
                ++sStats.mEvicted;
            }
            else
            {
                ++it;
            }
        }
    }

    static U64 expired_tag = 0;
    if (expired_tag != ctx.getFrameNumber() + 1)
    {
        expired_tag = ctx.getFrameNumber() + 1;
        const U64 tag = ctx.getFrameNumber() + 1;
        for (auto it = sGroups.begin(); it != sGroups.end();)
        {
            bool alive = false;
            for (U32 p = 0; p < kPasses; ++p)
            {
                U64& seen = it->second.mPassSeen[p];
                if (!seen)
                {
                    continue;
                }
                if (sPassFrame[p] == tag)
                {
                    // the pass rendered this frame: its casters are the ones it named
                    if (seen == tag)
                    {
                        alive = true;
                    }
                    else
                    {
                        seen = 0;
                    }
                }
                else if (tag <= seen + keep)
                {
                    alive = true;           // cached (or not rendered at all): the shadow map it fed is still in use
                }
            }
            it = alive ? std::next(it) : sGroups.erase(it);
        }
    }

    lap(0, t_lap);
    // ---- 1. what is stale -----------------------------------------------------------------------------------
    std::vector<Job> jobs;
    jobs.reserve((size_t)max_builds);
    U64 tris_budget = max_tris;
    LLVkGL::BufferInfo vi, ii;
    VkDeviceSize scratch_total = 0;
    // Which groups may need work: never built, or named this frame with changed buffers or new ranges. Everything else is left alone (the scan does no lookups for the many
    // groups that did not change). Nearest first (the distance of the object's origin in eye space): while a scene loads the budget goes to what is around the camera.
    const U64 scan_tag = ctx.getFrameNumber() + 1;
    std::vector<std::pair<float, Group*>> order;
    for (auto& kv : sGroups)
    {
        Group& g = kv.second;
        // a region's own matrix is marked by the viewer (top bit of the id, pipeline.cpp rt_matrix_id); anything else is an object that moves or is attached
        g.mSkip = (g.mKey.mKind == 1 && !alpha_on) || (!sDbg.mDynCasters && (g.mKey.mMatrixId & 0x80000000u) == 0 && g.mKey.mMatrixId != 0);
        if (g.mSkip)
        {
            continue;
        }
        const Blas& b = *g.mBlas;
        if (b.mAs != VK_NULL_HANDLE)
        {
            if (g.mNotedTag != scan_tag)
            {
                continue;                       // built, and not named this frame
            }
            bool changed = b.mVbEpoch != g.mVi.mEpoch || b.mIbEpoch != g.mIi.mEpoch || b.mVbVersion != g.mVi.mVersion || b.mIbVersion != g.mIi.mVersion || b.mRefits >= rebuild_after_refits;
            for (size_t k = 0; !changed && k < g.mRanges.size(); ++k)
            {
                changed = std::find(b.mBuilt.begin(), b.mBuilt.end(), g.mRanges[k]) == b.mBuilt.end();
            }
            if (!changed)
            {
                continue;
            }
        }
        F32 m[16];
        mat_mul(view, g.mModel, m);
        order.emplace_back(m[12] * m[12] + m[13] * m[13] + m[14] * m[14], &g);
    }
    lap(1, t_lap);
    std::sort(order.begin(), order.end(), [](const std::pair<float, Group*>& a, const std::pair<float, Group*>& b) { return a.first < b.first; });
    for (const auto& entry : order)
    {
        Group& g = *entry.second;
        if ((g.mKey.mKind == 1 && !alpha_on) || g.mSkip)
        {
            continue;
        }
        if (g.mNotedTag == scan_tag)
        {
            vi = g.mVi;
            ii = g.mIi;
        }
        else if (!LLVkGL::getBufferInfo(g.mKey.mVb, vi) || !LLVkGL::getBufferInfo(g.mKey.mIb, ii))
        {
            ++sStats.mNoData;
            continue;
        }
        if (!vi.mAddress || !ii.mAddress || vi.mSize < 16 || ii.mSize < 6)
        {
            ++sStats.mNoData;
            continue;
        }
        Blas& b = *g.mBlas;
        b.mLastSeen = sSceneCalls;
        if (b.mAs != VK_NULL_HANDLE && (b.mVbEpoch != vi.mEpoch || b.mIbEpoch != ii.mEpoch))
        {
            drop_blas(b);                       // the names hold something else now
            ++sStats.mWhyEpoch;
        }
        if (b.mAs == VK_NULL_HANDLE)
        {
            b.mVbEpoch = vi.mEpoch;
            b.mIbEpoch = ii.mEpoch;
        }
        // the ranges the structure has to cover: what it covers already plus what was drawn now
        std::vector<Range> want = b.mBuilt;
        bool grew = b.mAs == VK_NULL_HANDLE;
        if (!b.mBad.empty() && (b.mBadVbVersion != vi.mVersion || b.mBadIbVersion != ii.mVersion))
        {
            b.mBad.clear();                     // the data was written: look at the ranges again
        }
        for (size_t k = 0; k < g.mRanges.size(); ++k)
        {
            const Range& r = g.mRanges[k];
            if (((U64)r.mFirst + r.mCount) * 2 > ii.mSize)
            {
                continue;                       // outside the data (a stale range)
            }
            if (!b.mBad.empty() && std::find(b.mBad.begin(), b.mBad.end(), r.id()) != b.mBad.end())
            {
                continue;                       // failed the data check for these buffer versions
            }
            if (std::find(want.begin(), want.end(), r) == want.end())
            {
                want.push_back(r);
                grew = true;
            }
        }
        if (want.empty())
        {
            continue;
        }
        const bool fresh = b.mAs == VK_NULL_HANDLE;
        const bool ib_changed = b.mAs != VK_NULL_HANDLE && b.mIbVersion != ii.mVersion;
        const bool vb_changed = b.mAs != VK_NULL_HANDLE && b.mVbVersion != vi.mVersion;
        if (!grew && !ib_changed && !vb_changed && b.mRefits < rebuild_after_refits)
        {
            continue;                           // up to date
        }
        if (jobs.size() >= max_builds || tris_budget == 0)
        {
            ++sStats.mDeferred;
            continue;
        }
        if (fresh)
        {
            ++sStats.mWhyNew;
        }
        else if (grew)
        {
            ++sStats.mWhyGrew;
        }
        else if (ib_changed)
        {
            ++sStats.mWhyIb;
        }
        else if (!vb_changed)
        {
            ++sStats.mWhyRefitLimit;
        }
        Job job;
        job.mFresh = fresh;
        job.mKey = g.mKey;
        job.mBlas = &b;
        LLVkGL::markBufferUsed(g.mKey.mVb);         // the build reads them when the GPU gets here: a CPU rewrite in place before that would tear the data
        LLVkGL::markBufferUsed(g.mKey.mIb);
        job.mUpdate = !grew && !ib_changed && b.mRefits < rebuild_after_refits && b.mAs != VK_NULL_HANDLE && !sDbg.mNoRefit;
        job.mVbVersion = vi.mVersion;
        job.mIbVersion = ii.mVersion;
        std::sort(want.begin(), want.end());
        const U32 max_vertex = sDbg.mMaxVtx ? sDbg.mMaxVtx : (U32)(vi.mSize / 16 - 1);
        U64 prims = 0;
        for (const Range& r : want)
        {
            VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            // opaque casters commit their hit at once; masked ones hand every candidate triangle to the shader's alpha test
            geo.flags = g.mKey.mKind == 1 ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
            VkAccelerationStructureGeometryTrianglesDataKHR& t = geo.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = vi.mAddress;
            t.vertexStride = 16;
            t.maxVertex = max_vertex;
            t.indexType = VK_INDEX_TYPE_UINT16;
            t.indexData.deviceAddress = ii.mAddress + 2ull * r.mFirst;
            job.mGeometries.push_back(geo);
            VkAccelerationStructureBuildRangeInfoKHR range = {};
            range.primitiveCount = r.mCount / 3;
            job.mRanges.push_back(range);
            job.mCounts.push_back(range.primitiveCount);
            prims += range.primitiveCount;
        }
        job.mPrims = (U32)prims;
        // LL_VK_RT_CHECK (default on): the vertices the structure will be built over, when the CPU can see them (host-written buffers), must be finite and not absurd, and the
        // indices must name vertices of the buffer. A range that fails is left out of the structure (LL_VK_RT_SKIPBAD=0 keeps it).
        if (check_on && vi.mCpu && ii.mCpu)
        {
            ++sStats.mChecked;
            const F32* pos = (const F32*)vi.mCpu;
            const U16* idx = (const U16*)ii.mCpu;
            const U64 verts = vi.mSize / 16;
            std::vector<Range> kept;
            kept.reserve(want.size());
            for (const Range& r : want)
            {
                U32 out_of_range = 0, bad_coords = 0, first_bad_index = 0, max_index = 0, nans = 0, infs = 0, huge = 0;
                F32 sample[3] = {};
                F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                for (U32 k = 0; k < r.mCount; ++k)
                {
                    const U32 v = idx[r.mFirst + k];
                    max_index = std::max(max_index, v);
                    if (v >= verts)
                    {
                        if (!out_of_range)
                        {
                            first_bad_index = v;
                        }
                        ++out_of_range;
                        continue;
                    }
                    for (int a = 0; a < 3; ++a)
                    {
                        const F32 c = pos[4 * (size_t)v + a];
                        if (!(c > -1e5f && c < 1e5f))
                        {
                            if (bad_coords < 3)
                            {
                                sample[bad_coords] = c;
                            }
                            ++bad_coords;
                            if (c != c) ++nans;
                            else if (std::isinf(c)) ++infs;
                            else ++huge;
                        }
                        else
                        {
                            lo[a] = std::min(lo[a], c);
                            hi[a] = std::max(hi[a], c);
                        }
                    }
                }
                if (out_of_range || bad_coords)
                {
                    ++sStats.mBadVertices;
                    static U32 logged = 0;
                    if (logged++ < 40)
                    {
                        LL_WARNS("VkRt") << "bad range in a structure to build: vb " << g.mKey.mVb << " (" << vi.mSize << " bytes, " << verts << " vertex slots) ib " << g.mKey.mIb << " ("
                                         << ii.mSize << " bytes) kind " << g.mKey.mKind << ", range first " << r.mFirst << " count " << r.mCount << ": " << out_of_range << " indices out of range (first "
                                         << first_bad_index << ", largest " << max_index << "), " << bad_coords << " bad coordinates (" << nans << " NaN, " << infs << " infinite, " << huge << " huge; e.g. " << sample[0] << ", " << sample[1] << ", " << sample[2] << "), bounds of the good ones (" << lo[0] << "," << lo[1] << "," << lo[2]
                                         << ")-(" << hi[0] << "," << hi[1] << "," << hi[2] << ")" << LL_ENDL;
                    }
                    if (skip_bad)
                    {
                        b.mBad.push_back(r.id());
                        b.mBadVbVersion = vi.mVersion;
                        b.mBadIbVersion = ii.mVersion;
                        continue;
                    }
                }
                kept.push_back(r);
            }
            if (kept.size() != want.size())
            {
                if (kept.empty())
                {
                    continue;
                }
                want = kept;
                job.mUpdate = false;            // the geometry list changed: a refit cannot do that
                job.mGeometries.clear();
                job.mRanges.clear();
                job.mCounts.clear();
                prims = 0;
                for (const Range& r : want)
                {
                    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
                    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                    geo.flags = g.mKey.mKind == 1 ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
                    VkAccelerationStructureGeometryTrianglesDataKHR& t = geo.geometry.triangles;
                    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                    t.vertexData.deviceAddress = vi.mAddress;
                    t.vertexStride = 16;
                    t.maxVertex = max_vertex;
                    t.indexType = VK_INDEX_TYPE_UINT16;
                    t.indexData.deviceAddress = ii.mAddress + 2ull * r.mFirst;
                    job.mGeometries.push_back(geo);
                    VkAccelerationStructureBuildRangeInfoKHR range = {};
                    range.primitiveCount = r.mCount / 3;
                    job.mRanges.push_back(range);
                    job.mCounts.push_back(range.primitiveCount);
                    prims += range.primitiveCount;
                }
                job.mPrims = (U32)prims;
            }
        }
        job.mSet = std::move(want);
        job.mInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        job.mInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        job.mInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        job.mInfo.mode = job.mUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        job.mInfo.geometryCount = (U32)job.mGeometries.size();
        job.mInfo.pGeometries = job.mGeometries.data();
        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &job.mInfo, job.mCounts.data(), &sizes);
        job.mUpdateScratch = sizes.updateScratchSize;
        if (job.mUpdate)
        {
            job.mInfo.srcAccelerationStructure = b.mAs;
            job.mInfo.dstAccelerationStructure = b.mAs;
            job.mScratchSize = sizes.updateScratchSize;
        }
        else
        {
            if (!ctx.arenaAlloc(sizes.accelerationStructureSize, 256, ArenaKind::Accel, job.mNewStorage))
            {
                ++sStats.mFailed;
                continue;
            }
            VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
            ci.buffer = job.mNewStorage.mBuffer;
            ci.offset = job.mNewStorage.mOffset;
            ci.size = sizes.accelerationStructureSize;
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &job.mNewAs) != VK_SUCCESS)
            {
                ctx.arenaFree(job.mNewStorage);
                ++sStats.mFailed;
                continue;
            }
            VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
            ai.accelerationStructure = job.mNewAs;
            job.mNewAddress = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
            job.mInfo.dstAccelerationStructure = job.mNewAs;
            job.mScratchSize = sizes.buildScratchSize;
            sBlasStorageBytes += job.mNewStorage.mSize;
        }
        job.mScratchOffset = (scratch_total + align - 1) / align * align;
        scratch_total = job.mScratchOffset + job.mScratchSize + sDbg.mPad;
        tris_budget = prims >= tris_budget ? 0 : tris_budget - prims;
        jobs.push_back(std::move(job));
        b.mJob = &jobs.back();
        b.mJobCall = sSceneCalls;
    }

    lap(2, t_lap);
    // ---- 2. instances and the geometry table of the masked structures --------------------------------------
    // a structure that is being (re)built now is part of this frame's scene: its new address is used
    Tlas& tlas = sTlas[ctx.getFrameSlot()];
    const U64 frame_now = ctx.getFrameNumber();
    if (tlas.mCallFrame != frame_now)
    {
        tlas.mCallFrame = frame_now;
        tlas.mCalls = 0;
    }

    U32 call = tlas.mCalls++;
    if (call >= kCalls)
    {
        call = kCalls - 1;                      // more scene calls in one frame than regions: the last region is shared (earlier traces of the frame may see its later content)
        ++sStats.mExtraCalls;
    }
    GpuGeom* table = (GpuGeom*)((U8*)tlas.mTable.mMapped + (size_t)call * kTableBytes);
    U32 records = 0;
    sTableOffset = (U32)(call * kTableBytes);

    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(sGroups.size());
    for (auto& kv : sGroups)
    {
        const Group& g = kv.second;
        if (g.mSkip || (g.mKey.mKind == 1 && !alpha_on))
        {
            continue;
        }
        const Blas& b = *g.mBlas;
        VkDeviceAddress address = b.mAddress;
        const Job* job = b.mJobCall == sSceneCalls ? b.mJob : nullptr;
        if (job && !job->mUpdate)
        {
            address = job->mNewAddress;
        }
        if (!address)
        {
            continue;
        }
        if (instances.size() >= kInstanceCapacity || (sDbg.mMaxInstances && instances.size() >= sDbg.mMaxInstances))
        {
            ++sStats.mOverCap;
            break;
        }
        U32 custom_index = 0;
        if (g.mKey.mKind == 1)
        {
            // one record per geometry, in the order of the structure's geometry list
            const std::vector<Range>& set = job ? job->mSet : b.mBuilt;
            if (g.mNotedTag == scan_tag)
            {
                vi = g.mVi;
                ii = g.mIi;
            }
            else if (!LLVkGL::getBufferInfo(g.mKey.mVb, vi) || !LLVkGL::getBufferInfo(g.mKey.mIb, ii))
            {
                ++sStats.mNoData;
                continue;
            }
            if (records + set.size() > kMaxRecords)
            {
                ++sStats.mTableFull;
                continue;
            }
            custom_index = records;
            LLVkGL::markBufferUsed(g.mKey.mVb);     // the alpha test reads positions, texcoords, colours and indices while the lighting runs
            LLVkGL::markBufferUsed(g.mKey.mIb);
            for (const Range& r : set)
            {
                GpuGeom& rec = table[records++];
                memset(&rec, 0, sizeof(rec));
                auto ait = b.mAlpha.find(r.id());
                const AlphaInfo none;
                const AlphaInfo& a = ait != b.mAlpha.end() ? ait->second : none;
                const U64 pa = vi.mAddress, ia = ii.mAddress;
                rec.mPositions[0] = (U32)pa;
                rec.mPositions[1] = (U32)(pa >> 32);
                rec.mIndices[0] = (U32)ia;
                rec.mIndices[1] = (U32)(ia >> 32);
                if (a.mFlags & 8u)
                {
                    const U64 t = pa + a.mTexcoordOffset;
                    rec.mTexcoords[0] = (U32)t;
                    rec.mTexcoords[1] = (U32)(t >> 32);
                }
                if (a.mFlags & 16u)
                {
                    const U64 c = pa + a.mColorOffset;
                    rec.mColors[0] = (U32)c;
                    rec.mColors[1] = (U32)(c >> 32);
                }
                U32 slot[4] = {};
                for (U32 k = 0; k < 4; ++k)
                {
                    if (a.mTextures[k] && slot_of)
                    {
                        // many geometries share textures: a texture's slot is asked for once per frame
                        const U32 name = a.mTextures[k];
                        const U32 h = (name * 2654435761u) >> 21;
                        if (sSlotCacheTag != frame_now + 1 || sSlotCacheName[h] != name)
                        {
                            if (sSlotCacheTag != frame_now + 1)
                            {
                                memset(sSlotCacheName, 0, sizeof(sSlotCacheName));
                                sSlotCacheTag = frame_now + 1;
                            }
                            sSlotCacheName[h] = name;
                            sSlotCacheSlot[h] = slot_of(name) & 0xFFFFu;
                        }
                        slot[k] = sSlotCacheSlot[h];
                    }
                }
                rec.mSlots[0] = slot[0] | (slot[1] << 16);
                rec.mSlots[1] = slot[2] | (slot[3] << 16);
                rec.mFlags = a.mFlags;
                rec.mCutoff = a.mCutoff;
                rec.mFirst = r.mFirst;
            }
            ++sStats.mAlphaStructs;
        }
        F32 m[16];
        mat_mul(view, g.mModel, m);
        {
            bool bad = false;
            for (int k = 0; k < 16; ++k)
            {
                if (!(m[k] > -1e6f && m[k] < 1e6f))
                {
                    bad = true;
                }
            }
            F32 scale = 0.f;
            for (int c = 0; c < 3; ++c)
            {
                scale = std::max(scale, sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]));
            }
            if (bad)
            {
                ++sStats.mBadMatrices;
                static U32 logged = 0;
                if (logged++ < 40)
                {
                    LL_WARNS("VkRt") << "bad instance matrix: vb " << g.mKey.mVb << " ib " << g.mKey.mIb << " kind " << g.mKey.mKind << " matrix id " << g.mKey.mMatrixId << " model "
                                     << g.mModel[0] << "," << g.mModel[5] << "," << g.mModel[10] << " translation " << g.mModel[12] << "," << g.mModel[13] << "," << g.mModel[14] << LL_ENDL;
                }
                continue;
            }
            // the determinant of the 3x3 part: zero scale (how objects are hidden) has no inverse
            const F32 det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
            if (sDbg.mSingular && !(fabsf(det) > 1e-10f))
            {
                ++sStats.mSingular;
                static U32 logged_singular = 0;
                if (logged_singular++ < 30)
                {
                    LL_WARNS("VkRt") << "singular instance matrix left out: vb " << g.mKey.mVb << " ib " << g.mKey.mIb << " kind " << g.mKey.mKind << " matrix id " << g.mKey.mMatrixId << " determinant " << det
                                     << " model diag " << g.mModel[0] << "," << g.mModel[5] << "," << g.mModel[10] << LL_ENDL;
                }
                continue;
            }
            if (scale > sStats.mMaxScale)
            {
                sStats.mMaxScale = scale;
                sStats.mMaxScaleKey = g.mKey;
            }
        }
        VkAccelerationStructureInstanceKHR inst = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                inst.transform.matrix[row][col] = m[col * 4 + row];
            }
        }
        inst.instanceCustomIndex = custom_index;
        inst.mask = 1u << std::min<U32>(g.mKey.mKind, 7u);
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = address;
        instances.push_back(inst);
    }

    lap(3, t_lap);
    // ---- 3. record: builds, then the top-level structure -----------------------------------------------------
    VkAccelerationStructureBuildSizesInfoKHR tlas_size;
    tlas_sizes(tlas.mCapacity, tlas_size);
    const VkDeviceSize scratch_needed = std::max(scratch_total, tlas_size.buildScratchSize) + 2 * align;
    auto undo_jobs = [&]() {
        for (Job& j : jobs)
        {
            if (j.mNewAs != VK_NULL_HANDLE)
            {
                VkAccelerationStructureKHR as = j.mNewAs;
                ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });
                sBlasStorageBytes -= std::min<U64>(sBlasStorageBytes, j.mNewStorage.mSize);
                ctx.arenaFree(j.mNewStorage);
            }
        }
    };
    if (!ensure_scratch(scratch_needed))
    {
        undo_jobs();
        ++sStats.mFailed;
        return;
    }
    const VkDeviceSize instance_offset = (VkDeviceSize)call * 64ull * tlas.mCapacity;
    if (!instances.empty())
    {
        memcpy((U8*)tlas.mInstances.mMapped + instance_offset, instances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instances.size());
    }
    const VkDeviceAddress scratch_base = (sScratch.mAddress + align - 1) / align * align;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
    for (Job& j : jobs)
    {
        j.mInfo.scratchData.deviceAddress = scratch_base + j.mScratchOffset;
        infos.push_back(j.mInfo);
        ranges.push_back(j.mRanges.data());
    }
    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = tlas.mInstances.mAddress + instance_offset;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    tinfo.dstAccelerationStructure = tlas.mAs;
    tinfo.scratchData.deviceAddress = scratch_base;
    VkAccelerationStructureBuildRangeInfoKHR trange = {};
    trange.primitiveCount = (U32)instances.size();
    const VkAccelerationStructureBuildRangeInfoKHR* tranges[1] = { &trange };

    const bool recorded = Renderer::instance().recordExternal({}, [&](VkCommandBuffer cmd) {
        ctx.gpuStamp(Context::stampTag("rt scene"));
        // earlier work (last frame's traces, the shadow passes that wrote the vertex data) is done before anything is built over it
        full_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        if (!infos.empty() && sDbg.mSerial)
        {
            for (size_t k = 0; k < infos.size(); ++k)
            {
                vkCmdBuildAccelerationStructuresKHR(cmd, 1, &infos[k], &ranges[k]);
                full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            }
        }
        else if (!infos.empty())
        {
            vkCmdBuildAccelerationStructuresKHR(cmd, (U32)infos.size(), infos.data(), ranges.data());
            full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        }
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tinfo, tranges);
        full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    if (!recorded)
    {
        // no frame is being recorded: undo the new structures, the groups are named again when the passes run
        undo_jobs();
        return;
    }
    for (Job& j : jobs)
    {
        Blas& b = *j.mBlas;
        if (j.mUpdate)
        {
            ++b.mRefits;
            ++sStats.mRefits;
            sStats.mPrimsRefit += j.mPrims;
        }
        else
        {
            std::unordered_map<U64, AlphaInfo> keep = std::move(b.mAlpha);
            drop_blas(b);                       // the structure that is replaced (a new one was made)
            b.mAlpha = std::move(keep);
            b.mAs = j.mNewAs;
            b.mAddress = j.mNewAddress;
            b.mStorage = j.mNewStorage;
            b.mRefits = 0;
            ++sStats.mBuilds;
            sStats.mPrimsBuilt += j.mPrims;
        }
        b.mBuilt = j.mSet;
        b.mVbVersion = j.mVbVersion;
        b.mIbVersion = j.mIbVersion;
        b.mPrims = j.mPrims;
        b.mUpdateScratch = j.mUpdateScratch;
    }
    lap(4, t_lap);
    ++sStats.mScenes;
    sStats.mInstances += instances.size();
    sLastInstances = (U32)instances.size();
    sLastBuilt = (U32)jobs.size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sStats.mCpuMs += ms;
    sStats.mCpuMsMax = std::max(sStats.mCpuMsMax, ms);
    if (sDbg.mCallLog)
    {
        static FILE* log = fopen("<work>/rt-calls.log", "ab");
        if (log)
        {
            U32 fresh = 0, replaced = 0, refits = 0, alpha_inst = 0, kind0_new = 0;
            U64 prims = 0;
            for (const Job& j : jobs)
            {
                if (j.mUpdate) ++refits;
                else if (j.mFresh) ++fresh;
                else ++replaced;
                prims += j.mPrims;
                if (j.mKey.mKind == 0 && !j.mUpdate && j.mFresh) ++kind0_new;
            }
            alpha_inst = records;
            const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            fprintf(log, "%lld frame %llu call %u instances %zu jobs %zu fresh %u replaced %u refits %u prims %llu scratch %llu records %u kind0_new %u cpu_ms %.2f view %.3f %.3f %.3f %.4f %.4f\n", wall, (unsigned long long)frame_now, call,
                    instances.size(), jobs.size(), fresh, replaced, refits, (unsigned long long)prims, (unsigned long long)scratch_total, alpha_inst, kind0_new, ms,
                    view[12], view[13], view[14], view[0], view[2]);
            fflush(log);
        }
    }
}

void resetStats()
{
    sStats = Stats();
}

std::string report(double frames)
{
    if (!sInit)
    {
        return std::string();
    }
    U32 live = 0;
    U64 prims = 0;
    for (const auto& kv : sBlas)
    {
        if (kv.second.mAs != VK_NULL_HANDLE)
        {
            ++live;
            prims += kv.second.mPrims;
        }
    }
    const Stats& s = sStats;
    const double scenes = std::max<double>(1.0, (double)s.mScenes);
    const double f = std::max(1.0, frames);
    char buf[1000];
    snprintf(buf, sizeof(buf),
             "rt scene (mode %u): %u structures (%.0f MB, %.2f M triangles), instances %.0f/scene (last %u), casters named %.0f/frame, builds %.2f/frame (%.0f tri), refits %.2f/frame (%.0f tri), "
             "deferred %.2f, no data %.2f, masked structures %.0f/frame, table full %.2f, over instance cap %.2f, failed %.3f, evicted %.3f, scene calls %.2f/frame (over %u: %.2f), "
             "CPU %.3f ms/call (worst %.2f ms; expire %.2f, order %.2f, scan+sort %.2f, instances %.2f, record %.2f), scratch %.1f MB; builds because: new %.2f, names recycled %.2f, ranges grew %.2f, index data rewritten %.2f, refit limit %.2f; casters new %.2f/frame, groups held %zu; checks: structures checked %.2f/frame, bad vertex data %.3f, bad matrices %.3f, singular matrices %.3f, copy vs video memory compared %.2f mismatched %.2f, largest instance scale %.2f (vb %u ib %u matrix id %u kind %u)",
             shadowMode(), live, (double)sBlasStorageBytes / 1048576.0, (double)prims / 1e6, (double)s.mInstances / scenes, sLastInstances, (double)s.mCasters / f, (double)s.mBuilds / f,
             (double)s.mPrimsBuilt / f, (double)s.mRefits / f, (double)s.mPrimsRefit / f, (double)s.mDeferred / f, (double)s.mNoData / f, (double)s.mAlphaStructs / f, (double)s.mTableFull / f,
             (double)s.mOverCap / f, (double)s.mFailed / f, (double)s.mEvicted / f, (double)s.mScenes / f, kCalls, (double)s.mExtraCalls / f, s.mCpuMs / scenes, s.mCpuMsMax, s.mPhaseMs[0] / scenes, s.mPhaseMs[1] / scenes, s.mPhaseMs[2] / scenes, s.mPhaseMs[3] / scenes, s.mPhaseMs[4] / scenes,
             (double)sScratch.mSize / 1048576.0, (double)s.mWhyNew / f, (double)s.mWhyEpoch / f, (double)s.mWhyGrew / f, (double)s.mWhyIb / f, (double)s.mWhyRefitLimit / f,
             (double)s.mNewGroups / f, sGroups.size(), (double)s.mChecked / f, (double)s.mBadVertices / f, (double)s.mBadMatrices / f, (double)s.mSingular / f, (double)s.mCompared / f, (double)s.mMismatched / f, s.mMaxScale, s.mMaxScaleKey.mVb, s.mMaxScaleKey.mIb,
             s.mMaxScaleKey.mMatrixId, s.mMaxScaleKey.mKind);
    return buf;
}

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
