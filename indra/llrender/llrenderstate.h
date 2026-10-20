/**
 * @file llrenderstate.h
 * @brief F3.2: the render state vocabulary of the pass code: typed values and scoped guards (no GL, no Vulkan in this header).
 *
 * The state itself is the producer's block (llvkstate.h, LLVkState::Block). A guard saves the block's field when it is made, changes it when the
 * requested value differs (after gGL.flush(): pending immediate-mode vertices belong to the old state) and puts the saved value back when it
 * goes out of scope. There is no second copy of "what the state is" (LLGLState's map, LLGLDepthTest's statics): the block is the only truth, so a
 * path that set the state some other way (a render target, LLRender::setScissor ...) cannot leave a stale value for a guard to restore.
 *
 *   BlendScope blend(true);                         was: LLGLEnable blend(GL_BLEND);
 *   CullScope cull(false);                          was: LLGLDisable cull(GL_CULL_FACE);
 *   BlendScope blend(enabled);                      was: LLGLState blend(GL_BLEND, enabled);
 *   DepthScope depth(true, false, CompareOp::Less); was: LLGLDepthTest depth(true, false, GL_LESS);
 *   BlendScope blend(cond, false-or-true);          was: LLGLEnable blend(cond ? GL_BLEND : 0);   (second argument 'active': false changes nothing)
 *
 * LL_VK_STATE_GUARD_CHECK=1: when a guard is destroyed the block must still hold what the guard set; if not, something changed the state while the guard
 * was alive (and the guard puts back its saved value over it). The first occurrence per guard site is logged with the file and line of the constructor.
 * (The old LLGLState / LLGLEnable / LLGLDisable / LLGLDepthTest classes and their GL-number setters are gone: F3.2e.)
 */

#ifndef LL_LLRENDERSTATE_H
#define LL_LLRENDERSTATE_H

#include "stdtypes.h"
#include <string>

