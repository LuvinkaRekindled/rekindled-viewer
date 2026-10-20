/**
 * @file llvkrt.cpp
 * @brief Ray tracing scene: bottom-level structures per vertex/index buffer pair, a top-level structure per frame (see llvkrt.h and doc/vulkan/PLAN-rt-shadows.md)
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkrt.h"
#include <xmmintrin.h>
#include "llvkrenderer.h"
#include "llvkmirror.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
constexpr VkDeviceSize kGeomBytesBase = 2u << 20;           // per call: the geometry records of the alpha masked structures
constexpr VkDeviceSize kGeomBytesMats = 4u << 20;           // the same with the material tables on (LL_VK_RT_MATERIALS=1): a record for every geometry
constexpr U32 kMatCap = 16384;                              // rows of the material table (the viewer's registry has the same capacity)
constexpr VkDeviceSize kMatBytes = 96ull * kMatCap;
constexpr U32 kPinCap = 32768;                              // pin indices (a pin = a texture with a permanent bindless slot, see LLVkWorker::rtPinAcquire)
constexpr VkDeviceSize kPinBytes = 4ull * kPinCap;          // the pin table of a call's region: slot by pin index

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
    U32 mMaterial;          // row of the material table (0 = none)
    F32 mUv[6];             // texture coordinate transform (uv' = uv0 * u + uv1 * v + uv2)
    U32 mNormals[2];        // address of the normal array (flag kRecNormals); record 0 (the header) carries the address of the material table of the call here
};
static_assert(sizeof(GpuGeom) == 88, "geometry record");
constexpr U32 kRecNormals = 1024;                           // record flag: mNormals is valid
constexpr U32 kRecOpaque = 2048;                            // record flag: a record of an opaque structure (no alpha test data)
constexpr U32 kRecPinned = 4096;                            // record flag: mSlots hold PIN indices (16 bit each), the shader looks the slots up in the pin table
constexpr U32 kRecSkinned = 8192;                           // record flag: a skinned avatar draw (its normals are skinned smooth normals: the shader reads no normal map frame for it)

// one material as the shaders read it (LLRtMaterial in llvkshader.cpp): the viewer's RtMaterialDef with the texture names turned into slots
struct GpuMaterial
{
    U32 mSlots[2];          // four 16 bit slots: the diffuse / base colour textures
    U32 mSlots2[2];         // 16 bit slots: normal map, specular or metallic-roughness map, emissive map, unused
    F32 mColor[4];
    F32 mEmissive[3];
    F32 mCutoff;
    F32 mMetallic, mRoughness;
    U32 mFlags;
    U32 mPad0;
    F32 mUv[6];
    U32 mPad1[2];
};
static_assert(sizeof(GpuMaterial) == 96, "material row");

bool sMatsInit = false;                                     // LL_VK_RT_MATERIALS=1 at start-up: bigger tables, records for every geometry (the scene side can still be switched off live)
VkDeviceSize sGeomBytes = kGeomBytesBase, sRegionBytes = kGeomBytesBase;
U32 sMaxRecords = 0, sTableCalls = kCalls;
constexpr F32 kSkinReach = 300.f;                            // metres from the avatar's position: skinned vertices farther away are garbage and are left out
constexpr size_t kAvatarBuilds = 16;                        // structure builds of avatars per scene call beyond the groups' limit (the jobs list is reserved for both)
constexpr U32 kMaxSkinJobs = 2048;                          // compute skinning jobs per scene call
constexpr U32 kSkinChunkVerts = 128;                        // vertices per workgroup (one chunk table entry)
constexpr U32 kMaxSkinChunks = 131072;
constexpr VkDeviceSize kSkinJobBytes = 128ull * kMaxSkinJobs;
constexpr VkDeviceSize kSkinBytes = kSkinJobBytes + 8ull * kMaxSkinChunks;
constexpr U32 kPasses = 20;                                 // passes tracked: 0-3 sun cascades, 4-5 spot shadows, 6 other (the main view), 7-12 the six faces of the hero probe, 13-18 the faces of a reflection probe

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
    U32 mId = ~0u;                      // its row in sSeenRows (the per-pass note stamps live there, contiguous)
    bool mPending = false;              // in sPending: no built structure yet (or dropped), looked at at every scene call
    U64 mCandTag = 0;                   // the scene call tag it was last considered in
    bool mSkip = false;                 // left out of this scene call
    Blas* mBlas = nullptr;              // its structure (sBlas nodes keep their place)
    LLVkWorker::BufferInfo mVi, mIi;        // the buffers as of the latest note
    U64 mNotedTag = 0;                  // frame tag of the latest note
    F32 mPrevModel[16] = {};            // the matrix of the note before
    bool mMoving = false;               // the matrix changed between the last two notes
    bool mDynamic = false;              // its vertex data was rewritten since the structure was last built or refitted (this call)
    float mDistance = 0.f;              // eye space distance of the object's origin (this call)
    // the scene cache (sInstArray, sRecs): where this group's instance and alpha records are, and what they were made from
    U32 mSlot = ~0u;                    // index of its instance in sInstArray (~0: none)
    U32 mRecFirst = 0, mRecCount = 0;   // its rows in sRecs
    bool mDirty = false;                // in sDirtyKeys: instance and records are made again at the next scene call
    U64 mCacheVb = 0, mCacheIb = 0;     // the buffer addresses its records were made with
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
    bool mFastBuild = false;                        // avatar structures: built with PREFER_FAST_BUILD (the flags of an update must match the build's)
    bool mCompacted = false;                        // replaced by its compacted copy: not refitted any more (a change builds a new structure)
    U64 mLastSeen = 0;                              // scene call number
    U64 mJobCall = 0;                               // the scene call a job for this structure belongs to (mJob is valid only then)
    Job* mJob = nullptr;
};

struct Tlas
{
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    Buffer mStorage;
    Buffer mInstances;                              // kCalls regions of mCapacity instances
    Buffer mTable;                                  // sTableCalls regions of sRegionBytes: the geometry records, then (material tables on) the material rows
    Buffer mSkin;                                   // kCalls regions of kSkinBytes: the compute skinning job and chunk tables
    U32 mCapacity = 0;
    VkDescriptorSet mSet = VK_NULL_HANDLE;
    U64 mCallFrame = ~0ull;                         // frame number of the calls counted in mCalls
    U32 mCalls = 0;
    U64 mBuiltFrame = ~0ull;                        // frame number in whose command buffer its latest build was recorded
};

struct Stats
{
    U64 mDeadRefs = 0;                   // instances left out because they named a structure that was given up
    U64 mCeilingHeld = 0;                // new structures held back by the acceleration structure memory ceiling
    U64 mScenes = 0, mBuilds = 0, mRefits = 0, mPrimsBuilt = 0, mPrimsRefit = 0, mDeferred = 0, mNoData = 0, mAlphaStructs = 0, mTableFull = 0, mFailed = 0, mEvicted = 0, mInstances = 0, mCasters = 0;
    U64 mSkinJobs = 0, mSkinVerts = 0, mSkinFailed = 0, mAvatars = 0, mSkinOver = 0, mSkinThrottled = 0;
    U64 mAvatarInstances = 0, mAvatarStale = 0, mAvatarStaleFrames = 0, mAvatarStaleMax = 0;
    U64 mOverCap = 0, mExtraCalls = 0, mBadVertices = 0, mBadMatrices = 0, mChecked = 0, mCheckUs = 0, mSingular = 0, mCompared = 0, mMismatched = 0;
    F32 mMaxScale = 0.f;
    Key mMaxScaleKey;
    U64 mWhyNew = 0, mWhyEpoch = 0, mWhyGrew = 0, mWhyIb = 0, mWhyRefitLimit = 0, mNewGroups = 0, mNewMasked = 0;      // why structures were built (and how many casters were new)
    double mCpuMs = 0, mCpuMsMax = 0;
    U64 mSlotMisses = 0;            // texture slot lookups that went to the renderer
    U64 mDirtyWhy[7] = {};          // marked dirty because: moved, buffer address, alpha info, new range, buffer move check, structure change (3 places)
    double mSubMs[6] = {};          // inside "instances": cache refresh, verify, instance copy, records table, avatars
    double mPhaseMs[6] = {};        // expire, order, scan (with sort), instances, recording, the rest
    U64 mDropped[2][8] = {};         // instances taken out of the scene while they were in it, by kind (0 opaque, 1 masked) and reason: expired, switched off, no structure, bad matrix, singular matrix, no buffer data, records table full, other
    U64 mMaskedMin = ~0ull, mMaskedMax = 0;     // live alpha records per scene call over the report interval (a dip shows leaves leaving the scene)
    U64 mProbeCount = 0, mProbeNow = 0, mProbeNoUpdate = 0, mProbeNoUpdateFast = 0, mProbeFast = 0;     // size probe: bytes the new group structures need with the flags used / without ALLOW_UPDATE / without it and with PREFER_FAST_BUILD / with PREFER_FAST_BUILD
    U64 mCompacted = 0, mCompactBefore = 0, mCompactAfter = 0, mCompactSkipped = 0, mCompactFailed = 0;     // compaction: structures replaced, bytes before and after
    U64 mScanChecks = 0, mScanMismatch = 0;       // the candidate lists against a full walk (verify)
    U64 mForceWhy[5] = {};          // the whole cache was made again because: eviction, camera too far from the origin, alpha switch, material switch, compaction
    double mTblMs[4] = {};          // inside the table part: record slot patch, record write, material slot patch, material write
    U64 mPinsHeld = 0, mPinsDropped = 0, mPinWrites = 0;   // pins taken and given back by the rows, pin table entries written to the regions
    U64 mRecRowsWritten = 0, mMatRowsWritten = 0, mMatUpdates = 0, mTableMismatch = 0, mRecNoMaterial = 0, mRecMaterial = 0;     // the table writes (deltas) and the material tables
    U64 mRefreshed = 0, mFullRebuilds = 0, mVerifies = 0, mVerifyGroups = 0, mVerifyMismatch = 0, mTopologyChecks = 0;      // the scene cache (see sInstArray)
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
    bool mDynCasters = true;        // include casters with a matrix of their own (avatar attachments, moving linksets); they flashed before the preflush, fine since (see PLAN-rt-shadows.md)
    bool mCallLog = false;          // one line per scene call into <work>/rt-calls.log (wall clock, frame, call, jobs, instances)
    bool mVerify = false;           // every 120 scene calls: make the whole cached scene again from the groups and compare it with the one kept up to date (log line "scene cache check")
    U32 mSkinPeriodMax = 4;         // avatars farther than LL_VK_RT_SKIN_NEAR (8 m) are refreshed (skinned and refitted) every 2..this many viewer frames, by distance (1 = every frame for all)
    bool mPreflush = true;          // before the builds and the skinning: record the queued writes of video-memory buffers (they are only recorded when a draw reads the buffer, which a build does not)
    bool mAvatarLog = false;
    bool mAvatarKeepOff = false;       // live (avatarkeep=1): an avatar structure is made only from the draws named in this very frame (the old behaviour: the body vanished from the traced reflection on the frames without the naming pass)        // one line per avatar instance of every scene call into <work>/rt-avatars.log: how old its data is, why it was not refreshed
    bool mInstDump = false;            // live (instdump=1): the instance list (structure address, translation, mask) of the next 40 scene calls goes into <work>/rt-instances.log; 0 re-arms it
    U64 mGroupHold = 0;                // live (grouphold=N), TEST BRIDGE (off: it leaves ghosts behind moving objects): frames a caster group seen by a pass stays in the scene when that pass rendered without naming it (0: only the casters named in the frame stay). The proper fix is a pass id per hero probe face
    bool mSkinProbe = false;        // about every 90 scene calls: run the skin jobs again on their own, read a few results back and log their bounding boxes
    bool mRigged = true;            // rigged opaque casters (avatar attachments) in the scene, skinned by the compute pass
    bool mSizeProbe = false;        // log what the new group structures would need with other build flags (no behaviour change)
    bool mCompact = false;          // static structures are compacted: built with ALLOW_COMPACTION, the size asked for, copied into a smaller allocation a few frames later
    F32 mSkinMaxDist = 0.f;         // C4: avatars farther than this many metres (from the eye) get no new pose or structure (0 = no limit); one that was built keeps its last pose
    bool mSkinFast = false;         // C4: avatar structures are built with PREFER_FAST_BUILD instead of PREFER_FAST_TRACE (cheaper builds and refits, slower rays)
    bool mSkinTest = false;         // stage C step C1: structures of opaque casters are built from a compute-pass copy of their vertices (must look identical to the direct path)
    U32 mPfA = 14, mPfB = 6;        // slot lookups: how many materials ahead the texture state and the table entries are prefetched (0 = no prefetch)
    bool mMats = false;             // records for the opaque geometries too, the material table is filled and traced against (needs LL_VK_RT_MATERIALS=1 at start-up)
    bool mDelta = true;             // the tables are written to the call's region as deltas (rows changed since the region was last written); off: all rows every call
    bool mTableVerify = false;      // after every table write compare the region with the kept rows (slow: reads video memory)
    S32 mGlossyRt = 0;              // live (glossyrt=N, 0 = off by default until it has been seen working; needs LL_VK_RT_MATERIALS=1): 1 ray traced reflections on glossy surfaces of the lighting pass (gloss 0.9 and up), 2 PBR path pixels magenta, 3 / 4 where it traced: hits green, misses red (4: every gloss), 5 gloss as grey, 6 which lighting path a pixel takes
    S32 mHitRays = -1;              // live (hitrays=N, -1 = not set): shadow rays per local light at a traced hit (1, 2 or 4)
    S32 mHitQ = -1;                 // live (hitq=N, -1 = not set: the preference FSVkMirrorQuality decides): how a hit of the traced glossy mirror is lit, 0 flat diffuse, 1 the main pass lighting at the hit (normal map, roughness / metallic / specular map, probes at the hit)
    S32 mWaterRt = 0;               // live (waterrt=N, 0 by default: the preference FSVkRTWater asks for 1; needs LL_VK_RT_MATERIALS=1): 0 off, 1 ray traced reflections on the water (rtReflection in shadowUtil.glsl), 2 water painted magenta where the code runs, 3 hits green / misses red / downward rays blue
    S32 mView = 0;                  // live (view=N): the ray scene material debug view mode (RenderRTMaterialDebug) the viewer shows, 0 = the setting decides
    bool mGate = true;              // live (gate=0/1): avatars still loading and masked casters without texture data are not named (the viewer asks through LLVkBridge::rtLoadingGates)
    bool mBodyName = true;          // live (bodyname=0/1): the viewer names the body meshes of the last frames again before the scene call (LLVkBridge::rtBodyRename)
    F32 mSkinReach = kSkinReach;    // live (skinreach=N metres): skinned vertices farther than this from their avatar are dropped (the guard of the skinning pass)
    bool mBBox = false;             // one line per build/refit of a structure with a matrix of its own (CPU-visible data only) into <work>/rt-bbox.log: bounding box, largest coordinate (stage C step 0)
};
DebugSwitches sDbg;
std::atomic<S32> sViewLive{0};
std::atomic<S32> sWaterRtLive{0}, sGlossyRtLive{0}, sHitQLive{-1}, sHitRaysLive{-1};
std::atomic<bool> sGateLive{true}, sBodyNameLive{true};     // sDbg.mGate / mBodyName for the main thread (the viewer asks through LLVkBridge)
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
    d.mCheck = env_u64("LL_VK_RT_CHECK", 1) != 0;       // on by default: measured 1-2 microseconds a frame in the mirror room (2026-10-13), and it keeps NaN vertices and indices past the buffer out of the structures
    d.mSkipBad = env_u64("LL_VK_RT_SKIPBAD", 1) != 0;
    d.mDynCasters = env_u64("LL_VK_RT_DYNAMIC", 1) != 0;
    d.mSkinTest = env_u64("LL_VK_RT_SKINTEST", 0) != 0;
    d.mRigged = env_u64("LL_VK_RT_RIGGED", 1) != 0;
    d.mSkinPeriodMax = (U32)std::max<U64>(1, env_u64("LL_VK_RT_SKIN_PERIOD_MAX", 4));
    d.mCompact = env_u64("LL_VK_RT_COMPACT", 0) != 0;       // off by default: saved only about 5 % of the memory here and the tree leaf shadows disappeared with it on (PLAN-rt-shadows.md)
    d.mMats = sMatsInit;
    d.mSkinFast = env_u64("LL_VK_RT_SKIN_FAST", 1) != 0;       // on by default: builds and refits of the avatar structures 38 % cheaper, traces unchanged (measured 2026-10-12)
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
                else if (k == "skinfast") d.mSkinFast = v != 0;
                else if (k == "view") d.mView = (S32)v;
                else if (k == "waterrt") d.mWaterRt = (S32)v;
                else if (k == "glossyrt") d.mGlossyRt = (S32)v;
                else if (k == "hitq") d.mHitQ = (S32)v;
                else if (k == "hitrays") d.mHitRays = (S32)v;
                else if (k == "gate") d.mGate = v != 0;
                else if (k == "bodyname") d.mBodyName = v != 0;
                else if (k == "skinreach") d.mSkinReach = (F32)std::max<long long>(1, v);
                else if (k == "skindist") d.mSkinMaxDist = (F32)v;
                else if (k == "mats") d.mMats = sMatsInit && v != 0;
                else if (k == "pfa") d.mPfA = (U32)std::max<long long>(0, v);
                else if (k == "pfb") d.mPfB = (U32)std::max<long long>(0, v);
                else if (k == "delta") d.mDelta = v != 0;
                else if (k == "tableverify") d.mTableVerify = v != 0;
                else if (k == "compact") d.mCompact = v != 0;
                else if (k == "sizeprobe") d.mSizeProbe = v != 0;
                else if (k == "rigged") d.mRigged = v != 0;
                else if (k == "skinprobe") d.mSkinProbe = v != 0;
                else if (k == "avlog") d.mAvatarLog = v != 0;
                else if (k == "avatarkeep") d.mAvatarKeepOff = v != 0;
                else if (k == "instdump") d.mInstDump = v != 0;
                else if (k == "grouphold") d.mGroupHold = (U64)std::max<long long>(0, v);
                else if (k == "preflush") d.mPreflush = v != 0;
                else if (k == "verify") d.mVerify = v != 0;
                else if (k == "skinperiod") d.mSkinPeriodMax = (U32)std::max<long long>(1, v);
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
    if (d.mGate != sDbg.mGate || d.mBodyName != sDbg.mBodyName || d.mSkinReach != sDbg.mSkinReach)
    {
        LL_INFOS("VkRt") << "debug switches: gate (loading avatars and textures) " << d.mGate << ", bodyname " << d.mBodyName << ", skin reach " << d.mSkinReach << " m" << LL_ENDL;
    }
    sViewLive.store(d.mView, std::memory_order_relaxed);
    // (they read the material table: with the table off live (mats=0) a trace would read material rows through a null address and lose the device)
    sWaterRtLive.store(sMatsInit && d.mMats ? d.mWaterRt : 0, std::memory_order_relaxed);
    sGlossyRtLive.store(sMatsInit && d.mMats ? d.mGlossyRt : 0, std::memory_order_relaxed);
    sHitQLive.store(d.mHitQ, std::memory_order_relaxed);
    sHitRaysLive.store(d.mHitRays, std::memory_order_relaxed);
    sGateLive.store(d.mGate, std::memory_order_relaxed);
    sBodyNameLive.store(d.mBodyName, std::memory_order_relaxed);
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
    bool mDead = false;                 // not named for a while: its output is freed (a structure built over it still lists it)
    F32 mAvatarPos[3] = {};
    AlphaInfo mAlpha;
    LLVkWorker::BufferInfo mVi, mIi;
    U64 mNotedTag = 0, mSkinnedTag = 0;
    U64 mMarkedTag = 0;                 // frame in which its buffers were marked as in use for the records (once per frame, not per scene call)
    U32 mRecCalls = 0, mRecBad = 0;     // debug (avlog): scene calls that built an alpha test / material record for this draw, and how many of them had to reject its candidates
    U32 mWhyOut = 0;                    // debug (avlog): why it was left out of the structure at the last pass: 0 in, 1 dead, 2 never named, 3 named too long ago, 4 never skinned, 5 no output, 6 keep off, 7 buffer info missing, 8 epoch changed, 9 address changed, 10 size changed, 11 bad range
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
    bool mSelf = false;                 // the viewer's own avatar
    U32 mAvatarKind = 0;                // as SkinCaster::mAvatarKind
    U32 mSlotClears = 0;                // debug (avlog): how often a new draw took the slot of a dead one (the structure is then rebuilt)
    U32 mExcl[8] = {};                  // debug (avlog): why draws were left out of the structure in the last pass: dead, never named, named too long ago, never skinned, no output, keep off, buffer changed, bad range
    U64 mProcessedTag = 0;              // frame whose jobs were made
    U64 mOverTag = 0;                   // frame in which it was named but left alone (vertex budget, job tables full, no data)
    U64 mLastVerts = 0, mLastTris = 0;  // what its structure was last made of (skinned vertices, triangles)
    U64 mLastTag = 0;
    U64 mLastBuildTag = 0;              // frame of the latest build or refit
    bool mLastWasRefit = false;
    VkDeviceAddress mAddressNow = 0;    // the structure the scene uses (a new one when it is being built this call)
};

bool sInit = false;
bool sFailed = false;
VkDescriptorPool sPool = VK_NULL_HANDLE;
Tlas sTlas[Context::FRAMES_IN_FLIGHT];
// A scene that is always empty and always consistent (an empty top-level structure, a table whose header says there is one record). A pass recorded BEFORE a frame's scene call (the avatar
// impostors are drawn first) would trace against the frame slot's structure from three command buffers ago, which is rebuilt later in the same command buffer, together with the table
// that the CPU has already rewritten for this frame: a ray that hits a masked geometry of the old structure then reads a record that belongs to something else (its buffer addresses
// are followed by the shader), and the GPU faults or hangs. Such a pass is given this scene: no occluders, everything lit.
Tlas sNullTlas;
Buffer sScratch;
// The casters of the shadow passes. Far cascades are rendered only every few frames (FSShadowFarCascadeInterval) and their shadow map is reused in between, so the casters a
// frame names are not all there is. A pass that rendered this frame contributes exactly the casters it named (a moving object must not leave copies of its earlier positions);
// the casters of a pass that did not render stay, as its cached shadow map stays, until it renders again or for a few frames at most.
std::unordered_map<Key, Group, KeyHash> sGroups;
U64 sPassFrame[kPasses] = {};                           // per pass: the frame number + 1 of its latest note
std::unordered_map<Key, Blas, KeyHash> sBlas;
// the note stamps of the groups, per pass: the frame number + 1 of the latest note (0 = never), in rows of their own so that the expiry sweep walks contiguous memory
struct SeenRow
{
    U64 mSeen[kPasses] = {};
    Group* mGroup = nullptr;                            // null: a free row
};
std::vector<SeenRow> sSeenRows;
std::vector<U32> sSeenFree;
std::unordered_multimap<U32, Key> sNameGroups;           // masked groups by the GL names of their buffers (vertex and index), for the buffer move check
std::vector<Group*> sPending;                           // groups without a built structure: they are candidates for work at every call
std::vector<Group*> sNotedList;                         // the groups first noted in frame tag sNotedListTag
U64 sNotedListTag = 0;
std::unordered_map<U32, Avatar> sAvatars;

// The static part of the scene, kept from call to call: one instance per group (transform in world space relative to sOrigin, so it does not depend on the camera) in a contiguous
// array, plus the alpha records of the masked groups (texture slots are filled in at every call), both made again only for groups that changed (sDirtyKeys).
std::vector<VkAccelerationStructureInstanceKHR> sInstArray;
std::vector<Group*> sInstGroup;                 // the group behind each instance (valid until the group is erased: drop_entry runs first)
// A table that lives in memory the GPU reads, written to the regions of the scene calls as DELTAS: the rows are kept here (the truth), every change of a row is logged, and a region
// is brought up to date by copying the rows logged since it was last written (a region that is behind the log's start, or from before a reset, gets everything). The log is trimmed
// by a reset when it gets long (every region then copies everything once).
struct RowLog
{
    std::vector<U32> mLog;
    U64 mBase = 0;                  // sequence number of mLog[0]
    U64 mEpoch = 1;
    void touch(U32 row) { mLog.push_back(row); }
    void reset() { ++mEpoch; mBase += mLog.size(); mLog.clear(); }
    U64 seq() const { return mBase + mLog.size(); }
};
struct RegionState
{
    U64 mEpoch = 0, mSeq = 0;
};
// writes the rows that changed since the region was last written; returns the number of rows copied
template <class T>
size_t apply_rows(RowLog& log, RegionState& rs, const std::vector<T>& rows, T* dst, bool delta)
{
    if (log.mLog.size() > 400000)
    {
        log.reset();
    }
    size_t copied = 0;
    if (!delta || rs.mEpoch != log.mEpoch || rs.mSeq < log.mBase || rs.mSeq > log.seq())
    {
        if (!rows.empty())
        {
            memcpy(dst, rows.data(), rows.size() * sizeof(T));
        }
        copied = rows.size();
    }
    else
    {
        for (size_t k = (size_t)(rs.mSeq - log.mBase); k < log.mLog.size(); ++k)
        {
            const U32 i = log.mLog[k];
            if (i < rows.size())
            {
                dst[i] = rows[i];
                ++copied;
            }
        }
    }
    rs.mEpoch = log.mEpoch;
    rs.mSeq = log.seq();
    return copied;
}
RowLog sRecLog, sMatLog;
RegionState sRecRegion[Context::FRAMES_IN_FLIGHT * kCalls], sMatRegion[Context::FRAMES_IN_FLIGHT * kCalls];
std::vector<GpuMaterial> sMatRows;              // by viewer id (row 0 unused); slots are patched in at every frame
std::vector<std::array<U32, 8>> sMatPins;       // pin indices of each material row's textures: 4 diffuse, normal, specular, emissive, unused (0 = none)
std::vector<U32> sPinSlots;                     // the pin table: bindless slot by pin index (written by LLVkWorker's pin code through setPinSlot)
RowLog sPinLog;
RegionState sPinRegion[Context::FRAMES_IN_FLIGHT * kCalls];
std::vector<std::array<U32, 4>> sRecPins;       // pin indices of the texture names of each record row (all zero for rows without pins)
bool sMatsSeen = false;
std::vector<GpuGeom> sRecs;                     // alpha records, a group's rows contiguous; a dropped group's rows stay as garbage until the next compaction
std::vector<std::array<U32, 4>> sRecNames;      // the textures (GL names) of each row, all zero for garbage rows: the slots come from them
U32 sRecGarbage = 0;
std::vector<Key> sDirtyKeys;
bool sForceFull = true;                         // make everything again at the next call
float sOrigin[3] = {};
bool sOriginValid = false;
U64 sTopoSeen = 0;
bool sAlphaOnSeen = true;
U64 sVerifyCounter = 0;
U64 sVerifyScanCounter = 0;
U64 sSceneCalls = 0;
U64 sBlasStorageBytes = 0;
U32 sLastInstances = 0;
// Compaction of static structures (see buildScene): a query pool per frame slot for the compacted sizes, and what is waiting for its answer. The answer is read when the slot comes round again
// (its fence has been waited for by then): the structure is copied into an allocation of the compacted size, and the instance follows at the next call.
constexpr U32 kCompactQueries = 512;
struct PendingCompact
{
    Key mKey;
    VkAccelerationStructureKHR mAs = VK_NULL_HANDLE;
    VkDeviceAddress mAddress = 0;
};
VkQueryPool sCompactPool[Context::FRAMES_IN_FLIGHT] = {};
std::vector<PendingCompact> sCompactPending[Context::FRAMES_IN_FLIGHT];
U64 sCompactFrame[Context::FRAMES_IN_FLIGHT] = {};      // the frame + 1 of the slot's latest reset of its pool
U32 sCompactNext[Context::FRAMES_IN_FLIGHT] = {};       // the next free query of the slot's pool

// Flight recorder: what the latest scene calls handed to the GPU, printed when a frame is stuck (the driver resets the device after 2 s and nothing can be asked of it afterwards).
// The hangs seen so far were in the top-level structure build: what it is given (a count, transforms, references to bottom-level structures) is what this keeps.
struct SceneRecord
{
    U64 mFrame = 0, mCall = 0, mSceneCall = 0;
    U32 mInstances = 0, mCapacity = 0, mJobs = 0, mSkinChunks = 0, mRecords = 0;
    U32 mBadTransforms = 0;             // instances with a NaN or infinite value in their transform
    U32 mZeroRefs = 0;                  // instances that refer to no structure
    U32 mUnknownRefs = 0;               // instances that refer to a structure this scene does not know (checked on the stuck frame only)
    float mMaxTranslation = 0.f, mMaxScale = 0.f;
    float mOrigin[3] = {}, mCamera[3] = {};
    U64 mScratchNeeded = 0, mInstanceOffset = 0;
    std::vector<U64> mRefs;             // the structure addresses of the instances (the latest call only is kept in full)
    std::vector<U64> mWho;              // where each instance came from: the Group pointer (compared, never followed), or bit 63 + the avatar id
};
SceneRecord sRecent[6];
U32 sRecentNext = 0;
U32 sLastBuilt = 0;
U32 sTableOffset = 0;
U32 sSlotCacheName[16384] = {};
U32 sSlotCacheSlot[16384] = {};
U64 sSlotCacheTag = 0;
Stats sStats;

void mark_dirty(Group& g)
{
    if (!g.mDirty)
    {
        g.mDirty = true;
        sDirtyKeys.push_back(g.mKey);
    }
}

// groups that are left out of the scene: masked ones when alpha is off, and (unless dynamic casters are on) casters with a matrix of their own that is not a region's
bool group_skipped(const Key& k, bool alpha_on, bool dyn_casters)
{
    return (k.mKind == 1 && !alpha_on) || (!dyn_casters && (k.mMatrixId & 0x80000000u) == 0 && k.mMatrixId != 0);
}

void add_pending(Group& g)
{
    if (!g.mPending)
    {
        g.mPending = true;
        sPending.push_back(&g);
    }
}

void remove_pending(Group& g)
{
    if (g.mPending)
    {
        g.mPending = false;
        auto it = std::find(sPending.begin(), sPending.end(), &g);
        if (it != sPending.end())
        {
            *it = sPending.back();
            sPending.pop_back();
        }
    }
}

// takes the group's instance and records out of the cache
void release_row(size_t row);

void drop_entry(Group& g, U32 why = 7)
{
    if (g.mSlot != ~0u)
    {
        ++sStats.mDropped[g.mKey.mKind == 1 ? 1 : 0][std::min<U32>(why, 7)];
        const U32 last = (U32)sInstArray.size() - 1;
        if (g.mSlot != last)
        {
            sInstArray[g.mSlot] = sInstArray[last];
            sInstGroup[g.mSlot] = sInstGroup[last];
            sInstGroup[g.mSlot]->mSlot = g.mSlot;
        }
        sInstArray.pop_back();
        sInstGroup.pop_back();
        g.mSlot = ~0u;
    }
    if (g.mRecCount)
    {
        for (U32 k = 0; k < g.mRecCount; ++k)
        {
            release_row(g.mRecFirst + k);
        }
        sRecGarbage += g.mRecCount;
        g.mRecCount = 0;
    }
}

// a row of the alpha / opaque records is not used any more: its texture names are forgotten and the pins it held given back
void release_row(size_t row)
{
    sRecNames[row] = { 0u, 0u, 0u, 0u };
    if (row < sRecPins.size())
    {
        for (U32& pin : sRecPins[row])
        {
            if (pin)
            {
                LLVkWorker::rtPinRelease(pin);
                ++sStats.mPinsDropped;
                pin = 0;
            }
        }
    }
}

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

// LL_VK_DIAG=1 turns on the diagnostics that cost CPU time on the worker: the flight recorder (with the instance sources), the instance filter, the draw ring, the draw range check, the
// stall logger. Off by default: the launcher run-viewer-clean-rtshadows-diag.bat sets it together with the timers (LL_VK_PHASES, LL_VK_OPSTATS).
bool diag_on()
{
    static const bool on = []() { const char* e = getenv("LL_VK_DIAG"); return e && e[0] && atoi(e) != 0; }();
    return on;
}

bool device_ready()
{
    const Capabilities& caps = Context::instance().getCaps();
    return caps.mRt.mEnabled && caps.mRt.mUsable && vkCmdBuildAccelerationStructuresKHR && vkCreateAccelerationStructureKHR && vkGetAccelerationStructureDeviceAddressKHR;
}

// Structures given up lately (address -> the scene call that gave it up). The GPU must never be handed an instance that refers to one: the top-level build reads the structure it refers to, and a
// destroyed one (its storage is given back after the frames in flight, then reused) is memory that belongs to something else. The instance lists are kept up to date when a structure goes,
// this is the check that nothing slipped through: such an instance is left out of the scene and reported. An address that is made again is taken out.
std::unordered_map<U64, U64> sDeadRecent;

void drop_blas(Blas& b)
{
    if (b.mAddress)
    {
        sDeadRecent[b.mAddress] = sSceneCalls;
    }
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
bool make_tlas(Tlas& t, U32 capacity, U32 table_calls)
{
    Context& ctx = Context::instance();
    VkDevice dev = ctx.getDevice();
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    tlas_sizes(capacity, sizes);
    if (!ctx.createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, Context::Memory::Device, t.mStorage)
        || !ctx.createBuffer(64ull * capacity * kCalls, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, Context::Memory::UploadDevice, t.mInstances)
        || !ctx.createBuffer(sRegionBytes * table_calls, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, t.mTable)
        || !ctx.createBuffer(kSkinBytes * kCalls, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Context::Memory::UploadDevice, t.mSkin))
    {
        ctx.destroyBuffer(t.mStorage);
        ctx.destroyBuffer(t.mInstances);
        ctx.destroyBuffer(t.mTable);
        ctx.destroyBuffer(t.mSkin);
        return false;
    }
    memset(t.mTable.mMapped, 0, (size_t)(sRegionBytes * table_calls));
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
    VkDescriptorBufferInfo table = { t.mTable.mBuffer, 0, sGeomBytes };
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
    U32 mNormalSrc[2];              // the source normals (vec4 array, bind pose, indexed by the absolute vertex number) or 0: none are skinned
    U32 mNormalDst[2];              // where the skinned normals go (world space, vec4, indexed by the absolute vertex number)
    F32 mModel[12];                 // three rows of four
    F32 mGuard[4];                  // centre (xyz) and radius of the volume the skinned vertices must lie in (radius 0: no limit)
};
static_assert(sizeof(SkinJob) == 128, "skin job");

struct SkinChunk
{
    U32 mJob, mOffset;
};

// does the draw's source vertex buffer hold normals the compute pass can read (16 bytes a vertex from the named offset)?
static bool skin_normals_ok(const SkinDraw& d)
{
    return d.mAlpha.mNormalOffset != 0 && d.mVi.mAddress != 0 && (U64)d.mAlpha.mNormalOffset + ((U64)d.mVertexFirst + d.mVertexCount) * 16 <= d.mVi.mSize;
}

const char* kSkinShader = R"GLSL(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 128) in;
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InVec4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer InFloat { float w[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer OutVec4 { vec4 v[]; };
struct Joint { vec4 c0; vec4 c1; vec4 c2; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Palette { Joint j[]; };
struct Job { uvec2 src; uvec2 weights; uvec2 palette; uvec2 dst; uint first; uint count; uint mode; uint joints; uvec2 nsrc; uvec2 ndst; vec4 m0; vec4 m1; vec4 m2; vec4 guard; };
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
    const bool with_normals = (job.nsrc.x | job.nsrc.y) != 0u;
    vec3 nrm = with_normals ? InVec4(job.nsrc).v[v].xyz : vec3(0.0, 1.0, 0.0);
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
        nrm = m * nrm;                  // the same linear part (the viewer's own skinning shaders do the same: no inverse transpose)
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
        vec4 r0 = mix(ja.c0, jb.c0, x), r1 = mix(ja.c1, jb.c1, x), r2 = mix(ja.c2, jb.c2, x);
        pos = vec3(dot(r0, ps), dot(r1, ps), dot(r2, ps));
        nrm = vec3(dot(r0.xyz, nrm), dot(r1.xyz, nrm), dot(r2.xyz, nrm));
    }
    vec4 p4 = vec4(pos, 1.0);
    vec3 world = vec3(dot(job.m0, p4), dot(job.m1, p4), dot(job.m2, p4));
    // garbage from a half-loaded or hidden avatar (joint matrices not set or stale, NaN weights) would give the structure a bounding box that covers the world and every ray a very long walk
    // (a lighting pass of 2 M rays through hundreds of thousands of world-sized triangles outlasts the 2 s the driver allows a GPU job: VK_ERROR_DEVICE_LOST). A vertex outside the
    // volume the job may reach (a sphere round the avatar when it is known, else the plausible coordinates) becomes NaN: a triangle with a NaN vertex is inactive, it is left out of
    // the structure, no spike to an arbitrary point remains
    bool bad = !(abs(world.x) < 1.0e5 && abs(world.y) < 1.0e5 && abs(world.z) < 1.0e5);
    if (!bad && job.guard.w > 0.0)
    {
        vec3 d = world - job.guard.xyz;
        bad = !(dot(d, d) <= job.guard.w * job.guard.w);
    }
    if (bad)
    {
        world = vec3(uintBitsToFloat(0x7fc00000u));
    }
    OutVec4(job.dst).v[v] = vec4(world, source.w);       // w: the texture index of masked draws
    if (with_normals)
    {
        vec3 wn = vec3(dot(job.m0.xyz, nrm), dot(job.m1.xyz, nrm), dot(job.m2.xyz, nrm));
        float len = length(wn);
        OutVec4(job.ndst).v[v] = vec4(len > 1e-6 ? wn / len : vec3(0.0, 0.0, 1.0), 0.0);
    }
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
    bool mFastBuild = false;        // avatar structures: built with PREFER_FAST_BUILD
    bool mCompact = false;          // built with ALLOW_COMPACTION: the compacted size is asked for after the build
    U32 mQuery = ~0u;               // the query of that size (assigned when the call is recorded)
    SkinJob mSkin = {};
};

} // anonymous namespace

S32 glossyReflections() { return sGlossyRtLive.load(std::memory_order_relaxed); }
S32 waterReflections() { return sWaterRtLive.load(std::memory_order_relaxed); }
S32 hitQuality() { return sHitQLive.load(std::memory_order_relaxed); }
S32 hitRays() { return sHitRaysLive.load(std::memory_order_relaxed); }
S32 debugView() { return sViewLive.load(std::memory_order_relaxed); }
bool loadingGates() { return sGateLive.load(std::memory_order_relaxed); }
bool bodyRename() { return sBodyNameLive.load(std::memory_order_relaxed); }

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

// the flight recorder's report (Context::setHangReporter): the latest scene calls, oldest first; the newest one is also checked against the structures that exist now
void report_recent_scene_calls()
{
    for (U32 i = 0; i < Context::frameCount(); ++i)
    {
        const Tlas& t = sTlas[i];
        LL_WARNS("VkRt") << "ray scene buffers of frame slot " << i << " (device address, size): instances 0x" << std::hex << (unsigned long long)t.mInstances.mAddress << std::dec << " " << (unsigned long long)t.mInstances.mSize
                         << ", table 0x" << std::hex << (unsigned long long)t.mTable.mAddress << std::dec << " " << (unsigned long long)t.mTable.mSize
                         << ", skin 0x" << std::hex << (unsigned long long)t.mSkin.mAddress << std::dec << " " << (unsigned long long)t.mSkin.mSize
                         << ", storage 0x" << std::hex << (unsigned long long)t.mStorage.mAddress << std::dec << " " << (unsigned long long)t.mStorage.mSize << LL_ENDL;
    }
    std::unordered_set<U64> known;
    for (const auto& kv : sBlas)
    {
        if (kv.second.mAddress)
        {
            known.insert(kv.second.mAddress);
        }
    }
    for (const auto& kv : sAvatars)
    {
        if (kv.second.mAddressNow)
        {
            known.insert(kv.second.mAddressNow);
        }
    }
    LL_WARNS("VkRt") << "ray scene flight recorder (oldest first; " << known.size() << " structures exist now):" << LL_ENDL;
    for (U32 k = 0; k < 6; ++k)
    {
        const U32 index = (sRecentNext + k) % 6;
        SceneRecord& r = sRecent[index];
        if (!r.mSceneCall)
        {
            continue;
        }
        if (k == 5)
        {
            U32 shown = 0;
            for (size_t i = 0; i < r.mRefs.size(); ++i)
            {
                const U64 ref = r.mRefs[i];
                if (!ref || known.find(ref) != known.end())
                {
                    continue;
                }
                ++r.mUnknownRefs;
                if (shown++ >= 10)
                {
                    continue;
                }
                // where it came from, without following a pointer that may be gone: groups are found by comparing the pointer with the ones that exist
                const U64 who = i < r.mWho.size() ? r.mWho[i] : 0;
                std::string text = "avatar " + std::to_string((unsigned long long)(who & 0xFFFFFFFFull));
                if (!(who >> 63))
                {
                    text = "a group that no longer exists";
                    for (const auto& kv : sGroups)
                    {
                        if ((U64)(uintptr_t)&kv.second == who)
                        {
                            const Group& g = kv.second;
                            auto in_map = sBlas.find(g.mKey);
                            char b[40] = "";
                            if (in_map != sBlas.end())
                            {
                                snprintf(b, sizeof(b), "0x%llx", (unsigned long long)in_map->second.mAddress);
                            }
                            text = "group vb " + std::to_string(g.mKey.mVb) + " ib " + std::to_string(g.mKey.mIb) + " kind " + std::to_string(g.mKey.mKind) + " matrix " + std::to_string(g.mKey.mMatrixId);
                            if (in_map == sBlas.end())
                            {
                                text += ", its key is NOT in the structure registry";
                            }
                            else
                            {
                                text += std::string(g.mBlas == &in_map->second ? ", registry entry is its own" : ", its pointer is NOT the registry entry") + ", the registry entry's structure is now " + b;
                            }
                            break;
                        }
                    }
                }
                char addr[24];
                snprintf(addr, sizeof(addr), "0x%llx", (unsigned long long)ref);
                LL_WARNS("VkRt") << "  unknown structure in the newest scene call: instance " << i << " refers to " << addr << ", from " << text << LL_ENDL;
            }
        }
        LL_WARNS("VkRt") << "  scene call " << r.mSceneCall << " frame " << r.mFrame << " slot call " << r.mCall << ": " << r.mInstances << " instances of " << r.mCapacity << ", " << r.mJobs
                         << " structure builds, " << r.mSkinChunks << " skin chunks, " << r.mRecords << " records, scratch " << (r.mScratchNeeded >> 20) << " MB, instance buffer offset " << r.mInstanceOffset
                         << "; transforms with NaN/Inf " << r.mBadTransforms << ", references 0: " << r.mZeroRefs << ", unknown (newest only): " << r.mUnknownRefs << "; largest translation " << r.mMaxTranslation
                         << " scale " << r.mMaxScale << "; origin (" << r.mOrigin[0] << "," << r.mOrigin[1] << "," << r.mOrigin[2] << ") camera (" << r.mCamera[0] << "," << r.mCamera[1] << "," << r.mCamera[2] << ")" << LL_ENDL;
    }
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
    sMatsInit = env_u64("LL_VK_RT_MATERIALS", 0) != 0;
    sGeomBytes = sMatsInit ? kGeomBytesMats : kGeomBytesBase;
    sRegionBytes = sGeomBytes + (sMatsInit ? kMatBytes + kPinBytes : 0);
    sMaxRecords = (U32)(sGeomBytes / sizeof(GpuGeom));
    // One table region per scene call of a frame (up to kCalls). With the material tables on there used to be only 3: a frame with more scene calls (a mirror or other extra views: 3.5 calls per frame in a
    // mirror room) made the calls from the fourth on rewrite the third call's region while the GPU still had to trace the earlier calls against it: records that do not match their structures,
    // GPU hangs (device lost within seconds of entering the room, 2026-10-12). Cost: 6 x 5.6 MB per frame slot.
    sTableCalls = kCalls;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, Context::frameCount() + 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, Context::frameCount() + 1 } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = Context::frameCount() + 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &sPool) != VK_SUCCESS)
    {
        sFailed = true;
        return false;
    }
    VkDescriptorSetLayout layouts[Context::FRAMES_IN_FLIGHT + 1];
    for (auto& l : layouts) l = set_layout;
    VkDescriptorSet sets[Context::FRAMES_IN_FLIGHT + 1] = {};
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = sPool;
    ai.descriptorSetCount = Context::frameCount() + 1;
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
        if (!make_tlas(sTlas[i], kInstanceCapacity, sTableCalls))
        {
            sFailed = true;
            return false;
        }
    }
    sNullTlas.mSet = sets[Context::frameCount()];
    if (!make_tlas(sNullTlas, 16, 1))
    {
        sFailed = true;
        return false;
    }
    {
        // its table: record 0 is the header (identity rotation, the camera at the origin, one record in the table), the rest is zero
        float header[12] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f };
        GpuGeom* null_table = (GpuGeom*)sNullTlas.mTable.mMapped;
        memset(null_table, 0, sizeof(GpuGeom));
        memcpy(null_table, header, sizeof(header));
        null_table[0].mFirst = 1;
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
        for (U32 i = 0; i <= Context::frameCount(); ++i)
        {
            Tlas& built = i < Context::frameCount() ? sTlas[i] : sNullTlas;       // (the last one is the null scene)
            VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geo.geometry.instances.data.deviceAddress = built.mInstances.mAddress;
            VkAccelerationStructureBuildGeometryInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geo;
            info.dstAccelerationStructure = built.mAs;
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
    if (diag_on())
    {
        Context::instance().setHangReporter(report_recent_scene_calls);
    }
    LL_INFOS("VkRt") << "ray tracing scene ready: mode " << shadowMode() << ", " << Context::frameCount() << " frame slots, " << kCalls << " scene calls per frame, instance capacity "
                     << kInstanceCapacity << ", geometry table " << (sGeomBytes >> 10) << " KB per call (" << sMaxRecords << " records), material tables " << (sMatsInit ? "on" : "off")
                     << ", table regions per frame slot " << sTableCalls << LL_ENDL;
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
    if (sNullTlas.mAs != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(dev, sNullTlas.mAs, nullptr);
    }
    sNullTlas = Tlas();
    for (U32 i = 0; i < Context::FRAMES_IN_FLIGHT; ++i)
    {
        if (sCompactPool[i] != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(dev, sCompactPool[i], nullptr);
            sCompactPool[i] = VK_NULL_HANDLE;
        }
        sCompactPending[i].clear();
    }
    if (sPool)
    {
        vkDestroyDescriptorPool(dev, sPool, nullptr);
        sPool = VK_NULL_HANDLE;
    }
    sInit = false;
}

bool materialsOn()
{
    return sInit && sMatsInit;
}

void setPinSlot(U32 index, U32 slot)
{
    if (index == 0 || index >= kPinCap)
    {
        return;
    }
    if (sPinSlots.size() <= index)
    {
        sPinSlots.resize(index + 1, 0);
    }
    if (sPinSlots[index] != slot)
    {
        sPinSlots[index] = slot;
        sPinLog.touch(index);
    }
}

void updateMaterials(const LLVkBridge::RtMaterialDef* defs, U32 count)
{
    if (!sInit || !sMatsInit)
    {
        return;
    }
    for (U32 i = 0; i < count; ++i)
    {
        const LLVkBridge::RtMaterialDef& d = defs[i];
        if (d.mId == 0 || d.mId >= kMatCap)
        {
            continue;
        }
        if (sMatRows.size() <= d.mId)
        {
            sMatRows.resize(d.mId + 1);
            sMatPins.resize(d.mId + 1, std::array<U32, 8>{});
        }
        GpuMaterial& m = sMatRows[d.mId];
        memset(&m, 0, sizeof(m));
        memcpy(m.mColor, d.mColor, sizeof(m.mColor));
        memcpy(m.mEmissive, d.mEmissive, sizeof(m.mEmissive));
        m.mCutoff = d.mCutoff;
        m.mMetallic = d.mMetallic;
        m.mRoughness = d.mRoughness;
        m.mFlags = d.mFlags;
        if (!(d.mFlags & LLVkBridge::RTM_GLTF))
        {
            // legacy materials have no emissive colour here: the slots carry the specular colour (rgb, in place of the emissive colour) and the specular exponent / 255 (in the pad of the params),
            // which the lit hits of the traced mirror need (softenLightF.glsl: spec.rgb = specular map x colour, glossiness = exponent x normal map alpha, like the G-buffer pass)
            memcpy(m.mEmissive, d.mSpecular, 3 * sizeof(F32));
            memcpy(&m.mPad0, &d.mSpecular[3], sizeof(F32));
        }
        memcpy(m.mUv, d.mUv, sizeof(m.mUv));
        // the textures are pins: the pin index goes where the slot used to be (the shader looks the slot up), the pins of the old definition of this row are given back afterwards
        const U32 names[7] = { d.mTextures[0], d.mTextures[1], d.mTextures[2], d.mTextures[3], d.mNormalMap, d.mSpecularMap, d.mEmissiveMap };
        std::array<U32, 8> fresh = {};
        for (int k = 0; k < 7; ++k)
        {
            fresh[k] = names[k] ? LLVkWorker::rtPinAcquire(names[k]) : 0u;
            sStats.mPinsHeld += names[k] ? 1 : 0;
        }
        for (U32 old_pin : sMatPins[d.mId])
        {
            if (old_pin)
            {
                LLVkWorker::rtPinRelease(old_pin);
                ++sStats.mPinsDropped;
            }
        }
        sMatPins[d.mId] = fresh;
        m.mSlots[0] = (fresh[0] & 0xFFFFu) | (fresh[1] << 16);
        m.mSlots[1] = (fresh[2] & 0xFFFFu) | (fresh[3] << 16);
        m.mSlots2[0] = (fresh[4] & 0xFFFFu) | (fresh[5] << 16);
        m.mSlots2[1] = fresh[6] & 0xFFFFu;
        sMatLog.touch(d.mId);
        ++sStats.mMatUpdates;
    }
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
        it->second.mSkip = group_skipped(key, sDbg.mAlpha, sDbg.mDynCasters);
        if (!sSeenFree.empty())
        {
            it->second.mId = sSeenFree.back();
            sSeenFree.pop_back();
        }
        else
        {
            it->second.mId = (U32)sSeenRows.size();
            sSeenRows.emplace_back();
        }
        sSeenRows[it->second.mId] = SeenRow();
        sSeenRows[it->second.mId].mGroup = &it->second;
        if (key.mKind == 1 || (key.mKind == 0 && sDbg.mMats))
        {
            sNameGroups.emplace(key.mVb, key);
            sNameGroups.emplace(key.mIb, key);
        }
        add_pending(it->second);
        ++sStats.mNewGroups;
        mark_dirty(it->second);
    }
    Group& g = it->second;
    const U32 pass = std::min<U32>(c.mPass, kPasses - 1);
    const U64 frame_tag = Context::instance().getFrameNumber() + 1;
    sSeenRows[g.mId].mSeen[pass] = frame_tag;
    sPassFrame[pass] = frame_tag;
    if (g.mNotedTag != frame_tag)
    {
        if (sNotedListTag != frame_tag)
        {
            sNotedListTag = frame_tag;
            sNotedList.clear();
        }
        sNotedList.push_back(&g);
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
        if (g.mMoving || ((c.mKind == 1 || (c.mKind == 0 && c.mAlpha)) && (g.mVi.mAddress != c.mVi.mAddress || g.mIi.mAddress != c.mIi.mAddress)))
        {
            ++sStats.mDirtyWhy[g.mMoving ? 0 : 1];
            mark_dirty(g);                      // the instance matrix, or the buffer addresses in the alpha records, changed
        }
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
                g.mAlpha[k] = *c.mAlpha;                // the textures (or the material) may have changed
                g.mBlas->mAlpha[r.id()] = *c.mAlpha;
                ++sStats.mDirtyWhy[2];
                mark_dirty(g);
            }
            return;
        }
    }
    if (ranges.size() < 512)
    {
        ranges.push_back(r);
        ++sStats.mDirtyWhy[3];
        mark_dirty(g);
        g.mAlpha.push_back(c.mAlpha ? *c.mAlpha : AlphaInfo());
        if (c.mAlpha)
        {
            g.mBlas->mAlpha[r.id()] = g.mAlpha.back();     // the alpha test data of each range of a masked structure, as last drawn (and the material of an opaque one)
        }
    }
}

void noteSkinned(const SkinCaster& c)
{
    if (!sInit || sFailed || !sDbg.mRigged || c.mCount < 3 || c.mVertexCount == 0 || (c.mMode != 2 && (!c.mPalette || c.mJoints == 0)))
    {
        return;
    }
    if ((c.mAvatarKind & 64u) && sDbg.mGate)
    {
        return;                             // an avatar that is still loading in (shape, textures, attachments not final): not named, so not in the scene until it is
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
    if (c.mSelf)
    {
        a.mSelf = true;
    }
    if (c.mAvatarKind)
    {
        a.mAvatarKind = c.mAvatarKind;
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
                // the textures may have changed. A note without a material (the shadow passes name some draws without one) leaves the material the main view named.
                const U32 material = d.mAlpha.mMaterial, normals = d.mAlpha.mNormalOffset;
                d.mAlpha = *c.mAlpha;
                if (!d.mAlpha.mMaterial && material)
                {
                    d.mAlpha.mMaterial = material;
                    d.mAlpha.mNormalOffset = normals;
                }
            }
            d.mDead = false;
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
    SkinDraw* slot = nullptr;
    if (a.mDraws.size() >= 128)
    {
        // draws come and go (buffer names are recycled): a slot of a dead draw is reused. A structure built over the old draw is rebuilt in this frame's scene call (the avatar is named now).
        for (SkinDraw& old_draw : a.mDraws)
        {
            if (old_draw.mDead && !old_draw.mOut.valid())
            {
                slot = &old_draw;
                break;
            }
        }
        if (!slot && a.mDraws.size() >= 512)
        {
            return;
        }
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
    if (slot)
    {
        const U32 slot_index = (U32)(slot - a.mDraws.data());
        *slot = std::move(d);
        a.mSig.clear();                     // the structure is built again at the next pass over this avatar
        // The geometries of the structure that is built NOW stay in use until then, unless one of them is this very slot: a dead draw is left out of every structure made after it died, so the slot is normally not
        // in it. Wiping the list unconditionally took the whole avatar out of the scene calls that follow the note in the same frame ("no geometries"): the reflected avatar flashed whenever a draw
        // appeared (an avatar with 128 draws or more: a level of detail switch of a rigged attachment is enough).
        if (std::find(a.mBuiltIdx.begin(), a.mBuiltIdx.end(), slot_index) != a.mBuiltIdx.end())
        {
            a.mBuiltIdx.clear();
        }
        ++a.mSlotClears;
    }
    else
    {
        a.mDraws.push_back(std::move(d));
    }
}

// true when the frame slot's structure and table are the ones of THIS command buffer (its scene call has been recorded): otherwise the null scene is the consistent one
static bool scene_current(U32 frame_slot)
{
    return sNullTlas.mSet == VK_NULL_HANDLE || sTlas[frame_slot].mBuiltFrame == Context::instance().getFrameNumber();
}

VkDescriptorSet descriptorSet(U32 frame_slot)
{
    if (frame_slot >= Context::FRAMES_IN_FLIGHT)
    {
        return VK_NULL_HANDLE;
    }
    return scene_current(frame_slot) ? sTlas[frame_slot].mSet : sNullTlas.mSet;
}

U32 tableOffset(U32 frame_slot)
{
    return frame_slot < Context::FRAMES_IN_FLIGHT && !scene_current(frame_slot) ? 0u : sTableOffset;
}

void buildScene(const F32* view, TextureSlotFn slot_of, TexturePrefetchFn prefetch)
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
    if (sDbg.mPreflush)
    {
        LLVkWorker::flushPendingBufferWrites();
    }
    if (sDbg.mSingle)
    {
        const Tlas& t0 = sTlas[ctx.getFrameSlot()];
        if (t0.mCallFrame == ctx.getFrameNumber() && t0.mCalls > 0)
        {
            return;                             // later scene calls of a frame keep the first one's scene (debug switch)
        }
    }
    // Builds per scene call. A structure costs the GPU about 0.18 ms to build whatever its size (a plaza of small objects is a burst of 100 k of them), so on arrival somewhere new the old
    // limit of 256 per call put 33 ms of builds into every frame for two minutes. The nearest structures are built first, the rest follow over the next seconds.
    static const U64 max_builds_default = env_u64("LL_VK_RT_BUILD_MAX", 128);
    // Ceiling for the memory of the structures (LL_VK_RT_MAX_MB, default 1.5 GB: the steady state of a busy place is about 1 GB). Over it no NEW structure is started, and structures that nothing has
    // named for a short while are given back at once (see below), so that the scene cannot push video memory past the budget the OS grants (over it the driver moves allocations to system memory).
    static const U64 as_ceiling_mb = env_u64("LL_VK_RT_MAX_MB", 1536);
    const U64 as_ceiling = as_ceiling_mb << 20;
    const U64 max_builds = sDbg.mBuildMax ? sDbg.mBuildMax : max_builds_default;
    static const U64 max_tris = env_u64("LL_VK_RT_BUILD_TRIS", 800000);
    const bool alpha_on = sDbg.mAlpha;
    const bool check_on = sDbg.mCheck;
    const bool skip_bad = sDbg.mSkipBad;
    static const U64 rebuild_after_refits = 64;
    ++sSceneCalls;
    const U64 keep = sDbg.mKeep;
    if (sDbg.mCompact && sCompactPool[ctx.getFrameSlot()] == VK_NULL_HANDLE && vkCmdCopyAccelerationStructureKHR && vkCmdWriteAccelerationStructuresPropertiesKHR)
    {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = kCompactQueries;
        if (vkCreateQueryPool(dev, &qi, nullptr, &sCompactPool[ctx.getFrameSlot()]) != VK_SUCCESS)
        {
            sCompactPool[ctx.getFrameSlot()] = VK_NULL_HANDLE;
        }
    }

    auto lap = [&](int phase, std::chrono::steady_clock::time_point& last) {
        const auto now = std::chrono::steady_clock::now();
        sStats.mPhaseMs[phase] += std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
    };
    auto t_lap = t0;

    // forget structures that were not named for a while, and casters nothing has named for a few frames
    for (auto dead = sDeadRecent.begin(); dead != sDeadRecent.end();)
    {
        dead = (sSceneCalls - dead->second > 24) ? sDeadRecent.erase(dead) : std::next(dead);
    }
    const bool as_pressure = sBlasStorageBytes >= as_ceiling / 10 * 9;
    if (sSceneCalls % (as_pressure ? 30 : 120) == 0)
    {
        const U64 unseen_limit = as_pressure ? 150 : 900;       // calls: about 1.5 s under pressure, about 9 s otherwise
        for (auto it = sBlas.begin(); it != sBlas.end();)
        {
            // a structure that a live group or avatar still points at stays: the group (kept alive by the pass hold windows without being named every call) and the avatar keep a raw pointer to their node,
            // and erasing it left them with a destroyed structure (the next note of the group wrote into its freed alpha map: a crash about half a minute after login, when the arena was full)
            const bool in_use = sGroups.find(it->first) != sGroups.end() || (it->first.mKind == 3 && sAvatars.find(it->first.mVb) != sAvatars.end());
            if (!in_use && sSceneCalls - it->second.mLastSeen > unseen_limit)
            {
                drop_blas(it->second);
                it = sBlas.erase(it);
                ++sStats.mEvicted;
                sForceFull = true;
                ++sStats.mForceWhy[0];
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
        bool rendered[kPasses];
        for (U32 p = 0; p < kPasses; ++p)
        {
            rendered[p] = sPassFrame[p] == tag;
        }
        for (SeenRow& row : sSeenRows)
        {
            if (!row.mGroup)
            {
                continue;
            }
            bool alive = false;
            for (U32 p = 0; p < kPasses; ++p)
            {
                U64& seen = row.mSeen[p];
                if (!seen)
                {
                    continue;
                }
                if (rendered[p])
                {
                    // the pass rendered this frame: its casters are the ones it named. A pass that renders only part of its views per frame (the hero probe faces: two of six per frame, a turn
                    // every third frame) names a caster only in the frames of the faces that see it: such a group has to stay for a few frames, or it is in the scene two frames in three
                    // (the globe, window and wall pictures seen only in the mirror came and went every third frame). grouphold=0 (live) is the old behaviour.
                    if (seen == tag)
                    {
                        alive = true;
                    }
                    else if (tag <= seen + sDbg.mGroupHold)
                    {
                        alive = true;
                    }
                    else
                    {
                        seen = 0;
                    }
                }
                else if (tag <= seen + (p >= 7 ? std::max<U64>(keep, 128) : keep))
                {
                    alive = true;           // cached (or not rendered at all): the shadow map it fed is still in use (a probe face: until it renders again, a face of a reflection probe may wait many frames)
                }
            }
            if (!alive)
            {
                Group& g = *row.mGroup;
                const Key dead_key = g.mKey;
                static const bool keep_old_leak = getenv("LL_VK_RT_NAMELEAK") != nullptr;      // A/B switch: the old behaviour (opaque groups of the material mode never left the table)
                if (dead_key.mKind == 1 || (dead_key.mKind == 0 && sDbg.mMats && !keep_old_leak))     // the same groups that were added to sNameGroups (the material mode adds opaque ones too)
                {
                    for (U32 name : { dead_key.mVb, dead_key.mIb })
                    {
                        auto range = sNameGroups.equal_range(name);
                        for (auto nit = range.first; nit != range.second; ++nit)
                        {
                            if (nit->second == dead_key)
                            {
                                sNameGroups.erase(nit);
                                break;
                            }
                        }
                    }
                }
                drop_entry(g, 0);
                remove_pending(g);
                sSeenFree.push_back(g.mId);
                row.mGroup = nullptr;
                sGroups.erase(dead_key);
            }
        }
    }

    lap(0, t_lap);
    // ---- 1. what is stale -----------------------------------------------------------------------------------
    // The structures keep pointers into this list (Blas::mJob): it must NEVER reallocate while the call runs. Groups stop at max_builds, avatars at max_builds + kAvatarBuilds, and both also
    // check the capacity before they add one. (The avatars' extra 16 once went past the old reserve of max_builds: every pointer dangled, the instances of the groups got their structure
    // address from freed memory, and the top-level build read junk references and hung the GPU.)
    std::vector<Job> jobs;
    jobs.reserve((size_t)max_builds + kAvatarBuilds + 8);
    std::vector<SkinJob> skin_jobs;
    std::vector<SkinChunk> skin_chunks;
    std::vector<SkinDraw*> skin_job_draws;      // the draw behind each job (null for the skin test copies), for the probe
    const bool skin_on = sDbg.mSkinTest && ensure_skin_pipeline();
    U64 tris_budget = max_tris;
    LLVkWorker::BufferInfo vi, ii;
    VkDeviceSize scratch_total = 0;
    // Which groups may need work: never built, or named this frame with changed buffers or new ranges. Everything else is left alone (the scan does no lookups for the many
    // groups that did not change). Nearest first (the distance of the object's origin in eye space): while a scene loads the budget goes to what is around the camera.
    const U64 scan_tag = ctx.getFrameNumber() + 1;
    std::vector<std::pair<float, Group*>> order;
    // the switches that decide which groups are left out changed: look at all groups once (new groups get the flag when they are made)
    static int skip_alpha_seen = -1, skip_dyn_seen = -1;
    if (skip_alpha_seen != (alpha_on ? 1 : 0) || skip_dyn_seen != (sDbg.mDynCasters ? 1 : 0))
    {
        skip_alpha_seen = alpha_on ? 1 : 0;
        skip_dyn_seen = sDbg.mDynCasters ? 1 : 0;
        sPending.clear();
        for (auto& kv : sGroups)
        {
            Group& g = kv.second;
            const bool was_skipped = g.mSkip;
            g.mSkip = group_skipped(g.mKey, alpha_on, sDbg.mDynCasters);
            if (was_skipped != g.mSkip)
            {
                mark_dirty(g);
            }
            g.mPending = false;
            if (!g.mSkip && g.mBlas->mAs == VK_NULL_HANDLE)
            {
                add_pending(g);
            }
        }
    }
    auto needs_work = [&](const Group& g) -> bool
    {
        const Blas& b = *g.mBlas;
        if (b.mAs == VK_NULL_HANDLE)
        {
            return true;
        }
        if (g.mNotedTag != scan_tag)
        {
            return false;                       // built, and not named this frame
        }
        bool changed = b.mVbEpoch != g.mVi.mEpoch || b.mIbEpoch != g.mIi.mEpoch || b.mVbVersion != g.mVi.mVersion || b.mIbVersion != g.mIi.mVersion || b.mRefits >= rebuild_after_refits;
        for (size_t k = 0; !changed && k < g.mRanges.size(); ++k)
        {
            changed = std::find(b.mBuilt.begin(), b.mBuilt.end(), g.mRanges[k]) == b.mBuilt.end();
        }
        return changed;
    };
    auto consider = [&](Group& g)
    {
        if (g.mCandTag == scan_tag)
        {
            return;
        }
        g.mCandTag = scan_tag;
        if (g.mSkip || !needs_work(g))
        {
            return;
        }
        F32 m[16];
        mat_mul(view, g.mModel, m);
        order.emplace_back(m[12] * m[12] + m[13] * m[13] + m[14] * m[14], &g);
    };
    // Which groups may need work: never built (sPending), or named this frame (sNotedList) with changed buffers or new ranges. Everything else is left alone, not even looked at.
    for (size_t i = 0; i < sPending.size();)
    {
        Group* g = sPending[i];
        if (g->mSkip || g->mBlas->mAs != VK_NULL_HANDLE)
        {
            g->mPending = false;
            sPending[i] = sPending.back();
            sPending.pop_back();
            continue;
        }
        consider(*g);
        ++i;
    }
    if (sNotedListTag == scan_tag)
    {
        for (Group* g : sNotedList)
        {
            consider(*g);
        }
    }
    if (sDbg.mVerify && ++sVerifyScanCounter % 60 == 0)
    {
        // the check: the candidates against what a walk over all groups finds
        std::unordered_set<const Group*> found;
        for (const auto& entry : order)
        {
            found.insert(entry.second);
        }
        U64 expected = 0, wrong = 0;
        for (auto& kv : sGroups)
        {
            const Group& g = kv.second;
            if (g.mSkip || !needs_work(g))
            {
                continue;
            }
            ++expected;
            if (!found.count(&g))
            {
                ++wrong;
            }
        }
        sStats.mScanChecks += 1;
        sStats.mScanMismatch += wrong + (found.size() > expected ? found.size() - expected : 0);
        if (wrong || found.size() != expected)
        {
            LL_WARNS("VkRt") << "candidate scan check: " << expected << " groups need work by a full walk, " << found.size() << " found by the lists, " << wrong << " missing" << LL_ENDL;
        }
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
        else if (!LLVkWorker::getBufferInfo(g.mKey.mVb, vi) || !LLVkWorker::getBufferInfo(g.mKey.mIb, ii))
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
            ++sStats.mDirtyWhy[5];
            mark_dirty(g);                      // the cached instance points at the structure that is gone
            add_pending(g);
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
        if (jobs.size() >= max_builds || jobs.size() >= jobs.capacity() || tris_budget == 0)
        {
            ++sStats.mDeferred;
            continue;
        }
        if (fresh && sBlasStorageBytes >= as_ceiling)
        {
            ++sStats.mDeferred;
            ++sStats.mCeilingHeld;
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
        LLVkWorker::markBufferUsed(g.mKey.mVb);         // the build reads them when the GPU gets here: a CPU rewrite in place before that would tear the data
        LLVkWorker::markBufferUsed(g.mKey.mIb);
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
        job.mUpdate = !grew && !ib_changed && !force_build && b.mRefits < rebuild_after_refits && b.mAs != VK_NULL_HANDLE && !sDbg.mNoRefit && !b.mCompacted;
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
            LARGE_INTEGER check_t0;
            QueryPerformanceCounter(&check_t0);
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
            LARGE_INTEGER check_t1, check_f;
            QueryPerformanceCounter(&check_t1);
            QueryPerformanceFrequency(&check_f);
            sStats.mCheckUs += (U64)((check_t1.QuadPart - check_t0.QuadPart) * 1000000ll / check_f.QuadPart);
        }
        if (sDbg.mBBox && (g.mKey.mMatrixId & 0x80000000u) == 0 && g.mKey.mMatrixId != 0)
        {
            // stage C step 0: the size of what a structure with a matrix of its own is about to be built or refitted over (flexible prims that go unstable would show up as outsized boxes)
            static FILE* bb_log = fopen("<work>/rt-bbox.log", "ab");
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
        job.mCompact = sDbg.mCompact && !job.mUpdate && !job.mHasSkin && sCompactPool[ctx.getFrameSlot()] != VK_NULL_HANDLE;
        job.mInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | (job.mCompact ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
        job.mInfo.mode = job.mUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        job.mInfo.geometryCount = (U32)job.mGeometries.size();
        job.mInfo.pGeometries = job.mGeometries.data();
        VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &job.mInfo, job.mCounts.data(), &sizes);
        job.mUpdateScratch = sizes.updateScratchSize;
        if (sDbg.mSizeProbe && !job.mUpdate)
        {
            // what the same structure would need with other flags (the driver's answer for this device); nothing is built from it
            VkAccelerationStructureBuildGeometryInfoKHR other = job.mInfo;
            VkAccelerationStructureBuildSizesInfoKHR other_sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
            const VkBuildAccelerationStructureFlagsKHR keep_bits = job.mInfo.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            other.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | keep_bits;
            vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &other, job.mCounts.data(), &other_sizes);
            const U64 no_update = other_sizes.accelerationStructureSize;
            other.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | keep_bits;
            vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &other, job.mCounts.data(), &other_sizes);
            const U64 no_update_fast = other_sizes.accelerationStructureSize;
            other.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | keep_bits;
            vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &other, job.mCounts.data(), &other_sizes);
            ++sStats.mProbeCount;
            sStats.mProbeNow += sizes.accelerationStructureSize;
            sStats.mProbeNoUpdate += no_update;
            sStats.mProbeNoUpdateFast += no_update_fast;
            sStats.mProbeFast += other_sizes.accelerationStructureSize;
        }
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
            sDeadRecent.erase(job.mNewAddress);
            job.mInfo.dstAccelerationStructure = job.mNewAs;
            job.mScratchSize = sizes.buildScratchSize;
            sBlasStorageBytes += job.mNewStorage.mSize;
        }
        job.mScratchOffset = (scratch_total + align - 1) / align * align;
        scratch_total = job.mScratchOffset + job.mScratchSize + sDbg.mPad;
        tris_budget = prims >= tris_budget ? 0 : tris_budget - prims;
        if (!job.mUpdate)
        {
            ++sStats.mDirtyWhy[5];
            mark_dirty(g);                      // a new structure: new address, maybe another geometry list
        }
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
    // Which avatars get a new pose this call. The viewer's own avatar always. Every other one is due after a number of viewer frames that grows with its distance (1 up to
    // LL_VK_RT_SKIN_NEAR metres, then 2, 3 ... skinperiod): their shadows are small and need no new pose every frame. The due ones are served most overdue first (age divided by
    // that number of frames), until the vertex budget is used up: when there is more due than the budget allows every avatar slows down alike, none is starved. In between an
    // avatar's last structure stays in the scene.
    struct AvatarTurn { double mUrgency; Avatar* mAvatar; };
    std::vector<AvatarTurn> av_order;
    static const float skin_near = (float)env_u64("LL_VK_RT_SKIN_NEAR", 8);
    static const float skin_step = (float)std::max<U64>(1, env_u64("LL_VK_RT_SKIN_STEP", 10));
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
        float dist = 0.f;
        if (a.mHasPos && !a.mSelf)
        {
            const float ex = view[0] * a.mPos[0] + view[4] * a.mPos[1] + view[8] * a.mPos[2] + view[12];
            const float ey = view[1] * a.mPos[0] + view[5] * a.mPos[1] + view[9] * a.mPos[2] + view[13];
            const float ez = view[2] * a.mPos[0] + view[6] * a.mPos[1] + view[10] * a.mPos[2] + view[14];
            dist = sqrtf(ex * ex + ey * ey + ez * ez);
        }
        if (sDbg.mSkinMaxDist > 0.f && dist > sDbg.mSkinMaxDist)
        {
            continue;
        }
        U64 period = 1;
        if (dist > skin_near && sDbg.mSkinPeriodMax > 1)
        {
            period = std::min<U64>(sDbg.mSkinPeriodMax, 2 + (U64)((dist - skin_near) / skin_step));
        }
        const bool has_structure = a.mBlas->mAs != VK_NULL_HANDLE && !a.mSig.empty() && a.mLastBuildTag;
        double urgency;
        if (a.mSelf)
        {
            urgency = 1e12;
        }
        else if (!has_structure)
        {
            urgency = 1e9 - dist;                   // nothing to show yet: soon, the nearest first
        }
        else
        {
            urgency = (double)(av_tag - a.mLastBuildTag) / (2.0 * (double)period);        // frame slots: two per viewer frame
        }
        if (urgency < 1.0)
        {
            ++sStats.mSkinThrottled;                // not due yet
            continue;
        }
        av_order.push_back({ urgency, &a });
    }
    std::sort(av_order.begin(), av_order.end(), [](const AvatarTurn& l, const AvatarTurn& r) { return l.mUrgency > r.mUrgency; });
    for (const AvatarTurn& av_entry : av_order)
    {
        Avatar& a = *av_entry.mAvatar;
        for (SkinDraw& dead : a.mDraws)
        {
            if (!dead.mDead && av_tag > dead.mNotedTag + 60)
            {
                if (dead.mOut.valid())
                {
                    ctx.arenaFree(dead.mOut);
                    dead.mOut = ArenaRef();
                }
                dead.mDead = true;                  // the slot stays: the indices of a built structure must stay valid
            }
        }
        Blas& b = *a.mBlas;
        std::vector<SkinDraw*> cur;
        U64 total_verts = 0, chunks_needed = 0;
        memset(a.mExcl, 0, sizeof(a.mExcl));
        for (SkinDraw& d : a.mDraws)
        {
            // A draw is named by the passes that run in a frame, and not every pass runs every frame (the probe faces and cascades follow their own cadence): a structure made only from the draws
            // named THIS frame lost its opaque body and clothes on the frames whose passes did not name them (the body vanished from the traced reflection every few frames). A draw named
            // within the last few frames stays in the structure over its last skinned output (its palette is the memory of the frame it was named in: only draws named now are skinned again).
            // named in this scene call's frame or the slot before it: the notes of the shadow passes and of the main view's naming (rt_name_rigged_main_view) land one frame slot BEFORE the tag the scene call
            // computes (the parts of an avatar only they name, hair layer and sock tops, were never fresh and never skinned). The palette they carry is in the vertex scratch ring (one slot per frame
            // number, FRAMES_IN_FLIGHT of them): intact for this long.
            const bool fresh = d.mNotedTag != 0 && d.mNotedTag + 2 >= av_tag;
            d.mWhyOut = 0;
            if (!fresh && (d.mDead || !d.mNotedTag || av_tag > d.mNotedTag + std::max<U64>(1, sDbg.mKeep) || !d.mSkinnedTag || !d.mOut.valid() || sDbg.mAvatarKeepOff))
            {
                d.mWhyOut = d.mDead ? 1 : (!d.mNotedTag ? 2 : (av_tag > d.mNotedTag + std::max<U64>(1, sDbg.mKeep) ? 3 : (!d.mSkinnedTag ? 4 : (!d.mOut.valid() ? 5 : 6))));
                ++a.mExcl[d.mDead ? 0 : (!d.mNotedTag ? 1 : (av_tag > d.mNotedTag + std::max<U64>(1, sDbg.mKeep) ? 2 : (!d.mSkinnedTag ? 3 : (!d.mOut.valid() ? 4 : 5))))];
                continue;
            }
            if (d.mNotedTag != av_tag)
            {
                // a draw named some frames (or the slot before this scene call's tag) ago: its buffers must still be the ones it was named with (a buffer that was made again, moved or given up has a new epoch or address: the avatar's
                // meshes are rebuilt while it loads, and a structure over indices of a range that now holds something else is a spike in the shadow)
                LLVkWorker::BufferInfo vi_now, ii_now;
                const bool known = LLVkWorker::getBufferInfo(d.mVb, vi_now) && LLVkWorker::getBufferInfo(d.mIb, ii_now);
                const bool same_content = known && vi_now.mEpoch == d.mVi.mEpoch && ii_now.mEpoch == d.mIi.mEpoch && vi_now.mSize == d.mVi.mSize && ii_now.mSize == d.mIi.mSize;
                const bool same_place = same_content && vi_now.mAddress == d.mVi.mAddress && ii_now.mAddress == d.mIi.mAddress;
                if (!same_place)
                {
                    if (same_content && d.mMode != 0)
                    {
                        // A body mesh (mode 1 and 2: rewritten into its buffer at every avatar update, the same content each time) whose buffer only MOVED: the masked hair mesh was left out of every frame it was not named in
                        // (its address changed in between), so the hair came and went. Its skinned vertices are in memory of their own: the draw takes the buffers' current place (the next build or refit reads the indices there).
                        d.mVi = vi_now;
                        d.mIi = ii_now;
                    }
                    else
                    {
                        ++a.mExcl[6];
                        d.mWhyOut = !known ? 7 : (!same_content && (vi_now.mEpoch != d.mVi.mEpoch || ii_now.mEpoch != d.mIi.mEpoch) ? 8 : (same_content ? 9 : 10));
                        continue;
                    }
                }
            }
            // the ranges must lie inside the buffers as they are now
            const U64 weight_stride = d.mMode == 0 ? 16 : (d.mMode == 1 ? 4 : 0);
            if (!d.mVi.mAddress || !d.mIi.mAddress || ((U64)d.mVertexFirst + d.mVertexCount) * 16 > d.mVi.mSize
                || (weight_stride && (U64)d.mWeightOffset + ((U64)d.mVertexFirst + d.mVertexCount) * weight_stride > d.mVi.mSize) || ((U64)d.mFirst + d.mCount) * 2 > d.mIi.mSize)
            {
                ++sStats.mNoData;
                ++a.mExcl[7];
                d.mWhyOut = 11;
                continue;
            }
            cur.push_back(&d);
            if (fresh)
            {
                total_verts += d.mVertexCount;
                chunks_needed += (d.mVertexCount + kSkinChunkVerts - 1) / kSkinChunkVerts;
            }
        }
        if (sDbg.mAvatarLog && a.mSelf)
        {
            // debug (avlog=1): every draw of the viewer's own avatar, about once in 120 frames: what the structure is made of and what is left out
            static U64 last_draw_log = 0;
            if (av_tag >= last_draw_log + 20)
            {
                last_draw_log = av_tag;
                static FILE* draw_log = fopen("<work>/rt-avatar-draws.log", "ab");
                if (draw_log)
                {
                    fprintf(draw_log, "frame %llu own avatar: %zu draws, %zu in the structure\n", (unsigned long long)av_tag, a.mDraws.size(), cur.size());
                    for (const SkinDraw& d : a.mDraws)
                    {
                        const bool in = std::find(cur.begin(), cur.end(), &d) != cur.end();
                        fprintf(draw_log, "  kind %u mode %u tris %u first %u vtx %u+%u material %u normals %u texc %u color %u noted_age %lld dead %d out %d why %u rec_calls %u rec_bad %u %s\n", d.mKind, d.mMode, d.mCount / 3, d.mFirst, d.mVertexFirst, d.mVertexCount,
                                d.mAlpha.mMaterial, d.mAlpha.mNormalOffset, d.mAlpha.mTexcoordOffset, d.mAlpha.mColorOffset, (long long)(av_tag - d.mNotedTag), d.mDead ? 1 : 0, d.mOut.valid() ? 1 : 0, d.mWhyOut, d.mRecCalls, d.mRecBad, in ? "IN" : "left out");
                    }
                    fflush(draw_log);
                }
            }
        }
        if (cur.empty())
        {
            continue;
        }
        if (total_verts > skin_verts_left)
        {
            ++sStats.mSkinOver;
            a.mOverTag = av_tag;
            continue;
        }
        if (jobs.size() >= max_builds + kAvatarBuilds || jobs.size() >= jobs.capacity() || skin_jobs.size() + cur.size() > kMaxSkinJobs || skin_chunks.size() + chunks_needed > kMaxSkinChunks)       // (avatars have 16 builds of their own beyond the groups' limit)
        {
            ++sStats.mDeferred;
            a.mOverTag = av_tag;
            continue;
        }
        if (!ensure_skin_pipeline())
        {
            continue;
        }
        // what the structure covers: a change in the draws (or in the index data they use) means a new structure, otherwise it is refitted over the new pose
        // (the body meshes, mode 1 and 2, are rewritten into their buffer at every avatar update, indices included, with the same content: their write version is not part of it, or such an avatar
        // would be built from scratch again at every update instead of refitted; a buffer that is made again has a new epoch)
        std::vector<U64> sig;
        for (SkinDraw* d : cur)
        {
            U64 h = 1469598103934665603ull;
            for (U64 v : { (U64)d->mVb, (U64)d->mIb, (U64)d->mFirst, (U64)d->mCount, (U64)d->mVertexFirst, (U64)d->mVertexCount, (U64)d->mSkinId, (U64)d->mKind, (U64)d->mMode, d->mIi.mEpoch, d->mMode == 0 ? d->mIi.mVersion : 0ull, d->mVi.mEpoch })
            {
                h = (h ^ v) * 1099511628211ull;
            }
            sig.push_back(h);
        }
        bool need_build = b.mAs == VK_NULL_HANDLE || sig != a.mSig || b.mRefits >= skin_refit_limit || sDbg.mNoRefit || b.mFastBuild != sDbg.mSkinFast;
        bool alloc_failed = false;
        for (SkinDraw* d : cur)
        {
            // the skinned positions (16 bytes a vertex), then, when the draw has normals, the skinned normals (the same size): one allocation, the normals start at the aligned size of the positions
            const VkDeviceSize pos_bytes = ((VkDeviceSize)d->mVertexCount * 16 + 255) / 256 * 256;
            const VkDeviceSize bytes = pos_bytes * (skin_normals_ok(*d) ? 2 : 1);
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
            LLVkWorker::markBufferUsed(d->mVb);         // the compute pass and the build read them when the GPU gets here
            LLVkWorker::markBufferUsed(d->mIb);
            if (d->mNotedTag != 0 && d->mNotedTag + 2 >= av_tag && d->mSkinnedTag != av_tag)
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
                if (skin_normals_ok(*d) && d->mOut.mSize >= 2 * (((VkDeviceSize)d->mVertexCount * 16 + 255) / 256 * 256))
                {
                    const U64 nsrc = d->mVi.mAddress + d->mAlpha.mNormalOffset, ndst = d->mOut.mAddress + ((VkDeviceSize)d->mVertexCount * 16 + 255) / 256 * 256 - 16ull * d->mVertexFirst;
                    sj.mNormalSrc[0] = (U32)nsrc;
                    sj.mNormalSrc[1] = (U32)(nsrc >> 32);
                    sj.mNormalDst[0] = (U32)ndst;
                    sj.mNormalDst[1] = (U32)(ndst >> 32);
                }
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
                // an avatar's skinned vertices stay within kSkinReach of where it stands (the whole of a rigged attachment, however large, is a few tens of metres)
                if (a.mHasPos)
                {
                    sj.mGuard[0] = a.mPos[0];
                    sj.mGuard[1] = a.mPos[1];
                    sj.mGuard[2] = a.mPos[2];
                    sj.mGuard[3] = sDbg.mSkinReach;
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
        job.mFastBuild = sDbg.mSkinFast;
        job.mInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | (job.mFastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
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
            sDeadRecent.erase(job.mNewAddress);
            job.mInfo.dstAccelerationStructure = job.mNewAs;
            job.mScratchSize = sizes.buildScratchSize;
            sBlasStorageBytes += job.mNewStorage.mSize;
        }
        job.mScratchOffset = (scratch_total + align - 1) / align * align;
        scratch_total = job.mScratchOffset + job.mScratchSize;
        skin_verts_left -= total_verts;
        a.mLastVerts = total_verts;
        a.mLastTris = prims;
        a.mLastTag = av_tag;
        a.mSig = sig;
        a.mBuiltIdx.clear();
        for (SkinDraw* d : cur)
        {
            a.mBuiltIdx.push_back((U32)(d - a.mDraws.data()));
        }
        a.mProcessedTag = av_tag;
        a.mLastBuildTag = av_tag;
        a.mLastWasRefit = job.mUpdate;
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
    const U32 table_call = std::min(call, sTableCalls - 1);
    GpuGeom* table = (GpuGeom*)((U8*)tlas.mTable.mMapped + (size_t)table_call * sRegionBytes);
    GpuMaterial* mat_table = sMatsInit ? (GpuMaterial*)((U8*)table + sGeomBytes) : nullptr;
    const U64 mat_table_address = sMatsInit ? (U64)tlas.mTable.mAddress + (U64)table_call * sRegionBytes + sGeomBytes : 0;
    const U32 region_index = ctx.getFrameSlot() * kCalls + table_call;
    U32 records = 0;
    sTableOffset = (U32)(table_call * sRegionBytes);
    if (sDbg.mAvatarLog)
    {
        // which scene call is which (the main view, a probe face, a snapshot): the camera of every call, in the avatar log's file (rt-avatars.log, "SCENECALL" lines)
        static FILE* call_log = fopen("<work>/rt-avatars.log", "ab");
        if (call_log)
        {
            const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            float cam_here[3];
            for (int i = 0; i < 3; ++i)
            {
                cam_here[i] = -(view[i * 4 + 0] * view[12] + view[i * 4 + 1] * view[13] + view[i * 4 + 2] * view[14]);
            }
            fprintf(call_log, "%lld frame %llu SCENECALL %u camera %.2f %.2f %.2f forward %.3f %.3f %.3f\n", wall, (unsigned long long)frame_now, call, cam_here[0], cam_here[1], cam_here[2], -view[2], -view[6], -view[10]);
            fflush(call_log);
        }
    }

    // the camera in the scene's space, and the origin the instance transforms are relative to (a point on a 32 m grid near the camera, moved when the camera gets 48 m away: close to
    // the camera the numbers stay small, and everything is made again only when it moves)
    float cam[3];
    for (int i = 0; i < 3; ++i)
    {
        cam[i] = -(view[i * 4 + 0] * view[12] + view[i * 4 + 1] * view[13] + view[i * 4 + 2] * view[14]);
    }
    {
        const float dx = cam[0] - sOrigin[0], dy = cam[1] - sOrigin[1], dz = cam[2] - sOrigin[2];
        // 300 m, not 48: a second scene call in a frame (a reflection probe, a snapshot) has its own camera, often far from the main one; with a tight limit the two took the origin
        // from each other at every call and the whole cache was made again each time. Float precision at a few hundred metres (a few 1e-5 m) is plenty for the rays.
        if (!sOriginValid || dx * dx + dy * dy + dz * dz > 300.f * 300.f)
        {
            static U32 origin_logged = 0;
            if (origin_logged++ < 12)
            {
                LL_INFOS("VkRt") << "scene origin moves: camera " << cam[0] << "," << cam[1] << "," << cam[2] << " was " << dx << "," << dy << "," << dz << " from the origin (call " << call << " of frame " << frame_now << ")" << LL_ENDL;
            }
            for (int i = 0; i < 3; ++i)
            {
                sOrigin[i] = roundf(cam[i] / 32.f) * 32.f;      // the NEAREST grid point: with floor, a camera in the far corner of its cell is 55 m from the point, over the 48 m limit, and the whole scene was made again at every call
            }
            sOriginValid = true;
            sForceFull = true;
            ++sStats.mForceWhy[1];
        }
    }
    if (sAlphaOnSeen != alpha_on)
    {
        sAlphaOnSeen = alpha_on;
        sForceFull = true;
        ++sStats.mForceWhy[2];
    }
    if (sMatsSeen != sDbg.mMats)
    {
        sMatsSeen = sDbg.mMats;
        sForceFull = true;
        ++sStats.mForceWhy[3];
    }
    if (sRecs.size() > sMaxRecords - 4096 - 2048 || sRecGarbage * 2 > sRecs.size() + 64)
    {
        sForceFull = true;                      // compaction
        ++sStats.mForceWhy[4];
    }

    // one group's instance and alpha records, made from what it is now
    auto refresh_group = [&](Group& g)
    {
        g.mDirty = false;
        if (g.mSkip || (g.mKey.mKind == 1 && !alpha_on))
        {
            drop_entry(g, 1);
            return;
        }
        const Blas& b = *g.mBlas;
        VkDeviceAddress address = b.mAddress;
        const Job* job = b.mJobCall == sSceneCalls ? b.mJob : nullptr;
        if (job && !job->mUpdate)
        {
            address = job->mNewAddress;         // a structure that is being (re)built now is part of this frame's scene: its new address is used
        }
        if (!address)
        {
            drop_entry(g, 2);
            return;
        }
        if (g.mSlot == ~0u && sInstArray.size() + 512 >= kInstanceCapacity)
        {
            ++sStats.mOverCap;
            return;
        }
        const F32* m = g.mModel;
        {
            bool bad = false;
            for (int k = 0; k < 16; ++k)
            {
                if (!(m[k] > -1e6f && m[k] < 1e6f))
                {
                    bad = true;
                }
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
                drop_entry(g, 3);
                return;
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
                drop_entry(g, 4);
                return;
            }
            F32 scale = 0.f;
            for (int c = 0; c < 3; ++c)
            {
                scale = std::max(scale, sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]));
            }
            if (scale > sStats.mMaxScale)
            {
                sStats.mMaxScale = scale;
                sStats.mMaxScaleKey = g.mKey;
            }
        }
        // masked structures: one record per geometry, in the order of the structure's geometry list (the texture slots are put in at every call)
        std::vector<GpuGeom> recs;
        std::vector<std::array<U32, 4>> names;
        if (g.mKey.mKind == 1 || (g.mKey.mKind == 0 && sDbg.mMats))
        {
            const std::vector<Range>& set = job ? job->mSet : b.mBuilt;
            LLVkWorker::BufferInfo lvi, lii;
            if (g.mNotedTag == scan_tag)
            {
                lvi = g.mVi;
                lii = g.mIi;
            }
            else if (!LLVkWorker::getBufferInfo(g.mKey.mVb, lvi) || !LLVkWorker::getBufferInfo(g.mKey.mIb, lii))
            {
                ++sStats.mNoData;
                drop_entry(g, 5);
                return;
            }
            if (!lvi.mAddress || !lii.mAddress)
            {
                ++sStats.mNoData;
                drop_entry(g, 5);
                return;
            }
            if (sRecs.size() + set.size() > sMaxRecords - 4096)
            {
                ++sStats.mTableFull;
                drop_entry(g, 6);
                return;
            }
            for (const Range& r : set)
            {
                GpuGeom rec;
                memset(&rec, 0, sizeof(rec));
                auto ait = b.mAlpha.find(r.id());
                const AlphaInfo none;
                const AlphaInfo& a = ait != b.mAlpha.end() ? ait->second : none;
                const U64 pa = lvi.mAddress, ia = lii.mAddress;
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
                    const U64 cc = pa + a.mColorOffset;
                    rec.mColors[0] = (U32)cc;
                    rec.mColors[1] = (U32)(cc >> 32);
                }
                rec.mFlags = a.mFlags;
                if (a.mNormalOffset)
                {
                    const U64 nn = pa + a.mNormalOffset;
                    rec.mNormals[0] = (U32)nn;
                    rec.mNormals[1] = (U32)(nn >> 32);
                    rec.mFlags |= kRecNormals;
                }
                if (g.mKey.mKind == 0)
                {
                    rec.mFlags |= kRecOpaque;
                }
                rec.mMaterial = a.mMaterial;
                ++(a.mMaterial ? sStats.mRecMaterial : sStats.mRecNoMaterial);
                rec.mCutoff = a.mCutoff;
                rec.mFirst = r.mFirst;
                memcpy(rec.mUv, a.mUv, sizeof(rec.mUv));
                recs.push_back(rec);
                names.push_back({ a.mTextures[0], a.mTextures[1], a.mTextures[2], a.mTextures[3] });
            }
            g.mCacheVb = lvi.mAddress;
            g.mCacheIb = lii.mAddress;
        }
        // the new rows take their pins before the old rows of this group give theirs back: a texture that stays in use keeps its pin and its slot (no new lookup)
        std::vector<std::array<U32, 4>> pins(recs.size(), std::array<U32, 4>{ 0u, 0u, 0u, 0u });
        if (sMatsInit && sDbg.mMats)
        {
            // the textures of the rows become pins: the rows hold pin indices, no slot is looked up per frame
            for (size_t i = 0; i < recs.size(); ++i)
            {
                bool any = false;
                for (int k = 0; k < 4; ++k)
                {
                    if (names[i][k])
                    {
                        pins[i][k] = LLVkWorker::rtPinAcquire(names[i][k]);
                        ++sStats.mPinsHeld;
                        any = true;
                    }
                }
                if (any)
                {
                    recs[i].mSlots[0] = (pins[i][0] & 0xFFFFu) | (pins[i][1] << 16);
                    recs[i].mSlots[1] = (pins[i][2] & 0xFFFFu) | (pins[i][3] << 16);
                    recs[i].mFlags |= kRecPinned;
                }
            }
        }
        // install: the old rows become garbage, the new ones go to the end
        if (g.mRecCount)
        {
            for (U32 k = 0; k < g.mRecCount; ++k)
            {
                release_row(g.mRecFirst + k);
            }
            sRecGarbage += g.mRecCount;
            g.mRecCount = 0;
        }
        U32 custom_index = 0;
        if (!recs.empty())
        {
            g.mRecFirst = (U32)sRecs.size();
            g.mRecCount = (U32)recs.size();
            sRecs.insert(sRecs.end(), recs.begin(), recs.end());
            for (U32 k = 0; k < g.mRecCount; ++k)
            {
                sRecLog.touch(g.mRecFirst + k);
            }
            sRecNames.insert(sRecNames.end(), names.begin(), names.end());
            sRecPins.insert(sRecPins.end(), pins.begin(), pins.end());
            custom_index = 1 + g.mRecFirst;     // record 0 of the table is the header
        }
        VkAccelerationStructureInstanceKHR inst = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                inst.transform.matrix[row][col] = m[col * 4 + row];
            }
            inst.transform.matrix[row][3] -= sOrigin[row];
        }
        inst.instanceCustomIndex = custom_index;
        inst.mask = 1u << std::min<U32>(g.mKey.mKind, 7u);
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = address;
        if (g.mSlot == ~0u)
        {
            g.mSlot = (U32)sInstArray.size();
            sInstArray.push_back(inst);
            sInstGroup.push_back(&g);
        }
        else
        {
            sInstArray[g.mSlot] = inst;
        }
        ++sStats.mRefreshed;
    };
    auto make_everything_again = [&]()
    {
        sForceFull = false;
        sInstArray.clear();
        sInstGroup.clear();
        for (size_t row = 0; row < sRecPins.size(); ++row)
        {
            release_row(row);
        }
        sRecs.clear();
        sRecNames.clear();
        sRecPins.clear();
        sRecGarbage = 0;
        sRecLog.reset();
        sDirtyKeys.clear();
        for (auto& kv : sGroups)
        {
            Group& g = kv.second;
            g.mSlot = ~0u;
            g.mRecCount = 0;
            g.mDirty = false;
        }
        for (auto& kv : sGroups)
        {
            refresh_group(kv.second);
        }
        ++sStats.mFullRebuilds;
    };
    auto t_sub = std::chrono::steady_clock::now();
    auto sub_lap = [&](int k) { const auto now = std::chrono::steady_clock::now(); sStats.mSubMs[k] += std::chrono::duration<double, std::milli>(now - t_sub).count(); t_sub = now; };
    if (sForceFull)
    {
        make_everything_again();
    }
    else
    {
        // a watched buffer (the alpha records hold its address) changed its storage since the last call: look at the records of the masked groups again
        const U64 topology = LLVkWorker::bufferTopologyVersion();
        if (topology != sTopoSeen)
        {
            sTopoSeen = topology;
            ++sStats.mTopologyChecks;
            auto check_group = [&](Group* gp)
            {
                if ((gp->mKey.mKind == 1 || (gp->mKey.mKind == 0 && sDbg.mMats)) && gp->mSlot != ~0u && !gp->mDirty)
                {
                    LLVkWorker::BufferInfo tvi, tii;
                    if (!LLVkWorker::getBufferInfo(gp->mKey.mVb, tvi) || !LLVkWorker::getBufferInfo(gp->mKey.mIb, tii) || tvi.mAddress != gp->mCacheVb || tii.mAddress != gp->mCacheIb)
                    {
                        ++sStats.mDirtyWhy[4];
                        mark_dirty(*gp);
                    }
                }
            };
            std::vector<unsigned> changed_names;
            if (LLVkWorker::takeTopologyChanges(changed_names))
            {
                for (unsigned name : changed_names)
                {
                    auto range = sNameGroups.equal_range(name);
                    for (auto nit = range.first; nit != range.second; ++nit)
                    {
                        auto git = sGroups.find(nit->second);
                        if (git != sGroups.end())
                        {
                            check_group(&git->second);
                        }
                    }
                }
            }
            else
            {
                for (Group* gp : sInstGroup)
                {
                    check_group(gp);
                }
            }
        }
        sub_lap(5);
        std::vector<Key> dirty;
        dirty.swap(sDirtyKeys);
        for (const Key& k : dirty)
        {
            auto git = sGroups.find(k);
            if (git != sGroups.end())
            {
                refresh_group(git->second);
            }
        }
    }
    sub_lap(0);
    if (sDbg.mVerify && ++sVerifyCounter % 120 == 0)
    {
        // the check: what the incremental updates left, against a cache made again from scratch from the same groups (groups matched by their key)
        struct Row { VkAccelerationStructureInstanceKHR mInst; std::vector<GpuGeom> mRecs; };
        std::unordered_map<Key, Row, KeyHash> before;
        for (Group* gp : sInstGroup)
        {
            Row row;
            row.mInst = sInstArray[gp->mSlot];
            for (U32 k = 0; k < gp->mRecCount; ++k)
            {
                row.mRecs.push_back(sRecs[gp->mRecFirst + k]);
            }
            before.emplace(gp->mKey, std::move(row));
        }
        make_everything_again();
        U64 groups = 0, mismatched = 0;
        for (Group* gp : sInstGroup)
        {
            ++groups;
            auto it = before.find(gp->mKey);
            const char* why = nullptr;
            if (it == before.end())
            {
                why = "was missing";
            }
            else
            {
                // the position of a group's rows in the table (the custom index) is not part of what the cache has to keep: a layout made from scratch has no garbage
                VkAccelerationStructureInstanceKHR a = it->second.mInst, b = sInstArray[gp->mSlot];
                a.instanceCustomIndex = b.instanceCustomIndex = 0;
                if (memcmp(a.transform.matrix, b.transform.matrix, sizeof(a.transform.matrix)) != 0)
                {
                    why = "instance transform differs";
                }
                else if (a.accelerationStructureReference != b.accelerationStructureReference)
                {
                    why = "structure address differs";
                }
                else if (a.mask != b.mask || a.flags != b.flags)
                {
                    why = "mask or flags differ";
                }
                else if (it->second.mRecs.size() != gp->mRecCount)
                {
                    why = "number of records differs";
                }
                else
                {
                    for (U32 k = 0; k < gp->mRecCount && !why; ++k)
                    {
                        const GpuGeom& ra = it->second.mRecs[k];
                        const GpuGeom& rb = sRecs[gp->mRecFirst + k];
                        if (memcmp(ra.mPositions, rb.mPositions, sizeof(ra.mPositions)) != 0 || memcmp(ra.mIndices, rb.mIndices, sizeof(ra.mIndices)) != 0)
                        {
                            why = "record buffer address differs";
                        }
                        else if (memcmp(ra.mTexcoords, rb.mTexcoords, sizeof(ra.mTexcoords)) != 0 || memcmp(ra.mColors, rb.mColors, sizeof(ra.mColors)) != 0)
                        {
                            why = "record texcoord or colour address differs";
                        }
                        else if (ra.mFlags != rb.mFlags || ra.mCutoff != rb.mCutoff || ra.mFirst != rb.mFirst || memcmp(ra.mUv, rb.mUv, sizeof(ra.mUv)) != 0)
                        {
                            why = "record flags, cutoff, first index or uv differ";
                        }
                    }
                }
            }
            if (why)
            {
                ++mismatched;
                static U32 logged_mismatch = 0;
                if (logged_mismatch++ < 30)
                {
                    LL_WARNS("VkRt") << "scene cache check: group vb " << gp->mKey.mVb << " ib " << gp->mKey.mIb << " kind " << gp->mKey.mKind << " matrix id " << gp->mKey.mMatrixId << ": " << why << LL_ENDL;
                }
            }
        }
        const U64 missing_now = before.size() > groups ? before.size() - groups : 0;
        mismatched += missing_now;
        ++sStats.mVerifies;
        sStats.mVerifyGroups += groups;
        sStats.mVerifyMismatch += mismatched;
        LL_INFOS("VkRt") << "scene cache check: " << groups << " groups made again from scratch, " << mismatched << " differed from the incrementally kept ones (" << missing_now << " of them missing now)" << LL_ENDL;
    }
    sub_lap(1);
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(sInstArray.size() + 256);
    instances.assign(sInstArray.begin(), sInstArray.end());
    std::vector<U64> who;                       // where each instance came from (flight recorder)
    if (diag_on())
    {
        who.reserve(sInstArray.size() + 256);
        for (size_t i = 0; i < sInstArray.size(); ++i)
        {
            who.push_back(i < sInstGroup.size() ? (U64)(uintptr_t)sInstGroup[i] : 0ull);
        }
    }
    sub_lap(2);

    auto slot_lookup = [&](U32 name) -> U32
    {
        // many geometries share textures: a texture's slot is asked for once per frame
        const U32 h = (name * 2654435761u) >> 18;
        if (sSlotCacheTag != frame_now + 1 || sSlotCacheName[h] != name)
        {
            if (sSlotCacheTag != frame_now + 1)
            {
                memset(sSlotCacheName, 0, sizeof(sSlotCacheName));
                sSlotCacheTag = frame_now + 1;
            }
            sSlotCacheName[h] = name;
            ++sStats.mSlotMisses;
            sSlotCacheSlot[h] = slot_of ? (slot_of(name) & 0xFFFFu) : 0u;
        }
        return sSlotCacheSlot[h];
    };
    {
        // the table: the header (how a ray moves from eye space into the scene's space), then the cached records with this frame's texture slots
        const float header_words[12] = { view[0], view[4], view[8], view[1], view[5], view[9], view[2], view[6], view[10], cam[0] - sOrigin[0], cam[1] - sOrigin[1], cam[2] - sOrigin[2] };
        memset(&table[0], 0, sizeof(GpuGeom));
        memcpy(&table[0], header_words, sizeof(header_words));
        table[0].mNormals[0] = (U32)mat_table_address;          // where the material rows of this call are
        table[0].mNormals[1] = (U32)(mat_table_address >> 32);
        const bool pins_on = mat_table && sDbg.mMats;
        U32* pin_table = pins_on ? (U32*)((U8*)mat_table + kMatBytes) : nullptr;
        if (pins_on)
        {
            // the pinned textures: those that changed are looked at again, the pin table (slot by pin index) gets what changed since this region was written; its address is in the header
            LLVkWorker::rtPinUpdate();
            const U64 pa = mat_table_address + kMatBytes;
            const U32 words[2] = { (U32)pa, (U32)(pa >> 32) };
            memcpy(table[0].mUv, words, sizeof(words));
        }
        const size_t n = sRecs.size();
        auto tbl_t0 = std::chrono::steady_clock::now();
        auto tbl_lap = [&](int k) { const auto now = std::chrono::steady_clock::now(); sStats.mTblMs[k] += std::chrono::duration<double, std::milli>(now - tbl_t0).count(); tbl_t0 = now; };
        // this frame's texture slots go into the kept rows; only a changed word is logged (and so written to the region)
        for (size_t r = 0; r < n && !pins_on; ++r)
        {
            if (prefetch && sDbg.mPfA)
            {
                for (int stage = 0; stage < 2; ++stage)
                {
                    const size_t ahead = r + (stage == 0 ? sDbg.mPfA : sDbg.mPfB);
                    if (ahead < n)
                    {
                        const std::array<U32, 4>& an = sRecNames[ahead];
                        for (int k = 0; k < 4; ++k)
                        {
                            if (an[k])
                            {
                                prefetch(an[k], (U32)stage);
                            }
                        }
                    }
                }
            }
            const std::array<U32, 4>& nm = sRecNames[r];
            if (!(nm[0] | nm[1] | nm[2] | nm[3]))
            {
                continue;
            }
            U32 slot[4] = {};
            for (U32 k = 0; k < 4; ++k)
            {
                if (nm[k] && slot_of)
                {
                    slot[k] = slot_lookup(nm[k]);
                }
            }
            const U32 w0 = slot[0] | (slot[1] << 16), w1 = slot[2] | (slot[3] << 16);
            if (sRecs[r].mSlots[0] != w0 || sRecs[r].mSlots[1] != w1)
            {
                sRecs[r].mSlots[0] = w0;
                sRecs[r].mSlots[1] = w1;
                sRecLog.touch((U32)r);
            }
        }
        tbl_lap(0);
        sStats.mRecRowsWritten += apply_rows(sRecLog, sRecRegion[region_index], sRecs, table + 1, sDbg.mDelta);
        tbl_lap(1);
        if (sDbg.mTableVerify && n && memcmp(table + 1, sRecs.data(), n * sizeof(GpuGeom)) != 0)
        {
            ++sStats.mTableMismatch;
            static U32 logged_table = 0;
            if (logged_table++ < 20)
            {
                LL_WARNS("VkRt") << "table verify: the geometry records of region " << region_index << " differ from the kept rows (" << n << " rows, epoch " << sRecLog.mEpoch << ")" << LL_ENDL;
            }
        }
        if (pins_on)
        {
            tbl_lap(2);
            sStats.mPinWrites += apply_rows(sPinLog, sPinRegion[region_index], sPinSlots, pin_table, sDbg.mDelta);
            if (sDbg.mTableVerify && !sPinSlots.empty() && memcmp(pin_table, sPinSlots.data(), sPinSlots.size() * sizeof(U32)) != 0)
            {
                ++sStats.mTableMismatch;
                static U32 logged_pins = 0;
                if (logged_pins++ < 20)
                {
                    LL_WARNS("VkRt") << "table verify: the pin table of region " << region_index << " differs from the kept entries (" << sPinSlots.size() << " entries)" << LL_ENDL;
                }
            }
            tbl_lap(2);
            sStats.mMatRowsWritten += apply_rows(sMatLog, sMatRegion[region_index], sMatRows, mat_table, sDbg.mDelta);
            tbl_lap(3);
            if (sDbg.mTableVerify && !sMatRows.empty() && memcmp(mat_table, sMatRows.data(), sMatRows.size() * sizeof(GpuMaterial)) != 0)
            {
                ++sStats.mTableMismatch;
                static U32 logged_mat = 0;
                if (logged_mat++ < 20)
                {
                    LL_WARNS("VkRt") << "table verify: the material rows of region " << region_index << " differ from the kept rows (" << sMatRows.size() << " rows)" << LL_ENDL;
                }
            }
        }
        records = 1 + (U32)n;
        sStats.mAlphaStructs += n - std::min<size_t>(n, sRecGarbage);
        {
            const U64 live = n - std::min<size_t>(n, sRecGarbage);
            sStats.mMaskedMin = std::min<U64>(sStats.mMaskedMin, live);
            sStats.mMaskedMax = std::max<U64>(sStats.mMaskedMax, live);
        }
    }
    sub_lap(3);
    auto avatar_skipped = [&](const Avatar& a, U32 id, const char* why)
    {
        if (sDbg.mAvatarLog && a.mSelf)
        {
            static FILE* skip_log = fopen("<work>/rt-avatars.log", "ab");
            if (skip_log)
            {
                const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                fprintf(skip_log, "%lld frame %llu SKIP avatar %u self 1 why %s named_age %llu address %d built_geometries %zu draws %zu excluded dead %u unnamed %u old %u unskinned %u noout %u keepoff %u bufchanged %u badrange %u slot_clears %u processed_tag_age %lld\n", wall, (unsigned long long)frame_now, id, why, (unsigned long long)(av_tag - a.mNotedTag),
                        a.mAddressNow ? 1 : 0, a.mBuiltIdx.size(), a.mDraws.size(), a.mExcl[0], a.mExcl[1], a.mExcl[2], a.mExcl[3], a.mExcl[4], a.mExcl[5], a.mExcl[6], a.mExcl[7], a.mSlotClears, (long long)(av_tag - a.mProcessedTag));
                fflush(skip_log);
            }
        }
    };
    for (auto& kv : sAvatars)
    {
        Avatar& a = kv.second;
        if (!a.mAddressNow || av_tag > a.mNotedTag + keep || !sDbg.mRigged || a.mBuiltIdx.empty())
        {
            avatar_skipped(a, kv.first, !a.mAddressNow ? "no structure yet" : (av_tag > a.mNotedTag + keep ? "not named for too long" : (!sDbg.mRigged ? "switched off" : "no geometries")));
            continue;
        }
        if (instances.size() >= kInstanceCapacity)
        {
            ++sStats.mOverCap;
            break;
        }
        // masked geometries need a record each (the shader finds it by the geometry's place in the structure): what the alpha test reads, at the buffers' addresses of now
        bool any_alpha = false, any_material = false;
        for (U32 idx : a.mBuiltIdx)
        {
            any_alpha = any_alpha || (idx < a.mDraws.size() && a.mDraws[idx].mKind == 1);
            any_material = any_material || (sDbg.mMats && idx < a.mDraws.size() && a.mDraws[idx].mAlpha.mMaterial != 0);
        }
        U32 custom_index = 0;
        if (any_alpha || any_material)
        {
            if (records + a.mBuiltIdx.size() > sMaxRecords)
            {
                ++sStats.mTableFull;
                avatar_skipped(a, kv.first, "geometry table full");
                continue;
            }
            std::vector<LLVkWorker::BufferInfo> vis(a.mBuiltIdx.size()), iis(a.mBuiltIdx.size());
            std::vector<char> usable(a.mBuiltIdx.size(), 1);
            for (size_t k = 0; k < a.mBuiltIdx.size(); ++k)
            {
                const SkinDraw& d = a.mDraws[a.mBuiltIdx[k]];
                if (d.mKind != 1 && !(sDbg.mMats && d.mAlpha.mMaterial))
                {
                    continue;
                }
                bool ok = !d.mDead && d.mOut.valid();
                ++a.mDraws[a.mBuiltIdx[k]].mRecCalls;
                if (ok && d.mNotedTag == av_tag)
                {
                    vis[k] = d.mVi;
                    iis[k] = d.mIi;
                }
                else if (ok)
                {
                    ok = LLVkWorker::getBufferInfo(d.mVb, vis[k]) && LLVkWorker::getBufferInfo(d.mIb, iis[k]) && vis[k].mEpoch == d.mVi.mEpoch && iis[k].mEpoch == d.mIi.mEpoch;
                }
                ok = ok && vis[k].mAddress && iis[k].mAddress;
                if (!ok)
                {
                    usable[k] = 0;                  // the buffers of this draw changed since it was named: its alpha test is skipped (candidates rejected), the rest of the avatar stays
                    ++sStats.mNoData;
                    ++a.mDraws[a.mBuiltIdx[k]].mRecBad;
                }
            }
            custom_index = records;
            for (size_t k = 0; k < a.mBuiltIdx.size(); ++k)
            {
                SkinDraw& d = a.mDraws[a.mBuiltIdx[k]];
                // the record is built on the stack and copied once (the table is mapped device memory: scattered stores into it are slow); the copy happens at every exit of the iteration
                GpuGeom rec;
                memset(&rec, 0, sizeof(rec));
                struct Commit { GpuGeom* mDst; const GpuGeom* mSrc; ~Commit() { memcpy(mDst, mSrc, sizeof(GpuGeom)); } } commit{ &table[records++], &rec };
                const bool with_material = sDbg.mMats && d.mAlpha.mMaterial != 0;
                if (d.mKind != 1 && !with_material)
                {
                    continue;                       // an opaque geometry: its candidates never reach the alpha test
                }
                if (!usable[k])
                {
                    rec.mFlags = 256u;              // the shader rejects the candidates without reading anything
                    continue;
                }
                if (d.mMarkedTag != av_tag)
                {
                    d.mMarkedTag = av_tag;
                    LLVkWorker::markBufferUsed(d.mVb);  // the alpha test reads texcoords, colours and indices while the lighting runs
                    LLVkWorker::markBufferUsed(d.mIb);
                }
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
                rec.mFlags = d.mAlpha.mFlags | kRecSkinned;
                rec.mCutoff = d.mAlpha.mCutoff;
                rec.mFirst = d.mFirst;
                memcpy(rec.mUv, d.mAlpha.mUv, sizeof(rec.mUv));
                if (with_material)
                {
                    // ray scene materials: the texture is in the material row; the normals are the triangles' own (the skinned copy holds positions only)
                    rec.mMaterial = d.mAlpha.mMaterial;
                    ++sStats.mRecMaterial;
                    if (skin_normals_ok(d) && d.mOut.mSize >= 2 * (((VkDeviceSize)d.mVertexCount * 16 + 255) / 256 * 256))
                    {
                        // the compute pass skinned the normals too: smooth shading of the avatar (the triangles' own normals shaded it in facets)
                        const U64 nn = d.mOut.mAddress + ((VkDeviceSize)d.mVertexCount * 16 + 255) / 256 * 256 - 16ull * d.mVertexFirst;
                        rec.mNormals[0] = (U32)nn;
                        rec.mNormals[1] = (U32)(nn >> 32);
                        rec.mFlags |= kRecNormals;
                    }
                    if (d.mKind != 1)
                    {
                        rec.mFlags |= kRecOpaque;
                    }
                }
                else
                {
                    ++sStats.mRecNoMaterial;
                }
            }
            if (any_alpha)
            {
                ++sStats.mAlphaStructs;
            }
        }
        {
            ++sStats.mAvatarInstances;
            const U64 age = av_tag - a.mNotedTag;               // frames since a shadow pass named a draw of it
            const U64 data_age = av_tag - a.mLastBuildTag;      // frames since its structure was refitted (the pose it shows)
            if (data_age > 0)
            {
                ++sStats.mAvatarStale;
                sStats.mAvatarStaleFrames += data_age;
                sStats.mAvatarStaleMax = std::max<U64>(sStats.mAvatarStaleMax, data_age);
            }
            if (sDbg.mAvatarLog)
            {
                static FILE* av_log = fopen("<work>/rt-avatars.log", "ab");
                if (av_log)
                {
                    const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    fprintf(av_log, "%lld frame %llu call %u avatar %u self %d named_age %llu data_age %llu over_budget %d processed %d refit %d draws %zu built_draws %zu pos %.1f %.1f %.1f\n", wall, (unsigned long long)frame_now, call, kv.first, a.mSelf ? 1 : 0,
                            (unsigned long long)age, (unsigned long long)data_age, a.mOverTag == av_tag ? 1 : 0, a.mProcessedTag == av_tag ? 1 : 0, a.mLastWasRefit ? 1 : 0, a.mDraws.size(), a.mBuiltIdx.size(), a.mPos[0], a.mPos[1], a.mPos[2]);
                }
            }
        }
        VkAccelerationStructureInstanceKHR inst = {};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                inst.transform.matrix[row][col] = row == col ? 1.f : 0.f;       // the skinned vertices are in world space: the instance is just the move to the scene's origin
            }
            inst.transform.matrix[row][3] = -sOrigin[row];
        }
        inst.instanceCustomIndex = custom_index;
        inst.mask = 1u | (any_alpha ? 2u : 0u) | 4u;        // opaque (bit 0) and masked (bit 1) casters, what the shadow rays trace; bit 2 marks avatars (mode 4 shows only these)
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = a.mAddressNow;
        instances.push_back(inst);
        if (diag_on())
        {
            who.push_back((1ull << 63) | (U64)kv.first);
        }
    }

    table[0].mFirst = records;      // the header says how many records the table holds: the shader leaves a record index past it alone
    sub_lap(4);
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
            auto owner = sGroups.find(j.mKey);
            if (owner != sGroups.end())
            {
                ++sStats.mDirtyWhy[5];
                mark_dirty(owner->second);      // its cached instance names the structure that is thrown away
            }
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
    if (diag_on() && !sDeadRecent.empty())
    {
        // no instance may name a structure that was given up lately (see sDeadRecent)
        size_t kept = 0;
        U32 removed = 0;
        for (size_t i = 0; i < instances.size(); ++i)
        {
            auto dead = sDeadRecent.find(instances[i].accelerationStructureReference);
            if (dead != sDeadRecent.end())
            {
                static U32 logged = 0;
                if (logged++ < 30)
                {
                    LL_WARNS("VkRt") << "instance " << i << " of " << instances.size() << " names the structure 0x" << std::hex << (unsigned long long)instances[i].accelerationStructureReference << std::dec
                                     << " that was given up " << (sSceneCalls - dead->second) << " scene calls ago: left out of the scene" << LL_ENDL;
                }
                ++removed;
                continue;
            }
            who[kept] = who[i];
            instances[kept++] = instances[i];
        }
        if (removed)
        {
            instances.resize(kept);
            who.resize(kept);
            sStats.mDeadRefs += removed;
        }
    }
    const VkDeviceSize instance_offset = (VkDeviceSize)call * 64ull * tlas.mCapacity;
    if (!instances.empty())
    {
        memcpy((U8*)tlas.mInstances.mMapped + instance_offset, instances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instances.size());
    }
    if (sDbg.mAvatarLog)
    {
        static FILE* inst_log = fopen("<work>/rt-avatars.log", "ab");
        if (inst_log)
        {
            const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            size_t avatar_inst = 0, masked_inst = 0;
            for (const VkAccelerationStructureInstanceKHR& inst : instances)
            {
                avatar_inst += (inst.mask & 4u) ? 1 : 0;
                masked_inst += (inst.mask & 2u) ? 1 : 0;
            }
            fprintf(inst_log, "%lld frame %llu SCENEINST %u instances %zu (avatars %zu, with masked geometry %zu) groups %zu records %u\n", wall, (unsigned long long)frame_now, call, instances.size(), avatar_inst, masked_inst, sGroups.size(), records);
            fflush(inst_log);
        }
    }
    if (sDbg.mInstDump)
    {
        static int dumped = 0;
        static FILE* dump_log = fopen("<work>/rt-instances.log", "ab");
        if (dump_log && dumped < 40)
        {
            ++dumped;
            const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            fprintf(dump_log, "CALL %lld frame %llu call %u camera %.2f %.2f %.2f instances %zu%s", wall, (unsigned long long)frame_now, call, cam[0], cam[1], cam[2], instances.size(), "\n");
            for (const VkAccelerationStructureInstanceKHR& inst : instances)
            {
                fprintf(dump_log, "%llx %.2f %.2f %.2f %u %u%s", (unsigned long long)inst.accelerationStructureReference, inst.transform.matrix[0][3], inst.transform.matrix[1][3], inst.transform.matrix[2][3], inst.mask, inst.instanceCustomIndex, "\n");
            }
            fflush(dump_log);
        }
    }
    if (diag_on())
    {
        // flight recorder (see sRecent): a pass over the instances, a few microseconds per thousand
        SceneRecord& r = sRecent[sRecentNext++ % 6];
        r.mFrame = frame_now;
        r.mCall = call;
        r.mSceneCall = sSceneCalls;
        r.mInstances = (U32)instances.size();
        r.mCapacity = tlas.mCapacity;
        r.mJobs = (U32)jobs.size();
        r.mSkinChunks = (U32)skin_chunks.size();
        r.mRecords = records;
        r.mBadTransforms = r.mZeroRefs = r.mUnknownRefs = 0;
        r.mMaxTranslation = r.mMaxScale = 0.f;
        r.mScratchNeeded = scratch_needed;
        r.mInstanceOffset = instance_offset;
        for (int i = 0; i < 3; ++i)
        {
            r.mOrigin[i] = sOrigin[i];
            r.mCamera[i] = cam[i];
        }
        r.mWho = who;
        r.mRefs.clear();
        r.mRefs.reserve(instances.size());
        for (const VkAccelerationStructureInstanceKHR& inst : instances)
        {
            bool bad = false;
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    const float v = inst.transform.matrix[row][col];
                    if (!std::isfinite(v))
                    {
                        bad = true;
                    }
                    else if (col == 3)
                    {
                        r.mMaxTranslation = std::max(r.mMaxTranslation, fabsf(v));
                    }
                    else
                    {
                        r.mMaxScale = std::max(r.mMaxScale, fabsf(v));
                    }
                }
            }
            r.mBadTransforms += bad ? 1u : 0u;
            r.mZeroRefs += inst.accelerationStructureReference == 0 ? 1u : 0u;
            r.mRefs.push_back(inst.accelerationStructureReference);
        }
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
        {
            // range check: how much of the skinned vertex spans do the draws' indices actually use (only draws whose index data the CPU has: host-written buffers), and who the volume belongs to
            std::vector<char> seen(65536);
            U64 draws = 0, draws_data = 0, span = 0, span_data = 0, referenced = 0, index_span = 0, tris = 0, tris_data = 0;
            for (const SkinDraw* d : skin_job_draws)
            {
                if (!d)
                {
                    continue;
                }
                ++draws;
                span += d->mVertexCount;
                tris += d->mCount / 3;
                if (d->mIi.mCpu && ((U64)d->mFirst + d->mCount) * 2 <= d->mIi.mSize)
                {
                    ++draws_data;
                    span_data += d->mVertexCount;
                    tris_data += d->mCount / 3;
                    std::fill(seen.begin(), seen.end(), 0);
                    const U16* idx = (const U16*)d->mIi.mCpu + d->mFirst;
                    U32 lo = 0xFFFFu, hi = 0, distinct = 0;
                    for (U32 k = 0; k < d->mCount; ++k)
                    {
                        const U32 v = idx[k];
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                        if (!seen[v])
                        {
                            seen[v] = 1;
                            ++distinct;
                        }
                    }
                    referenced += distinct;
                    index_span += hi >= lo ? hi - lo + 1 : 0;
                }
            }
            LL_INFOS("VkRt") << "skin range check: " << draws << " skinned draws, " << span << " vertices in their ranges, " << tris << " triangles; of them " << draws_data << " draws have index data on the CPU: "
                             << span_data << " vertices in the ranges, " << index_span << " between the lowest and the highest index used, " << referenced << " really referenced, " << tris_data << " triangles" << LL_ENDL;
            std::vector<std::pair<U64, const Avatar*>> big;
            for (const auto& kv : sAvatars)
            {
                if (kv.second.mLastTag + 2 >= av_tag)
                {
                    big.emplace_back(kv.second.mLastVerts, &kv.second);
                }
            }
            std::sort(big.begin(), big.end(), [](const std::pair<U64, const Avatar*>& l, const std::pair<U64, const Avatar*>& r) { return l.first > r.first; });
            for (size_t k = 0; k < big.size() && k < 8; ++k)
            {
                const Avatar& a = *big[k].second;
                float dist = -1.f;
                if (a.mHasPos)
                {
                    const float ex = view[0] * a.mPos[0] + view[4] * a.mPos[1] + view[8] * a.mPos[2] + view[12];
                    const float ey = view[1] * a.mPos[0] + view[5] * a.mPos[1] + view[9] * a.mPos[2] + view[13];
                    const float ez = view[2] * a.mPos[0] + view[6] * a.mPos[1] + view[10] * a.mPos[2] + view[14];
                    dist = sqrtf(ex * ex + ey * ey + ez * ez);
                }
                LL_INFOS("VkRt") << "skin range check: avatar " << a.mKey.mVb << (a.mSelf ? " (self)" : "") << " kind " << a.mAvatarKind << ((a.mAvatarKind & 1) ? " [animated object]" : "") << ((a.mAvatarKind & 2) ? " [impostor]" : "") << ((a.mAvatarKind & 8) ? " [jelly doll]" : "") << ((a.mAvatarKind & 16) ? " [invisible]" : "") << ((a.mAvatarKind & 32) ? " [too slow]" : "") << ": " << a.mLastVerts << " skinned vertices, " << a.mLastTris << " triangles, " << a.mDraws.size() << " draws, distance " << dist << " m" << LL_ENDL;
            }
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
    // Compaction (static structures only; the avatars' structures refit in place). The sizes asked for by the builds of this frame slot's last use are read now (its fence has been waited for
    // when the slot's frame began): a structure that would shrink by 15 % or more is copied into an allocation of the compacted size, recorded in this call's command buffer after the builds.
    // The swap (and, through the dirty mark, the instance) follows once the copy is recorded; the old structure goes the way of every given-up one (destroyLater: after the frame that may still use it).
    struct Compaction
    {
        Key mKey;
        Blas* mBlas = nullptr;
        VkAccelerationStructureKHR mOldAs = VK_NULL_HANDLE, mNewAs = VK_NULL_HANDLE;
        VkDeviceAddress mNewAddress = 0;
        ArenaRef mNewStorage;
    };
    std::vector<Compaction> compactions;
    const U32 compact_slot = ctx.getFrameSlot();
    const bool compact_ok = sDbg.mCompact && sCompactPool[compact_slot] != VK_NULL_HANDLE;
    bool compact_reset = false;
    if (compact_ok && sCompactFrame[compact_slot] != ctx.getFrameNumber() + 1)
    {
        sCompactFrame[compact_slot] = ctx.getFrameNumber() + 1;
        compact_reset = true;
        std::vector<PendingCompact>& pending = sCompactPending[compact_slot];
        if (!pending.empty())
        {
            std::vector<U64> sizes(pending.size(), 0);
            if (vkGetQueryPoolResults(dev, sCompactPool[compact_slot], 0, (U32)pending.size(), sizes.size() * sizeof(U64), sizes.data(), sizeof(U64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                for (size_t i = 0; i < pending.size() && compactions.size() < 64; ++i)
                {
                    const PendingCompact& pc = pending[i];
                    auto it = sBlas.find(pc.mKey);
                    if (it == sBlas.end())
                    {
                        continue;
                    }
                    Blas& b = it->second;
                    // still the structure that was asked about, untouched since (an update makes the answer wrong), not being rebuilt in this call
                    if (b.mAs != pc.mAs || b.mAddress != pc.mAddress || b.mRefits != 0 || b.mJobCall == sSceneCalls || b.mCompacted || sizes[i] == 0)
                    {
                        continue;
                    }
                    if (sizes[i] * 100 > b.mStorage.mSize * 85)
                    {
                        ++sStats.mCompactSkipped;       // not worth a copy
                        continue;
                    }
                    Compaction c;
                    c.mKey = pc.mKey;
                    c.mBlas = &b;
                    c.mOldAs = b.mAs;
                    if (!ctx.arenaAlloc(sizes[i], 256, ArenaKind::Accel, c.mNewStorage))
                    {
                        ++sStats.mCompactFailed;
                        continue;
                    }
                    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
                    ci.buffer = c.mNewStorage.mBuffer;
                    ci.offset = c.mNewStorage.mOffset;
                    ci.size = sizes[i];
                    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                    if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &c.mNewAs) != VK_SUCCESS)
                    {
                        ctx.arenaFree(c.mNewStorage);
                        ++sStats.mCompactFailed;
                        continue;
                    }
                    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
                    ai.accelerationStructure = c.mNewAs;
                    c.mNewAddress = vkGetAccelerationStructureDeviceAddressKHR(dev, &ai);
                    compactions.push_back(c);
                }
            }
            pending.clear();
        }
        sCompactNext[compact_slot] = 0;
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
        if (compact_reset)
        {
            vkCmdResetQueryPool(cmd, sCompactPool[compact_slot], 0, kCompactQueries);
        }
        // earlier work (last frame's traces, the shadow passes that wrote the vertex data) is done before anything is built over it
        full_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        if (!skin_chunks.empty())
        {
            // the compute pass writes the vertices the builds below read (and the alpha test reads later): one dispatch, every job of the call
            ctx.gpuStamp(Context::stampTag("rt scene skin"));        // a fault between this stamp and "rt scene builds" is in the skinning dispatch
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
        if (compact_ok)
        {
            // the sizes the builds above would have when compacted (they are final: the barrier above made the builds visible)
            for (Job& j : jobs)
            {
                if (j.mCompact && !j.mUpdate && sCompactNext[compact_slot] < kCompactQueries)
                {
                    j.mQuery = sCompactNext[compact_slot]++;
                    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &j.mNewAs, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, sCompactPool[compact_slot], j.mQuery);
                }
            }
            if (!compactions.empty())
            {
                for (const Compaction& c : compactions)
                {
                    VkCopyAccelerationStructureInfoKHR ci = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
                    ci.src = c.mOldAs;
                    ci.dst = c.mNewAs;
                    ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                    vkCmdCopyAccelerationStructureKHR(cmd, &ci);
                }
                full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            }
        }
        ctx.gpuStamp(Context::stampTag("rt scene tlas"));
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tinfo, tranges);
        full_barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    });
    if (!recorded)
    {
        // no frame is being recorded: undo the new structures, the groups are named again when the passes run
        for (Compaction& c : compactions)
        {
            vkDestroyAccelerationStructureKHR(dev, c.mNewAs, nullptr);
            ctx.arenaFree(c.mNewStorage);
        }
        undo_jobs();
        return;
    }
    tlas.mBuiltFrame = frame_now;       // (this slot's structure and table belong to this command buffer from here on)
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
            b.mFastBuild = j.mFastBuild;
            b.mCompacted = false;
            ++sStats.mBuilds;
            sStats.mPrimsBuilt += j.mPrims;
        }
        b.mBuilt = j.mSet;
        b.mVbVersion = j.mVbVersion;
        b.mIbVersion = j.mIbVersion;
        b.mPrims = j.mPrims;
        b.mUpdateScratch = j.mUpdateScratch;
    }
    if (compact_ok)
    {
        // what this call built and asked the compacted size of: read when this frame slot comes round again
        for (Job& j : jobs)
        {
            if (j.mCompact && !j.mUpdate && j.mQuery != ~0u)
            {
                PendingCompact pc;
                pc.mKey = j.mKey;
                pc.mAs = j.mNewAs;
                pc.mAddress = j.mNewAddress;
                sCompactPending[compact_slot].push_back(pc);
            }
        }
        // the copies are recorded: the structures are the compacted ones from here (the instances follow at the next call, through the dirty mark)
        for (Compaction& c : compactions)
        {
            Blas& b = *c.mBlas;
            ++sStats.mCompacted;
            sStats.mCompactBefore += b.mStorage.mSize;
            sStats.mCompactAfter += c.mNewStorage.mSize;
            sDeadRecent[b.mAddress] = sSceneCalls;
            VkAccelerationStructureKHR old_as = b.mAs;
            ctx.destroyLater([=]() { vkDestroyAccelerationStructureKHR(dev, old_as, nullptr); });     // before its storage: deferred calls run in order
            sBlasStorageBytes -= std::min<U64>(sBlasStorageBytes, b.mStorage.mSize);
            ctx.arenaFree(b.mStorage);
            b.mAs = c.mNewAs;
            b.mAddress = c.mNewAddress;
            b.mStorage = c.mNewStorage;
            b.mCompacted = true;
            sBlasStorageBytes += b.mStorage.mSize;
            auto owner = sGroups.find(c.mKey);
            if (owner != sGroups.end())
            {
                mark_dirty(owner->second);
            }
        }
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
    unsigned long long pin_total = 0, pin_resolves = 0, pin_events = 0;
    LLVkWorker::rtPinCounts(pin_total, pin_resolves, pin_events);
    char buf[3600];
    snprintf(buf, sizeof(buf),
             "rt scene (mode %u): %u structures (%.0f MB, %.2f M triangles), instances %.0f/scene (last %u), casters named %.0f/frame, builds %.2f/frame (%.0f tri), refits %.2f/frame (%.0f tri), "
             "deferred %.2f, no data %.2f, masked structures %.0f/frame, table full %.2f, over instance cap %.2f, failed %.3f, evicted %.3f, scene calls %.2f/frame (over %u: %.2f), "
             "CPU %.3f ms/call (worst %.2f ms; expire %.2f, order %.2f, scan+sort %.2f, instances %.2f, record %.2f), scratch %.1f MB; builds because: new %.2f, names recycled %.2f, ranges grew %.2f, index data rewritten %.2f, refit limit %.2f; skin: %.1f jobs/frame, %.0f vertices/frame, avatars %.2f/frame (over budget %.2f, throttled by distance %.2f), failed %.3f, avatar instances %.2f/call of which showing an older pose %.2f (on average %.1f frames old, oldest %llu); casters new %.2f/frame, groups held %zu; scene cache: %zu instances, %zu record rows (%u garbage), refreshed %.1f groups/call, made again %.3f/call, buffer moves checked %.3f/call, check: %llu runs, %llu groups, %llu differed; checks: structures checked %.2f/frame, bad vertex data %.3f, bad matrices %.3f, singular matrices %.3f, copy vs video memory compared %.2f mismatched %.2f, largest instance scale %.2f (vb %u ib %u matrix id %u kind %u)",
             shadowMode(), live, (double)sBlasStorageBytes / 1048576.0, (double)prims / 1e6, (double)s.mInstances / scenes, sLastInstances, (double)s.mCasters / f, (double)s.mBuilds / f,
             (double)s.mPrimsBuilt / f, (double)s.mRefits / f, (double)s.mPrimsRefit / f, (double)s.mDeferred / f, (double)s.mNoData / f, (double)s.mAlphaStructs / f, (double)s.mTableFull / f,
             (double)s.mOverCap / f, (double)s.mFailed / f, (double)s.mEvicted / f, (double)s.mScenes / f, kCalls, (double)s.mExtraCalls / f, s.mCpuMs / scenes, s.mCpuMsMax, s.mPhaseMs[0] / scenes, s.mPhaseMs[1] / scenes, s.mPhaseMs[2] / scenes, s.mPhaseMs[3] / scenes, s.mPhaseMs[4] / scenes,
             (double)sScratch.mSize / 1048576.0, (double)s.mWhyNew / f, (double)s.mWhyEpoch / f, (double)s.mWhyGrew / f, (double)s.mWhyIb / f, (double)s.mWhyRefitLimit / f,
             (double)s.mSkinJobs / f, (double)s.mSkinVerts / f, (double)s.mAvatars / f, (double)s.mSkinOver / f, (double)s.mSkinThrottled / f, (double)s.mSkinFailed / f, (double)s.mAvatarInstances / scenes, (double)s.mAvatarStale / scenes, s.mAvatarStale ? (double)s.mAvatarStaleFrames / (double)s.mAvatarStale : 0.0, (unsigned long long)s.mAvatarStaleMax, (double)s.mNewGroups / f, sGroups.size(), sInstArray.size(), sRecs.size(), sRecGarbage, (double)s.mRefreshed / scenes, (double)s.mFullRebuilds / scenes, (double)s.mTopologyChecks / scenes, (unsigned long long)s.mVerifies, (unsigned long long)s.mVerifyGroups, (unsigned long long)s.mVerifyMismatch, (double)s.mChecked / f, (double)s.mBadVertices / f, (double)s.mBadMatrices / f, (double)s.mSingular / f, (double)s.mCompared / f, (double)s.mMismatched / f, s.mMaxScale, s.mMaxScaleKey.mVb, s.mMaxScaleKey.mIb,
             s.mMaxScaleKey.mMatrixId, s.mMaxScaleKey.mKind);
    {
        size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, ", instances split: refresh %.2f, verify %.2f, copy %.2f, table %.2f, avatars %.2f", s.mSubMs[0] / scenes, s.mSubMs[1] / scenes, s.mSubMs[2] / scenes, s.mSubMs[3] / scenes, s.mSubMs[4] / scenes);
        used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, "; tables: %s, records written %.0f/call, material rows defined %zu (updates %.2f/frame, written %.0f/call), rows made with a material %.1f without %.1f /frame, pins %llu (resolves %.1f events %.1f /frame, taken %llu given back %llu, table entries written %.1f/call), verify mismatches %llu, table ms/call: record slots %.2f write %.2f, pins %.2f write %.2f, cache made again because: eviction %llu, origin %llu, alpha %llu, materials %llu, compaction %llu",
                 sMatsInit ? (sDbg.mMats ? "materials on" : "materials off live") : "no materials", (double)s.mRecRowsWritten / scenes, sMatRows.size(), (double)s.mMatUpdates / f, (double)s.mMatRowsWritten / scenes,
                 (double)s.mRecMaterial / f, (double)s.mRecNoMaterial / f, pin_total, (double)pin_resolves / f, (double)pin_events / f, (unsigned long long)s.mPinsHeld, (unsigned long long)s.mPinsDropped, (double)s.mPinWrites / scenes, (unsigned long long)s.mTableMismatch, s.mTblMs[0] / scenes, s.mTblMs[1] / scenes, s.mTblMs[2] / scenes, s.mTblMs[3] / scenes,
                 (unsigned long long)s.mForceWhy[0], (unsigned long long)s.mForceWhy[1], (unsigned long long)s.mForceWhy[2], (unsigned long long)s.mForceWhy[3], (unsigned long long)s.mForceWhy[4]);
        used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, " (refresh = move check %.2f + dirty groups), slot lookups/call %.0f, dirty/call: moved %.1f, address %.1f, alpha info %.1f, new range %.1f, move check %.1f, structure %.1f", s.mSubMs[5] / scenes, (double)s.mSlotMisses / scenes, (double)s.mDirtyWhy[0] / scenes, (double)s.mDirtyWhy[1] / scenes, (double)s.mDirtyWhy[2] / scenes, (double)s.mDirtyWhy[3] / scenes, (double)s.mDirtyWhy[4] / scenes, (double)s.mDirtyWhy[5] / scenes);
    }
    {
        const size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, ", tables sizes: groups %zu, seen rows %zu (free %zu), name entries %zu, structures %zu", sGroups.size(), sSeenRows.size(), sSeenFree.size(), sNameGroups.size(), sBlas.size());
    }
    if (s.mChecked)
    {
        const size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, ", range check CPU %.4f ms/frame (%.2f structures/frame, %llu bad ranges in total)", (double)s.mCheckUs / 1000.0 / f, (double)s.mChecked / f, (unsigned long long)s.mBadVertices);
    }
    if (s.mScanChecks)
    {
        const size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, ", candidate scan check: %llu runs, %llu wrong", (unsigned long long)s.mScanChecks, (unsigned long long)s.mScanMismatch);
    }
    {
        const size_t used = strlen(buf);
        snprintf(buf + used, sizeof(buf) - used, ", structure memory ceiling %llu MB (new structures held back by it: %.2f/frame), instances left out for naming a dead structure: %llu", (unsigned long long)env_u64("LL_VK_RT_MAX_MB", 1536), (double)s.mCeilingHeld / f, (unsigned long long)s.mDeadRefs);
    }
    std::string text = buf;
    {
        char cbuf[400];
        snprintf(cbuf, sizeof(cbuf), ", compaction (%s): %llu structures replaced by a compacted copy, %.0f MB -> %.0f MB, kept as they were (not worth it) %llu, failed %llu", sDbg.mCompact ? "on" : "off", (unsigned long long)s.mCompacted,
                 (double)s.mCompactBefore / 1048576.0, (double)s.mCompactAfter / 1048576.0, (unsigned long long)s.mCompactSkipped, (unsigned long long)s.mCompactFailed);
        text += cbuf;
    }
    {
        // why instances left the scene: leaves flickering on a camera turn show as masked drops and as a dip of the live alpha records
        static const char* const why[8] = { "expired", "switched off", "no structure", "bad matrix", "singular matrix", "no buffer data", "records table full", "other" };
        char dbuf[600];
        size_t used = (size_t)snprintf(dbuf, sizeof(dbuf), ", instances taken out of the scene: masked");
        for (int k = 0; k < 8; ++k)
        {
            used += (size_t)snprintf(dbuf + used, sizeof(dbuf) - used, " %s %llu,", why[k], (unsigned long long)s.mDropped[1][k]);
        }
        used += (size_t)snprintf(dbuf + used, sizeof(dbuf) - used, " opaque");
        for (int k = 0; k < 8 && used < sizeof(dbuf) - 40; ++k)
        {
            used += (size_t)snprintf(dbuf + used, sizeof(dbuf) - used, " %s %llu,", why[k], (unsigned long long)s.mDropped[0][k]);
        }
        snprintf(dbuf + used, sizeof(dbuf) - used, " (live alpha records per call %llu..%llu)", (unsigned long long)(s.mMaskedMin == ~0ull ? 0 : s.mMaskedMin), (unsigned long long)s.mMaskedMax);
        text += dbuf;
    }
    if (s.mProbeCount)
    {
        char pbuf[500];
        const double mb = 1.0 / 1048576.0;
        snprintf(pbuf, sizeof(pbuf), ", size probe (%llu new structures): flags used %.1f MB, without ALLOW_UPDATE %.1f MB (%.0f %%), without ALLOW_UPDATE and PREFER_FAST_BUILD %.1f MB (%.0f %%), ALLOW_UPDATE + PREFER_FAST_BUILD %.1f MB (%.0f %%)",
                 (unsigned long long)s.mProbeCount, s.mProbeNow * mb, s.mProbeNoUpdate * mb, 100.0 * (double)s.mProbeNoUpdate / (double)std::max<U64>(1, s.mProbeNow), s.mProbeNoUpdateFast * mb,
                 100.0 * (double)s.mProbeNoUpdateFast / (double)std::max<U64>(1, s.mProbeNow), s.mProbeFast * mb, 100.0 * (double)s.mProbeFast / (double)std::max<U64>(1, s.mProbeNow));
        text += pbuf;
    }
    return text;
}

} // namespace Rt
} // namespace LLVk

#endif // LL_VULKAN
