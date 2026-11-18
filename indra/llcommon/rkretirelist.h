/**
 * @file rkretirelist.h
 * @brief The retire list: an object whose last counted reference goes is destroyed a few frames later, not at once.
 *
 * doc/vulkan/PLAN-clean-core.md step C2 (C2.2 for textures, C2.4 generalised). Render-side data that refers to an object by handle (a draw info, a row) owns no reference: it resolves the handle where it
 * reads. The readers run a few frames behind the main thread (the render map, the packet builders, the render worker), so an object must stay intact for the frames that could still read it after its last
 * OWNER let go. LLThreadSafeRefCount::unref() asks the virtual retireOnLastRef(); an object that takes part answers RKRetireList::park(this): it is queued with the frame number, and pump() (main thread,
 * every frame) destroys it once it is old enough (with a time budget: a teleport can release thousands at once). What must not wait for the list is done when the object is parked (onRetire()). A parked
 * object whose reference count rose again is left alone. Only on the main thread: another thread's last unref deletes at once, as before.
 *
 * RK_VK_RETIRE_FRAMES (default 4) is the age at which a parked object is destroyed.
 * Header only; the state lives in function-local statics. Independent of the viewer classes: an object is an LLThreadSafeRefCount with the hooks in llrefcount.h (onRetire, retireKind, mParked).
 */

#ifndef RK_RKRETIRELIST_H
#define RK_RKRETIRELIST_H

#include "llrefcount.h"
#include "llframetimer.h"
#include "llthread.h"
#include "llerror.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>

class RKRetireList
{
public:
    static constexpr U32 kKinds = 8;            // retireKind() values (0 texture, 1 glTF material, 2 mesh skin info, 3 avatar, 4 vertex buffer; 5-7 free)

    // from the last unref (any thread); false: not parked, the caller deletes
    static bool park(LLThreadSafeRefCount* obj)
    {
        State& s = state();
        const U32 kind = obj->retireKind() < kKinds ? obj->retireKind() : 0;
        if (s.mStopped.load(std::memory_order_acquire) || s.mKindStopped[kind].load(std::memory_order_acquire) || !on_main_thread())
        {
            return false;
        }
        if (obj->mParked.exchange(true, std::memory_order_acq_rel))
        {
            return true;                        // already waiting in the list
        }
        obj->onRetire();
        {
            std::lock_guard<std::mutex> lock(s.mMutex);
            s.mParked.push_back({ obj, nullptr, LLFrameTimer::getFrameCount() });
            if (s.mParked.size() > s.mPeak) s.mPeak = (U32)s.mParked.size();
        }
        ++s.mParkedTotal[kind];
        return true;
    }

    // the same for an object with the plain count (LLRefCount: not thread safe, so only ever on the main thread)
    static bool park(LLRefCount* obj)
    {
        State& s = state();
        const U32 kind = obj->retireKind() < kKinds ? obj->retireKind() : 0;
        if (s.mStopped.load(std::memory_order_acquire) || s.mKindStopped[kind].load(std::memory_order_acquire) || !on_main_thread())
        {
            return false;
        }
        if (obj->mParked)
        {
            return true;                        // already waiting in the list
        }
        obj->mParked = true;
        obj->onRetire();
        {
            std::lock_guard<std::mutex> lock(s.mMutex);
            s.mParked.push_back({ nullptr, obj, LLFrameTimer::getFrameCount() });
            if (s.mParked.size() > s.mPeak) s.mPeak = (U32)s.mParked.size();
        }
        ++s.mParkedTotal[kind];
        return true;
    }

    // main thread: destroy every parked object of this kind and park no more of it (from here its last unref deletes at once, as before). For a kind whose destructors need systems that are shut down
    // before the list is (the avatar: it unregisters from lists, uses the pipeline).
    static void stopKind(U32 kind)
    {
        State& s = state();
        if (kind >= kKinds) return;
        s.mKindStopped[kind].store(true, std::memory_order_release);
        std::deque<Parked> keep;
        std::deque<Parked> mine;
        {
            std::lock_guard<std::mutex> lock(s.mMutex);
            for (const Parked& p : s.mParked)
            {
                const U32 k = p.mTs ? p.mTs->retireKind() : p.mPlain->retireKind();
                (k == kind ? mine : keep).push_back(p);
            }
            s.mParked.swap(keep);
        }
        for (const Parked& p : mine)
        {
            destroy(s, p);
        }
    }

