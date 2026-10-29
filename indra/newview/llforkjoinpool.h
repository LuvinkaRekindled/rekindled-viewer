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
#include "llerror.h"

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
    // <FS> 2026-10-29 (a lap hung with every worker idle and the main thread waiting in run() for jobs that were all done): runs whose executed jobs were not the count asked for, and runs that were
    // given up on after a second of waiting with every job claimed and none in flight (the state is logged; the report line prints both; 0 and 0 are the expectation)
    static inline std::atomic<unsigned> sMismatchRuns{ 0 }, sRecoveredRuns{ 0 }, sUnderflows{ 0 };

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
        if (count >= kMaxJobs)
        {
            for (size_t i = 0; i < count; ++i)      // (never: a run is a few hundred jobs; the claim word has room for a million)
            {
                fn(i);
            }
            return;
        }
        unsigned long long generation;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFn = &fn;
            mCount.store(count, std::memory_order_relaxed);
            mRemaining.store(count, std::memory_order_relaxed);
            mExecuted.store(0, std::memory_order_relaxed);
            generation = mGeneration.load(std::memory_order_relaxed) + 1;
            // the run's number (24 bits), its job count (20 bits) and the next index to hand out (20 bits) are ONE word. 2026-10-29: the count used to be a word of its own, written just before the
            // claim word; a worker still draining the run before could see the new, larger count together with the old claim word (its last index, now below the count), claim that index
            // successfully and run it with the new run's function: a job that was then run a second time by the real claim, a count of finished jobs that hit zero while that second one
            // was still running (run() returned, the caller freed the results the job wrote into: the crash on closing a floater, this morning's lap), or a decrement after the count
            // was zero (wrapped: run() waiting for ever: the hang). With the count inside the word that is claimed, a claim is valid only for the run it names.
            mClaim.store(((generation & kTagMask) << kTagShift) | ((unsigned long long)count << kCountShift), std::memory_order_release);
            mGeneration.store(generation, std::memory_order_release);
        }
        mWake.notify_all();

        drain(generation);

        // the last jobs are usually a moment from done: spin for a short while before sleeping on the condition variable (a wake-up costs tens of microseconds)
        if (!spinUntil([this] { return mRemaining.load(std::memory_order_acquire) == 0; }, 100))
        {
            std::unique_lock<std::mutex> lock(mMutex);
            for (int waits = 1; !mDone.wait_for(lock, std::chrono::milliseconds(200), [this] { return mRemaining == 0; }); ++waits)
            {
                const unsigned long long claim = mClaim.load();
                const size_t claimed = (size_t)(claim & kIndexMask);
                if (waits == 1 || waits == 5)
                {
                    LL_WARNS("ForkJoin") << "run " << generation << " has waited " << waits * 200 << " ms: count " << count << ", next index " << claimed << ", remaining " << mRemaining.load()
                        << ", executed " << mExecuted.load() << ", in flight " << mInFlight.load() << ", claim tag " << (claim >> kTagShift) << " (run tag " << (generation & kTagMask) << ")" << LL_ENDL;
                }
                if (waits >= 5 && claimed >= count && mInFlight.load() == 0)
                {
                    // every job claimed and none running, yet the count says some are left: the count is wrong; the run is over
                    ++sRecoveredRuns;
                    LL_WARNS("ForkJoin") << "run " << generation << " given up on: every job claimed, none running, remaining " << mRemaining.load() << ", executed " << mExecuted.load() << " of " << count << LL_ENDL;
                    mRemaining.store(0);
                    break;
                }
            }
        }
        if (mExecuted.load(std::memory_order_acquire) != count)
        {
            if (++sMismatchRuns <= 8)
            {
                LL_WARNS("ForkJoin") << "run " << generation << " executed " << mExecuted.load() << " jobs for a count of " << count << LL_ENDL;
            }
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
        const unsigned long long tag = generation & kTagMask;
        for (;;)
        {
            unsigned long long claim = mClaim.load(std::memory_order_acquire);
            if (((claim >> kTagShift) & kTagMask) != tag)
            {
                return;                 // a later run has begun: this one is over
            }
            const size_t index = (size_t)(claim & kIndexMask);
            if (index >= (size_t)((claim >> kCountShift) & kIndexMask))
            {
                return;
            }
            if (!mClaim.compare_exchange_weak(claim, claim + 1, std::memory_order_acq_rel))
            {
                continue;
            }
            mInFlight.fetch_add(1, std::memory_order_relaxed);
            (*mFn)(index);
            mExecuted.fetch_add(1, std::memory_order_relaxed);
            mInFlight.fetch_sub(1, std::memory_order_relaxed);
            // (never below zero: a decrement too many would leave the count huge and run() waiting for ever)
            size_t remaining = mRemaining.load();
            do
            {
                if (remaining == 0)
                {
                    ++sUnderflows;
                    break;
                }
            } while (!mRemaining.compare_exchange_weak(remaining, remaining - 1));
            if (remaining == 1)
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

    // the claim word: run number (bits 40-63) | job count (20-39) | next index (0-19)
    static constexpr unsigned long long kTagMask = 0xFFFFFFull, kIndexMask = 0xFFFFFull;
    static constexpr unsigned kTagShift = 40, kCountShift = 20;
    static constexpr size_t kMaxJobs = 0xFFFFF;

    std::vector<std::thread> mThreads;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mDone;
    const std::function<void(size_t)>* mFn = nullptr;
    std::atomic<size_t> mCount{ 0 };
    std::atomic<unsigned long long> mClaim{ 0 };      // run number << 40 | job count << 20 | next index
    std::atomic<size_t> mRemaining{ 0 };
    std::atomic<size_t> mExecuted{ 0 };              // jobs finished in this run (diagnostic: must equal the count when run() ends)
    std::atomic<unsigned> mInFlight{ 0 };            // jobs being executed now
    std::atomic<unsigned long long> mGeneration{ 0 };
    std::atomic<bool> mStop{ false };
    std::atomic<bool> mBusy{ false };
};

#endif // LL_LLFORKJOINPOOL_H
