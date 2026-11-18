/**
 * @file rkasyncpool.h
 * @brief Non-blocking worker pool for pure builders (see doc/vulkan/F2-start.md, "how parallel recording is made safe")
 *
 * RKForkJoinPool::run() blocks the caller until everything is done. This pool does not: submit() queues a job and returns at once; the thread that needs the result calls
 * Job::wait(). A job that no worker has started yet is run by the waiting thread itself (so a wait never idles behind a queue, and with no workers at all every job simply runs
 * at its wait()). A job writes only its own output and reads only what nobody changes until the wait (that is the builders' contract); the wait is the join: everything the job wrote is
 * visible to the waiter afterwards.
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

#ifndef LL_LLASYNCPOOL_H
#define LL_LLASYNCPOOL_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class RKAsyncPool
{
public:
    class Job
    {
    public:
        explicit Job(std::function<void()> fn) : mFn(std::move(fn)) {}

        // the thread that needs the result: runs the job if nobody has started it, else waits for the one who did
        void wait()
        {
            int expected = QUEUED;
            if (mState.compare_exchange_strong(expected, RUNNING, std::memory_order_acq_rel))
            {
                mFn();
                mFn = nullptr;
                finish();
                mRanByWaiter = true;
                return;
            }
            if (mState.load(std::memory_order_acquire) != DONE)
            {
                const auto start = std::chrono::steady_clock::now();
                // a job is a fraction of a millisecond: spin a little before sleeping
                for (int spin = 0; spin < 2000 && mState.load(std::memory_order_acquire) != DONE; ++spin)
                {
                    std::this_thread::yield();
                }
                if (mState.load(std::memory_order_acquire) != DONE)
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mDoneCv.wait(lock, [this] { return mState.load(std::memory_order_acquire) == DONE; });
                }
                mWaitedNs = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
            }
        }

        bool done() const { return mState.load(std::memory_order_acquire) == DONE; }
        bool ranByWaiter() const { return mRanByWaiter; }
        long long waitedNs() const { return mWaitedNs; }

    private:
        friend class RKAsyncPool;
        enum { QUEUED = 0, RUNNING = 1, DONE = 2 };

        // a worker takes it if the waiter has not
        bool tryRun()
        {
            int expected = QUEUED;
            if (!mState.compare_exchange_strong(expected, RUNNING, std::memory_order_acq_rel))
            {
                return false;
            }
            mFn();
            mFn = nullptr;
            finish();
            return true;
        }

        void finish()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mState.store(DONE, std::memory_order_release);
            }
            mDoneCv.notify_all();
        }

        std::function<void()> mFn;
        std::atomic<int> mState{QUEUED};
        std::mutex mMutex;
        std::condition_variable mDoneCv;
        bool mRanByWaiter = false;      // written by the waiter before it returns
        long long mWaitedNs = 0;
    };

    typedef std::shared_ptr<Job> JobPtr;

    // when set, jobs are not handed to the workers: each runs on the thread that waits for it (a live switch, see FSAsyncBuilders)
    static inline std::atomic<bool> sRunInline{false};

    explicit RKAsyncPool(unsigned num_workers)
    {
        for (unsigned i = 0; i < num_workers; ++i)
        {
            mThreads.emplace_back([this] { workerLoop(); });
        }
    }

    ~RKAsyncPool()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStop = true;
        }
        mWake.notify_all();
        for (std::thread& thread : mThreads)
        {
            thread.join();
        }
    }

    size_t workerCount() const { return mThreads.size(); }

    JobPtr submit(std::function<void()> fn)
    {
        JobPtr job = std::make_shared<Job>(std::move(fn));
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mPending.size() > 256)
            {
                mPending.erase(std::remove_if(mPending.begin(), mPending.end(), [](const JobPtr& j) { return j->done(); }), mPending.end());
            }
            mPending.push_back(job);
            mPendingCount.fetch_add(1, std::memory_order_relaxed);
            if (!mThreads.empty() && !sRunInline.load(std::memory_order_relaxed))
            {
                mQueue.push_back(job);
            }
        }
        if (!mThreads.empty() && !sRunInline.load(std::memory_order_relaxed))
        {
            mWake.notify_one();
        }
        return job;
    }

    // true while a job may still be reading (submitted and not yet seen done by a quiesce or a wait)
    bool anyPending() const { return mPendingCount.load(std::memory_order_acquire) > 0; }

    // everything submitted so far is done when this returns (jobs nobody started run here). Called by code that is about to change what builders read.
    void quiesce()
    {
        std::vector<JobPtr> jobs;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            jobs = mPending;
        }
        for (const JobPtr& job : jobs)
        {
            job->wait();
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mPending.erase(std::remove_if(mPending.begin(), mPending.end(), [](const JobPtr& j) { return j->done(); }), mPending.end());
        mPendingCount.store((int)mPending.size(), std::memory_order_release);
    }

    // the pool the builders share: LL_VK_BUILDER_THREADS workers (default 2; 0 = every job runs at its wait(), on the thread that waits)
    static RKAsyncPool& shared()
    {
        static RKAsyncPool* pool = []()
        {
            unsigned n = 2;
            if (const char* e = getenv("LL_VK_BUILDER_THREADS"))
            {
                n = (unsigned)atoi(e);
            }
            return new RKAsyncPool(n);
        }();
        return *pool;
    }

private:
    void workerLoop()
    {
        for (;;)
        {
            JobPtr job;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [this] { return mStop || !mQueue.empty(); });
                if (mStop && mQueue.empty())
                {
                    return;
                }
                job = std::move(mQueue.front());
                mQueue.pop_front();
            }
            job->tryRun();
        }
    }

    std::vector<std::thread> mThreads;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<JobPtr> mQueue;
    std::vector<JobPtr> mPending;
    std::atomic<int> mPendingCount{0};
    bool mStop = false;
};

// What a pure builder reads (a group's draw infos, its vertex buffers) must not change under it. The few places that change them call beforeMutation() first: it waits for
// every builder in flight, and when the render map of the main view is live (made by the early sort and not yet drawn) it counts a change of what that map points to, so the
// frame can sort again instead of drawing from a map with freed entries.
struct RKBuilderGuard
{
    static inline std::atomic<bool> sMainMapLive{false};
    static inline std::atomic<unsigned> sEpoch{0};
    static inline std::atomic<unsigned> sQuiesces{0};

    // 'in_main_map': the thing about to change is part of the main view's render map
    static void beforeMutation(bool in_main_map)
    {
        RKAsyncPool& pool = RKAsyncPool::shared();
        if (pool.anyPending())
        {
            pool.quiesce();
            ++sQuiesces;
        }
        if (in_main_map && sMainMapLive.load(std::memory_order_acquire))
        {
            ++sEpoch;
        }
    }
};

#endif // LL_LLASYNCPOOL_H
