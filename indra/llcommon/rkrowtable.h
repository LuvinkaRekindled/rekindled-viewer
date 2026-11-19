/**
 * @file rkrowtable.h
 * @brief A table of rows: one row per owner object, handed out and given back, rows never move.
 *
 * doc/vulkan/PLAN-clean-core.md step C3. The generic form of the instance table (rkinstancetable.h) and the group table (rkgrouptable.h): RKRowTable<Row, Capacity, ChunkShift>. An owner (a face) gets a
 * row id, owns no copy of what the row holds, and its accessors read the row; the row is addressable by id from a job or, later, a GPU upload. Rows are contiguous in chunks that are allocated when the
 * first row of the chunk is handed out (cost = what is used), a row never moves (no lock for readers), acquire() / release() take a mutex.
 *
 * A released row is not reused at once: it waits kQuarantineFrames frames in a quarantine, where it reads what the owner chose (state_on_release) and has no owner, so a stale pointer to a destroyed
 * owner does not write into the row of an unrelated live owner. When a row leaves the quarantine its state and owner are checked: a mismatch (a stale write) is counted and logged.
 *
 * Row requirements: a plain struct with `const void* owner;` and `U32 state;` members (the canary), at most 64 KB; acquire() hands out a zeroed row with owner set.
 * Independent of the viewer classes: plain types only.
 */

#ifndef RK_RKROWTABLE_H
#define RK_RKROWTABLE_H

#include "stdtypes.h"
#include "llframetimer.h"
#include "llerror.h"
#include "llmemory.h"       // ll_aligned_malloc_64
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

template <class Row, U32 CAPACITY, U32 CHUNK_SHIFT>
class RKRowTable
{
public:
    static constexpr U32 kCapacity = CAPACITY;
    static constexpr U32 kChunkShift = CHUNK_SHIFT;
    static constexpr U32 kChunkRows = 1u << CHUNK_SHIFT;
    static constexpr U32 kChunks = CAPACITY >> CHUNK_SHIFT;
    static constexpr U32 kQuarantineFrames = 8;

    static Row& row(U32 id) { return state().mChunks[id >> kChunkShift][id & (kChunkRows - 1)]; }

    // a zeroed row for a new owner. Row 0 is never handed out. 0 = the table is full (the caller treats that as fatal)
    static U32 acquire(const void* owner)
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        reclaim(s);
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
        Row*& chunk = s.mChunks[id >> kChunkShift];
        if (!chunk)
        {
            chunk = static_cast<Row*>(ll_aligned_malloc_64(sizeof(Row) * kChunkRows));       // 64-byte aligned: a row of a multiple of 64 bytes starts on a cache line
            if (chunk) std::memset(static_cast<void*>(chunk), 0, sizeof(Row) * kChunkRows);
            if (!chunk)
            {
                ++s.mFull;
                s.mFree.push_back(id);
                return 0;
            }
        }
        Row& r = chunk[id & (kChunkRows - 1)];
        std::memset(static_cast<void*>(&r), 0, sizeof(Row));
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
        s.mCanary = state_on_release;
        s.mQuarantine.push_back({ id, LLFrameTimer::getFrameCount() });
        if (s.mLive) --s.mLive;
    }

    static bool ownedBy(U32 id, const void* owner)
    {
        State& s = state();
        return id && id < kCapacity && s.mChunks[id >> kChunkShift] && row(id).owner == owner;
    }

    struct Stats { U32 mLive, mPeak, mNext; U64 mAcquired, mFull; U32 mQuarantined; U64 mWrittenWhileFree; };
    static Stats stats()
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mMutex);
        return { s.mLive, s.mPeak, s.mNext, s.mAcquired, s.mFull, (U32)s.mQuarantine.size(), s.mWritten };
    }

private:
    struct State
    {
        std::mutex mMutex;
        Row** mChunks;
        std::vector<U32> mFree;
        struct Released { U32 mId; U32 mFrame; };
        std::deque<Released> mQuarantine;       // oldest first
        U32 mNext = 1;
        U32 mLive = 0, mPeak = 0;
        U32 mCanary = 0;
        U64 mAcquired = 0, mFull = 0, mWritten = 0;
        State() { mChunks = static_cast<Row**>(std::calloc(kChunks, sizeof(Row*))); }
    };
    static State& state()
    {
        static State* s = new State;            // (never destroyed: owners released during exit must still find it)
        return *s;
    }
    static void reclaim(State& s)
    {
        const U32 now = LLFrameTimer::getFrameCount();
        while (!s.mQuarantine.empty() && now - s.mQuarantine.front().mFrame >= kQuarantineFrames)
        {
            const U32 id = s.mQuarantine.front().mId;
            s.mQuarantine.pop_front();
            const Row& r = s.mChunks[id >> kChunkShift][id & (kChunkRows - 1)];
            if (r.state != s.mCanary || r.owner != nullptr)
            {
                if (++s.mWritten <= 20)
                {
                    LL_WARNS("RowTable") << "row " << id << " of a table of " << sizeof(Row) << "-byte rows changed while it was free: state " << r.state << " (left " << s.mCanary << "), owner " << r.owner
                                         << " (a stale pointer to a destroyed owner wrote); " << s.mWritten << " so far" << LL_ENDL;
                }
            }
            s.mFree.push_back(id);
        }
    }
};

#endif // RK_RKROWTABLE_H
