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
    };
    CpuZone sCpuZones[512];
    unsigned sCpuZoneCount = 0;
    std::thread::id sCpuMainThread;
    bool sCpuMainThreadKnown = false;
    const bool sCpuZonesOn = getenv("FS_CPU_ZONES") != nullptr;
    unsigned sCpuFrames = 0;
    long long sCpuFirstFrame = 0;

    long long cpu_now()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

LLCpuZoneScope::LLCpuZoneScope(const char* name)
    : mName(nullptr), mStart(0)
{
    if (sCpuZonesOn && sCpuMainThreadKnown && std::this_thread::get_id() == sCpuMainThread)
    {
        mName = name;
        mStart = cpu_now();
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
    for (unsigned i = 0; i < sCpuZoneCount; ++i)
    {
        if (sCpuZones[i].mName == mName)
        {
            sCpuZones[i].mNanos += d;
            ++sCpuZones[i].mCalls;
            return;
        }
    }
    if (sCpuZoneCount < sizeof(sCpuZones) / sizeof(sCpuZones[0]))
    {
        sCpuZones[sCpuZoneCount].mName = mName;
        sCpuZones[sCpuZoneCount].mNanos = d;
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
    LL_INFOS("UniCensus") << "per frame: uniform writes " << (double)sTotal.mCalls / frames << " (changed " << (double)sTotal.mChanged / frames << ", " << (double)sTotal.mBytes / frames << " bytes), texture binds "
        << (double)sTotal.mTex / frames << ", shader binds " << (double)sTotal.mBinds / frames << ", programs seen " << sProgs.size() << LL_ENDL;
    {
        std::vector<std::pair<const char*, Counters*>> zones;
        for (auto& kv : sByZone)
        {
            if (kv.second.mCalls || kv.second.mTex || kv.second.mBinds)
            {
                zones.push_back({ kv.first, &kv.second });
            }
        }
        std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) { return a.second->mCalls + a.second->mTex + 4 * a.second->mBinds > b.second->mCalls + b.second->mTex + 4 * b.second->mBinds; });
        std::string text;
        size_t n = 0;
        for (auto& z : zones)
        {
            if (++n > 40) break;
            text += llformat(" | %s: writes %.1f (changed %.1f), tex %.1f, binds %.1f, entered %.1f", z.first, (double)z.second->mCalls / frames, (double)z.second->mChanged / frames, (double)z.second->mTex / frames,
                             (double)z.second->mBinds / frames, (double)z.second->mScopes / frames);
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
    }
    if (sCpuFrames == 0)
    {
        sCpuFirstFrame = cpu_now();
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
        text += llformat(" | %s %.2f ms (%.1f calls)", z.mName, (double)z.mNanos / 1.0e6 / frames, (double)z.mCalls / frames);
    }
    LL_INFOS("CpuZones") << "main thread per frame over " << sCpuFrames << " frames: loop interval " << interval_ms << " ms (inclusive zones, nested ones count in their parents)" << text << LL_ENDL;
    uni_census_report(frames);
    for (unsigned i = 0; i < sCpuZoneCount; ++i)
    {
        sCpuZones[i].mNanos = 0;
        sCpuZones[i].mCalls = 0;
    }
    sCpuFrames = 0;
}
