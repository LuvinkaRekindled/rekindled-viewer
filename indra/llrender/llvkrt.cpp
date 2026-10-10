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

// what the shadow passes of a frame drew from one buffer pair with one matrix
struct Group
{
    Key mKey;
    F32 mModel[16] = {};
    std::vector<Range> mRanges;
    std::vector<AlphaInfo> mAlpha;      // kind 1: parallel to mRanges
};

struct Blas
{
    U64 mVbEpoch = 0, mIbEpoch = 0;
    U64 mVbVersion = ~0ull, mIbVersion = ~0ull;     // contents at the last build or refit
    std::vector<Range> mBuilt;                      // sorted: what the structure covers (the geometry index is the position in this list)
    std::unordered_map<U64, AlphaInfo> mAlpha;      // kind 1: the alpha test data of each range, as last drawn
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    VkDeviceAddress mAddress = 0;
    ArenaRef mStorage;
    VkDeviceSize mUpdateScratch = 0;
    U32 mPrims = 0;
    U32 mRefits = 0;                                // since the last build (a refit keeps the tree: after a number of them, build again)
    U64 mLastSeen = 0;                              // scene call number
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
    U64 mOverCap = 0, mExtraCalls = 0;
    double mCpuMs = 0, mCpuMsMax = 0;
};

