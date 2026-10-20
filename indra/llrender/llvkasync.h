/**
 * @file llvkasync.h
 * @brief Runs the Vulkan mirror on its own thread (LL_VK_ASYNC=1, Vulkan-only mode).
 *
 * The viewer's main thread no longer translates GL calls: every call the mirror owns is written into a queue (with copies of the data
 * it points to) and a worker thread replays the queue in order through the same translation code that ran inline before. It is what
 * the OpenGL driver does on its own thread. Calls that hand a value back wait for the worker (rare), except the occlusion query
 * results, which the worker publishes into a table the main thread reads without waiting.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKASYNC_H
#define LL_LLVKASYNC_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llrenderformat.h"

#include <functional>
#include <string>

namespace LLVkAsync
{
    // true while the worker thread runs and GL calls are queued instead of translated inline
    extern bool gEnabled;

    // LL_VK_ASYNC is set (and the mirror owns the resources)
    bool requested();
    // starts the worker thread; the calling thread becomes the queue's main producer. Call between two frames.
    bool start();
    // plays everything that is queued, then stops the worker
    void stop();

    // ---- producers ----
    void push(unsigned short op, const void* args, unsigned size);
    void pushData(unsigned short op, const void* args, unsigned size, const void* data, size_t data_size);
    // runs fn on the worker after everything queued before it, and waits for it to finish
    void syncCall(const std::function<void()>& fn);
    // bytes the worker has yet to replay (what the main thread has written and the worker has not taken): a syncCall waits for all of it
    unsigned long long backlogBytes();

    // ---- typed records (N1b): state the viewer knows as a whole is written as one record instead of a run of GL-shaped calls ----
    // true on the thread that writes the queue while the worker runs
    bool mainProducer();

    // F3: records written by another thread. Between begin() and end() the typed records and the state setters of the calling thread go to a private buffer (its own render state
    // context, pending texture slots and uniform writes) instead of the queue; the main thread splices the buffer into the queue, in order, where the work would have run.
    // A sub-stream starts from the render state the main producer had when it was made, and writes whatever it sets (nothing is assumed about what the worker holds besides
    // that); after the splice the main producer announces its own state again.
    class SubStream
    {
    public:
        SubStream();                // on the main thread: takes the render state the sub-stream starts from
        ~SubStream();
        void begin();               // on the thread that produces
        void end();                 // on the same thread: the pending slots and uniform writes go into the buffer
        void splice();              // on the main thread: appends the buffer to the queue, then resets it
        U32 records() const;
    private:
        SubStream(const SubStream&) = delete;
        SubStream& operator=(const SubStream&) = delete;
        struct Impl;
        Impl* mImpl;
    };
    // one vertex attribute: kind 0 is glVertexAttribPointer, kind 1 glVertexAttribIPointer (same layout the coalesced run uses)
    struct VertexAttrib { U32 mKind; U32 mIndex; S32 mSize; ComponentType mType; S32 mStride; U32 mNormalized; const void* mPointer; };
    // the vertex array state of LLVertexBuffer::setBuffer(): optional array buffer bind, the attribute pointers, optional index buffer bind (played in that order).
    // Only on the main producer (see mainProducer()); one record, played by the worker through the same functions the separate calls reach.
    // a texture bind with its unit (LLTexUnit): replaces a held glActiveTexture + glBindTexture pair
    void bindTextureUnit(U32 unit, U32 texture);
    // draws (LLVertexBuffer): the same records glDrawRangeElements / glDrawArrays queue, written without the call coalescer in between
    void drawRangeElements(PrimitiveMode mode, U32 start, U32 end, S32 count, ComponentType type, const void* indices);
    void drawArrays(PrimitiveMode mode, S32 first, S32 count);
    // F3.1 immediate mode: one draw whose vertices travel inside the record (non-indexed). `mask` says which attribute locations are supplied (bit = attribute location:
    // SCRATCH_POSITION always, SCRATCH_TEXCOORD0, SCRATCH_COLOR); the others read the constant attribute. The bytes are `count` positions (4 floats, 3 used), then `count` texcoords
    // (2 floats) when SCRATCH_TEXCOORD0 is set, then `count` colours (4 normalized bytes) when SCRATCH_COLOR is set. The worker copies them into the frame's vertex scratch.
    enum ScratchAttr : U32 { SCRATCH_POSITION = 1u << 0, SCRATCH_TEXCOORD0 = 1u << 2, SCRATCH_COLOR = 1u << 6 };
    void drawScratch(PrimitiveMode mode, S32 count, U32 mask, const void* positions, const void* texcoords, const void* colors);
    // buffer storage by name (LLVertexBuffer): (re)specify a buffer / write a range of it, without binding it first. target only says vertex or index.
    void bufferData(U32 name, BufferKind kind, S64 size, const void* data);
    void bufferSubData(U32 name, S64 offset, S64 size, const void* data);
    // the vertex buffer pool gives a buffer name to another owner (or takes it back): what the name held is not what it will hold; the arena's epoch moves on
    void bufferRecycle(U32 name);
    // LLGLSLShader::bind(): unbinds the vertex and index buffers, binds the program (null: unchanged) and enables / disables vertex attribute arrays (bit masks of attribute locations)
    void shaderBind(void* program);
    // one texture parameter (wrap, filter, anisotropy ...), see LLVkTex::params
    struct TexParam { U32 mPname; U32 mIsFloat; S32 mInt; F32 mFloat; };
    // render targets (LLRenderTarget): attach a texture to a framebuffer by name (then bind restore_fbo again), and make a framebuffer current with its draw buffers and viewport
    void framebufferAttach(U32 fbo, U32 slot, U32 texture);
    // a query begins / ends in the command stream (llvkquery.h; kind is an LLVkQuery::Kind)
    void queryBeginCmd(U32 kind, U32 id);
    void queryEndCmd(U32 kind);
    // clears the current target (the state block is written first): colour and/or depth with the given colour
    void clearTarget(bool color, bool depth, const F32* rgba);
    // draw_buffers: the number of colour attachments to draw to (0: none, -1: the back buffer)
    void targetState(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh);
    // the colour attachment count of a target without touching the viewport
    void targetDrawBuffers(U32 fbo, S32 draw_buffers);
    // F3.5d: a pass begins: the target (colour attachment count), the viewport and the load op (clear colour / depth with the colour to clear to) are ONE record (was: targetState + clear)
    void passBegin(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh, bool clear_color, bool clear_depth, const F32* rgba);
    // a named GPU zone (the viewer's LL_PROFILE_GPU_ZONE scopes, enabled with FSGpuZoneTimers + LL_VK_PHASES): the worker writes a GPU timestamp at this point of the frame
    void gpuZone(const char* name, bool begin);

    // ---- F2.5 measure first: main-thread cost per front-end entry point (counted with LL_VK_OPSTATS, every 16th call timed; reported in the VkOps "worker operations" line) ----
    enum FrontPoint { FP_SHADER_BIND, FP_UNIFORM, FP_UNIFORM_MATRIX, FP_UNIFORM_SENT, FP_TEXUNIT_BIND, FP_BLEND, FP_GLSTATE, FP_SET_BUFFER, FP_DRAW, FP_SYNC_MATRICES, FP_FLUSH, FP_MATRIX_STACK, FP_MATRIX_LOAD, FP_MATRIX_XFORM, FP_MATRIX_MODE, FP_TEXUNIT_UNBIND, FP_TEXUNIT_STATE, FP_TEXUNIT_MANUAL, FP_TEXUNIT_OPTIONS, FP_COUNT };
    extern bool gFrontStats;
    unsigned long long frontStart(int id);                 // counts the call (main thread only); returns a timestamp when this call is timed, else 0
    void frontStop(int id, unsigned long long t0);
    void frontNote(int id);                                // count only
    struct FrontLap
    {
        int mId;
        unsigned long long mT0;
        explicit FrontLap(int id) : mId(id), mT0(0) { if (gFrontStats) mT0 = frontStart(id); }
        ~FrontLap() { if (mT0) frontStop(mId, mT0); }
    };
    inline void frontCount(int id) { if (gFrontStats) frontNote(id); }
    unsigned long long drawSeq();                          // F3.5a: draws counted so far on the main thread (only while gFrontStats): "no draw between a bind and its clear"

    // ---- F2.4: the draw record (doc/vulkan/F2-draw-records.md) ----
    // F2.4: the draw record (doc/vulkan/F2-draw-records.md): a draw names its program and carries its vertex binding, the texture ids and the uniform writes that changed
    // F2.5: LL_VK_UNIFORMREC: 0 per-uniform records and a value cache per shader (old), 1 verify (old + the producer's class blocks, range writes go to a second copy the worker
    // compares with the real one at every draw), 2 range writes only (the producer keeps a copy of each program's class blocks, resolves uniforms to byte ranges, sends the changed bytes)
    extern int gUniformRec;
    bool mainOnlyProducer();            // the main thread, not inside a sub-stream: the only producer that owns the shaders' class block copies
    void uniformRange(void* program, U32 cls, U32 offset, U32 size, const void* data);
    // where the vertex attributes of a draw come from, as values: the buffers and, for each shader attribute location that is read from an array, how
    struct BindAttr { S32 mSize; S32 mStride; U64 mOffset; U8 mType; U8 mNormalized; U8 mInteger; U8 mPad[5]; };       // mType: LLVkFormat::PType
    struct DrawBinding
    {
        U32 mArray = 0;             // vertex buffer id (0: none)
        U32 mElem = 0;              // index buffer id
        U32 mPresent = 0;           // bit per attribute location read from the array; the others read the constant attribute
        BindAttr mAttr[16] = {};
    };
    // a binding in a record: array, elem, present mask, then one BindAttr per present location in location order
    inline size_t drawBindingSize(const DrawBinding& b)
    {
        size_t n = 0;
        for (U32 m = b.mPresent; m; m &= m - 1) { ++n; }
        return 3 * sizeof(U32) + n * sizeof(BindAttr);
    }
    inline U8* drawBindingWrite(U8* p, const DrawBinding& b)
    {
        memcpy(p, &b, 3 * sizeof(U32));
        p += 3 * sizeof(U32);
        for (U32 i = 0; i < 16; ++i)
        {
            if (b.mPresent & (1u << i)) { memcpy(p, &b.mAttr[i], sizeof(BindAttr)); p += sizeof(BindAttr); }
        }
        return p;
    }
    inline const U8* drawBindingRead(const U8* p, DrawBinding& b)
    {
        memcpy(&b, p, 3 * sizeof(U32));
        p += 3 * sizeof(U32);
        for (U32 i = 0; i < 16; ++i)
        {
            if (b.mPresent & (1u << i)) { memcpy(&b.mAttr[i], p, sizeof(BindAttr)); p += sizeof(BindAttr); }
            else { memset(&b.mAttr[i], 0, sizeof(BindAttr)); }
        }
        return p;
    }
    // the draw context of the calling producer: LLGLSLShader::bind/unbind and LLVertexBuffer::setBuffer/unbind write it (only when gDrawRec != 0)
    void drawProgram(void* program);                                                             // the program of the next draws (null: none)
    void drawBindingClear();                                                                     // no buffers bound
    void drawBinding(U32 array_buffer, U32 elem_buffer, const VertexAttrib* attribs, U32 count); // the whole binding for the bound shader's attributes (GL type numbers as in setArrayState)
    void drawBindingSet(const DrawBinding& binding);        // a binding worked out earlier (LLVertexBuffer keeps the last one of each buffer)
    void drawBindingGet(DrawBinding& out);                  // the producer's current binding

    // ---- texture pixels without a copy (C4) ----
    // LL_VK_TEXONECOPY=1 turns the one-copy upload path on (off by default: no measured gain, see PLAN-cleanup-before-rt.md C4)
    extern bool gTexOneCopy;
    // Decoded pixels the caller keeps alive and unchanged: while a hold is set on the main thread, a texture upload whose pixels lie inside [mBase, mBase+mSize)
    // is queued as a reference (mRef is called now, mUnref by the worker once the pixels are in the staging buffer) instead of a copy of the bytes.
    struct PixelHold { const void* mBase; size_t mSize; const void* mOwner; void (*mRef)(const void*); void (*mUnref)(const void*); };
    void setPixelHold(const PixelHold* hold);       // null clears; only the main producer thread uses it, other threads copy as before
    // waits until the worker has played everything queued so far (what an image does before it is changed while a hold on it may still be queued)
    void waitForPixelHolds();
    // main-thread cost of the texture uploads and what was handed over, and the worker's side, since the last call (frames: the period's frame count)
    std::string texUploadReport(size_t frames);
    // the worker adds what it spent on uploads (microseconds): converting into a vector (old path), filling the staging buffer, the whole call; calls: uploads counted by this call
    void noteTexWorker(unsigned long long convert_us, unsigned long long fill_us, unsigned long long total_us, unsigned calls);

    // the records of the command stream
    enum Cmd : unsigned short
    {
        CMD_BASE = 1000,
        CMD_FRAME_BEGIN, CMD_FRAME_END, CMD_PRESENT, CMD_HOLD, CMD_SNAPSHOT_BOUNDARY,
        CMD_UPSCALE, CMD_PROGRAM_BIND, CMD_UNIFORM_SCALAR, CMD_UNIFORM_VECTOR, CMD_UNIFORM_MATRIX, CMD_SAMPLER_UNIT, CMD_PROGRAM_DESTROY,
        CMD_SYNC, CMD_STOP, CMD_WRAP,
        CMD_BUFFER_DATA, CMD_BUFFER_SUBDATA,       // named buffer storage (bufferData / bufferSubData)
        CMD_SHADOW_PACKETS,                        // a pass of shadow casters in one record (LLVkMirror::shadowPackets)
        CMD_TEX_PARAMS,                            // texture parameters of the bound texture, several in one record (LLTexUnit sampler state)
        CMD_FBO_ATTACH, CMD_TARGET_STATE,          // render target commands (framebufferAttach, targetState)
        CMD_TEX_SLOTS,                             // texture ids that changed since the last draw, when something other than a draw comes first (a draw record carries them itself)
        CMD_GPU_ZONE,                              // a named GPU timing zone begins or ends here in the command stream (FSGpuZoneTimers)
        CMD_UNIFORM_BATCH,                         // loose uniform writes that were pending when something other than a draw came first (a draw carries them inside its own record)
        CMD_BUFFER_RECYCLE,                        // a pooled buffer name changes hands (bufferRecycle)
        CMD_RT_SCENE,                              // the ray tracing scene of the frame is built from the casters named so far (LLVkMirror::rtScene)
        CMD_STATE,                                 // F1.1: the render state block (LLVkState::Block) changed since the last one written
        CMD_TEX_IMAGE, CMD_TEX_PARAMS_ID, CMD_TEX_OP,  // F1.2: texture edits that name their texture (llvktex.h): image data, parameters, small operations
        CMD_UNIFORM_BLOCK,                         // F1.3: a buffer becomes the contents of a numbered uniform block (llvkbuf.h)
        CMD_CONST_ATTRIB,                          // F1.3: the constant value of a vertex attribute that has no array
        CMD_CLEAR,                                 // F1.4: clear the current target (llvktarget.h)
        CMD_QUERY_BEGIN, CMD_QUERY_END,            // F1.6: a query kind begins / ends here in the stream (llvkquery.h)
        CMD_RESOURCE_CREATE, CMD_RESOURCE_DESTROY, // F2.1: names of textures / targets / buffers / queries come into being or go (resourceCreate / resourceDestroy)
        CMD_DRAW,                                  // F2.4: the draw record: program, vertex binding when it changed, texture ids, uniform writes
        CMD_UNIFORM_RANGE,                         // F2.5: inside a uniform batch: bytes of a program's class block (uniformRange)
        CMD_PASS,                                  // F3.5d: a render pass begins: target, colour attachment count and the load op (clear) in one record (passBegin)
        CMD_DRAW_SCRATCH,                          // F3.1: a non-indexed draw with its vertices inline (immediate mode: LLRender::flush); no vertex binding, the worker supplies the vertex scratch
        // program bind + vertex buffer unbind + attribute array enables (LLGLSLShader::bind)
    };

    // frame boundaries: the main thread may be at most two frames ahead of the worker
    void frameBegin(S32 width, S32 height);
    void frameEnd();
    void framePresent();
    void frameHold(bool hold);
    void snapshotBoundary(S32 mode);

    // resource lifetime: the names are handed out on the calling thread (resourceCreate) and the worker makes / releases the objects in stream order
    enum ResourceKind : U32 { RES_TEXTURE = 0, RES_TARGET = 1, RES_BUFFER = 2, RES_QUERY = 4 };
    void resourceCreate(U32 kind, S32 n, U32* ids);
    void resourceDestroy(U32 kind, S32 n, const U32* ids);
    // occlusion query results published by the worker; false: ask the worker (sync call)
    bool query_get(int kind, unsigned id, unsigned pname, void* params);

    // shader program commands (LLGLSLShader's mirror calls)
    void programBind(void* program);
    void programDestroy(void* program);
    void samplerUnit(void* program, unsigned sampler_index, unsigned unit);
    void uniformScalar(void* program, unsigned uniform_index, int kind, unsigned n, const float* f, const int* i);
    void uniformVector(void* program, unsigned uniform_index, unsigned components, unsigned count, const void* data);
    void uniformMatrix(void* program, unsigned uniform_index, unsigned columns, unsigned rows, unsigned count, const float* data);

    // for the occlusion query bookkeeping done while a query command is queued / when the worker harvests a result
    // Every query the main thread begins gets a generation number for its GL name (the viewer recycles names constantly). A result only
    // counts for the viewer when it carries a generation at least as new as the latest one begun.
    unsigned long long queryBegin(unsigned id);                                           // main thread: the new generation
    void queryPublish(unsigned id, unsigned long long gen, unsigned long long value);     // worker: the result of generation 'gen'
    void queryIdle(unsigned id, unsigned long long gen);                                  // worker: generation 'gen' was never issued or has been consumed: report visible
    unsigned long long workerQueryGen(unsigned id);                                       // worker: the generation of the query being begun / the latest one begun
    void setWorkerQueryGen(unsigned id, unsigned long long gen);
    // timer queries are not published: reads of their results go to the worker
    void queryMarkSync(unsigned id, bool sync);
    // counters since the last call: [0] marked pending, [1] published, [2] set idle, [3] polls answered ready, [4] polls answered pending, [5] polls answered idle (never issued), [6] result reads sent to the worker
    void queryStats(unsigned long long out[7]);
    // result values handed to the viewer (either mode), counted since the last call: zero (occluded) and non-zero (visible)
    void queryResultTally(bool nonzero);
    void queryResultStats(unsigned long long out[2]);
    // frame pacing since the last call: spacing of the main thread's frame starts and of the worker's presents (ms: n, avg, p50, p95, p99, max), and how long each side waited for the other
    std::string pacingReport();
    // LL_VK_PHASES: what the worker thread spent its time on since the last call, per GL / command operation (calls, ms per frame, ns per call)
    std::string opStatsReport(size_t frames);
}

#endif // LL_VULKAN

#endif // LL_LLVKASYNC_H
