/**
 * @file llgpuzonetimer.cpp
 * @brief Logs how much GPU time each profiled zone takes
 */

#include "llviewerprecompiledheaders.h"

#include "llgpuzonetimer.h"

#include <algorithm>
#include <deque>
#include <thread>
#include <unordered_map>

#include "llfasttimer.h"
#include "llrenderheaders.h"
#include "llvkzones.h"
#include "llvkbridge.h"
#include "lluniformcensus.h"
#include "llshaderprogram.h"
#include "llviewercontrol.h"

namespace
{
    struct Aggregate
    {
        double mTotalNs = 0.0;
        U64 mCalls = 0;
    };

    bool sHooked = false;
    std::thread::id sMainThread;

    std::unordered_map<std::string, double> sCounters;   // summed since the last summary
    U64 sCounterFrames = 0;

    std::unordered_map<std::string, Aggregate> sAggregates;
    double sSpanNs = 0.0;                  // first zone start to last zone end, summed over measured frames
    U64 sMeasuredFrames = 0;
    F64 sLastLog = 0.0;

    bool isMainThread()
    {
        return std::this_thread::get_id() == sMainThread;
    }

    // Hooks. Zones begun on other threads (shared-context image threads) are ignored, and since
    // the thread never changes between begin and end the two stay balanced.
    // <FS> native-vk: a zone is a record in the command stream; the Vulkan worker stamps the GPU timeline there
    void zoneBegin(const char* name)
    {
        if (isMainThread() && LLVkZones::available())
        {
            LLVkZones::zone(name, true);
        }
    }

    void zoneEnd()
    {
        if (isMainThread() && LLVkZones::available())
        {
            LLVkZones::zone(nullptr, false);
        }
    }

    void uninstall()
    {
        LLGpuZoneHooks::sBegin = nullptr;
        LLGpuZoneHooks::sEnd = nullptr;
        sHooked = false;

        sAggregates.clear();
        sSpanNs = 0.0;
        sMeasuredFrames = 0;
        sCounters.clear();
        sCounterFrames = 0;
    }

    void writeCounters()
    {
        if (sCounterFrames == 0 || sCounters.empty())
        {
            sCounters.clear();
            sCounterFrames = 0;
            return;
        }
        std::vector<std::pair<std::string, double>> rows(sCounters.begin(), sCounters.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& row : rows)
        {
            LL_INFOS("GPUZones") << llformat("counter %12.1f /frame  ", row.second / (double)sCounterFrames) << row.first << LL_ENDL;
        }
        sCounters.clear();
        sCounterFrames = 0;
    }

    void writeSummary()
    {
        writeCounters();
        if (sMeasuredFrames == 0)
        {
            return;
        }

        std::vector<std::pair<std::string, Aggregate>> rows(sAggregates.begin(), sAggregates.end());
        std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.mTotalNs > b.second.mTotalNs; });

        const double frames = (double)sMeasuredFrames;
        LL_INFOS("GPUZones") << "---- GPU zones over " << sMeasuredFrames << " frames: GPU busy span "
            << llformat("%.2f", sSpanNs / frames / 1.0e6) << " ms/frame (zones overlap, they are inclusive) ----" << LL_ENDL;

        const size_t count = llmin(rows.size(), (size_t)60);
        for (size_t i = 0; i < count; ++i)
        {
            LL_INFOS("GPUZones") << llformat("%7.3f ms/frame %7.1f calls/frame  ",
                rows[i].second.mTotalNs / frames / 1.0e6, (double)rows[i].second.mCalls / frames)
                << rows[i].first << LL_ENDL;
        }

        sAggregates.clear();
        sSpanNs = 0.0;
        sMeasuredFrames = 0;
    }
}

