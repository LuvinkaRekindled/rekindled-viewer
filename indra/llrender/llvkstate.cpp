/**
 * @file llvkstate.cpp
 * @brief F1.1: the producer-side render state context (see llvkstate.h).
 */

#include "linden_common.h"

#define LL_VK_NO_INTERCEPT      // GL constants only; this file never calls GL
#include "llglheaders.h"

#if defined(LL_VULKAN)

#include "llvkstate.h"

#include <cstring>
#include <intrin.h>

using namespace LLVk;

namespace LLVkState
{

namespace
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

Context gCtx;

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
        if (gCtx.mUnknownFactor++ == 0)
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
    gCtx.mBlock.mRaster.mCull = gCtx.mCullOn ? gCtx.mCullFace : VK_CULL_MODE_NONE;
}

void recomputeDepthWrite()
{
    // GL never writes depth while the depth test is disabled; Vulkan does when writes are enabled
    gCtx.mBlock.mDepth.mDepthWrite = gCtx.mBlock.mDepthMask && gCtx.mBlock.mDepth.mDepthTest;
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

#define CTX_CALL() if (!ownThread(__func__)) return; ++gCtx.mCalls

void setEnable(U32 cap, bool on)
{
    CTX_CALL();
    Block& b = gCtx.mBlock;
    switch (cap)
    {
    case GL_BLEND: b.mBlend.mEnable = on; break;
    case GL_DEPTH_TEST: b.mDepth.mDepthTest = on; recomputeDepthWrite(); break;
    case GL_CULL_FACE: gCtx.mCullOn = on; recomputeCull(); break;
    case GL_SCISSOR_TEST: b.mScissorOn = on; break;
    case GL_STENCIL_TEST: b.mDepth.mStencilTest = on; break;
    case GL_POLYGON_OFFSET_FILL: b.mRaster.mDepthBias = on; break;
    case GL_DEPTH_CLAMP: b.mRaster.mDepthClamp = on; break;
    default: break;                         // other capabilities do not change what a draw does here
    }
}

void setBlendFunc(U32 src_rgb, U32 dst_rgb, U32 src_a, U32 dst_a)
{
    CTX_CALL();
    BlendState& bs = gCtx.mBlock.mBlend;
    bs.mSrcColor = blendFactor(src_rgb);
    bs.mDstColor = blendFactor(dst_rgb);
    bs.mSrcAlpha = blendFactor(src_a);
    bs.mDstAlpha = blendFactor(dst_a);
}

void setDepthFunc(U32 func)
{
    CTX_CALL();
    gCtx.mBlock.mDepth.mDepthOp = compareOp(func);
}

void setDepthMask(bool flag)
{
    CTX_CALL();
    gCtx.mBlock.mDepthMask = flag;
    recomputeDepthWrite();
}

void setColorMask(bool r, bool g, bool b, bool a)
{
    CTX_CALL();
    gCtx.mBlock.mBlend.mWriteMask = (r ? VK_COLOR_COMPONENT_R_BIT : 0) | (g ? VK_COLOR_COMPONENT_G_BIT : 0)
        | (b ? VK_COLOR_COMPONENT_B_BIT : 0) | (a ? VK_COLOR_COMPONENT_A_BIT : 0);
}

void setCullFace(U32 mode)
{
    CTX_CALL();
    gCtx.mCullFace = mode == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : (mode == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
    recomputeCull();
}

void setPolygonOffset(F32 factor, F32 units)
{
    CTX_CALL();
    gCtx.mBlock.mRaster.mBiasSlope = factor;
    gCtx.mBlock.mRaster.mBiasConstant = units;
}

void setPolygonMode(U32 mode)
{
    CTX_CALL();
    gCtx.mBlock.mRaster.mPolygonMode = mode == GL_LINE ? VK_POLYGON_MODE_LINE : (mode == GL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
}

void setLineWidth(F32 width)
{
    CTX_CALL();
    gCtx.mBlock.mRaster.mLineWidth = width;
}

void setStencilFunc(U32 func, S32 ref, U32 mask)
{
    CTX_CALL();
    DepthStencilState& ds = gCtx.mBlock.mDepth;
    ds.mStencilOp = compareOp(func);
    ds.mReference = (U32)ref;
    ds.mCompareMask = mask;
}

void setStencilOp(U32 sfail, U32 dpfail, U32 dppass)
{
    CTX_CALL();
    DepthStencilState& ds = gCtx.mBlock.mDepth;
    ds.mFail = stencilOp(sfail);
    ds.mDepthFail = stencilOp(dpfail);
    ds.mPass = stencilOp(dppass);
}

void setStencilMask(U32 mask)
{
    CTX_CALL();
    gCtx.mBlock.mDepth.mWriteMask = mask;
}

void setViewport(S32 x, S32 y, S32 w, S32 h)
{
    CTX_CALL();
    Block& b = gCtx.mBlock;
    b.mViewport[0] = x; b.mViewport[1] = y; b.mViewport[2] = w; b.mViewport[3] = h;
    b.mViewportSet = true;
}

void setScissor(S32 x, S32 y, S32 w, S32 h)
{
    CTX_CALL();
    Block& b = gCtx.mBlock;
    b.mScissor[0] = x; b.mScissor[1] = y; b.mScissor[2] = w; b.mScissor[3] = h;
}

void forceEmit()
{
    gCtx.mTakenValid = false;
}

bool takeChanged(Block& out)
{
    Context& c = gCtx;
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
    return gCtx.mBlock;
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
    if (gCtx.mOffThread++ < 5)
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
