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
#include "llglheaders.h"
#include "llvkzones.h"
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
    }
}

LLCpuZoneScope::~LLCpuZoneScope()
{
    if (!mName)
    {
        return;
    }
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
    for (unsigned i = 0; i < sCpuZoneCount; ++i)
    {
        sCpuZones[i].mNanos = 0;
        sCpuZones[i].mCalls = 0;
    }
    sCpuFrames = 0;
}
