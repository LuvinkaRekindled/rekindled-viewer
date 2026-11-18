/**
 * @file rkgpuzonetimer.cpp
 * @brief Logs how much GPU time each profiled zone takes
 */

#include "llviewerprecompiledheaders.h"

#include "rkgpuzonetimer.h"

#include <algorithm>
#include <deque>
#include <thread>
#include "rkvkplatform.h"
#if LL_WINDOWS
#include <psapi.h>
#endif
#include <nmmintrin.h>
#include <unordered_map>
#include <unordered_set>

#include "llfasttimer.h"
#include "rkvkbridge.h"
#include "rkrenderheaders.h"
#include "rkvkzones.h"
#include "rkvkbridge.h"
#include "rkuniformcensus.h"
#include "rkshaderprogram.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewertexturelist.h"
#include "lldrawable.h"
#include "llvertexbuffer.h"
#include "pipeline.h"

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
        if (isMainThread() && RKVkZones::available())
        {
            RKVkZones::zone(name, true);
        }
    }

    void zoneEnd()
    {
        if (isMainThread() && RKVkZones::available())
        {
            RKVkZones::zone(nullptr, false);
        }
    }

    void uninstall()
    {
        RKGpuZoneHooks::sBegin = nullptr;
        RKGpuZoneHooks::sEnd = nullptr;
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
void RKGpuZoneTimer::onFrameEnd()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSGpuZoneTimers", false);

    if (enabled && !sHooked)
    {
        sMainThread = std::this_thread::get_id();
        RKGpuZoneHooks::sBegin = &zoneBegin;
        RKGpuZoneHooks::sEnd = &zoneEnd;
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
bool RKGpuZoneTimer::isActive()
{
    return sHooked;
}

// static
void RKGpuZoneTimer::count(const char* name, double value)
{
    if (sHooked && std::this_thread::get_id() == sMainThread)
    {
        sCounters[name] += value;
    }
}

// <FS> CPU zones of the main thread (see rkgpuzonetimer.h)
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
    CpuZone sCpuZones[1024];
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

    // LL_VK_CPUPROBE=1 with FS_CPU_ZONES=1: which logical processor the main thread runs on at each frame end (histogram over the window, moves between frames), and the
    // clock of every logical processor at the report (CallNtPowerInformation, loaded at run time). A 9950X has two core complexes and SMT pairs on neighbouring numbers.
    const bool sCoreProbeOn = sCpuZonesOn && getenv("LL_VK_CPUPROBE") != nullptr;
    unsigned sCoreHist[64] = {};
    unsigned sCoreMoves = 0;
    DWORD sCoreLast = 0xFFFFFFFFu;
    std::vector<double> sChainNs;               // ns per dependent step, one sample every 30 frames
    long long cpu_now();

    // The real core clock as the main thread sees it: 20000 crc32 steps, each waiting for the one before (3 cycles on Zen), timed with the same clock as the zones.
    // CallNtPowerInformation reports the nominal 4300 MHz and QueryThreadCycleTime counts at the fixed timestamp rate, so neither shows boost or a drop.
#if !LL_WINDOWS
    __attribute__((target("sse4.2")))        // the crc32 instruction (MSVC needs no target option)
#endif
    double chain_ns_per_step()
    {
        const int steps = 20000;
        unsigned long long x = 0x9E3779B97F4A7C15ull;
        const long long t0 = cpu_now();
        for (int i = 0; i < steps; ++i)
        {
            x = _mm_crc32_u64(x, 0x1234567ull);
        }
        const long long t1 = cpu_now();
        static volatile unsigned long long sink;
        sink = x;
        return (double)(t1 - t0) / steps;
    }

    std::string core_probe_text(double frames)
    {
        std::string out;
        if (!sChainNs.empty())
        {
            std::sort(sChainNs.begin(), sChainNs.end());
            const double med = sChainNs[sChainNs.size() / 2];
            out += llformat(", dependent crc32 chain (3 cycles a step): %.3f ns a step median (%.3f lowest, %.3f highest of %zu), which is %.2f GHz if 3 cycles", med, sChainNs.front(), sChainNs.back(), sChainNs.size(), 3.0 / med);
            sChainNs.clear();
        }
        unsigned top[3] = { 0, 0, 0 };
        unsigned taken[64] = {};
        for (int k = 0; k < 3; ++k)
        {
            unsigned best = 0;
            unsigned best_count = 0;
            for (unsigned c = 0; c < 64; ++c)
            {
                if (!taken[c] && sCoreHist[c] > best_count)
                {
                    best = c;
                    best_count = sCoreHist[c];
                }
            }
            taken[best] = 1;
            top[k] = best;
        }
        unsigned half[2] = { 0, 0 };
        for (unsigned c = 0; c < 64; ++c)
        {
            half[(c / 16) & 1] += sCoreHist[c];
        }
        out += llformat(", main thread on logical cpu %u %.0f %%, %u %.0f %%, %u %.0f %% of the frames (cpus 0-15 %.0f %%, 16-31 %.0f %%), moved %.2f times a frame",
            top[0], 100.0 * sCoreHist[top[0]] / frames, top[1], 100.0 * sCoreHist[top[1]] / frames, top[2], 100.0 * sCoreHist[top[2]] / frames,
            100.0 * half[0] / frames, 100.0 * half[1] / frames, sCoreMoves / frames);
#if LL_WINDOWS
        struct PowerInfo { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; };
        typedef LONG(WINAPI * CallNtPowerInformationFn)(int, PVOID, ULONG, PVOID, ULONG);
        static CallNtPowerInformationFn fn = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HMODULE pw = LoadLibraryA("powrprof.dll"))
            {
                fn = (CallNtPowerInformationFn)GetProcAddress(pw, "CallNtPowerInformation");
            }
        }
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const unsigned n = std::min<unsigned>(si.dwNumberOfProcessors, 64);
        PowerInfo info[64] = {};
        if (fn && n && fn(11 /* ProcessorInformation */, nullptr, 0, info, n * sizeof(PowerInfo)) == 0)
        {
            double sum = 0.0, lo = 1e9, hi = 0.0;
            for (unsigned i = 0; i < n; ++i)
            {
                sum += info[i].CurrentMhz;
                lo = std::min<double>(lo, info[i].CurrentMhz);
                hi = std::max<double>(hi, info[i].CurrentMhz);
            }
            out += llformat("; clocks at the report: mean %.0f MHz, lowest %.0f, highest %.0f (max %lu); on cpu %u %lu MHz", sum / n, lo, hi, info[0].MaxMhz, top[0], info[top[0] < n ? top[0] : 0].CurrentMhz);
        }
#endif // LL_WINDOWS (the clocks through powrprof.dll)
        for (unsigned c = 0; c < 64; ++c) sCoreHist[c] = 0;
        sCoreMoves = 0;
        return out;
    }

    long long cpu_now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    unsigned long long thread_cycles()
    {
#if LL_WINDOWS
        ULONG64 c = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c);
        return (unsigned long long)c;
