/**
 * @file llvkrt.cpp
 * @brief Ray tracing scene: bottom-level structures per vertex/index buffer pair, a top-level structure per frame (see llvkrt.h and doc/vulkan/PLAN-rt-shadows.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkrt.h"
#include "llvkrenderer.h"
#include "llvkmirror.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

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
    F32 mUv[6];             // texture coordinate transform (uv' = uv0 * u + uv1 * v + uv2)
};
static_assert(sizeof(GpuGeom) == 80, "geometry record");
constexpr U32 kMaxRecords = (U32)(kTableBytes / sizeof(GpuGeom));
constexpr U32 kMaxSkinJobs = 2048;                          // compute skinning jobs per scene call
constexpr U32 kSkinChunkVerts = 128;                        // vertices per workgroup (one chunk table entry)
constexpr U32 kMaxSkinChunks = 131072;
constexpr VkDeviceSize kSkinJobBytes = 96ull * kMaxSkinJobs;
constexpr VkDeviceSize kSkinBytes = kSkinJobBytes + 8ull * kMaxSkinChunks;
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
    ArenaRef mSkinOut;                              // the compute pass output the structure is built from (skin test and stage C), same indexing as the source vertex buffer
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
    Buffer mSkin;                                   // kCalls regions of kSkinBytes: the compute skinning job and chunk tables
    U32 mCapacity = 0;
    VkDescriptorSet mSet = VK_NULL_HANDLE;
    U64 mCallFrame = ~0ull;                         // frame number of the calls counted in mCalls
    U32 mCalls = 0;
};

struct Stats
{
    U64 mScenes = 0, mBuilds = 0, mRefits = 0, mPrimsBuilt = 0, mPrimsRefit = 0, mDeferred = 0, mNoData = 0, mAlphaStructs = 0, mTableFull = 0, mFailed = 0, mEvicted = 0, mInstances = 0, mCasters = 0;
    U64 mSkinJobs = 0, mSkinVerts = 0, mSkinFailed = 0, mAvatars = 0, mSkinOver = 0;
    U64 mOverCap = 0, mExtraCalls = 0, mBadVertices = 0, mBadMatrices = 0, mChecked = 0, mSingular = 0, mCompared = 0, mMismatched = 0;
    F32 mMaxScale = 0.f;
    Key mMaxScaleKey;
    U64 mWhyNew = 0, mWhyEpoch = 0, mWhyGrew = 0, mWhyIb = 0, mWhyRefitLimit = 0, mNewGroups = 0, mNewMasked = 0;      // why structures were built (and how many casters were new)
    double mCpuMs = 0, mCpuMsMax = 0;
    double mPhaseMs[6] = {};        // expire, order, scan (with sort), instances, recording, the rest
};

// Switches that can be changed while the viewer runs: the file named by LL_VK_RT_DEBUG_FILE (default D:/claude/rt-debug.txt) is read again every half second (key=value lines:
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
    bool mCallLog = false;          // one line per scene call into D:/claude/rt-calls.log (wall clock, frame, call, jobs, instances)
    bool mSkinProbe = false;        // about every 90 scene calls: run the skin jobs again on their own, read a few results back and log their bounding boxes
    bool mRigged = true;            // rigged opaque casters (avatar attachments) in the scene, skinned by the compute pass
    bool mSkinTest = false;         // stage C step C1: structures of opaque casters are built from a compute-pass copy of their vertices (must look identical to the direct path)
    bool mBBox = false;             // one line per build/refit of a structure with a matrix of its own (CPU-visible data only) into D:/claude/rt-bbox.log: bounding box, largest coordinate (stage C step 0)
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
    static const char* path = []() { const char* e = getenv("LL_VK_RT_DEBUG_FILE"); return e && e[0] ? e : "D:/claude/rt-debug.txt"; }();
    DebugSwitches d;
    d.mKeep = env_u64("LL_VK_RT_KEEP_FRAMES", 8);
    d.mAlpha = env_u64("LL_VK_RT_ALPHA", 1) != 0;
    d.mCheck = env_u64("LL_VK_RT_CHECK", 0) != 0;
    d.mSkipBad = env_u64("LL_VK_RT_SKIPBAD", 1) != 0;
    d.mDynCasters = env_u64("LL_VK_RT_DYNAMIC", 0) != 0;
    d.mSkinTest = env_u64("LL_VK_RT_SKINTEST", 0) != 0;
    d.mRigged = env_u64("LL_VK_RT_RIGGED", 1) != 0;
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
                else if (k == "bbox") d.mBBox = v != 0;
                else if (k == "skintest") d.mSkinTest = v != 0;
                else if (k == "rigged") d.mRigged = v != 0;
                else if (k == "skinprobe") d.mSkinProbe = v != 0;
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
        || d.mMaxDist != sDbg.mMaxDist || d.mSingular != sDbg.mSingular || d.mMaxInstances != sDbg.mMaxInstances || d.mBuildMax != sDbg.mBuildMax || d.mSerial != sDbg.mSerial || d.mPad != sDbg.mPad || d.mCallLog != sDbg.mCallLog || d.mDynCasters != sDbg.mDynCasters || d.mCompare != sDbg.mCompare || d.mBBox != sDbg.mBBox || d.mSkinTest != sDbg.mSkinTest || d.mRigged != sDbg.mRigged || d.mMaxVtx != sDbg.mMaxVtx || d.mSkipBuf != sDbg.mSkipBuf)
    {
        LL_INFOS("VkRt") << "debug switches: single " << d.mSingle << ", norefit " << d.mNoRefit << ", keep " << d.mKeep << ", common_only " << d.mCommonOnly << ", alpha " << d.mAlpha
                         << ", check " << d.mCheck << ", skipbad " << d.mSkipBad << ", only " << d.mOnly << ", moving " << d.mMoving << ", dynamic " << d.mDynamic << ", kind " << d.mKind
                         << ", dist " << d.mMinDist << ".." << d.mMaxDist << ", dump " << d.mDump << ", dynamic casters " << d.mDynCasters << ", singular " << d.mSingular << ", max instances " << d.mMaxInstances << ", build max " << d.mBuildMax << ", serial " << d.mSerial << ", pad " << d.mPad << ", skintest " << d.mSkinTest << ", rigged " << d.mRigged << LL_ENDL;
    }
    sDbg = d;
}

// a rigged caster draw of an avatar (a draw of a rigged mesh: body parts do not come through here, attachments do)
struct SkinDraw
{
    U32 mVb = 0, mIb = 0, mFirst = 0, mCount = 0, mVertexFirst = 0, mVertexCount = 0, mSkinId = 0, mWeightOffset = 0, mJoints = 0;
    U64 mPalette = 0;                   // this frame's palette (memory of the frame; valid in the frame it was named)
    F32 mModel[16] = {};
    bool mHasModel = false;
    U32 mKind = 0;                      // 0 opaque, 1 alpha masked
    U32 mMode = 0;                      // skinning mode of the compute pass
    F32 mAvatarPos[3] = {};
    AlphaInfo mAlpha;
    LLVkGL::BufferInfo mVi, mIi;
    U64 mNotedTag = 0, mSkinnedTag = 0;
    ArenaRef mOut;                      // the skinned vertices (world space), indexed like the vertex buffer relative to mVertexFirst
};

// one structure per avatar: a geometry per rigged draw of the frame, over the skinned copies
struct Avatar
{
    Key mKey;
    Blas* mBlas = nullptr;
    std::vector<SkinDraw> mDraws;
    std::vector<U64> mSig;              // what the structure was built from
    std::vector<U32> mBuiltIdx;         // its geometries: indices into mDraws (mDraws only grows, except where mSig is cleared)
    U64 mNotedTag = 0;                  // latest frame a draw was named
    F32 mPos[3] = {};                   // where the avatar is (named by its body meshes)
    bool mHasPos = false;
    U64 mProcessedTag = 0;              // frame whose jobs were made
    VkDeviceAddress mAddressNow = 0;    // the structure the scene uses (a new one when it is being built this call)
};

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
std::unordered_map<U32, Avatar> sAvatars;
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
    if (b.mSkinOut.valid())
    {
        Context::instance().arenaFree(b.mSkinOut);
        b.mSkinOut = ArenaRef();
    }
    b.mBuilt.clear();
    b.mVbVersion = b.mIbVersion = ~0ull;
    b.mPrims = 0;
    b.mRefits = 0;
}

void release_avatar(Avatar& a)
{
    Context& ctx = Context::instance();
    for (SkinDraw& d : a.mDraws)
    {
        if (d.mOut.valid())
        {
            ctx.arenaFree(d.mOut);
        }
    }
    a.mDraws.clear();
    if (a.mBlas)
    {
        drop_blas(*a.mBlas);
        sBlas.erase(a.mKey);
        a.mBlas = nullptr;
    }
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
        || !ctx.createBuffer(kTableBytes * kCalls, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, t.mTable)
        || !ctx.createBuffer(kSkinBytes * kCalls, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, t.mSkin))
    {
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        ctx.destroyBuffer(t.mTable);
        ctx.destroyBuffer(t.mSkin);
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
        ctx.destroyBuffer(t.mSkin);
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

// ---- compute skinning pass (stage C, doc/vulkan/PLAN-rt-shadows.md) --------------------------------------------------------------------------------------------------------
// One dispatch per scene call over a list of jobs; a job turns a range of a source vertex array (vec4 positions, bind pose) into world space positions at the same indices of an
// output array: mode 0 = rigged (weight4 array + matrix palette, 12 floats per joint), mode 1 = an avatar's own mesh (one weight float), mode 2 = model matrix only (also the plain copy of the skin test). The model matrix of a job
// is applied last (rows of a 3x4). dst is the address the shader indexes by the ABSOLUTE vertex index (the caller subtracts 16 * first from the range's address).
struct SkinJob
{
    U32 mSrc[2];
    U32 mWeights[2];
    U32 mPalette[2];
    U32 mDst[2];
    U32 mFirst, mCount, mMode, mJoints;
    F32 mModel[12];                 // three rows of four
};
static_assert(sizeof(SkinJob) == 96, "skin job");

struct SkinChunk
{
    U32 mJob, mOffset;
};

const char* kSkinShader = R"GLSL(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 128) in;
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer InFloat { float w[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer OutVec4 { vec4 v[]; };
struct Joint { vec4 c0; vec4 c1; vec4 c2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Palette { Joint j[]; };
struct Job { uvec2 src; uvec2 weights; uvec2 palette; uvec2 dst; uint first; uint count; uint mode; uint joints; vec4 m0; vec4 m1; vec4 m2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Jobs { Job j[]; };
struct Chunk { uint job; uint offset; };
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Chunks { Chunk c[]; };
layout(push_constant) uniform Push { uvec2 jobs; uvec2 chunks; } pc;
void main()
{
    Chunk ch = Chunks(pc.chunks).c[gl_WorkGroupID.x];
    Job job = Jobs(pc.jobs).j[ch.job];
    uint i = ch.offset + gl_LocalInvocationID.x;
    if (i >= job.count)
    {
        return;
    }
    uint v = job.first + i;
    vec4 source = InVec4(job.src).v[v];
    vec3 pos = source.xyz;
    if (job.mode == 0u)
    {
        // as getObjectSkinnedTransform() in objectSkinV.glsl; the joint numbers stay inside the palette that was sent
        vec4 weight4 = InVec4(job.weights).v[v];
        vec4 w = fract(weight4);
        vec4 joint = clamp(floor(weight4), vec4(0.0), vec4(float(job.joints) - 1.0));
        float weight_sum = w.x + w.y + w.z + w.w;
        w = weight_sum > 1e-6 ? w / weight_sum : vec4(1.0, 0.0, 0.0, 0.0);
        Palette palette = Palette(job.palette);
        Joint j1 = palette.j[int(joint.x)];
        Joint j2 = palette.j[int(joint.y)];
        Joint j3 = palette.j[int(joint.z)];
        Joint j4 = palette.j[int(joint.w)];
        mat3 m = mat3(j1.c0.xyz, j1.c1.xyz, j1.c2.xyz) * w.x;
        m += mat3(j2.c0.xyz, j2.c1.xyz, j2.c2.xyz) * w.y;
        m += mat3(j3.c0.xyz, j3.c1.xyz, j3.c2.xyz) * w.z;
        m += mat3(j4.c0.xyz, j4.c1.xyz, j4.c2.xyz) * w.w;
        vec3 trans = vec3(j1.c0.w, j1.c1.w, j1.c2.w) * w.x;
        trans += vec3(j2.c0.w, j2.c1.w, j2.c2.w) * w.y;
        trans += vec3(j3.c0.w, j3.c1.w, j3.c2.w) * w.z;
        trans += vec3(j4.c0.w, j4.c1.w, j4.c2.w) * w.w;
        pos = m * pos + trans;
    }
    else if (job.mode == 1u)
    {
        // an avatar's own mesh (avatarSkinV.glsl): one weight per vertex, the joints i and i + 1 of the palette mixed
        float weight = InFloat(job.weights).w[v];
        int i = int(clamp(floor(weight), 0.0, float(job.joints) - 1.0));
        int i2 = min(i + 1, int(job.joints) - 1);
        float x = fract(weight);
        Palette palette = Palette(job.palette);
        Joint ja = palette.j[i];
        Joint jb = palette.j[i2];
        vec4 ps = vec4(pos, 1.0);
        pos = vec3(dot(mix(ja.c0, jb.c0, x), ps), dot(mix(ja.c1, jb.c1, x), ps), dot(mix(ja.c2, jb.c2, x), ps));
    }
    vec4 p4 = vec4(pos, 1.0);
    OutVec4(job.dst).v[v] = vec4(dot(job.m0, p4), dot(job.m1, p4), dot(job.m2, p4), source.w);       // w: the texture index of masked draws
}
)GLSL";

struct SkinPipeline
{
    VkShaderModule mModule = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    bool mTried = false;
} sSkinPipe;

bool ensure_skin_pipeline()
{
    if (sSkinPipe.mPipe != VK_NULL_HANDLE)
    {
        return true;
    }
    if (sSkinPipe.mTried)
    {
        return false;
    }
    sSkinPipe.mTried = true;
    static const bool glslang_ready = glslang::InitializeProcess();
    if (!glslang_ready)
    {
        LL_WARNS("VkRt") << "skinning pass: glslang did not start" << LL_ENDL;
        return false;
    }
    glslang::TShader shader(EShLangCompute);
    const char* strings[1] = { kSkinShader };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, ECoreProfile, false, false, msgs))
    {
        LL_WARNS("VkRt") << "skinning pass: compile failed: " << shader.getInfoLog() << LL_ENDL;
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs))
    {
        LL_WARNS("VkRt") << "skinning pass: link failed: " << program.getInfoLog() << LL_ENDL;
        return false;
    }
    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &logger, &options);
    if (spirv.empty())
    {
        LL_WARNS("VkRt") << "skinning pass: no SPIR-V: " << logger.getAllMessages() << LL_ENDL;
        return false;
    }
    VkDevice dev = Context::instance().getDevice();
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spirv.size() * sizeof(unsigned int);
    mi.pCode = spirv.data();
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    if (vkCreateShaderModule(dev, &mi, nullptr, &sSkinPipe.mModule) != VK_SUCCESS || vkCreatePipelineLayout(dev, &li, nullptr, &sSkinPipe.mLayout) != VK_SUCCESS)
    {
        LL_WARNS("VkRt") << "skinning pass: shader module or layout failed" << LL_ENDL;
        return false;
    }
    pi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = sSkinPipe.mModule;
    pi.stage.pName = "main";
    pi.layout = sSkinPipe.mLayout;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &sSkinPipe.mPipe) != VK_SUCCESS)
    {
        LL_WARNS("VkRt") << "skinning pass: pipeline failed" << LL_ENDL;
        sSkinPipe.mPipe = VK_NULL_HANDLE;
        return false;
    }
    LL_INFOS("VkRt") << "skinning pass ready (compute, " << kSkinChunkVerts << " vertices per workgroup, up to " << kMaxSkinJobs << " jobs and " << kMaxSkinChunks << " chunks per scene call)" << LL_ENDL;
    return true;
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
    bool mHasSkin = false;          // a compute pass job fills the vertex data the structure is built from
    SkinJob mSkin = {};
};

} // anonymous namespace

U32 shadowMode()
{
    static const U32 mode = []() {
        const U64 v = env_u64("LL_VK_RT_SHADOWS", 0);
        return v >= 1 && v <= 5 ? (U32)v : 0u;
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
    if (sSkinPipe.mPipe != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(dev, sSkinPipe.mPipe, nullptr);
    }
    if (sSkinPipe.mLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(dev, sSkinPipe.mLayout, nullptr);
    }
    if (sSkinPipe.mModule != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(dev, sSkinPipe.mModule, nullptr);
    }
    sSkinPipe = SkinPipeline();
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

void noteSkinned(const SkinCaster& c)
{
    if (!sInit || sFailed || !sDbg.mRigged || c.mCount < 3 || c.mVertexCount == 0 || (c.mMode != 2 && (!c.mPalette || c.mJoints == 0)))
    {
        return;
    }
    auto it = sAvatars.find(c.mAvatarId);
    if (it == sAvatars.end())
    {
        Key key;
        key.mVb = c.mAvatarId;
        key.mIb = 0xFFFFFFFFu;
        key.mMatrixId = 0;
        key.mKind = 3;
        it = sAvatars.emplace(c.mAvatarId, Avatar()).first;
        it->second.mKey = key;
        it->second.mBlas = &sBlas[key];
    }
    Avatar& a = it->second;
    if (c.mAvatarPos[0] != 0.f || c.mAvatarPos[1] != 0.f || c.mAvatarPos[2] != 0.f)
    {
        memcpy(a.mPos, c.mAvatarPos, sizeof(a.mPos));
        a.mHasPos = true;
    }
    const U64 tag = Context::instance().getFrameNumber() + 1;
    if (a.mNotedTag != tag)
    {
        a.mNotedTag = tag;
        a.mBlas->mLastSeen = sSceneCalls;
    }
    ++sStats.mCasters;
    for (SkinDraw& d : a.mDraws)
    {
        if (d.mVb == c.mVb && d.mIb == c.mIb && d.mFirst == c.mFirst && d.mCount == c.mCount && d.mVertexFirst == c.mVertexFirst && d.mVertexCount == c.mVertexCount && d.mSkinId == c.mSkinId && d.mKind == c.mKind && d.mMode == c.mMode)
        {
            if (c.mAlpha)
            {
                d.mAlpha = *c.mAlpha;               // the textures may have changed
            }
            if (d.mNotedTag != tag)
            {
                // first note of this frame (the cascades name a draw once each): this frame's palette and where the buffers are now
                d.mNotedTag = tag;
                d.mPalette = c.mPalette;
                d.mJoints = c.mJoints;
                d.mVi = c.mVi;
                d.mIi = c.mIi;
                d.mWeightOffset = c.mWeightOffset;
                d.mHasModel = c.mModel != nullptr;
                if (c.mModel)
                {
                    memcpy(d.mModel, c.mModel, sizeof(d.mModel));
                }
            }
            return;
        }
    }
    if (a.mDraws.size() >= 256)
    {
        return;
    }
    SkinDraw d;
    d.mVb = c.mVb;
    d.mIb = c.mIb;
    d.mFirst = c.mFirst;
    d.mCount = c.mCount;
    d.mVertexFirst = c.mVertexFirst;
    d.mVertexCount = c.mVertexCount;
    d.mSkinId = c.mSkinId;
    d.mKind = c.mKind;
    d.mMode = c.mMode;
    memcpy(d.mAvatarPos, c.mAvatarPos, sizeof(d.mAvatarPos));
    if (c.mAlpha)
    {
        d.mAlpha = *c.mAlpha;
    }
    d.mWeightOffset = c.mWeightOffset;
    d.mJoints = c.mJoints;
    d.mPalette = c.mPalette;
    d.mVi = c.mVi;
    d.mIi = c.mIi;
    d.mNotedTag = tag;
    d.mHasModel = c.mModel != nullptr;
    if (c.mModel)
    {
        memcpy(d.mModel, c.mModel, sizeof(d.mModel));
    }
    a.mDraws.push_back(std::move(d));
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
    std::vector<SkinJob> skin_jobs;
    std::vector<SkinChunk> skin_chunks;
    std::vector<SkinDraw*> skin_job_draws;      // the draw behind each job (null for the skin test copies), for the probe
    const bool skin_on = sDbg.mSkinTest && ensure_skin_pipeline();
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
        // skin test: the structure is built from the compute pass output (a copy of the vertex buffer), not from the buffer itself
        VkDeviceAddress vertex_address = vi.mAddress;
        bool force_build = false;
        const bool skin_copy = skin_on && g.mKey.mKind == 0;
        if (skin_copy)
        {
            const U32 vertex_count = (U32)(vi.mSize / 16);
            const U32 chunks_needed = (vertex_count + kSkinChunkVerts - 1) / kSkinChunkVerts;
            if (skin_jobs.size() >= kMaxSkinJobs || skin_chunks.size() + chunks_needed > kMaxSkinChunks)
            {
                ++sStats.mDeferred;
                continue;
            }
            const VkDeviceSize bytes = ((VkDeviceSize)vertex_count * 16 + 255) / 256 * 256;
            if (!b.mSkinOut.valid() || b.mSkinOut.mSize < bytes)
            {
                if (b.mSkinOut.valid())
                {
                    ctx.arenaFree(b.mSkinOut);
                    b.mSkinOut = ArenaRef();
                }
                if (!ctx.arenaAlloc(bytes, 256, ArenaKind::Device, b.mSkinOut))
                {
                    ++sStats.mSkinFailed;
                    continue;
                }
                force_build = true;             // the structure was built from other memory
            }
            vertex_address = b.mSkinOut.mAddress;
            job.mHasSkin = true;
            SkinJob& sj = job.mSkin;
            memset(&sj, 0, sizeof(sj));
            sj.mSrc[0] = (U32)vi.mAddress;
            sj.mSrc[1] = (U32)(vi.mAddress >> 32);
            sj.mDst[0] = (U32)b.mSkinOut.mAddress;
            sj.mDst[1] = (U32)(b.mSkinOut.mAddress >> 32);
            sj.mFirst = 0;
            sj.mCount = vertex_count;
            sj.mMode = 2;
            sj.mModel[0] = sj.mModel[5] = sj.mModel[10] = 1.f;
        }
        job.mUpdate = !grew && !ib_changed && !force_build && b.mRefits < rebuild_after_refits && b.mAs != VK_NULL_HANDLE && !sDbg.mNoRefit;
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
            t.vertexData.deviceAddress = vertex_address;
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
                    t.vertexData.deviceAddress = vertex_address;
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
        if (sDbg.mBBox && (g.mKey.mMatrixId & 0x80000000u) == 0 && g.mKey.mMatrixId != 0)
        {
            // stage C step 0: the size of what a structure with a matrix of its own is about to be built or refitted over (flexible prims that go unstable would show up as outsized boxes)
            static FILE* bb_log = fopen("D:/claude/rt-bbox.log", "ab");
            if (bb_log)
            {
                F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                F32 max_abs = 0.f;
                U32 nonfinite = 0, out_of_range = 0, verts_seen = 0;
                if (vi.mCpu && ii.mCpu)
                {
                    const F32* pos = (const F32*)vi.mCpu;
                    const U16* idx = (const U16*)ii.mCpu;
                    const U64 verts = vi.mSize / 16;
                    for (const Range& r : want)
                    {
                        for (U32 k = 0; k < r.mCount; ++k)
                        {
                            const U32 v = idx[r.mFirst + k];
                            if (v >= verts)
                            {
                                ++out_of_range;
                                continue;
                            }
                            ++verts_seen;
                            for (int a = 0; a < 3; ++a)
                            {
                                const F32 c = pos[4 * (size_t)v + a];
                                if (!(c > -1e30f && c < 1e30f))
                                {
                                    ++nonfinite;
                                    continue;
                                }
                                lo[a] = std::min(lo[a], c);
                                hi[a] = std::max(hi[a], c);
                                max_abs = std::max(max_abs, fabsf(c));
                            }
                        }
                    }
                }
                const F32 extent = verts_seen ? std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2])) : -1.f;
                F32 scale = 0.f;
                for (int c = 0; c < 3; ++c)
                {
                    scale = std::max(scale, sqrtf(g.mModel[c * 4] * g.mModel[c * 4] + g.mModel[c * 4 + 1] * g.mModel[c * 4 + 1] + g.mModel[c * 4 + 2] * g.mModel[c * 4 + 2]));
                }
                const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                fprintf(bb_log, "%lld frame %llu vb %u ib %u matrix %u kind %u %s prims %llu ranges %zu cpu %d verts %u nonfinite %u oor %u extent %.3f maxabs %.3f lo %.2f %.2f %.2f hi %.2f %.2f %.2f scale %.3f pos %.1f %.1f %.1f\n",
                        wall, (unsigned long long)ctx.getFrameNumber(), g.mKey.mVb, g.mKey.mIb, g.mKey.mMatrixId, g.mKey.mKind, job.mUpdate ? "refit" : (fresh ? "new" : "rebuild"), (unsigned long long)prims, want.size(),
                        (vi.mCpu && ii.mCpu) ? 1 : 0, verts_seen, nonfinite, out_of_range, extent, max_abs, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], scale, g.mModel[12], g.mModel[13], g.mModel[14]);
                fflush(bb_log);
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
        if (jobs.back().mHasSkin)
        {
            const U32 ji = (U32)skin_jobs.size();
            skin_jobs.push_back(jobs.back().mSkin);
            skin_job_draws.push_back(nullptr);
            for (U32 off = 0; off < jobs.back().mSkin.mCount; off += kSkinChunkVerts)
            {
                skin_chunks.push_back({ ji, off });
            }
        }
        b.mJob = &jobs.back();
        b.mJobCall = sSceneCalls;
    }

    // ---- 1b. rigged draws: skin them on the GPU, one structure per avatar over the skinned copies -------------------------------------------------
    const U64 av_tag = ctx.getFrameNumber() + 1;
    static const U64 skin_refit_limit = env_u64("LL_VK_RT_SKIN_REFITS", 32);
    static const U64 skin_vertex_budget = env_u64("LL_VK_RT_SKIN_VERTS", 2000000);
    U64 skin_verts_left = skin_vertex_budget;
    std::vector<std::pair<float, Avatar*>> av_order;
    for (auto ait = sAvatars.begin(); ait != sAvatars.end();)
    {
        Avatar& a = ait->second;
        if (av_tag > a.mNotedTag + 120)
        {
            release_avatar(a);
            ait = sAvatars.erase(ait);
            continue;
        }
        ++ait;
        if (a.mNotedTag != av_tag || a.mProcessedTag == av_tag || !sDbg.mRigged)
        {
            continue;
        }
        float dist2 = 0.f;
        if (a.mHasPos)
        {
            const float ex = view[0] * a.mPos[0] + view[4] * a.mPos[1] + view[8] * a.mPos[2] + view[12];
            const float ey = view[1] * a.mPos[0] + view[5] * a.mPos[1] + view[9] * a.mPos[2] + view[13];
            const float ez = view[2] * a.mPos[0] + view[6] * a.mPos[1] + view[10] * a.mPos[2] + view[14];
            dist2 = ex * ex + ey * ey + ez * ez;
        }
        av_order.emplace_back(dist2, &a);
    }
    // nearest first: when the budget runs out it is the far avatars that keep their last pose
    std::sort(av_order.begin(), av_order.end(), [](const std::pair<float, Avatar*>& l, const std::pair<float, Avatar*>& r) { return l.first < r.first; });
    for (const auto& av_entry : av_order)
    {
        Avatar& a = *av_entry.second;
        for (size_t k = 0; k < a.mDraws.size();)
        {
            if (av_tag > a.mDraws[k].mNotedTag + 60)
            {
                if (a.mDraws[k].mOut.valid())
                {
                    ctx.arenaFree(a.mDraws[k].mOut);
                }
                a.mDraws.erase(a.mDraws.begin() + k);
                a.mSig.clear();                     // the geometry indices of the structure are void
                a.mBuiltIdx.clear();
            }
            else
            {
                ++k;
            }
        }
        Blas& b = *a.mBlas;
        std::vector<SkinDraw*> cur;
        U64 total_verts = 0, chunks_needed = 0;
        for (SkinDraw& d : a.mDraws)
        {
            if (d.mNotedTag != av_tag)
            {
                continue;
            }
            // the ranges must lie inside the buffers as they are now
            const U64 weight_stride = d.mMode == 0 ? 16 : (d.mMode == 1 ? 4 : 0);
            if (!d.mVi.mAddress || !d.mIi.mAddress || ((U64)d.mVertexFirst + d.mVertexCount) * 16 > d.mVi.mSize
                || (weight_stride && (U64)d.mWeightOffset + ((U64)d.mVertexFirst + d.mVertexCount) * weight_stride > d.mVi.mSize) || ((U64)d.mFirst + d.mCount) * 2 > d.mIi.mSize)
            {
                ++sStats.mNoData;
                continue;
            }
            cur.push_back(&d);
            total_verts += d.mVertexCount;
            chunks_needed += (d.mVertexCount + kSkinChunkVerts - 1) / kSkinChunkVerts;
        }
        if (cur.empty())
        {
            continue;
        }
        if (total_verts > skin_verts_left)
        {
            ++sStats.mSkinOver;
            continue;
        }
        if (jobs.size() >= max_builds || skin_jobs.size() + cur.size() > kMaxSkinJobs || skin_chunks.size() + chunks_needed > kMaxSkinChunks)
        {
            ++sStats.mDeferred;
            continue;
        }
        if (!ensure_skin_pipeline())
        {
            continue;
        }
        // what the structure covers: a change in the draws (or in the index data they use) means a new structure, otherwise it is refitted over the new pose
        std::vector<U64> sig;
        for (SkinDraw* d : cur)
        {
            U64 h = 1469598103934665603ull;
            for (U64 v : { (U64)d->mVb, (U64)d->mIb, (U64)d->mFirst, (U64)d->mCount, (U64)d->mVertexFirst, (U64)d->mVertexCount, (U64)d->mSkinId, (U64)d->mKind, (U64)d->mMode, d->mIi.mEpoch, d->mIi.mVersion, d->mVi.mEpoch })
            {
                h = (h ^ v) * 1099511628211ull;
            }
            sig.push_back(h);
        }
        bool need_build = b.mAs == VK_NULL_HANDLE || sig != a.mSig || b.mRefits >= skin_refit_limit || sDbg.mNoRefit;
        bool alloc_failed = false;
        for (SkinDraw* d : cur)
        {
            const VkDeviceSize bytes = ((VkDeviceSize)d->mVertexCount * 16 + 255) / 256 * 256;
            if (!d->mOut.valid() || d->mOut.mSize < bytes)
            {
                if (d->mOut.valid())
                {
                    ctx.arenaFree(d->mOut);
                    d->mOut = ArenaRef();
                }
                if (!ctx.arenaAlloc(bytes, 256, ArenaKind::Device, d->mOut))
                {
                    ++sStats.mSkinFailed;
                    alloc_failed = true;
                    break;
                }
                need_build = true;
            }
        }
        if (alloc_failed)
        {
            continue;
        }
        Job job;
        job.mFresh = b.mAs == VK_NULL_HANDLE;
        job.mKey = a.mKey;
        job.mBlas = &b;
        job.mUpdate = !need_build;
        U64 prims = 0;
        for (SkinDraw* d : cur)
        {
            LLVkGL::markBufferUsed(d->mVb);         // the compute pass and the build read them when the GPU gets here
            LLVkGL::markBufferUsed(d->mIb);
            if (d->mSkinnedTag != av_tag)
            {
                d->mSkinnedTag = av_tag;
                const U32 ji = (U32)skin_jobs.size();
                SkinJob sj;
                memset(&sj, 0, sizeof(sj));
                const U64 src = d->mVi.mAddress, weights = d->mMode == 2 ? 0ull : d->mVi.mAddress + d->mWeightOffset, dst = d->mOut.mAddress - 16ull * d->mVertexFirst;
                sj.mSrc[0] = (U32)src;
                sj.mSrc[1] = (U32)(src >> 32);
                sj.mWeights[0] = (U32)weights;
                sj.mWeights[1] = (U32)(weights >> 32);
                sj.mPalette[0] = (U32)d->mPalette;
                sj.mPalette[1] = (U32)(d->mPalette >> 32);
                sj.mDst[0] = (U32)dst;
                sj.mDst[1] = (U32)(dst >> 32);
                sj.mFirst = d->mVertexFirst;
                sj.mCount = d->mVertexCount;
                sj.mMode = d->mMode;
                sj.mJoints = d->mJoints;
                const F32* m = d->mHasModel ? d->mModel : kIdentity;
                for (int row = 0; row < 3; ++row)
                {
                    for (int col = 0; col < 4; ++col)
                    {
                        sj.mModel[row * 4 + col] = m[col * 4 + row];
                    }
                }
                skin_jobs.push_back(sj);
                skin_job_draws.push_back(d);
                for (U32 off = 0; off < d->mVertexCount; off += kSkinChunkVerts)
                {
                    skin_chunks.push_back({ ji, off });
                }
            }
            VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geo.flags = d->mKind == 1 ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
            VkAccelerationStructureGeometryTrianglesDataKHR& t = geo.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = d->mOut.mAddress - 16ull * d->mVertexFirst;       // indices are absolute: the copy is indexed relative to its first vertex
            t.vertexStride = 16;
            t.maxVertex = d->mVertexFirst + d->mVertexCount - 1;
            t.indexType = VK_INDEX_TYPE_UINT16;
            t.indexData.deviceAddress = d->mIi.mAddress + 2ull * d->mFirst;
            job.mGeometries.push_back(geo);
            VkAccelerationStructureBuildRangeInfoKHR range = {};
            range.primitiveCount = d->mCount / 3;
            job.mRanges.push_back(range);
            job.mCounts.push_back(range.primitiveCount);
            prims += range.primitiveCount;
        }
        job.mPrims = (U32)prims;
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
        skin_verts_left -= total_verts;
        a.mSig = sig;
        a.mBuiltIdx.clear();
        for (SkinDraw* d : cur)
        {
            a.mBuiltIdx.push_back((U32)(d - a.mDraws.data()));
        }
        a.mProcessedTag = av_tag;
        a.mAddressNow = job.mUpdate ? b.mAddress : job.mNewAddress;
        b.mLastSeen = sSceneCalls;
        ++sStats.mAvatars;
        jobs.push_back(std::move(job));
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
                memcpy(rec.mUv, a.mUv, sizeof(rec.mUv));
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

    auto slot_lookup = [&](U32 name) -> U32
    {
        // many geometries share textures: a texture's slot is asked for once per frame
        const U32 h = (name * 2654435761u) >> 21;
        if (sSlotCacheTag != frame_now + 1 || sSlotCacheName[h] != name)
        {
            if (sSlotCacheTag != frame_now + 1)
            {
                memset(sSlotCacheName, 0, sizeof(sSlotCacheName));
                sSlotCacheTag = frame_now + 1;
            }
            sSlotCacheName[h] = name;
            sSlotCacheSlot[h] = slot_of ? (slot_of(name) & 0xFFFFu) : 0u;
        }
        return sSlotCacheSlot[h];
    };
    for (auto& kv : sAvatars)
    {
        Avatar& a = kv.second;
        if (!a.mAddressNow || av_tag > a.mNotedTag + keep || !sDbg.mRigged || a.mBuiltIdx.empty())
        {
            continue;
        }
        if (instances.size() >= kInstanceCapacity)
        {
            ++sStats.mOverCap;
            break;
        }
        // masked geometries need a record each (the shader finds it by the geometry's place in the structure): what the alpha test reads, at the buffers' addresses of now
        bool any_alpha = false;
        for (U32 idx : a.mBuiltIdx)
        {
            any_alpha = any_alpha || (idx < a.mDraws.size() && a.mDraws[idx].mKind == 1);
        }
        U32 custom_index = 0;
        if (any_alpha)
        {
            if (records + a.mBuiltIdx.size() > kMaxRecords)
            {
                ++sStats.mTableFull;
                continue;
            }
            std::vector<LLVkGL::BufferInfo> vis(a.mBuiltIdx.size()), iis(a.mBuiltIdx.size());
            bool ok = true;
            for (size_t k = 0; k < a.mBuiltIdx.size() && ok; ++k)
            {
                const SkinDraw& d = a.mDraws[a.mBuiltIdx[k]];
                if (d.mKind != 1)
                {
                    continue;
                }
                if (d.mNotedTag == av_tag)
                {
                    vis[k] = d.mVi;
                    iis[k] = d.mIi;
                }
                else
                {
                    ok = LLVkGL::getBufferInfo(d.mVb, vis[k]) && LLVkGL::getBufferInfo(d.mIb, iis[k]) && vis[k].mEpoch == d.mVi.mEpoch && iis[k].mEpoch == d.mIi.mEpoch;
                }
                ok = ok && vis[k].mAddress && iis[k].mAddress;
            }
            if (!ok)
            {
                ++sStats.mNoData;
                continue;
            }
            custom_index = records;
            for (size_t k = 0; k < a.mBuiltIdx.size(); ++k)
            {
                const SkinDraw& d = a.mDraws[a.mBuiltIdx[k]];
                GpuGeom& rec = table[records++];
                memset(&rec, 0, sizeof(rec));
                if (d.mKind != 1)
                {
                    continue;                       // an opaque geometry: its candidates never reach the alpha test
                }
                LLVkGL::markBufferUsed(d.mVb);      // the alpha test reads texcoords, colours and indices while the lighting runs
                LLVkGL::markBufferUsed(d.mIb);
                const U64 pa = d.mOut.mAddress - 16ull * d.mVertexFirst, ia = iis[k].mAddress, va = vis[k].mAddress;
                rec.mPositions[0] = (U32)pa;
                rec.mPositions[1] = (U32)(pa >> 32);
                rec.mIndices[0] = (U32)ia;
                rec.mIndices[1] = (U32)(ia >> 32);
                if (d.mAlpha.mFlags & 8u)
                {
                    const U64 t = va + d.mAlpha.mTexcoordOffset;
                    rec.mTexcoords[0] = (U32)t;
                    rec.mTexcoords[1] = (U32)(t >> 32);
                }
                if (d.mAlpha.mFlags & 16u)
                {
                    const U64 c = va + d.mAlpha.mColorOffset;
                    rec.mColors[0] = (U32)c;
                    rec.mColors[1] = (U32)(c >> 32);
                }
                U32 slot[4] = {};
                for (U32 t = 0; t < 4; ++t)
                {
                    if (d.mAlpha.mTextures[t])
                    {
                        slot[t] = slot_lookup(d.mAlpha.mTextures[t]);
                    }
                }
                rec.mSlots[0] = slot[0] | (slot[1] << 16);
                rec.mSlots[1] = slot[2] | (slot[3] << 16);
                rec.mFlags = d.mAlpha.mFlags;
                rec.mCutoff = d.mAlpha.mCutoff;
                rec.mFirst = d.mFirst;
                memcpy(rec.mUv, d.mAlpha.mUv, sizeof(rec.mUv));
            }
            ++sStats.mAlphaStructs;
        }
        VkAccelerationStructureInstanceKHR inst = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                inst.transform.matrix[row][col] = view[col * 4 + row];      // the skinned vertices are in world space: the instance is just the view
            }
        }
        inst.instanceCustomIndex = custom_index;
        inst.mask = 1u | (any_alpha ? 2u : 0u) | 4u;        // opaque (bit 0) and masked (bit 1) casters, what the shadow rays trace; bit 2 marks avatars (mode 4 shows only these)
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = a.mAddressNow;
        instances.push_back(inst);
    }

    lap(3, t_lap);
    // ---- 3. record: builds, then the top-level structure -----------------------------------------------------
    VkAccelerationStructureBuildSizesInfoKHR tlas_size;
    tlas_sizes(tlas.mCapacity, tlas_size);
    const VkDeviceSize scratch_needed = std::max(scratch_total, tlas_size.buildScratchSize) + 2 * align;
    auto undo_jobs = [&]() {
        for (auto& kv : sAvatars)
        {
            Avatar& a = kv.second;
            if (a.mProcessedTag == av_tag)
            {
                a.mProcessedTag = 0;
                a.mAddressNow = a.mBlas ? a.mBlas->mAddress : 0;
                a.mSig.clear();
                for (SkinDraw& d : a.mDraws)
                {
                    d.mSkinnedTag = 0;
                }
            }
        }
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
    VkDeviceAddress skin_jobs_address = 0, skin_chunks_address = 0;
    if (!skin_chunks.empty())
    {
        U8* base = (U8*)tlas.mSkin.mMapped + (size_t)call * kSkinBytes;
        memcpy(base, skin_jobs.data(), sizeof(SkinJob) * skin_jobs.size());
        memcpy(base + kSkinJobBytes, skin_chunks.data(), sizeof(SkinChunk) * skin_chunks.size());
        skin_jobs_address = tlas.mSkin.mAddress + (VkDeviceAddress)call * kSkinBytes;
        skin_chunks_address = skin_jobs_address + kSkinJobBytes;
    }
    static U64 last_probe_call = 0;
    if (sDbg.mSkinProbe && !skin_chunks.empty() && sSceneCalls >= last_probe_call + 90 && ensure_skin_pipeline())
    {
        last_probe_call = sSceneCalls;
        // Debug: the same dispatch again, on its own, then the first few skinned ranges copied to host memory (the outputs do not change: the jobs are idempotent).
        static Buffer probe_buffer;
        if (!probe_buffer.valid())
        {
            ctx.createBuffer(8u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Context::Memory::Readback, probe_buffer);
        }
        struct Target { U32 mJob; VkDeviceSize mOffset, mBytes; };
        std::vector<Target> targets;
        VkDeviceSize used = 0;
        std::unordered_map<U32, U32> per_avatar;
        for (U32 ji = 0; ji < skin_job_draws.size() && targets.size() < 12; ++ji)
        {
            const SkinDraw* d = skin_job_draws[ji];
            if (!d || !d->mOut.valid() || per_avatar[d->mAvatarPos[0] != 0.f ? (U32)d->mSkinId : 0u] >= 2)
            {
                continue;
            }
            const VkDeviceSize bytes = (VkDeviceSize)d->mVertexCount * 16;
            if (used + bytes > probe_buffer.mSize)
            {
                continue;
            }
            ++per_avatar[d->mAvatarPos[0] != 0.f ? (U32)d->mSkinId : 0u];
            targets.push_back({ ji, used, bytes });
            used += (bytes + 15) / 16 * 16;
        }
        if (probe_buffer.valid() && !targets.empty())
        {
            const VkDeviceAddress pj = skin_jobs_address, pc = skin_chunks_address;
            const bool ok = ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sSkinPipe.mPipe);
                const U32 push[4] = { (U32)pj, (U32)(pj >> 32), (U32)pc, (U32)(pc >> 32) };
                vkCmdPushConstants(cmd, sSkinPipe.mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
                vkCmdDispatch(cmd, (U32)skin_chunks.size(), 1, 1);
                full_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                for (const Target& t : targets)
                {
                    const SkinDraw* d = skin_job_draws[t.mJob];
                    VkBufferCopy region = { d->mOut.mOffset, t.mOffset, t.mBytes };
                    vkCmdCopyBuffer(cmd, d->mOut.mBuffer, probe_buffer.mBuffer, 1, &region);
                }
                full_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            });
            if (ok)
            {
                for (const Target& t : targets)
                {
                    const SkinDraw* d = skin_job_draws[t.mJob];
                    const F32* v = (const F32*)((const U8*)probe_buffer.mMapped + t.mOffset);
                    F32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                    U32 bad = 0;
                    for (U32 k = 0; k < d->mVertexCount; ++k)
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            const F32 c = v[4 * (size_t)k + a];
                            if (!(c > -1e6f && c < 1e6f))
                            {
                                ++bad;
                                continue;
                            }
                            lo[a] = std::min(lo[a], c);
                            hi[a] = std::max(hi[a], c);
                        }
                    }
                    LL_INFOS("VkRt") << "skin probe: avatar " << d->mSkinId << " mode " << d->mMode << " kind " << d->mKind << " vertices " << d->mVertexCount << " (from " << d->mVertexFirst
                                     << ") bounds (" << lo[0] << "," << lo[1] << "," << lo[2] << ")-(" << hi[0] << "," << hi[1] << "," << hi[2] << ") bad " << bad << ", avatar at ("
                                     << d->mAvatarPos[0] << "," << d->mAvatarPos[1] << "," << d->mAvatarPos[2] << "), first vertex (" << v[0] << "," << v[1] << "," << v[2] << ")" << LL_ENDL;
                }
            }
        }
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
        full_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        if (!skin_chunks.empty())
        {
            // the compute pass writes the vertices the builds below read (and the alpha test reads later): one dispatch, every job of the call
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sSkinPipe.mPipe);
            const U32 push[4] = { (U32)skin_jobs_address, (U32)(skin_jobs_address >> 32), (U32)skin_chunks_address, (U32)(skin_chunks_address >> 32) };
            vkCmdPushConstants(cmd, sSkinPipe.mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            vkCmdDispatch(cmd, (U32)skin_chunks.size(), 1, 1);
            full_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        }
        ctx.gpuStamp(Context::stampTag("rt scene builds"));
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
    for (const SkinJob& sj : skin_jobs)
    {
        ++sStats.mSkinJobs;
        sStats.mSkinVerts += sj.mCount;
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
        static FILE* log = fopen("D:/claude/rt-calls.log", "ab");
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
    char buf[1600];
    snprintf(buf, sizeof(buf),
             "rt scene (mode %u): %u structures (%.0f MB, %.2f M triangles), instances %.0f/scene (last %u), casters named %.0f/frame, builds %.2f/frame (%.0f tri), refits %.2f/frame (%.0f tri), "
             "deferred %.2f, no data %.2f, masked structures %.0f/frame, table full %.2f, over instance cap %.2f, failed %.3f, evicted %.3f, scene calls %.2f/frame (over %u: %.2f), "
             "CPU %.3f ms/call (worst %.2f ms; expire %.2f, order %.2f, scan+sort %.2f, instances %.2f, record %.2f), scratch %.1f MB; builds because: new %.2f, names recycled %.2f, ranges grew %.2f, index data rewritten %.2f, refit limit %.2f; skin: %.1f jobs/frame, %.0f vertices/frame, avatars %.2f/frame (over budget %.2f), failed %.3f; casters new %.2f/frame, groups held %zu; checks: structures checked %.2f/frame, bad vertex data %.3f, bad matrices %.3f, singular matrices %.3f, copy vs video memory compared %.2f mismatched %.2f, largest instance scale %.2f (vb %u ib %u matrix id %u kind %u)",
             shadowMode(), live, (double)sBlasStorageBytes / 1048576.0, (double)prims / 1e6, (double)s.mInstances / scenes, sLastInstances, (double)s.mCasters / f, (double)s.mBuilds / f,
             (double)s.mPrimsBuilt / f, (double)s.mRefits / f, (double)s.mPrimsRefit / f, (double)s.mDeferred / f, (double)s.mNoData / f, (double)s.mAlphaStructs / f, (double)s.mTableFull / f,
             (double)s.mOverCap / f, (double)s.mFailed / f, (double)s.mEvicted / f, (double)s.mScenes / f, kCalls, (double)s.mExtraCalls / f, s.mCpuMs / scenes, s.mCpuMsMax, s.mPhaseMs[0] / scenes, s.mPhaseMs[1] / scenes, s.mPhaseMs[2] / scenes, s.mPhaseMs[3] / scenes, s.mPhaseMs[4] / scenes,
             (double)sScratch.mSize / 1048576.0, (double)s.mWhyNew / f, (double)s.mWhyEpoch / f, (double)s.mWhyGrew / f, (double)s.mWhyIb / f, (double)s.mWhyRefitLimit / f,
             (double)s.mSkinJobs / f, (double)s.mSkinVerts / f, (double)s.mAvatars / f, (double)s.mSkinOver / f, (double)s.mSkinFailed / f, (double)s.mNewGroups / f, sGroups.size(), (double)s.mChecked / f, (double)s.mBadVertices / f, (double)s.mBadMatrices / f, (double)s.mSingular / f, (double)s.mCompared / f, (double)s.mMismatched / f, s.mMaxScale, s.mMaxScaleKey.mVb, s.mMaxScaleKey.mIb,
             s.mMaxScaleKey.mMatrixId, s.mMaxScaleKey.mKind);
    return buf;
}

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