// static
void LLGpuZoneTimer::onFrameEnd()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSGpuZoneTimers", false);

    if (enabled && !sHooked)
    {
        sMainThread = std::this_thread::get_id();
        LLGpuZoneHooks::sBegin = &zoneBegin;
        LLGpuZoneHooks::sEnd = &zoneEnd;
        sHooked = true;
        sLastLog = LLTimer::getElapsedSeconds();
        LL_INFOS("GPUZones") << "GPU zone timers enabled" << LL_ENDL;
        return;
    }
    if (!enabled && sHooked)
    {
        uninstall();
        LL_INFOS("GPUZones") << "GPU zone timers disabled" << LL_ENDL;
        return;
    }
    if (!sHooked)
    {
        return;
    }

    ++sCounterFrames;

    const F64 now = LLTimer::getElapsedSeconds();
    if (now - sLastLog >= 10.0)
    {
        sLastLog = now;
        writeSummary();
    }
}

// static
bool LLGpuZoneTimer::isActive()
{
    return sHooked;
}

// static
void LLGpuZoneTimer::count(const char* name, double value)
{
    if (sHooked && std::this_thread::get_id() == sMainThread)
    {
        sCounters[name] += value;
    }
}

// <FS> CPU zones of the main thread (see llgpuzonetimer.h)
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "llstring.h"

namespace
{
    struct CpuZone
    {
        const char* mName = nullptr;
        long long mNanos = 0;
        unsigned mCalls = 0;
        unsigned long long mCycles = 0;     // FS_CPU_ZONES_CPU=1: the thread's own cycles inside the zone (what ran; wall time minus this is time spent blocked or descheduled)
        unsigned mCycleCalls = 0;           // ... over this many calls (only zones nested at most three deep are read: one reading costs about 170 ns)
    };
    CpuZone sCpuZones[512];
    unsigned sCpuZoneCount = 0;
    std::thread::id sCpuMainThread;
    bool sCpuMainThreadKnown = false;
    const bool sCpuZonesOn = getenv("FS_CPU_ZONES") != nullptr;
    const bool sCpuCyclesOn = sCpuZonesOn && getenv("FS_CPU_ZONES_CPU") != nullptr;
    double sCyclesPerNs = 0.0;                  // calibrated once (a short busy loop), only used to turn cycles into milliseconds
    double sCycleReadNs = 0.0;                  // what one QueryThreadCycleTime call costs, measured at the calibration
    unsigned sCpuFrames = 0;
    long long sCpuFirstFrame = 0;
    unsigned long long sCpuThreadCyclesFirst = 0;
    int sCpuDepth = 0;

    long long cpu_now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    unsigned long long thread_cycles()
    {
        ULONG64 c = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c);
        return (unsigned long long)c;
    }

    void calibrate_cycles()
    {
        // 20 ms of pure spinning on this thread: cycles against wall time, and the price of reading the counter
        const long long t0 = cpu_now();
        const unsigned long long c0 = thread_cycles();
        volatile unsigned sink = 0;
        while (cpu_now() - t0 < 20000000)
        {
            sink = sink + 1;
        }
        const unsigned long long c1 = thread_cycles();
        const long long t1 = cpu_now();
        sCyclesPerNs = (double)(c1 - c0) / (double)(t1 - t0);
        const long long r0 = cpu_now();
        for (int i = 0; i < 20000; ++i)
        {
            thread_cycles();
        }
        sCycleReadNs = (double)(cpu_now() - r0) / 20000.0;
    }
}

LLCpuZoneScope::LLCpuZoneScope(const char* name)
    : mName(nullptr), mStart(0), mStartCycles(0)
{
    if (sCpuZonesOn && sCpuMainThreadKnown && std::this_thread::get_id() == sCpuMainThread)
    {
        mName = name;
        mStart = cpu_now();
        ++sCpuDepth;
        if (sCpuCyclesOn && sCpuDepth <= 3)
        {
            mStartCycles = thread_cycles();
        }
        LLUniCensus::push(name);
    }
}