    // main thread, every frame: destroy what is old enough (a time budget of 1.5 ms; the rest waits for the next frame)
    static void pump()
    {
        State& s = state();
        const U64 now = LLFrameTimer::getFrameCount();
        const auto t0 = std::chrono::steady_clock::now();
        U32 destroyed = 0;
        for (;;)
        {
            Parked item{};
            {
                std::lock_guard<std::mutex> lock(s.mMutex);
                if (s.mParked.empty() || now - s.mParked.front().mFrame < s.mFrames)
                {
                    break;
                }
                item = s.mParked.front();
                s.mParked.pop_front();
            }
            destroy(s, item);
            if ((++destroyed & 15) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(1500))
            {
                break;
            }
        }
        if (now - s.mLastLog >= 3600)
        {
            s.mLastLog = now;
            size_t waiting;
            { std::lock_guard<std::mutex> lock(s.mMutex); waiting = s.mParked.size(); }
            LL_INFOS("Retire") << "retire list: parked " << totals(s.mParkedTotal) << " (textures " << s.mParkedTotal[0] << ", materials " << s.mParkedTotal[1] << ", skin infos " << s.mParkedTotal[2] << ", avatars " << s.mParkedTotal[3] << ", vertex buffers " << s.mParkedTotal[4] << "), destroyed " << totals(s.mDestroyedTotal)
                               << ", count rose again " << totals(s.mRevivedTotal) << ", waiting " << waiting << " (peak " << s.mPeak << "), age " << s.mFrames << " frames" << LL_ENDL;
        }
    }

    // main thread: destroy everything parked and park no more (from here the last unref deletes at once, as before)
    static void shutdown()
    {
        State& s = state();
        s.mStopped.store(true, std::memory_order_release);
        for (;;)
        {
            Parked item{};
            {
                std::lock_guard<std::mutex> lock(s.mMutex);
                if (s.mParked.empty())
                {
                    break;
                }
                item = s.mParked.front();
                s.mParked.pop_front();
            }
            destroy(s, item);
        }
        LL_INFOS("Retire") << "retire list at shutdown: parked " << totals(s.mParkedTotal) << ", destroyed " << totals(s.mDestroyedTotal) << ", count rose again " << totals(s.mRevivedTotal) << LL_ENDL;
    }

private:
    struct Parked { LLThreadSafeRefCount* mTs; LLRefCount* mPlain; U64 mFrame; };
    struct State
    {
        std::mutex mMutex;
        std::deque<Parked> mParked;             // oldest first
        std::atomic<bool> mStopped{ false };
        std::atomic<bool> mKindStopped[kKinds] = {};
        std::atomic<U64> mParkedTotal[kKinds] = {}, mDestroyedTotal[kKinds] = {}, mRevivedTotal[kKinds] = {};
        U32 mPeak = 0;
        U64 mLastLog = 0;
        U64 mFrames = 4;
        State()
        {
            const char* f = getenv("RK_VK_RETIRE_FRAMES");
            const int v = f ? atoi(f) : 4;
            mFrames = (U64)(v < 1 ? 1 : v);
        }
    };
    static State& state()
    {
        static State* s = new State;            // (never destroyed: objects released during exit must still find it)
        return *s;
    }
    static U64 totals(const std::atomic<U64>* a)
    {
        U64 t = 0;
        for (U32 i = 0; i < kKinds; ++i) t += a[i].load();
        return t;
    }
    // the main thread destroys what is due: an object whose count rose again meanwhile is left alone
    static void destroy(State& s, const Parked& item)
    {
        if (item.mTs)
        {
            LLThreadSafeRefCount* obj = item.mTs;
            const U32 kind = obj->retireKind() < kKinds ? obj->retireKind() : 0;
            obj->mParked.store(false, std::memory_order_release);
            if (obj->getNumRefs() == 0) { obj->destroyRetired(); ++s.mDestroyedTotal[kind]; }
            else { ++s.mRevivedTotal[kind]; }
        }
        else
        {
            LLRefCount* obj = item.mPlain;
            const U32 kind = obj->retireKind() < kKinds ? obj->retireKind() : 0;
            obj->mParked = false;
            if (obj->getNumRefs() == 0) { obj->destroyRetired(); ++s.mDestroyedTotal[kind]; }
            else { ++s.mRevivedTotal[kind]; }
        }
    }
};

#endif // RK_RKRETIRELIST_H
