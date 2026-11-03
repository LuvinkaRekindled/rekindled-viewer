/**
 * @file llvkstate.cpp
 * @brief F1.1: the producer-side render state context (see llvkstate.h).
 */

#include "linden_common.h"

#include "llrenderheaders.h"

#if defined(LL_VULKAN)

#include "llvkstate.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llvkasync.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>
#include <cstring>
#include "llvkplatform.h"

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
    return mDepthMask == o.mDepthMask && mRaster == o.mRaster && mDepth == o.mDepth && mBlend == o.mBlend && mViewportSet == o.mViewportSet && mDepthReadOnly == o.mDepthReadOnly && mScissorOn == o.mScissorOn
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
    b.mDepthReadOnly = false;
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

#define CTX_CALL() if (!ownThread(__func__)) return; ++C().mCalls; if (LLRenderState::sWhy && !LLRenderState::sInGuard && !tlsCtx) LLRenderState::noteDirect(__func__)
static bool scissor_trace_now();

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

void setPolygonOffset(F32 factor, F32 units)
{
    CTX_CALL();
    C().mBlock.mRaster.mBiasSlope = factor;
    C().mBlock.mRaster.mBiasConstant = units;
}

void setLineWidth(F32 width)
{
    CTX_CALL();
    C().mBlock.mRaster.mLineWidth = width;
}

void setStencilMask(U32 mask)
{
    CTX_CALL();
    C().mBlock.mDepth.mWriteMask = mask;
}