LLCpuZoneScope::~LLCpuZoneScope()
{
    if (!mName)
    {
        return;
    }
    LLUniCensus::pop();
    const long long d = cpu_now() - mStart;
    const bool measured = mStartCycles != 0;
    const unsigned long long cycles = measured ? thread_cycles() - mStartCycles : 0;
    --sCpuDepth;
    for (unsigned i = 0; i < sCpuZoneCount; ++i)
    {
        if (sCpuZones[i].mName == mName)
        {
            sCpuZones[i].mNanos += d;
            sCpuZones[i].mCycles += cycles;
            sCpuZones[i].mCycleCalls += measured;
            ++sCpuZones[i].mCalls;
            return;
        }
    }
    if (sCpuZoneCount < sizeof(sCpuZones) / sizeof(sCpuZones[0]))
    {
        sCpuZones[sCpuZoneCount].mName = mName;
        sCpuZones[sCpuZoneCount].mNanos = d;
        sCpuZones[sCpuZoneCount].mCycles = cycles;
        sCpuZones[sCpuZoneCount].mCycleCalls = measured ? 1 : 0;
        sCpuZones[sCpuZoneCount].mCalls = 1;
        ++sCpuZoneCount;
    }
}


#include "llview.h"
namespace
{
    // the draw time of the views near the top of the UI tree, by view name (inclusive: a floater counts its children)
    std::vector<LLCpuZoneScope*> sViewScopes;
    void cpu_view_hook(const LLView* view, bool begin)
    {
        if (begin)
        {
            sViewScopes.push_back(new LLCpuZoneScope(view->getName().c_str()));
        }
        else if (!sViewScopes.empty())
        {
            delete sViewScopes.back();
            sViewScopes.pop_back();
        }
    }
}

