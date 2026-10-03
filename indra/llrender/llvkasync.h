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

    // commands that are not GL calls (the GL calls are numbered by the generated enum in llvkgl.h)
    enum Cmd : unsigned short
    {
        CMD_BASE = 1000,
        CMD_FRAME_BEGIN, CMD_FRAME_END, CMD_PRESENT, CMD_HOLD, CMD_SNAPSHOT_BOUNDARY, CMD_ARM_SNAPSHOT,
        CMD_PROGRAM_BIND, CMD_UNIFORM_SCALAR, CMD_UNIFORM_VECTOR, CMD_UNIFORM_MATRIX, CMD_SAMPLER_UNIT, CMD_PROGRAM_DESTROY,
        CMD_SYNC, CMD_STOP, CMD_WRAP
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
    void queryPending(unsigned id);
    void queryPublish(unsigned id, unsigned long long value);
    void queryIdle(unsigned id);
}

#endif // LL_VULKAN

#endif // LL_LLVKASYNC_H
