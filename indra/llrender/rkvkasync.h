/**
 * @file rkvkasync.h
 * @brief Runs the Vulkan mirror on its own thread (RK_VK_ASYNC=1, Vulkan-only mode).
 *
 * The viewer's main thread no longer translates GL calls: every call the mirror owns is written into a queue (with copies of the data
 * it points to) and a worker thread replays the queue in order through the same translation code that ran inline before. It is what
 * the OpenGL driver does on its own thread. Calls that hand a value back wait for the worker (rare), except the occlusion query
 * results, which the worker publishes into a table the main thread reads without waiting.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef RK_RKVKASYNC_H
#define RK_RKVKASYNC_H

#if defined(RK_VULKAN)

#include "linden_common.h"
#include "rkrenderformat.h"

#include <functional>
#include <vector>
#include <string>

namespace RKVkAsync
{
    // true while the worker thread runs and GL calls are queued instead of translated inline
    extern bool gEnabled;
    long long lastPresentTicks();       // QueryPerformanceCounter ticks of the last present the worker made (0 before the first)

    // RK_VK_ASYNC is set (and the mirror owns the resources)
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
    // <FS> runs fn on the worker after everything queued before it, and does NOT wait (hitchless loading: a read of a render target for the avatar bake). fn must own what it touches.
    void postCall(std::function<void()> fn);
    // <FS> RK_VK_SYNC_LOG (default on): the main thread's waits in syncCall are timed by the function that called it and reported every 10 s (VkSync); the symbolizer names an address
    void setSymbolizer(std::string (*fn)(const void*));
    // <FS> the calling thread's stack as text, one function a line (the symbolizer above; addresses without it)
    std::string stackHere(int skip, int frames);
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
        // UI record cache (doc/vulkan/PLAN-ui-record-cache.md)
        void reseed();              // main thread: the render state the next begin() starts from is the main producer's now
        const std::vector<U8>& bytes() const;   // the records written since begin() (before splice())
        bool leftState() const;     // end() found the render state different from where begin() started
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
    // RKShaderProgram::bind(): unbinds the vertex and index buffers, binds the program (null: unchanged) and enables / disables vertex attribute arrays (bit masks of attribute locations)
    void shaderBind(void* program);
    // render targets (LLRenderTarget): attach a texture to a framebuffer by name
    void framebufferAttach(U32 fbo, U32 slot, U32 texture);
    // a query begins / ends in the command stream (rkvkquery.h; kind is an RKVkQuery::Kind)
    void queryBeginCmd(U32 kind, U32 id);
    void queryEndCmd(U32 kind);
    // clears the current target (the state block is written first): colour and/or depth with the given colour
    void clearTarget(bool color, bool depth, const F32* rgba);
    // the colour attachment count of the current target (0: none, -1: the back buffer); not a pass boundary
    void targetDrawBuffers(U32 fbo, S32 draw_buffers);
    // S6.1: a pass is one record. PASS_NEW: the viewer bound a render target (it nests inside the pass that was open); PASS_RESUME: the pass an inner one interrupted goes on (after passEnd of the inner);
    // PASS_SCREEN: the default framebuffer again, no pass is open. The record carries the target (colour attachment count, -1: the back buffer), the viewport, the load op (clear colour / depth with the
    // colour to clear to), the pass id and its parent's, and where the viewer bound it (file:line, for the logs).
    enum PassKind : U32 { PASS_NEW = 0, PASS_RESUME = 1, PASS_SCREEN = 2 };
    void passBegin(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh, bool clear_color, bool clear_depth, const F32* rgba, PassKind kind, const char* file, int line);
    // the viewer flushed the pass it bound last (LLRenderTarget::flush): it is closed; the next passBegin is PASS_RESUME of the one below it or PASS_SCREEN
    void passEnd();
    // S6.2: a pass is closed: everything it reads of the state classes that used to persist from the pass before (render state block, program uniform class blocks, texture units, uniform buffer
    // bindings, constant attributes, the program of the packet records, the vertex binding) is sent again at its first use in the pass. passEpoch changes with every pass record.
    unsigned long long passEpoch();
    // UI record cache: a segment of records (a view's draw, recorded in a sub-stream on the main thread) that can be played again later. segmentBoundary() is what the end of a pass does to
    // the producer's beliefs (render state, draw binding, texture units, uniform buffer bindings and constant attributes are sent again at their first use) without a pass record; call it
    // right before a segment is recorded or replayed and right after (then what the producer believes and what the worker holds agree again, whichever of the two ran).
    void segmentBoundary();
    struct SegmentScan
    {
        bool mCacheable = true;         // only records a replay may repeat: state, draws, texture slots, uniforms, program binds, sampler units, constant attributes
        U16 mBadOp = 0;                 // the first record that is not (the Cmd), when not cacheable
        U32 mRecords = 0, mDraws = 0;
        std::vector<U32> mTextures;     // the texture ids the draws and slot records name (sorted, unique)
        std::vector<U32> mBuffers;      // the vertex / index buffer names the draws' bindings name (sorted, unique): a replay draws from them by name
    };
    void scanSegment(const U8* data, size_t size, SegmentScan& out);
    void replaySegment(const U8* data, size_t size);        // main producer: appends the records to the queue as a splice does (no state adopted)
    void spliceSideEffects(const U8* data, size_t size);    // main producer: only the records of a recording that are not state, draws, texture slots, uniforms, program binds, sampler units or constant attributes
                                                            // (texture and glyph uploads, buffer writes, resource creation: what a draw did to the worker besides drawing)
    std::string describeSegmentDifference(const U8* a, size_t a_size, const U8* b, size_t b_size);       // the first record that differs (empty: equal)
    void uiCacheNameTextures(const U32* ids, size_t count);  // the ids a cached segment names: a freed one of these bumps the epoch
    void uiCacheTextureFreed(U32 id);                        // RKImageTex::deleteTextures
    void uiCacheNameBuffers(const U32* names, size_t count);
    void uiCacheBufferTouched(U32 name);                     // a named buffer is recycled, destroyed or written: a cached segment that draws from it is stale
    void uiCacheInvalidate();                                // every cached segment is stale (a skin or colour reload, a window resize)
    unsigned long long uiCacheEpoch();                       // changes when a program is destroyed, a named texture is freed, or uiCacheInvalidate() is called: every cached segment is stale
    void uniformRangeAnnounce(void* program, U32 cls, U32 offset, U32 size, const void* data);      // uniformRange, marked as a full class block announced at the first use of the program in a pass
    // the pass being played (worker thread): what the last pass record said
    struct PassInfo { U32 mId = 0, mParent = 0, mFbo = 0; S32 mBuffers = 0; U32 mSite = 0, mKind = 0; };
    const PassInfo& currentPass();
    // a named GPU zone (the viewer's LL_PROFILE_GPU_ZONE scopes, enabled with RKGpuZoneTimers + RK_VK_PHASES): the worker writes a GPU timestamp at this point of the frame
    void gpuZone(const char* name, bool begin);

    // ---- F2.5 measure first: main-thread cost per front-end entry point (counted with RK_VK_OPSTATS, every 16th call timed; reported in the VkOps "worker operations" line) ----
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
    // F2.5: the main producer keeps a copy of each program's class blocks, resolves uniforms to byte ranges and sends the changed bytes (uniformRange); other producers (a sub-stream) send per-uniform records
    bool mainOnlyProducer();            // the main thread, not inside a sub-stream: the only producer that owns the shaders' class block copies
    void uniformRange(void* program, U32 cls, U32 offset, U32 size, const void* data);
    // where the vertex attributes of a draw come from, as values: the buffers and, for each shader attribute location that is read from an array, how
    struct BindAttr { S32 mSize; S32 mStride; U64 mOffset; U8 mType; U8 mNormalized; U8 mInteger; U8 mPad[5]; };       // mType: RKVkFormat::PType
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
        memcpy((void*)&b, p, 3 * sizeof(U32));
        p += 3 * sizeof(U32);
        for (U32 i = 0; i < 16; ++i)
        {
            if (b.mPresent & (1u << i)) { memcpy(&b.mAttr[i], p, sizeof(BindAttr)); p += sizeof(BindAttr); }
            else { memset(&b.mAttr[i], 0, sizeof(BindAttr)); }
        }
        return p;
    }
    // the draw context of the calling producer: RKShaderProgram::bind/unbind and LLVertexBuffer::setBuffer/unbind write it (only when gDrawRec != 0)
    void drawProgram(void* program);                                                             // the program of the next draws (null: none)
    void drawBindingClear();                                                                     // no buffers bound
    void drawBinding(U32 array_buffer, U32 elem_buffer, const VertexAttrib* attribs, U32 count); // the whole binding for the bound shader's attributes (GL type numbers as in setArrayState)
    void drawBindingSet(const DrawBinding& binding);        // a binding worked out earlier (LLVertexBuffer keeps the last one of each buffer)
    void drawBindingGet(DrawBinding& out);                  // the producer's current binding

    // ---- texture pixels without a copy (C4) ----
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
        CMD_SYNC, CMD_ASYNC_CALL, CMD_STOP, CMD_WRAP,
        CMD_BUFFER_DATA, CMD_BUFFER_SUBDATA,       // named buffer storage (bufferData / bufferSubData)
        CMD_SHADOW_PACKETS,                        // a pass of shadow casters in one record (RKVkBridge::shadowPackets)
        CMD_FBO_ATTACH, CMD_TARGET_STATE,          // render target commands (framebufferAttach, targetState)
        CMD_TEX_SLOTS,                             // texture ids that changed since the last draw, when something other than a draw comes first (a draw record carries them itself)
        CMD_GPU_ZONE,                              // a named GPU timing zone begins or ends here in the command stream (RKGpuZoneTimers)
        CMD_UNIFORM_BATCH,                         // loose uniform writes that were pending when something other than a draw came first (a draw carries them inside its own record)
        CMD_BUFFER_RECYCLE,                        // a pooled buffer name changes hands (bufferRecycle)
        CMD_RT_SCENE,                              // the ray tracing scene of the frame is built from the casters named so far (RKVkBridge::rtScene)
        CMD_STATE,                                 // F1.1: the render state block (RKVkState::Block) changed since the last one written
        CMD_TEX_IMAGE, CMD_TEX_PARAMS_ID, CMD_TEX_OP,  // F1.2: texture edits that name their texture (rkvktex.h): image data, parameters, small operations
        CMD_UNIFORM_BLOCK,                         // F1.3: a buffer becomes the contents of a numbered uniform block (rkvkbuf.h)
        CMD_CONST_ATTRIB,                          // F1.3: the constant value of a vertex attribute that has no array
        CMD_CLEAR,                                 // F1.4: clear the current target (rkvktarget.h)
        CMD_QUERY_BEGIN, CMD_QUERY_END,            // F1.6: a query kind begins / ends here in the stream (rkvkquery.h)
        CMD_RESOURCE_CREATE, CMD_RESOURCE_DESTROY, // F2.1: names of textures / targets / buffers / queries come into being or go (resourceCreate / resourceDestroy)
        CMD_DRAW,                                  // F2.4: the draw record: program, vertex binding when it changed, texture ids, uniform writes
        CMD_UNIFORM_RANGE,                         // F2.5: inside a uniform batch: bytes of a program's class block (uniformRange)
        CMD_PASS,                                  // F3.5d: a render pass begins: target, colour attachment count and the load op (clear) in one record (passBegin)
        CMD_DRAW_SCRATCH,                          // F3.1: a non-indexed draw with its vertices inline (immediate mode: LLRender::flush); no vertex binding, the worker supplies the vertex scratch
        CMD_DENOISE,                               // the AMD reflection denoiser runs on mirror-owned textures (RKVkBridge::denoiseReflections, PLAN-amd-denoiser.md)
        // program bind + vertex buffer unbind + attribute array enables (RKShaderProgram::bind)
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

    // shader program commands (RKShaderProgram's mirror calls)
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
    // RK_VK_PHASES: what the worker thread spent its time on since the last call, per GL / command operation (calls, ms per frame, ns per call)
    std::string opStatsReport(size_t frames);
}

#endif // RK_VULKAN

#endif // RK_RKVKASYNC_H
