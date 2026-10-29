/**
 * @file fstexturearrivaltrace.cpp
 * @brief A per-texture arrival trace (doc/vulkan/PLAN-aurora-ideas.md section 6, step one). See the header.
 */

#include "llviewerprecompiledheaders.h"
#include "fstexturearrivaltrace.h"

#include "lldir.h"
#include "llfile.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llstartup.h"

#include <mutex>
#include <vector>

namespace FSTextureArrivalTrace
{
    std::atomic<bool> sEnabled { false };

    namespace
    {
        struct Rec
        {
            F64 mTime;              // seconds since the trace opened
            const char* mEvent;
            LLUUID mID;
            S32 mA, mB;
            S64 mBytes;
            F32 mExtra;
        };
        std::mutex sMutex;
        std::vector<Rec> sRows;
        LLFILE* sFile = nullptr;
        F64 sOpenedAt = 0.0;
        bool sWorldStamped = false;
        U64 sRowsWritten = 0;
    }

    void note(const char* event, const LLUUID& id, S32 a, S32 b, S64 bytes, F32 extra)
    {
        if (!enabled()) return;
        Rec r { LLTimer::getElapsedSeconds() - sOpenedAt, event, id, a, b, bytes, extra };
        std::lock_guard<std::mutex> lock(sMutex);
        sRows.push_back(r);
    }

    static void flush()
    {
        std::vector<Rec> rows;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            rows.swap(sRows);
        }
        if (!sFile) return;
        for (const Rec& r : rows)
        {
            fprintf(sFile, "%.1f,%s,%s,%d,%d,%lld,%.1f\n", r.mTime * 1000.0, r.mEvent, r.mID.asString().c_str(), r.mA, r.mB, (long long)r.mBytes, r.mExtra);
        }
        sRowsWritten += rows.size();
        if (!rows.empty()) fflush(sFile);
    }

    void update()
    {
        static LLCachedControl<bool> on(gSavedSettings, "FSTextureArrivalTrace", false);
        if (on() && !sFile)
        {
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "texture-arrival.csv");
            sFile = LLFile::fopen(path, "wb");
            if (sFile)
            {
                sOpenedAt = LLTimer::getElapsedSeconds();
                sWorldStamped = false;
                sRowsWritten = 0;
                fprintf(sFile, "ms,event,id,a,b,bytes,extra\n");
                sEnabled.store(true, std::memory_order_relaxed);
                LL_INFOS("TextureArrivalTrace") << "texture arrival trace on: " << path << LL_ENDL;
            }
        }
        else if (!on() && sFile)
        {
            sEnabled.store(false, std::memory_order_relaxed);
            flush();
            fclose(sFile);
            sFile = nullptr;
            LL_INFOS("TextureArrivalTrace") << "texture arrival trace off, " << sRowsWritten << " rows" << LL_ENDL;
        }
        if (!sFile) return;
        if (!sWorldStamped && LLStartUp::getStartupState() >= STATE_STARTED)
        {
            sWorldStamped = true;
            note("world", LLUUID::null);
        }
        flush();
    }

    void shutdown()
    {
        if (sFile)
        {
            sEnabled.store(false, std::memory_order_relaxed);
            flush();
            fclose(sFile);
            sFile = nullptr;
        }
    }
}