#else
        // the thread's CPU time in nanoseconds stands in for its cycles (calibrate_cycles measures cycles against wall time anyway)
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
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

RKCpuZoneScope::RKCpuZoneScope(const char* name)
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
        RKUniCensus::push(name);
    }
}

RKCpuZoneScope::~RKCpuZoneScope()
{
    if (!mName)
    {
        return;
    }
    RKUniCensus::pop();
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
    std::vector<RKCpuZoneScope*> sViewScopes;
    void cpu_view_hook(const LLView* view, bool begin)
    {
        if (begin)
        {
            // (zones are matched by name pointer: one pointer per distinct name, or every widget with the same name takes a slot of the table and the table is full before the render zones register)
            static std::unordered_set<std::string> names;
            sViewScopes.push_back(new RKCpuZoneScope(names.insert(view->getName()).first->c_str()));
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
    using namespace RKUniCensus;
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
void RKCpuZoneScope::frame()
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
    if (sCoreProbeOn)
    {
        const DWORD core = GetCurrentProcessorNumber();
        sCoreHist[core & 63]++;
        if (sCoreLast != 0xFFFFFFFFu && core != sCoreLast)
        {
            ++sCoreMoves;
        }
        sCoreLast = core;
        if (sCpuFrames % 30 == 5)
        {
            sChainNs.push_back(chain_ns_per_step());
        }
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
        if (++listed > 250) break;      // (90 before: a zone just under the cut showed as absent, and its growth after a turn looked like a jump from nothing)
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
    {
        // the process's page faults a frame (soft faults count: memory handed back and taken again shows here), working set and private bytes
#if LL_WINDOWS
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        static DWORD last_faults = 0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        {
            blocked_text += llformat(", process page faults %.1f a frame, working set %.0f MB, private %.0f MB", (double)(pmc.PageFaultCount - last_faults) / frames, pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0);
            last_faults = pmc.PageFaultCount;
        }
#else
        // page faults from getrusage, the resident size from /proc/self/statm
        static long last_faults = 0;
        rusage ru = {};
        getrusage(RUSAGE_SELF, &ru);
        long statm_size = 0, statm_resident = 0;
        if (FILE* f = fopen("/proc/self/statm", "r"))
        {
            if (fscanf(f, "%ld %ld", &statm_size, &statm_resident) != 2)
            {
                statm_resident = 0;
            }
            fclose(f);
        }
        const long faults = ru.ru_minflt + ru.ru_majflt;
        blocked_text += llformat(", process page faults %.1f a frame, resident %.0f MB", (double)(faults - last_faults) / frames, (double)statm_resident * (double)sysconf(_SC_PAGESIZE) / 1048576.0);
        last_faults = faults;
#endif
    }
    if (sCoreProbeOn)
    {
        blocked_text += core_probe_text(frames);
        extern std::string rk_hero_probe_report(double frames);     // (llheroprobemanager.cpp)
        blocked_text += rk_hero_probe_report(frames);
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

#if LL_WINDOWS
#include <mutex>
#include <dbghelp.h>
// <FS> the name of the function an address is in (DbgHelp, loaded here the way the hitch watcher does it), for censuses that count by call site (LL_VK_REBUILDCENSUS in LLPipeline::markRebuild)
std::string rk_describe_address(const void* pc)
{
    typedef BOOL(WINAPI * SymInitializeFn)(HANDLE, PCSTR, BOOL);
    typedef DWORD(WINAPI * SymSetOptionsFn)(DWORD);
    typedef BOOL(WINAPI * SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    static bool tried = false;
    static SymFromAddrFn sym_from_addr = nullptr;
    if (!tried)
    {
        tried = true;
        if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll"))
        {
            SymSetOptionsFn set_options = (SymSetOptionsFn)GetProcAddress(dbghelp, "SymSetOptions");
            SymInitializeFn init = (SymInitializeFn)GetProcAddress(dbghelp, "SymInitialize");
            sym_from_addr = (SymFromAddrFn)GetProcAddress(dbghelp, "SymFromAddr");
            if (set_options && init && sym_from_addr)
            {
                set_options(0x00000002 /* SYMOPT_UNDNAME */ | 0x00000004 /* SYMOPT_DEFERRED_LOADS */);
                init(GetCurrentProcess(), nullptr, TRUE);
            }
            else
            {
                sym_from_addr = nullptr;
            }
        }
    }
    char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (sym_from_addr && sym_from_addr(GetCurrentProcess(), (DWORD64)(uintptr_t)pc, &displacement, symbol))
    {
        return llformat("%s+0x%llx", symbol->Name, (unsigned long long)displacement);
    }
    return llformat("0x%llx", (unsigned long long)(uintptr_t)pc);
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
        DWORD64 mPc[32] = {};
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

    // <FS> the stack walk of a thread the watcher has suspended: that thread can be in the middle of a prologue, an epilogue or a context switch, and RtlVirtualUnwind then reads memory that is not a
    // stack (access violation in the watcher thread, which killed the viewer at the login of 2026-11-03: crash-stack-24784). The walk is fault proof, and the thread is always resumed.
    static void hw_walk(CONTEXT& ctx, HwCap& cap)
    {
        __try
        {
            for (int i = 0; i < 32 && ctx.Rip; i++)
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
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // whatever was captured so far is kept
        }
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
                hw_walk(ctx, cap);
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
            const long long present = RKVkBridge::lastPresentTicks();
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

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// LL_VK_SAMPLE=<ms>: a sampling profile of the main thread, without administrator rights. Every <ms> (2 is a good value) the thread is suspended for the few microseconds it takes to read where it
// is (the same capture as the hitch watcher), and every 10 s the functions that held it are logged with their share of the samples ("Sampler" tag). Two windows of a lap (before a turn, after it) show
// whether the same functions got slower all over (memory or cache: the shares stay, the loop gets longer) or particular ones grew. Needs the pdb next to the executable for names.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace
{
    void sample_thread(double interval_ms)
    {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        SymFromAddrFn sym_from_addr = nullptr;
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
                init(GetCurrentProcess(), exe, TRUE);
            }
            sym_from_addr = (SymFromAddrFn)GetProcAddress(dbghelp, "SymFromAddr");
        }
        std::unordered_map<DWORD64, unsigned> counts;       // by the start of the function (or the address itself when it has no unwind data)
        std::unordered_map<DWORD64, unsigned> modules;      // by module base
        std::unordered_map<DWORD64, unsigned> inclusive;    // J1 hotspots: every function on the stack once per sample (the stack is 32 frames deep from the leaf)
        std::unordered_map<DWORD64, std::unordered_map<DWORD64, unsigned>> callers;     // J1 hotspots: for a function, who called it when it was the leaf
        // J1 hotspots: the samples whose leaf is outside the executable (the allocator, waits, the CRT, kernel calls) are attributed to the nearest frame INSIDE it, with the leaf functions they were in
        struct OutsideSample { DWORD64 mLeaf; DWORD64 mInside[6]; int mCount; };
        std::vector<OutsideSample> outside_samples;
        const DWORD64 exe_base = (DWORD64)GetModuleHandleA(nullptr);
        auto function_start = [](DWORD64 pc) -> DWORD64
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, nullptr);
            return fn ? base + fn->BeginAddress : pc;
        };
        unsigned total = 0, windows = 0;
        LARGE_INTEGER f, window_start;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&window_start);
        HwCap cap;
        for (;;)
        {
            if (timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(interval_ms * 10000.0);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            }
            else
            {
                Sleep((DWORD)interval_ms);
            }
            const DWORD main_tid = sHwMainTid.load();
            if (!main_tid)
            {
                continue;
            }
            hw_capture(main_tid, cap);
            if (cap.mCount)
            {
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(cap.mPc[0], &base, nullptr);
                const DWORD64 leaf = fn ? base + fn->BeginAddress : cap.mPc[0];
                ++counts[leaf];
                ++modules[base];
                ++total;
                {
                    DWORD64 seen[32];
                    int n_seen = 0;
                    for (int k = 0; k < cap.mCount; ++k)
                    {
                        const DWORD64 start = function_start(cap.mPc[k]);
                        bool dup = false;
                        for (int q = 0; q < n_seen && !dup; ++q)
                        {
                            dup = seen[q] == start;
                        }
                        if (!dup)
                        {
                            seen[n_seen++] = start;
                            ++inclusive[start];
                        }
                    }
                    if (cap.mCount > 1)
                    {
                        ++callers[leaf][function_start(cap.mPc[1])];
                    }
                    if (base != exe_base)
                    {
                        OutsideSample sample = {};
                        sample.mLeaf = leaf;
                        for (int k = 1; k < cap.mCount && sample.mCount < 6; ++k)
                        {
                            DWORD64 b2 = 0;
                            PRUNTIME_FUNCTION f2 = RtlLookupFunctionEntry(cap.mPc[k], &b2, nullptr);
                            if (f2 && b2 == exe_base)
                            {
                                sample.mInside[sample.mCount++] = b2 + f2->BeginAddress;
                            }
                        }
                        if (sample.mCount)
                        {
                            outside_samples.push_back(sample);
                        }
                    }
                }
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if ((double)(now.QuadPart - window_start.QuadPart) / (double)f.QuadPart < 10.0)
            {
                continue;
            }
            window_start = now;
            std::vector<std::pair<unsigned, DWORD64>> top;
            for (const auto& kv : counts)
            {
                top.emplace_back(kv.second, kv.first);
            }
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string text;
            for (size_t i = 0; i < top.size() && i < 40; ++i)
            {
                std::string name = llformat("0x%llx", (unsigned long long)top[i].second);
                if (sym_from_addr)
                {
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                    SYMBOL_INFO* info = (SYMBOL_INFO*)buffer;
                    info->SizeOfStruct = sizeof(SYMBOL_INFO);
                    info->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    if (sym_from_addr(GetCurrentProcess(), top[i].second, &displacement, info))
                    {
                        name = info->Name;
                    }
                }
                text += llformat(" | %s %.1f%%", name.c_str(), 100.0 * top[i].first / total);
            }
            std::string mods;
            std::vector<std::pair<unsigned, DWORD64>> mtop;
            for (const auto& kv : modules)
            {
                mtop.emplace_back(kv.second, kv.first);
            }
            std::sort(mtop.begin(), mtop.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < mtop.size() && i < 8; ++i)
            {
                char mod[MAX_PATH] = {};
                if (mtop[i].second)
                {
                    GetModuleFileNameA((HMODULE)mtop[i].second, mod, MAX_PATH);
                }
                const char* slash = strrchr(mod, '\\');
                mods += llformat(" | %s %.1f%%", mtop[i].second ? (slash ? slash + 1 : mod) : "(unknown)", 100.0 * mtop[i].first / total);
            }
            LL_INFOS("Sampler") << "main thread, window " << ++windows << ": " << total << " samples, by module:" << mods << " ; by function:" << text << LL_ENDL;
            {
                auto name_of = [&](DWORD64 pc) -> std::string
                {
                    if (sym_from_addr)
                    {
                        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
                        SYMBOL_INFO* info = (SYMBOL_INFO*)buffer;
                        info->SizeOfStruct = sizeof(SYMBOL_INFO);
                        info->MaxNameLen = 255;
                        DWORD64 displacement = 0;
                        if (sym_from_addr(GetCurrentProcess(), pc, &displacement, info))
                        {
                            return info->Name;
                        }
                    }
                    return llformat("0x%llx", (unsigned long long)pc);
                };
                std::vector<std::pair<unsigned, DWORD64>> itop;
                for (const auto& kv : inclusive)
                {
                    itop.emplace_back(kv.second, kv.first);
                }
                std::sort(itop.begin(), itop.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string itext;
                for (size_t i = 0; i < itop.size() && i < 60; ++i)
                {
                    itext += llformat(" | %s %.1f%%", name_of(itop[i].second).c_str(), 100.0 * itop[i].first / total);
                }
                LL_INFOS("Sampler") << "main thread, window " << windows << ", inclusive (a function and everything it calls):" << itext << LL_ENDL;
                std::string ctext;
                for (size_t i = 0; i < top.size() && i < 20; ++i)
                {
                    const auto found = callers.find(top[i].second);
                    if (found == callers.end())
                    {
                        continue;
                    }
                    std::vector<std::pair<unsigned, DWORD64>> who;
                    for (const auto& kv : found->second)
                    {
                        who.emplace_back(kv.second, kv.first);
                    }
                    std::sort(who.begin(), who.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    ctext += llformat(" | %s (%.1f%%) called by", name_of(top[i].second).c_str(), 100.0 * top[i].first / total);
                    for (size_t q = 0; q < who.size() && q < 3; ++q)
                    {
                        ctext += llformat(" %s %u%%", name_of(who[q].second).c_str(), (unsigned)(100.0 * who[q].first / top[i].first + 0.5));
                    }
                }
                LL_INFOS("Sampler") << "main thread, window " << windows << ", callers of the top self time functions:" << ctext << LL_ENDL;
                // the first frame inside the executable that is not the allocator's own plumbing is the site; its caller is shown with it
                auto plumbing = [](const std::string& n)
                {
                    return n.find("rk_tracy_") == 0 || n.find("operator new") == 0 || n.find("operator delete") == 0 || n.find("malloc") != std::string::npos || n.find("free") == 0
                        || n.find("_Allocate") != std::string::npos || n.find("_Deallocate") != std::string::npos || n.find("::allocate") != std::string::npos || n.find("::deallocate") != std::string::npos
                        || n.find("_Change_array") != std::string::npos || n.find("_Reallocate") != std::string::npos;
                };
                std::unordered_map<std::string, std::pair<unsigned, std::unordered_map<std::string, unsigned>>> sites;
                for (const OutsideSample& os : outside_samples)
                {
                    int at = 0;
                    std::string site;
                    for (; at < os.mCount; ++at)
                    {
                        site = name_of(os.mInside[at]);
                        if (!plumbing(site))
                        {
                            break;
                        }
                    }
                    if (at >= os.mCount)
                    {
                        site = name_of(os.mInside[0]);
                        at = 0;
                    }
                    const std::string caller = at + 1 < os.mCount ? name_of(os.mInside[at + 1]) : std::string("-");
                    auto& entry = sites[site];
                    ++entry.first;
                    ++entry.second[name_of(os.mLeaf)];
                    (void)caller;
                }
                std::vector<std::pair<unsigned, std::string>> otop;
                for (const auto& kv : sites)
                {
                    otop.emplace_back(kv.second.first, kv.first);
                }
                std::sort(otop.begin(), otop.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string otext;
                for (size_t i = 0; i < otop.size() && i < 30; ++i)
                {
                    const auto& entry = sites[otop[i].second];
                    std::vector<std::pair<unsigned, std::string>> leaves;
                    for (const auto& kv : entry.second)
                    {
                        leaves.emplace_back(kv.second, kv.first);
                    }
                    std::sort(leaves.begin(), leaves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                    otext += llformat(" | %s %.2f%% in", otop[i].second.c_str(), 100.0 * otop[i].first / total);
                    for (size_t q = 0; q < leaves.size() && q < 3; ++q)
                    {
                        otext += llformat(" %s %u%%", leaves[q].second.c_str(), (unsigned)(100.0 * leaves[q].first / otop[i].first + 0.5));
                    }
                }
                LL_INFOS("Sampler") << "main thread, window " << windows << ", time outside the executable (allocator, waits, CRT, kernel) by the nearest function inside it that is not the allocator's plumbing:" << otext << LL_ENDL;
            }
            counts.clear();
            modules.clear();
            inclusive.clear();
            callers.clear();
            outside_samples.clear();
            total = 0;
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
    static const double sample_ms = []()
    {
        const char* e = getenv("LL_VK_SAMPLE");
        return e ? atof(e) : 0.0;
    }();
    if (threshold <= 0.0 && sample_ms <= 0.0)
    {
        return;
    }
    static std::once_flag started;
    std::call_once(started, []()
    {
        if (threshold > 0.0)
        {
            std::thread(hw_thread, threshold).detach();
        }
        if (sample_ms > 0.0)
        {
            std::thread(sample_thread, sample_ms).detach();
        }
    });
    sHwMainTid.store(GetCurrentThreadId());
    sHwFrameNo.fetch_add(1);
    sHwLastFrame.store(hw_now());
}

// <FS> LL_VK_GROWTH=1: one line every 10 s from the main loop with what could be growing while the scene is flat (process private bytes, working set, page faults,
// object and active object counts, vertex buffer bytes). For the late creep in a long still (doc/vulkan/EXPIRY-design.md).
void rk_growth_probe()
{
    static const bool on = getenv("LL_VK_GROWTH") != nullptr;
    if (!on)
    {
        return;
    }
    static double last_at = 0.0;
    static DWORD last_faults = 0;
    const double now = LLTimer::getTotalSeconds();
    if (now - last_at < 10.0)
    {
        return;
    }
    const double dt = last_at > 0.0 ? now - last_at : 10.0;
    last_at = now;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    double best_step = 1e9;
    for (int k = 0; k < 5; ++k)
    {
        best_step = std::min(best_step, chain_ns_per_step());
    }
    LL_INFOS("Growth") << "private " << llformat("%.0f", pmc.PrivateUsage / 1048576.0) << " MB, working set " << llformat("%.0f", pmc.WorkingSetSize / 1048576.0) << " MB, page faults "
        << llformat("%.0f", (pmc.PageFaultCount - last_faults) / dt) << "/s | objects " << gObjectList.getNumObjects() << ", active " << gObjectList.getNumActiveObjects()
        << ", textures " << gTextureList.getNumImages() << ", core clock " << llformat("%.2f", 3.0 / best_step) << " GHz if 3 cycles a step"
        << ", vertex buffers " << llformat("%.0f", LLVertexBuffer::getBytesAllocated() / 1048576.0) << " MB" << LL_ENDL;
    last_faults = pmc.PageFaultCount;
}

// <FS> LL_VK_SYNC_LOG: names the functions that wait in RKVkAsync::syncCall
static const bool sSyncSymbolizerSet = []() { RKVkBridge::setSymbolizer(&rk_describe_address); return true; }();

// <FS> one stack address as text: module+offset (what a pdb reads) and the nearest symbol
std::string rk_describe_frame(const void* pc)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &module) && module)
    {
        GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* base_name = strrchr(name, '\\');
    return llformat("%s+0x%llx  %s", base_name ? base_name + 1 : name, (unsigned long long)((DWORD64)pc - (DWORD64)module), rk_describe_address(pc).c_str());
}

// <FS> the calling thread's stack as text, one frame a line (module+offset and the nearest symbol), for probes that log where something happened (LL_VK_DRAWABLE_TRACE)
std::string rk_stack_here(int skip, int max_frames)
{
    void* frames[48] = {};
    const USHORT n = CaptureStackBackTrace((DWORD)skip + 1, (DWORD)std::min(max_frames, 48), frames, nullptr);
    std::string text;
    for (USHORT i = 0; i < n; ++i)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &module) && module)
        {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        const char* base_name = strrchr(name, '\\');
        text += llformat("  %s+0x%llx  %s\n", base_name ? base_name + 1 : name, (unsigned long long)((DWORD64)frames[i] - (DWORD64)module), rk_describe_address(frames[i]).c_str());
    }
    return text;
}

// <FS> The faulting thread's stack when an access violation happens (written next to the executable as crash-stack-<pid>.txt, and to the log): the viewer's own crash handler leaves no dump here, and the
// Windows event only gives a module and an offset. A first-chance handler for the access violation only (C++ exceptions and the SEH the work queue converts are other codes); at most 6 are written.
// Off with LL_VK_NO_CRASHSTACK=1.
namespace
{
    LONG CALLBACK rk_crash_stack_handler(EXCEPTION_POINTERS* ep)
    {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        static std::atomic<int> written{0};
        if (written.fetch_add(1) >= 6)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        std::string text = llformat("exception 0x%08lx at %p on thread %lu, %s address %p\n", code, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(),
            ep->ExceptionRecord->NumberParameters >= 2 ? (ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "executing") : "?",
            ep->ExceptionRecord->NumberParameters >= 2 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
        CONTEXT ctx = *ep->ContextRecord;
        for (int i = 0; i < 40 && ctx.Rip; ++i)
        {
            HMODULE module = nullptr;
            char name[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &module) && module)
            {
                GetModuleFileNameA(module, name, MAX_PATH);
            }
            const char* base_name = strrchr(name, '\\');
            text += llformat("  %s+0x%llx  %s\n", base_name ? base_name + 1 : name, (unsigned long long)(ctx.Rip - (DWORD64)module), rk_describe_address((void*)ctx.Rip).c_str());
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (!fn)
            {
                if (IsBadReadPtr((void*)ctx.Rsp, 8)) { break; }
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
            }
            else
            {
                PVOID handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
            }
        }
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string file(path);
        file = file.substr(0, file.find_last_of('\\') + 1) + "crash-stack-" + std::to_string(GetCurrentProcessId()) + ".txt";
        if (FILE* f = fopen(file.c_str(), "ab"))
        {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
        LL_WARNS("CrashStack") << text << LL_ENDL;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const bool sCrashStackInstalled = []() {
        if (!getenv("LL_VK_NO_CRASHSTACK"))
        {
            AddVectoredExceptionHandler(1, rk_crash_stack_handler);
        }
        return true;
    }();
}

#else // Linux: the diagnostics below read the Windows debug help and process APIs; the entry points exist and do nothing, addresses are printed as numbers

std::string rk_describe_address(const void* pc)
{
    return llformat("0x%llx", (unsigned long long)(uintptr_t)pc);
}

void HitchWatch::frame()
{
}

void rk_growth_probe()
{
}

std::string rk_describe_frame(const void* pc)
{
    return rk_describe_address(pc);
}

std::string rk_stack_here(int skip, int max_frames)
{
    void* frames[48] = {};
    const USHORT n = CaptureStackBackTrace((DWORD)skip + 1, (DWORD)std::min(max_frames, 48), frames, nullptr);
    std::string text;
    for (USHORT i = 0; i < n; ++i)
    {
        text += llformat("  %s\n", rk_describe_address(frames[i]).c_str());
    }
    return text;
}

#endif // LL_WINDOWS