// <FS> U0 (doc/vulkan/U-pass-uniforms-design.md): the census of uniform writes, shader binds and texture binds (LL_VK_UNICENSUS=1 with FS_CPU_ZONES=1), per frame over 600 frames
static void uni_census_report(double frames)
{
    using namespace LLUniCensus;
    if (!sOn)
    {
        return;
    }
    LL_INFOS("UniCensus") << "per frame: uniform writes " << (double)sTotal.mCalls / frames << " (changed " << (double)sTotal.mChanged / frames << ", " << (double)sTotal.mBytes / frames << " bytes), records " << (double)sTotal.mRecords / frames << ", draws " << (double)sTotal.mDraws / frames << ", setBuffer " << (double)sTotal.mSetBuf / frames << ", syncMatrices " << (double)sTotal.mSync / frames << ", texture binds "
        << (double)sTotal.mTex / frames << ", shader binds " << (double)sTotal.mBinds / frames << ", programs seen " << sProgs.size() << LL_ENDL;
    {
        std::vector<std::pair<const char*, Counters*>> zones;
        for (auto& kv : sByZone)
        {
            if (kv.second.mCalls || kv.second.mTex || kv.second.mBinds || kv.second.mRecords || kv.second.mDraws)
            {
                zones.push_back({ kv.first, &kv.second });
            }
        }
        std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) { return a.second->mRecords + a.second->mCalls + a.second->mTex + 4 * a.second->mBinds > b.second->mRecords + b.second->mCalls + b.second->mTex + 4 * b.second->mBinds; });
        std::string text;
        size_t n = 0;
        for (auto& z : zones)
        {
            if (++n > 60) break;
            text += llformat(" | %s: writes %.1f (changed %.1f), tex %.1f, binds %.1f, entered %.1f, records %.1f, draws %.1f, setBuffer %.1f, syncMatrices %.1f", z.first, (double)z.second->mCalls / frames, (double)z.second->mChanged / frames, (double)z.second->mTex / frames,
                             (double)z.second->mBinds / frames, (double)z.second->mScopes / frames, (double)z.second->mRecords / frames, (double)z.second->mDraws / frames, (double)z.second->mSetBuf / frames, (double)z.second->mSync / frames);
        }
        LL_INFOS("UniCensus") << "by innermost zone (writes / changes / texture binds / shader binds per frame):" << text << LL_ENDL;
        for (auto& kv : sByZone)
        {
            kv.second = Counters();
        }
    }
    {
        std::vector<ProgReg> progs = sProgs;
        std::sort(progs.begin(), progs.end(), [](const ProgReg& a, const ProgReg& b) { return a.mStats->mDefNs > b.mStats->mDefNs; });
        std::string text;
        size_t n = 0;
        for (const ProgReg& p : progs)
        {
            const ProgStats& s = *p.mStats;
            if (!s.mDefBinds || ++n > 28) continue;
            const double b = (double)s.mDefBinds;
            text += llformat(" | %s: %.2f binds/frame, %.1f us each, %.1f writes (%.1f changed), %.1f tex; sections us: bind %.1f gbuf %.1f depth+exp %.1f viewport %.1f noise %.1f lightfn %.1f lighttgt %.1f shadowmaps %.1f shadowmat %.1f env %.1f probes %.1f ssao %.1f norm %.1f tail %.1f",
                             p.mName->c_str(), b / frames, (double)s.mDefNs / 1000.0 / b, (double)s.mDefCalls / b, (double)s.mDefChanged / b, (double)s.mDefTex / b,
                             (double)s.mSec[0] / 1000.0 / b, (double)s.mSec[1] / 1000.0 / b, (double)s.mSec[2] / 1000.0 / b, (double)s.mSec[3] / 1000.0 / b, (double)s.mSec[4] / 1000.0 / b,
                             (double)s.mSec[5] / 1000.0 / b, (double)s.mSec[6] / 1000.0 / b, (double)s.mSec[7] / 1000.0 / b, (double)s.mSec[8] / 1000.0 / b, (double)s.mSec[9] / 1000.0 / b,
                             (double)s.mSec[10] / 1000.0 / b, (double)s.mSec[11] / 1000.0 / b, (double)s.mSec[12] / 1000.0 / b, (double)s.mSec[SECTIONS - 1] / 1000.0 / b);
        }
        LL_INFOS("UniCensus") << "bindDeferredShader by program (sorted by wall time per frame):" << text << LL_ENDL;
        // by uniform name over all programs: writes, changes and how many programs
        {
            struct Agg { U64 mCalls = 0, mChanged = 0; int mPrograms = 0; int mClass = -1; };
            std::unordered_map<std::string, Agg> agg;
            for (const ProgReg& p : sProgs)
            {
                const ProgStats& s = *p.mStats;
                for (size_t i = 0; i < s.mUniCalls.size(); ++i)
                {
                    if (!s.mUniCalls[i])
                    {
                        continue;
                    }
                    const std::string* name = p.mProg->censusUniformName(i);
                    if (!name)
                    {
                        continue;
                    }
                    Agg& a = agg[*name];
                    a.mCalls += s.mUniCalls[i];
                    a.mChanged += i < s.mUniChanged.size() ? s.mUniChanged[i] : 0;
                    ++a.mPrograms;
                    a.mClass = p.mProg->censusUniformClass(i);
                }
            }
            std::vector<std::pair<std::string, Agg>> list(agg.begin(), agg.end());
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.mCalls > b.second.mCalls; });
            static const char* const kClass[] = { "pass", "draw", "material", "light", "skin" };
            std::string by_uniform;
            size_t k = 0;
            for (const auto& u : list)
            {
                if (++k > 60) break;
                by_uniform += llformat(" | %s [%s, %d programs]: %.1f writes, %.1f changed", u.first.c_str(), u.second.mClass >= 0 && u.second.mClass < 5 ? kClass[u.second.mClass] : "?", u.second.mPrograms,
                                       (double)u.second.mCalls / frames, (double)u.second.mChanged / frames);
            }
            U64 by_class_calls[6] = {}, by_class_changed[6] = {};
            for (const auto& u : list)
            {
                const int c = u.second.mClass >= 0 && u.second.mClass < 5 ? u.second.mClass : 5;
                by_class_calls[c] += u.second.mCalls;
                by_class_changed[c] += u.second.mChanged;
            }
            std::string classes;
            for (int c = 0; c < 6; ++c)
            {
                classes += llformat(" %s %.1f writes / %.1f changed;", c < 5 ? kClass[c] : "other", (double)by_class_calls[c] / frames, (double)by_class_changed[c] / frames);
            }
            LL_INFOS("UniCensus") << "by class:" << classes << LL_ENDL;
            LL_INFOS("UniCensus") << "by uniform name (all programs, sorted by writes per frame):" << by_uniform << LL_ENDL;
        }
        std::sort(progs.begin(), progs.end(), [](const ProgReg& a, const ProgReg& b) { return a.mStats->mCalls > b.mStats->mCalls; });
        text.clear();
        n = 0;
        for (const ProgReg& p : progs)
        {
            const ProgStats& s = *p.mStats;
            if (!s.mCalls || ++n > 28) continue;
            text += llformat(" | %s: writes %.1f (changed %.1f), binds %.2f, environment update %.2f a frame (%.1f us, %.1f writes each)", p.mName->c_str(), (double)s.mCalls / frames, (double)s.mChanged / frames,
                             (double)s.mBinds / frames, (double)s.mUpdBinds / frames, s.mUpdBinds ? (double)s.mUpdNs / 1000.0 / (double)s.mUpdBinds : 0.0, s.mUpdBinds ? (double)s.mUpdCalls / (double)s.mUpdBinds : 0.0);
        }
        LL_INFOS("UniCensus") << "by program (sorted by uniform writes per frame):" << text << LL_ENDL;
        for (const ProgReg& p : sProgs)
        {
            const bool reg = p.mStats->mReg;
            std::vector<U32> zero_calls(p.mStats->mUniCalls.size(), 0), zero_changed(p.mStats->mUniChanged.size(), 0);
            *p.mStats = ProgStats();
            p.mStats->mReg = reg;
            p.mStats->mUniCalls = zero_calls;
            p.mStats->mUniChanged = zero_changed;
        }
    }
    sTotal = Counters();
}

