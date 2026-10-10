/**
 * @file llvkasync.cpp
 * @brief The command queue and worker thread that run the Vulkan mirror off the viewer's main thread (see llvkasync.h).
 */

#include "linden_common.h"

#if defined(LL_VULKAN)

#include "llvkasync.h"
#include "llglheaders.h"
#include "llvkgl.h"             // the generated GL command numbers and enq_ declarations (with the intercepting macros)
#include "llvkmirror.h"
#include "llvkzones.h"
#include "llvkrenderer.h"
#include "llvkstate.h"
#include "llvktex.h"
#include "llvkbuf.h"

#include <windows.h>
#include <synchapi.h>
#include <dbghelp.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <algorithm>
#include <intrin.h>
#include <vector>

#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "dbghelp.lib")

namespace LLVkGL
{
    void set_worker_context(int id);
    void register_main_context();
    void on_bufferData(GLuint name, GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void on_bufferSubData(GLuint name, GLintptr offset, GLsizeiptr size, const void* data);
    void on_bufferRecycle(GLuint name);
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
static_assert(OP_GL_COUNT == LLVkGL::HOOK_COUNT, "the worker statistics name an operation by its place in the hook table");
constexpr int OP_STAT_SLOTS = OP_GL_COUNT + 40;
bool gOpStats = false;
bool gOpStatsPeriodic = false;      // LL_VK_OPSTATS without LL_VK_PHASES: the worker logs the numbers itself every 600 presented frames
unsigned gOpStatsFrames = 0;
unsigned long long gOpStatTicks[OP_STAT_SLOTS] = {};
unsigned long long gOpStatCalls[OP_STAT_SLOTS] = {};
unsigned long long gOpStatTsc0 = 0;
long long gOpStatQpc0 = 0;
inline int opStatSlot(unsigned op) { return op < (unsigned)OP_GL_COUNT ? (int)op : std::min((int)(OP_GL_COUNT + (op - CMD_BASE)), OP_STAT_SLOTS - 1); }
const char* const kCmdNames[] = { "base", "frameBegin", "frameEnd", "present", "hold", "snapshotBoundary", "upscale", "programBind", "uniformScalar",
                                  "uniformVector", "uniformMatrix", "samplerUnit", "programDestroy", "sync", "stop", "wrap", "bindTextureUnit", "arrayRun", "bufferData", "bufferSubData", "shadowPackets", "shaderBind", "texParams", "fboAttach", "targetState", "texSlots", "draw", "gpuZone", "uniformBatch", "bufferRecycle", "rtScene", "state", "texImage", "texParamsId", "texOp", "uniformBlock", "constAttrib" };
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
unsigned long long gMainRecords = 0;        // records actually written by the main thread

struct BindTexUnitArgs { GLenum mUnit; GLenum mTarget; GLuint mTexture; U32 mPad; };
struct RunHeader { GLenum mTarget; GLuint mBuffer; U32 mCount; U32 mFlags; GLuint mElem; U32 mPad; };   // flags: bit 0 array buffer bind, bit 1 index buffer bind (played after the pointers)
struct RunEntry { U32 mKind; GLuint mIndex; GLint mSize; GLenum mType; GLsizei mStride; U32 mNormalized; const void* mPointer; };   // kind 0: glVertexAttribPointer, 1: glVertexAttribIPointer
static_assert(sizeof(RunEntry) == 32, "run entry");
constexpr GLenum GL_ARRAY_BUFFER_ENUM = 0x8892;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER_ENUM = 0x8893;

inline void rawPush(U16 op, const void* args, unsigned size)
{
    U8* p = mainReserve(size, op, 0);
    if (size)
    {
        memcpy(p, args, size);
    }
    mainCommit(size);
    ++gMainRecords;
}

// <FS> draw-record plan step 1: texture binds of LLTexUnit do not write a record each. The producer keeps the slot table (unit -> target, name) and the last
// active unit; what changed since the last draw is written with that draw (CMD_DRAW) or, when something that can read texture state comes first, as CMD_TEX_SLOTS.
struct PendTex { U32 mTarget; U32 mName; };
inline U32 lowBit(U32 m) { unsigned long i; _BitScanForward(&i, m); return (U32)i; }
constexpr U32 TEX_UNIT_BASE = 0x84C0;       // GL_TEXTURE0
PendTex gPendTex[32];
U32 gPendMask = 0;
U32 gLatestActive = TEX_UNIT_BASE;          // the active unit as the producer sees it (the worker's after everything queued)

// the entries of the pending slots in unit order, appended to p; returns the bytes
U32 pendTexWrite(U8* p)
{
    U32 n = 0;
    for (U32 m = gPendMask; m; m &= m - 1)
    {
        const U32 u = lowBit(m);
        memcpy(p + n * sizeof(PendTex), &gPendTex[u], sizeof(PendTex));
        ++n;
    }
    return n * (U32)sizeof(PendTex);
}

unsigned long long gTexFlushWhy[OP_STAT_SLOTS] = {};     // LL_VK_OPSTATS: what forced the pending texture slots out as a record of their own (slot of the operation)

void flushPendingTex(int why = 0)
{
    if (!gPendMask)
    {
        return;
    }
    ++gTexFlushWhy[why];
    const U32 count = (U32)__popcnt(gPendMask);
    const U32 payload = (U32)(sizeof(U32) * 3 + sizeof(PendTex) * count);
    U8* p = mainReserve(payload, CMD_TEX_SLOTS, 0);
    const U32 head[3] = { gPendMask, gLatestActive, count };
    memcpy(p, head, sizeof(head));
    pendTexWrite(p + sizeof(head));
    mainCommit(payload);
    ++gMainRecords;
    gPendMask = 0;
}

// <FS> draw-record plan: loose uniform writes (scalar, vector, matrix) of the main thread are not records of their own either. They pile up here as
// {op, payload bytes, payload} entries (the argument struct and the values, padded to 8) and are written inside the next draw record. Anything that can
// need them earlier flushes them first (every operation that is not independent of them goes through coFlushAll).
struct UniformScalarArgs { void* mProgram; unsigned mIndex; int mKind; unsigned mN; float mF[4]; int mI[4]; };
std::vector<U8> gPendUni;

void flushPendingUni(int why = 0)
{
    (void)why;
    if (gPendUni.empty())
    {
        return;
    }
    const U32 bytes = (U32)gPendUni.size();
    const U32 payload = (U32)(sizeof(U32) * 2 + bytes);
    U8* p = mainReserve(payload, CMD_UNIFORM_BATCH, 0);
    const U32 head[2] = { bytes, 0 };
    memcpy(p, head, sizeof(head));
    memcpy(p + sizeof(head), gPendUni.data(), bytes);
    mainCommit(payload);
    ++gMainRecords;
    gPendUni.clear();
}

inline void uniAppend(U32 op, const void* args, size_t asz, const void* data, size_t dsz)
{
    const size_t payload = (asz + dsz + 7) & ~(size_t)7;
    const size_t off = gPendUni.size();
    gPendUni.resize(off + 8 + payload);
    const U32 head[2] = { op, (U32)payload };
    memcpy(&gPendUni[off], head, 8);
    memcpy(&gPendUni[off + 8], args, asz);
    if (dsz)
    {
        memcpy(&gPendUni[off + 8 + asz], data, dsz);
    }
    if (gPendUni.size() > (1u << 20))
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
    case OP_glClear: case OP_glClearColor:
    case CMD_CONST_ATTRIB: case CMD_BUFFER_DATA: case CMD_BUFFER_SUBDATA: case CMD_BUFFER_RECYCLE: case CMD_GPU_ZONE:
    case OP_glBeginQuery: case OP_glEndQuery: case OP_glUseProgram:
    case CMD_UNIFORM_SCALAR: case CMD_UNIFORM_VECTOR: case CMD_UNIFORM_MATRIX: case CMD_SAMPLER_UNIT: case CMD_PROGRAM_BIND: case CMD_SHADER_BIND:
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
    if (isMainThread())
    {
        if (op == OP_glClear)
        {
            stateEmit();
        }
        const bool timed = gOpStats && ((++gMainPushOps & 15) == 0);
        const unsigned long long t0 = timed ? __rdtsc() : 0;
        if (gOpStats)
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
    MainLap() : mTimed(gOpStats && ((++gMainPushOps & 15) == 0)), mT0(mTimed ? __rdtsc() : 0) {}
    ~MainLap() { if (mTimed) { gMainPushTicks += (__rdtsc() - mT0) * 16; } }
};

bool mainProducer()
{
    return gEnabled && gTyped && isMainThread();
}

void framebufferAttach(U32 fbo, U32 attachment, U32 textarget, U32 texture, U32 restore_fbo)
{
    MainLap lap;
    coFlushAll(opStatSlot(CMD_FBO_ATTACH));
    const U32 a[5] = { fbo, attachment, textarget, texture, restore_fbo };
    rawPush(CMD_FBO_ATTACH, a, sizeof(a));
}

void targetState(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh)
{
    MainLap lap;
    coFlushAll(opStatSlot(CMD_TARGET_STATE));
    if (isMainThread())
    {
        LLVkState::setViewport(vx, vy, vw, vh);
    }
    const S32 a[6] = { (S32)fbo, draw_buffers, vx, vy, vw, vh };
    rawPush(CMD_TARGET_STATE, a, sizeof(a));
}

void shaderBind(void* program, U32 enable_mask, U32 disable_mask)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::shader_bind(program, enable_mask, disable_mask);
        return;
    }
    if (!isMainThread())
    {
        return;
    }
    MainLap lap;
    struct A { void* mProgram; U32 mEnable; U32 mDisable; } a = { program, enable_mask, disable_mask };
    rawPush(CMD_SHADER_BIND, &a, sizeof(a));
}

void bindTextureUnit(U32 unit, U32 target, U32 texture)
{
    MainLap lap;
    const U32 u = unit - TEX_UNIT_BASE;
    if (u >= 32)
    {
        return;
    }
    // the slot table is the record: nothing is written here, what changed goes out with the next draw (or when something that can read texture state comes first)
    gPendTex[u] = { target, texture };
    gPendMask |= 1u << u;
}

namespace
{
    struct NamedBufArgs { U32 mName; U32 mTarget; U32 mUsage; U32 mPad; S64 mOffset; S64 mSize; };
}

void bufferData(U32 name, U32 target, S64 size, const void* data, U32 usage)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::on_bufferData(name, target, (GLsizeiptr)size, data, usage);
        return;
    }
    MainLap lap;
    const NamedBufArgs a = { name, target, usage, 0, 0, size };
    pushData(CMD_BUFFER_DATA, &a, sizeof(a), data, data ? (size_t)std::max<S64>(size, 0) : 0);
}

