/**
 * @file llvkbuf.h
 * @brief F1.3: buffers that are named by their id (the handle) and the vertex input state as typed values.
 *
 * Same idea as llvktex.h: a buffer write names its buffer, it does not depend on what is bound to which target; uniform blocks are bound by index with
 * their own record; nothing here is a GL binding point.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKBUF_H
#define LL_LLVKBUF_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llvkasync.h"
#include "llvkstate.h"

namespace LLVkBuf
{
    // handles: the producer allocates the ids, the worker makes and destroys the buffers in stream order
    void create(S32 count, U32* ids);
    void destroy(S32 count, const U32* ids);
    // (re)specifies the storage of a buffer and optionally fills it (target says what the contents are for: vertex, index or uniform data)
    void setData(U32 id, BufferKind kind, S64 size, const void* data);
    // makes a buffer the contents of a numbered uniform block (what the shaders' named blocks read)
    void bindUniformBlock(U32 binding, U32 id);
    // a vertex attribute that has no array (the shader reads this constant); the worker keeps one value per location
    void setConstantAttribute(U32 location, F32 x, F32 y, F32 z, F32 w);
}

namespace LLVkWorker
{
    void uniform_block_bind(unsigned binding, unsigned buffer);
    void draw_arrays(unsigned mode, int first, int count);
    void draw_elements(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices);
    // F3.1: a non-indexed draw whose vertices are given (see LLVkAsync::drawScratch for the layout of `bytes`); they are copied to the frame's vertex scratch
    void draw_scratch(unsigned mode, int count, unsigned mask, const void* bytes);
    void constant_attribute(unsigned location, float x, float y, float z, float w);
    // F2.4 draw records: the worker makes the program and (when given) the vertex binding of a CMD_DRAW2 current, in place of the shader bind / array run records
    void draw_context_apply(void* program, const LLVkAsync::DrawBinding* binding);
    // F2.6: a draw record's texture slot: the unit holds this texture (no target)
    void slot_set(unsigned unit, unsigned name);
}

#endif // LL_VULKAN

#endif // LL_LLVKBUF_H
