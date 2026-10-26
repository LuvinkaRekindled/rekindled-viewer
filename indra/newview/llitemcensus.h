/**
 * @file llitemcensus.h
 * @brief LL_VK_ITEMCENSUS=1: time and count the small main thread items of doc/vulkan/overview-brief/REPORT-fable.md (ideas 4, 5, 6, 9, 12)
 *
 * The cpu zone line of the log lists only the largest zones, so these small items need a line of their own: every 600 frames one
 * "ItemCensus" line with the ms a frame and the counts a frame of each item. The timing is the processor's time stamp counter
 * (about 10 ns a reading), calibrated against the performance counter between the first and the last frame of the window.
 * Main thread only. Off unless the variable is set.
 */

#ifndef LL_LLITEMCENSUS_H
#define LL_LLITEMCENSUS_H

#include "linden_common.h"
#include "llframetimer.h"
#include "llview.h"
#include "llrender.h"
#include <unordered_map>
#include <vector>
#include <intrin.h>
#include <chrono>

namespace ItemCensus
{
    enum Slot
    {
        IDLE_AV,        // idleUpdate of avatars
        IDLE_ATT,       // ... of attachments
        IDLE_MOVING,    // ... of other objects with a velocity, acceleration or angular velocity
        IDLE_STILL,     // ... of other objects with none of them (a: of these, with a drawable)
        LIGHTS,         // LLPipeline::calcNearbyLights (a: lights in mLights, b: nearby lights kept)
        NAMETAGS,       // LLHUDNameTag::updateAll (a: visible tags)
        BALANCE,        // octree balance of all partitions (a: trees balanced)
        ANGLES,         // LLViewerObjectList::updateApparentAngles (a: objects visited, b: of these with a visible drawable)
        SLOT_COUNT
    };

    struct Acc
    {
        unsigned long long mCycles = 0, mCalls = 0, mA = 0, mB = 0;
    };

    inline Acc sAcc[SLOT_COUNT];

    inline bool on()
    {
        static const bool value = []() { const char* e = getenv("LL_VK_ITEMCENSUS"); return e && atoi(e) != 0; }();
        return value;
    }

    inline void add(Slot slot, unsigned long long a, unsigned long long b = 0)
    {
        if (on())
        {
            sAcc[slot].mA += a;
            sAcc[slot].mB += b;
        }
    }

    struct Scope
    {
        Acc* mAcc;
        unsigned long long mStart;
        explicit Scope(Slot slot) : mAcc(on() ? &sAcc[slot] : nullptr), mStart(mAcc ? __rdtsc() : 0) {}
        ~Scope()
        {
            if (mAcc)
            {
                mAcc->mCycles += __rdtsc() - mStart;
                ++mAcc->mCalls;
            }
        }
    };

    // the UI: for each view down to depth 6 (the views LLView::drawChildren reports to sDrawHook): how many, how much time inclusive, and how much of that time the view
    // emitted exactly what it emitted in the previous frame (the hash of its vertices and matrices, children included); the time of the hashing itself is subtracted
    struct UiDepth
    {
        unsigned long long mViews = 0, mCycles = 0, mSameViews = 0, mSameCycles = 0;
    };
    inline UiDepth sUiDepth[8];
    inline std::unordered_map<const LLView*, unsigned long long> sUiLast;
    struct UiFrame
    {
        const LLView* mView;
        unsigned long long mStart, mHashCyclesStart, mSavedAccum;
    };
    inline std::vector<UiFrame> sUiStack;
    inline void (*sUiPrevHook)(const LLView*, bool) = nullptr;