void bufferSubData(U32 name, S64 offset, S64 size, const void* data)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::on_bufferSubData(name, (GLintptr)offset, (GLsizeiptr)size, data);
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

// a draw record: the draw and the texture slots that changed since the last one (CMD_DRAW), or the plain draw call record when none changed
namespace
{
    struct DrawHeader { U32 mKind; U32 mMode; U32 mStart; U32 mEnd; S32 mCount; U32 mType; const void* mIndices; U32 mMask; U32 mActive; U32 mCountTex; U32 mUniBytes; };
}

// the draw and the texture slots / uniform writes that changed since the last one, as one record
static void writeDraw(U32 kind, U32 mode, U32 start, U32 end, S32 count, U32 type, const void* indices)
{
    MainLap lap;
    stateEmit();
    const U32 n = (U32)__popcnt(gPendMask);
    const U32 uni = (U32)gPendUni.size();
    const U32 payload = (U32)(sizeof(DrawHeader) + sizeof(PendTex) * n + uni);
    U8* p = mainReserve(payload, CMD_DRAW, 0);
    const DrawHeader h = { kind, mode, start, end, count, type, indices, gPendMask, gLatestActive, n, uni };
    memcpy(p, &h, sizeof(h));
    pendTexWrite(p + sizeof(h));
    if (uni)
    {
        memcpy(p + sizeof(h) + sizeof(PendTex) * n, gPendUni.data(), uni);
    }
    mainCommit(payload);
    ++gMainRecords;
    gPendMask = 0;
    gPendUni.clear();
}

