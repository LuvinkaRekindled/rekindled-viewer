/**
 * @file llrenderpass.h
 * @brief F3.5a: a render pass as a value the producer writes down (no GL, no Vulkan in this header).
 *
 * Today a pass is a pair of calls on a target: LLRenderTarget::bindTarget() ... draws ... flush(), with an optional clear() in between, and the
 * "previous target" is a linked list inside the targets (mPreviousRT). RenderPassScope is the same pair as one object: the constructor binds the
 * target (and clears it when the load op says so), the destructor flushes it, and the nesting of scopes is the nesting of passes. It is written on
 * the existing bindTarget() / flush() (they stay, as the thing the scope calls, until the last site is migrated: F3.5b), so a pass made with a scope
 * and a pass made with the pair are the same pass for the worker.
 *
 *   {
 *       RenderPassScope pass(target, RenderPass::LoadOp::Clear);       was: target.bindTarget(); target.clear();  ...  target.flush();
 *       ... draws ...
 *   }
 *
 * Counters: with LL_VK_OPSTATS (or LL_VK_PHASES) the VkOps "worker operations" line has a "render targets (producer)" part: binds, nested binds,
 * flushes, clears (how many directly after their bind, with no draw in between), the deepest nesting, and the bind / clear call sites
 * (file:line, through default arguments like the state guards) per frame.
 *
 * LL_VK_PASS_CHECK=1: the first occurrence per call site is logged (category PassCheck) for: a bindTarget() of a target that is already bound
 * (a cycle in the mPreviousRT chain), a bindTarget() of a target without a frame buffer, a flush() of a target that is not the innermost bound
 * one (bad nesting: the chain is not a stack), a flush() without a bind, a clear() of a target that is not the innermost bound one, a target
 * released while bound, and a target still bound at the end of the frame (a missing flush; the message names the bind site).
 */

#ifndef LL_LLRENDERPASS_H
#define LL_LLRENDERPASS_H

#include "stdtypes.h"
#include <string>

class LLRenderTarget;

namespace RenderPass
{

enum class LoadOp : U8 { Load, Clear };                 // what the pass starts with: the target's contents, or a clear of colour and depth (the common bindTarget(); clear(); shape)

void begin(LLRenderTarget& target, LoadOp load, U32 clear_mask, const char* file, int line);
void end(LLRenderTarget& target, const char* file, int line);

// before the present: LL_VK_PASS_CHECK=1 reports a target that is still bound
void frameEnd();
// the VkOps part for the render targets of the producer (counts since the last call, per frame)
std::string report(size_t frames);

}

class RenderPassScope
{
public:
    explicit RenderPassScope(LLRenderTarget& target, RenderPass::LoadOp load = RenderPass::LoadOp::Load, U32 clear_mask = 0xFFFFFFFF,
                             const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mTarget(&target), mFile(file), mLine(line)
    {
        RenderPass::begin(target, load, clear_mask, file, line);
    }
    ~RenderPassScope() { end(); }
    RenderPassScope(const RenderPassScope&) = delete;
    RenderPassScope& operator=(const RenderPassScope&) = delete;

    // ends the pass before the end of the C++ scope (the destructor then does nothing)
    void end()
    {
        if (mTarget)
        {
            RenderPass::end(*mTarget, mFile, mLine);
            mTarget = nullptr;
        }
    }
    LLRenderTarget* target() const { return mTarget; }

private:
    LLRenderTarget* mTarget;
    const char* mFile;
    int mLine;
};

#endif
