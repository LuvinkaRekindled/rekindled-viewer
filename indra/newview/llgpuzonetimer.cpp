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
#include "llviewercontrol.h"

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

namespace
{
    struct Zone
    {
        const char* mName;
        GLuint mStart;
        GLuint mEnd;
    };

    struct Aggregate
    {
        double mTotalNs = 0.0;
        U64 mCalls = 0;
    };

    bool sHooked = false;
    std::thread::id sMainThread;

    std::vector<Zone> sCurrent;            // zones recorded this frame
    std::vector<U32> sOpen;                // indices into sCurrent of zones that have begun but not ended
    std::deque<std::vector<Zone>> sPending; // finished frames whose queries may still be in flight
    std::vector<GLuint> sFreeQueries;

    std::unordered_map<std::string, double> sCounters;   // summed since the last summary
    U64 sCounterFrames = 0;

    std::unordered_map<std::string, Aggregate> sAggregates;
    double sSpanNs = 0.0;                  // first zone start to last zone end, summed over measured frames
    U64 sMeasuredFrames = 0;
    F64 sLastLog = 0.0;

    GLuint acquireQuery()
    {
        if (!sFreeQueries.empty())
        {
            GLuint q = sFreeQueries.back();
            sFreeQueries.pop_back();
            return q;
        }
        GLuint q = 0;
        gGL.genQueries(1, &q);
        return q;
    }

    void releaseFrame(std::vector<Zone>& frame)
    {
        for (const Zone& z : frame)
        {
            sFreeQueries.push_back(z.mStart);
            sFreeQueries.push_back(z.mEnd);
        }
        frame.clear();
    }

    bool isMainThread()
    {
        return std::this_thread::get_id() == sMainThread;
    }

    // Hooks. Zones begun on other threads (shared-context image threads) are ignored, and since
    // the thread never changes between begin and end the two stay balanced.
    void zoneBegin(const char* name)
    {
        if (!isMainThread())
        {
            return;
        }
        Zone z{ name, acquireQuery(), 0 };
        gGL.queryTimestamp(z.mStart);
        sOpen.push_back((U32)sCurrent.size());
        sCurrent.push_back(z);
    }

    void zoneEnd()
    {
        if (!isMainThread() || sOpen.empty())
        {
            return;
        }
        Zone& z = sCurrent[sOpen.back()];
        sOpen.pop_back();
        z.mEnd = acquireQuery();
        gGL.queryTimestamp(z.mEnd);
    }

    void uninstall()
    {
        LLGpuZoneHooks::sBegin = nullptr;
        LLGpuZoneHooks::sEnd = nullptr;
        sHooked = false;

        // Zones that were open lose their end hook, so throw away the partial frame.
        for (Zone& z : sCurrent)
        {
            sFreeQueries.push_back(z.mStart);
            if (z.mEnd)
            {
                sFreeQueries.push_back(z.mEnd);
            }
        }
        sCurrent.clear();
        sOpen.clear();
        for (auto& frame : sPending)
        {
            releaseFrame(frame);
        }
        sPending.clear();
        sAggregates.clear();
        sSpanNs = 0.0;
        sMeasuredFrames = 0;
        sCounters.clear();
        sCounterFrames = 0;
    }

    // true if every end query of the frame has a result
    bool frameReady(const std::vector<Zone>& frame)
    {
        for (const Zone& z : frame)
        {
            GLint available = 0;
            gGL.getQueryObjectiv(z.mEnd, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
            {
                return false;
            }
        }
        return true;
    }

    void harvest(std::vector<Zone>& frame)
    {
        double first_start = 0.0;
        double last_end = 0.0;
        bool have_range = false;

        for (const Zone& z : frame)
        {
            GLuint64 t0 = 0;
            GLuint64 t1 = 0;
            gGL.getQueryObjectui64v(z.mStart, GL_QUERY_RESULT, &t0);
            gGL.getQueryObjectui64v(z.mEnd, GL_QUERY_RESULT, &t1);
            if (t1 < t0)
            {
                continue;
            }

            Aggregate& agg = sAggregates[z.mName];
            agg.mTotalNs += (double)(t1 - t0);
            agg.mCalls++;

            if (!have_range || (double)t0 < first_start)
            {
                first_start = (double)t0;
            }
            if (!have_range || (double)t1 > last_end)
            {
                last_end = (double)t1;
            }
            have_range = true;
        }

        if (have_range)
        {
            sSpanNs += last_end - first_start;
            sMeasuredFrames++;
        }
        releaseFrame(frame);
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
        if (!glQueryCounter || !glGetQueryObjectui64v || !glGenQueries || !glGetQueryObjectiv)
        {
            return;
        }
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

    // Hand the zones that have finished to the pending list; their queries resolve on the GPU later.
    // Zones that enclose the whole frame (such as "Frame" in the main loop) are still open here and
    // stay in place until they end, which puts them in the next batch.
    {
        std::vector<Zone> finished;
        std::vector<Zone> still_open;
        std::vector<int> remap(sCurrent.size(), -1);
        for (size_t i = 0; i < sCurrent.size(); ++i)
        {
            if (sCurrent[i].mEnd != 0)
            {
                finished.push_back(sCurrent[i]);
            }
            else
            {
                remap[i] = (int)still_open.size();
                still_open.push_back(sCurrent[i]);
            }
        }
        std::vector<U32> open_indices;
        for (U32 idx : sOpen)
        {
            if (remap[idx] >= 0)
            {
                open_indices.push_back((U32)remap[idx]);
            }
        }
        sCurrent = std::move(still_open);
        sOpen = std::move(open_indices);

        if (!finished.empty())
        {
            sPending.push_back(std::move(finished));
        }
    }

    while (!sPending.empty() && frameReady(sPending.front()))
    {
        harvest(sPending.front());
        sPending.pop_front();
    }

    // never let unfinished frames pile up
    while (sPending.size() > 8)
    {
        releaseFrame(sPending.front());
        sPending.pop_front();
    }

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