namespace LLRenderState
{

enum class CullFace : U8 { None, Back, Front, FrontAndBack };           // None: leave the face as it is
enum class PolygonMode : U8 { Fill, Line, Point };
enum class CompareOp : U8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class StencilOp : U8 { Keep, Zero, Replace, IncrClamp, DecrClamp, Invert, IncrWrap, DecrWrap };
enum class BlendFactor : U8 { Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, DstColor, OneMinusDstColor, SrcAlphaSaturate };

// ---- the machinery behind the guards (llvkstate.cpp) ----
// a guarded field of the block, with its value packed into a U32
enum class Kind : U8
{
    Blend,              // 0 / 1
    Scissor,            // 0 / 1
    Stencil,            // 0 / 1   (the stencil test)
    PolygonOffset,      // 0 / 1   (fill)
    DepthClamp,         // 0 / 1
    Cull,               // bit 0: culling on, bits 1-2: CullFace (never None in a stored value)
    Depth,              // bit 0: depth test, bit 1: depth write mask, bits 2-4: CompareOp
    DepthTest,          // 0 / 1   (the depth test alone: write mask and function untouched)
    DepthReadOnly,      // 0 / 1   (F3.5c: the draws of this stage see the depth attachment read-only: it is sampled, never written; the pass takes the read-only layout)
    Count
};

extern bool sGuardCheck;                                                                    // LL_VK_STATE_GUARD_CHECK=1
U32 read(Kind kind);                                                                     // the field now
U32 exchange(Kind kind, U32 value, const char* file = nullptr, int line = 0);                   // flush if it changes, set it, return what it was (file/line: LL_VK_STATE_WHY)
extern bool sWhy;                                                                           // LL_VK_STATE_WHY=1: which writers cause the CMD_STATE records (top 15 per 600 frames in the log)
extern bool sInGuard;
void noteWriter(const char* file, int line, int kind);                                      // kind: Kind value, -1 any
void noteDirect(const char* what);                                                          // a direct LLVkState setter
void leave(Kind kind, U32 saved, U32 set, const char* file, int line);                      // check (when sGuardCheck), then put 'saved' back if the guard changed anything

void whyEmit(const std::string& diff);                                                      // llvkstate: a CMD_STATE record is written
std::string guardReport();                                                                  // for the state stats line

constexpr U32 packCull(bool on, CullFace face) { return (on ? 1u : 0u) | ((U32)face << 1); }
constexpr U32 packDepth(bool test, bool write, CompareOp op) { return (test ? 1u : 0u) | (write ? 2u : 0u) | ((U32)op << 2); }

// ---- guards ----
// 'active' false: the guard does nothing at all (the conditional-cap sites: cond ? GL_BLEND : 0)
// the file and line of the constructor's caller are only used by LL_VK_STATE_GUARD_CHECK
struct Keep {};                                                                             // 'do not change the field now, I will call setEnabled()'

template<Kind K>
class FlagScope
{
public:
    explicit FlagScope(bool on, bool active = true, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mActive(active), mSaved(0), mSet(0), mLine(line), mFile(file)
    {
        if (mActive)
        {
            mSaved = exchange(K, on ? 1u : 0u, file, line);
            mSet = on ? 1u : 0u;
        }
    }
    // the scope keeps the field as it is until setEnabled() is called (what LLGLState(cap) with the default 'current state' was for)
    explicit FlagScope(Keep, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mActive(true), mSaved(read(K)), mSet(mSaved), mLine(line), mFile(file)
    {
    }
    FlagScope(const FlagScope&) = delete;
    FlagScope& operator=(const FlagScope&) = delete;
    ~FlagScope()
    {
        if (mActive && (mSet != mSaved || sGuardCheck))
        {
            leave(K, mSaved, mSet, mFile, mLine);
        }
    }
    void setEnabled(bool on)
    {
        if (mActive)
        {
            exchange(K, on ? 1u : 0u, mFile, mLine);
            mSet = on ? 1u : 0u;
        }
    }
    void enable() { setEnabled(true); }
    void disable() { setEnabled(false); }

private:
    bool mActive;
    U8 mSaved;
    U8 mSet;
    int mLine;
    const char* mFile;
};

using BlendScope = FlagScope<Kind::Blend>;
using ScissorScope = FlagScope<Kind::Scissor>;
using StencilScope = FlagScope<Kind::Stencil>;
using PolygonOffsetScope = FlagScope<Kind::PolygonOffset>;
using DepthClampScope = FlagScope<Kind::DepthClamp>;
using DepthTestScope = FlagScope<Kind::DepthTest>;
using DepthReadOnlyScope = FlagScope<Kind::DepthReadOnly>;       // F3.5c: DepthReadOnlyScope ro(true);  the stage samples the depth buffer it tests against and writes no depth

// culling on or off; 'face' None leaves the face that is set (LLGLEnable(GL_CULL_FACE) never changed it)
class CullScope
{
public:
    explicit CullScope(bool on, CullFace face = CullFace::None, bool active = true, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mActive(active), mSaved(0), mSet(0), mLine(line), mFile(file)
    {
        if (mActive)
        {
            mSaved = exchange(Kind::Cull, packCull(on, face), file, line);
            mSet = packCull(on, face == CullFace::None ? (CullFace)(mSaved >> 1) : face);
        }
    }
    // cull on or off, 'active' false: nothing
    CullScope(bool on, bool active, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : CullScope(on, CullFace::None, active, file, line) {}
    CullScope(const CullScope&) = delete;
    CullScope& operator=(const CullScope&) = delete;
    ~CullScope()
    {
        if (mActive && (mSet != mSaved || sGuardCheck))
        {
            leave(Kind::Cull, mSaved, mSet, mFile, mLine);
        }
    }

private:
    bool mActive;
    U8 mSaved;
    U8 mSet;
    int mLine;
    const char* mFile;
};

// depth test, depth write and depth function together (was LLGLDepthTest): no depth writes without the test
class DepthScope
{
public:
    explicit DepthScope(bool test, bool write = true, CompareOp op = CompareOp::LessEqual, bool active = true, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mActive(active), mSaved(0), mSet(0), mLine(line), mFile(file)
    {
        if (mActive)
        {
            mSet = packDepth(test, test && write, op);
            mSaved = exchange(Kind::Depth, mSet, file, line);
        }
    }
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;
    ~DepthScope()
    {
        if (mActive && (mSet != mSaved || sGuardCheck))
        {
            leave(Kind::Depth, mSaved, mSet, mFile, mLine);
        }
    }

private:
    bool mActive;
    U8 mSaved;
    U8 mSet;
    int mLine;
    const char* mFile;
};

}

#endif // LL_LLRENDERSTATE_H
