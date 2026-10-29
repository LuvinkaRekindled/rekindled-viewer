/**
 * @file llforkjoinpool.h
 * @brief Minimal fork-join worker pool shared by the parallel avatar motion blend and the parallel shadow cascade preparation
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * $/LicenseInfo$
 */

#ifndef LL_LLFORKJOINPOOL_H
#define LL_LLFORKJOINPOOL_H

#include <atomic>
#include <chrono>
#include <intrin.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// run() calls fn(0..count-1), spread over the workers and the calling thread, and only returns once every
// call has finished. While it runs the caller must not do anything else, so the callbacks have the world to
// themselves (the callbacks may run on the calling thread too: thread-local state they change must be put back).
class LLForkJoinPool
{
public:
    explicit LLForkJoinPool(unsigned num_workers)
    {
        for (unsigned i = 0; i < num_workers; ++i)
        {
            mThreads.emplace_back([this] { workerLoop(); });
        }
    }

    ~LLForkJoinPool()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStop.store(true);
        }
        mWake.notify_all();
        for (std::thread& thread : mThreads)
        {
            thread.join();
        }
    }

    size_t workerCount() const { return mThreads.size(); }
    static inline std::atomic<unsigned> sNestedRuns{ 0 };     // runs that found the pool busy and ran inline (a diagnostic: the report line prints it)

    void run(size_t count, const std::function<void(size_t)>& fn)
    {
        // A run inside a run (a job that reaches a path which runs the pool again, or a second thread) would reset the count, the claim word and the
        // remaining count under the outer run, whose caller then returns with jobs still writing into results it is about to free (2026-10-29: a crash
        // on closing a floater, a shadow prep mark job still running while generateSunShadow had moved on). Such a run executes on the calling thread.
        if (mBusy.exchange(true, std::memory_order_acq_rel))
        {
            ++sNestedRuns;
            for (size_t i = 0; i < count; ++i)
            {
                fn(i);
            }
            return;
        }
        struct BusyScope { std::atomic<bool>& b; ~BusyScope() { b.store(false, std::memory_order_release); } } busy_scope { mBusy };
        unsigned long long generation;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFn = &fn;
            mCount.store(count, std::memory_order_relaxed);
            mRemaining.store(count, std::memory_order_relaxed);
            generation = mGeneration.load(std::memory_order_relaxed) + 1;
            // the run's number (high 32 bits) and the next index to hand out (low 32 bits) are one word: a worker that is late from the run before cannot take an index of this one by mistake
            // (with the index and the count read apart, a stale index could be valid against the new count: that job ran twice, the count of finished jobs hit zero early, run() returned while
            // a job was still writing into results its caller was about to free)
            mClaim.store((generation & 0xFFFFFFFFull) << 32, std::memory_order_release);
            mGeneration.store(generation, std::memory_order_release);
        }
        mWake.notify_all();

        drain(generation);

        // the last jobs are usually a moment from done: spin for a short while before sleeping on the condition variable (a wake-up costs tens of microseconds)
        if (!spinUntil([this] { return mRemaining.load(std::memory_order_acquire) == 0; }, 100))
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mDone.wait(lock, [this] { return mRemaining == 0; });
        }
        mFn = nullptr;
    }

    // The pool the viewer shares between its parallel phases (they never overlap): null when the machine has too few
    // threads for it to be worth it. The calling thread works too, so one worker fewer than the budget.
    static LLForkJoinPool* shared()
    {
        static std::unique_ptr<LLForkJoinPool> pool;
        static bool checked = false;
        if (!checked)
        {
            checked = true;
            const unsigned hardware_threads = std::thread::hardware_concurrency();
            const unsigned budget = hardware_threads / 2 < 8u ? hardware_threads / 2 : 8u;
            const unsigned workers = (hardware_threads >= 8) ? budget - 1 : 0;
            if (workers > 0)
            {
                pool = std::make_unique<LLForkJoinPool>(workers);
            }
        }
        return pool.get();
    }

private:
    // true if the condition held within the given microseconds
    template <class F>
    static bool spinUntil(F&& condition, int microseconds)
    {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0;; ++i)
        {
            if (condition())
            {
                return true;
            }
            _mm_pause();
            if ((i & 31) == 31 && std::chrono::steady_clock::now() - start > std::chrono::microseconds(microseconds))
            {
                return condition();
            }
        }
    }

    void drain(unsigned long long generation)
    {
        const unsigned long long tag = generation & 0xFFFFFFFFull;
        for (;;)
        {
            unsigned long long claim = mClaim.load(std::memory_order_acquire);
            if ((claim >> 32) != tag)
            {
                return;                 // a later run has begun: this one is over
            }
            const size_t index = (size_t)(claim & 0xFFFFFFFFull);
            if (index >= mCount.load(std::memory_order_relaxed))
            {
                return;
            }
            if (!mClaim.compare_exchange_weak(claim, claim + 1, std::memory_order_acq_rel))
            {
                continue;
            }
            (*mFn)(index);
            if (mRemaining.fetch_sub(1) == 1)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mDone.notify_all();
            }
        }
    }

    void workerLoop()
    {
        unsigned long long seen_generation = 0;
        for (;;)
        {
            // the next run of a frame often follows within a moment (shadow culls, then render maps): spin briefly before sleeping
            if (!spinUntil([&] { return mStop.load(std::memory_order_relaxed) || mGeneration.load(std::memory_order_acquire) != seen_generation; }, 150))
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [&] { return mStop.load() || mGeneration.load(std::memory_order_acquire) != seen_generation; });
            }
            if (mStop.load())
            {
                return;
            }
            seen_generation = mGeneration.load(std::memory_order_acquire);
            drain(seen_generation);
        }
    }

    std::vector<std::thread> mThreads;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mDone;
    const std::function<void(size_t)>* mFn = nullptr;
    std::atomic<size_t> mCount{ 0 };
    std::atomic<unsigned long long> mClaim{ 0 };      // run number << 32 | next index
    std::atomic<size_t> mRemaining{ 0 };
    std::atomic<unsigned long long> mGeneration{ 0 };
    std::atomic<bool> mStop{ false };
    std::atomic<bool> mBusy{ false };
};

#endif // LL_LLFORKJOINPOOL_H
