/**
 * @file rkphasecheck.h
 * @brief Thread phase checks for the writers of shared viewer state (Rekindled, GPU-LIFETIMES.md step 4)
 *
 * The rules (GPU-LIFETIMES.md 3.4): T1 a pool job writes only memory its item owns, per-thread lists and plain delta records; shared state (queues, group
 * flags, the moved lists, GPU records) is written by the main thread from those records afterwards. T2 what a phase's jobs read is not written by anyone
 * until the phase has joined: a main-thread step that writes such data runs before the fork or after the join, never beside it.
 *
 * rk_phase_check("name") goes into the direct write of a shared writer (the lines after the delta sink diversion): "main thread, or no phase running".
 * A call from another thread, or from the main thread while a fork-join batch is active (sPhaseDepth: from the slot taken in launch to the end of the
 * join), is counted per writer and logged (tag PhaseCheck: the first 10 of each writer, then every 1000th). It never stops the call: it only says
 * that the rule was broken, in every build. Healthy = no line in a lap.
 *
 * Writers that are made thread safe on purpose (wakeRequest has its own lock) or that a job owns (a pose job's joint arrays) do not call it.
 */
#ifndef LL_LLPHASECHECK_H
#define LL_LLPHASECHECK_H

#include <atomic>
#include <cstring>
#include <string>

#include "llerror.h"
#include "llthread.h"

namespace RKPhaseCheck
{
    // fork-join batches between the slot taken in launch and the end of the join (RKForkJoinPool keeps it)
    inline std::atomic<unsigned> sPhaseDepth{ 0 };

    struct Slot
    {
        std::atomic<const char*> mName{ nullptr };
        std::atomic<unsigned long long> mWorker{ 0 }, mPhase{ 0 };
    };
    inline Slot sSlots[24];

    inline void report(const char* writer, bool worker)
    {
        Slot* slot = nullptr;
        for (Slot& s : sSlots)
        {
            const char* have = s.mName.load(std::memory_order_acquire);
            if (!have)
            {
                const char* expected = nullptr;
                if (s.mName.compare_exchange_strong(expected, writer, std::memory_order_acq_rel))
                {
                    slot = &s;
                    break;
                }
                have = expected;
            }
            if (have && (have == writer || strcmp(have, writer) == 0))
            {
                slot = &s;
                break;
            }
        }
        if (!slot)
        {
            return;     // (more writers than slots: not counted)
        }
        const unsigned long long n = (worker ? slot->mWorker : slot->mPhase).fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10 || n % 1000 == 0)
        {
            LL_WARNS("PhaseCheck") << "thread phase check: " << writer << " written " << (worker ? "from a thread that is not the main thread" : "by the main thread while a fork-join batch is running")
                                   << " (" << n << " so far for this case)" << LL_ENDL;
        }
    }

    // the totals by writer, one line (main thread: the periodic report and the end of the pool self test): the count is the signal, a line of an event can be missed
    inline void logSummary()
    {
        unsigned long long total = 0;
        std::string detail;
        for (Slot& s : sSlots)
        {
            const char* name = s.mName.load(std::memory_order_acquire);
            if (!name)
            {
                continue;
            }
            const unsigned long long w = s.mWorker.load(std::memory_order_relaxed), p = s.mPhase.load(std::memory_order_relaxed);
            total += w + p;
            detail += std::string(" | ") + name + ": " + std::to_string(w) + " from other threads, " + std::to_string(p) + " during a batch";
        }
        LL_INFOS("PhaseCheck") << "thread phase check totals: " << total << " (healthy = 0)" << detail << LL_ENDL;
    }
}

inline void rk_phase_check(const char* writer)
{
    const bool main_thread = on_main_thread();
    if (main_thread && RKPhaseCheck::sPhaseDepth.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    RKPhaseCheck::report(writer, !main_thread);
}

#endif // LL_LLPHASECHECK_H