void drawRangeElements(U32 mode, U32 start, U32 end, S32 count, U32 type, const void* indices)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts): the draw is made here
        LLVkGL::draw_elements(mode, start, end, count, type, indices);
        return;
    }
    if (!isMainThread())
    {
        return;     // draws are the main thread's
    }
    writeDraw(0, mode, start, end, count, type, indices);
}

void drawArrays(U32 mode, S32 first, S32 count)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);
        LLVkGL::draw_arrays(mode, first, count);
        return;
    }
    if (!isMainThread())
    {
        return;
    }
    writeDraw(1, mode, (U32)first, 0, count, 0, nullptr);
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

void setArrayState(bool bind_array, U32 array_buffer, bool bind_elem, U32 elem_buffer, const VertexAttrib* attribs, U32 count)
{
    if (!gEnabled)
    {
        std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);       // inline mode (before the queue starts)
        LLVkGL::vertex_input_apply(bind_array, array_buffer, bind_elem, elem_buffer, attribs, count);
        return;
    }
    if (!isMainThread())
    {
        return;
    }
    MainLap lap;
    static_assert(sizeof(VertexAttrib) == sizeof(RunEntry), "typed attribute record matches the run entry");
    const U32 payload = (U32)(sizeof(RunHeader) + sizeof(RunEntry) * count);       // the array state does not depend on texture state: the pending slots stay
    U8* p = mainReserve(payload, CMD_ARRAY_RUN, 0);
    const RunHeader h = { GL_ARRAY_BUFFER_ENUM, array_buffer, count, (bind_array ? 1u : 0u) | (bind_elem ? 2u : 0u), elem_buffer, 0 };
    memcpy(p, &h, sizeof(h));
    if (count)
    {
        memcpy(p + sizeof(h), attribs, sizeof(RunEntry) * count);
    }
    mainCommit(payload);
    ++gMainRecords;
}

