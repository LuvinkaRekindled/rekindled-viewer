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

    // ---- typed records (N1b): state the viewer knows as a whole is written as one record instead of a run of GL-shaped calls ----
    // true on the thread that writes the queue while the worker runs
    bool mainProducer();
    // one vertex attribute: kind 0 is glVertexAttribPointer, kind 1 glVertexAttribIPointer (same layout the coalesced run uses)
    struct VertexAttrib { U32 mKind; U32 mIndex; S32 mSize; U32 mType; S32 mStride; U32 mNormalized; const void* mPointer; };
    // the vertex array state of LLVertexBuffer::setBuffer(): optional array buffer bind, the attribute pointers, optional index buffer bind (played in that order).
    // Only on the main producer (see mainProducer()); one record, played by the worker through the same functions the separate calls reach.
    // a texture bind with its unit (LLTexUnit): replaces a held glActiveTexture + glBindTexture pair
    void bindTextureUnit(U32 unit, U32 target, U32 texture);
    // draws (LLVertexBuffer): the same records glDrawRangeElements / glDrawArrays queue, written without the call coalescer in between
    void drawRangeElements(U32 mode, U32 start, U32 end, S32 count, U32 type, const void* indices);
    void drawArrays(U32 mode, S32 first, S32 count);
    // buffer storage by name (LLVertexBuffer): (re)specify a buffer / write a range of it, without binding it first. target only says vertex or index.
    void bufferData(U32 name, U32 target, S64 size, const void* data, U32 usage);
    void bufferSubData(U32 name, S64 offset, S64 size, const void* data);
    // LLGLSLShader::bind(): unbinds the vertex and index buffers, binds the program (null: unchanged) and enables / disables vertex attribute arrays (bit masks of attribute locations)
    void shaderBind(void* program, U32 enable_mask, U32 disable_mask);
    // the unit a following texture bind or texture call uses (LLTexUnit::activate): held until the next record, replaces a glActiveTexture call
    void activeTextureUnit(U32 unit);
    // texture parameters (wrap, filter, anisotropy) of the texture bound on the active unit, written as one record (played as glTexParameteri / glTexParameterf in order)
    struct TexParam { U32 mPname; U32 mIsFloat; S32 mInt; F32 mFloat; };
    void texParameters(U32 target, const TexParam* params, U32 count);
    // render targets (LLRenderTarget): attach a texture to a framebuffer by name (then bind restore_fbo again), and make a framebuffer current with its draw buffers and viewport
    void framebufferAttach(U32 fbo, U32 attachment, U32 textarget, U32 texture, U32 restore_fbo);
    // draw_buffers: the number of colour attachments to draw to (0: none, -1: the back buffer)
    void targetState(U32 fbo, S32 draw_buffers, S32 vx, S32 vy, S32 vw, S32 vh);
    void setArrayState(bool bind_array, U32 array_buffer, bool bind_elem, U32 elem_buffer, const VertexAttrib* attribs, U32 count);

    // commands that are not GL calls (the GL calls are numbered by the generated enum in llvkgl.h)
    enum Cmd : unsigned short
    {
        CMD_BASE = 1000,
        CMD_FRAME_BEGIN, CMD_FRAME_END, CMD_PRESENT, CMD_HOLD, CMD_SNAPSHOT_BOUNDARY, CMD_ARM_SNAPSHOT,
        CMD_UPSCALE, CMD_PROGRAM_BIND, CMD_UNIFORM_SCALAR, CMD_UNIFORM_VECTOR, CMD_UNIFORM_MATRIX, CMD_SAMPLER_UNIT, CMD_PROGRAM_DESTROY,
        CMD_SYNC, CMD_STOP, CMD_WRAP,
        CMD_BIND_TEXTURE_UNIT, CMD_ARRAY_RUN,      // merged records, see the coalescing in llvkasync.cpp
        CMD_BUFFER_DATA, CMD_BUFFER_SUBDATA,       // named buffer storage (bufferData / bufferSubData)
        CMD_SHADOW_PACKETS,                        // a pass of shadow casters in one record (LLVkMirror::shadowPackets)
        CMD_SHADER_BIND,                           // program bind + vertex buffer unbind + attribute array enables (LLGLSLShader::bind)
        CMD_TEX_PARAMS,                            // texture parameters of the bound texture, several in one record (LLTexUnit sampler state)
        CMD_FBO_ATTACH, CMD_TARGET_STATE,          // render target commands (framebufferAttach, targetState)
        CMD_TEX_SLOTS, CMD_DRAW                    // texture slots that changed since the last draw, alone or folded into the draw record
        // program bind + vertex buffer unbind + attribute array enables (LLGLSLShader::bind)
    };

    // frame boundaries: the main thread may be at most two frames ahead of the worker
    void frameBegin(S32 width, S32 height);
    void frameEnd();
    void framePresent();
    void frameHold(bool hold);
    void snapshotBoundary(S32 mode);
    void armSnapshot();

    // names for glGen*: handed out on the calling thread, created on the worker
    void gen_enq(unsigned short op, int kind, S32 n, unsigned* out);
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