bool sInit = false;
bool sFailed = false;
VkDescriptorPool sPool = VK_NULL_HANDLE;
Tlas sTlas[Context::FRAMES_IN_FLIGHT];
Buffer sScratch;
std::unordered_map<Key, Group, KeyHash> sGroups;        // casters of the latest frame that named any
bool sGroupsConsumed = true;                            // a scene call has been made since the last note: the next note starts a new list
std::unordered_map<Key, Blas, KeyHash> sBlas;
U64 sSceneCalls = 0;
U64 sBlasStorageBytes = 0;
U32 sLastInstances = 0;
U32 sLastBuilt = 0;
U32 sTableOffset = 0;
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
    if (sGroupsConsumed)
    {
        sGroups.clear();
        sGroupsConsumed = false;
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
        memcpy(it->second.mModel, c.mModel ? c.mModel : kIdentity, sizeof(it->second.mModel));
    }
    std::vector<Range>& ranges = it->second.mRanges;
    const Range r = { c.mFirst, c.mCount };
    for (const Range& e : ranges)
    {
        if (e == r)
        {
            return;
        }
    }
    if (ranges.size() < 512)
    {
        ranges.push_back(r);
        it->second.mAlpha.push_back(c.mAlpha ? *c.mAlpha : AlphaInfo());
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
    static const U64 max_builds = env_u64("LL_VK_RT_BUILD_MAX", 256);
    static const U64 max_tris = env_u64("LL_VK_RT_BUILD_TRIS", 800000);
    static const bool alpha_on = env_u64("LL_VK_RT_ALPHA", 1) != 0;
    static const U64 rebuild_after_refits = 64;
    ++sSceneCalls;
    sGroupsConsumed = true;

    // forget structures that were not named for a while
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

    // ---- 1. what is stale -----------------------------------------------------------------------------------
    std::vector<Job> jobs;
    jobs.reserve((size_t)max_builds);
    U64 tris_budget = max_tris;
    LLVkGL::BufferInfo vi, ii;
    VkDeviceSize scratch_total = 0;
    // nearest first (the distance of the object's origin in eye space): while a scene loads the budget goes to what is around the camera
    std::vector<std::pair<float, Group*>> order;
    order.reserve(sGroups.size());
    for (auto& kv : sGroups)
    {
        Group& g = kv.second;
        F32 m[16];
        mat_mul(view, g.mModel, m);
        order.emplace_back(m[12] * m[12] + m[13] * m[13] + m[14] * m[14], &g);
    }
    std::sort(order.begin(), order.end(), [](const std::pair<float, Group*>& a, const std::pair<float, Group*>& b) { return a.first < b.first; });
    for (const auto& entry : order)
    {
        Group& g = *entry.second;
        if (g.mKey.mKind == 1 && !alpha_on)
        {
            continue;
        }
        if (!LLVkGL::getBufferInfo(g.mKey.mVb, vi) || !LLVkGL::getBufferInfo(g.mKey.mIb, ii) || !vi.mAddress || !ii.mAddress || vi.mSize < 16 || ii.mSize < 6)
        {
            ++sStats.mNoData;
            continue;
        }
        Blas& b = sBlas[g.mKey];
        b.mLastSeen = sSceneCalls;
        if (b.mAs != VK_NULL_HANDLE && (b.mVbEpoch != vi.mEpoch || b.mIbEpoch != ii.mEpoch))
        {
            drop_blas(b);                       // the names hold something else now
            b.mAlpha.clear();
        }
        if (b.mAs == VK_NULL_HANDLE)
        {
            b.mVbEpoch = vi.mEpoch;
            b.mIbEpoch = ii.mEpoch;
        }
        // the ranges the structure has to cover: what it covers already plus what was drawn now
        std::vector<Range> want = b.mBuilt;
        bool grew = b.mAs == VK_NULL_HANDLE;
        for (size_t k = 0; k < g.mRanges.size(); ++k)
        {
            const Range& r = g.mRanges[k];
            if (((U64)r.mFirst + r.mCount) * 2 > ii.mSize)
            {
                continue;                       // outside the data (a stale range)
            }
            if (g.mKey.mKind == 1)
            {
                b.mAlpha[r.id()] = g.mAlpha[k];     // the textures and cutoff as drawn now
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
        Job job;
        job.mKey = g.mKey;
        job.mBlas = &b;
        job.mUpdate = !grew && !ib_changed && b.mRefits < rebuild_after_refits && b.mAs != VK_NULL_HANDLE;
        job.mVbVersion = vi.mVersion;
        job.mIbVersion = ii.mVersion;
        std::sort(want.begin(), want.end());
        const U32 max_vertex = (U32)(vi.mSize / 16 - 1);
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
        scratch_total = job.mScratchOffset + job.mScratchSize;
        tris_budget = prims >= tris_budget ? 0 : tris_budget - prims;
        jobs.push_back(std::move(job));
    }

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
    std::unordered_map<Key, const Job*, KeyHash> job_of;
    for (const Job& j : jobs)
    {
        job_of[j.mKey] = &j;
    }
    for (auto& kv : sGroups)
    {
        const Group& g = kv.second;
        if (g.mKey.mKind == 1 && !alpha_on)
        {
            continue;
        }
        auto bit = sBlas.find(g.mKey);
        if (bit == sBlas.end())
        {
            continue;
        }
        const Blas& b = bit->second;
        VkDeviceAddress address = b.mAddress;
        auto jit = job_of.find(g.mKey);
        const Job* job = jit != job_of.end() ? jit->second : nullptr;
        if (job && !job->mUpdate)
        {
            address = job->mNewAddress;
        }
        if (!address)
        {
            continue;
        }
        if (instances.size() >= kInstanceCapacity)
        {
            ++sStats.mOverCap;
            break;
        }
        U32 custom_index = 0;
        if (g.mKey.mKind == 1)
        {
            // one record per geometry, in the order of the structure's geometry list
            const std::vector<Range>& set = job ? job->mSet : b.mBuilt;
            if (records + set.size() > kMaxRecords || !LLVkGL::getBufferInfo(g.mKey.mVb, vi) || !LLVkGL::getBufferInfo(g.mKey.mIb, ii))
            {
                ++sStats.mTableFull;
                continue;
            }
            custom_index = records;
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
                        slot[k] = slot_of(a.mTextures[k]) & 0xFFFFu;
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
        if (!infos.empty())
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
        sGroupsConsumed = false;
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
    ++sStats.mScenes;
    sStats.mInstances += instances.size();
    sLastInstances = (U32)instances.size();
    sLastBuilt = (U32)jobs.size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sStats.mCpuMs += ms;
    sStats.mCpuMsMax = std::max(sStats.mCpuMsMax, ms);
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
             "CPU %.3f ms/call (worst %.2f ms), scratch %.1f MB",
             shadowMode(), live, (double)sBlasStorageBytes / 1048576.0, (double)prims / 1e6, (double)s.mInstances / scenes, sLastInstances, (double)s.mCasters / f, (double)s.mBuilds / f,
             (double)s.mPrimsBuilt / f, (double)s.mRefits / f, (double)s.mPrimsRefit / f, (double)s.mDeferred / f, (double)s.mNoData / f, (double)s.mAlphaStructs / f, (double)s.mTableFull / f,
             (double)s.mOverCap / f, (double)s.mFailed / f, (double)s.mEvicted / f, (double)s.mScenes / f, kCalls, (double)s.mExtraCalls / f, s.mCpuMs / scenes, s.mCpuMsMax,
             (double)sScratch.mSize / 1048576.0);
    return buf;
}

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