void pushData(unsigned short op, const void* args, unsigned size, const void* data, size_t data_size)
{
    if (isMainThread())
    {
        const bool timed = gOpStats && ((++gMainPushOps & 15) == 0);
        const unsigned long long t0 = timed ? __rdtsc() : 0;
        struct Lap { const bool mTimed; const unsigned long long mT0; ~Lap() { if (mTimed) { gMainPushTicks += (__rdtsc() - mT0) * 16; } } } lap = { timed, t0 };
        if (gOpStats)
        {
            ++gMainOpCalls[opStatSlot(op)];
        }
        if (op == CMD_SHADOW_PACKETS)
        {
            stateEmit();
        }
        if (!texIndependent(op))
        {
            coFlushAll(opStatSlot(op));
        }
        ++gMainRecords;
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
            const char* name = slot < OP_GL_COUNT ? LLVkGL::hook_name(slot) : (slot - OP_GL_COUNT < (int)(sizeof(kCmdNames) / sizeof(kCmdNames[0])) ? kCmdNames[slot - OP_GL_COUNT] : "cmd?");
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
            const char* name = slot == 0 ? "other" : (slot < OP_GL_COUNT ? LLVkGL::hook_name(slot) : (slot - OP_GL_COUNT < (int)(sizeof(kCmdNames) / sizeof(kCmdNames[0])) ? kCmdNames[slot - OP_GL_COUNT] : "cmd?"));
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
        const char* name = r.mSlot < OP_GL_COUNT ? LLVkGL::hook_name(r.mSlot) : (r.mSlot - OP_GL_COUNT < (int)(sizeof(kCmdNames) / sizeof(kCmdNames[0])) ? kCmdNames[r.mSlot - OP_GL_COUNT] : "cmd?");
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

void programBind(void* program) { push(CMD_PROGRAM_BIND, &program, sizeof(program)); }
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

void gen_enq(unsigned short op, int kind, S32 n, unsigned* out)
{
    LLVkGL::own_gen(kind, n, out);
    pushData(op, &n, sizeof(n), out, (size_t)std::max<S32>(n, 0) * 4);
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
            case 0: *(GLint*)params = 1; break;
            case 1: *(GLuint*)params = 1; break;
            default: *(GLuint64*)params = 1; break;
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
    case 0: *(GLint*)params = (GLint)value; break;
    case 1: *(GLuint*)params = (GLuint)value; break;
    default: *(GLuint64*)params = value; break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// hand-written encoders for the calls that carry data or keep main-side state
// ---------------------------------------------------------------------------------------------------------------------------------

namespace
{

size_t pixel_bytes(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei d, const PixelStoreShadow& ps)
{
    unsigned comps = 0, tb = 0;
    switch (format)
    {
    case 0x1908: case 0x80E1: comps = 4; break;                     // RGBA, BGRA
    case 0x1907: case 0x80E0: comps = 3; break;                     // RGB, BGR
    case 0x190A: case 0x8227: comps = 2; break;                     // LUMINANCE_ALPHA, RG
    case 0x1906: case 0x1909: case 0x1903: case 0x1902: comps = 1; break;   // ALPHA, LUMINANCE, RED, DEPTH_COMPONENT
    default: break;
    }
    switch (type)
    {
    case 0x1401: case 0x1400: tb = 1; break;
    case 0x1403: case 0x1402: case 0x140B: tb = 2; break;
    case 0x1405: case 0x1404: case 0x1406: tb = 4; break;
    default: break;
    }
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
    GLenum mTarget; GLint mLevel; GLint mInternalFormat;
    GLsizei mWidth, mHeight, mDepth;
    GLint mX, mY, mZ;
    GLenum mFormat, mType;
    GLint mBorder; GLsizei mImageSize;
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

void enq_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)
{
    TexMainLap lap;
    TexArgs a = { target, level, internalformat, width, height, 1, 0, 0, 0, format, type, border, 0, 0, 0, nullptr, nullptr };
    push_tex(OP_glTexImage2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}
void enq_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)
{
    TexMainLap lap;
    TexArgs a = { target, level, internalformat, width, height, depth, 0, 0, 0, format, type, border, 0, 0, 0, nullptr, nullptr };
    push_tex(OP_glTexImage3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}
void enq_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    TexMainLap lap;
    TexArgs a = { target, level, 0, width, height, 1, xoffset, yoffset, 0, format, type, 0, 0, 0, 0, nullptr, nullptr };
    push_tex(OP_glTexSubImage2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}
void enq_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    TexMainLap lap;
    TexArgs a = { target, level, 0, width, height, depth, xoffset, yoffset, zoffset, format, type, 0, 0, 0, 0, nullptr, nullptr };
    push_tex(OP_glTexSubImage3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}
void enq_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data)
{
    TexMainLap lap;
    TexArgs a = { target, level, (GLint)internalformat, width, height, 1, 0, 0, 0, 0, 0, border, imageSize, 0, 0, nullptr, nullptr };
    push_tex(OP_glCompressedTexImage2D, a, data, (size_t)std::max(imageSize, 0));
}


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
    glGenTextures(count, ids);
}

void destroy(S32 count, const U32* ids)
{
    glDeleteTextures(count, ids);
}

void setUnpackAlignment(S32 alignment)
{
    tlsPixelStore.mUnpackAlignment = alignment;
}

void setUnpackRowLength(S32 row_length)
{
    tlsPixelStore.mUnpackRowLength = row_length;
}

void bindSlot(S32 unit, U32 target, U32 id)
{
    if (LLVkAsync::gEnabled)
    {
        if (isMainThread())
        {
            LLVkAsync::bindTextureUnit(TEX_UNIT_BASE + (U32)unit, target, id);
        }
        return;
    }
    auto lock = inlineLock();
    LLVkGL::slot_bind((unsigned)unit, target, id);
}

void image2D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 border, U32 format, U32 type, const void* pixels)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_image2D(id, currentPS(), target, level, internal_format, width, height, border, format, type, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = { target, level, internal_format, width, height, 1, 0, 0, 0, format, type, border, 0, 0, 0, nullptr, nullptr, id };
    pushImage(K_IMAGE2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}

void image3D(U32 id, U32 target, S32 level, S32 internal_format, S32 width, S32 height, S32 depth, S32 border, U32 format, U32 type, const void* pixels)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_image3D(id, currentPS(), target, level, internal_format, width, height, depth, border, format, type, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = { target, level, internal_format, width, height, depth, 0, 0, 0, format, type, border, 0, 0, 0, nullptr, nullptr, id };
    pushImage(K_IMAGE3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}

void subImage2D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 width, S32 height, U32 format, U32 type, const void* pixels)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_subimage2D(id, currentPS(), target, level, x, y, width, height, format, type, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = { target, level, 0, width, height, 1, x, y, 0, format, type, 0, 0, 0, 0, nullptr, nullptr, id };
    pushImage(K_SUB2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}

void subImage3D(U32 id, U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format, U32 type, const void* pixels)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_subimage3D(id, currentPS(), target, level, x, y, z, width, height, depth, format, type, pixels);
        return;
    }
    TexMainLap lap;
    TexArgs a = { target, level, 0, width, height, depth, x, y, z, format, type, 0, 0, 0, 0, nullptr, nullptr, id };
    pushImage(K_SUB3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}

void compressedImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 width, S32 height, S32 border, S32 image_size, const void* data)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_compressed2D(id, currentPS(), target, level, internal_format, width, height, border, image_size, data);
        return;
    }
    TexMainLap lap;
    TexArgs a = { target, level, (S32)internal_format, width, height, 1, 0, 0, 0, 0, 0, border, image_size, 0, 0, nullptr, nullptr, id };
    pushImage(K_COMPRESSED2D, a, data, (size_t)std::max(image_size, 0));
}

