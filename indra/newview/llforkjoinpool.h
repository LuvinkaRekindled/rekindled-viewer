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
            mStop = true;
        }
        mWake.notify_all();
        for (std::thread& thread : mThreads)
        {
            thread.join();
        }
    }

    size_t workerCount() const { return mThreads.size(); }

    void run(size_t count, const std::function<void(size_t)>& fn)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFn = &fn;
            mCount = count;
            mNext = 0;
            mRemaining = count;
            ++mGeneration;
        }
        mWake.notify_all();

        drain();

        std::unique_lock<std::mutex> lock(mMutex);
        mDone.wait(lock, [this] { return mRemaining == 0; });
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
    void drain()
    {
        for (;;)
        {
            const size_t index = mNext.fetch_add(1);
            if (index >= mCount)
            {
                return;
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
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [&] { return mStop || mGeneration != seen_generation; });
                if (mStop)
                {
                    return;
                }
                seen_generation = mGeneration;
            }
            drain();
        }
    }

    std::vector<std::thread> mThreads;
    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mDone;
    const std::function<void(size_t)>* mFn = nullptr;
    std::atomic<size_t> mCount{ 0 };
    std::atomic<size_t> mNext{ 0 };
    std::atomic<size_t> mRemaining{ 0 };
    unsigned long long mGeneration = 0;
    bool mStop = false;
};

#endif // LL_LLFORKJOINPOOL_H
