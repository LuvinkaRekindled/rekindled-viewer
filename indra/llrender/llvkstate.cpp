/**
 * @file llvkstate.cpp
 * @brief F1.1: the producer-side render state context (see llvkstate.h).
 */

#include "linden_common.h"

#include "llglheaders.h"

#if defined(LL_VULKAN)

#include "llvkstate.h"
#include "llframetimer.h"

#include <atomic>
#include <cstring>
#include <intrin.h>

using namespace LLVk;

namespace LLVkState
{

// the state as the producer holds it: the block, plus the viewer's separate inputs that one block field is derived from
struct Context
{
    Block mBlock;
    Block mTaken;                   // the last block written as a record
    bool mTakenValid = false;
    bool mCullOn = false;
    VkCullModeFlags mCullFace = VK_CULL_MODE_BACK_BIT;
    // counters
    U64 mCalls = 0, mOffThread = 0, mBlocks = 0, mUnknownFactor = 0;
};

namespace
{

Context gCtx;                           // the main producer's
thread_local Context* tlsCtx = nullptr;   // the context a sub-stream thread writes (null: the main producer's, subject to the owner rule below)
inline Context& C() { return tlsCtx ? *tlsCtx : gCtx; }

VkCompareOp compareOp(U32 f)
{
    switch (f)
    {
    case GL_NEVER: return VK_COMPARE_OP_NEVER;
    case GL_LESS: return VK_COMPARE_OP_LESS;
    case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkBlendFactor blendFactor(U32 f)
{
    switch (f)
    {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
        if (C().mUnknownFactor++ == 0)
        {
            LL_WARNS("VkState") << "unsupported blend factor 0x" << std::hex << f << std::dec << LL_ENDL;
        }
        return VK_BLEND_FACTOR_ONE;
    }
}

VkStencilOp stencilOp(U32 op)
{
    switch (op)
    {
    case 0: return VK_STENCIL_OP_ZERO;                  // GL_ZERO
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;                 // GL_KEEP
    }
}

void recomputeCull()
{
    C().mBlock.mRaster.mCull = C().mCullOn ? C().mCullFace : VK_CULL_MODE_NONE;
}

void recomputeDepthWrite()
{
    // GL never writes depth while the depth test is disabled; Vulkan does when writes are enabled
    C().mBlock.mDepth.mDepthWrite = C().mBlock.mDepthMask && C().mBlock.mDepth.mDepthTest;
}

} // anonymous namespace

bool Block::operator==(const Block& o) const
{
    return mDepthMask == o.mDepthMask && mRaster == o.mRaster && mDepth == o.mDepth && mBlend == o.mBlend && mViewportSet == o.mViewportSet && mScissorOn == o.mScissorOn
        && memcmp(mViewport, o.mViewport, sizeof(mViewport)) == 0 && memcmp(mScissor, o.mScissor, sizeof(mScissor)) == 0;
}

void reset()
{
    Context& c = gCtx;
    c = Context();
    Block& b = c.mBlock;
    // the defaults of GL (what the old state machine starts from)
    b.mRaster.mCull = VK_CULL_MODE_NONE;
    b.mRaster.mPolygonMode = VK_POLYGON_MODE_FILL;
    b.mRaster.mLineWidth = 1.f;
    b.mRaster.mDepthBias = false;
    b.mRaster.mDepthClamp = false;
    b.mRaster.mBiasConstant = 0.f;
    b.mRaster.mBiasSlope = 0.f;
    // Vulkan's face-orientation area has the opposite sign of GL's for the same framebuffer coordinates (its y axis points down),
    // and the mirror keeps GL's positive viewport, so GL's counter-clockwise front faces are clockwise here
    b.mRaster.mFrontFace = VK_FRONT_FACE_CLOCKWISE;
    b.mDepth.mDepthTest = false;
    b.mDepth.mDepthWrite = false;           // mask true and test false
    b.mDepthMask = true;
    b.mDepth.mDepthOp = VK_COMPARE_OP_LESS;
    b.mBlend.mEnable = false;
    b.mBlend.mSrcColor = b.mBlend.mSrcAlpha = VK_BLEND_FACTOR_ONE;
    b.mBlend.mDstColor = b.mBlend.mDstAlpha = VK_BLEND_FACTOR_ZERO;
    b.mBlend.mWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(b.mViewport, 0, sizeof(b.mViewport));
    memset(b.mScissor, 0, sizeof(b.mScissor));
    b.mViewportSet = false;
    b.mScissorOn = false;
}

namespace
{
struct InitContext { InitContext() { reset(); } } gInitContext;     // the context starts from the GL defaults before the first call
}

// the setters belong to the thread that first calls one (the main thread: the viewer starts drawing there); a call from another thread is dropped and counted
U32 gOwner = 0;
inline bool ownThread(const char* what)
{
    if (tlsCtx)
    {
        return true;                        // a sub-stream thread writes its own context
    }
    const U32 tid = __readgsdword(0x48);
    if (!gOwner)
    {
        gOwner = tid;
    }
    if (tid != gOwner)
    {
        noteOffThread(what);
        return false;
    }
    return true;
}

#define CTX_CALL() if (!ownThread(__func__)) return; ++C().mCalls
static bool scissor_trace_now();

void setEnable(U32 cap, bool on)
{
    CTX_CALL();
    Block& b = C().mBlock;
    switch (cap)
    {
    case GL_BLEND: b.mBlend.mEnable = on; break;
    case GL_DEPTH_TEST: b.mDepth.mDepthTest = on; recomputeDepthWrite(); break;
    case GL_CULL_FACE: C().mCullOn = on; recomputeCull(); break;
    case GL_SCISSOR_TEST:
        b.mScissorOn = on;
        if (scissor_trace_now())
        {
            LL_INFOS("ScissorTrace") << "frame " << LLFrameTimer::getFrameCount() << " scissor test " << (on ? "on" : "off") << (tlsCtx ? " [sub-stream]" : " [main]") << LL_ENDL;
        }
        break;
    case GL_STENCIL_TEST: b.mDepth.mStencilTest = on; break;
    case GL_POLYGON_OFFSET_FILL: b.mRaster.mDepthBias = on; break;
    case GL_DEPTH_CLAMP: b.mRaster.mDepthClamp = on; break;
    default: break;                         // other capabilities do not change what a draw does here
    }
}

void setBlendFunc(U32 src_rgb, U32 dst_rgb, U32 src_a, U32 dst_a)
{
    CTX_CALL();
    BlendState& bs = C().mBlock.mBlend;
    bs.mSrcColor = blendFactor(src_rgb);
    bs.mDstColor = blendFactor(dst_rgb);
    bs.mSrcAlpha = blendFactor(src_a);
    bs.mDstAlpha = blendFactor(dst_a);
}

void setDepthFunc(U32 func)
{
    CTX_CALL();
    C().mBlock.mDepth.mDepthOp = compareOp(func);
}

void setDepthMask(bool flag)
{
    CTX_CALL();
    C().mBlock.mDepthMask = flag;
    recomputeDepthWrite();
}

void setColorMask(bool r, bool g, bool b, bool a)
{
    CTX_CALL();
    C().mBlock.mBlend.mWriteMask = (r ? VK_COLOR_COMPONENT_R_BIT : 0) | (g ? VK_COLOR_COMPONENT_G_BIT : 0)
        | (b ? VK_COLOR_COMPONENT_B_BIT : 0) | (a ? VK_COLOR_COMPONENT_A_BIT : 0);
}

void setCullFace(U32 mode)
{
    CTX_CALL();
    C().mCullFace = mode == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : (mode == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
    recomputeCull();
}

void setPolygonOffset(F32 factor, F32 units)
{
    CTX_CALL();
    C().mBlock.mRaster.mBiasSlope = factor;
    C().mBlock.mRaster.mBiasConstant = units;
}

void setPolygonMode(U32 mode)
{
    CTX_CALL();
    C().mBlock.mRaster.mPolygonMode = mode == GL_LINE ? VK_POLYGON_MODE_LINE : (mode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
}

void setLineWidth(F32 width)
{
    CTX_CALL();
    C().mBlock.mRaster.mLineWidth = width;
}

void setStencilFunc(U32 func, S32 ref, U32 mask)
{
    CTX_CALL();
    DepthStencilState& ds = C().mBlock.mDepth;
    ds.mStencilOp = compareOp(func);
    ds.mReference = (U32)ref;
    ds.mCompareMask = mask;
}

void setStencilOp(U32 sfail, U32 dpfail, U32 dppass)
{
    CTX_CALL();
    DepthStencilState& ds = C().mBlock.mDepth;
    ds.mFail = stencilOp(sfail);
    ds.mDepthFail = stencilOp(dpfail);
    ds.mPass = stencilOp(dppass);
}

void setStencilMask(U32 mask)
{
    CTX_CALL();
    C().mBlock.mDepth.mWriteMask = mask;
}

void setViewport(S32 x, S32 y, S32 w, S32 h)
{
    CTX_CALL();
    Block& b = C().mBlock;
    b.mViewport[0] = x; b.mViewport[1] = y; b.mViewport[2] = w; b.mViewport[3] = h;
    b.mViewportSet = true;
}

// LL_VK_SCISSOR_TRACE=<frame>: the scissor calls of that frame and the next, with the context they went to (a debugging aid for the sub-stream)
static std::atomic<U32> gScissorTraceFirst{0};

void setScissorTraceFrame(U32 frame)       // the viewer sets it relative to the first world frame (LL_VK_SCISSOR_TRACE=rel)
{
    gScissorTraceFirst = frame;
}

static bool scissor_trace_now()
{
    static const char* e = getenv("LL_VK_SCISSOR_TRACE");
    if (!e)
    {
        return false;
    }
    static const bool relative = (e[0] == 'r');
    const U32 first = relative ? gScissorTraceFirst.load() : (U32)atoi(e);
    if (!first)
    {
        return false;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    return frame == first || frame == first + 1 || frame == first + 60 || frame == first + 61;
}

void setScissor(S32 x, S32 y, S32 w, S32 h)
{
    CTX_CALL();
    Block& b = C().mBlock;
    b.mScissor[0] = x; b.mScissor[1] = y; b.mScissor[2] = w; b.mScissor[3] = h;
    if (scissor_trace_now())
    {
        LL_INFOS("ScissorTrace") << "frame " << LLFrameTimer::getFrameCount() << " rect " << x << "," << y << "," << w << "," << h << (tlsCtx ? " [sub-stream]" : " [main]") << LL_ENDL;
    }
}

void forceEmit()
{
    C().mTakenValid = false;
}

Context* cloneContext()
{
    return cloneContext(&C());
}

Context* cloneContext(const Context* from)
{
    Context* c = new Context(*from);
    c->mTakenValid = false;                 // its first record carries the whole block
    c->mCalls = c->mOffThread = c->mBlocks = c->mUnknownFactor = 0;
    return c;
}

void destroyContext(Context* ctx)
{
    delete ctx;
}

const Block& blockOf(const Context* ctx)
{
    return ctx->mBlock;
}

void adoptChanges(const Context* seed, const Context* final)
{
    Context& m = C();
#define ADOPT(field) if (final->field != seed->field) { m.field = final->field; }
    ADOPT(mBlock.mRaster.mCull) ADOPT(mBlock.mRaster.mFrontFace) ADOPT(mBlock.mRaster.mPolygonMode) ADOPT(mBlock.mRaster.mLineWidth) ADOPT(mBlock.mRaster.mDepthBias)
    ADOPT(mBlock.mRaster.mDepthClamp) ADOPT(mBlock.mRaster.mBiasConstant) ADOPT(mBlock.mRaster.mBiasSlope)
    ADOPT(mBlock.mDepth.mDepthTest) ADOPT(mBlock.mDepth.mDepthWrite) ADOPT(mBlock.mDepth.mDepthOp) ADOPT(mBlock.mDepth.mStencilTest) ADOPT(mBlock.mDepth.mStencilOp)
    ADOPT(mBlock.mDepth.mFail) ADOPT(mBlock.mDepth.mDepthFail) ADOPT(mBlock.mDepth.mPass) ADOPT(mBlock.mDepth.mCompareMask) ADOPT(mBlock.mDepth.mWriteMask) ADOPT(mBlock.mDepth.mReference)
    ADOPT(mBlock.mBlend.mEnable) ADOPT(mBlock.mBlend.mSrcColor) ADOPT(mBlock.mBlend.mDstColor) ADOPT(mBlock.mBlend.mColorOp) ADOPT(mBlock.mBlend.mSrcAlpha)
    ADOPT(mBlock.mBlend.mDstAlpha) ADOPT(mBlock.mBlend.mAlphaOp) ADOPT(mBlock.mBlend.mWriteMask)
    ADOPT(mBlock.mDepthMask) ADOPT(mBlock.mViewportSet) ADOPT(mBlock.mScissorOn)
    ADOPT(mBlock.mViewport[0]) ADOPT(mBlock.mViewport[1]) ADOPT(mBlock.mViewport[2]) ADOPT(mBlock.mViewport[3])
    ADOPT(mBlock.mScissor[0]) ADOPT(mBlock.mScissor[1]) ADOPT(mBlock.mScissor[2]) ADOPT(mBlock.mScissor[3])
    ADOPT(mCullOn) ADOPT(mCullFace)
#undef ADOPT
    m.mTakenValid = false;
}

std::string describeDifference(const Block& a, const Block& b)
{
    std::string d;
    auto note = [&d](const char* what) { d += what; d += ' '; };
    const RasterState& ra = a.mRaster;
    const RasterState& rb = b.mRaster;
    if (ra.mCull != rb.mCull) note("cull");
    if (ra.mFrontFace != rb.mFrontFace) note("frontFace");
    if (ra.mPolygonMode != rb.mPolygonMode) note("polygonMode");
    if (ra.mLineWidth != rb.mLineWidth) note("lineWidth");
    if (ra.mDepthBias != rb.mDepthBias || ra.mBiasConstant != rb.mBiasConstant || ra.mBiasSlope != rb.mBiasSlope) note("polygonOffset");
    if (ra.mDepthClamp != rb.mDepthClamp) note("depthClamp");
    const DepthStencilState& da = a.mDepth;
    const DepthStencilState& db = b.mDepth;
    if (da.mDepthTest != db.mDepthTest) note("depthTest");
    if (da.mDepthWrite != db.mDepthWrite || a.mDepthMask != b.mDepthMask) note("depthWrite");
    if (da.mDepthOp != db.mDepthOp) note("depthFunc");
    if (da.mStencilTest != db.mStencilTest || da.mStencilOp != db.mStencilOp || da.mFail != db.mFail || da.mDepthFail != db.mDepthFail || da.mPass != db.mPass
        || da.mCompareMask != db.mCompareMask || da.mWriteMask != db.mWriteMask || da.mReference != db.mReference) note("stencil");
    const BlendState& ba = a.mBlend;
    const BlendState& bb = b.mBlend;
    if (ba.mEnable != bb.mEnable) note("blendEnable");
    if (ba.mSrcColor != bb.mSrcColor || ba.mDstColor != bb.mDstColor || ba.mSrcAlpha != bb.mSrcAlpha || ba.mDstAlpha != bb.mDstAlpha || ba.mColorOp != bb.mColorOp || ba.mAlphaOp != bb.mAlphaOp) note("blendFunc");
    if (ba.mWriteMask != bb.mWriteMask) note("colorMask");
    if (memcmp(a.mViewport, b.mViewport, sizeof(a.mViewport)) != 0 || a.mViewportSet != b.mViewportSet) note("viewport");
    if (memcmp(a.mScissor, b.mScissor, sizeof(a.mScissor)) != 0) note("scissorRect");
    if (a.mScissorOn != b.mScissorOn) note("scissorTest");
    return d;
}

void bindContext(Context* ctx)
{
    tlsCtx = ctx;
}

bool takeChanged(Block& out)
{
    Context& c = C();           // the context of the thread that is writing the record: a sub-stream's own, not the main producer's
    if (c.mTakenValid && c.mBlock == c.mTaken)
    {
        return false;
    }
    c.mTaken = c.mBlock;
    c.mTakenValid = true;
    ++c.mBlocks;
    out = c.mBlock;
    return true;
}

const Block& current()
{
    return C().mBlock;
}

namespace
{
Block gReceived;
bool gReceivedValid = false;
}

void receive(const Block& b)
{
    gReceived = b;
    gReceivedValid = true;
}

const Block* received()
{
    return gReceivedValid ? &gReceived : nullptr;
}

void toRenderer(const Block& b, bool has_depth, RasterState& rs, DepthStencilState& ds, BlendState& bs)
{
    rs = b.mRaster;
    ds = DepthStencilState();
    ds.mDepthTest = b.mDepth.mDepthTest && has_depth;
    ds.mDepthWrite = b.mDepth.mDepthWrite && has_depth;
    ds.mDepthOp = b.mDepth.mDepthOp;
    ds.mStencilTest = false;        // the mirror's depth images carry no stencil yet (F1.1 step 4)
    bs = b.mBlend;
}

void noteOffThread(const char* what)
{
    if (C().mOffThread++ < 5)
    {
        LL_WARNS("VkState") << what << " came from thread " << __readgsdword(0x48) << " (owner " << gOwner << "), not the main producer: dropped" << LL_ENDL;
    }
}

std::string statsReport()
{
    Context& c = gCtx;
    char buf[260];
    snprintf(buf, sizeof(buf), "state context: %llu setter calls, %llu blocks written, %llu setter calls from other threads (ignored), %llu unknown blend factors", c.mCalls, c.mBlocks, c.mOffThread, c.mUnknownFactor);
    c.mCalls = c.mBlocks = c.mOffThread = c.mUnknownFactor = 0;
    return buf;
}

} // namespace LLVkState

#endif // LL_VULKAN