void params(U32 id, U32 target, const LLVkAsync::TexParam* p, U32 count)
{
    if (!count)
    {
        return;
    }
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_params(id, target, p, count);
        return;
    }
    const U32 head[3] = { id, target, count };
    pushData(CMD_TEX_PARAMS_ID, head, sizeof(head), p, sizeof(LLVkAsync::TexParam) * count);
}

static void pushOp(const TexOp& op)
{
    push(CMD_TEX_OP, &op, sizeof(op));
}

void swizzle(U32 id, U32 target, const S32* rgba)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_swizzle(id, target, rgba);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_SWIZZLE; op.mTex = id; op.mTarget = target;
    memcpy(op.mRgba, rgba, sizeof(op.mRgba));
    pushOp(op);
}

void generateMipmap(U32 id, U32 target)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_mipmap(id, target);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_MIPMAP; op.mTex = id; op.mTarget = target;
    pushOp(op);
}

void copySubImage(U32 id, U32 target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_copy_sub(id, target, level, xoffset, yoffset, zoffset, x, y, width, height);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_COPY_SUB; op.mTex = id; op.mTarget = target; op.mLevel = level;
    op.mX0 = xoffset; op.mY0 = yoffset; op.mZ0 = zoffset; op.mX = x; op.mY = y; op.mW = width; op.mH = height;
    pushOp(op);
}

void copyImage2D(U32 id, U32 target, S32 level, U32 internal_format, S32 x, S32 y, S32 width, S32 height, S32 border)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_copy_image(id, target, level, internal_format, x, y, width, height, border);
        return;
    }
    TexOp op = {};
    op.mKind = OPK_COPY_IMAGE; op.mTex = id; op.mTarget = target; op.mLevel = level; op.mFormat = internal_format;
    op.mX = x; op.mY = y; op.mW = width; op.mH = height; op.mBorder = border;
    pushOp(op);
}

S32 levelParameter(U32 id, U32 target, S32 level, U32 pname)
{
    S32 v = 0;
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        return LLVkGL::tex_level_parameter(id, target, level, pname);
    }
    LLVkAsync::syncCall([&]() { v = LLVkGL::tex_level_parameter(id, target, level, pname); });
    return v;
}

void getImage(U32 id, U32 target, S32 level, U32 format, U32 type, void* out)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_get_image(id, target, level, format, type, out);
        return;
    }
    LLVkAsync::syncCall([&]() { LLVkGL::tex_get_image(id, target, level, format, type, out); });
}

void getCompressedImage(U32 id, U32 target, S32 level, void* out)
{
    if (!LLVkAsync::gEnabled)
    {
        auto lock = inlineLock();
        LLVkGL::tex_get_compressed_image(id, target, level, out);
        return;
    }
    LLVkAsync::syncCall([&]() { LLVkGL::tex_get_compressed_image(id, target, level, out); });
}

}

// ---- F1.3: buffers named by id (llvkbuf.h) ----
namespace LLVkBuf
{

void create(S32 count, U32* ids)
{
    glGenBuffers(count, ids);
}

void destroy(S32 count, const U32* ids)
{
    glDeleteBuffers(count, ids);
}

void setData(U32 id, U32 target, S64 size, const void* data, U32 usage)
{
    LLVkAsync::bufferData(id, target, size, data, usage);
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

void enq_glPixelStorei(GLenum pname, GLint param)
{
    if (pname == 0x0CF5) tlsPixelStore.mUnpackAlignment = param;        // UNPACK_ALIGNMENT
    else if (pname == 0x0CF2) tlsPixelStore.mUnpackRowLength = param;   // UNPACK_ROW_LENGTH
    struct A { GLenum pname; GLint param; } a = { pname, param };
    push(OP_glPixelStorei, &a, sizeof(a));
}

static void enq_ids(unsigned short op, GLsizei n, const GLuint* ids)
{
    pushData(op, &n, sizeof(n), ids, (size_t)std::max(n, 0) * 4);
}
void enq_glDeleteTextures(GLsizei n, const GLuint* textures) { enq_ids(OP_glDeleteTextures, n, textures); }
void enq_glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) { enq_ids(OP_glDeleteFramebuffers, n, framebuffers); }
void enq_glDeleteBuffers(GLsizei n, const GLuint* buffers) { enq_ids(OP_glDeleteBuffers, n, buffers); }
void enq_glDeleteQueries(GLsizei n, const GLuint* ids) { enq_ids(OP_glDeleteQueries, n, ids); }
void enq_glDrawBuffers(GLsizei n, const GLenum* bufs) { pushData(OP_glDrawBuffers, &n, sizeof(n), bufs, (size_t)std::max(n, 0) * 4); }
void enq_glTexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    struct A { GLenum target; GLenum pname; } a = { target, pname };
    const size_t n = (pname == 0x1004 || pname == 0x8E46) ? 4 : 1;      // BORDER_COLOR, SWIZZLE_RGBA
    pushData(OP_glTexParameteriv, &a, sizeof(a), params, params ? n * 4 : 0);
}

