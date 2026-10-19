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
    void setData(U32 id, U32 target, S64 size, const void* data, U32 usage);
    // makes a buffer the contents of a numbered uniform block (what the shaders' named blocks read)
    void bindUniformBlock(U32 binding, U32 id);
    // a vertex attribute that has no array (the shader reads this constant); the worker keeps one value per location
    void setConstantAttribute(U32 location, F32 x, F32 y, F32 z, F32 w);
}

namespace LLVkGL
{
    void uniform_block_bind(unsigned binding, unsigned buffer);
    // the vertex input state the draws read: which buffer holds the vertices and the indices, and where each attribute of the shader comes from
    void vertex_input_apply(bool bind_array, unsigned array_buffer, bool bind_elem, unsigned elem_buffer, const LLVkAsync::VertexAttrib* attribs, unsigned count);
    // binds the program and switches the attribute arrays (bit masks of locations); the vertex and index buffers are unbound
    void shader_bind(void* program, unsigned enable_mask, unsigned disable_mask);
    void draw_arrays(unsigned mode, int first, int count);
    void draw_elements(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices);
    void constant_attribute(unsigned location, float x, float y, float z, float w);
    // F2.4 draw records: the worker makes the program and (when given) the vertex binding of a CMD_DRAW2 current, in place of the shader bind / array run records
    void draw_context_apply(void* program, const LLVkAsync::DrawBinding* binding);
    // LL_VK_DRAWREC=1: compares what the producer says the next draw uses (CMD_DRAW_CHECK, right before the old CMD_DRAW) with what the old state machine holds
    void draw_check(void* program, const LLVkState::Block& producer_state, const LLVkAsync::DrawBinding& binding);
    void draw_check_draw();                 // an old CMD_DRAW arrived (verify counts a draw without a check)
    std::string draw_check_report();        // counts since the last call
}

#endif // LL_VULKAN

#endif // LL_LLVKBUF_H
