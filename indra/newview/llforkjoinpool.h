/**
 * @file llforkjoinpool.h
 * @brief Minimal fork-join worker pool shared by the parallel avatar motion blend, the parallel shadow cascade preparation and the parallel view stages
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

#include "llthread.h"
#include <string>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include "llvkplatform.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "llerror.h"

// A batch of jobs: fn(0..count-1) spread over the workers and the thread that joins it.
//
//   Handle h = pool->launch(count, fn);   // returns at once; the jobs start on the workers
//   ... the launching thread does other work ...
//   pool->join(h);                        // works through the jobs nobody has started, returns once every one has finished
//
// run(count, fn) is join(launch(count, fn)): the caller does nothing else while it runs (the shape every stage had before launch and join existed).
//
// Several batches can be in flight at once (up to kSlots; a batch launched while all slots are taken runs on the launching thread, inside launch). A job may launch and join a batch of its own. A join never
// deadlocks: the joining thread can run every job of its batch itself. fn must stay alive, and what the jobs write must stay alive, until the join has returned. Jobs run on the workers and on the joining
// thread: thread-local state a job changes has to be put back (ViewScope and the other task scopes do this).
class LLForkJoinPool
{
public:
    // what launch returns: join takes it. A default one (invalid) joins to nothing: the batch had no jobs, or ran inside launch.
    struct Handle
    {
        int mSlot = -1;
        unsigned long long mTag = 0;
        bool valid() const { return mSlot >= 0; }
    };

    explicit LLForkJoinPool(unsigned num_workers)
    {
        // LL_VK_POOL_LEGACY=1: the pool as it was before launch and join (one run at a time, a run inside a run on the calling thread), for an A/B in the same build (2026-10-31). launch runs its jobs inside the call then.
        const char* legacy = getenv("LL_VK_POOL_LEGACY");
        mLegacy = legacy && atoi(legacy) != 0;
        for (unsigned i = 0; i < num_workers; ++i)
        {
            if (mLegacy)
            {
                mThreads.emplace_back([this] { workerLoopLegacy(); });
            }
            else
            {
                mThreads.emplace_back([this] { workerLoop(); });
            }
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
    static inline std::atomic<unsigned> sNestedRuns{ 0 };     // launches that found every slot taken and ran their jobs inside launch (a diagnostic: the report line prints it)
    // <FS> 2026-10-29 (a lap hung with every worker idle and the main thread waiting in run() for jobs that were all done): batches whose executed jobs were not the count asked for, and batches that were
    // given up on after a second of waiting with every job claimed and none in flight (the state is logged; the report line prints both; 0 and 0 are the expectation)
    static inline std::atomic<unsigned> sMismatchRuns{ 0 }, sRecoveredRuns{ 0 }, sUnderflows{ 0 };

    // <FS> LL_VK_POOL_LABELS=1 (C4 measurement, doc/vulkan/PLAN-clean-core.md): the time the jobs of each named family take on ALL threads of a batch (the workers and the launching thread), per frame, logged
    // every 600 frames (reportLabels, called from the main loop). A launch names its family with runLabeled / launchLabeled (or a Label in scope); a batch launched without one is counted as "other".
    // Wall time of each job (two clock reads a job: only while the switch is set). For a 4-core machine the total of the families is the number, not the main thread's wait zone.
    struct Label
    {
        const char* mPrevious;
        explicit Label(const char* name) { mPrevious = sCurrentLabel; sCurrentLabel = name; }
        ~Label() { sCurrentLabel = mPrevious; }
    };
    static inline thread_local const char* sCurrentLabel = nullptr;
    static bool labelsOn() { static const bool on = getenv("LL_VK_POOL_LABELS") != nullptr; return on; }
    void runLabeled(const char* label, size_t count, const std::function<void(size_t)>& fn) { Label l(label); run(count, fn); }
    Handle launchLabeled(const char* label, size_t count, const std::function<void(size_t)>& fn) { Label l(label); return launch(count, fn); }
    // main thread, every frame: logs and resets the per-label accounting every 600 frames (a few lines: family, ms a frame on all threads, of it on the main thread, jobs and batches a frame)
    static void reportLabels(unsigned frame_count);

    Handle launch(size_t count, const std::function<void(size_t)>& fn)
    {
        if (count == 0)
        {
            return Handle();
        }
        if (mLegacy)
        {
            runLegacy(count, fn);
            return Handle();
        }
        int slot = -1;
        if (count < kMaxJobs)
        {
            for (int i = 0; i < kSlots; ++i)
            {
                bool expected = false;
                if (mBatches[i].mActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0)
        {
            ++sNestedRuns;
            for (size_t i = 0; i < count; ++i)      // (all slots taken, or a count the claim word has no room for: never in practice, a batch is a few hundred jobs)
            {
                fn(i);
            }
            return Handle();
        }
        Batch& b = mBatches[slot];
        // the batch's number (24 bits), its job count (20 bits) and the next index to hand out (20 bits) are ONE word. 2026-10-29: the count used to be a word of its own, written just before the
        // claim word; a worker still draining the batch before could see the new, larger count together with the old claim word (its last index, now below the count), claim that index
        // successfully and run it with the new batch's function: a job that was then run a second time by the real claim, a count of finished jobs that hit zero while that second one
        // was still running (run() returned, the caller freed the results the job wrote into: the crash on closing a floater), or a decrement after the count was zero (wrapped: run() waiting for
        // ever: the hang). With the count inside the word that is claimed, a claim is valid only for the batch it names. A slot is used again only after every claimed job of its batch has finished
        // (the join waits for that), so the function a successful claim reads is the claimed batch's own.
        const unsigned long long tag = (mSerial.fetch_add(1, std::memory_order_relaxed) + 1) & kTagMask;
        b.mFn = &fn;
        b.mLabel = sCurrentLabel;
        b.mCount = count;
        if (labelsOn())
        {
            labelStat(sCurrentLabel).mBatches.fetch_add(1, std::memory_order_relaxed);
        }
        b.mRemaining.store(count, std::memory_order_relaxed);
        b.mExecuted.store(0, std::memory_order_relaxed);
        b.mClaim.store((tag << kTagShift) | ((unsigned long long)count << kCountShift), std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mMutex);      // (under the mutex: a worker that is about to sleep cannot miss it)
            mGeneration.fetch_add(1, std::memory_order_release);
        }
        // <FS> LL_VK_POOL_WAKE (default 1, 0 = notify_all as before): wake only as many sleeping workers as the batch has jobs for besides the launching thread's own share. A batch of one or two jobs (a shadow cascade's
        // cull) used to wake every sleeper: seven futex wake-ups on the launching thread and seven threads fighting for the mutex to find nothing to do. Workers that are still spinning see the new generation by themselves.
        static const bool targeted_wake = !getenv("LL_VK_POOL_WAKE") || atoi(getenv("LL_VK_POOL_WAKE")) != 0;
        static const bool wake_stats = getenv("LL_VK_POOL_WAKE_STATS") != nullptr;
        const auto notify_t0 = wake_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        const int sleepers = mSleeping.load(std::memory_order_acquire);
        if (targeted_wake && sleepers > 0)
        {
            const int want = (int)std::min<size_t>(count > 0 ? count - 1 : 0, (size_t)sleepers);
            if (want >= sleepers)
            {
                mWake.notify_all();
            }
            else
            {
                for (int i = 0; i < want; ++i)
                {
                    mWake.notify_one();
                }
            }
        }
        else if (!targeted_wake)
        {
            mWake.notify_all();
        }
        if (wake_stats)
        {
            static std::atomic<unsigned long long> launches{ 0 }, notify_ns{ 0 }, sleeper_sum{ 0 }, job_sum{ 0 };
            notify_ns += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - notify_t0).count();
            sleeper_sum += (unsigned long long)sleepers;
            job_sum += count;
            if (++launches % 3000 == 0)
            {
                LL_INFOS("ForkJoin") << "pool launches (every 3000): " << (double)notify_ns.load() / launches.load() / 1000.0 << " us a launch spent waking, " << (double)sleeper_sum.load() / launches.load()
                    << " workers asleep at a launch, " << (double)job_sum.load() / launches.load() << " jobs a launch (targeted wake " << (targeted_wake ? "on" : "off") << ")" << LL_ENDL;
            }
        }
        Handle h;
        h.mSlot = slot;
        h.mTag = tag;
        return h;
    }

    // Works through the jobs of the batch nobody has started, then waits for the ones that are running. help_others: while it waits, the thread also runs jobs of other batches (it is a worker then).
    void join(const Handle& h, bool help_others = false)
    {
        if (!h.valid())
        {
            return;
        }
        Batch& b = mBatches[h.mSlot];
        const size_t count = b.mCount;
        drainBatch(b);
        if (help_others)
        {
            while (b.mRemaining.load(std::memory_order_acquire) != 0 && drainAll())
            {
            }
        }
        // the last jobs are usually a moment from done: spin for a short while before sleeping on the condition variable (a wake-up costs tens of microseconds)
        if (!spinUntil([&b] { return b.mRemaining.load(std::memory_order_acquire) == 0; }, 100))
        {
            std::unique_lock<std::mutex> lock(mMutex);
            for (int waits = 1; !mDone.wait_for(lock, std::chrono::milliseconds(200), [&b] { return b.mRemaining.load(std::memory_order_acquire) == 0; }); ++waits)
            {
                const unsigned long long claim = b.mClaim.load();
                const size_t claimed = (size_t)(claim & kIndexMask);
                if (waits == 1 || waits == 5)
                {
                    LL_WARNS("ForkJoin") << "batch " << h.mTag << " has waited " << waits * 200 << " ms: count " << count << ", next index " << claimed << ", remaining " << b.mRemaining.load()
                        << ", executed " << b.mExecuted.load() << ", in flight " << b.mInFlight.load() << ", claim tag " << (claim >> kTagShift) << " (batch tag " << h.mTag << ")" << LL_ENDL;
                }
                if (waits >= 5 && claimed >= count && b.mInFlight.load() == 0)
                {
                    // every job claimed and none running, yet the count says some are left: the count is wrong; the batch is over
                    ++sRecoveredRuns;
                    LL_WARNS("ForkJoin") << "batch " << h.mTag << " given up on: every job claimed, none running, remaining " << b.mRemaining.load() << ", executed " << b.mExecuted.load() << " of " << count << LL_ENDL;
                    b.mRemaining.store(0);
                    break;
                }
            }
        }
        if (b.mExecuted.load(std::memory_order_acquire) != count)
        {
            if (++sMismatchRuns <= 8)
            {
                LL_WARNS("ForkJoin") << "batch " << h.mTag << " executed " << b.mExecuted.load() << " jobs for a count of " << count << LL_ENDL;
            }
        }
        b.mFn = nullptr;
        b.mActive.store(false, std::memory_order_release);      // (the slot can be launched again; its claim word already has the index at the count)
    }

    void run(size_t count, const std::function<void(size_t)>& fn)
    {
        if (mLegacy)
        {
            runLegacy(count, fn);
            return;
        }
        join(launch(count, fn));
    }

    // The pool the viewer shares between its parallel phases: null when the machine has too few
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
            unsigned workers = (hardware_threads >= 8) ? budget - 1 : 0;
            // LL_VK_POOL_WORKERS=<n>: the worker count by hand (a test of a larger pool: the main thread works a share of every batch, so a pool of n workers is n + 1 threads of work)
            const char* forced = getenv("LL_VK_POOL_WORKERS");
            if (forced && hardware_threads >= 8)
            {
                workers = (unsigned)std::max(atoi(forced), 0);
            }
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

    // the claim word: batch number (bits 40-63) | job count (20-39) | next index (0-19)
    static constexpr unsigned long long kTagMask = 0xFFFFFFull, kIndexMask = 0xFFFFFull;
    static constexpr unsigned kTagShift = 40, kCountShift = 20;
    static constexpr size_t kMaxJobs = 0xFFFFF;
    static constexpr int kSlots = 16;

    struct alignas(64) Batch
    {
        std::atomic<unsigned long long> mClaim{ 0 };     // batch number << 40 | job count << 20 | next index (0 while the slot has never been used: index 0 of count 0, no job to claim)
        std::atomic<size_t> mRemaining{ 0 };
        std::atomic<size_t> mExecuted{ 0 };              // jobs finished in this batch (diagnostic: must equal the count when the join ends)
        std::atomic<unsigned> mInFlight{ 0 };            // jobs being executed now
        std::atomic<bool> mActive{ false };              // the slot is taken (from launch to the end of the join)
        const std::function<void(size_t)>* mFn = nullptr;
        const char* mLabel = nullptr;
        size_t mCount = 0;
    };

    // the per-label accounting (LL_VK_POOL_LABELS): a few families, found by the address of their label string
    struct LabelStat
    {
        std::atomic<const char*> mName{ nullptr };
        std::atomic<unsigned long long> mNs{ 0 }, mNsMain{ 0 }, mJobs{ 0 }, mBatches{ 0 };
    };
    static constexpr int kLabelStats = 48;
    static LabelStat* labelStats() { static LabelStat stats[kLabelStats]; return stats; }
    static LabelStat& labelStat(const char* name)
    {
        LabelStat* stats = labelStats();
        for (int i = 0; i < kLabelStats; ++i)
        {
            const char* have = stats[i].mName.load(std::memory_order_acquire);
            if (have == name)
            {
                return stats[i];
            }
            if (!have)
            {
                const char* expected = nullptr;
                if (stats[i].mName.compare_exchange_strong(expected, name, std::memory_order_acq_rel) || expected == name)
                {
                    return stats[i];
                }
            }
        }
        return stats[kLabelStats - 1];       // (never in practice)
    }

    // claims and runs jobs of one batch until none is left to claim; true if it ran any
    bool drainBatch(Batch& b)
    {
        bool did = false;
        for (;;)
        {
            unsigned long long claim = b.mClaim.load(std::memory_order_acquire);
            const size_t index = (size_t)(claim & kIndexMask);
            if (index >= (size_t)((claim >> kCountShift) & kIndexMask))
            {
                return did;
            }
            if (!b.mClaim.compare_exchange_weak(claim, claim + 1, std::memory_order_acq_rel))
            {
                continue;
            }
            // (the claim succeeded for the batch the word named, and its function is read only now: the slot is not used again before this job has finished)
            const std::function<void(size_t)>* fn = b.mFn;
            const char* label = b.mLabel;
            b.mInFlight.fetch_add(1, std::memory_order_relaxed);
            if (labelsOn())
            {
                const auto t0 = std::chrono::steady_clock::now();
                (*fn)(index);
                const unsigned long long ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                LabelStat& st = labelStat(label);
                st.mNs.fetch_add(ns, std::memory_order_relaxed);
                st.mJobs.fetch_add(1, std::memory_order_relaxed);
                if (on_main_thread())
                {
                    st.mNsMain.fetch_add(ns, std::memory_order_relaxed);
                }
            }
            else
            {
                (*fn)(index);
            }
            b.mExecuted.fetch_add(1, std::memory_order_relaxed);
            b.mInFlight.fetch_sub(1, std::memory_order_relaxed);
            did = true;
            // (never below zero: a decrement too many would leave the count huge and the join waiting for ever)
            size_t remaining = b.mRemaining.load();
            do
            {
                if (remaining == 0)
                {
                    ++sUnderflows;
                    break;
                }
            } while (!b.mRemaining.compare_exchange_weak(remaining, remaining - 1));
            if (remaining == 1)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mDone.notify_all();
            }
        }
    }

    bool drainAll()
    {
        bool did = false;
        for (int i = 0; i < kSlots; ++i)
        {
            if (drainBatch(mBatches[i]))
            {
                did = true;
            }
        }
        return did;
    }

    void workerLoop()
    {
        for (;;)
        {
            const unsigned long long seen = mGeneration.load(std::memory_order_acquire);       // (read before the scan: a launch after this changes it and wakes the wait below)
            if (drainAll())
            {
                continue;
            }
            // the next batch of a frame often follows within a moment (shadow culls, then render maps): spin briefly before sleeping
            if (!spinUntil([&] { return mStop.load(std::memory_order_relaxed) || mGeneration.load(std::memory_order_acquire) != seen; }, 150))
            {
                std::unique_lock<std::mutex> lock(mMutex);
                ++mSleeping;        // (under the mutex: a launch that reads it afterwards counts this worker; one that read it before has already changed the generation, which the predicate below sees)
                mWake.wait(lock, [&] { return mStop.load() || mGeneration.load(std::memory_order_acquire) != seen; });
                --mSleeping;
            }
            if (mStop.load())
            {
                return;
            }
        }
    }

    // ---- the pool as it was before launch and join (LL_VK_POOL_LEGACY=1), kept for the A/B and deleted after the soak
    void runLegacy(size_t count, const std::function<void(size_t)>& fn)
    {
        if (mBusyL.exchange(true, std::memory_order_acq_rel))
        {
            ++sNestedRuns;
            for (size_t i = 0; i < count; ++i)
            {
                fn(i);
            }
            return;
        }
        struct BusyScope { std::atomic<bool>& b; ~BusyScope() { b.store(false, std::memory_order_release); } } busy_scope { mBusyL };
        if (count >= kMaxJobs)
        {
            for (size_t i = 0; i < count; ++i)
            {
                fn(i);
            }
            return;
        }
        unsigned long long generation;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFnL = &fn;
            mRemainingL.store(count, std::memory_order_relaxed);
            mExecutedL.store(0, std::memory_order_relaxed);
            generation = mGenerationL.load(std::memory_order_relaxed) + 1;
            mClaimL.store(((generation & kTagMask) << kTagShift) | ((unsigned long long)count << kCountShift), std::memory_order_release);
            mGenerationL.store(generation, std::memory_order_release);
        }
        mWake.notify_all();
        drainLegacy(generation);
        if (!spinUntil([this] { return mRemainingL.load(std::memory_order_acquire) == 0; }, 100))
        {
            std::unique_lock<std::mutex> lock(mMutex);
            for (int waits = 1; !mDone.wait_for(lock, std::chrono::milliseconds(200), [this] { return mRemainingL.load() == 0; }); ++waits)
            {
                const unsigned long long claim = mClaimL.load();
                const size_t claimed = (size_t)(claim & kIndexMask);
                if (waits >= 5 && claimed >= count && mInFlightL.load() == 0)
                {
                    ++sRecoveredRuns;
                    mRemainingL.store(0);
                    break;
                }
            }
        }
        if (mExecutedL.load(std::memory_order_acquire) != count)
        {
            ++sMismatchRuns;
        }
        mFnL = nullptr;
    }

    void drainLegacy(unsigned long long generation)
    {
        const unsigned long long tag = generation & kTagMask;
        for (;;)
        {
            unsigned long long claim = mClaimL.load(std::memory_order_acquire);
            if (((claim >> kTagShift) & kTagMask) != tag)
            {
                return;
            }
            const size_t index = (size_t)(claim & kIndexMask);
            if (index >= (size_t)((claim >> kCountShift) & kIndexMask))
            {
                return;
            }
            if (!mClaimL.compare_exchange_weak(claim, claim + 1, std::memory_order_acq_rel))
            {
                continue;
            }
            mInFlightL.fetch_add(1, std::memory_order_relaxed);
            (*mFnL)(index);
            mExecutedL.fetch_add(1, std::memory_order_relaxed);
            mInFlightL.fetch_sub(1, std::memory_order_relaxed);
            size_t remaining = mRemainingL.load();
            do
            {
                if (remaining == 0)
                {
                    ++sUnderflows;
                    break;
                }
            } while (!mRemainingL.compare_exchange_weak(remaining, remaining - 1));
            if (remaining == 1)
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mDone.notify_all();
            }
        }
    }

    void workerLoopLegacy()
    {
        unsigned long long seen_generation = 0;
        for (;;)
        {
            if (!spinUntil([&] { return mStop.load(std::memory_order_relaxed) || mGenerationL.load(std::memory_order_acquire) != seen_generation; }, 150))
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [&] { return mStop.load() || mGenerationL.load(std::memory_order_acquire) != seen_generation; });
            }
            if (mStop.load())
            {
                return;
            }
            seen_generation = mGenerationL.load(std::memory_order_acquire);
            drainLegacy(seen_generation);
        }
    }

    bool mLegacy = false;
    const std::function<void(size_t)>* mFnL = nullptr;
    std::atomic<unsigned long long> mClaimL{ 0 };
    std::atomic<size_t> mRemainingL{ 0 };
    std::atomic<size_t> mExecutedL{ 0 };
    std::atomic<unsigned> mInFlightL{ 0 };
    std::atomic<unsigned long long> mGenerationL{ 0 };
    std::atomic<bool> mBusyL{ false };

    std::vector<std::thread> mThreads;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::atomic<int> mSleeping{ 0 };       // workers waiting on mWake (LL_VK_POOL_WAKE)
    std::condition_variable mDone;
    Batch mBatches[kSlots];
    std::atomic<unsigned long long> mSerial{ 0 };
    std::atomic<unsigned long long> mGeneration{ 0 };
    std::atomic<bool> mStop{ false };
};


inline void LLForkJoinPool::reportLabels(unsigned frame_count)
{
    if (!labelsOn())
    {
        return;
    }
    static unsigned last_frame = 0;
    static unsigned long long last_ns[kLabelStats] = {}, last_main[kLabelStats] = {}, last_jobs[kLabelStats] = {}, last_batches[kLabelStats] = {};
    if (last_frame == 0)
    {
        last_frame = frame_count;
        return;
    }
    if (frame_count - last_frame < 600)
    {
        return;
    }
    const double frames = (double)(frame_count - last_frame);
    last_frame = frame_count;
    LabelStat* stats = labelStats();
    double total = 0.0, total_main = 0.0;
    std::string lines;
    for (int i = 0; i < kLabelStats; ++i)
    {
        const char* name = stats[i].mName.load(std::memory_order_acquire);
        if (!name && !stats[i].mNs.load())
        {
            continue;
        }
        const unsigned long long ns = stats[i].mNs.load(), nm = stats[i].mNsMain.load(), jb = stats[i].mJobs.load(), bt = stats[i].mBatches.load();
        const double ms = (double)(ns - last_ns[i]) / 1e6 / frames, mm = (double)(nm - last_main[i]) / 1e6 / frames;
        char buf[200];
        snprintf(buf, sizeof(buf), " | %s %.3f (main %.3f, %.1f jobs, %.2f batches)", name ? name : "other", ms, mm, (double)(jb - last_jobs[i]) / frames, (double)(bt - last_batches[i]) / frames);
        if (ms > 0.0005)
        {
            lines += buf;
        }
        total += ms;
        total_main += mm;
        last_ns[i] = ns; last_main[i] = nm; last_jobs[i] = jb; last_batches[i] = bt;
    }
    LL_INFOS("PoolLabels") << "pool job time per frame over " << frames << " frames (ms on all threads, of it on the main thread): total " << total << " (main " << total_main << ")" << lines << LL_ENDL;
}

#endif // LL_LLFORKJOINPOOL_H
