/**
 * @file llvktarget.h
 * @brief F1.4: render targets named by their id, clears and the read source of copies.
 *
 * A render target is a handle (the id the producer allocates) with attached textures; the target a draw goes to is set by one record that also carries the number of
 * colour attachments (LLVkAsync::targetState). Clears and copies name what they act on instead of reading "the bound framebuffer" / "the clear colour" state.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKTARGET_H
#define LL_LLVKTARGET_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llvkformat.h"

namespace LLVkTarget
{
    constexpr U32 DEPTH_SLOT = 8;       // attach() slot of the depth attachment; slots 0-7 are the colour attachments

    void create(U32* id);
    void destroy(U32 id);
    // attaches a texture (mip level 0) to a slot of the target; texture 0 detaches
    void attach(U32 target, U32 slot, U32 texture);
    // clears the current target's colour and/or depth attachment (honouring the current colour / depth write masks and scissor, like a GL clear)
    void clear(bool color, bool depth, const F32* rgba);
    // reads a rectangle of colour attachment 0 of 'target' (0: the back buffer) into dst, rows tightly packed (GL format / type pairs as for texture uploads); returns when the data is there
    void readPixels(U32 target, S32 x, S32 y, S32 width, S32 height, PixelFormat format, ComponentType type, void* dst, const char* file = __builtin_FILE(), int line = __builtin_LINE());
}

namespace LLVkWorker
{
    void target_attach(unsigned fbo, unsigned slot, unsigned texture);
    // makes 'fbo' the target draws go to (0: the back buffer); draw_buffers: colour attachments written (0: none, -1: the back buffer)
    void target_state(unsigned fbo, int draw_buffers);
    void clear_target(bool color, bool depth, const float* rgba);
    void read_pixels(unsigned fbo, int x, int y, int width, int height, LLVkFormat::Transfer tr, void* dst);
}

#endif // LL_VULKAN

#endif // LL_LLVKTARGET_H
