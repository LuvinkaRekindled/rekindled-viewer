/**
 * @file llinstancetable.h
 * @brief The instance table: one row per drawable, the per-instance data in a 32-byte row indexed by instance id.
 *
 * doc/vulkan/PLAN-clean-core.md step C1a. A drawable owns no copy of its flag word, bounding radius, scale or render type: it owns an instance id, and the data is the row of that id. LLDrawable's
 * accessors read the row, so every existing caller stays as it is. What changes is where the data lives: contiguous, one writer per datum, addressable from a job or (later) uploadable to the GPU
 * as an instance buffer, and a row is the thing a face row (C3) will name by id.
 *
 * One row, not one array per field (the first version had a column per field: loop +1.25 ms in a lap, because an access that needs the flags, the render type and the radius touched three
 * cache lines instead of one). A row is 32 bytes: two rows to a cache line, all the fields a drawable access reads together in one of them.
 *
 * Fixed capacity (the rows are calloc'ed once, pages that were never touched cost nothing), so a row never moves and a reader on another thread needs no lock. acquire() and release() take a mutex
 * (a drawable gets its row when it is made and gives it back when it is destroyed). A recycled row is cleared by acquire(). A released row keeps a state the owner chose (flags_on_release) so a
 * stale pointer to a destroyed owner reads "dead", as the freed object's own memory used to, and its owner field is cleared so ownedBy() tells a stale pointer from the live owner.
 *
 * The transform (an LLXformMatrix: position, rotation, scale, world position and rotation, parent link, world matrix, bounds; about 190 bytes) is the second part of the row, in chunks of 4096 that are
 * allocated when the first row of the chunk is handed out (a table for 1M drawables costs what is used) and never move. It is built in place when the row is handed out and destroyed when it is
 * given back. It is a separate array from the 32-byte row because the two are read at different times (flags, radius, type: most loops; the transform: the move and matrix code).
 *
 * Independent of the viewer classes on purpose (the instance data must be reusable by a native renderer): only plain types.
 */

#ifndef LL_LLINSTANCETABLE_H
#define LL_LLINSTANCETABLE_H

#include "stdtypes.h"
#include "v3math.h"
#include "xform.h"
#include <new>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

class LLInstances
{
public:
    static constexpr U32 kCapacity = 1u << 20;      // ~1M drawables; 32 MB of address space, committed as touched

    struct Row
    {
        U32 flags;              // the drawable state word
        F32 radius;             // bounding radius (setRadius / getRadius)
        S32 renderType;         // pool / render type
        LLVector3 scale;        // the damped scale (getScale hands out a reference into the row)
        const void* owner;      // the object this row belongs to (nullptr while free): ownedBy()
    };
    static_assert(sizeof(Row) == 32, "two rows to a cache line");

    inline static Row* sRows = nullptr;

    static constexpr U32 kXformChunkShift = 12;
    static constexpr U32 kXformChunkRows = 1u << kXformChunkShift;
    static constexpr U32 kXformChunks = kCapacity >> kXformChunkShift;
    inline static LLXformMatrix** sXformChunks = nullptr;      // kXformChunks pointers (null until the first row of the chunk is handed out)

    static Row& row(U32 idx) { return sRows[idx]; }
    static LLXformMatrix& xform(U32 idx) { return sXformChunks[idx >> kXformChunkShift][idx & (kXformChunkRows - 1)]; }

    // a row for a new owner, cleared (flags 0, radius 0, render type 0, scale 1,1,1). Row 0 is never handed out. 0 = the table is full (the caller must treat that as fatal).
    static U32 acquire(const void* owner)
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        U32 idx;
        if (!s.mFree.empty())
        {
            idx = s.mFree.back();
            s.mFree.pop_back();
        }
        else if (s.mNext < kCapacity)
        {
            idx = s.mNext++;
        }
        else
        {
            ++s.mFull;
            return 0;
        }
        LLXformMatrix*& chunk = sXformChunks[idx >> kXformChunkShift];
        if (!chunk)
        {
            chunk = static_cast<LLXformMatrix*>(std::malloc(sizeof(LLXformMatrix) * kXformChunkRows));
            if (!chunk)
            {
                ++s.mFull;
                s.mFree.push_back(idx);
                return 0;
            }
        }
        new (&chunk[idx & (kXformChunkRows - 1)]) LLXformMatrix();
        Row& r = sRows[idx];
        r.flags = 0;
        r.radius = 0.f;
        r.renderType = 0;
        r.scale.mV[0] = r.scale.mV[1] = r.scale.mV[2] = 1.f;
        r.owner = owner;
        ++s.mLive;
        ++s.mAcquired;
        if (s.mLive > s.mPeak) s.mPeak = s.mLive;
        return idx;
    }

    static void release(U32 idx, U32 flags_on_release)
    {
        if (!idx || idx >= kCapacity) return;
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        xform(idx).~LLXformMatrix();
        Row& r = sRows[idx];
        r.flags = flags_on_release;
        r.owner = nullptr;
        s.mFree.push_back(idx);
        if (s.mLive) --s.mLive;
    }

    // true while `idx` is the live row of `owner` (false for a stale pointer to a destroyed owner, whose row is free or already someone else's). Any thread.
    static bool ownedBy(U32 idx, const void* owner)
    {
        return idx && idx < kCapacity && sRows[idx].owner == owner;
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
            sRows = static_cast<Row*>(std::calloc(kCapacity, sizeof(Row)));
            sXformChunks = static_cast<LLXformMatrix**>(std::calloc(kXformChunks, sizeof(LLXformMatrix*)));
        }
    };
    static State& state()
    {
        static State* s = new State;                // (intentionally never destroyed: drawables released during exit must still find it)
        return *s;
    }
};

#endif // LL_LLINSTANCETABLE_H
