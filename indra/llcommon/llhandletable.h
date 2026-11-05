/**
 * @file llhandletable.h
 * @brief A stable 32-bit handle for an object that render-side data refers to, instead of a counted pointer.
 *
 * doc/vulkan/PLAN-clean-core.md step C2. A face, a draw info or a row that holds a handle writes no reference count and shares no ownership; it resolves the handle where it needs the object.
 * A handle is generation << kIndexBits | index (0 = none). The slot array has a fixed capacity (no reallocation under readers), so resolve() is lock-free and safe from any thread; acquire()
 * and release() take a mutex (an object gets a handle once, when something first asks for it, and gives it back in its destructor).
 *
 * resolve() returns the object while its handle is current and nullptr after the object was destroyed (the generation moved on; a recycled slot never answers an old handle). It does NOT keep the
 * object alive: a thread that uses the returned pointer must be sure the object outlives its use. That guarantee is the job of the retire epoch (C2 design, PLAN-clean-core.md): objects whose
 * last owner lets go are destroyed after the frames that could still read them are done. Until a holder of handles exists, nothing depends on it.
 *
 * Independent of the viewer on purpose (the texture core must be reusable by a native renderer): T is only a tag and a pointer type.
 */

#ifndef LL_LLHANDLETABLE_H
#define LL_LLHANDLETABLE_H

#include "stdtypes.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>

template <typename T, U32 INDEX_BITS = 18>
class LLHandleTable
{
public:
    static_assert(INDEX_BITS >= 8 && INDEX_BITS <= 24, "index bits");
    static constexpr U32 kIndexBits = INDEX_BITS;
    static constexpr U32 kCapacity = 1u << INDEX_BITS;
    static constexpr U32 kIndexMask = kCapacity - 1;
    static constexpr U32 kGenMask = (1u << (32 - INDEX_BITS)) - 1;

    // a handle for `obj` (never 0); 0 when the table is full. The caller stores it in the object and calls release() from the object's destructor.
    static U32 acquire(T* obj)
    {
        Table& t = table();
        std::lock_guard<std::mutex> lock(t.mMutex);
        U32 idx;
        if (!t.mFree.empty())
        {
            idx = t.mFree.back();
            t.mFree.pop_back();
        }
        else if (t.mNext < kCapacity)
        {
            idx = t.mNext++;
        }
        else
        {
            ++t.mFull;
            return 0;
        }
        U32 gen = t.mGen[idx].load(std::memory_order_relaxed);
        gen = (gen + 1) & kGenMask;
        if (gen == 0) gen = 1;                      // a handle is never 0
        t.mGen[idx].store(gen, std::memory_order_release);
        t.mPtr[idx].store(obj, std::memory_order_release);
        ++t.mLive;
        ++t.mAcquired;
        if (t.mLive > t.mPeak) t.mPeak = t.mLive;
        return (gen << kIndexBits) | idx;
    }

    // the object is going away: the slot answers nobody until it is handed out again, and then only to the new handle
    static void release(U32 handle)
    {
        const U32 idx = handle & kIndexMask;
        if (!handle || idx == 0 || idx >= kCapacity) return;
        Table& t = table();
        std::lock_guard<std::mutex> lock(t.mMutex);
        if (t.mGen[idx].load(std::memory_order_relaxed) != (handle >> kIndexBits)) return;   // already released
        U32 next_gen = ((handle >> kIndexBits) + 1) & kGenMask;
        if (next_gen == 0) next_gen = 1;
        t.mPtr[idx].store(nullptr, std::memory_order_release);
        t.mGen[idx].store(next_gen, std::memory_order_release);   // (the old handle no longer matches; acquire() moves it on once more before the slot answers again)
        t.mFree.push_back(idx);
        if (t.mLive) --t.mLive;
    }

    // lock-free; any thread. nullptr: no handle, a stale handle, or an object that was destroyed
    static T* resolve(U32 handle)
    {
        const U32 idx = handle & kIndexMask;
        if (!handle || idx == 0 || idx >= kCapacity) return nullptr;
        Table& t = table();
        const U32 gen = handle >> kIndexBits;
        if (t.mGen[idx].load(std::memory_order_acquire) != gen) return nullptr;
        T* p = t.mPtr[idx].load(std::memory_order_acquire);
        if (t.mGen[idx].load(std::memory_order_acquire) != gen) return nullptr;   // released while we read it
        return p;
    }

    struct Stats { U32 mLive, mPeak, mNext; U64 mAcquired, mFull; };
    static Stats stats()
    {
        Table& t = table();
        std::lock_guard<std::mutex> lock(t.mMutex);
        return { t.mLive, t.mPeak, t.mNext, t.mAcquired, t.mFull };
    }

private:
    struct Table
    {
        std::atomic<T*>* mPtr;
        std::atomic<U32>* mGen;
        std::mutex mMutex;
        std::vector<U32> mFree;
        U32 mNext = 1;          // index 0 is never used
        U32 mLive = 0, mPeak = 0;
        U64 mAcquired = 0, mFull = 0;
        Table()
        {
            // zeroed pages are not committed until touched: a table for 262144 objects costs what is used
            mPtr = static_cast<std::atomic<T*>*>(std::calloc(kCapacity, sizeof(std::atomic<T*>)));
            mGen = static_cast<std::atomic<U32>*>(std::calloc(kCapacity, sizeof(std::atomic<U32>)));
        }
    };
    static Table& table()
    {
        static Table* t = new Table;       // (intentionally never destroyed: objects released during exit must still find it)
        return *t;
    }
};

#endif // LL_LLHANDLETABLE_H