void enq_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)
{
    std::vector<U8> blob;
    for (GLsizei i = 0; i < count; ++i)
    {
        const GLchar* s = string ? string[i] : nullptr;
        const U32 len = !s ? 0 : (length && length[i] >= 0) ? (U32)length[i] : (U32)strlen(s);
        const size_t at = blob.size();
        blob.resize(at + sizeof(U32) + len);
        memcpy(blob.data() + at, &len, sizeof(len));
        if (len)
        {
            memcpy(blob.data() + at + sizeof(len), s, len);
        }
    }
    struct A { GLuint shader; GLsizei count; } a = { shader, count };
    pushData(OP_glShaderSource, &a, sizeof(a), blob.data(), blob.size());
}
void enq_glBindAttribLocation(GLuint program, GLuint index, const GLchar* name)
{
    struct A { GLuint program; GLuint index; } a = { program, index };
    const size_t n = name ? strlen(name) + 1 : 1;
    pushData(OP_glBindAttribLocation, &a, sizeof(a), name ? name : "", n);
}
void enq_glProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length)
{
    struct A { GLuint program; GLenum format; GLsizei length; } a = { program, binaryFormat, length };
    pushData(OP_glProgramBinary, &a, sizeof(a), binary, binary ? (size_t)std::max(length, 0) : 0);
}

void enq_glBeginQuery(GLenum target, GLuint id)
{
    // occlusion queries are the ones the viewer waits for; the others (timers) are answered as before
    const bool occlusion = target == 0x8914 || target == 0x8C2F || target == 0x8D6A;
    queryMarkSync(id, !occlusion);
    struct A { GLenum target; GLuint id; U64 gen; } a = { target, id, occlusion ? queryBegin(id) : 0 };
    push(OP_glBeginQuery, &a, sizeof(a));
}
void enq_glEndQuery(GLenum target)
{
    struct A { GLenum target; } a = { target };
    push(OP_glEndQuery, &a, sizeof(a));
}
void enq_glQueryCounter(GLuint id, GLenum target)
{
    queryMarkSync(id, true);
    struct A { GLuint id; GLenum target; } a = { id, target };
    push(OP_glQueryCounter, &a, sizeof(a));
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

void run_tex(unsigned short op, Reader& r, U8 flags)
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
    struct Release { const TexArgs& mA; ~Release() { if (mA.mMode == 3 && mA.mUnref) mA.mUnref(mA.mOwner); } } release = { a };      // the held pixels are let go once the upload has read them
    switch (op)
    {
    case OP_glTexImage2D: LLVkGL::on_glTexImage2D(a.mTarget, a.mLevel, a.mInternalFormat, a.mWidth, a.mHeight, a.mBorder, a.mFormat, a.mType, pixels); break;
    case OP_glTexImage3D: LLVkGL::on_glTexImage3D(a.mTarget, a.mLevel, a.mInternalFormat, a.mWidth, a.mHeight, a.mDepth, a.mBorder, a.mFormat, a.mType, pixels); break;
    case OP_glTexSubImage2D: LLVkGL::on_glTexSubImage2D(a.mTarget, a.mLevel, a.mX, a.mY, a.mWidth, a.mHeight, a.mFormat, a.mType, pixels); break;
    case OP_glTexSubImage3D: LLVkGL::on_glTexSubImage3D(a.mTarget, a.mLevel, a.mX, a.mY, a.mZ, a.mWidth, a.mHeight, a.mDepth, a.mFormat, a.mType, pixels); break;
    default: LLVkGL::on_glCompressedTexImage2D(a.mTarget, a.mLevel, (GLenum)a.mInternalFormat, a.mWidth, a.mHeight, a.mBorder, a.mImageSize, pixels); break;
    }
}