// F3.2: typed overloads (llrenderstate.h). The enums are in Vulkan's order, so the conversion is a table of one line each
namespace
{
VkCompareOp toVk(LLRenderState::CompareOp op)
{
    using LLRenderState::CompareOp;
    switch (op)
    {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

LLRenderState::CompareOp fromVk(VkCompareOp op)
{
    using LLRenderState::CompareOp;
    switch (op)
    {
    case VK_COMPARE_OP_NEVER: return CompareOp::Never;
    case VK_COMPARE_OP_LESS: return CompareOp::Less;
    case VK_COMPARE_OP_EQUAL: return CompareOp::Equal;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return CompareOp::LessEqual;
    case VK_COMPARE_OP_GREATER: return CompareOp::Greater;
    case VK_COMPARE_OP_NOT_EQUAL: return CompareOp::NotEqual;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return CompareOp::GreaterEqual;
    default: return CompareOp::Always;
    }
}

VkStencilOp toVk(LLRenderState::StencilOp op)
{
    using LLRenderState::StencilOp;
    switch (op)
    {
    case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
    case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::IncrClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::DecrClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
    case StencilOp::IncrWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::DecrWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor toVk(LLRenderState::BlendFactor f)
{
    using LLRenderState::BlendFactor;
    switch (f)
    {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
}

void setBlendEnable(bool on)
{
    CTX_CALL();
    C().mBlock.mBlend.mEnable = on;
}

void setDepthTest(bool on)
{
    CTX_CALL();
    C().mBlock.mDepth.mDepthTest = on;
    recomputeDepthWrite();
}

void setCullEnable(bool on)
{
    CTX_CALL();
    C().mCullOn = on;
    recomputeCull();
}

void setScissorTest(bool on)
{
    CTX_CALL();
    C().mBlock.mScissorOn = on;
    if (scissor_trace_now())
    {
        LL_INFOS("ScissorTrace") << "frame " << LLFrameTimer::getFrameCount() << " scissor test " << (on ? "on" : "off") << (tlsCtx ? " [sub-stream]" : " [main]") << LL_ENDL;
    }
}

void setStencilTest(bool on)
{
    CTX_CALL();
    C().mBlock.mDepth.mStencilTest = on;
}

void setPolygonOffsetFill(bool on)
{
    CTX_CALL();
    C().mBlock.mRaster.mDepthBias = on;
}

void setDepthReadOnly(bool on)
{
    CTX_CALL();
    C().mBlock.mDepthReadOnly = on;
}

void setDepthClamp(bool on)
{
    CTX_CALL();
    C().mBlock.mRaster.mDepthClamp = on;
}

void setBlendFunc(LLRenderState::BlendFactor src_rgb, LLRenderState::BlendFactor dst_rgb, LLRenderState::BlendFactor src_a, LLRenderState::BlendFactor dst_a)
{
    CTX_CALL();
    BlendState& bs = C().mBlock.mBlend;
    bs.mSrcColor = toVk(src_rgb);
    bs.mDstColor = toVk(dst_rgb);
    bs.mSrcAlpha = toVk(src_a);
    bs.mDstAlpha = toVk(dst_a);
}

namespace
{
}

void setCullFace(LLRenderState::CullFace face)
{
    using LLRenderState::CullFace;
    if (face == CullFace::None)
    {
        return;
    }
    CTX_CALL();
    C().mCullFace = face == CullFace::Front ? VK_CULL_MODE_FRONT_BIT : (face == CullFace::FrontAndBack ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
    recomputeCull();
}

void setPolygonMode(LLRenderState::PolygonMode mode)
{
    using LLRenderState::PolygonMode;
    CTX_CALL();
    C().mBlock.mRaster.mPolygonMode = mode == PolygonMode::Line ? VK_POLYGON_MODE_LINE : (mode == PolygonMode::Point ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
}

void setDepthFunc(LLRenderState::CompareOp op)
{
    CTX_CALL();
    C().mBlock.mDepth.mDepthOp = toVk(op);
}

void setStencilFunc(LLRenderState::CompareOp op, S32 ref, U32 mask)
{
    CTX_CALL();
    DepthStencilState& ds = C().mBlock.mDepth;
    ds.mStencilOp = toVk(op);
    ds.mReference = (U32)ref;
    ds.mCompareMask = mask;
}

void setStencilOp(LLRenderState::StencilOp sfail, LLRenderState::StencilOp dpfail, LLRenderState::StencilOp dppass)
{
    CTX_CALL();
    DepthStencilState& ds = C().mBlock.mDepth;
    ds.mFail = toVk(sfail);
    ds.mDepthFail = toVk(dpfail);
    ds.mPass = toVk(dppass);
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
    ADOPT(mBlock.mDepthMask) ADOPT(mBlock.mViewportSet) ADOPT(mBlock.mScissorOn) ADOPT(mBlock.mDepthReadOnly)
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
    if (a.mDepthReadOnly != b.mDepthReadOnly) note("depthReadOnly");
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
    if (LLRenderState::sWhy && !tlsCtx)
    {
        LLRenderState::whyEmit(c.mTakenValid ? describeDifference(c.mTaken, c.mBlock) : std::string("first"));
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
    ds.mDeclaredRO = b.mDepthReadOnly && has_depth;
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
    return std::string(buf) + LLRenderState::guardReport();
}

} // namespace LLVkState

// ---- F3.2: the scoped guards of llrenderstate.h ----
namespace LLRenderState
{

bool sGuardCheck = []() { const char* e = getenv("LL_VK_STATE_GUARD_CHECK"); return e && e[0] == '1'; }();

namespace
{
using LLVkState::C;

const char* kindName(Kind k)
{
    static const char* const names[(int)Kind::Count] = { "blend", "scissor test", "stencil test", "polygon offset", "depth clamp", "cull (bit 0 on, bits 1-2 face)", "depth (bit 0 test, bit 1 write, bits 2-4 func)", "depth test" };
    return names[(int)k];
}

// a warning per guard site (file + line of the constructor), the first 200 sites; the total is in the stats line
struct Site { const char* mFile; int mLine; U32 mCount; };
std::vector<Site> gSites;
U64 gWarnings = 0;

void warn(const char* what, Kind k, U32 expected, U32 found, U32 saved, const char* file, int line)
{
    ++gWarnings;
    for (Site& s : gSites)
    {
        if (s.mLine == line && s.mFile == file)
        {
            ++s.mCount;
            return;
        }
    }
    if (gSites.size() < 200)
    {
        gSites.push_back({ file, line, 1 });
        LL_WARNS("StateGuard") << what << ": " << kindName(k) << " expected 0x" << std::hex << expected << " found 0x" << found << " (saved 0x" << saved << std::dec
            << ") guard made at " << (file ? file : "?") << ":" << line << LL_ENDL;
    }
}
}

std::string guardReport()
{
    if (!sGuardCheck)
    {
        return std::string();
    }
    char buf[96];
    snprintf(buf, sizeof(buf), ", guard check: %llu warnings at %u sites", gWarnings, (U32)gSites.size());
    return buf;
}

bool sWhy = []() { const char* e = getenv("LL_VK_STATE_WHY"); return e && e[0] == '1'; }();
bool sInGuard = false;

namespace
{
struct Pending { const char* mFile; int mLine; int mKind; };
std::vector<Pending> gPending;
std::map<std::string, U64> gByField;
std::map<std::string, U64> gByWriter;
U64 gEmitted = 0;
U32 gLastDump = 0;

const char* fieldOfKind(int kind)
{
    static const char* const names[(int)Kind::Count] = { "blendEnable", "scissorTest", "stencil", "polygonOffset", "depthClamp", "cull", "depth", "depth" };
    return kind >= 0 && kind < (int)Kind::Count ? names[kind] : nullptr;
}

void dumpTop(const char* what, std::map<std::string, U64>& m)
{
    std::vector<std::pair<U64, std::string>> v;
    for (auto& e : m)
    {
        v.push_back({ e.second, e.first });
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string line;
    for (size_t i = 0; i < v.size() && i < 15; ++i)
    {
        line += "\n   " + std::to_string(v[i].first) + "  " + v[i].second;
    }
    LL_INFOS("StateWhy") << what << line << LL_ENDL;
    m.clear();
}
}

void noteWriter(const char* file, int line, int kind)
{
    if (!sWhy || LLVkState::tlsCtx)
    {
        return;
    }
    for (const Pending& p : gPending)
    {
        if (p.mLine == line && p.mFile == file && p.mKind == kind)
        {
            return;
        }
    }
    if (gPending.size() < 64)
    {
        gPending.push_back({ file, line, kind });
    }
}

void noteDirect(const char* what)
{
    noteWriter(what, 0, -1);
}

// a CMD_STATE record is about to be written because the block differs from the last one taken: credit the fields that differ and the writers (since the last record) whose field is among them
void whyEmit(const std::string& diff)
{
    ++gEmitted;
    size_t pos = 0;
    while (pos < diff.size())
    {
        const size_t sp = diff.find(' ', pos);
        const std::string f = diff.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
        if (!f.empty())
        {
            ++gByField[f];
        }
        if (sp == std::string::npos)
        {
            break;
        }
        pos = sp + 1;
    }
    for (const Pending& p : gPending)
    {
        const char* field = fieldOfKind(p.mKind);
        if (field && diff.find(field) == std::string::npos)
        {
            continue;                           // that write was undone before the draw
        }
        std::string key;
        if (p.mLine)
        {
            const char* base = strrchr(p.mFile, '\\');
            key = std::string(base ? base + 1 : p.mFile) + ":" + std::to_string(p.mLine) + (field ? std::string(" ") + field : std::string());
        }
        else
        {
            key = std::string("direct ") + p.mFile;
        }
        ++gByWriter[key];
    }
    gPending.clear();
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame - gLastDump >= 600)
    {
        if (gLastDump)
        {
            LL_INFOS("StateWhy") << "CMD_STATE records since the last report: " << gEmitted << " over " << (frame - gLastDump) << " frames" << LL_ENDL;
            dumpTop("changed fields (per record)", gByField);
            dumpTop("writers (guard file:line field, or direct setter)", gByWriter);
        }
        gByField.clear();
        gByWriter.clear();
        gEmitted = 0;
        gLastDump = frame;
    }
}


U32 read(Kind kind)
{
    const LLVkState::Block& b = C().mBlock;
    switch (kind)
    {
    case Kind::Blend: return b.mBlend.mEnable;
    case Kind::Scissor: return b.mScissorOn;
    case Kind::Stencil: return b.mDepth.mStencilTest;
    case Kind::PolygonOffset: return b.mRaster.mDepthBias;
    case Kind::DepthClamp: return b.mRaster.mDepthClamp;
    case Kind::Cull:
    {
        const VkCullModeFlags f = C().mCullFace;
        const CullFace face = f == VK_CULL_MODE_FRONT_BIT ? CullFace::Front : (f == VK_CULL_MODE_FRONT_AND_BACK ? CullFace::FrontAndBack : CullFace::Back);
        return packCull(C().mCullOn, face);
    }
    case Kind::Depth:
        return packDepth(b.mDepth.mDepthTest, b.mDepthMask, LLVkState::fromVk(b.mDepth.mDepthOp));
    case Kind::DepthTest: return b.mDepth.mDepthTest;
    case Kind::DepthReadOnly: return b.mDepthReadOnly;
    default: return 0;
    }
}

U32 exchange(Kind kind, U32 value, const char* file, int line)
{
    LLVkAsync::FrontLap front_lap(LLVkAsync::FP_GLSTATE);
    const U32 old = read(kind);
    if (kind == Kind::Cull && !(value & 6))
    {
        value |= old & 6;                       // face None: keep the face
    }
    if (old == value)
    {
        return old;
    }
    gRender.flush();                                // pending immediate-mode vertices belong to the old state
    sInGuard = true;
    switch (kind)
    {
    case Kind::Blend: LLVkState::setBlendEnable(value != 0); break;
    case Kind::Scissor: LLVkState::setScissorTest(value != 0); break;
    case Kind::Stencil: LLVkState::setStencilTest(value != 0); break;
    case Kind::PolygonOffset: LLVkState::setPolygonOffsetFill(value != 0); break;
    case Kind::DepthClamp: LLVkState::setDepthClamp(value != 0); break;
    case Kind::DepthTest: LLVkState::setDepthTest(value != 0); break;
    case Kind::DepthReadOnly: LLVkState::setDepthReadOnly(value != 0); break;
    case Kind::Cull:
        if ((old & 1) != (value & 1))
        {
            LLVkState::setCullEnable((value & 1) != 0);
        }
        if ((old & 6) != (value & 6))
        {
            LLVkState::setCullFace((CullFace)((value >> 1) & 3));
        }
        break;
    case Kind::Depth:
        if ((old & 1) != (value & 1))
        {
            LLVkState::setDepthTest((value & 1) != 0);
        }
        if ((old & 28) != (value & 28))
        {
            LLVkState::setDepthFunc((CompareOp)((value >> 2) & 7));
        }
        if ((old & 2) != (value & 2))
        {
            LLVkState::setDepthMask((value & 2) != 0);
        }
        break;
    default: break;
    }
    sInGuard = false;
    if (sWhy)
    {
        noteWriter(file ? file : "?", line, (int)kind);
    }
    return old;
}

void leave(Kind kind, U32 saved, U32 set, const char* file, int line)
{
    if (sGuardCheck)
    {
        const U32 now = read(kind);
        if (now != set)
        {
            warn("state changed while a guard was alive", kind, set, now, saved, file, line);
        }
    }
    if (set != saved)
    {
        exchange(kind, saved, file, line);
    }
}

}

#endif // LL_VULKAN
