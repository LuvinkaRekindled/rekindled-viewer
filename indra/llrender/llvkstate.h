/**
 * @file llvkstate.h
 * @brief F1.1: the render state as values, on the producer side (the typed state block and the context that holds it).
 *
 * The viewer's state setters (glEnable, glBlendFunc, glDepthMask, glViewport ... today; LLGLState / LLRender directly after the switch) write the context
 * here, already translated to native Vulkan values. At a draw (or anything else that reads state) the producer compares the context's block with the last
 * block it wrote and, when they differ, writes one CMD_STATE record holding the block. The worker copies the block into the renderer: no GL state machine.
 *
 * Step 1 (this file): dual-write beside the old GL-shaped state calls; the worker verifies the block against what the old path would have produced
 * (LL_VK_STATE_VERIFY=1) and logs mismatches.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef LL_LLVKSTATE_H
#define LL_LLVKSTATE_H

#if defined(LL_VULKAN)

#include "linden_common.h"
#include "llvkrenderer.h"

#include <string>

namespace LLVkState
{
    // everything a draw reads that is not a program, a texture, a buffer or a target: one plain struct, written as a record when it changes
    struct Block
    {
        LLVk::RasterState mRaster;
        LLVk::DepthStencilState mDepth;     // mDepthTest / mDepthWrite as the viewer set them (the worker still ANDs the test with 'the target has depth')
        LLVk::BlendState mBlend;            // mWriteMask is the colour write mask
        S32 mViewport[4];
        S32 mScissor[4];
        bool mDepthMask;                    // the depth write mask as the viewer set it (mDepth.mDepthWrite is mask AND test); clears and glGet answers read it
        bool mViewportSet;                  // a viewport was ever set (the worker keeps the target's own until then)
        bool mScissorOn;
        bool operator==(const Block& o) const;
    };

    // the producer's context (main thread only: the calls below are ignored from other threads, and counted)
    // set* take the GL values the viewer's calls carry today; they translate once, here
    void reset();
    void setEnable(U32 cap, bool on);
    void setBlendFunc(U32 src_rgb, U32 dst_rgb, U32 src_a, U32 dst_a);
    void setDepthFunc(U32 func);
    void setDepthMask(bool flag);
    void setColorMask(bool r, bool g, bool b, bool a);
    void setCullFace(U32 mode);
    void setPolygonOffset(F32 factor, F32 units);
    void setPolygonMode(U32 mode);
    void setLineWidth(F32 width);
    void setStencilFunc(U32 func, S32 ref, U32 mask);
    void setStencilOp(U32 sfail, U32 dpfail, U32 dppass);
    void setStencilMask(U32 mask);
    void setViewport(S32 x, S32 y, S32 w, S32 h);
    void setScissor(S32 x, S32 y, S32 w, S32 h);
    // a setter call arrived on a thread other than the main producer: it is not written to the context (the old path still sees it)
    void noteOffThread(const char* what);

    // makes the next takeChanged() write a block even if nothing changed (the receiver may have lost it)
    void forceEmit();

    // F3: another producer thread (a sub-stream, llvkasync.h) has a context of its own: it starts from 'seed', and the setters of the thread that binds it write it
    struct Context;
    Context* cloneContext();                          // a copy of the calling thread's context: its block and the inputs the block is derived from (cull on/off and face)
    Context* cloneContext(const Context* from);
    void destroyContext(Context* ctx);
    void bindContext(Context* ctx);       // the calling thread (nullptr: unbind)
    const Block& blockOf(const Context* ctx);
    std::string describeDifference(const Block& a, const Block& b);      // which fields differ (empty when none)
    // after work ran in a sub-stream that started from 'seed' and ended in 'final': what that work changed (the fields where final differs from seed) becomes the state of the calling
    // thread's context, as if the work had run in line; everything else stays as it is. The next block written is a whole one.
    void adoptChanges(const Context* seed, const Context* final);

    // the context's block, and whether it differs from the last block taken (then 'out' is set and the last-taken block is updated)
    bool takeChanged(Block& out);
    const Block& current();

    // worker side: the latest block a CMD_STATE record carried (null before the first)
    void receive(const Block& b);
    const Block* received();

    // worker side: what the renderer's native state structs are for a block (the same result the old sync_draw_state computes from the GL values)
    void toRenderer(const Block& b, bool has_depth, LLVk::RasterState& rs, LLVk::DepthStencilState& ds, LLVk::BlendState& bs);

    // counters since the last call (for the phases log): blocks written, calls made, calls ignored because they came from another thread
    std::string statsReport();
}

#endif // LL_VULKAN

#endif // LL_LLVKSTATE_H