void dispatch_gl(unsigned short op, U8 flags, const U8* payload)
{
    const U8* p = payload;       // the generated cases read their arguments from here
    Reader r{ payload };
    switch (op)
    {
#include "llvkgl_async_dispatch.inc"
    case OP_glGenTextures: case OP_glGenFramebuffers: case OP_glGenBuffers: case OP_glGenQueries:
    {
        const S32 n = r.get<S32>();
        Blob b = read_blob(r, flags);
        std::vector<GLuint> ids((size_t)std::max<S32>(n, 0));
        if (b.mData) memcpy(ids.data(), b.mData, ids.size() * 4);
        switch (op)
        {
        case OP_glGenTextures: LLVkGL::on_glGenTextures(n, ids.data()); break;
        case OP_glGenFramebuffers: LLVkGL::on_glGenFramebuffers(n, ids.data()); break;
        case OP_glGenBuffers: LLVkGL::on_glGenBuffers(n, ids.data()); break;
        default: LLVkGL::on_glGenQueries(n, ids.data()); break;
        }
        break;
    }
    case OP_glDeleteTextures: case OP_glDeleteFramebuffers: case OP_glDeleteBuffers: case OP_glDeleteQueries:
    {
        const S32 n = r.get<S32>();
        Blob b = read_blob(r, flags);
        const GLuint* ids = (const GLuint*)b.mData;
        switch (op)
        {
        case OP_glDeleteTextures: LLVkGL::on_glDeleteTextures(n, ids); break;
        case OP_glDeleteFramebuffers: LLVkGL::on_glDeleteFramebuffers(n, ids); break;
        case OP_glDeleteBuffers: LLVkGL::on_glDeleteBuffers(n, ids); break;
        default: LLVkGL::on_glDeleteQueries(n, ids); break;
        }
        break;
    }
    case OP_glDrawBuffers:
    {
        const S32 n = r.get<S32>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glDrawBuffers(n, (const GLenum*)b.mData);
        break;
    }
    case OP_glTexParameteriv:
    {
        const GLenum target = r.get<GLenum>(), pname = r.get<GLenum>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glTexParameteriv(target, pname, (const GLint*)b.mData);
        break;
    }
    case OP_glPixelStorei:
    {
        const GLenum pname = r.get<GLenum>();
        const GLint param = r.get<GLint>();
        LLVkGL::on_glPixelStorei(pname, param);
        break;
    }
    case OP_glTexImage2D: case OP_glTexImage3D: case OP_glTexSubImage2D: case OP_glTexSubImage3D: case OP_glCompressedTexImage2D:
        run_tex(op, r, flags);
        break;
    case OP_glShaderSource:
    {
        const GLuint shader = r.get<GLuint>();
        const GLsizei count = r.get<GLsizei>();
        Blob b = read_blob(r, flags);
        std::vector<const GLchar*> strings;
        std::vector<GLint> lengths;
        const U8* p = b.mData;
        for (GLsizei i = 0; i < count; ++i)
        {
            U32 len;
            memcpy(&len, p, sizeof(len));
            p += sizeof(len);
            strings.push_back((const GLchar*)p);
            lengths.push_back((GLint)len);
            p += len;
        }
        LLVkGL::on_glShaderSource(shader, count, strings.data(), lengths.data());
        break;
    }
    case OP_glBindAttribLocation:
    {
        const GLuint program = r.get<GLuint>(), index = r.get<GLuint>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glBindAttribLocation(program, index, (const GLchar*)b.mData);
        break;
    }
    case OP_glProgramBinary:
    {
        const GLuint program = r.get<GLuint>();
        const GLenum format = r.get<GLenum>();
        const GLsizei length = r.get<GLsizei>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glProgramBinary(program, format, b.mData, length);
        break;
    }
    case OP_glBeginQuery:
    {
        const GLenum t = r.get<GLenum>();
        const GLuint id = r.get<GLuint>();
        const U64 gen = r.get<U64>();
        setWorkerQueryGen(id, gen);
        LLVkGL::on_glBeginQuery(t, id);
        break;
    }
    case OP_glEndQuery: { const GLenum t = r.get<GLenum>(); LLVkGL::on_glEndQuery(t); break; }
    case OP_glQueryCounter: { const GLuint id = r.get<GLuint>(); const GLenum t = r.get<GLenum>(); LLVkGL::on_glQueryCounter(id, t); break; }
    default:
        LL_WARNS("VkAsync") << "unknown GL command " << op << LL_ENDL;
        break;
    }
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
    switch (a.mKind)
    {
    case K_IMAGE2D: LLVkGL::tex_image2D(a.mTex, ps, a.mTarget, a.mLevel, a.mInternalFormat, a.mWidth, a.mHeight, a.mBorder, a.mFormat, a.mType, pixels); break;
    case K_IMAGE3D: LLVkGL::tex_image3D(a.mTex, ps, a.mTarget, a.mLevel, a.mInternalFormat, a.mWidth, a.mHeight, a.mDepth, a.mBorder, a.mFormat, a.mType, pixels); break;
    case K_SUB2D: LLVkGL::tex_subimage2D(a.mTex, ps, a.mTarget, a.mLevel, a.mX, a.mY, a.mWidth, a.mHeight, a.mFormat, a.mType, pixels); break;
    case K_SUB3D: LLVkGL::tex_subimage3D(a.mTex, ps, a.mTarget, a.mLevel, a.mX, a.mY, a.mZ, a.mWidth, a.mHeight, a.mDepth, a.mFormat, a.mType, pixels); break;
    default: LLVkGL::tex_compressed2D(a.mTex, ps, a.mTarget, a.mLevel, (U32)a.mInternalFormat, a.mWidth, a.mHeight, a.mBorder, a.mImageSize, pixels); break;
    }
}

void runParams(Reader& r, U8 flags)
{
    const U32 id = r.get<U32>(), target = r.get<U32>(), count = r.get<U32>();
    Blob b = read_blob(r, flags);
    LLVkGL::tex_params(id, target, (const LLVkAsync::TexParam*)b.mData, count);
}

