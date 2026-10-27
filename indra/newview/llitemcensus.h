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
#include <algorithm>
#include <typeinfo>
#include <string>
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
        ALPHA_PLAN,     // LLDrawPoolAlpha::renderAlpha: ll_alpha_plan of each group (a: groups with a plan)
        ALPHA_TR_ADD,   // ... tr_add: one transient entry (a: rigged ones)
        ALPHA_TR_FLUSH, // ... tr_flush: a record of transient entries to the worker (a: entries)
        ALPHA_RUN_FLUSH,// ... run_flush: a record of ranges to the worker, palette bindings included (a: ranges)
        ALPHA_RUN_BEGIN,// ... run_begin: a new run's program and state
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
    struct UiLast { unsigned long long mHash = 0; unsigned mFrameChanged = 0; };
    inline std::unordered_map<const LLView*, UiLast> sUiLast;
    inline unsigned long long sUiAgeHist[9] = {};          // frames since a depth 4+ view last changed, when it changes: 1, 2, 3-4, 5-8, 9-16, 17-60, 61-300, over 300, first time
    inline unsigned long long sUiFramesAny = 0, sUiFramesAll = 0, sUiD4Changed = 0;     // frames with at least one depth 4 view changed / frames counted / depth 4 views changed in them
    inline bool sUiFrameChanged = false;
    inline unsigned sUiFrameNo = 0;
    // by name (depth 1-4, the path of the view): views, time, time and views of the unchanged ones
    inline std::unordered_map<std::string, UiDepth> sUiByName;
    struct UiFrame
    {
        const LLView* mView;
        unsigned long long mStart, mHashCyclesStart, mSavedAccum;
        unsigned long long mChildCycles = 0;       // the time of the views drawn inside this one (inclusive of the hashing), for the exclusive time by class
    };
    inline std::unordered_map<std::string, UiDepth> sUiByClass;       // exclusive time (own code, children and hashing left out) by widget class
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
            sUiStack.push_back({ view, __rdtsc(), LLRender::sUiHashCycles, LLRender::sUiHashAccum, 0 });
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
            const unsigned long long exclusive = total > f.mChildCycles + hashing ? total - f.mChildCycles - hashing : 0;       // (the children's hashing is inside their time: close enough)
            if (!sUiStack.empty())
            {
                sUiStack.back().mChildCycles += total;
            }
            LLRender::sUiHashAccum = (f.mSavedAccum ^ view_hash) * 0x100000001b3ULL;
            const size_t depth = llmin<size_t>(sUiStack.size() + 1, 7);
            UiLast& last = sUiLast[f.mView];
            const bool same = last.mHash == view_hash;
            last.mHash = view_hash;
            if (!same)
            {
                if (depth >= 4)
                {
                    const unsigned age = last.mFrameChanged ? sUiFrameNo - last.mFrameChanged : 0;
                    ++sUiAgeHist[!last.mFrameChanged ? 8 : age <= 1 ? 0 : age == 2 ? 1 : age <= 4 ? 2 : age <= 8 ? 3 : age <= 16 ? 4 : age <= 60 ? 5 : age <= 300 ? 6 : 7];
                }
                if (depth == 4)
                {
                    sUiFrameChanged = true;
                    ++sUiD4Changed;
                }
                last.mFrameChanged = sUiFrameNo;
            }
            UiDepth& d = sUiDepth[depth];
            ++d.mViews;
            d.mCycles += own;
            if (same)
            {
                ++d.mSameViews;
                d.mSameCycles += own;
            }
            {
                UiDepth& c = sUiByClass[typeid(*f.mView).name()];
                ++c.mViews;
                c.mCycles += exclusive;
                if (same)
                {
                    ++c.mSameViews;
                    c.mSameCycles += exclusive;
                }
            }
            if (depth <= 6)
            {
                std::string path = f.mView->getName();
                for (const LLView* p = f.mView->getParent(); p && p->getParent(); p = p->getParent())
                {
                    path = p->getName() + "/" + path;
                }
                UiDepth& n = sUiByName[std::to_string(depth) + " " + path];
                ++n.mViews;
                n.mCycles += own;
                if (same)
                {
                    ++n.mSameViews;
                    n.mSameCycles += own;
                }
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
        ++sUiFrameNo;
        if (sUiFrameNo > 5)
        {
            ++sUiFramesAll;
            sUiFramesAny += sUiFrameChanged;
        }
        sUiFrameChanged = false;
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
        static const char* const names[SLOT_COUNT] = { "idle avatars", "idle attachments", "idle other moving", "idle other still", "nearby lights", "name tags", "octree balance", "apparent angles", "alpha plans", "alpha tr_add", "alpha tr_flush", "alpha run_flush", "alpha run_begin" };
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
            {
                std::vector<std::pair<std::string, UiDepth>> rows(sUiByName.begin(), sUiByName.end());
                std::sort(rows.begin(), rows.end(), [](const auto& l, const auto& r) { return l.second.mCycles > r.second.mCycles; });
                std::string top;
                for (size_t i = 0; i < rows.size() && i < 40; ++i)
                {
                    const UiDepth& u = rows[i].second;
                    top += llformat(" | %s: %.3f ms (%.3f unchanged, %.1f views)", rows[i].first.c_str(), 1000.0 * (double)u.mCycles / hz / frames, 1000.0 * (double)u.mSameCycles / hz / frames, (double)u.mViews / frames);
                }
                LL_INFOS("ItemCensus") << "ui views by path, top by time:" << top << LL_ENDL;
                sUiByName.clear();
                std::vector<std::pair<std::string, UiDepth>> classes(sUiByClass.begin(), sUiByClass.end());
                std::sort(classes.begin(), classes.end(), [](const auto& l, const auto& r) { return l.second.mCycles > r.second.mCycles; });
                std::string by_class;
                for (size_t i = 0; i < classes.size() && i < 30; ++i)
                {
                    const UiDepth& u = classes[i].second;
                    by_class += llformat(" | %s: %.3f ms exclusive (%.3f in unchanged views), %.1f views (%.1f changed)", classes[i].first.c_str(), 1000.0 * (double)u.mCycles / hz / frames, 1000.0 * (double)u.mSameCycles / hz / frames,
                                         (double)u.mViews / frames, (double)(u.mViews - u.mSameViews) / frames);
                }
                LL_INFOS("ItemCensus") << "ui views by class (exclusive time, depth 1-6):" << by_class << LL_ENDL;
                sUiByClass.clear();
            }
            {
                static const char* const slot_names[LLRender::UI_CENSUS_SLOTS] = { "text (LLFontGL::render)", "images (gl_draw_*image*, segmented rects)", "rects / lines / shadows", "flush with vertices (inside the others)", "matrix push/pop/translate",
                                                                                     "clip rects (ctor + dtor)", "texture binds (inside the others)", "calcScreenRect", "views drawn, all depths (calls only)", "empty flush calls (calls only)", "views drawn entirely outside the clip rect (calls = views)" };
                // what one timing scope costs, to be subtracted from the slots with many calls
                static double scope_ns = 0.0;
                if (scope_ns == 0.0)
                {
                    const unsigned long long c0 = __rdtsc();
                    for (int k = 0; k < 20000; ++k)
                    {
                        LLRender::UiCensusScope probe(9);
                    }
                    scope_ns = 1.0e9 * (double)(__rdtsc() - c0) / hz / 20000.0;
                    LLRender::sUiCensusCycles[9] = 0;
                    LLRender::sUiCensusCalls[9] = 0;
                }
                std::string parts;
                for (int k = 0; k < LLRender::UI_CENSUS_SLOTS; ++k)
                {
                    parts += llformat(" | %s: %.3f ms (about %.3f of it the timing itself), %.1f calls", slot_names[k], 1000.0 * (double)LLRender::sUiCensusCycles[k] / hz / frames,
                                      (k == 8 || k == 9) ? 0.0 : 1.0e-6 * scope_ns * (double)LLRender::sUiCensusCalls[k] / frames, (double)LLRender::sUiCensusCalls[k] / frames);
                    LLRender::sUiCensusCycles[k] = 0;
                    LLRender::sUiCensusCalls[k] = 0;
                }
                LL_INFOS("ItemCensus") << "ui drawing entry points per frame (inclusive; the hashing is inside them; one timing scope costs " << scope_ns << " ns), vertices flushed " << (double)LLRender::sUiCensusVerts / frames << " a frame ("
                                       << 28.0 * (double)LLRender::sUiCensusVerts / frames / 1024.0 << " KB of record bytes):" << parts << LL_ENDL;
                LLRender::sUiCensusVerts = 0;
                {
                    static const char* const age_names[9] = { "1", "2", "3-4", "5-8", "9-16", "17-60", "61-300", "over 300", "first" };
                    std::string ages;
                    unsigned long long total_changes = 0;
                    for (int k = 0; k < 9; ++k) { total_changes += sUiAgeHist[k]; }
                    for (int k = 0; k < 9; ++k)
                    {
                        ages += llformat(" | %s frames: %.2f%%", age_names[k], total_changes ? 100.0 * (double)sUiAgeHist[k] / (double)total_changes : 0.0);
                        sUiAgeHist[k] = 0;
                    }
                    LL_INFOS("ItemCensus") << "ui: depth 4+ views changing, " << (double)total_changes / frames << " per frame, by frames since their last change:" << ages
                                           << "; frames where some depth 4 view changed " << (sUiFramesAll ? 100.0 * (double)sUiFramesAny / (double)sUiFramesAll : 0.0) << "% (" << (sUiFramesAll ? (double)sUiD4Changed / (double)sUiFramesAll : 0.0) << " depth 4 views a frame)" << LL_ENDL;
                    sUiFramesAny = sUiFramesAll = sUiD4Changed = 0;
                }
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