    inline void ui_hook(const LLView* view, bool begin)
    {
        if (sUiPrevHook && begin)
        {
            sUiPrevHook(view, begin);
        }
        if (begin)
        {
            sUiStack.push_back({ view, __rdtsc(), LLRender::sUiHashCycles, LLRender::sUiHashAccum });
            LLRender::sUiHashAccum = 0;
        }
        else if (!sUiStack.empty())
        {
            const UiFrame f = sUiStack.back();
            sUiStack.pop_back();
            const unsigned long long view_hash = LLRender::sUiHashAccum;
            const unsigned long long total = __rdtsc() - f.mStart;
            const unsigned long long hashing = LLRender::sUiHashCycles - f.mHashCyclesStart;
            const unsigned long long own = total > hashing ? total - hashing : 0;
            LLRender::sUiHashAccum = (f.mSavedAccum ^ view_hash) * 0x100000001b3ULL;
            const size_t depth = llmin<size_t>(sUiStack.size() + 1, 7);
            unsigned long long& last = sUiLast[f.mView];
            const bool same = last == view_hash;
            last = view_hash;
            UiDepth& d = sUiDepth[depth];
            ++d.mViews;
            d.mCycles += own;
            if (same)
            {
                ++d.mSameViews;
                d.mSameCycles += own;
            }
        }
        if (sUiPrevHook && !begin)
        {
            sUiPrevHook(view, begin);
        }
    }

    // once a frame; logs and restarts the window every 600 frames
    inline void frame()
    {
        if (!on())
        {
            return;
        }
        static int sCalls = 0;
        if (++sCalls == 5)
        {
            // after the cpu zones installed their own hook (they do it in the first frame): ours calls it
            sUiPrevHook = LLView::sDrawHook;
            LLView::sDrawHook = &ui_hook;
            LLRender::sUiHashing = true;
        }
        static unsigned long long sTsc0 = 0;
        static std::chrono::steady_clock::time_point sTime0;
        static U32 sFrame0 = 0;
        const U32 now_frame = LLFrameTimer::getFrameCount();
        if (!sTsc0)
        {
            sTsc0 = __rdtsc();
            sTime0 = std::chrono::steady_clock::now();
            sFrame0 = now_frame;
            return;
        }
        const U32 frames = now_frame - sFrame0;
        if (frames < 600)
        {
            return;
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - sTime0).count();
        const double hz = (double)(__rdtsc() - sTsc0) / seconds;
        static const char* const names[SLOT_COUNT] = { "idle avatars", "idle attachments", "idle other moving", "idle other still", "nearby lights", "name tags", "octree balance", "apparent angles" };
        std::string line;
        for (int i = 0; i < SLOT_COUNT; i++)
        {
            const Acc& acc = sAcc[i];
            line += llformat(" | %s %.3f ms, %.1f calls, a %.1f, b %.1f", names[i], 1000.0 * (double)acc.mCycles / hz / frames, (double)acc.mCalls / frames, (double)acc.mA / frames, (double)acc.mB / frames);
            sAcc[i] = Acc();
        }
        {
            static unsigned long long sHashCycles0 = 0;
            std::string ui;
            for (int d = 1; d <= 7; d++)
            {
                const UiDepth& u = sUiDepth[d];
                ui += llformat(" | depth %d%s: %.1f views, %.3f ms, %.3f ms unchanged (%.1f views)", d, d == 7 ? "+" : "", (double)u.mViews / frames, 1000.0 * (double)u.mCycles / hz / frames,
                               1000.0 * (double)u.mSameCycles / hz / frames, (double)u.mSameViews / frames);
                sUiDepth[d] = UiDepth();
            }
            LL_INFOS("ItemCensus") << "ui views per frame (hashing excluded, it cost " << 1000.0 * (double)(LLRender::sUiHashCycles - sHashCycles0) / hz / frames << " ms a frame):" << ui << LL_ENDL;
            sHashCycles0 = LLRender::sUiHashCycles;
        }
        LL_INFOS("ItemCensus") << "per frame over " << frames << " frames (" << seconds << " s, loop " << 1000.0 * seconds / frames << " ms)" << line << LL_ENDL;
        sTsc0 = __rdtsc();
        sTime0 = std::chrono::steady_clock::now();
        sFrame0 = now_frame;
    }
}

#endif // LL_LLITEMCENSUS_H