void runOp(Reader& r)
{
    const TexOp o = r.get<TexOp>();
    switch (o.mKind)
    {
    case OPK_MIPMAP: LLVkGL::tex_mipmap(o.mTex, o.mTarget); break;
    case OPK_COPY_SUB: LLVkGL::tex_copy_sub(o.mTex, o.mTarget, o.mLevel, o.mX0, o.mY0, o.mZ0, o.mX, o.mY, o.mW, o.mH); break;
    case OPK_COPY_IMAGE: LLVkGL::tex_copy_image(o.mTex, o.mTarget, o.mLevel, o.mFormat, o.mX, o.mY, o.mW, o.mH, o.mBorder); break;
    default: LLVkGL::tex_swizzle(o.mTex, o.mTarget, o.mRgba); break;
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
    case CMD_BIND_TEXTURE_UNIT:
    {
        const BindTexUnitArgs* a = (const BindTexUnitArgs*)payload;
        LLVkGL::slot_bind(a->mUnit - TEX_UNIT_BASE, a->mTarget, a->mTexture);
        break;
    }
    case CMD_ARRAY_RUN:
    {
        const RunHeader* h = (const RunHeader*)payload;
        const RunEntry* e = (const RunEntry*)(payload + sizeof(RunHeader));
        LLVkGL::vertex_input_apply((h->mFlags & 1) != 0, h->mBuffer, (h->mFlags & 2) != 0, h->mElem, (const VertexAttrib*)e, h->mCount);
        break;
    }
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
        LLVkGL::on_bufferData(a.mName, a.mTarget, (GLsizeiptr)a.mSize, b.mData, a.mUsage);
        break;
    }
    case CMD_BUFFER_SUBDATA:
    {
        const NamedBufArgs a = r.get<NamedBufArgs>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_bufferSubData(a.mName, (GLintptr)a.mOffset, (GLsizeiptr)a.mSize, b.mData);
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
        const PendTex* e = (const PendTex*)(payload + sizeof(U32) * 3);
        U32 k = 0;
        for (U32 m = head[0]; m; m &= m - 1, ++k)
        {
            LLVkGL::slot_bind(lowBit(m), e[k].mTarget, e[k].mName);
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
    {
        const DrawHeader* h = (const DrawHeader*)payload;
        const PendTex* e = (const PendTex*)(payload + sizeof(DrawHeader));
        if (h->mUniBytes)
        {
            applyUniformBatch(payload + sizeof(DrawHeader) + sizeof(PendTex) * h->mCountTex, h->mUniBytes);
        }
        U32 k = 0;
        for (U32 m = h->mMask; m; m &= m - 1, ++k)
        {
            LLVkGL::slot_bind(lowBit(m), e[k].mTarget, e[k].mName);
        }
        if (h->mKind == 0)
        {
            LLVkGL::draw_elements(h->mMode, h->mStart, h->mEnd, h->mCount, h->mType, h->mIndices);
        }
        else
        {
            LLVkGL::draw_arrays(h->mMode, (GLint)h->mStart, h->mCount);
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
        LLVkGL::on_glBindFramebuffer(0x8D40, a[0]);                     // GL_FRAMEBUFFER
        LLVkGL::on_glFramebufferTexture2D(0x8D40, a[1], a[2], a[3], 0);
        LLVkGL::on_glBindFramebuffer(0x8D40, a[4]);
        break;
    }
    case CMD_TARGET_STATE:
    {
        const S32* a = (const S32*)payload;
        LLVkGL::on_glBindFramebuffer(0x8D40, (GLuint)a[0]);
        if (a[1] > 0)
        {
            const GLenum bufs[4] = { 0x8CE0, 0x8CE1, 0x8CE2, 0x8CE3 };     // GL_COLOR_ATTACHMENT0..3
            LLVkGL::on_glDrawBuffers(std::min(a[1], 4), bufs);
            LLVkGL::on_glReadBuffer(0x8CE0);
        }
        else if (a[1] == 0)
        {
            LLVkGL::on_glDrawBuffer(0);                                  // GL_NONE
            LLVkGL::on_glReadBuffer(0);
        }
        else
        {
            LLVkGL::on_glReadBuffer(0x0405);                             // GL_BACK
            LLVkGL::on_glDrawBuffer(0x0405);
        }
        // the viewport is part of the state block (the producer sets it with this record)
        break;
    }
    case CMD_UNIFORM_BLOCK: { const U32* a = (const U32*)payload; LLVkGL::uniform_block_bind(a[0], a[1]); break; }
    case CMD_TEX_IMAGE: runImage(r, flags); break;
    case CMD_TEX_PARAMS_ID: runParams(r, flags); break;
    case CMD_TEX_OP: runOp(r); break;
    case CMD_SHADER_BIND:
    {
        struct A { void* mProgram; U32 mEnable; U32 mDisable; };
        const A* a = (const A*)payload;
        LLVkGL::shader_bind(a->mProgram, a->mEnable, a->mDisable);
        break;
    }
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
    if (op < OP_GL_COUNT)
    {
        dispatch_gl(op, flags, payload);
    }
    else
    {
        dispatch_cmd(op, flags, payload);
    }
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

} // namespace LLVkAsync

bool LLVkZones::available()
{
    return LLVkAsync::mainProducer();
}

void LLVkZones::zone(const char* name, bool begin)
{
    LLVkAsync::gpuZone(name, begin);
}

#endif // LL_VULKAN
