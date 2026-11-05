/**
 * @file llgrouptable.h
 * @brief The group table: one row per octree group, the data the cull reads in one 224-byte row indexed by group row id.
 *
 * doc/vulkan/PLAN-clean-core.md step C1b. An octree group owns no copy of its bounds, extents, state word or per-camera stamps: it owns a row id, and the data is the row. The group's accessors (getBounds,
 * getState, isOcclusionState, ...) read the row, so callers stay as they are. A row holds what the walk of the octree reads together for one group: the four boxes (node bounds, object bounds, node extents,
 * object extents: 128 bytes), the state word, the visible stamp and the occlusion state of every camera (the per-view data is in the row by camera id, not shared between views), and the LOD hash. Rows are
 * contiguous in chunks of 1024 (allocated when the first row of a chunk is handed out, never moved), so a pass over many groups (a job, a GPU upload, the S7 flat table) reads rows, not groups.
 *
 * What is NOT here yet (C1b.2): the per-view caches of LLSpatialGroup (distance, depth, pixel area, view angle: written by whichever view culls the group), the draw data (C4/C5).
 *
 * One row per group, same rule as the instance table (llinstancetable.h): fields read together share a row. A released row keeps the state the owner chose (DEAD) and has no owner, so a stale pointer to a
 * destroyed group reads "dead" and ownedBy() tells it from the live owner. acquire() and release() take a mutex (a group gets its row when it is made and gives it back when it is destroyed); reads take no lock.
 *
 * Independent of the viewer classes on purpose: only plain types and LLVector4a. The camera count is a template-free constant checked against LLViewerCamera::NUM_CAMERAS where the group uses it.
 */

#ifndef LL_LLGROUPTABLE_H
#define LL_LLGROUPTABLE_H

#include "stdtypes.h"
#include "llvector4a.h"
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

class LLGroupRows
{
public:
    static constexpr U32 kCameras = 9;                      // LLViewerCamera::NUM_CAMERAS (static_assert in llvieweroctree.cpp)
    static constexpr U32 kCapacity = 1u << 19;              // 524,288 groups
    static constexpr U32 kChunkShift = 10;
    static constexpr U32 kChunkRows = 1u << kChunkShift;
    static constexpr U32 kChunks = kCapacity >> kChunkShift;

    struct Row
    {
        LLVector4a bounds[2];           // bounding box (center, size) of this node and all its children (tight fit to objects)
        LLVector4a objectBounds[2];     // bounding box (center, size) of objects in this node
        LLVector4a extents[2];          // extents (min, max) of this node and all its children
        LLVector4a objectExtents[2];    // extents (min, max) of objects in this node
        const void* owner;              // the group this row belongs to (nullptr while free)
        U32 state;                      // the group state word
        S32 anyVisible;                 // latest visible to any camera
        S32 lodHash;
        S32 visible[kCameras];          // per camera: the frame the group was last visible
        U32 occlusion[kCameras];        // per camera: the occlusion state
        // the view caches of LLSpatialGroup (C1b.2). Written by LLSpatialGroup::updateDistance, which accepts the world camera only (the shadow and water views never write them): one writer, so they
        // are not keyed by view. Read by the sort (distance, depth), the rebuild urgency, the alpha ordering (view angle) and the texture priority (pixel area).
        LLVector4a viewAngle;           // the view direction when the group was last re-sorted (alpha)
        LLVector4a lastUpdateViewAngle;
        F32 distance;
        F32 depth;
        F32 lastUpdateDistance;
        F32 lastUpdateTime;
        F32 pixelArea;
        F32 radius;
        F32 objectBoxSize;              // cached objectBounds[1].getLength3()
    };
    static_assert(sizeof(Row) == 288, "a row is 18 x 16 bytes");

    inline static Row** sChunks = nullptr;                  // kChunks pointers (null until the first row of the chunk is handed out)

    static Row& row(U32 id) { return sChunks[id >> kChunkShift][id & (kChunkRows - 1)]; }

    // a cleared row for a new group (bounds and extents 0, state 0, stamps 0). Row 0 is never handed out. 0 = the table is full (fatal for the caller).
    static U32 acquire(const void* owner)
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        U32 id;
        if (!s.mFree.empty())
        {
            id = s.mFree.back();
            s.mFree.pop_back();
        }
        else if (s.mNext < kCapacity)
        {
            id = s.mNext++;
        }
        else
        {
            ++s.mFull;
            return 0;
        }
        Row*& chunk = sChunks[id >> kChunkShift];
        if (!chunk)
        {
            chunk = static_cast<Row*>(std::calloc(kChunkRows, sizeof(Row)));      // (calloc: 16-byte aligned on the platforms we build for)
            if (!chunk)
            {
                ++s.mFull;
                s.mFree.push_back(id);
                return 0;
            }
        }
        Row& r = row(id);
        std::memset(&r, 0, sizeof(Row));
        r.owner = owner;
        ++s.mLive;
        ++s.mAcquired;
        if (s.mLive > s.mPeak) s.mPeak = s.mLive;
        return id;
    }

    static void release(U32 id, U32 state_on_release)
    {
        if (!id || id >= kCapacity) return;
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        Row& r = row(id);
        r.owner = nullptr;
        r.state = state_on_release;
        s.mFree.push_back(id);
        if (s.mLive) --s.mLive;
    }

    static bool ownedBy(U32 id, const void* owner)
    {
        return id && id < kCapacity && sChunks[id >> kChunkShift] && row(id).owner == owner;
    }

    struct Stats { U32 mLive, mPeak, mNext; U64 mAcquired, mFull; };
    static Stats stats()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        return { s.mLive, s.mPeak, s.mNext, s.mAcquired, s.mFull };
    }

private:
    struct State
    {
        std::mutex mMutex;
        std::vector<U32> mFree;
        U32 mNext = 1;
        U32 mLive = 0, mPeak = 0;
        U64 mAcquired = 0, mFull = 0;
        State()
        {
            sChunks = static_cast<Row**>(std::calloc(kChunks, sizeof(Row*)));
        }
    };
    static State& state()
    {
        static State* s = new State;                // (intentionally never destroyed: groups released during exit must still find it)
        return *s;
    }
};

#endif // LL_LLGROUPTABLE_H
