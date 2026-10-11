/**
 * @file llasyncpool.h
 * @brief Non-blocking worker pool for pure builders (see doc/vulkan/F2-start.md, "how parallel recording is made safe")
 *
 * LLForkJoinPool::run() blocks the caller until everything is done. This pool does not: submit() queues a job and returns at once; the thread that needs the result calls
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

class LLAsyncPool
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
        friend class LLAsyncPool;
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

    explicit LLAsyncPool(unsigned num_workers)
    {
        for (unsigned i = 0; i < num_workers; ++i)
        {
            mThreads.emplace_back([this] { workerLoop(); });
        }
    }

    ~LLAsyncPool()
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
        if (!mThreads.empty())
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueue.push_back(job);
            }
            mWake.notify_one();
        }
        return job;
    }

    // the pool the builders share: LL_VK_BUILDER_THREADS workers (default 2; 0 = every job runs at its wait(), on the thread that waits)
    static LLAsyncPool& shared()
    {
        static LLAsyncPool* pool = []()
        {
            unsigned n = 2;
            if (const char* e = getenv("LL_VK_BUILDER_THREADS"))
            {
                n = (unsigned)atoi(e);
            }
            return new LLAsyncPool(n);
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
    bool mStop = false;
};

#endif // LL_LLASYNCPOOL_H
