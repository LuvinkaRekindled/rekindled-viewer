/**
 * @file llvkpasscensus.cpp
 * @brief S6.0 pass state census (see llvkpasscensus.h and doc/vulkan/S6-design.md).
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkpasscensus.h"
#include "llvkasync.h"
#include "llvkrenderer.h"
#include "llvkworker.h"

#include "llvkplatform.h"
#include "llvkplatform.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace LLVkPassCensus
{
bool sOn = false;
int sMode = 1;
int sClose = 1;
int sPoisonMask = 127;
U64 sPass = 0;
U64 sFrame = 0;

namespace
{
constexpr int NKIND = 40;

// what one pass (or the sum of many passes of one kind) did: every field adds up
struct Rec
{
    U64 mPasses = 0, mRecords = 0, mDraws = 0, mPackets = 0;
    U64 mTicks[3] = {};                     // draw records, packet records, everything else
    U64 mProgs = 0;                         // distinct programs drawn with
    U64 mState[AGE_COUNT] = {};             // first read of the state block in the pass, by the age of the record that last wrote it
    U64 mPktProg[AGE_COUNT] = {};           // a packet record's program (set by an earlier bind record or by this pass)
    U64 mUnit[AGE_COUNT] = {};              // first read of each texture unit
    U64 mUbo[AGE_COUNT] = {};               // first read of each uniform buffer binding
    U64 mCls[5][AGE_COUNT] = {};            // first read of each uniform class block (draw time), by class
    U64 mClsBytes[AGE_COUNT] = {};          // ... and the bytes of those blocks
    U64 mWritten = 0;                       // bytes the stream wrote in this pass into the class blocks it read
    U64 mBindCarried = 0, mBindInherited = 0;
    U64 mKinds[NKIND] = {};                 // packet records by kind
    std::map<std::string, U64> mProgDraws; // S6.6: draw records by program (what still goes through the draw record)
    U64 mRefused[6] = {};                   // draws the worker refused in this pass: no program, no target, no vertex input, no index buffer, refused by the renderer, outside a frame
    void add(const Rec& o)
    {
        mPasses += o.mPasses; mRecords += o.mRecords; mDraws += o.mDraws; mPackets += o.mPackets; mProgs += o.mProgs; mWritten += o.mWritten;
        mBindCarried += o.mBindCarried; mBindInherited += o.mBindInherited;
        for (int i = 0; i < 3; ++i) mTicks[i] += o.mTicks[i];
        for (int a = 0; a < AGE_COUNT; ++a)
        {
            mState[a] += o.mState[a]; mPktProg[a] += o.mPktProg[a]; mUnit[a] += o.mUnit[a]; mUbo[a] += o.mUbo[a]; mClsBytes[a] += o.mClsBytes[a];
            for (int c = 0; c < 5; ++c) mCls[c][a] += o.mCls[c][a];
        }
        for (int k = 0; k < NKIND; ++k) mKinds[k] += o.mKinds[k];
        for (int k = 0; k < 6; ++k) mRefused[k] += o.mRefused[k];
        for (const auto& kv : o.mProgDraws) mProgDraws[kv.first] += kv.second;
    }
};
struct Agg { Rec mSum; std::string mFirstProg; };

Rec gCur;                                   // the pass being played
S32 gCurFbo = -3;
U32 gCurSite = 0;
std::string gLabel;                         // modes 2 and 3: the pass is named by this instead of the bind site
std::vector<std::string> gZones;
std::vector<const void*> gProgs;
std::string gFirstProg;
bool gStateSeen = false, gPktProgSeen = false, gBindSeen = false;
U64 gUnitReadPass[32] = {}, gUboReadPass[4] = {};
Stamp gState, gProgSet, gUnit[32], gUbo[4];
U64 gPrepareFrame = ~0ull, gGeoReads = 0, gGeoStale = 0;
std::map<std::string, Agg> gAgg;
Rec gAll;
U64 gPresents = 0;
U64 gTsc0 = 0, gQpc0 = 0;

// per program: how its class blocks were first read in the passes (what a re-announce at the first use in a pass would send)
struct ProgAgg { U64 mReads[5] = {}; U64 mAge[AGE_COUNT] = {}; U64 mBytes = 0; U64 mPasses = 0; U64 mLastPass = 0; std::string mName; };
std::map<const char*, ProgAgg> gProgAgg;

std::mutex gSiteMutex;
std::vector<std::string> gSites;
std::map<std::pair<const char*, int>, U32> gSiteIds[3];        // by file pointer and line, per kind

const char* kindName(int k)
{
    static char buf[8][12];
    static int n = 0;
    char* b = buf[n++ & 7];
    snprintf(b, 12, "%d", k);
    return b;
}

U64 gRefusedLast[6] = {};

void endPass()
{
    {
        // the counters are per frame (reset at the frame's end): a smaller value than before is a new frame
        U64 now[6];
        LLVkWorker::refusals(now);
        for (int k = 0; k < 6; ++k)
        {
            gCur.mRefused[k] += now[k] >= gRefusedLast[k] ? now[k] - gRefusedLast[k] : now[k];
            gRefusedLast[k] = now[k];
        }
    }
    if (!gCur.mRecords)
    {
        return;
    }
    gCur.mPasses = 1;
    gCur.mProgs = gProgs.size();
    std::string key;
    if (gCurFbo == -2) key = "(frame start)";
    else if (gCurFbo == -4) key = "(after present)";
    else if (!gLabel.empty()) key = gLabel;
    else
    {
        key = siteName(gCurSite);
        key += gCur.mDraws || gCur.mPackets ? " | " : " | (no draws) ";
        if (gCur.mDraws || gCur.mPackets)
        {
            LLVk::Renderer& r = LLVk::Renderer::instance();
            key += r.describeTarget();
        }
    }
    Agg& a = gAgg[key];
    a.mSum.add(gCur);
    if (a.mFirstProg.empty()) a.mFirstProg = gFirstProg;
    gAll.add(gCur);
}

void beginPass(S32 fbo, U32 site)
{
    endPass();
    gCur = Rec();
    gProgs.clear();
    gFirstProg.clear();
    gStateSeen = gPktProgSeen = gBindSeen = false;
    gCurFbo = fbo;
    gCurSite = site;
    gLabel.clear();
    ++sPass;
}

void report()
{
    const double f = (double)std::max<U64>(1, gPresents);
    const double ratio = [&]() {
        LARGE_INTEGER q, fq;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&fq);
        const double secs = (double)(q.QuadPart - (long long)gQpc0) / (double)fq.QuadPart;
        const double ticks = (double)(__rdtsc() - gTsc0);
        return secs > 0 && ticks > 0 ? secs * 1e6 / ticks : 0.0;     // microseconds per TSC tick
    }();
    std::vector<std::pair<double, std::string>> rows;
    for (const auto& kv : gAgg)
    {
        const Rec& s = kv.second.mSum;
        rows.push_back({ (double)(s.mTicks[0] + s.mTicks[1] + s.mTicks[2]), kv.first });
    }
    std::sort(rows.rbegin(), rows.rend());
    auto line = [&](const std::string& key, const Rec& s, const std::string& first)
    {
        const double n = (double)std::max<U64>(1, s.mPasses);
        char b[1500];
        const double us_all = (double)(s.mTicks[0] + s.mTicks[1] + s.mTicks[2]) * ratio / f;
        int w = snprintf(b, sizeof(b),
            "%s: %.2f/frame, per pass: %.0f records %.1f draws %.1f packets %.1f programs, worker %.0f us/frame (draws %.0f packets %.0f other %.0f), first program %s | "
            "state block own/frame/older/never %.2f/%.2f/%.2f/%.2f | packet program %.2f/%.2f/%.2f/%.2f | texture units %.1f/%.1f/%.1f/%.1f | uniform buffers %.2f/%.2f/%.2f/%.2f | "
            "uniform class blocks read %.1f/%.1f/%.1f/%.1f (%.0f/%.0f/%.0f/%.0f bytes), stream wrote %.0f bytes of them | binding carried %.2f inherited %.2f | classes (pass/draw/material/light/skin):",
            key.c_str(), (double)s.mPasses / f, (double)s.mRecords / n, (double)s.mDraws / n, (double)s.mPackets / n, (double)s.mProgs / n, us_all,
            (double)s.mTicks[0] * ratio / f, (double)s.mTicks[1] * ratio / f, (double)s.mTicks[2] * ratio / f, first.empty() ? "-" : first.c_str(),
            (double)s.mState[0] / n, (double)s.mState[1] / n, (double)s.mState[2] / n, (double)s.mState[3] / n,
            (double)s.mPktProg[0] / n, (double)s.mPktProg[1] / n, (double)s.mPktProg[2] / n, (double)s.mPktProg[3] / n,
            (double)s.mUnit[0] / n, (double)s.mUnit[1] / n, (double)s.mUnit[2] / n, (double)s.mUnit[3] / n,
            (double)s.mUbo[0] / n, (double)s.mUbo[1] / n, (double)s.mUbo[2] / n, (double)s.mUbo[3] / n,
            (double)(s.mCls[0][0] + s.mCls[1][0] + s.mCls[2][0] + s.mCls[3][0] + s.mCls[4][0]) / n, (double)(s.mCls[0][1] + s.mCls[1][1] + s.mCls[2][1] + s.mCls[3][1] + s.mCls[4][1]) / n,
            (double)(s.mCls[0][2] + s.mCls[1][2] + s.mCls[2][2] + s.mCls[3][2] + s.mCls[4][2]) / n, (double)(s.mCls[0][3] + s.mCls[1][3] + s.mCls[2][3] + s.mCls[3][3] + s.mCls[4][3]) / n,
            (double)s.mClsBytes[0] / n, (double)s.mClsBytes[1] / n, (double)s.mClsBytes[2] / n, (double)s.mClsBytes[3] / n, (double)s.mWritten / n,
            (double)s.mBindCarried / n, (double)s.mBindInherited / n);
        for (int c = 0; c < 5 && w > 0 && w < (int)sizeof(b); ++c)
        {
            w += snprintf(b + w, sizeof(b) - w, " %.1f/%.1f/%.1f/%.1f", (double)s.mCls[c][0] / n, (double)s.mCls[c][1] / n, (double)s.mCls[c][2] / n, (double)s.mCls[c][3] / n);
        }
        std::string ks;
        for (int k = 0; k < NKIND; ++k)
        {
            if (s.mKinds[k])
            {
                char kb[32];
                snprintf(kb, sizeof(kb), " k%s x%.1f", kindName(k), (double)s.mKinds[k] / n);
                ks += kb;
            }
        }
        std::string pd;
        {
            std::vector<std::pair<U64, std::string>> by;
            for (const auto& kv : s.mProgDraws) by.push_back({ kv.second, kv.first });
            std::sort(by.rbegin(), by.rend());
            for (size_t i = 0; i < by.size() && i < 6; ++i)
            {
                char pb[160];
                snprintf(pb, sizeof(pb), " %s x%.1f", by[i].second.c_str(), (double)by[i].first / f);
                pd += pb;
            }
        }
        char rb[160] = "";
        if (s.mRefused[0] | s.mRefused[1] | s.mRefused[2] | s.mRefused[3] | s.mRefused[4] | s.mRefused[5])
        {
            snprintf(rb, sizeof(rb), " | REFUSED per frame: no program %.2f, no target %.2f, no vertex input %.2f, no index buffer %.2f, renderer %.2f, outside a frame %.2f",
                     (double)s.mRefused[0] / f, (double)s.mRefused[1] / f, (double)s.mRefused[2] / f, (double)s.mRefused[3] / f, (double)s.mRefused[4] / f, (double)s.mRefused[5] / f);
        }
        LL_INFOS("VkPassCensus") << b << " | packet kinds per pass:" << ks << rb << " | DRAW RECORDS per frame by program:" << pd << LL_ENDL;
    };
    LL_INFOS("VkPassCensus") << "pass census over " << gPresents << " presented frames (ages: written in this pass / earlier in this frame / in an earlier frame / never by the stream; "
        << "uniform class blocks written by the worker itself (bindless slots in the draw class) are not stamped; 'frame start' and 'after present' are the records outside any pass; the table rows are the top by worker time)" << LL_ENDL;
    line("ALL PASSES", gAll, "");
    for (size_t i = 0; i < rows.size() && i < 48; ++i)
    {
        const Agg& a = gAgg[rows[i].second];
        line(rows[i].second, a.mSum, a.mFirstProg);
    }
    {
        // programs by the bytes their class blocks were first-read for in passes: the cost of announcing them at the first use in a pass
        std::vector<std::pair<U64, const char*>> prog_rows;
        for (const auto& kv : gProgAgg) prog_rows.push_back({ kv.second.mBytes, kv.first });
        std::sort(prog_rows.rbegin(), prog_rows.rend());
        U64 total = 0, total_reads = 0;
        for (const auto& kv : gProgAgg) { total += kv.second.mBytes; for (int c = 0; c < 5; ++c) total_reads += kv.second.mReads[c]; }
        LL_INFOS("VkPassCensus") << "uniform class blocks first-read in passes, per frame: " << (double)total_reads / f << " blocks, " << (double)total / f << " bytes (a full announce at the first use in a pass would send that), "
                                 << gProgAgg.size() << " programs; top programs by bytes:" << LL_ENDL;
        for (size_t i = 0; i < prog_rows.size() && i < 30; ++i)
        {
            const ProgAgg& pa = gProgAgg[prog_rows[i].second];
            char b[400];
            snprintf(b, sizeof(b), "  %s: %.1f passes/frame, %.0f bytes/frame, blocks by class %.1f/%.1f/%.1f/%.1f/%.1f, written by the stream own/frame/older/never %.1f/%.1f/%.1f/%.1f",
                     pa.mName.c_str(), (double)pa.mPasses / f, (double)pa.mBytes / f, (double)pa.mReads[0] / f, (double)pa.mReads[1] / f, (double)pa.mReads[2] / f, (double)pa.mReads[3] / f, (double)pa.mReads[4] / f,
                     (double)pa.mAge[0] / f, (double)pa.mAge[1] / f, (double)pa.mAge[2] / f, (double)pa.mAge[3] / f);
            LL_INFOS("VkPassCensus") << b << LL_ENDL;
        }
    }
    gProgAgg.clear();
    LL_INFOS("VkPassCensus") << "frame inputs: packet records that read the prepare output " << gGeoReads << ", of them from a frame without a prepare record " << gGeoStale << LL_ENDL;
    gAgg.clear();
    gAll = Rec();
    gGeoReads = gGeoStale = 0;
    gPresents = 0;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    gQpc0 = (U64)q.QuadPart;
    gTsc0 = __rdtsc();
}
} // anonymous namespace

namespace
{
struct CmpAgg { U64 mN = 0, mDiff = 0, mDiffBytes = 0, mBytes = 0; U64 mDiffClass[5] = {}; };
std::map<std::string, CmpAgg> gCmp;
}

void announceCompare(const char* program, U32 cls, bool same, U32 bytes)
{
    CmpAgg& c = gCmp[program];
    ++c.mN;
    c.mBytes += bytes;
    if (!same)
    {
        ++c.mDiff;
        c.mDiffBytes += bytes;
        if (cls < 5) ++c.mDiffClass[cls];
    }
}

void closeReport()
{
    if (gCmp.empty())
    {
        return;
    }
    U64 n = 0, diff = 0, bytes = 0, diff_bytes = 0;
    std::vector<std::pair<U64, std::string>> rows;
    for (const auto& kv : gCmp)
    {
        n += kv.second.mN; diff += kv.second.mDiff; bytes += kv.second.mBytes; diff_bytes += kv.second.mDiffBytes;
        if (kv.second.mDiff) rows.push_back({ kv.second.mDiffBytes, kv.first });
    }
    std::sort(rows.rbegin(), rows.rend());
    LL_INFOS("VkPassClose") << "announced class blocks: " << n << " (" << bytes << " bytes), of them different from what the worker held: " << diff << " (" << diff_bytes << " bytes); programs with differences: " << rows.size() << LL_ENDL;
    for (size_t i = 0; i < rows.size() && i < 25; ++i)
    {
        const CmpAgg& c = gCmp[rows[i].second];
        LL_INFOS("VkPassClose") << "  " << rows[i].second << ": " << c.mDiff << " of " << c.mN << " differ (" << c.mDiffBytes << " bytes), by class " << c.mDiffClass[0] << "/" << c.mDiffClass[1] << "/" << c.mDiffClass[2] << "/" << c.mDiffClass[3] << "/" << c.mDiffClass[4] << LL_ENDL;
    }
    gCmp.clear();
}

void init()
{
    {
        const char* c = getenv("LL_VK_PASSCLOSE");
        sClose = c && (c[0] == '2' || c[0] == '3') ? c[0] - '0' : 1;
        const char* pm = getenv("LL_VK_PASSPOISON");
        if (pm && pm[0]) sPoisonMask = atoi(pm);
        if (sClose > 1)
        {
            LL_INFOS("VkPassClose") << "LL_VK_PASSCLOSE=" << sClose << ": passes are closed" << (sClose == 2 ? " (announced blocks are compared with the worker's)" : sClose == 3 ? " (the worker forgets the inherited state at every pass, mask " : "") << (sClose == 3 ? std::to_string(sPoisonMask) + ")" : std::string()) << LL_ENDL;
        }
    }
    const char* v = getenv("LL_VK_PASSCENSUS");
    sOn = v && v[0] >= '1' && v[0] <= '3';
    sMode = sOn ? v[0] - '0' : 1;
    if (sOn)
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        gQpc0 = (U64)q.QuadPart;
        gTsc0 = __rdtsc();
        LL_INFOS("VkPassCensus") << "LL_VK_PASSCENSUS=" << sMode << ": pass state census on (report every 600 presented frames; 1 target binds, 2 Vulkan render passes, 3 GPU zone phases)" << LL_ENDL;
    }
}

U32 internSite(const char* file, int line, char kind)
{
    const int k = kind == 'r' ? 1 : kind == 'f' ? 2 : 0;
    std::lock_guard<std::mutex> lock(gSiteMutex);
    auto it = gSiteIds[k].find({ file, line });
    if (it != gSiteIds[k].end())
    {
        return it->second;
    }
    const char* base = file ? strrchr(file, '\\') : nullptr;
    const char* base2 = file ? strrchr(file, '/') : nullptr;
    if (base2 > base) base = base2;
    base = base ? base + 1 : (file ? file : "?");
    char buf[96];
    const char* tag = kind == 'r' ? "restore of " : kind == 'f' ? "screen after " : "";
    snprintf(buf, sizeof(buf), "%s%s:%d", tag, base, line);
    gSites.push_back(buf);
    gSiteIds[k][{ file, line }] = (U32)gSites.size();
    return (U32)gSites.size();
}

std::string siteName(U32 id)
{
    std::lock_guard<std::mutex> lock(gSiteMutex);
    if (id == 0 || id > gSites.size()) return std::string(id ? "?" : "-");
    return gSites[id - 1];
}

void frameBegin()
{
    ++sFrame;
    gZones.clear();
    beginPass(-2, 0);
}

void present()
{
    beginPass(-4, 0);
    if (++gPresents >= 600)
    {
        endPass();
        report();
    }
}

void passBegin(S32 fbo, U32 site)
{
    if (sMode != 1)
    {
        return;         // the passes are made by renderPassBegin / zone
    }
    beginPass(fbo, site);
}

void renderPassBegin(const char* label)
{
    if (sMode != 2)
    {
        return;
    }
    beginPass(-5, 0);
    gLabel = label ? label : "render pass";
}

void zone(const char* name, bool begin)
{
    if (sMode != 3)
    {
        return;
    }
    if (begin) gZones.push_back(name ? name : "?");
    else if (!gZones.empty()) gZones.pop_back();
    beginPass(-6, 0);
    std::string path;
    const size_t from = gZones.size() > 3 ? gZones.size() - 3 : 0;
    for (size_t i = from; i < gZones.size(); ++i)
    {
        if (!path.empty()) path += "/";
        path += gZones[i];
    }
    gLabel = path.empty() ? std::string("(outside any zone)") : path;
}

void opDone(U16 op, U32 packet_kind, U64 ticks)
{
    ++gCur.mRecords;
    if (op == LLVkAsync::CMD_DRAW || op == LLVkAsync::CMD_DRAW_SCRATCH)
    {
        ++gCur.mDraws;
        gCur.mTicks[0] += ticks;
    }
    else if (op == LLVkAsync::CMD_SHADOW_PACKETS)
    {
        ++gCur.mPackets;
        if (packet_kind < NKIND) ++gCur.mKinds[packet_kind];
        gCur.mTicks[1] += ticks;
    }
    else
    {
        gCur.mTicks[2] += ticks;
    }
}

void stateReceived() { stampNow(gState); }

void stateRead()
{
    if (gStateSeen) return;
    gStateSeen = true;
    ++gCur.mState[ageOf(gState)];
}

void programAnnounced() { stampNow(gProgSet); }

static void noteProgram(const void* program, const char* name)
{
    for (const void* p : gProgs) if (p == program) return;
    gProgs.push_back(program);
    if (gFirstProg.empty() && name) gFirstProg = name;
}

void packetProgram(const void* program, const char* name)
{
    if (!gPktProgSeen)
    {
        gPktProgSeen = true;
        ++gCur.mPktProg[ageOf(gProgSet)];
    }
    noteProgram(program, name);
}

void drawProgram(const void* program, const char* name)
{
    noteProgram(program, name);
    ++gCur.mProgDraws[name ? name : "(none)"];
}

void binding(bool carried)
{
    if (gBindSeen) return;
    gBindSeen = true;
    if (carried) ++gCur.mBindCarried; else ++gCur.mBindInherited;
}

void unitSet(U32 unit) { if (unit < 32) stampNow(gUnit[unit]); }

void unitRead(U32 unit)
{
    if (unit >= 32 || gUnitReadPass[unit] == sPass) return;
    gUnitReadPass[unit] = sPass;
    ++gCur.mUnit[ageOf(gUnit[unit])];
}

void uboSet(U32 index) { if (index < 4) stampNow(gUbo[index]); }

void uboRead(U32 index)
{
    if (index >= 4 || gUboReadPass[index] == sPass) return;
    gUboReadPass[index] = sPass;
    ++gCur.mUbo[ageOf(gUbo[index])];
}

void uniformWrite(ProgState& ps, U32 cls, U32 bytes)
{
    if (cls >= 5) return;
    stampNow(ps.mWrite[cls]);
    if (ps.mWriteBytesPass[cls] != sPass)
    {
        ps.mWriteBytesPass[cls] = sPass;
        ps.mWriteBytes[cls] = 0;
    }
    ps.mWriteBytes[cls] += bytes;
    if (ps.mReadPass[cls] == sPass) gCur.mWritten += bytes;
}

void classRead(ProgState& ps, U32 cls, U32 bytes, const char* program_name)
{
    if (cls >= 5 || ps.mReadPass[cls] == sPass) return;
    ps.mReadPass[cls] = sPass;
    const Age a = ageOf(ps.mWrite[cls]);
    {
        ProgAgg& pa = gProgAgg[program_name];
        if (pa.mName.empty()) pa.mName = program_name;
        ++pa.mReads[cls];
        ++pa.mAge[a];
        pa.mBytes += bytes;
        if (pa.mLastPass != sPass) { pa.mLastPass = sPass; ++pa.mPasses; }
    }
    ++gCur.mCls[cls][a];
    gCur.mClsBytes[a] += bytes;
    if (ps.mWriteBytesPass[cls] == sPass) gCur.mWritten += std::min(ps.mWriteBytes[cls], bytes);
}

void prepareSeen() { gPrepareFrame = sFrame; }

void geoDrawSeen()
{
    ++gGeoReads;
    if (gPrepareFrame != sFrame) ++gGeoStale;
}

} // namespace LLVkPassCensus

#endif // LL_VULKAN
