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
#include "llvkrenderer.h"

#include <windows.h>
#include <synchapi.h>
#include <dbghelp.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "dbghelp.lib")

namespace LLVkGL
{
    void set_worker_context(int id);
    void register_main_context();
}

namespace LLVkMirror
{
    void beginFrameImpl(S32 width, S32 height);
    void endFrameImpl();
    void presentFrameImpl();
    void holdFrameImpl(bool hold);
    void snapshotBoundaryImpl(S32 mode);
    void armSnapshotCompareImpl();
}

namespace LLVkAsync
{

bool gEnabled = false;
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

void push(unsigned short op, const void* args, unsigned size)
{
    if (isMainThread())
    {
        U8* p = mainReserve(size, op, 0);
        if (size)
        {
            memcpy(p, args, size);
        }
        mainCommit(size);
        return;
    }
    std::vector<U8> payload((const U8*)args, (const U8*)args + size);
    sidePush(op, std::move(payload));
}

void pushData(unsigned short op, const void* args, unsigned size, const void* data, size_t data_size)
{
    if (isMainThread())
    {
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

// ---------------------------------------------------------------------------------------------------------------------------------
// frame boundaries
// ---------------------------------------------------------------------------------------------------------------------------------

void frameBegin(S32 width, S32 height)
{
    // how many frames the main thread may be ahead of the worker: one keeps the frame times even (LL_VK_ASYNC_DEPTH overrides, for comparing)
    static const unsigned depth = []() { const char* v = getenv("LL_VK_ASYNC_DEPTH"); return v ? (unsigned)std::max(1, atoi(v)) : 1u; }();
    int spins = 0;
    while (gBeginsEnqueued.load(std::memory_order_relaxed) - gBeginsDone.load(std::memory_order_acquire) > depth)
    {
        wake();
        if (++spins < 200) YieldProcessor(); else SwitchToThread();
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
void armSnapshot() { push(CMD_ARM_SNAPSHOT, nullptr, 0); }

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
    push(CMD_UNIFORM_SCALAR, &a, sizeof(a));
}
void uniformVector(void* program, unsigned uniform_index, unsigned components, unsigned count, const void* data)
{
    const UniformVecArgs a = { program, uniform_index, components, count };
    pushData(CMD_UNIFORM_VECTOR, &a, sizeof(a), data, (size_t)components * count * 4);
}
void uniformMatrix(void* program, unsigned uniform_index, unsigned columns, unsigned rows, unsigned count, const float* data)
{
    const UniformMatArgs a = { program, uniform_index, columns, rows, count };
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
            gQStat[6].fetch_add(1, std::memory_order_relaxed);
            return false;           // not there yet: the worker resolves it
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
    U64 mOffset;                // the pixels pointer when the data comes from a bound pixel unpack buffer
    U32 mMode;                  // 0 no data, 1 data follows, 2 offset into the unpack buffer
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
    else
    {
        a.mMode = 1;
        pushData(op, &a, sizeof(a), pixels, bytes);
    }
}

} // anonymous namespace

void enq_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels)
{
    TexArgs a = { target, level, internalformat, width, height, 1, 0, 0, 0, format, type, border, 0, 0, 0 };
    push_tex(OP_glTexImage2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}
void enq_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels)
{
    TexArgs a = { target, level, internalformat, width, height, depth, 0, 0, 0, format, type, border, 0, 0, 0 };
    push_tex(OP_glTexImage3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}
void enq_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    TexArgs a = { target, level, 0, width, height, 1, xoffset, yoffset, 0, format, type, 0, 0, 0, 0 };
    push_tex(OP_glTexSubImage2D, a, pixels, pixel_bytes(format, type, width, height, 1, tlsPixelStore));
}
void enq_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    TexArgs a = { target, level, 0, width, height, depth, xoffset, yoffset, zoffset, format, type, 0, 0, 0, 0 };
    push_tex(OP_glTexSubImage3D, a, pixels, pixel_bytes(format, type, width, height, depth, tlsPixelStore));
}
void enq_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data)
{
    TexArgs a = { target, level, (GLint)internalformat, width, height, 1, 0, 0, 0, 0, 0, border, imageSize, 0, 0 };
    push_tex(OP_glCompressedTexImage2D, a, data, (size_t)std::max(imageSize, 0));
}

void enq_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    struct A { GLenum target; GLenum usage; S64 size; } a = { target, usage, (S64)size };
    pushData(OP_glBufferData, &a, sizeof(a), data, data ? (size_t)std::max<S64>(size, 0) : 0);
}
void enq_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    struct A { GLenum target; GLenum pad; S64 offset; S64 size; } a = { target, 0, (S64)offset, (S64)size };
    pushData(OP_glBufferSubData, &a, sizeof(a), data, data ? (size_t)std::max<S64>(size, 0) : 0);
}
void enq_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == 0x88EC)       // PIXEL_UNPACK_BUFFER
    {
        tlsPixelStore.mUnpackBuffer = buffer;
    }
    struct A { GLenum target; GLuint buffer; } a = { target, buffer };
    push(OP_glBindBuffer, &a, sizeof(a));
}
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
void enq_glVertexAttrib4fv(GLuint index, const GLfloat* v)
{
    struct A { GLuint index; } a = { index };
    pushData(OP_glVertexAttrib4fv, &a, sizeof(a), v, v ? 16 : 0);
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
    else if (a.mMode == 2)
    {
        pixels = (const void*)(uintptr_t)a.mOffset;
    }
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
    case OP_glGenTextures: case OP_glGenFramebuffers: case OP_glGenBuffers: case OP_glGenVertexArrays: case OP_glGenQueries:
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
        case OP_glGenVertexArrays: LLVkGL::on_glGenVertexArrays(n, ids.data()); break;
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
    case OP_glVertexAttrib4fv:
    {
        const GLuint index = r.get<GLuint>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glVertexAttrib4fv(index, (const GLfloat*)b.mData);
        break;
    }
    case OP_glBufferData:
    {
        const GLenum target = r.get<GLenum>(), usage = r.get<GLenum>();
        const S64 size = r.get<S64>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glBufferData(target, (GLsizeiptr)size, b.mData, usage);
        break;
    }
    case OP_glBufferSubData:
    {
        const GLenum target = r.get<GLenum>();
        r.get<GLenum>();
        const S64 offset = r.get<S64>(), size = r.get<S64>();
        Blob b = read_blob(r, flags);
        LLVkGL::on_glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, b.mData);
        break;
    }
    case OP_glBindBuffer:
    {
        const GLenum target = r.get<GLenum>();
        const GLuint buffer = r.get<GLuint>();
        LLVkGL::on_glBindBuffer(target, buffer);
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
    case CMD_PRESENT: LLVkMirror::presentFrameImpl(); break;
    case CMD_HOLD: LLVkMirror::holdFrameImpl(r.get<S32>() != 0); break;
    case CMD_SNAPSHOT_BOUNDARY: LLVkMirror::snapshotBoundaryImpl(r.get<S32>()); break;
    case CMD_ARM_SNAPSHOT: LLVkMirror::armSnapshotCompareImpl(); break;
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
    if (op < OP_GL_COUNT)
    {
        dispatch_gl(op, flags, payload);
    }
    else
    {
        dispatch_cmd(op, flags, payload);
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
        if (!v) return false;
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

#endif // LL_VULKAN
