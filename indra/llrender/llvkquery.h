/**
 * @file llvkquery.h
 * @brief F1.6: occlusion and timer queries by id, with query kinds of their own (no GL targets or result enums).
 *
 * A query is a handle (the id the producer allocates, recycled by the viewer's pools) that begins and ends around draws in the command stream. begin()/end() write
 * records (CMD_QUERY_BEGIN / CMD_QUERY_END); ready()/result() are answered from the results the worker publishes (occlusion) or by asking the worker (timers).
 * An occlusion result that is not ready when it is forced reads 'visible', which is what the viewer assumes for a query it cannot resolve.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKQUERY_H
#define LL_LLVKQUERY_H

#if defined(LL_VULKAN)

#include "linden_common.h"

namespace LLVkQuery
{
    enum Kind : U32
    {
        ANY_SAMPLES = 0,            // occlusion: result 0 / 1
        SAMPLES_PASSED = 1,         // occlusion: the number of samples
        TIME_ELAPSED = 2,           // GPU time between begin and end, nanoseconds
        PRIMITIVES_GENERATED = 3    // not available (reports nothing)
    };

    void create(S32 count, U32* ids);
    void destroy(S32 count, const U32* ids);
    void begin(Kind kind, U32 id);
    void end(Kind kind);
    // true when result(id) would not have to wait
    bool ready(U32 id);
    // the result of the latest begin under this id (blocks for timer queries; occlusion queries not ready answer 'visible')
    U64 result(U32 id);
}

namespace LLVkWorker
{
    void query_begin(unsigned kind, unsigned id);
    void query_end(unsigned kind);
    // the worker's answer for a query id: available (want_result false) or the result
    bool query_value(unsigned id, bool want_result, unsigned long long& value);
}

#endif // LL_VULKAN

#endif // LL_LLVKQUERY_H
