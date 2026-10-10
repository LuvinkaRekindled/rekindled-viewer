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

struct Range
{
    U32 mFirst = 0, mCount = 0;
    bool operator<(const Range& o) const { return mFirst != o.mFirst ? mFirst < o.mFirst : mCount < o.mCount; }
    bool operator==(const Range& o) const { return mFirst == o.mFirst && mCount == o.mCount; }
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
};

struct Blas
{
    U64 mVbEpoch = 0, mIbEpoch = 0;
    U64 mVbVersion = ~0ull, mIbVersion = ~0ull;     // contents at the last build or refit
    std::vector<Range> mBuilt;                      // sorted: what the structure covers
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
    Buffer mInstances;
    U32 mCapacity = 0;
    VkDescriptorSet mSet = VK_NULL_HANDLE;
};

struct Stats
{
    U64 mScenes = 0, mBuilds = 0, mRefits = 0, mPrimsBuilt = 0, mPrimsRefit = 0, mDeferred = 0, mNoData = 0, mAlphaSkipped = 0, mFailed = 0, mEvicted = 0, mInstances = 0, mCasters = 0;
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

// a top-level structure (and its instance buffer) for up to `capacity` instances; the set is written to point at it
bool make_tlas(Tlas& t, U32 capacity)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    if (t.mAs != VK_NULL_HANDLE)
    {
        VkAccelerationStructureKHR old = t.mAs;
        ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, old, nullptr); });
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        t.mAs = VK_NULL_HANDLE;
        t.mCapacity = 0;
    }
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    tlas_sizes(capacity, sizes);
    if (!ctx.createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Context::Memory::Device, t.mStorage)
        || !ctx.createBuffer(64ull * capacity, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, Context::Memory::UploadDevice, t.mInstances))
    {
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = t.mStorage.mBuffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &t.mAs) != VK_SUCCESS)
    {
        t.mAs = VK_NULL_HANDLE;
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        return false;
    }
    t.mCapacity = capacity;
    VkWriteDescriptorSetAccelerationStructureKHR as_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &t.mAs;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.pNext = &as_write;
    w.dstSet = t.mSet;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
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
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Context::frameCount() };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = Context::frameCount();
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
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
    const U32 initial_capacity = 1024;
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    tlas_sizes(initial_capacity, sizes);
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        sTlas[i].mSet = sets[i];
        if (!make_tlas(sTlas[i], initial_capacity))
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
    LL_INFOS("VkRt") << "ray tracing scene ready: mode " << shadowMode() << ", " << Context::frameCount() << " frame slots, instance capacity " << initial_capacity << LL_ENDL;
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
    }
}

VkDescriptorSet descriptorSet(U32 frame_slot)
{
    return frame_slot < Context::FRAMES_IN_FLIGHT ? sTlas[frame_slot].mSet : VK_NULL_HANDLE;
}

void buildScene(const F32* view)
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
    static const bool alpha_as_opaque = env_u64("LL_VK_RT_ALPHA_OPAQUE", 0) != 0;
    static const U64 rebuild_after_refits = 64;
    ++sSceneCalls;
    sGroupsConsumed = true;

    // forget structures that were not named for a while, or whose buffers hold something else
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
        if (g.mKey.mKind == 1 && !alpha_as_opaque)
        {
            ++sStats.mAlphaSkipped;
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
        }
        if (b.mAs == VK_NULL_HANDLE)
        {
            b.mVbEpoch = vi.mEpoch;
            b.mIbEpoch = ii.mEpoch;
        }
        // the ranges the structure has to cover: what it covers already plus what was drawn now
        std::vector<Range> want = b.mBuilt;
        bool grew = b.mAs == VK_NULL_HANDLE;
        for (const Range& r : g.mRanges)
        {
            if (((U64)r.mFirst + r.mCount) * 2 > ii.mSize)
            {
                continue;                       // outside the data (a stale range)
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
            geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
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

    // ---- 2. instances ---------------------------------------------------------------------------------------
    // a structure that is being (re)built now is part of this frame's scene: its new address is used
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
        if (g.mKey.mKind == 1 && !alpha_as_opaque)
        {
            continue;
        }
        auto bit = sBlas.find(g.mKey);
        if (bit == sBlas.end())
        {
            continue;
        }
        VkDeviceAddress address = bit->second.mAddress;
        auto jit = job_of.find(g.mKey);
        if (jit != job_of.end() && !jit->second->mUpdate)
        {
            address = jit->second->mNewAddress;
        }
        if (!address)
        {
            continue;
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
        inst.instanceCustomIndex = 0;
        inst.mask = 1u << std::min<U32>(g.mKey.mKind, 7u);
        if (g.mKey.mKind == 1 && alpha_as_opaque)
        {
            inst.mask = 1;
        }
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = address;
        instances.push_back(inst);
    }

    // ---- 3. record: builds, then the top-level structure -----------------------------------------------------
    Tlas& tlas = sTlas[ctx.getFrameSlot()];
    U32 capacity = tlas.mCapacity;
    while (capacity < instances.size())
    {
        capacity *= 2;
    }
    if (capacity != tlas.mCapacity && !make_tlas(tlas, capacity))
    {
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
        ++sStats.mFailed;
        return;
    }
    VkAccelerationStructureBuildSizesInfoKHR tlas_size;
    tlas_sizes(tlas.mCapacity, tlas_size);
    const VkDeviceSize scratch_needed = std::max(scratch_total, tlas_size.buildScratchSize) + 2 * align;
    if (!ensure_scratch(scratch_needed))
    {
        ++sStats.mFailed;
        return;
    }
    if (!instances.empty())
    {
        memcpy(tlas.mInstances.mMapped, instances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instances.size());
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
    tgeo.geometry.instances.data.deviceAddress = tlas.mInstances.mAddress;
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
            drop_blas(b);                       // the structure that is replaced (a new one was made)
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
    char buf[900];
    snprintf(buf, sizeof(buf),
             "rt scene (mode %u): %u structures (%.0f MB, %.2f M triangles), instances %.0f/scene (last %u), casters named %.0f/frame, builds %.2f/frame (%.0f tri), refits %.2f/frame (%.0f tri), "
             "deferred %.2f, no data %.2f, alpha casters skipped %.1f, failed %.3f, evicted %.3f, scene calls %.2f/frame, CPU %.3f ms/call (worst %.2f ms), scratch %.1f MB",
             shadowMode(), live, (double)sBlasStorageBytes / 1048576.0, (double)prims / 1e6, (double)s.mInstances / scenes, sLastInstances, (double)s.mCasters / std::max(1.0, frames),
             (double)s.mBuilds / std::max(1.0, frames), (double)s.mPrimsBuilt / std::max(1.0, frames), (double)s.mRefits / std::max(1.0, frames), (double)s.mPrimsRefit / std::max(1.0, frames),
             (double)s.mDeferred / std::max(1.0, frames), (double)s.mNoData / std::max(1.0, frames), (double)s.mAlphaSkipped / std::max(1.0, frames), (double)s.mFailed / std::max(1.0, frames),
             (double)s.mEvicted / std::max(1.0, frames), (double)s.mScenes / std::max(1.0, frames), s.mCpuMs / scenes, s.mCpuMsMax, (double)sScratch.mSize / 1048576.0);
    return buf;
}

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
