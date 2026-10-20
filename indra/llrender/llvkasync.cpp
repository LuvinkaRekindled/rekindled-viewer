/**
 * @file llvkasync.cpp
 * @brief The command queue and worker thread that run the Vulkan mirror off the viewer's main thread (see llvkasync.h).
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkasync.h"
#include "llrenderpass.h"
#include "llglheaders.h"
#include "llvkgl.h"
#include "llvkmirror.h"
#include "llvkzones.h"
#include "llvkrenderer.h"
#include "llvkstate.h"
#include "llvktex.h"
#include "llvkbuf.h"
#include "llvktarget.h"
#include "llvkquery.h"
#include "llframetimer.h"
#include "llvkformat.h"

#include <windows.h>
#include <synchapi.h>
#include <dbghelp.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <set>
#include <algorithm>
#include <intrin.h>
#include <vector>

#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "dbghelp.lib")

namespace LLVkGL
{
    void set_worker_context(int id);
    void register_main_context();
    void on_bufferData(U32 name, unsigned kind, S64 size, const void* data);
    void on_bufferSubData(U32 name, S64 offset, S64 size, const void* data);
    void on_bufferRecycle(U32 name);
}

namespace LLVkMirror
{
    void beginFrameImpl(S32 width, S32 height);
    void endFrameImpl();
    void presentFrameImpl();
    void holdFrameImpl(bool hold);
    void snapshotBoundaryImpl(S32 mode);
}

namespace LLVkAsync
{

bool gEnabled = false;
bool gTexOneCopy = false;   // C4: opt-in (LL_VK_TEXONECOPY=1), the measured gain was nil
static int gMode = 1;
static int uniformRecFromEnv() { const char* e = getenv("LL_VK_UNIFORMREC"); return e ? std::min(2, std::max(0, atoi(e))) : 2; }
int gUniformRec = uniformRecFromEnv();  // F2.5, see llvkasync.h

namespace
{

struct RecHeader
{
    U32 mSize;          // header + payload, rounded up to 8
    U16 mOp;
    U8 mCtx;            // which thread's GL context state the call belongs to
    U8 mFlags;          // bit 0: the data follows as a heap pointer instead of inline
};
static_assert(sizeof(RecHeader) == 8, "record header");

constexpr size_t RING_BYTES = 64u << 20;
constexpr size_t MAX_INLINE = 1u << 20;

U8* gRing = nullptr;
std::atomic<U64> gHead{ 0 }, gTail{ 0 };
unsigned long gMainTid = 0;
unsigned long gWorkerTid = 0;
std::atomic<bool> gRunning{ false };
HANDLE gThread = nullptr;
std::atomic<int> gSleeping{ 0 };
alignas(64) long gWakeWord = 0;
std::atomic<U32> gBeginsEnqueued{ 0 }, gBeginsDone{ 0 };

struct SideRec
{
    U16 mOp = 0;
    U8 mCtx = 0;
    U64 mAfter = 0;                 // the main thread's queue position when this was queued: replayed only once the worker has got that far
    std::vector<U8> mPayload;
};
// LL_VK_OPSTATS: what the main thread spends writing records (one write in 16 is timed, the total scaled up, so the timer does not distort it)
unsigned long long gMainPushOps = 0;
unsigned long long gMainPushTicks = 0;
std::mutex gSideMutex;
std::deque<SideRec> gSide;
std::atomic<int> gSideCount{ 0 };
std::atomic<int> gNextCtx{ 1 };

inline bool isMainThread() { return __readgsdword(0x48) == gMainTid; }

// F3: the records of a thread inside a sub-stream go to this buffer instead of the queue
struct SubBuf { std::vector<U8> mBuf; size_t mCur = 0; };
thread_local SubBuf* tlsBuf = nullptr;
// does this thread write records: the main producer, or a thread inside a sub-stream
inline bool produces() { return tlsBuf != nullptr || isMainThread(); }

int producerCtx()
{
    if (isMainThread())
    {
        return 0;
    }
    static thread_local int ctx = -1;
    if (ctx < 0)
    {
        ctx = std::min(gNextCtx.fetch_add(1), 255);
    }
    return ctx;
}

inline void wake()
{
    InterlockedExchange(&gWakeWord, 1);
    WakeByAddressSingle(&gWakeWord);
}

void drain_inline();
void wait_drained();

inline bool workAvailable()
{
    return gHead.load(std::memory_order_relaxed) != gTail.load(std::memory_order_acquire) || gSideCount.load(std::memory_order_relaxed) > 0;
}

// argument layouts of the uniform commands: one definition for both sides so the padding can never differ
struct UniformVecArgs { void* mProgram; unsigned mIndex, mComponents, mCount; };
struct UniformMatArgs { void* mProgram; unsigned mIndex, mColumns, mRows, mCount; };

// ---- main-side shadows of the state the encoders need ----
struct PixelStoreShadow
{
    int mUnpackAlignment = 4;
    int mUnpackRowLength = 0;
    unsigned mUnpackBuffer = 0;
};
thread_local PixelStoreShadow tlsPixelStore;
thread_local std::vector<std::pair<unsigned, unsigned>> tlsActiveQueries;     // target -> query name in progress

// ---- the occlusion query results the worker publishes ----
constexpr unsigned QUERY_TABLE = 1u << 20;
struct PubQuery
{
    std::atomic<unsigned long long> mIssued{ 0 };   // the latest generation begun by the main thread
    std::atomic<unsigned long long> mReady{ 0 };    // the newest generation whose result has been published
    std::atomic<unsigned long long> mValue{ 1 };    // the result of mReady (1: visible)
    std::atomic<unsigned> mSync{ 0 };               // 1: a timer query; its results are read through the worker
};
PubQuery* gPub = nullptr;
unsigned long long* gWorkerGen = nullptr;           // worker side: the generation of the latest Begin it has replayed, by name
std::atomic<unsigned long long> gQStat[7];
std::atomic<unsigned long long> gQValue[2];

// ---- frame pacing statistics (LL_VK_PHASES) ----
std::mutex gPaceMutex;
std::vector<float> gBeginGaps, gPresentGaps;        // milliseconds between consecutive frame starts (main) / presents (worker)
long long gLastBeginTick = 0, gLastPresentTick = 0;
long long gMainWaitTicks = 0, gWorkerIdleTicks = 0;

// LL_VK_PHASES: the worker's time per operation, in processor time-stamp ticks (a cheap read; converted with a ratio measured against the performance counter)
constexpr int OP_STAT_SLOTS = 56;
bool gOpStats = false;
bool gOpStatsPeriodic = false;      // LL_VK_OPSTATS without LL_VK_PHASES: the worker logs the numbers itself every 600 presented frames
unsigned gOpStatsFrames = 0;
unsigned long long gOpStatTicks[OP_STAT_SLOTS] = {};
unsigned long long gOpStatCalls[OP_STAT_SLOTS] = {};
unsigned long long gOpStatTsc0 = 0;
unsigned long long gFrontCalls[FP_COUNT] = {};
unsigned long long gFrontTicks[FP_COUNT] = {};
const char* const kFrontNames[FP_COUNT] = { "LLGLSLShader::bind", "uniform*", "uniformMatrix*", "uniform writes sent (count only)", "LLTexUnit::bind", "blendFunc", "state guards", "setBuffer", "draw*", "syncMatrices (contains uniformMatrix)", "LLRender::flush (contains draw*)", "matrix push/pop (contains flush)", "matrix set/mult/identity/ortho (contains flush)", "matrix translate/rotate/scale (contains flush)", "matrixMode", "LLTexUnit::unbind/unbindFast (contains flush)", "LLTexUnit::activate/enable/disable", "LLTexUnit::bindManual (contains flush)", "LLTexUnit::setTextureAddressMode/FilteringOption" };
long long gOpStatQpc0 = 0;
inline int opStatSlot(unsigned op) { return std::min((int)(op - CMD_BASE), OP_STAT_SLOTS - 1); }
const char* const kCmdNames[] = { "base", "frameBegin", "frameEnd", "present", "hold", "snapshotBoundary", "upscale", "programBind", "uniformScalar",
                                  "uniformVector", "uniformMatrix", "samplerUnit", "programDestroy", "sync", "stop", "wrap", "bufferData", "bufferSubData", "shadowPackets", "texParams", "fboAttach", "targetState", "texSlots", "gpuZone", "uniformBatch", "bufferRecycle", "rtScene", "state", "texImage", "texParamsId", "texOp", "uniformBlock", "constAttrib", "clear", "queryBegin", "queryEnd", "resourceCreate", "resourceDestroy", "draw", "uniformRange", "pass", "drawScratch" };
const char* slotName(int slot) { return slot >= 0 && slot < (int)(sizeof(kCmdNames) / sizeof(kCmdNames[0])) ? kCmdNames[slot] : "cmd?"; }
unsigned long long gMainOpCalls[OP_STAT_SLOTS] = {};     // LL_VK_OPSTATS: the plain calls the main thread pushed (the ones not yet typed), by operation

// C4: texture upload costs on the main thread, and what was handed over without a copy
std::atomic<unsigned long long> gTexMainCalls{ 0 }, gTexMainTicks{ 0 }, gTexHandedCalls{ 0 }, gTexHandedBytes{ 0 }, gTexCopiedBytes{ 0 }, gHoldWaits{ 0 }, gHoldWaitTicks{ 0 };
std::atomic<unsigned long long> gTexWorkerCalls{ 0 }, gTexWorkerConvertUs{ 0 }, gTexWorkerFillUs{ 0 }, gTexWorkerTotalUs{ 0 };
thread_local PixelHold tlsHoldData = {};
thread_local bool tlsHoldSet = false;

double ticks_to_ms(long long t) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)t * 1000.0 / (double)f.QuadPart; }
long long now_ticks() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
void note_gap(std::vector<float>& gaps, long long& last)
{
    const long long n = now_ticks();
    if (last)
    {
        std::lock_guard<std::mutex> lock(gPaceMutex);
        gaps.push_back((float)ticks_to_ms(n - last));
        if (gaps.size() > 4000) gaps.erase(gaps.begin(), gaps.begin() + 2000);
    }
    last = n;
}
std::string describe(std::vector<float>& v)
{
    if (v.empty()) return "no frames";
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (float f : v) sum += f;
    char buf[200];
    snprintf(buf, sizeof(buf), "n %zu, avg %.1f, p50 %.1f, p95 %.1f, p99 %.1f, max %.1f", v.size(), sum / v.size(), v[v.size() / 2], v[(size_t)(v.size() * 0.95)], v[(size_t)(v.size() * 0.99)], v.back());
    return buf;
}

// ---- writing records ----
inline size_t alignUp(size_t n) { return (n + 7) & ~(size_t)7; }

U8* mainReserve(size_t payload_bytes, U16 op, U8 flags)
{
    const size_t total = alignUp(sizeof(RecHeader) + payload_bytes);
    if (tlsBuf)
    {
        SubBuf& b = *tlsBuf;
        b.mCur = b.mBuf.size();
        b.mBuf.resize(b.mCur + total);
        RecHeader sh = { (U32)total, op, 0, flags };
        memcpy(b.mBuf.data() + b.mCur, &sh, sizeof(sh));
        return b.mBuf.data() + b.mCur + sizeof(RecHeader);
    }
    U64 tail = gTail.load(std::memory_order_relaxed);
    for (;;)
    {
        const U64 head = gHead.load(std::memory_order_acquire);
        const size_t pos = (size_t)(tail % RING_BYTES);
        const size_t contiguous = RING_BYTES - pos;
        const U64 need = contiguous < total ? contiguous + total : total;
        if (tail + need - head <= RING_BYTES)
        {
            break;
        }
        // the worker is behind by a whole ring: wake it and wait for space
        wake();
        SwitchToThread();
    }
    size_t pos = (size_t)(tail % RING_BYTES);
    const size_t contiguous = RING_BYTES - pos;
    if (contiguous < total)
    {
        RecHeader pad = { (U32)contiguous, (U16)CMD_WRAP, 0, 0 };
        memcpy(gRing + pos, &pad, sizeof(pad));
        tail += contiguous;
        gTail.store(tail, std::memory_order_release);
        pos = 0;
    }
    RecHeader h = { (U32)total, op, 0, flags };
    memcpy(gRing + pos, &h, sizeof(h));
    return gRing + pos + sizeof(RecHeader);
}

inline void mainCommit(size_t payload_bytes)
{
    if (tlsBuf)
    {
        return;             // the record is in the sub-stream buffer already
    }
    const size_t total = alignUp(sizeof(RecHeader) + payload_bytes);
    gTail.store(gTail.load(std::memory_order_relaxed) + total, std::memory_order_release);
    if (gMode == 2)
    {
        drain_inline();
        return;
    }
    if (gSleeping.load(std::memory_order_relaxed))
    {
        wake();
    }
}

// ---- merging records on the main thread (LL_VK_COALESCE=0 turns it off) ----
// A draw is preceded by runs of glActiveTexture + glBindTexture and of glBindBuffer(ARRAY) + glVertexAttribPointer*. They are held back
// and written as one record each: BIND_TEXTURE_UNIT (the unit and the bind) and ARRAY_RUN (the buffer bind and its attribute pointers).
// Nothing is dropped and nothing is reordered: whatever else is queued first writes out what is held, and the worker plays a merged record
// by calling the same functions in the same order.
bool gTyped = true;                           // LL_VK_TYPED=0: typed records (LLVertexBuffer::setBuffer) off, the separate calls are queued as before
unsigned long long gMainRecords = 0;        // records actually written by the main thread (a sub-stream's are counted when it is spliced)
inline void noteRecord() { if (!tlsBuf) { ++gMainRecords; } }


inline void rawPush(U16 op, const void* args, unsigned size)
{
    U8* p = mainReserve(size, op, 0);
    if (size)
    {
        memcpy(p, args, size);
    }
    mainCommit(size);
    noteRecord();
}

// <FS> draw-record plan step 1: texture binds of LLTexUnit do not write a record each. The producer keeps the slot array (unit -> texture id); what changed since the last
// draw is written with that draw (CMD_DRAW) or, when something that can read texture state comes first, as CMD_TEX_SLOTS.
inline U32 lowBit(U32 m) { unsigned long i; _BitScanForward(&i, m); return (U32)i; }
constexpr U32 TEX_UNIT_BASE = 0x84C0;       // GL_TEXTURE0
// the pending texture slots, active unit and uniform writes of one producer (the main thread's, or a sub-stream's)
// F2.4: what the next draw uses besides state, textures and uniforms: the program and the vertex binding, as values (written by LLGLSLShader::bind/unbind and LLVertexBuffer::setBuffer)
struct DrawCtx
{
    void* mProgram = nullptr;
    void* mSent = (void*)(intptr_t)-1;    // the program the worker's renderer holds as far as the stream says (a draw record names its program; packet records read the renderer's: see pushData); -1: unknown
    DrawBinding mBinding;
    bool mDirty = true;                   // the binding changed since the last draw record that carried it (a draw record only carries a binding when it is new)
};
struct Pend
{
    U32 mTex[32] = {};                    // the texture each unit holds as the producer sees it (the ids the draw records carry)
    U32 mMask = 0;                        // the units whose texture changed since the last record
    std::vector<U8> mUni;
    DrawCtx mDraw;
};
Pend gMainPend;
thread_local Pend* tlsPend = nullptr;
inline Pend& pend() { return tlsPend ? *tlsPend : gMainPend; }

// the entries of the pending slots in unit order, appended to p; returns the bytes
U32 pendTexWrite(U8* p)
{
    U32 n = 0;
    for (U32 m = pend().mMask; m; m &= m - 1)
    {
        const U32 u = lowBit(m);
        memcpy(p + n * sizeof(U32), &pend().mTex[u], sizeof(U32));
        ++n;
    }
    return n * (U32)sizeof(U32);
}

unsigned long long gTexFlushWhy[OP_STAT_SLOTS] = {};     // LL_VK_OPSTATS: what forced the pending texture slots out as a record of their own (slot of the operation)

void flushPendingTex(int why = 0)
{
    if (!pend().mMask)
    {
        return;
    }
    if (!tlsBuf)
    {
        ++gTexFlushWhy[why];
    }
    const U32 count = (U32)__popcnt(pend().mMask);
    const U32 payload = (U32)(sizeof(U32) * 2 + sizeof(U32) * count);
    U8* p = mainReserve(payload, CMD_TEX_SLOTS, 0);
    const U32 head[2] = { pend().mMask, count };
    memcpy(p, head, sizeof(head));
    pendTexWrite(p + sizeof(head));
    mainCommit(payload);
    noteRecord();
    pend().mMask = 0;
}

// <FS> draw-record plan: loose uniform writes (scalar, vector, matrix) of the main thread are not records of their own either. They pile up here as
// {op, payload bytes, payload} entries (the argument struct and the values, padded to 8) and are written inside the next draw record. Anything that can
// need them earlier flushes them first (every operation that is not independent of them goes through coFlushAll).
struct UniformScalarArgs { void* mProgram; unsigned mIndex; int mKind; unsigned mN; float mF[4]; int mI[4]; };

void flushPendingUni(int why = 0)
{
    (void)why;
    if (pend().mUni.empty())
    {
        return;
    }
    const U32 bytes = (U32)pend().mUni.size();
    const U32 payload = (U32)(sizeof(U32) * 2 + bytes);
    U8* p = mainReserve(payload, CMD_UNIFORM_BATCH, 0);
    const U32 head[2] = { bytes, 0 };
    memcpy(p, head, sizeof(head));
    memcpy(p + sizeof(head), pend().mUni.data(), bytes);
    mainCommit(payload);
    noteRecord();
    pend().mUni.clear();
}

inline void uniAppend(U32 op, const void* args, size_t asz, const void* data, size_t dsz)
{
    const size_t payload = (asz + dsz + 7) & ~(size_t)7;
    const size_t off = pend().mUni.size();
    pend().mUni.resize(off + 8 + payload);
    const U32 head[2] = { op, (U32)payload };
    memcpy(&pend().mUni[off], head, 8);
    memcpy(&pend().mUni[off + 8], args, asz);
    if (dsz)
    {
        memcpy(&pend().mUni[off + 8 + asz], data, dsz);
    }
    if (pend().mUni.size() > (1u << 20))
    {
        flushPendingUni();      // bounded: a long run of uniform writes with no draw in between
    }
}

inline void coFlushAll(int why = 0)
{
    flushPendingTex(why);
    flushPendingUni(why);
}

// operations that cannot read or change texture state: they do not force the pending slots out
inline bool texIndependent(U16 op)
{
    switch (op)
    {
    case CMD_TEX_IMAGE: case CMD_TEX_PARAMS_ID: case CMD_TEX_OP: case CMD_UNIFORM_BLOCK:
    case CMD_CONST_ATTRIB: case CMD_CLEAR: case CMD_BUFFER_DATA: case CMD_BUFFER_SUBDATA: case CMD_BUFFER_RECYCLE: case CMD_GPU_ZONE:
    case CMD_QUERY_BEGIN: case CMD_QUERY_END:
    case CMD_UNIFORM_SCALAR: case CMD_UNIFORM_VECTOR: case CMD_UNIFORM_MATRIX: case CMD_SAMPLER_UNIT: case CMD_PROGRAM_BIND:
        return true;
    default:
        return false;
    }
}

void sidePush(U16 op, std::vector<U8>&& payload)
{
    SideRec r;
    r.mOp = op;
    r.mCtx = (U8)producerCtx();
    r.mAfter = gTail.load(std::memory_order_acquire);
    r.mPayload = std::move(payload);
    {
        std::lock_guard<std::mutex> lock(gSideMutex);
        gSide.push_back(std::move(r));
        gSideCount.fetch_add(1);
    }
    wake();
}

} // anonymous namespace

// <FS> F1.1: a state block record is written before anything that reads draw state (draws, clears, shadow packets, synchronous answers) when the producer's
// context (llvkstate: written directly by the viewer's state setters) changed since the last one.
// main thread only
inline void stateEmit()
{
    LLVkState::Block b;
    if (LLVkState::takeChanged(b))
    {
        rawPush(CMD_STATE, &b, sizeof(b));
    }
}

void push(unsigned short op, const void* args, unsigned size)
{
    if (produces())
    {
        const bool stats = gOpStats && !tlsBuf;
        const bool timed = stats && ((++gMainPushOps & 15) == 0);
        const unsigned long long t0 = timed ? __rdtsc() : 0;
        if (stats)
        {
            ++gMainOpCalls[opStatSlot(op)];
        }
        if (!texIndependent(op))
        {
            coFlushAll(opStatSlot(op));     // what can read texture state or uniforms writes the pending slots and uniform writes out first
        }
        rawPush(op, args, size);
        if (timed)
        {
            gMainPushTicks += (__rdtsc() - t0) * 16;
        }
        return;
    }
    std::vector<U8> payload((const U8*)args, (const U8*)args + size);
    sidePush(op, std::move(payload));
}

// the typed records are timed like the pushed calls (every 16th one), so the 'main thread writing the queue' figure covers them
struct MainLap
{
    const bool mTimed;
    const unsigned long long mT0;
    MainLap() : mTimed(gOpStats && !tlsBuf && ((++gMainPushOps & 15) == 0)), mT0(mTimed ? __rdtsc() : 0) {}
    ~MainLap() { if (mTimed) { gMainPushTicks += (__rdtsc() - mT0) * 16; } }
};

bool gFrontStats = false;
bool mainOnlyProducer() { return gEnabled && tlsBuf == nullptr && isMainThread(); }
void uniformRange(void* program, U32 cls, U32 offset, U32 size, const void* data)
{
    MainLap lap;
    struct A { void* mProgram; U32 mClass; U32 mOffset; U32 mSize; U32 mPad; } a = { program, cls, offset, size, 0 };
    uniAppend(CMD_UNIFORM_RANGE, &a, sizeof(a), data, size);
}
static unsigned long long gDrawSeq = 0;
unsigned long long drawSeq() { return gDrawSeq; }
unsigned long long frontStart(int id)
{
    if (!isMainThread())
    {
        return 0;
    }
    if (id == FP_DRAW)
    {
        ++gDrawSeq;
    }
    const unsigned long long n = ++gFrontCalls[id];
    return (n & 15) == 0 ? (__rdtsc() | 1) : 0;
}
void frontStop(int id, unsigned long long t0) { gFrontTicks[id] += (__rdtsc() - t0) * 16; }
void frontNote(int id) { if (isMainThread()) { ++gFrontCalls[id]; if (id == FP_DRAW) { ++gDrawSeq; } } }

bool mainProducer()
{
    return gEnabled && gTyped && produces();
}

void framebufferAttach(U32 fbo, U32 slot, U32 texture)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::target_attach(fbo, slot, texture);
        return;
    }
    if (!produces())
    {
        return;
    }
    MainLap lap;
    coFlushAll(opStatSlot(CMD_FBO_ATTACH));
    const U32 a[3] = { fbo, slot, texture };
    rawPush(CMD_FBO_ATTACH, a, sizeof(a));
}

void targetState(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh)
{
    // the viewport is part of the render state context; the record carries the target and its colour attachment count
    LLVkState::setViewport(vx, vy, vw, vh);
    targetDrawBuffers(fbo, draw_buffers);
}

void targetDrawBuffers(U32 fbo, S32 draw_buffers)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::target_state(fbo, draw_buffers);
        return;
    }
    if (!produces())
    {
        return;
    }
    MainLap lap;
    coFlushAll(opStatSlot(CMD_TARGET_STATE));
    const S32 a[2] = { (S32)fbo, draw_buffers };
    rawPush(CMD_TARGET_STATE, a, sizeof(a));
}

void passBegin(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh, bool clear_color, bool clear_depth, const F32* rgba)
{
    LLVkState::setViewport(vx, vy, vw, vh);
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::target_state(fbo, draw_buffers);
        if (clear_color || clear_depth)
        {
            LLVkGL::clear_target(clear_color, clear_depth, rgba);
        }
        return;
    }
    if (!produces())
    {
        return;
    }
    MainLap lap;
    coFlushAll(opStatSlot(CMD_PASS));
    if (clear_color || clear_depth)
    {
        stateEmit();        // the clear honours the colour / depth write masks and the scissor: the state record comes first, as before a clear
    }
    struct R { S32 mFbo; S32 mDrawBuffers; U32 mFlags; F32 mColor[4]; } rec = { (S32)fbo, draw_buffers, (clear_color ? 1u : 0u) | (clear_depth ? 2u : 0u), { rgba[0], rgba[1], rgba[2], rgba[3] } };
    rawPush(CMD_PASS, &rec, sizeof(rec));
}

void clearTarget(bool color, bool depth, const F32* rgba)
{
    if (!produces())
    {
        return;
    }
    MainLap lap;
    stateEmit();        // the clear honours the colour / depth write masks and the scissor
    struct R { U32 mFlags; F32 mColor[4]; } rec = { (color ? 1u : 0u) | (depth ? 2u : 0u), { rgba[0], rgba[1], rgba[2], rgba[3] } };
    rawPush(CMD_CLEAR, &rec, sizeof(rec));
}

namespace
{
// the draw context of the calling producer (null when the draw record is off, or on a thread that does not produce)
inline DrawCtx* drawCtx()
{
    return (!gEnabled || produces()) ? &pend().mDraw : nullptr;
}
}

void drawProgram(void* program)
{
    if (DrawCtx* d = drawCtx())
    {
        d->mProgram = program;
    }
}

void drawBindingClear()
{
    if (DrawCtx* d = drawCtx())
    {
        d->mBinding = DrawBinding();
        d->mDirty = true;
    }
}

void drawBinding(U32 array_buffer, U32 elem_buffer, const VertexAttrib* given, U32 count)
{
    DrawCtx* d = drawCtx();
    if (!d)
    {
        return;
    }
    DrawBinding& b = d->mBinding;
    b = DrawBinding();
    b.mArray = array_buffer;
    b.mElem = elem_buffer;
    for (U32 i = 0; i < count; ++i)
    {
        const VertexAttrib& e = given[i];
        if (e.mIndex >= 16)
        {
            continue;
        }
        BindAttr& a = b.mAttr[e.mIndex];
        a.mSize = e.mSize;
        a.mStride = e.mStride;
        a.mOffset = (U64)e.mPointer;
        a.mType = (U8)e.mType;
        a.mNormalized = (e.mKind == 0 && e.mNormalized != 0) ? 1 : 0;
        a.mInteger = e.mKind == 1 ? 1 : 0;
        b.mPresent |= 1u << e.mIndex;
    }
    d->mDirty = true;
}

void drawBindingSet(const DrawBinding& binding)
{
    if (DrawCtx* d = drawCtx())
    {
        d->mBinding = binding;
        d->mDirty = true;
    }
}

void drawBindingGet(DrawBinding& out)
{
    if (DrawCtx* d = drawCtx())
    {
        out = d->mBinding;
    }
}

// LLGLSLShader::bind: the program of the next draws; a program bind leaves no buffers bound
void shaderBind(void* program)
{
    if (DrawCtx* d = drawCtx())
    {
        if (program)
        {
            d->mProgram = program;
        }
        d->mBinding = DrawBinding();
        d->mDirty = true;
    }
}

void bindTextureUnit(U32 unit, U32 texture)
{
    MainLap lap;
    if (unit >= 32)
    {
        return;
    }
    // the slot array is the record: nothing is written here, what changed goes out with the next draw (or when something that can read texture state comes first)
    pend().mTex[unit] = texture;
    pend().mMask |= 1u << unit;
}

namespace
{
    struct NamedBufArgs { U32 mName; U32 mTarget; U32 mUsage; U32 mPad; S64 mOffset; S64 mSize; };
}

void bufferData(U32 name, BufferKind buffer_kind, S64 size, const void* data)
{
    const U32 kind = (U32)buffer_kind;
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::on_bufferData(name, kind, (S64)size, data);
        return;
    }
    MainLap lap;
    const NamedBufArgs a = { name, kind, 0, 0, 0, size };
    pushData(CMD_BUFFER_DATA, &a, sizeof(a), data, data ? (size_t)std::max<S64>(size, 0) : 0);
}

void bufferSubData(U32 name, S64 offset, S64 size, const void* data)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::on_bufferSubData(name, (S64)offset, (S64)size, data);
        return;
    }
    MainLap lap;
    const NamedBufArgs a = { name, 0, 0, 0, offset, size };
    pushData(CMD_BUFFER_SUBDATA, &a, sizeof(a), data, data ? (size_t)std::max<S64>(size, 0) : 0);
}

void bufferRecycle(U32 name)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::on_bufferRecycle(name);
        return;
    }
    MainLap lap;
    const NamedBufArgs a = { name, 0, 0, 0, 0, 0 };
    pushData(CMD_BUFFER_RECYCLE, &a, sizeof(a), nullptr, 0);
}

// the draw record: the draw, the program, the vertex binding when it changed, the texture ids that changed since the last one and the uniform writes
namespace
{
    struct DrawHeader { U32 mKind; U32 mMode; U32 mStart; U32 mEnd; S32 mCount; U32 mType; const void* mIndices; U32 mMask; U32 mCountTex; U32 mUniBytes; };
    // CMD_DRAW: the draw, then (mBindBytes) the vertex binding when it changed, the texture ids, the uniform writes
    struct DrawHeader2 { DrawHeader mDraw; void* mProgram; U32 mBindBytes; U32 mPad; };
}

// the draw and the texture slots / uniform writes that changed since the last one, as one record
// scratch (kind 2, F3.1): the vertices follow the uniform writes (extra: three pieces, null when absent); the draw reads no vertex binding, so the producer's binding is neither written nor marked as sent
static void writeDraw(U32 kind, U32 mode, U32 start, U32 end, S32 count, U32 type, const void* indices, const void* const* extra = nullptr, const size_t* extra_bytes = nullptr)
{
    MainLap lap;
    stateEmit();
    {
        // the draw record: the draw plus the program and, when it changed, the vertex binding (nothing the draw reads is left in the worker's state)
        DrawCtx& d = pend().mDraw;
        const U32 n2 = (U32)__popcnt(pend().mMask);
        const U32 uni2 = (U32)pend().mUni.size();
        const bool scratch = kind == 2;
        const U32 bb = (d.mDirty && !scratch) ? (U32)((drawBindingSize(d.mBinding) + 7) & ~(size_t)7) : 0;       // padded: the texture entries and uniform writes after it stay aligned
        const U32 idb = (n2 * (U32)sizeof(U32) + 7) & ~7u;       // F2.6: the texture ids of the units that changed (no target), padded
        size_t vbytes = 0;
        for (int i = 0; scratch && i < 3; ++i)
        {
            vbytes += extra_bytes[i];
        }
        const U32 payload2 = (U32)(sizeof(DrawHeader2) + bb + idb + uni2 + vbytes);
        U8* p2 = mainReserve(payload2, scratch ? CMD_DRAW_SCRATCH : CMD_DRAW, 0);
        DrawHeader2 h2 = { { kind, mode, start, end, count, type, indices, pend().mMask, n2, uni2 }, d.mProgram, bb, 0 };
        memcpy(p2, &h2, sizeof(h2));
        if (bb)
        {
            drawBindingWrite(p2 + sizeof(h2), d.mBinding);
        }
        if (scratch)
        {
            U8* q = p2 + sizeof(h2) + idb + uni2;
            for (int i = 0; i < 3; ++i)
            {
                if (extra_bytes[i])
                {
                    memcpy(q, extra[i], extra_bytes[i]);
                    q += extra_bytes[i];
                }
            }
        }
        {
            U32* ids = (U32*)(p2 + sizeof(h2) + bb);
            U32 k = 0;
            for (U32 m = pend().mMask; m; m &= m - 1)
            {
                ids[k++] = pend().mTex[lowBit(m)];
            }
            if (idb > n2 * (U32)sizeof(U32))
            {
                ids[n2] = 0;
            }
        }
        if (uni2)
        {
            memcpy(p2 + sizeof(h2) + bb + idb, pend().mUni.data(), uni2);
        }
        mainCommit(payload2);
        noteRecord();
        if (!scratch)
        {
            d.mDirty = false;
        }
        d.mSent = d.mProgram;
        pend().mMask = 0;
        pend().mUni.clear();
    }
}

// inline mode (the queue has not started): a draw record's program and binding are applied directly (the gMutex is held)
static void inlineDrawApply()
{
    DrawCtx& d = pend().mDraw;
    LLVkGL::draw_context_apply(d.mProgram, d.mDirty ? &d.mBinding : nullptr);
    d.mDirty = false;
}

void drawRangeElements(PrimitiveMode primitive, U32 start, U32 end, S32 count, ComponentType component, const void* indices)
{
    const U32 mode = (U32)primitive, type = (U32)component;
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts): the draw is made here
        inlineDrawApply();
        LLVkGL::draw_elements(mode, start, end, count, type, indices);
        return;
    }
    if (!produces())
    {
        return;     // draws are the producers'
    }
    writeDraw(0, mode, start, end, count, type, indices);
}

void drawArrays(PrimitiveMode primitive, S32 first, S32 count)
{
    const U32 mode = (U32)primitive;
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        inlineDrawApply();
        LLVkGL::draw_arrays(mode, first, count);
        return;
    }
    if (!produces())
    {
        return;
    }
    writeDraw(1, mode, (U32)first, 0, count, 0, nullptr);
}

void drawScratch(PrimitiveMode primitive, S32 count, U32 mask, const void* positions, const void* texcoords, const void* colors)
{
    const U32 mode = (U32)primitive;
    const size_t n = (size_t)std::max<S32>(count, 0);
    const void* const pieces[3] = { positions, (mask & SCRATCH_TEXCOORD0) ? texcoords : nullptr, (mask & SCRATCH_COLOR) ? colors : nullptr };
    const size_t bytes[3] = { n * 16, (mask & SCRATCH_TEXCOORD0) ? n * 8 : 0, (mask & SCRATCH_COLOR) ? n * 4 : 0 };
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::draw_context_apply(pend().mDraw.mProgram, nullptr);
        std::vector<U8> joined(bytes[0] + bytes[1] + bytes[2]);
        size_t at = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (bytes[i]) { memcpy(joined.data() + at, pieces[i], bytes[i]); at += bytes[i]; }
        }
        LLVkGL::draw_scratch(mode, count, mask, joined.data());
        return;
    }
    if (!produces())
    {
        return;
    }
    // (kind 2: start carries the attribute mask)
    writeDraw(2, mode, mask, 0, count, 0, nullptr, pieces, bytes);
}

void gpuZone(const char* name, bool begin)
{
    char text[64];
    const size_t len = name ? std::min<size_t>(strlen(name), sizeof(text) - 1) : 0;
    if (len) memcpy(text, name, len);
    text[len] = 0;
    const U32 head[2] = { begin ? 1u : 0u, (U32)len };
    pushData(CMD_GPU_ZONE, head, sizeof(head), text, len + 1);
}

void pushData(unsigned short op, const void* args, unsigned size, const void* data, size_t data_size)
{
    if (produces())
    {
        const bool stats = gOpStats && !tlsBuf;
        const bool timed = stats && ((++gMainPushOps & 15) == 0);
        const unsigned long long t0 = timed ? __rdtsc() : 0;
        struct Lap { const bool mTimed; const unsigned long long mT0; ~Lap() { if (mTimed) { gMainPushTicks += (__rdtsc() - mT0) * 16; } } } lap = { timed, t0 };
        if (stats)
        {
            ++gMainOpCalls[opStatSlot(op)];
        }
        if (op == CMD_SHADOW_PACKETS)
        {
            stateEmit();
            {
                // the packet handlers draw with the renderer's current program: it is written here, when the program changed since the last record that set it
                DrawCtx& d = pend().mDraw;
                if (d.mSent != d.mProgram)
                {
                    rawPush(CMD_PROGRAM_BIND, &d.mProgram, sizeof(d.mProgram));
                    d.mSent = d.mProgram;
                }
            }
        }
        if (!texIndependent(op))
        {
            coFlushAll(opStatSlot(op));
        }
        noteRecord();
        const bool heap = data_size > MAX_INLINE;
        const size_t payload = size + sizeof(U64) + (heap ? sizeof(U64) : data_size);
        U8* p = mainReserve(payload, op, heap ? 1 : 0);
        if (size)
        {
            memcpy(p, args, size);
        }
        const U64 ds = data_size;
        memcpy(p + size, &ds, sizeof(ds));
        if (heap)
        {
            void* copy = malloc(data_size);
            memcpy(copy, data, data_size);
            const U64 ptr = (U64)(uintptr_t)copy;
            memcpy(p + size + sizeof(ds), &ptr, sizeof(ptr));
        }
        else if (data_size)
        {
            memcpy(p + size + sizeof(ds), data, data_size);
        }
        mainCommit(payload);
        return;
    }
    std::vector<U8> payload(size + sizeof(U64) + data_size);
    if (size)
    {
        memcpy(payload.data(), args, size);
    }
    const U64 ds = data_size;
    memcpy(payload.data() + size, &ds, sizeof(ds));
    if (data_size)
    {
        memcpy(payload.data() + size + sizeof(ds), data, data_size);
    }
    sidePush(op, std::move(payload));
}

void syncCall(const std::function<void()>& fn)
{
    if (isMainThread())
    {
        stateEmit();        // the answer may read render state (glGet*, glIsEnabled): the worker needs the producer's latest block first
    }
    std::atomic<int> done{ 0 };
    struct Args { const std::function<void()>* mFn; std::atomic<int>* mDone; } a = { &fn, &done };
    // inline when this thread is the one that replays commands (the worker, or the main thread in loopback mode) or nothing is replaying them (stopped)
    const bool replayer = gMode == 2 ? isMainThread() : (!gRunning.load() || __readgsdword(0x48) == gWorkerTid);
    if (replayer)
    {
        fn();
        return;
    }
    push(CMD_SYNC, &a, sizeof(a));
    wake();
    int spins = 0;
    while (done.load(std::memory_order_acquire) == 0)
    {
        if (++spins < 2000)
        {
            YieldProcessor();
        }
        else
        {
            int undesired = 0;
            WaitOnAddress(&done, &undesired, sizeof(int), 1);
        }
    }
}

unsigned long long backlogBytes()
{
    const U64 tail = gTail.load(std::memory_order_acquire);
    const U64 head = gHead.load(std::memory_order_acquire);
    return tail > head ? tail - head : 0;
}

void setPixelHold(const PixelHold* hold)
{
    if (hold && gEnabled && gTexOneCopy && isMainThread())
    {
        tlsHoldData = *hold;
        tlsHoldSet = true;
    }
    else
    {
        tlsHoldSet = false;
    }
}

void waitForPixelHolds()
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    syncCall([]() {});
    QueryPerformanceCounter(&t1);
    gHoldWaits.fetch_add(1, std::memory_order_relaxed);
    gHoldWaitTicks.fetch_add((unsigned long long)(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
}

void noteTexWorker(unsigned long long convert_us, unsigned long long fill_us, unsigned long long total_us, unsigned calls)
{
    gTexWorkerCalls.fetch_add(calls, std::memory_order_relaxed);
    gTexWorkerConvertUs.fetch_add(convert_us, std::memory_order_relaxed);
    gTexWorkerFillUs.fetch_add(fill_us, std::memory_order_relaxed);
    gTexWorkerTotalUs.fetch_add(total_us, std::memory_order_relaxed);
}

std::string texUploadReport(size_t frames)
{
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const double f = (double)std::max<size_t>(1, frames);
    const double ms_per_tick = 1000.0 / (double)qf.QuadPart;
    const unsigned long long calls = gTexMainCalls.exchange(0), ticks = gTexMainTicks.exchange(0), handed = gTexHandedCalls.exchange(0), handed_bytes = gTexHandedBytes.exchange(0),
                             copied = gTexCopiedBytes.exchange(0), waits = gHoldWaits.exchange(0), wait_ticks = gHoldWaitTicks.exchange(0);
    const unsigned long long wcalls = gTexWorkerCalls.exchange(0), wconv = gTexWorkerConvertUs.exchange(0), wfill = gTexWorkerFillUs.exchange(0), wtotal = gTexWorkerTotalUs.exchange(0);
    char buf[600];
    snprintf(buf, sizeof(buf), "one-copy path %s | main thread: %.1f upload calls/frame take %.3f ms/frame; %.1f/frame handed over without a copy (%.0f KB/frame), %.0f KB/frame copied into the queue; "
             "%llu waits for held pixels (%.2f ms in total) | worker: %.1f uploads/frame: convert into a vector %.3f ms/frame, fill staging %.3f ms/frame, the whole upload call %.3f ms/frame",
             gTexOneCopy ? "on" : "off", (double)calls / f, (double)ticks * ms_per_tick / f, (double)handed / f, (double)handed_bytes / f / 1024.0, (double)copied / f / 1024.0,
             waits, (double)wait_ticks * ms_per_tick, (double)wcalls / f, (double)wconv / 1000.0 / f, (double)wfill / 1000.0 / f, (double)wtotal / 1000.0 / f);
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// frame boundaries
// ---------------------------------------------------------------------------------------------------------------------------------

std::string opStatsReport(size_t frames)
{
    if (!gOpStats)
    {
        return "(off)";
    }
    frames = std::max<size_t>(1, frames);
    LARGE_INTEGER qf, qn;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qn);
    const unsigned long long tsc_now = __rdtsc();
    const long long window_qpc = std::max<long long>(1, qn.QuadPart - gOpStatQpc0);      // this report's window, before it is restarted below
    const double tsc_per_qpc = (double)(tsc_now - gOpStatTsc0) / (double)std::max<long long>(1, qn.QuadPart - gOpStatQpc0);
    const double ms_per_tick = 1000.0 / ((double)qf.QuadPart * tsc_per_qpc);
    struct Row { int mSlot; unsigned long long mTicks, mCalls; };
    std::vector<Row> rows;
    unsigned long long total = 0;
    for (int i = 0; i < OP_STAT_SLOTS; ++i)
    {
        if (gOpStatCalls[i])
        {
            rows.push_back({ i, gOpStatTicks[i], gOpStatCalls[i] });
            total += gOpStatTicks[i];
        }
        gOpStatTicks[i] = 0;
        gOpStatCalls[i] = 0;
    }
    gOpStatTsc0 = tsc_now;
    gOpStatQpc0 = qn.QuadPart;
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mTicks > b.mTicks; });
    std::string out;
    char buf[200];
    const double wall_ms = (double)window_qpc * 1000.0 / (double)qf.QuadPart;
    {
        const unsigned long long push_ops = gMainPushOps, push_ticks = gMainPushTicks, records = gMainRecords;
        gMainPushOps = 0;
        gMainPushTicks = 0;
        gMainRecords = 0;
        snprintf(buf, sizeof(buf), "MAIN THREAD writing the queue: %.0f calls/frame -> %.0f records/frame, %.2f ms/frame (%.0f ns per call) | ",
                 (double)push_ops / (double)frames, (double)records / (double)frames,
                 (double)push_ticks * ms_per_tick / (double)frames, push_ops ? (double)push_ticks * ms_per_tick * 1e6 / (double)push_ops : 0.0);
        out = buf;
    }
    {
        // F2.5: the viewer's front-end entry points on the main thread (calls per frame, ms per frame from every 16th call timed, ns per call)
        out += "FRONT END main thread: ";
        for (int i = 0; i < FP_COUNT; ++i)
        {
            const double ms = (double)gFrontTicks[i] * ms_per_tick / (double)frames;
            snprintf(buf, sizeof(buf), "%s %.0f calls %.2f ms %.0f ns, ", kFrontNames[i], (double)gFrontCalls[i] / (double)frames, ms,
                     gFrontCalls[i] ? (double)gFrontTicks[i] * ms_per_tick * 1e6 / (double)gFrontCalls[i] : 0.0);
            out += buf;
            gFrontCalls[i] = 0;
            gFrontTicks[i] = 0;
        }
        out += "| ";
        out += RenderPass::report(frames);                  // F3.5a: render targets of the producer, bind / clear sites
        out += LLVk::Renderer::instance().takePassReport(frames);      // F3.5a: render passes of the worker
    }
    {
        // the plain calls still pushed one by one (candidates for typed records), most frequent first
        std::vector<std::pair<unsigned long long, int>> mc;
        for (int i = 0; i < OP_STAT_SLOTS; ++i)
        {
            if (gMainOpCalls[i])
            {
                mc.push_back({ gMainOpCalls[i], i });
            }
            gMainOpCalls[i] = 0;
        }
        std::sort(mc.begin(), mc.end(), [](const std::pair<unsigned long long, int>& a, const std::pair<unsigned long long, int>& b) { return a.first > b.first; });
        out += "main thread pushed calls/frame: ";
        for (size_t k = 0; k < mc.size() && k < 16; ++k)
        {
            const int slot = mc[k].second;
            const char* name = slotName(slot);
            snprintf(buf, sizeof(buf), "%s %.0f, ", name, (double)mc[k].first / (double)frames);
            out += buf;
        }
        out += "| ";
    }
    {
        // what forced the pending texture slots out as records of their own (everything else folds into a draw)
        std::vector<std::pair<unsigned long long, int>> fw;
        for (int i = 0; i < OP_STAT_SLOTS; ++i)
        {
            if (gTexFlushWhy[i])
            {
                fw.push_back({ gTexFlushWhy[i], i });
            }
            gTexFlushWhy[i] = 0;
        }
        std::sort(fw.begin(), fw.end(), [](const std::pair<unsigned long long, int>& a, const std::pair<unsigned long long, int>& b) { return a.first > b.first; });
        out += "texture slots flushed alone by (per frame): ";
        for (size_t k = 0; k < fw.size() && k < 8; ++k)
        {
            const int slot = fw[k].second;
            const char* name = slot == 0 ? "other" : slotName(slot);
            snprintf(buf, sizeof(buf), "%s %.0f, ", name, (double)fw[k].first / (double)frames);
            out += buf;
        }
        out += "| ";
    }
    if (LLVkMirror::gPhaseTiming)
    {
        // LL_VK_WORKERPHASES: where the worker's draw time goes (ms per frame; the timers themselves add about 25 ns per reading)
        LARGE_INTEGER pf;
        QueryPerformanceFrequency(&pf);
        out += "worker draw phases ms/frame: ";
        for (int p = 0; p < LLVkMirror::PH_COUNT; ++p)
        {
            const double ms = (double)LLVkMirror::gPhaseTicks[p] * 1000.0 / (double)pf.QuadPart / (double)frames;
            LLVkMirror::gPhaseTicks[p] = 0;
            if (ms >= 0.01)
            {
                snprintf(buf, sizeof(buf), "%s %.2f, ", LLVkMirror::phaseName(p), ms);
                out += buf;
            }
        }
        out += "| ";
    }
    snprintf(buf, sizeof(buf), "%.2f ms/frame in operations out of %.2f ms/frame wall (the timer costs about 15 ns per operation) | ", (double)total * ms_per_tick / (double)frames, wall_ms / (double)frames);
    out += buf;
    for (size_t k = 0; k < rows.size() && k < 22; ++k)
    {
        const Row& r = rows[k];
        const char* name = slotName(r.mSlot);
        snprintf(buf, sizeof(buf), "%s %.0f calls %.2f ms %.0f ns | ", name, (double)r.mCalls / (double)frames, (double)r.mTicks * ms_per_tick / (double)frames,
                 (double)r.mTicks * ms_per_tick * 1e6 / (double)r.mCalls);
        out += buf;
    }
    return out;
}

std::string pacingReport()
{
    std::vector<float> b, p;
    double main_wait = 0, worker_idle = 0;
    {
        std::lock_guard<std::mutex> lock(gPaceMutex);
        b.swap(gBeginGaps);
        p.swap(gPresentGaps);
        main_wait = ticks_to_ms(gMainWaitTicks);
        worker_idle = ticks_to_ms(gWorkerIdleTicks);
        gMainWaitTicks = gWorkerIdleTicks = 0;
    }
    const size_t frames = std::max<size_t>(1, p.size());
    char buf[160];
    snprintf(buf, sizeof(buf), " | main thread waited for the worker %.1f ms/frame, worker waited for work %.1f ms/frame", main_wait / frames, worker_idle / frames);
    return "frame starts on the main thread (ms between): " + describe(b) + " | worker presents (ms between): " + describe(p) + buf;
}

void frameBegin(S32 width, S32 height)
{
    note_gap(gBeginGaps, gLastBeginTick);
    const long long wait_start = now_ticks();
    // how many frames the main thread may be ahead of the worker: one keeps the frame times even (LL_VK_ASYNC_DEPTH overrides, for comparing)
    static const unsigned depth = []() { const char* v = getenv("LL_VK_ASYNC_DEPTH"); return v ? (unsigned)std::max(1, atoi(v)) : 1u; }();
    int spins = 0;
    while (gBeginsEnqueued.load(std::memory_order_relaxed) - gBeginsDone.load(std::memory_order_acquire) > depth)
    {
        wake();
        if (++spins < 200) YieldProcessor(); else SwitchToThread();
    }
    {
        std::lock_guard<std::mutex> lock(gPaceMutex);
        gMainWaitTicks += now_ticks() - wait_start;
    }
    gBeginsEnqueued.fetch_add(1);
    S32 a[2] = { width, height };
    push(CMD_FRAME_BEGIN, a, sizeof(a));
    wake();
}
void frameEnd() { push(CMD_FRAME_END, nullptr, 0); wake(); if (gMode == 3) wait_drained(); }
void framePresent() { push(CMD_PRESENT, nullptr, 0); wake(); if (gMode == 3) wait_drained(); }
void frameHold(bool hold) { S32 a = hold ? 1 : 0; push(CMD_HOLD, &a, sizeof(a)); }
void snapshotBoundary(S32 mode) { push(CMD_SNAPSHOT_BOUNDARY, &mode, sizeof(mode)); }

// ---------------------------------------------------------------------------------------------------------------------------------
// shader program commands
// ---------------------------------------------------------------------------------------------------------------------------------

void programBind(void* program)
{
    drawProgram(program);
    if (DrawCtx* d = drawCtx())
    {
        d->mSent = program;
    }
    push(CMD_PROGRAM_BIND, &program, sizeof(program));
}
void programDestroy(void* program) { push(CMD_PROGRAM_DESTROY, &program, sizeof(program)); }
void samplerUnit(void* program, unsigned sampler_index, unsigned unit)
{
    struct A { void* p; unsigned i, u; } a = { program, sampler_index, unit };
    push(CMD_SAMPLER_UNIT, &a, sizeof(a));
}
void uniformScalar(void* program, unsigned uniform_index, int kind, unsigned n, const float* f, const int* i)
{
    struct A { void* p; unsigned idx; int kind; unsigned n; float f[4]; int i[4]; } a = { program, uniform_index, kind, n, {}, {} };
    for (unsigned k = 0; k < 4; ++k)
    {
        a.f[k] = f ? f[k] : 0.f;
        a.i[k] = i ? i[k] : 0;
    }
    if (mainProducer())
    {
        MainLap lap;
        uniAppend(CMD_UNIFORM_SCALAR, &a, sizeof(a), nullptr, 0);
        return;
    }
    push(CMD_UNIFORM_SCALAR, &a, sizeof(a));
}
void uniformVector(void* program, unsigned uniform_index, unsigned components, unsigned count, const void* data)
{
    const UniformVecArgs a = { program, uniform_index, components, count };
    if (mainProducer())
    {
        MainLap lap;
        uniAppend(CMD_UNIFORM_VECTOR, &a, sizeof(a), data, (size_t)components * count * 4);
        return;
    }
    pushData(CMD_UNIFORM_VECTOR, &a, sizeof(a), data, (size_t)components * count * 4);
}
void uniformMatrix(void* program, unsigned uniform_index, unsigned columns, unsigned rows, unsigned count, const float* data)
{
    const UniformMatArgs a = { program, uniform_index, columns, rows, count };
    if (mainProducer())
    {
        MainLap lap;
        uniAppend(CMD_UNIFORM_MATRIX, &a, sizeof(a), data, (size_t)columns * rows * count * 4);
        return;
    }
    pushData(CMD_UNIFORM_MATRIX, &a, sizeof(a), data, (size_t)columns * rows * count * 4);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// names and queries
// ---------------------------------------------------------------------------------------------------------------------------------

void resourceCreate(U32 kind, S32 n, U32* ids)
{
    LLVkGL::own_gen((int)kind, n, ids);
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::resource_create(kind, n, ids);
        return;
    }
    const U32 a[2] = { kind, (U32)std::max<S32>(n, 0) };
    pushData(CMD_RESOURCE_CREATE, a, sizeof(a), ids, (size_t)std::max<S32>(n, 0) * 4);
}

void resourceDestroy(U32 kind, S32 n, const U32* ids)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::resource_destroy(kind, n, ids);
        return;
    }
    const U32 a[2] = { kind, (U32)std::max<S32>(n, 0) };
    pushData(CMD_RESOURCE_DESTROY, a, sizeof(a), ids, (size_t)std::max<S32>(n, 0) * 4);
}

void queryResultTally(bool nonzero) { gQValue[nonzero ? 1 : 0].fetch_add(1, std::memory_order_relaxed); }
void queryResultStats(unsigned long long out[2])
{
    out[0] = gQValue[0].exchange(0);
    out[1] = gQValue[1].exchange(0);
}

void queryStats(unsigned long long out[7])
{
    for (int i = 0; i < 7; ++i)
    {
        out[i] = gQStat[i].exchange(0);
    }
}

unsigned long long queryBegin(unsigned id)
{
    if (!gPub || id >= QUERY_TABLE)
    {
        return 0;
    }
    gQStat[0].fetch_add(1, std::memory_order_relaxed);
    return gPub[id].mIssued.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void queryMarkSync(unsigned id, bool sync)
{
    if (gPub && id < QUERY_TABLE)
    {
        gPub[id].mSync.store(sync ? 1 : 0, std::memory_order_relaxed);
    }
}

void setWorkerQueryGen(unsigned id, unsigned long long gen)
{
    if (gWorkerGen && id < QUERY_TABLE)
    {
        gWorkerGen[id] = gen;
    }
}

unsigned long long workerQueryGen(unsigned id)
{
    return gWorkerGen && id < QUERY_TABLE ? gWorkerGen[id] : 0;
}

void queryPublish(unsigned id, unsigned long long gen, unsigned long long value)
{
    if (!gPub || id >= QUERY_TABLE || gen == 0)
    {
        return;
    }
    gQStat[1].fetch_add(1, std::memory_order_relaxed);
    PubQuery& q = gPub[id];
    if (gen >= q.mReady.load(std::memory_order_relaxed))
    {
        q.mValue.store(value, std::memory_order_relaxed);
        q.mReady.store(gen, std::memory_order_release);
    }
}

void queryIdle(unsigned id, unsigned long long gen)
{
    if (!gPub || id >= QUERY_TABLE || gen == 0)
    {
        return;
    }
    gQStat[2].fetch_add(1, std::memory_order_relaxed);
    PubQuery& q = gPub[id];
    if (gen >= q.mReady.load(std::memory_order_relaxed))
    {
        q.mValue.store(1, std::memory_order_relaxed);
        q.mReady.store(gen, std::memory_order_release);
    }
}

bool query_get(int kind, unsigned id, unsigned pname, void* params)
{
    static const bool no_table = getenv("LL_VK_ASYNC_NOQT") != nullptr;      // debugging: every query read goes through the mirror's own bookkeeping
    if (no_table || !gPub || id >= QUERY_TABLE || !params)
    {
        return false;
    }
    PubQuery& q = gPub[id];
    if (q.mSync.load(std::memory_order_relaxed))
    {
        return false;               // a timer query: the worker has the real answer
    }
    const unsigned long long ready = q.mReady.load(std::memory_order_acquire);
    const bool pending = ready < q.mIssued.load(std::memory_order_acquire);
    unsigned long long value = 0;
    if (pname == 0x8867)            // QUERY_RESULT_AVAILABLE
    {
        value = pending ? 0 : 1;
        gQStat[pending ? 4 : 3].fetch_add(1, std::memory_order_relaxed);
    }
    else if (pname == 0x8866)       // QUERY_RESULT
    {
        if (pending)
        {
            // The viewer gives up waiting after a few polls and forces the read. Waiting for the worker (and the GPU) would stall the main thread
            // for a frame or more; answer "visible", which is what the viewer assumes for a query it cannot resolve. The late result is ignored
            // (it carries an older generation than the next query begun under this name).
            gQStat[6].fetch_add(1, std::memory_order_relaxed);
            queryResultTally(true);
            switch (kind)
            {
            case 0: *(S32*)params = 1; break;
            case 1: *(U32*)params = 1; break;
            default: *(U64*)params = 1; break;
            }
            return true;
        }
        value = q.mValue.load(std::memory_order_relaxed);
        queryResultTally(value != 0);
        q.mValue.store(1, std::memory_order_relaxed);       // read once: afterwards a query nobody is waiting for reports visible
    }
    else
    {
        return false;
    }
    switch (kind)
    {
    case 0: *(S32*)params = (S32)value; break;
    case 1: *(U32*)params = (U32)value; break;
    default: *(U64*)params = value; break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// hand-written encoders for the calls that carry data or keep main-side state
// ---------------------------------------------------------------------------------------------------------------------------------

namespace
{

size_t pixel_bytes(LLVkFormat::Transfer tr, S32 w, S32 h, S32 d, const PixelStoreShadow& ps)
{
    const unsigned comps = LLVkFormat::compCount(tr.mComps), tb = LLVkFormat::typeBytes(tr.mType);
    if (!comps || !tb || w <= 0 || h <= 0 || d <= 0)
    {
        return 0;       // the worker's conversion refuses these without reading anything
    }
    const size_t row_texels = ps.mUnpackRowLength > 0 ? (size_t)ps.mUnpackRowLength : (size_t)w;
    const size_t align = ps.mUnpackAlignment > 0 ? (size_t)ps.mUnpackAlignment : 4;
    const size_t row_bytes = (row_texels * comps * tb + align - 1) / align * align;
    return ((size_t)d * h - 1) * row_bytes + (size_t)w * comps * tb;
}

struct TexArgs
{
    LLVkFormat::Target mTarget; LLVkFormat::Transfer mTransfer; U8 mStore; U8 mPad8[3];
    S32 mLevel;
    S32 mWidth, mHeight, mDepth;
    S32 mX, mY, mZ;
    S32 mImageSize;
    U64 mOffset;                // the pixels pointer when the data comes from a bound pixel unpack buffer (mode 2) or from a held image (mode 3)
    U32 mMode;                  // 0 no data, 1 data follows, 2 offset into the unpack buffer, 3 pixels in a held image (no copy)
    const void* mOwner;         // mode 3: what keeps the pixels alive; the worker calls mUnref on it after reading them
    void (*mUnref)(const void*);
    // F1.2 (CMD_TEX_IMAGE): the texture the edit names, which call it is, and the unpack state it needs
    U32 mTex; U32 mKind; S32 mAlignment; S32 mRowLength; U32 mUnpackBuffer; U32 mPad;
};

struct TexMainLap
{
    LARGE_INTEGER mT0;
    bool mMain;
    TexMainLap() : mMain(isMainThread()) { if (mMain) QueryPerformanceCounter(&mT0); }
    ~TexMainLap()
    {
        if (mMain)
        {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            gTexMainTicks.fetch_add((unsigned long long)(t1.QuadPart - mT0.QuadPart), std::memory_order_relaxed);
            gTexMainCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

void push_tex(unsigned short op, TexArgs& a, const void* pixels, size_t bytes)
{
    if (!pixels)
    {
        a.mMode = 0;
        push(op, &a, sizeof(a));
    }
    else if (tlsPixelStore.mUnpackBuffer)
    {
        a.mMode = 2;
        a.mOffset = (U64)(uintptr_t)pixels;
        push(op, &a, sizeof(a));
    }
    else if (gTexOneCopy && tlsHoldSet && bytes && isMainThread() && (const U8*)pixels >= (const U8*)tlsHoldData.mBase
             && (const U8*)pixels + bytes <= (const U8*)tlsHoldData.mBase + tlsHoldData.mSize)
    {
        // the caller keeps these pixels alive and unchanged: queue a reference, the worker reads them where they are
        a.mMode = 3;
        a.mOffset = (U64)(uintptr_t)pixels;
        a.mOwner = tlsHoldData.mOwner;
        a.mUnref = tlsHoldData.mUnref;
        tlsHoldData.mRef(tlsHoldData.mOwner);
        gTexHandedCalls.fetch_add(1, std::memory_order_relaxed);
        gTexHandedBytes.fetch_add(bytes, std::memory_order_relaxed);
        push(op, &a, sizeof(a));
    }
    else
    {
        a.mMode = 1;
        gTexCopiedBytes.fetch_add(bytes, std::memory_order_relaxed);
        pushData(op, &a, sizeof(a), pixels, bytes);
    }
}

} // anonymous namespace


// ---- F1.2: texture edits that name their texture (llvktex.h) ----
} // namespace LLVkAsync (reopened below)

namespace LLVkTex
{
using namespace LLVkAsync;

namespace
{

PixelStore currentPS()
{
    PixelStore ps;
    ps.mAlignment = tlsPixelStore.mUnpackAlignment;
    ps.mRowLength = tlsPixelStore.mUnpackRowLength;
    ps.mUnpackBuffer = LLVkAsync::gEnabled ? tlsPixelStore.mUnpackBuffer : LLVkGL::inline_pixel_store().mUnpackBuffer;     // the buffer binding is still a GL call (buffers family)
    return ps;
}

void pushImage(TexKind kind, TexArgs a, const void* pixels, size_t bytes)
{
    a.mKind = kind;
    a.mAlignment = tlsPixelStore.mUnpackAlignment;
    a.mRowLength = tlsPixelStore.mUnpackRowLength;
    a.mUnpackBuffer = tlsPixelStore.mUnpackBuffer;
    push_tex(CMD_TEX_IMAGE, a, pixels, bytes);
}

inline std::lock_guard<LLVkMirror::Mutex> inlineLock() { return std::lock_guard<LLVkMirror::Mutex>(LLVkGL::gMutex); }
}

void create(S32 count, U32* ids)
{
    LLVkAsync::resourceCreate(LLVkAsync::RES_TEXTURE, count, ids);
}

void destroy(S32 count, const U32* ids)
{
    LLVkAsync::resourceDestroy(LLVkAsync::RES_TEXTURE, count, ids);
}

void setUnpackAlignment(S32 alignment)
{
    tlsPixelStore.mUnpackAlignment = alignment;
}

void setUnpackRowLength(S32 row_length)
{
    tlsPixelStore.mUnpackRowLength = row_length;
}

void bindSlot(S32 unit, U32 id)
{
    if (LLVkAsync::gEnabled)
    {
        if (produces())
        {
            LLVkAsync::bindTextureUnit((U32)unit, id);
        }
        return;
    }
    auto lock = inlineLock();
    LLVkGL::slot_set((unsigned)unit, id);
}

void image2D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 border, U32 format, U32 type, const void* pixels)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Store st = LLVkFormat::store(internal_format);
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_image2D(id, currentPS(), tg, level, st, width, height, tr, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = {};
    a.mTarget = tg; a.mTransfer = tr; a.mStore = st; a.mLevel = level; a.mWidth = width; a.mHeight = height; a.mDepth = 1; a.mTex = id;
    pushImage(K_IMAGE2D, a, pixels, pixel_bytes(tr, width, height, 1, tlsPixelStore));
}

void image3D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 depth, S32 border, U32 format, U32 type, const void* pixels)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Store st = LLVkFormat::store(internal_format);
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_image3D(id, currentPS(), tg, level, st, width, height, depth, tr, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = {};
    a.mTarget = tg; a.mTransfer = tr; a.mStore = st; a.mLevel = level; a.mWidth = width; a.mHeight = height; a.mDepth = depth; a.mTex = id;
    pushImage(K_IMAGE3D, a, pixels, pixel_bytes(tr, width, height, depth, tlsPixelStore));
}

void subImage2D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_subimage2D(id, currentPS(), tg, level, x, y, width, height, tr, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = {};
    a.mTarget = tg; a.mTransfer = tr; a.mLevel = level; a.mWidth = width; a.mHeight = height; a.mDepth = 1; a.mX = x; a.mY = y; a.mTex = id;
    pushImage(K_SUB2D, a, pixels, pixel_bytes(tr, width, height, 1, tlsPixelStore));
}

void subImage3D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type, const void* pixels)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_subimage3D(id, currentPS(), tg, level, x, y, z, width, height, depth, tr, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = {};
    a.mTarget = tg; a.mTransfer = tr; a.mLevel = level; a.mWidth = width; a.mHeight = height; a.mDepth = depth; a.mX = x; a.mY = y; a.mZ = z; a.mTex = id;
    pushImage(K_SUB3D, a, pixels, pixel_bytes(tr, width, height, depth, tlsPixelStore));
}

void compressedImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 width, S32 height, S32 border, S32 image_size, const void* data)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Store st = LLVkFormat::store((S32)internal_format);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_compressed2D(id, currentPS(), tg, level, st, width, height, image_size, data);
        return;
    }
    TexMainLap lap;
    TexArgs a = {};
    a.mTarget = tg; a.mStore = st; a.mLevel = level; a.mWidth = width; a.mHeight = height; a.mDepth = 1; a.mImageSize = image_size; a.mTex = id;
    pushImage(K_COMPRESSED2D, a, data, (size_t)std::max(image_size, 0));
}

void params(U32 id, U32 target, const LLVkAsync::TexParam* p, U32 count)
{
    (void)target;           // a setting acts on the named texture: its dimensionality does not matter
    constexpr U32 CHUNK = 16;
    for (U32 base = 0; base < count; base += CHUNK)
    {
        LLVkFormat::Setting st[CHUNK];
        U32 n = 0;
        for (U32 i = base; i < count && n < CHUNK; ++i)
        {
            const LLVkFormat::Setting s = LLVkFormat::setting(p[i].mPname, p[i].mIsFloat != 0, p[i].mInt, p[i].mFloat);
            if (s.mParam != LLVkFormat::P_NONE && s.mParam != LLVkFormat::P_GENERATE_MIPMAP)
            {
                st[n++] = s;
            }
        }
        if (!n)
        {
            continue;
        }
        if (!LLVkAsync::gEnabled)
        {
            auto lock = inlineLock();
            LLVkGL::tex_params(id, st, n);
            continue;
        }
        const U32 head[2] = { id, n };
        pushData(CMD_TEX_PARAMS_ID, head, sizeof(head), st, sizeof(LLVkFormat::Setting) * n);
    }
}

static void pushOp(const TexOp& op)
{
    push(CMD_TEX_OP, &op, sizeof(op));
}

void swizzle(U32 id, U32 target, const S32* rgba)
{
    (void)target;
    LLVkFormat::Swizzle swz[4];
    for (U32 i = 0; i < 4; ++i)
    {
        swz[i] = LLVkFormat::swizzle(rgba[i]);
    }
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_swizzle(id, swz);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_SWIZZLE; op.mTex = id;
    for (U32 i = 0; i < 4; ++i)
    {
        op.mSwz[i] = swz[i];
    }
    pushOp(op);
}

void generateMipmap(U32 id, U32 target)
{
    (void)target;
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_mipmap(id);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_MIPMAP; op.mTex = id;
    pushOp(op);
}

void copySubImage(U32 id, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height, U32 source)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_copy_sub(id, tg, level, xoffset, yoffset, zoffset, x, y, width, height, source);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_COPY_SUB; op.mTex = id; op.mTarget = tg; op.mLevel = level;
    op.mX0 = xoffset; op.mY0 = yoffset; op.mZ0 = zoffset; op.mX = x; op.mY = y; op.mW = width; op.mH = height; op.mSource = source;
    pushOp(op);
}

void copyImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 x, S32 y, S32 width, S32 height, S32 border, U32 source)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Store st = LLVkFormat::store((S32)internal_format);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_copy_image(id, tg, level, st, x, y, width, height, source);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_COPY_IMAGE; op.mTex = id; op.mTarget = tg; op.mLevel = level; op.mStore = st;
    op.mX = x; op.mY = y; op.mW = width; op.mH = height; op.mSource = source;
    pushOp(op);
}

// A read back of a texture is a round trip: the main thread waits until the worker has replayed everything queued before it (and then for the copy). With the worker far behind (a burst of
// texture uploads) one sculpted prim asking for its map cost seconds, and a plaza of them 15 s. The caller asks first and tries again in a later frame when the answer is no: not when
// the worker has more than LL_VK_READBACK_BACKLOG_KB (default 4096) queued, and not more than LL_VK_READBACKS_PER_FRAME (default 4) a frame. Main thread only.
bool readbackAffordable()
{
    if (!LLVkAsync::gEnabled)
    {
        return true;
    }
    static const U64 limit_bytes = [](){ const char* e = getenv("LL_VK_READBACK_BACKLOG_KB"); return (U64)(e && e[0] ? atoi(e) : 4096) * 1024; }();
    static const U32 per_frame = [](){ const char* e = getenv("LL_VK_READBACKS_PER_FRAME"); return (U32)(e && e[0] ? atoi(e) : 4); }();
    static U32 frame = ~0u, used = 0;
    const U32 now = (U32)LLFrameTimer::getFrameCount();
    if (frame != now)
    {
        frame = now;
        used = 0;
    }
    if (used >= per_frame || LLVkAsync::backlogBytes() > limit_bytes)
    {
        return false;
    }
    ++used;
    return true;
}

S32 levelParameter(U32 id, U32 target, S32 level, U32 pname)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Level q = LLVkFormat::level(pname);
    S32 v = 0;
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        return LLVkGL::tex_level_parameter(id, tg, level, q);
    }
    LLVkAsync::syncCall([&]() { v = LLVkGL::tex_level_parameter(id, tg, level, q); });
    return v;
}

void getImage(U32 id, U32 target, S32 level, U32 format, U32 type, void* out)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_get_image(id, tg, level, tr, out);
        return;
    }
    LLVkAsync::syncCall([&]() { LLVkGL::tex_get_image(id, tg, level, tr, out); });
}

void getCompressedImage(U32 id, U32 target, S32 level, void* out)
{
    const LLVkFormat::Target tg = LLVkFormat::target(target);
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_get_compressed_image(id, tg, level, out);
        return;
    }
    LLVkAsync::syncCall([&]() { LLVkGL::tex_get_compressed_image(id, tg, level, out); });
}

}

// ---- F1.4: render targets named by id (llvktarget.h) ----
namespace LLVkTarget
{

void create(U32* id)
{
    LLVkAsync::resourceCreate(LLVkAsync::RES_TARGET, 1, id);
}

void destroy(U32 id)
{
    LLVkAsync::resourceDestroy(LLVkAsync::RES_TARGET, 1, &id);
}

void attach(U32 target, U32 slot, U32 texture)
{
    LLVkAsync::framebufferAttach(target, slot, texture);
}

void clear(bool color, bool depth, const F32* rgba)
{
    if (!LLVkAsync::gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::clear_target(color, depth, rgba);
        return;
    }
    LLVkAsync::clearTarget(color, depth, rgba);
}

void readPixels(U32 target, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, void* dst)
{
    const LLVkFormat::Transfer tr = LLVkFormat::transfer(format, type);
    if (!LLVkAsync::gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::read_pixels(target, x, y, width, height, tr, dst);
        return;
    }
    LLVkAsync::syncCall([&]() { LLVkGL::read_pixels(target, x, y, width, height, tr, dst); });
}

}

// ---- F1.6: queries by id (llvkquery.h) ----
namespace LLVkQuery
{

void create(S32 count, U32* ids)
{
    LLVkAsync::resourceCreate(LLVkAsync::RES_QUERY, count, ids);
}

void destroy(S32 count, const U32* ids)
{
    LLVkAsync::resourceDestroy(LLVkAsync::RES_QUERY, count, ids);
}

void begin(Kind kind, U32 id)
{
    if (!LLVkAsync::gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::query_begin(kind, id);
        return;
    }
    LLVkAsync::queryBeginCmd(kind, id);
}

void end(Kind kind)
{
    if (!LLVkAsync::gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::query_end(kind);
        return;
    }
    LLVkAsync::queryEndCmd(kind);
}

namespace
{
// 0x8867: result available, 0x8866: the result
U64 query_read(U32 id, bool want_result)
{
    U64 value = 0;
    if (LLVkAsync::gEnabled)
    {
        if (LLVkAsync::query_get(2, id, want_result ? 0x8866 : 0x8867, &value))
        {
            return value;
        }
        LLVkAsync::syncCall([&]() { LLVkGL::query_value(id, want_result, value); });
        return value;
    }
    std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
    LLVkGL::query_value(id, want_result, value);
    return value;
}
}

bool ready(U32 id)
{
    return query_read(id, false) != 0;
}

U64 result(U32 id)
{
    return query_read(id, true);
}

}

// ---- F1.3: buffers named by id (llvkbuf.h) ----
namespace LLVkBuf
{

void create(S32 count, U32* ids)
{
    LLVkAsync::resourceCreate(LLVkAsync::RES_BUFFER, count, ids);
}

void destroy(S32 count, const U32* ids)
{
    LLVkAsync::resourceDestroy(LLVkAsync::RES_BUFFER, count, ids);
}

void setData(U32 id, BufferKind kind, S64 size, const void* data)
{
    LLVkAsync::bufferData(id, kind, size, data);
}

void setConstantAttribute(U32 location, F32 x, F32 y, F32 z, F32 w)
{
    if (!LLVkAsync::gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::constant_attribute(location, x, y, z, w);
        return;
    }
    struct { U32 mLocation; F32 mV[4]; } rec = { location, { x, y, z, w } };
    LLVkAsync::push(LLVkAsync::CMD_CONST_ATTRIB, &rec, sizeof(rec));
}

void bindUniformBlock(U32 binding, U32 id)
{
    const U32 a[2] = { binding, id };
    LLVkAsync::push(LLVkAsync::CMD_UNIFORM_BLOCK, a, sizeof(a));
}

}

namespace LLVkAsync
{

void queryBeginCmd(U32 kind, U32 id)
{
    // occlusion queries are the ones the viewer waits for; the others (timers) are answered through the worker
    const bool occlusion = kind <= LLVkQuery::SAMPLES_PASSED;
    queryMarkSync(id, !occlusion);
    struct A { U32 mKind; U32 mId; U64 mGen; } a = { kind, id, occlusion ? queryBegin(id) : 0 };
    push(CMD_QUERY_BEGIN, &a, sizeof(a));
}

void queryEndCmd(U32 kind)
{
    push(CMD_QUERY_END, &kind, sizeof(kind));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------------------------------------------------------------

namespace
{

struct Reader
{
    const U8* mP;
    template <class T> T get() { T v; memcpy(&v, mP, sizeof(T)); mP += sizeof(T); return v; }
    const U8* bytes(size_t n) { const U8* r = mP; mP += n; return r; }
};

// the data block of a pushData record: inline, or a heap copy the worker frees
struct Blob
{
    const U8* mData = nullptr;
    size_t mSize = 0;
    void* mFree = nullptr;
    Blob() = default;
    Blob(const Blob&) = delete;                 // the heap copy has exactly one owner
    Blob& operator=(const Blob&) = delete;
    Blob(Blob&& o) noexcept : mData(o.mData), mSize(o.mSize), mFree(o.mFree) { o.mData = nullptr; o.mFree = nullptr; }
    Blob& operator=(Blob&& o) noexcept
    {
        if (this != &o)
        {
            if (mFree) free(mFree);
            mData = o.mData; mSize = o.mSize; mFree = o.mFree;
            o.mData = nullptr; o.mFree = nullptr;
        }
        return *this;
    }
    ~Blob() { if (mFree) free(mFree); }
};
Blob read_blob(Reader& r, U8 flags)
{
    Blob b;
    b.mSize = (size_t)r.get<U64>();
    if (flags & 1)
    {
        b.mFree = (void*)(uintptr_t)r.get<U64>();
        b.mData = (const U8*)b.mFree;
    }
    else if (b.mSize)
    {
        b.mData = r.bytes(b.mSize);
    }
    return std::move(b);
}

// plays the entries of a batch of loose uniform writes (see uniAppend) with the same calls the separate records make
void applyUniformBatch(const U8* p, U32 bytes)
{
    const U8* end = p + bytes;
    while (p + 8 <= end)
    {
        U32 head[2];
        memcpy(head, p, 8);
        const U8* d = p + 8;
        switch (head[0])
        {
        case CMD_UNIFORM_SCALAR:
        {
            UniformScalarArgs a;
            memcpy(&a, d, sizeof(a));
            LLVk::Program* pr = (LLVk::Program*)a.mProgram;
            if (pr)
            {
                if (a.mKind == 0) pr->setFloatsAt((size_t)a.mIndex, a.mF, a.mN, 1);
                else pr->setFloatsAt((size_t)a.mIndex, reinterpret_cast<const F32*>(a.mI), a.mN, 1);
            }
            break;
        }
        case CMD_UNIFORM_VECTOR:
        {
            UniformVecArgs a;
            memcpy(&a, d, sizeof(a));
            LLVk::Program* pr = (LLVk::Program*)a.mProgram;
            if (pr) pr->setFloatsAt((size_t)a.mIndex, reinterpret_cast<const F32*>(d + sizeof(a)), a.mComponents, a.mCount);
            break;
        }
        case CMD_UNIFORM_MATRIX:
        {
            UniformMatArgs a;
            memcpy(&a, d, sizeof(a));
            LLVk::Program* pr = (LLVk::Program*)a.mProgram;
            if (pr) pr->setMatricesAt((size_t)a.mIndex, reinterpret_cast<const F32*>(d + sizeof(a)), a.mColumns, a.mRows, a.mCount);
            break;
        }
        case CMD_UNIFORM_RANGE:
        {
            struct A { void* mProgram; U32 mClass; U32 mOffset; U32 mSize; U32 mPad; } a;
            memcpy(&a, d, sizeof(a));
            LLVk::Program* pr = (LLVk::Program*)a.mProgram;
            if (pr)
            {
                if (gUniformRec == 1) pr->writeRangeVerify(a.mClass, a.mOffset, a.mSize, d + sizeof(a));
                else pr->writeRange(a.mClass, a.mOffset, a.mSize, d + sizeof(a));
            }
            break;
        }
        default:
            break;
        }
        p += 8 + head[1];
    }
}


// F1.2: the worker side of the texture records (llvktex.h)
using namespace LLVkTex;
void runImage(Reader& r, U8 flags)
{
    const TexArgs a = r.get<TexArgs>();
    Blob blob;
    const void* pixels = nullptr;
    if (a.mMode == 1)
    {
        blob = read_blob(r, flags);
        pixels = blob.mData;
    }
    else if (a.mMode == 2 || a.mMode == 3)
    {
        pixels = (const void*)(uintptr_t)a.mOffset;
    }
    struct Release { const TexArgs& mA; ~Release() { if (mA.mMode == 3 && mA.mUnref) mA.mUnref(mA.mOwner); } } release = { a };
    PixelStore ps;
    ps.mAlignment = a.mAlignment; ps.mRowLength = a.mRowLength; ps.mUnpackBuffer = a.mUnpackBuffer;
    const LLVkFormat::Store st = (LLVkFormat::Store)a.mStore;
    switch (a.mKind)
    {
    case K_IMAGE2D: LLVkGL::tex_image2D(a.mTex, ps, a.mTarget, a.mLevel, st, a.mWidth, a.mHeight, a.mTransfer, pixels); break;
    case K_IMAGE3D: LLVkGL::tex_image3D(a.mTex, ps, a.mTarget, a.mLevel, st, a.mWidth, a.mHeight, a.mDepth, a.mTransfer, pixels); break;
    case K_SUB2D: LLVkGL::tex_subimage2D(a.mTex, ps, a.mTarget, a.mLevel, a.mX, a.mY, a.mWidth, a.mHeight, a.mTransfer, pixels); break;
    case K_SUB3D: LLVkGL::tex_subimage3D(a.mTex, ps, a.mTarget, a.mLevel, a.mX, a.mY, a.mZ, a.mWidth, a.mHeight, a.mDepth, a.mTransfer, pixels); break;
    default: LLVkGL::tex_compressed2D(a.mTex, ps, a.mTarget, a.mLevel, st, a.mWidth, a.mHeight, a.mImageSize, pixels); break;
    }
}

void runParams(Reader& r, U8 flags)
{
    const U32 id = r.get<U32>(), count = r.get<U32>();
    Blob b = read_blob(r, flags);
    LLVkGL::tex_params(id, (const LLVkFormat::Setting*)b.mData, count);
}

void runOp(Reader& r)
{
    const TexOp o = r.get<TexOp>();
    switch (o.mKind)
    {
    case OPK_MIPMAP: LLVkGL::tex_mipmap(o.mTex); break;
    case OPK_COPY_SUB: LLVkGL::tex_copy_sub(o.mTex, o.mTarget, o.mLevel, o.mX0, o.mY0, o.mZ0, o.mX, o.mY, o.mW, o.mH, o.mSource); break;
    case OPK_COPY_IMAGE: LLVkGL::tex_copy_image(o.mTex, o.mTarget, o.mLevel, (LLVkFormat::Store)o.mStore, o.mX, o.mY, o.mW, o.mH, o.mSource); break;
    default:
    {
        LLVkFormat::Swizzle swz[4];
        for (U32 i = 0; i < 4; ++i)
        {
            swz[i] = (LLVkFormat::Swizzle)o.mSwz[i];
        }
        LLVkGL::tex_swizzle(o.mTex, swz);
        break;
    }
    }
}

void dispatch_cmd(unsigned short op, U8 flags, const U8* payload)
{
    Reader r{ payload };
    switch (op)
    {
    case CMD_FRAME_BEGIN:
    {
        const S32 w = r.get<S32>(), h = r.get<S32>();
        gBeginsDone.fetch_add(1, std::memory_order_release);
        LLVkMirror::beginFrameImpl(w, h);
        break;
    }
    case CMD_FRAME_END: LLVkMirror::endFrameImpl(); break;
    case CMD_CONST_ATTRIB:
    {
        struct R { U32 mLocation; F32 mV[4]; };
        const R* a = (const R*)payload;
        LLVkGL::constant_attribute(a->mLocation, a->mV[0], a->mV[1], a->mV[2], a->mV[3]);
        break;
    }
    case CMD_BUFFER_DATA:
    {
        const NamedBufArgs a = r.get<NamedBufArgs>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_bufferData(a.mName, a.mTarget, (S64)a.mSize, b.mData);
        break;
    }
    case CMD_BUFFER_SUBDATA:
    {
        const NamedBufArgs a = r.get<NamedBufArgs>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_bufferSubData(a.mName, (S64)a.mOffset, (S64)a.mSize, b.mData);
        break;
    }
    case CMD_BUFFER_RECYCLE:
    {
        const NamedBufArgs a = r.get<NamedBufArgs>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_bufferRecycle(a.mName);
        break;
    }
    case CMD_SHADOW_PACKETS:
    {
        const LLVkMirror::ShadowPacketsHeader h = r.get<LLVkMirror::ShadowPacketsHeader>();
        Blob b = read_blob(r, flags);
        if (b.mData)
        {
            LLVkMirror::shadowPacketsImpl(h, b.mData);
        }
        break;
    }
    case CMD_PRESENT:
        LLVkMirror::presentFrameImpl();
        note_gap(gPresentGaps, gLastPresentTick);
        if (gOpStatsPeriodic && ++gOpStatsFrames >= 600)
        {
            LL_INFOS("VkOps") << "frame pacing: " << pacingReport() << LL_ENDL;
            LL_INFOS("VkOps") << "worker operations: " << opStatsReport(gOpStatsFrames) << LL_ENDL;
            gOpStatsFrames = 0;
        }
        if (gUniformRec == 1)
        {
            static unsigned check_frames = 0;
            if (++check_frames >= 600)
            {
                check_frames = 0;
                LL_INFOS("VkDrawRec") << "draw record check: " << LLVkGL::draw_check_report() << LL_ENDL;
            }
        }
        break;
    case CMD_HOLD: LLVkMirror::holdFrameImpl(r.get<S32>() != 0); break;
    case CMD_SNAPSHOT_BOUNDARY: LLVkMirror::snapshotBoundaryImpl(r.get<S32>()); break;
    case CMD_RT_SCENE:
    {
        LLVkMirror::RtSceneArgs a;
        memcpy(&a, payload, sizeof(a));
        LLVkMirror::rtSceneImpl(a);
        break;
    }
    case CMD_UPSCALE:
    {
        LLVkMirror::UpscaleArgs a;
        memcpy(&a, payload, sizeof(a));         // the same struct the producer pushed
        LLVkMirror::upscaleImpl(a);
        break;
    }
    case CMD_GPU_ZONE:
    {
        const U32 head[2] = { r.get<U32>(), r.get<U32>() };
        Blob b = read_blob(r, flags);
        LLVk::Renderer::instance().gpuZone(b.mData ? (const char*)b.mData : "", head[0] != 0);
        break;
    }
    case CMD_TEX_SLOTS:
    {
        const U32* head = (const U32*)payload;
        const U32* e = (const U32*)(payload + sizeof(U32) * 2);
        U32 k = 0;
        for (U32 m = head[0]; m; m &= m - 1, ++k)
        {
            LLVkGL::slot_set(lowBit(m), e[k]);
        }
        break;
    }
    case CMD_UNIFORM_BATCH:
    {
        U32 bytes;
        memcpy(&bytes, payload, sizeof(bytes));
        applyUniformBatch(payload + sizeof(U32) * 2, bytes);
        break;
    }
    case CMD_DRAW:
    case CMD_DRAW_SCRATCH:
    {
        const DrawHeader2* h2 = (const DrawHeader2*)payload;
        const DrawHeader* h = &h2->mDraw;
        DrawBinding b;
        if (h2->mBindBytes)
        {
            drawBindingRead(payload + sizeof(DrawHeader2), b);
        }
        LLVkGL::draw_context_apply(h2->mProgram, h2->mBindBytes ? &b : nullptr);
        const U32* ids = (const U32*)(payload + sizeof(DrawHeader2) + h2->mBindBytes);
        const U32 id_bytes = (h->mCountTex * (U32)sizeof(U32) + 7) & ~7u;
        if (h->mUniBytes)
        {
            applyUniformBatch(payload + sizeof(DrawHeader2) + h2->mBindBytes + id_bytes, h->mUniBytes);
        }
        if (gUniformRec == 1)
        {
            LLVkGL::uniform_check();
        }
        U32 k = 0;
        for (U32 m = h->mMask; m; m &= m - 1, ++k)
        {
            LLVkGL::slot_set(lowBit(m), ids[k]);
        }
        if (h->mKind == 0)
        {
            LLVkGL::draw_elements(h->mMode, h->mStart, h->mEnd, h->mCount, h->mType, h->mIndices);
        }
        else if (h->mKind == 2)
        {
            LLVkGL::draw_scratch(h->mMode, h->mCount, h->mStart, payload + sizeof(DrawHeader2) + h2->mBindBytes + id_bytes + h->mUniBytes);
        }
        else
        {
            LLVkGL::draw_arrays(h->mMode, (S32)h->mStart, h->mCount);
        }
        break;
    }
    case CMD_STATE:
    {
        LLVkState::Block b;
        memcpy(&b, payload, sizeof(b));
        LLVkState::receive(b);
        break;
    }
    case CMD_FBO_ATTACH:
    {
        const U32* a = (const U32*)payload;
        LLVkGL::target_attach(a[0], a[1], a[2]);
        break;
    }
    case CMD_TARGET_STATE:
    {
        const S32* a = (const S32*)payload;
        LLVkGL::target_state((unsigned)a[0], a[1]);
        break;
    }
    case CMD_PASS:
    {
        struct R { S32 mFbo; S32 mDrawBuffers; U32 mFlags; F32 mColor[4]; };
        const R* a = (const R*)payload;
        LLVkGL::target_state((unsigned)a->mFbo, a->mDrawBuffers);
        if (a->mFlags & 3)
        {
            LLVkGL::clear_target((a->mFlags & 1) != 0, (a->mFlags & 2) != 0, a->mColor);
        }
        break;
    }
    case CMD_CLEAR:
    {
        struct R { U32 mFlags; F32 mColor[4]; };
        const R* a = (const R*)payload;
        LLVkGL::clear_target((a->mFlags & 1) != 0, (a->mFlags & 2) != 0, a->mColor);
        break;
    }
    case CMD_UNIFORM_BLOCK: { const U32* a = (const U32*)payload; LLVkGL::uniform_block_bind(a[0], a[1]); break; }
    case CMD_QUERY_BEGIN:
    {
        struct A { U32 mKind; U32 mId; U64 mGen; };
        const A* a = (const A*)payload;
        setWorkerQueryGen(a->mId, a->mGen);
        LLVkGL::query_begin(a->mKind, a->mId);
        break;
    }
    case CMD_QUERY_END: LLVkGL::query_end(*(const U32*)payload); break;
    case CMD_RESOURCE_CREATE: case CMD_RESOURCE_DESTROY:
    {
        const U32 kind = r.get<U32>();
        const U32 n = r.get<U32>();
        Blob b = read_blob(r, flags);
        if (op == CMD_RESOURCE_CREATE) LLVkGL::resource_create(kind, (int)n, (const unsigned*)b.mData);
        else LLVkGL::resource_destroy(kind, (int)n, (const unsigned*)b.mData);
        break;
    }
    case CMD_TEX_IMAGE: runImage(r, flags); break;
    case CMD_TEX_PARAMS_ID: runParams(r, flags); break;
    case CMD_TEX_OP: runOp(r); break;
    case CMD_PROGRAM_BIND:
    {
        LLVk::Program* p = r.get<LLVk::Program*>();
        if (LLVkMirror::isReady())
        {
            LLVk::Renderer::instance().setProgram(p);       // null unbinds
        }
        break;
    }
    case CMD_PROGRAM_DESTROY:
    {
        LLVk::Program* p = r.get<LLVk::Program*>();
        if (p)
        {
            LLVk::Renderer& rr = LLVk::Renderer::instance();
            if (rr.getProgram() == p)
            {
                rr.setProgram(nullptr);     // never leave the renderer pointing at a freed program
            }
            p->destroy();
            delete p;
        }
        break;
    }
    case CMD_SAMPLER_UNIT:
    {
        LLVk::Program* p = r.get<LLVk::Program*>();
        const unsigned i = r.get<unsigned>(), u = r.get<unsigned>();
        if (p) p->setSamplerUnit((size_t)i, (U32)u);
        break;
    }
    case CMD_UNIFORM_SCALAR:
    {
        LLVk::Program* p = r.get<LLVk::Program*>();
        const unsigned idx = r.get<unsigned>();
        const int kind = r.get<int>();
        const unsigned n = r.get<unsigned>();
        float f[4];
        int iv[4];
        memcpy(f, r.bytes(sizeof(f)), sizeof(f));
        memcpy(iv, r.bytes(sizeof(iv)), sizeof(iv));
        if (p)
        {
            if (kind == 0) p->setFloatsAt((size_t)idx, f, n, 1);
            else p->setFloatsAt((size_t)idx, reinterpret_cast<const F32*>(iv), n, 1);
        }
        break;
    }
    case CMD_UNIFORM_VECTOR:
    {
        const UniformVecArgs a = r.get<UniformVecArgs>();
        LLVk::Program* p = (LLVk::Program*)a.mProgram;
        Blob b = read_blob(r, flags);
        if (p && b.mData) p->setFloatsAt((size_t)a.mIndex, reinterpret_cast<const F32*>(b.mData), a.mComponents, a.mCount);
        break;
    }
    case CMD_UNIFORM_MATRIX:
    {
        const UniformMatArgs a = r.get<UniformMatArgs>();
        LLVk::Program* p = (LLVk::Program*)a.mProgram;
        Blob b = read_blob(r, flags);
        if (p && b.mData) p->setMatricesAt((size_t)a.mIndex, reinterpret_cast<const F32*>(b.mData), a.mColumns, a.mRows, a.mCount);
        break;
    }
    case CMD_SYNC:
    {
        struct Args { const std::function<void()>* mFn; std::atomic<int>* mDone; };
        const Args a = r.get<Args>();
        (*a.mFn)();
        a.mDone->store(1, std::memory_order_release);
        WakeByAddressAll(a.mDone);
        break;
    }
    default:
        LL_WARNS("VkAsync") << "unknown command " << op << LL_ENDL;
        break;
    }
}

unsigned short gCurrentOp = 0;
unsigned char gCurrentCtx = 0;

void execute(U16 op, U8 ctx, U8 flags, const U8* payload)
{
    gCurrentOp = op;
    gCurrentCtx = ctx;
    LLVkGL::set_worker_context(ctx);
    const unsigned long long t0 = gOpStats ? __rdtsc() : 0;
    dispatch_cmd(op, flags, payload);
    if (gOpStats)
    {
        const int slot = opStatSlot(op);
        gOpStatTicks[slot] += __rdtsc() - t0;
        ++gOpStatCalls[slot];
    }
}

bool pollSide()
{
    bool any = false;
    for (;;)
    {
        SideRec r;
        {
            std::lock_guard<std::mutex> lock(gSideMutex);
            if (gSide.empty() || gSide.front().mAfter > gHead.load(std::memory_order_relaxed))
            {
                break;          // not yet: the main thread queued work before this that has to run first
            }
            r = std::move(gSide.front());
            gSide.pop_front();
            gSideCount.fetch_sub(1);
        }
        std::vector<U8>& pl = r.mPayload;
        pl.resize(pl.size() + 16);      // slack so a reader never walks off the end of a short payload
        execute(r.mOp, r.mCtx, 0, pl.data());
        any = true;
    }
    return any;
}

void idleWait()
{
    const long long idle_start = now_ticks();
    struct Timer { long long mStart; ~Timer() { std::lock_guard<std::mutex> lock(gPaceMutex); gWorkerIdleTicks += now_ticks() - mStart; } } idle_timer = { idle_start };
    for (int i = 0; i < 300; ++i)
    {
        YieldProcessor();
        if (workAvailable()) return;
    }
    SwitchToThread();
    if (workAvailable()) return;
    gSleeping.store(1);
    long undesired = 0;
    InterlockedExchange(&gWakeWord, 0);
    if (!workAvailable())
    {
        WaitOnAddress(&gWakeWord, &undesired, sizeof(long), 1);
    }
    gSleeping.store(0);
}

LONG crash_filter(EXCEPTION_POINTERS* info)
{
    // written with plain file calls: the process is about to die and the log system may not be safe to use
    const char* path = "D:\\claude\\vk-crash.txt";
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        const U64 base_addr = (U64)GetModuleHandleA(nullptr);
        char line[256];
        auto out = [&](const char* s) { DWORD w; WriteFile(f, s, (DWORD)strlen(s), &w, nullptr); };
        snprintf(line, sizeof(line), "exception 0x%08lX at exe+0x%llX, access address 0x%llX (%s)\r\n", info->ExceptionRecord->ExceptionCode,
                 (unsigned long long)((U64)info->ExceptionRecord->ExceptionAddress - base_addr),
                 (unsigned long long)(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0),
                 info->ExceptionRecord->NumberParameters > 0 && info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        out(line);
        snprintf(line, sizeof(line), "worker was replaying command %u of context %u; queue head %llu tail %llu; draw path breadcrumb %d (0..17 apply phases, 200+ prepare_draw, 210 before apply, 211 apply)\r\n",
                 (unsigned)gCurrentOp, (unsigned)gCurrentCtx, (unsigned long long)gHead.load(), (unsigned long long)gTail.load(), (int)LLVkMirror::gLastLap);
        out(line);
        snprintf(line, sizeof(line), "threads that used the Vulkan layer since the worker started: %u (", (unsigned)LLVkMirror::gRendererTidCount);
        out(line);
        for (unsigned t = 0; t < 8 && t < LLVkMirror::gRendererTidCount; ++t)
        {
            snprintf(line, sizeof(line), "%lu ", (unsigned long)LLVkMirror::gRendererTids[t]);
            out(line);
        }
        snprintf(line, sizeof(line), "); main thread %lu, worker thread %lu\r\n", gMainTid, gWorkerTid);
        out(line);
        snprintf(line, sizeof(line), "debug values: mProgram 0x%llX, slot %llu, cmd 0x%llX, ring mapped 0x%llX\r\n", LLVkMirror::gDbg[0], LLVkMirror::gDbg[1], LLVkMirror::gDbg[2], LLVkMirror::gDbg[3]);
        out(line);
        void* frames[40] = {};
        const USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);
        // names and lines from the .pdb next to the exe, looked up inside the process
        HANDLE proc = GetCurrentProcess();
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        char* slash = strrchr(exe_path, '\\');
        if (slash) *slash = 0;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        const bool have_syms = SymInitialize(proc, exe_path, TRUE) != FALSE;
        alignas(8) char sym_buf[sizeof(SYMBOL_INFO) + 512] = {};
        for (USHORT i = 0; i < n; ++i)
        {
            const char* name = "?";
            DWORD64 disp = 0;
            DWORD line_disp = 0;
            IMAGEHLP_LINE64 li = {};
            li.SizeOfStruct = sizeof(li);
            char where[200] = "";
            if (have_syms)
            {
                SYMBOL_INFO* si = (SYMBOL_INFO*)sym_buf;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 500;
                if (SymFromAddr(proc, (DWORD64)frames[i], &disp, si))
                {
                    name = si->Name;
                }
                if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &line_disp, &li))
                {
                    snprintf(where, sizeof(where), " (%s:%lu)", li.FileName, li.LineNumber);
                }
            }
            snprintf(line, sizeof(line), "frame %u: exe+0x%llX %s+0x%llX%s\r\n", (unsigned)i, (unsigned long long)((U64)frames[i] - base_addr), name, (unsigned long long)disp, where);
            out(line);
        }
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void run_loop();

DWORD WINAPI workerMain(LPVOID)
{
    __try
    {
        run_loop();
    }
    __except (crash_filter(GetExceptionInformation()))
    {
    }
    return 0;
}

// plays every record in the ring (and the other threads' records that are due); returns true if it ran the stop command
bool process_ring(bool& did, U32& count)
{
    bool stopping = false;
    U64 head = gHead.load(std::memory_order_relaxed);
    const U64 tail = gTail.load(std::memory_order_acquire);
    while (head != tail)
    {
        const size_t pos = (size_t)(head % RING_BYTES);
        RecHeader h;
        memcpy(&h, gRing + pos, sizeof(h));
        if (h.mOp == CMD_WRAP)
        {
            head += h.mSize;
            gHead.store(head, std::memory_order_release);
            continue;
        }
        if (h.mOp == CMD_STOP)
        {
            stopping = true;
        }
        else
        {
            execute(h.mOp, h.mCtx, h.mFlags, gRing + pos + sizeof(RecHeader));
        }
        head += h.mSize;
        gHead.store(head, std::memory_order_release);
        did = true;
        if ((++count & 63) == 0 && gSideCount.load(std::memory_order_relaxed) > 0)
        {
            pollSide();
        }
        if (stopping)
        {
            break;
        }
    }
    return stopping;
}

void drain_inline()
{
    static thread_local bool draining = false;
    if (draining)
    {
        return;         // a command being replayed queued another one: the loop below plays it
    }
    draining = true;
    bool did = false;
    U32 count = 0;
    process_ring(did, count);
    pollSide();
    draining = false;
}

void wait_drained()
{
    int spins = 0;
    while (gHead.load(std::memory_order_acquire) != gTail.load(std::memory_order_acquire))
    {
        wake();
        if (++spins < 500) YieldProcessor(); else SwitchToThread();
    }
}

void run_loop()
{
    gWorkerTid = __readgsdword(0x48);
    LLVkMirror::gMainThreadId = gWorkerTid;                 // the phase timers now measure the worker
    bool stopping = false;
    U32 count = 0;
    while (!stopping)
    {
        bool did = false;
        stopping = process_ring(did, count);
        if (stopping)
        {
            break;
        }
        if (pollSide())
        {
            did = true;
        }
        if (!did && !stopping)
        {
            idleWait();
        }
    }
    gRunning.store(false);
}

} // anonymous namespace

bool requested()
{
    static const bool on = []() {
        const char* v = getenv("LL_VK_ASYNC");
        if (!v || strcmp(v, "0") == 0) return false;        // LL_VK_ASYNC=0 forces the inline mirror whatever the setting says
        if (strcmp(v, "loopback") == 0) gMode = 2;
        else if (strcmp(v, "lockstep") == 0) gMode = 3;
        return true;
    }();
    return on;
}

bool start()
{
    if (gEnabled)
    {
        return true;
    }
    gMainTid = __readgsdword(0x48);
    { const char* ty = getenv("LL_VK_TYPED"); gTyped = !(ty && ty[0] == '0'); }
    { const char* t1 = getenv("LL_VK_TEXONECOPY"); gTexOneCopy = t1 && t1[0] == '1'; }
    if (!gRing)
    {
        gRing = (U8*)VirtualAlloc(nullptr, RING_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        gPub = new PubQuery[QUERY_TABLE];
        gWorkerGen = new unsigned long long[QUERY_TABLE]();
    }
    if (!gRing)
    {
        return false;
    }
    gHead.store(0);
    gTail.store(0);
    gBeginsEnqueued.store(0);
    gBeginsDone.store(0);
    LLVkGL::register_main_context();
    LLVkState::forceEmit();         // the receiver gets the first block of this run whatever was written before
    LLVkMirror::gRendererTidCount = 0;
    LLVkMirror::gAsyncEpoch = LLVkMirror::gAsyncEpoch + 1;
    if (gMode == 2)
    {
        // loopback: everything is encoded into the queue, and the main thread replays it at once
        gWorkerTid = gMainTid;
        gEnabled = true;
        LL_INFOS("VkAsync") << "LL_VK_ASYNC=loopback: GL calls go through the queue and are replayed at once on the main thread (no worker thread)" << LL_ENDL;
        return true;
    }
    if (getenv("LL_VK_PHASES") || getenv("LL_VK_OPSTATS"))
    {
        gOpStatsPeriodic = getenv("LL_VK_PHASES") == nullptr;
        LARGE_INTEGER qn;
        QueryPerformanceCounter(&qn);
        gOpStatQpc0 = qn.QuadPart;
        gOpStatTsc0 = __rdtsc();
        gOpStats = true;
        gFrontStats = true;
    }
    if (getenv("LL_VK_WORKERPHASES"))
    {
        LLVkMirror::gPhaseTiming = true;        // the draw path's own phase timers (they run on the thread that replays the frames: the worker here); each costs a counter read
    }
    gRunning.store(true);
    gThread = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
    if (!gThread)
    {
        gRunning.store(false);
        return false;
    }
    gEnabled = true;
    LL_INFOS("VkAsync") << (gMode == 3 ? "LL_VK_ASYNC=lockstep: the Vulkan mirror runs on its own thread, the main thread waits for it at the end of every frame"
                                       : "the Vulkan mirror runs on its own thread (LL_VK_ASYNC): GL calls are queued by the main thread") << LL_ENDL;
    return true;
}

void stop()
{
    if (gMode == 2)
    {
        gEnabled = false;
        return;
    }
    if (!gEnabled || __readgsdword(0x48) == gWorkerTid)
    {
        return;         // not running, or called from the worker itself (a failure inside it)
    }
    push(CMD_STOP, nullptr, 0);
    wake();
    if (gThread)
    {
        WaitForSingleObject(gThread, 10000);
        CloseHandle(gThread);
        gThread = nullptr;
    }
    gEnabled = false;
    LLVkMirror::gMainThreadId = __readgsdword(0x48);
    LL_INFOS("VkAsync") << "the Vulkan mirror thread stopped" << LL_ENDL;
}

// ---- F3: sub-streams ----
struct SubStream::Impl
{
    SubBuf mBuf;
    Pend mPend;
    LLVkState::Context* mState = nullptr;
    LLVkState::Context* mSeed = nullptr;            // the main producer's render state when the sub-stream was made
    std::string mLeft;                              // what the work left changed in the render state
    DrawCtx mSeedDraw;                              // F2.4: the main producer's program and vertex binding when the sub-stream was made
};

SubStream::SubStream() : mImpl(new Impl)
{
    mImpl->mSeed = LLVkState::cloneContext();       // the main producer's state now: where the sub-stream's work starts from
    mImpl->mSeedDraw = gMainPend.mDraw;
}

SubStream::~SubStream()
{
    if (mImpl->mState)
    {
        LLVkState::destroyContext(mImpl->mState);
    }
    if (mImpl->mSeed)
    {
        LLVkState::destroyContext(mImpl->mSeed);
    }
    delete mImpl;
}

void SubStream::begin()
{
    Impl& s = *mImpl;
    if (s.mState)
    {
        LLVkState::destroyContext(s.mState);
    }
    s.mState = LLVkState::cloneContext(s.mSeed);
    s.mBuf.mBuf.clear();
    s.mBuf.mCur = 0;
    s.mPend = Pend();
    s.mPend.mDraw = s.mSeedDraw;
    s.mPend.mDraw.mSent = (void*)(intptr_t)-1;
    s.mPend.mDraw.mDirty = true;                    // the first draw of the sub-stream carries its binding: nothing is assumed about what the worker holds
    tlsBuf = &s.mBuf;
    tlsPend = &s.mPend;
    LLVkState::bindContext(s.mState);
}

void SubStream::end()
{
    coFlushAll();                                   // the pending slots and uniform writes of the producer are records too
    mImpl->mLeft = LLVkState::describeDifference(LLVkState::blockOf(mImpl->mState), LLVkState::blockOf(mImpl->mSeed));
    tlsBuf = nullptr;
    tlsPend = nullptr;
    LLVkState::bindContext(nullptr);
}

void SubStream::splice()
{
    Impl& s = *mImpl;
    coFlushAll();                                   // the main producer's own pending writes come first: they would have been folded into the next draw
    const U8* base = s.mBuf.mBuf.data();
    const size_t n = s.mBuf.mBuf.size();
    for (size_t off = 0; off < n; )
    {
        RecHeader h;
        memcpy(&h, base + off, sizeof(h));
        const size_t payload = h.mSize - sizeof(RecHeader);
        U8* w = mainReserve(payload, h.mOp, h.mFlags);
        memcpy(w, base + off + sizeof(RecHeader), payload);
        mainCommit(payload);
        ++gMainRecords;
        off += h.mSize;
    }
    s.mBuf.mBuf.clear();
    s.mBuf.mCur = 0;
    if (!s.mLeft.empty())
    {
        static std::set<std::string> said;
        if (said.insert(s.mLeft).second)
        {
            LL_INFOS("VkSub") << "a sub-stream left the render state changed: " << s.mLeft << "(the main producer takes it over)" << LL_ENDL;
        }
    }
    LLVkState::adoptChanges(s.mSeed, s.mState);     // what the work changed in the render state is the state the main producer goes on with
    LLVkState::forceEmit();                         // what the worker holds is the sub-stream's last state, not what the main producer last wrote
    gMainPend.mDraw = s.mPend.mDraw;                // the program and binding the viewer's classes believe are bound are the sub-stream's last (the beliefs are shared statics)
    gMainPend.mDraw.mDirty = true;
}

U32 SubStream::records() const
{
    U32 count = 0;
    const U8* base = mImpl->mBuf.mBuf.data();
    for (size_t off = 0; off < mImpl->mBuf.mBuf.size(); ++count)
    {
        RecHeader h;
        memcpy(&h, base + off, sizeof(h));
        off += h.mSize;
    }
    return count;
}

} // namespace LLVkAsync

bool LLVkZones::available()
{
    return LLVkAsync::mainProducer();
}

void LLVkZones::zone(const char* name, bool begin)
{
    LLVkAsync::gpuZone(name, begin);
}

#include "llvksubstream.h"

bool LLVkSubStream::usable()
{
    return LLVkAsync::gEnabled && LLVkAsync::mainProducer();
}

LLVkSubStream::LLVkSubStream() : mImpl(new LLVkAsync::SubStream()) {}
LLVkSubStream::~LLVkSubStream() { delete (LLVkAsync::SubStream*)mImpl; }
void LLVkSubStream::begin() { ((LLVkAsync::SubStream*)mImpl)->begin(); }
void LLVkSubStream::end() { ((LLVkAsync::SubStream*)mImpl)->end(); }
void LLVkSubStream::splice() { ((LLVkAsync::SubStream*)mImpl)->splice(); }
unsigned LLVkSubStream::records() const { return ((LLVkAsync::SubStream*)mImpl)->records(); }

#endif // LL_VULKAN
