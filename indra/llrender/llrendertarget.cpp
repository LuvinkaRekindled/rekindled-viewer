/**
 * @file llrendertarget.cpp
 * @brief LLRenderTarget implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llrendertarget.h"
#include "llrenderpass.h"
#include "llrender.h"
#include "llshaderprogram.h"
#include "lldevicecaps.h"
#include <atomic>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include "llvkplatform.h"
#if defined(LL_VULKAN)
#include "llvkasync.h"
#include "llvkstate.h"
#include "llvktex.h"
#include "llvktarget.h"
#endif

// attaching a texture to a target: a slot (colour attachment index, or the depth slot) of the target names the texture
static void ll_fboAttach(U32 fbo, U32 slot, U32 texture)
{
    LLVkTarget::attach(fbo, slot, texture);
}

LLRenderTarget* LLRenderTarget::sBoundTarget = NULL;
// F3.5d: the open passes, outermost first (was a linked list through the targets: mPreviousRT); sBoundTarget is its last element
static std::vector<LLRenderTarget*> sTargetStack;

void LLRenderTarget::setCurrentDrawBufferCount(S32 count)
{
    LLVkAsync::targetDrawBuffers(sCurFBO, count);
}

U32 LLRenderTarget::sBytesAllocated = 0;

bool LLRenderTarget::sUseFBO = false;
U32 LLRenderTarget::sCurFBO = 0;


extern S32 gRenderViewport[4];

U32 LLRenderTarget::sCurResX = 0;
U32 LLRenderTarget::sCurResY = 0;

LLRenderTarget::LLRenderTarget() :
    mResX(0),
    mResY(0),
    mFBO(0),
    mDepth(0),
    mUseDepth(false),
    mUsage(LLTexUnit::TT_TEXTURE)
{
}

LLRenderTarget::~LLRenderTarget()
{
    release();
}

void LLRenderTarget::resize(U32 resx, U32 resy)
{
    //for accounting, get the number of pixels added/subtracted
    S32 pix_diff = (resx*resy)-(mResX*mResY);

    mResX = resx;
    mResY = resy;

    llassert(mInternalFormat.size() == mTex.size());

    for (U32 i = 0; i < mTex.size(); ++i)
    { //resize color attachments
        gRender.getTexUnit(0)->bindManual(mUsage, mTex[i]);
        LLImageTex::setManualImage(mTex[i], LLTexUnit::getInternalType(mUsage), 0, mInternalFormat[i], mResX, mResY, PixelFormat::RGBA, ComponentType::U8, NULL, false);
        sBytesAllocated += pix_diff*4;
    }

    if (mDepth)
    {
        gRender.getTexUnit(0)->bindManual(mUsage, mDepth);
        TextureTarget internal_type = LLTexUnit::getInternalType(mUsage);
        LLImageTex::setManualImage(mDepth, internal_type, 0, TexelFormat::Depth, mResX, mResY, PixelFormat::Depth, ComponentType::U32, NULL, false);

        sBytesAllocated += pix_diff*4;
    }
}


bool LLRenderTarget::allocate(U32 resx, U32 resy, TexelFormat color_fmt, bool depth, LLTexUnit::eTextureType usage, LLTexUnit::eTextureMipGeneration generateMipMaps, const char* file, int line)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(usage == LLTexUnit::TT_TEXTURE);
    llassert(!isBoundInStack());

    if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps)
    {
        return true;
    }
    resx = llmin(resx, (U32) gDeviceCaps.mTextureSizeLimit);
    resy = llmin(resy, (U32) gDeviceCaps.mTextureSizeLimit);

    release();

    mResX = resx;
    mResY = resy;

    mUsage = usage;
    mUseDepth = depth;

    mGenerateMipMaps = generateMipMaps;

    if (mGenerateMipMaps != LLTexUnit::TMG_NONE) {
        // Calculate the number of mip levels based upon resolution that we should have.
        mMipLevels = 1 + (U32)floor(log10((float)llmax(mResX, mResY)) / log10(2.0));
    }

    if (depth)
    {
        if (!allocateDepth())
        {
            LL_WARNS() << "Failed to allocate depth buffer for render target." << LL_ENDL;
            return false;
        }
    }

    LLVkTarget::create((U32*)&mFBO);

    if (mDepth)
    {
        ll_fboAttach(mFBO, LLVkTarget::DEPTH_SLOT, mDepth);
    }

    return addColorAttachment(color_fmt, file, line);
}

void LLRenderTarget::setColorAttachment(LLImageTex* img, U32 use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(img != nullptr); // img must not be null
    llassert(sUseFBO); // FBO support must be enabled
    llassert(mDepth == 0); // depth buffers not supported with this mode
    llassert(mTex.empty()); // mTex must be empty with this mode (binding target should be done via LLImageTex)
    llassert(!isBoundInStack());

    if (mFBO == 0)
    {
        LLVkTarget::create((U32*)&mFBO);
    }

    mResX = img->getWidth();
    mResY = img->getHeight();
    mUsage = img->getTarget();

    if (use_name == 0)
    {
        use_name = img->getTexName();
    }

    mTex.push_back(use_name);

    ll_fboAttach(mFBO, 0, use_name);

}

void LLRenderTarget::releaseColorAttachment()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(!isBoundInStack());
    llassert(mTex.size() == 1); //cannot use releaseColorAttachment with LLRenderTarget managed color targets
    llassert(mFBO != 0);  // mFBO must be valid

    ll_fboAttach(mFBO, 0, 0);

    mTex.clear();
}

bool LLRenderTarget::addColorAttachment(TexelFormat color_fmt, const char* file, int line)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());
    LLVkFormat::noteFormat("RT.color", file, line, (U8)color_fmt, 0, 0);

    if (color_fmt == TexelFormat::None)
    {
        return true;
    }

    U32 offset = static_cast<U32>(mTex.size());

    if( offset >= 4 )
    {
        LL_WARNS() << "Too many color attachments" << LL_ENDL;
        llassert( offset < 4 );
        return false;
    }
    if( offset > 0 && (mFBO == 0) )
    {
        llassert(  mFBO != 0 );
        return false;
    }

    U32 tex;
    LLImageTex::generateTextures(1, &tex);
    gRender.getTexUnit(0)->bindManual(mUsage, tex);



    {
        LLImageTex::setManualImage(tex, LLTexUnit::getInternalType(mUsage), 0, color_fmt, mResX, mResY, PixelFormat::RGBA, ComponentType::U8, NULL, false);
    }

    sBytesAllocated += mResX*mResY*4;



    if (offset == 0)
    { //use bilinear filtering on single texture render targets that aren't multisampled
        gRender.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
    }
    else
    { //don't filter data attachments
        gRender.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    }

    if (mUsage != LLTexUnit::TT_RECT_TEXTURE)
    {
        gRender.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_MIRROR);
    }
    else
    {
        // ATI doesn't support mirrored repeat for rectangular textures.
        gRender.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    if (mFBO)
    {
        ll_fboAttach(mFBO, offset, tex);

    }

    mTex.push_back(tex);
    mInternalFormat.push_back(color_fmt);

    if (gDebugRender)
    { //bind and unbind to validate target
        bindTarget();
        flush();
    }


    return true;
}

bool LLRenderTarget::allocateDepth()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLImageTex::generateTextures(1, &mDepth);
    gRender.getTexUnit(0)->bindManual(mUsage, mDepth);

    TextureTarget internal_type = LLTexUnit::getInternalType(mUsage);
    LLImageTex::setManualImage(mDepth, internal_type, 0, TexelFormat::Depth, mResX, mResY, PixelFormat::Depth, ComponentType::U32, NULL, false);
    gRender.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

    sBytesAllocated += mResX*mResY*4;


    return true;
}

void LLRenderTarget::shareDepthBuffer(LLRenderTarget& target)
{
    llassert(!isBoundInStack());

    if (!mFBO || !target.mFBO)
    {
        LL_ERRS() << "Cannot share depth buffer between non FBO render targets." << LL_ENDL;
    }

    if (target.mDepth)
    {
        LL_ERRS() << "Attempting to override existing depth buffer.  Detach existing buffer first." << LL_ENDL;
    }

    if (target.mUseDepth)
    {
        LL_ERRS() << "Attempting to override existing shared depth buffer. Detach existing buffer first." << LL_ENDL;
    }

    if (mDepth)
    {
        ll_fboAttach(target.mFBO, LLVkTarget::DEPTH_SLOT, mDepth);


        target.mUseDepth = true;
    }
}

void LLRenderTarget::release()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    noteRelease();
    llassert(!isBoundInStack());

    if (mDepth)
    {
        LLImageTex::deleteTextures(1, &mDepth);

        mDepth = 0;

        sBytesAllocated -= mResX*mResY*4;
    }
    // else if (mFBO)
    if (mFBO)
    {
        if (mUseDepth)
        { //detach shared depth buffer
            ll_fboAttach(mFBO, LLVkTarget::DEPTH_SLOT, 0);
            mUseDepth = false;
        }
    }

    // Detach any extra color buffers (e.g. SRGB spec buffers)
    //
    if (mFBO && (mTex.size() > 1))
    {
        size_t z;
        for (z = mTex.size() - 1; z >= 1; z--)
        {
            sBytesAllocated -= mResX*mResY*4;
            ll_fboAttach(mFBO, static_cast<U32>(z), 0);
            LLImageTex::deleteTextures(1, &mTex[z]);
        }
    }

    if (mFBO)
    {
        if (mFBO == sCurFBO)
        {
            sCurFBO = 0;
            LLVkAsync::passBegin(0, -1, gRenderViewport[0], gRenderViewport[1], gRenderViewport[2], gRenderViewport[3], false, false, nullptr, LLVkAsync::PASS_SCREEN, __FILE__, __LINE__);
        }

        LLVkTarget::destroy(mFBO);
        mFBO = 0;
    }

    if (mTex.size() > 0)
    {
        sBytesAllocated -= mResX*mResY*4;
        LLImageTex::deleteTextures(1, &mTex[0]);
    }

    mTex.clear();
    mInternalFormat.clear();

    mResX = mResY = 0;
}

void LLRenderTarget::bindTarget(const char* file, int line)
{
    bindImpl(file, line, false);
}

// restore: the flush of a nested pass puts the pass it interrupted back (it keeps the bind site and the draw count of its own bindTarget)
void LLRenderTarget::bindImpl(const char* file, int line, bool restore, ClearBits clear_mask, const F32* clear_color)
{
    LL_PROFILE_GPU_ZONE("bindTarget");
    if (!restore)
    {
        noteBind(file, line);
        mBindFile = file;
        mBindLine = line;
        mBindDraw = LLVkAsync::drawSeq();
    }
    llassert(mFBO);
    llassert(!isBoundInStack());

    // the target, its colour attachment count and the viewport are one record
    sCurFBO = mFBO;
    // the load op: a clear that came with the bind is part of the same record (what bindTarget(); clear(); did as two)
    const ClearBits clears = !restore ? ((ClearBits::Color | (mUseDepth ? ClearBits::Depth : ClearBits::None)) & clear_mask) : ClearBits::None;
    if (hasBits(clears, ClearBits::All))
    {
        sBoundTarget = this;
        noteClear(file, line);
    }
    LLVkAsync::passBegin(mFBO, mTex.empty() ? 0 : (S32)mTex.size(), 0, 0, (S32)mResX, (S32)mResY, hasBits(clears, ClearBits::Color), hasBits(clears, ClearBits::Depth), clear_color,
                         restore ? LLVkAsync::PASS_RESUME : LLVkAsync::PASS_NEW, file, line);
    LLShaderProgram::announceBound();
    sCurResX = mResX;
    sCurResY = mResY;

    if (!restore)
    {
        sTargetStack.push_back(this);
    }
    sBoundTarget = this;
}

void LLRenderTarget::clear(ClearBits mask_in, const char* file, int line)
{
    LL_PROFILE_GPU_ZONE("clear");
    noteClear(file, line);
    llassert(mFBO);
    ClearBits mask = ClearBits::Color;
    if (mUseDepth)
    {
        mask = mask | ClearBits::Depth;
    }
    if (mFBO)
    {
        gRender.clearBuffers(mask & mask_in);
    }
    else
    {
        LLRenderState::ScissorScope scissor(true);
        LLVkState::setScissor(0, 0, mResX, mResY);
        gRender.clearBuffers(mask & mask_in);
    }
}

U32 LLRenderTarget::getTexture(U32 attachment) const
{
    if (attachment >= mTex.size())
    {
        // <FS> this was logged on every call (a million times in a few minutes with LL_VK_NOGL): the first few with the caller, then now and then
        static std::atomic<U32> sInvalidCount(0);
        const U32 n = sInvalidCount.fetch_add(1);
        if (n < 4)
        {
            void* frames[10] = {};
            const USHORT count = CaptureStackBackTrace(1, 10, frames, nullptr);
            std::string where;
            for (USHORT i = 0; i < count; ++i)
            {
                char buf[24];
                snprintf(buf, sizeof(buf), " %p", frames[i]);
                where += buf;
            }
            LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << " (target " << mResX << "x" << mResY << ", depth "
                << mDepth << "), called from:" << where << LL_ENDL;
        }
        else if ((n & 0xFFFFF) == 0)
        {
            LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << " (" << n << " times so far)" << LL_ENDL;
        }
        llassert(false);
        return 0;
    }
    return mTex[attachment];
}

U32 LLRenderTarget::getNumTextures() const
{
    return static_cast<U32>(mTex.size());
}

void LLRenderTarget::bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options)
{
    gRender.getTexUnit(channel)->bindManual(mUsage, getTexture(index), filter_options == LLTexUnit::TFO_TRILINEAR || filter_options == LLTexUnit::TFO_ANISOTROPIC);
    gRender.getTexUnit(channel)->setTextureFilteringOption(filter_options);
}

void LLRenderTarget::flush(const char* file, int line)
{
    LL_PROFILE_GPU_ZONE("rt flush");
    noteFlush(file, line);
    gRender.flush();
    llassert(mFBO);
    llassert(sCurFBO == mFBO);
    llassert(sBoundTarget == this);

    if (mGenerateMipMaps == LLTexUnit::TMG_AUTO)
    {
        LL_PROFILE_GPU_ZONE("rt generate mipmaps");
        bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
        LLVkTex::generateMipmap(mTex[0]);
    }

    LLVkAsync::passEnd();
    sTargetStack.erase(std::remove(sTargetStack.begin(), sTargetStack.end(), this), sTargetStack.end());
    if (!sTargetStack.empty())
    {
        // a bit hacky -- pop the RT stack back two frames and push
        // the previous frame back on to play nice with the GL state machine
        LLRenderTarget* previous = sTargetStack.back();
        sBoundTarget = previous;
        previous->bindImpl(previous->mBindFile, previous->mBindLine, true);
    }
    else
    {
        sBoundTarget = nullptr;
        LLVkAsync::passBegin(0, -1, gRenderViewport[0], gRenderViewport[1], gRenderViewport[2], gRenderViewport[3], false, false, nullptr, LLVkAsync::PASS_SCREEN, file, line);
        LLShaderProgram::announceBound();
        sCurFBO = 0;
        sCurResX = gRenderViewport[2];
        sCurResY = gRenderViewport[3];
    }
    mBindFile = nullptr;
}

bool LLRenderTarget::isComplete() const
{
    return !mTex.empty() || mDepth;
}

void LLRenderTarget::getViewport(S32* viewport)
{
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = mResX;
    viewport[3] = mResY;
}

bool LLRenderTarget::isBoundInStack() const
{
    return std::find(sTargetStack.begin(), sTargetStack.end(), this) != sTargetStack.end();
}

void LLRenderTarget::swapFBORefs(LLRenderTarget& other)
{
    // Must be initialized
    llassert(mFBO);
    llassert(other.mFBO);

    // Must be unbound
    llassert(sCurFBO != mFBO);
    llassert(sCurFBO != other.mFBO);
    llassert(!isBoundInStack());
    llassert(!other.isBoundInStack());

    // Must be same type
    llassert(sUseFBO == other.sUseFBO);
    llassert(mResX == other.mResX);
    llassert(mResY == other.mResY);
    llassert(mInternalFormat == other.mInternalFormat);
    llassert(mTex.size() == other.mTex.size());
    llassert(mDepth == other.mDepth);
    llassert(mUseDepth == other.mUseDepth);
    llassert(mGenerateMipMaps == other.mGenerateMipMaps);
    llassert(mMipLevels == other.mMipLevels);
    llassert(mUsage == other.mUsage);

    std::swap(mFBO, other.mFBO);
    std::swap(mTex, other.mTex);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// F3.5a: the counters of the render target entry points (LL_VK_OPSTATS) and the pass check (LL_VK_PASS_CHECK=1); see llrenderpass.h.
// Both only look at the main thread's producer (the targets of the frame); a target bound on another thread is not counted.
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace
{

struct PassSite { const char* mFile; int mLine; int mKind; unsigned long long mCount; unsigned long long mAfterBind; };      // mKind: 0 bindTarget, 1 clear
constexpr int PASS_SITES = 192;
PassSite gPassSites[PASS_SITES];
std::atomic<int> gPassSiteCount{ 0 };
unsigned long long gPassBinds = 0, gPassNested = 0, gPassRestores = 0, gPassFlushes = 0, gPassClears = 0, gPassClearsAfterBind = 0;
unsigned gPassMaxDepth = 0;

bool pass_check()
{
    static const bool on = []() {
        const char* e = getenv("LL_VK_PASS_CHECK");
        const bool v = e && e[0] && e[0] != '0';
        if (v)
        {
            LL_INFOS("PassCheck") << "LL_VK_PASS_CHECK is on: warnings name the call site of the first occurrence" << LL_ENDL;
        }
        return v;
    }();
    return on;
}

const char* pass_base(const char* file)
{
    if (!file)
    {
        return "?";
    }
    const char* base = file;
    for (const char* p = file; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }
    return base;
}

PassSite* pass_site(int kind, const char* file, int line)
{
    const int n = gPassSiteCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
    {
        PassSite& s = gPassSites[i];
        if (s.mKind == kind && s.mLine == line && (s.mFile == file || (file && s.mFile && strcmp(s.mFile, file) == 0)))
        {
            return &s;
        }
    }
    if (n >= PASS_SITES)
    {
        return nullptr;
    }
    gPassSites[n] = { file, line, kind, 0, 0 };
    gPassSiteCount.store(n + 1, std::memory_order_release);
    return &gPassSites[n];
}

// the first occurrence per (what, call site) is logged
void pass_warn(const char* what, const char* file, int line, const std::string& detail)
{
    static std::vector<std::string> seen;
    const std::string key = std::string(what) + "|" + pass_base(file) + ":" + std::to_string(line);
    if (seen.size() >= 200 || std::find(seen.begin(), seen.end(), key) != seen.end())
    {
        return;
    }
    seen.push_back(key);
    LL_WARNS("PassCheck") << what << " (call at " << pass_base(file) << ":" << line << ")" << detail << LL_ENDL;
}

std::string pass_site_text(const char* file, int line)
{
    return std::string(pass_base(file)) + ":" + std::to_string(line);
}

}

unsigned LLRenderTarget::stackDepth()
{
    unsigned d = 0;
    d = (unsigned)sTargetStack.size();
    return d;
}

// isBoundInStack() walks the chain without a limit: a target bound twice makes the chain a cycle, which the check has to survive
bool LLRenderTarget::boundInStackBounded() const
{
    unsigned d = 0;
    (void)d;
    return std::find(sTargetStack.begin(), sTargetStack.end(), this) != sTargetStack.end();
}

void LLRenderTarget::noteBind(const char* file, int line)
{
    const bool stats = LLVkAsync::gFrontStats;
    const bool check = pass_check();
    if ((!stats && !check) || !LLVkAsync::mainOnlyProducer())
    {
        return;
    }
    if (stats)
    {
        ++gPassBinds;
        if (PassSite* s = pass_site(0, file, line))
        {
            ++s->mCount;
        }
        if (sBoundTarget)
        {
            ++gPassNested;
        }
        const unsigned d = stackDepth() + 1;
        if (d > gPassMaxDepth)
        {
            gPassMaxDepth = d;
        }
    }
    if (check)
    {
        if (!mFBO)
        {
            pass_warn("bindTarget of a target without a frame buffer", file, line, "");
        }
        if (boundInStackBounded())
        {
            pass_warn("bindTarget of a target that is already bound (it is in the stack of open passes twice)", file, line,
                      std::string("; it was bound at ") + pass_site_text(mBindFile, mBindLine));
        }
        else if (stackDepth() >= 8)
        {
            pass_warn("bindTarget with 8 passes already open (nesting this deep is not meant)", file, line,
                      std::string("; innermost bound at ") + (sBoundTarget ? pass_site_text(sBoundTarget->mBindFile, sBoundTarget->mBindLine) : std::string("?")));
        }
    }
}

void LLRenderTarget::noteFlush(const char* file, int line)
{
    const bool stats = LLVkAsync::gFrontStats;
    const bool check = pass_check();
    if ((!stats && !check) || !LLVkAsync::mainOnlyProducer())
    {
        return;
    }
    if (stats)
    {
        ++gPassFlushes;
        if (sTargetStack.size() > 1)
        {
            ++gPassRestores;
        }
    }
    if (check)
    {
        if (!sBoundTarget)
        {
            pass_warn("flush with no target bound (flushed twice, or never bound)", file, line, "");
        }
        else if (sBoundTarget != this)
        {
            if (!mBindFile)
            {
                pass_warn("flush of a target that is not bound (flushed twice, or never bound)", file, line,
                          std::string("; the innermost bound target was bound at ") + pass_site_text(sBoundTarget->mBindFile, sBoundTarget->mBindLine));
            }
            else
            {
                pass_warn("flush of a target that is not the innermost bound one (bad nesting: the chain is not a stack)", file, line,
                          std::string("; this target was bound at ") + pass_site_text(mBindFile, mBindLine) + ", the innermost at " + pass_site_text(sBoundTarget->mBindFile, sBoundTarget->mBindLine));
            }
        }
    }
}

void LLRenderTarget::noteClear(const char* file, int line)
{
    const bool stats = LLVkAsync::gFrontStats;
    const bool check = pass_check();
    if ((!stats && !check) || !LLVkAsync::mainOnlyProducer())
    {
        return;
    }
    if (stats)
    {
        ++gPassClears;
        // directly after its bind: no draw was made between (the shape that becomes the load op of the pass)
        const bool after_bind = sBoundTarget == this && mBindFile && LLVkAsync::drawSeq() == mBindDraw;
        if (after_bind)
        {
            ++gPassClearsAfterBind;
        }
        if (PassSite* s = pass_site(1, file, line))
        {
            ++s->mCount;
            if (after_bind)
            {
                ++s->mAfterBind;
            }
        }
    }
    if (check)
    {
        if (sBoundTarget != this)
        {
            pass_warn("clear of a target that is not the innermost bound one", file, line,
                      std::string("; the innermost bound target was bound at ") + (sBoundTarget ? pass_site_text(sBoundTarget->mBindFile, sBoundTarget->mBindLine) : std::string("(none)")));
        }
        else if (sCurFBO != mFBO)
        {
            pass_warn("clear of a target whose frame buffer is not the current one", file, line, "");
        }
    }
}

void LLRenderTarget::noteRelease()
{
    if (pass_check() && mFBO && LLVkAsync::mainOnlyProducer() && boundInStackBounded())
    {
        pass_warn("a target is released while it is bound", mBindFile, mBindLine, " (the call site is where it was bound)");
    }
}

void LLRenderTarget::checkFrameEnd()
{
    if (!pass_check() || !LLVkAsync::mainOnlyProducer())
    {
        return;
    }
    if (sBoundTarget)
    {
        pass_warn("a target is still bound at the end of the frame (a flush is missing)", sBoundTarget->mBindFile, sBoundTarget->mBindLine,
                  std::string("; ") + std::to_string(stackDepth()) + " pass(es) open (the call site is where the innermost was bound)");
    }
    else if (sCurFBO != 0)
    {
        pass_warn("sCurFBO is not 0 at the end of the frame although no target is bound", nullptr, 0, "");
    }
}

namespace RenderPass
{

void begin(LLRenderTarget& target, LoadOp load, ClearBits clear_mask, const F32* clear_color, const char* file, int line)
{
    if (load == LoadOp::Clear)
    {
        target.bindImpl(file, line, false, clear_mask, clear_color ? clear_color : gRender.getClearColor());
    }
    else
    {
        target.bindImpl(file, line, false);
    }
}

void end(LLRenderTarget& target, const char* file, int line)
{
    target.flush(file, line);
}

void suspend(LLRenderTarget& target, const char* file, int line)
{
    target.flush(file, line);
}

void resume(LLRenderTarget& target, const char* file, int line)
{
    target.bindTarget(file, line);
}

void frameEnd()
{
    LLRenderTarget::checkFrameEnd();
}

std::string report(size_t frames)
{
    if (!LLVkAsync::gFrontStats)
    {
        return std::string();
    }
    const double f = (double)std::max<size_t>(1, frames);
    char buf[320];
    snprintf(buf, sizeof(buf), "render targets (producer) per frame: bindTarget %.1f (inside another pass %.1f, deepest stack %u), flush %.1f (restoring the pass before %.1f), clear %.1f (directly after its bind, no draw between: %.1f; later: %.1f) | ",
             (double)gPassBinds / f, (double)gPassNested / f, gPassMaxDepth, (double)gPassFlushes / f, (double)gPassRestores / f, (double)gPassClears / f,
             (double)gPassClearsAfterBind / f, (double)(gPassClears - std::min(gPassClears, gPassClearsAfterBind)) / f);
    std::string out = buf;
    const int n = gPassSiteCount.load(std::memory_order_acquire);
    std::vector<PassSite> sites(gPassSites, gPassSites + n);
    std::sort(sites.begin(), sites.end(), [](const PassSite& a, const PassSite& b) { return a.mCount > b.mCount; });
    for (int kind = 0; kind < 2; ++kind)
    {
        out += kind == 0 ? "bindTarget sites per frame:" : "clear sites per frame (n, of them directly after the bind):";
        int shown = 0;
        for (const PassSite& s : sites)
        {
            if (s.mKind != kind || !s.mCount)
            {
                continue;
            }
            if (++shown > (kind == 0 ? 60 : 40))
            {
                break;
            }
            if (kind == 0)
            {
                snprintf(buf, sizeof(buf), " [%s:%d %.2f]", pass_base(s.mFile), s.mLine, (double)s.mCount / f);
            }
            else
            {
                snprintf(buf, sizeof(buf), " [%s:%d %.2f, %.2f]", pass_base(s.mFile), s.mLine, (double)s.mCount / f, (double)s.mAfterBind / f);
            }
            out += buf;
        }
        out += " | ";
    }
    for (int i = 0; i < n; ++i)
    {
        gPassSites[i].mCount = 0;
        gPassSites[i].mAfterBind = 0;
    }
    gPassBinds = gPassNested = gPassRestores = gPassFlushes = gPassClears = gPassClearsAfterBind = 0;
    gPassMaxDepth = 0;
    return out;
}

}

U32 LLRenderTarget::currentId(const char* file, int line)
{
    const U32 id = idOf(sBoundTarget);
    if (id != sCurFBO)
    {
        pass_warn("the target of the innermost open pass is not the current frame buffer (a copy or readback source)", file, line, "");
    }
    return sCurFBO;
}

void LLRenderTarget::checkSource(const LLRenderTarget* named, const char* file, int line)
{
    if (idOf(named) != sCurFBO)
    {
        pass_warn("a copy names a source that is not the current frame buffer", file, line, "");
    }
}
