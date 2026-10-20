/**
 * @file llrenderpass.h
 * @brief F3.5a: a render pass as a value the producer writes down (no GL, no Vulkan in this header).
 *
 * Today a pass is a pair of calls on a target: LLRenderTarget::bindTarget() ... draws ... flush(), with an optional clear() in between, and the
 * "previous target" was a linked list inside the targets (mPreviousRT: now a stack of open passes inside llrendertarget.cpp). RenderPassScope is the same pair as one object: the constructor binds the
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
 * (it would be in the stack of open passes twice), a bindTarget() of a target without a frame buffer, a flush() of a target that is not the innermost bound
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

// the load op of a pass that clears, with its value: colour (and depth to 1) of the targets the mask names (the colour attachments / the depth buffer, the target's own: what clear(mask) did)
struct Clear
{
    F32 mColor[4];
    U32 mMask;
    Clear(F32 r, F32 g, F32 b, F32 a, U32 mask = 0xFFFFFFFF) : mMask(mask) { mColor[0] = r; mColor[1] = g; mColor[2] = b; mColor[3] = a; }
};

// clear_color null: clear to the renderer's current clear colour (LLRender::setClearColor); load Load: no clear
void begin(LLRenderTarget& target, LoadOp load, U32 clear_mask, const F32* clear_color, const char* file, int line);
void end(LLRenderTarget& target, const char* file, int line);
void suspend(LLRenderTarget& target, const char* file, int line);       // ends the pass on the target (flush) ...
void resume(LLRenderTarget& target, const char* file, int line);        // ... and takes it up again (bindTarget): the pass of a caller that a callee has to interrupt

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
        RenderPass::begin(target, load, clear_mask, nullptr, file, line);
    }
    // a pass that clears to its own value (the clear colour is an argument of the pass, not a global that has to be set before and put back)
    RenderPassScope(LLRenderTarget& target, const RenderPass::Clear& clear, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mTarget(&target), mFile(file), mLine(line)
    {
        RenderPass::begin(target, RenderPass::LoadOp::Clear, clear.mMask, clear.mColor, file, line);
    }
    // a scope whose pass begins later (begin(): the pass crosses a block or a function: the scope is declared where it ends)
    RenderPassScope() : mTarget(nullptr), mFile(nullptr), mLine(0) {}
    void begin(LLRenderTarget& target, RenderPass::LoadOp load = RenderPass::LoadOp::Load, U32 clear_mask = 0xFFFFFFFF,
               const char* file = __builtin_FILE(), int line = __builtin_LINE())
    {
        end();
        mTarget = &target;
        mFile = file;
        mLine = line;
        RenderPass::begin(target, load, clear_mask, nullptr, file, line);
    }
    void begin(LLRenderTarget& target, const RenderPass::Clear& clear, const char* file = __builtin_FILE(), int line = __builtin_LINE())
    {
        end();
        mTarget = &target;
        mFile = file;
        mLine = line;
        RenderPass::begin(target, RenderPass::LoadOp::Clear, clear.mMask, clear.mColor, file, line);
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

// the pass on a target is interrupted for the lifetime of this object (or until resume()): the idiom flush(); ...other passes...; bindTarget();
class RenderPassSuspend
{
public:
    explicit RenderPassSuspend(LLRenderTarget& target, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mTarget(&target), mFile(file), mLine(line)
    {
        RenderPass::suspend(target, file, line);
    }
    ~RenderPassSuspend() { resume(); }
    RenderPassSuspend(const RenderPassSuspend&) = delete;
    RenderPassSuspend& operator=(const RenderPassSuspend&) = delete;

    void resume()
    {
        if (mTarget)
        {
            RenderPass::resume(*mTarget, mFile, mLine);
            mTarget = nullptr;
        }
    }

private:
    LLRenderTarget* mTarget;
    const char* mFile;
    int mLine;
};

#endif