// static
void LLCpuZoneScope::frame()
{
    if (!sCpuZonesOn)
    {
        return;
    }
    if (!sCpuMainThreadKnown)
    {
        sCpuMainThread = std::this_thread::get_id();
        sCpuMainThreadKnown = true;
        LLView::sDrawHook = &cpu_view_hook;
        if (sCpuCyclesOn)
        {
            calibrate_cycles();
            LL_INFOS("CpuZones") << "thread cycles: " << sCyclesPerNs << " per ns, one reading costs " << sCycleReadNs << " ns" << LL_ENDL;
        }
    }
    if (sCpuFrames == 0)
    {
        sCpuFirstFrame = cpu_now();
        sCpuThreadCyclesFirst = sCpuCyclesOn ? thread_cycles() : 0;
    }
    if (++sCpuFrames < 600)
    {
        return;
    }
    const double frames = (double)sCpuFrames;
    const double interval_ms = (double)(cpu_now() - sCpuFirstFrame) / 1.0e6 / frames;
    std::vector<CpuZone> zones(sCpuZones, sCpuZones + sCpuZoneCount);
    std::sort(zones.begin(), zones.end(), [](const CpuZone& a, const CpuZone& b) { return a.mNanos > b.mNanos; });
    std::string text;
    size_t listed = 0;
    for (const CpuZone& z : zones)
    {
        if (++listed > 90) break;
        if (sCpuCyclesOn && sCyclesPerNs > 0.0 && z.mCycleCalls == z.mCalls && z.mCalls > 0)
        {
            const double wall_ms = (double)z.mNanos / 1.0e6 / frames;
            const double run_ms = (double)z.mCycles / sCyclesPerNs / 1.0e6 / frames;
            text += llformat(" | %s %.2f ms (%.1f calls, blocked %.2f)", z.mName, wall_ms, (double)z.mCalls / frames, std::max(0.0, wall_ms - run_ms));
        }
        else
        {
            text += llformat(" | %s %.2f ms (%.1f calls)", z.mName, (double)z.mNanos / 1.0e6 / frames, (double)z.mCalls / frames);
        }
    }
    std::string blocked_text;
    if (sCpuCyclesOn && sCyclesPerNs > 0.0)
    {
        // the whole thread over the window: how much of the wall time it was running, and what the cycle readings themselves cost (two per zone call)
        double zone_calls = 0.0;
        for (unsigned i = 0; i < sCpuZoneCount; ++i)
        {
            zone_calls += sCpuZones[i].mCycleCalls;
        }
        const double run_ms = (double)(thread_cycles() - sCpuThreadCyclesFirst) / sCyclesPerNs / 1.0e6 / frames;
        blocked_text = llformat(", the thread ran %.2f ms of the loop (%.0f %%), blocked or descheduled %.2f ms; the cycle readings cost about %.3f ms a frame (%.0f zone calls read a frame)", run_ms, 100.0 * run_ms / interval_ms, std::max(0.0, interval_ms - run_ms), zone_calls * 2.0 * sCycleReadNs / 1.0e6 / frames, zone_calls / frames);
    }
    LL_INFOS("CpuZones") << "main thread per frame over " << sCpuFrames << " frames: loop interval " << interval_ms << " ms" << blocked_text << " (inclusive zones, nested ones count in their parents)" << text << LL_ENDL;
    uni_census_report(frames);
    for (unsigned i = 0; i < sCpuZoneCount; ++i)
    {
        sCpuZones[i].mNanos = 0;
        sCpuZones[i].mCycles = 0;
        sCpuZones[i].mCycleCalls = 0;
        sCpuZones[i].mCalls = 0;
    }
    sCpuFrames = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// LL_VK_HITCHWATCH=<ms>: where is the main thread when a frame takes seconds?
//
// The 2.4 s hitches of 2026-10-26 (frame max 2453.9 and 2361.3 ms, the 0% GPU dips) were blocked time inside whatever zone the main thread was in (idle: audio in one
// lap, idle: idleNetwork in the next), and the log went quiet for the whole process, so the cause is not a call but a wait. This watcher thread looks at the time of
// the last finished frame every 25 ms; past the threshold it suspends each thread in turn, unwinds its stack from the context (no allocation while it is suspended),
// resumes it and writes the stacks to the log, with symbol names when the pdb is next to the executable. Off unless the variable is set.
#include <atomic>
#include <mutex>
#include <tlhelp32.h>
#include <dbghelp.h>

namespace
{
    struct HwCap
    {
        DWORD mTid = 0;
        int mCount = 0;
        DWORD64 mPc[20] = {};
    };

    std::atomic<long long> sHwLastFrame{ 0 };
    std::atomic<unsigned long long> sHwFrameNo{ 0 };
    std::atomic<DWORD> sHwMainTid{ 0 };

    long long hw_now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    void hw_capture(DWORD tid, HwCap& cap)
    {
        cap.mTid = tid;
        cap.mCount = 0;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h)
        {
            return;
        }
        if (SuspendThread(h) != (DWORD)-1)
        {
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
            {
                for (int i = 0; i < 20 && ctx.Rip; i++)
                {
                    cap.mPc[cap.mCount++] = ctx.Rip;
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                    if (!fn)
                    {
                        DWORD64 ret = 0;
                        if (!ctx.Rsp || !ReadProcessMemory(GetCurrentProcess(), (const void*)ctx.Rsp, &ret, sizeof(ret), nullptr))
                        {
                            break;
                        }
                        ctx.Rip = ret;
                        ctx.Rsp += 8;
                    }
                    else
                    {
                        PVOID handler = nullptr;
                        DWORD64 frame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
                    }
                }
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }

    typedef BOOL(WINAPI* SymInitializeFn)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI* SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    typedef DWORD(WINAPI* SymSetOptionsFn)(DWORD);

    std::string hw_describe(DWORD64 pc, SymFromAddrFn sym_from_addr)
    {
        char name[MAX_PATH] = {};
        HMODULE module = nullptr;
        std::string text;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &module))
        {
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            text = llformat("%s+0x%llx", base ? base + 1 : name, (unsigned long long)(pc - (DWORD64)module));
        }
        else
        {
            text = llformat("0x%llx", (unsigned long long)pc);
        }
        if (sym_from_addr)
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* info = (SYMBOL_INFO*)buffer;
            info->SizeOfStruct = sizeof(SYMBOL_INFO);
            info->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (sym_from_addr(GetCurrentProcess(), pc, &displacement, info))
            {
                text += llformat(" %s+0x%llx", info->Name, (unsigned long long)displacement);
            }
        }
        return text;
    }

    void hw_thread(double threshold_ms)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double ticks_per_ms = (double)f.QuadPart / 1000.0;
        SymFromAddrFn sym_from_addr = nullptr;
        bool sym_tried = false;
        unsigned long long episode = ~0ull;
        double next_capture = 0.0;
        double peak = 0.0;
        int captures = 0;
        std::vector<HwCap> caps;
        for (;;)
        {
            Sleep(25);
            const long long last = sHwLastFrame.load();
            if (!last)
            {
                continue;
            }
            unsigned long long frame_no = sHwFrameNo.load();
            double age = (double)(hw_now() - last) / ticks_per_ms;
            const char* clock_name = "the main loop has not finished frame";
            // the worker's presents are the second clock: a frame the main thread finished on time can still reach the screen late (the GPU, the swap chain, the worker waiting)
            const long long present = LLVkBridge::lastPresentTicks();
            if (present)
            {
                const double present_age = (double)(hw_now() - present) / ticks_per_ms;
                if (present_age > age)
                {
                    age = present_age;
                    frame_no = ~(unsigned long long)present;        // (an id of its own: the stall ends when a present comes)
                    clock_name = "the worker has not presented since main frame";
                }
            }
            if (age < threshold_ms)
            {
                if (episode != ~0ull && frame_no != episode)
                {
                    LL_WARNS("HitchWatch") << "the frame came back, it took about " << peak << " ms" << LL_ENDL;
                    episode = ~0ull;
                    peak = 0.0;
                }
                continue;
            }
            peak = llmax(peak, age);
            if (frame_no != episode)
            {
                episode = frame_no;
                next_capture = threshold_ms;
                captures = 0;
            }
            if (age < next_capture || captures >= 6)
            {
                continue;
            }
            next_capture += 500.0;
            ++captures;
            if (!sym_tried)
            {
                sym_tried = true;
                if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll"))
                {
                    SymSetOptionsFn set_options = (SymSetOptionsFn)GetProcAddress(dbghelp, "SymSetOptions");
                    SymInitializeFn init = (SymInitializeFn)GetProcAddress(dbghelp, "SymInitialize");
                    char exe[MAX_PATH] = {};
                    GetModuleFileNameA(nullptr, exe, MAX_PATH);
                    if (char* slash = strrchr(exe, '\\'))
                    {
                        *slash = 0;
                    }
                    if (set_options && init)
                    {
                        set_options(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
                        init(GetCurrentProcess(), exe, TRUE);   // (it fails if the viewer's crash handler already did it: the calls below still work)
                    }
                    sym_from_addr = (SymFromAddrFn)GetProcAddress(dbghelp, "SymFromAddr");
                }
            }
            std::vector<DWORD> tids;
            if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0))
            {
                THREADENTRY32 te;
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
                    {
                        tids.push_back(te.th32ThreadID);
                    }
                }
                CloseHandle(snap);
            }
            caps.assign(tids.size(), HwCap());
            for (size_t i = 0; i < tids.size(); i++)
            {
                hw_capture(tids[i], caps[i]);
            }
            const DWORD main_tid = sHwMainTid.load();
            LL_WARNS("HitchWatch") << clock_name << " " << sHwFrameNo.load() << " for " << age << " ms (capture " << captures << " of the stall), " << caps.size() << " threads" << LL_ENDL;
            for (const HwCap& cap : caps)
            {
                if (!cap.mCount)
                {
                    continue;
                }
                const bool is_main = cap.mTid == main_tid;
                const int shown = is_main ? cap.mCount : llmin(cap.mCount, 6);
                std::string stack;
                for (int i = 0; i < shown; i++)
                {
                    stack += (i ? " < " : "") + hw_describe(cap.mPc[i], sym_from_addr);
                }
                LL_WARNS("HitchWatch") << (is_main ? "MAIN " : "thread ") << cap.mTid << ": " << stack << LL_ENDL;
            }
        }
    }
}

void HitchWatch::frame()
{
    static const double threshold = []()
    {
        const char* e = getenv("LL_VK_HITCHWATCH");
        return e ? atof(e) : 0.0;
    }();
    if (threshold <= 0.0)
    {
        return;
    }
    static std::once_flag started;
    std::call_once(started, []() { std::thread(hw_thread, threshold).detach(); });
    sHwMainTid.store(GetCurrentThreadId());
    sHwFrameNo.fetch_add(1);
    sHwLastFrame.store(hw_now());
}
