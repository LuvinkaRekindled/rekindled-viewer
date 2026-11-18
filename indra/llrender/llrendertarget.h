/**
 * @file llrendertarget.h
 * @brief Off screen render target abstraction.  Loose wrapper for GL_EXT_framebuffer_objects.
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

#ifndef LL_LLRENDERTARGET_H
#define LL_LLRENDERTARGET_H

// LLRenderTarget is unavailible on the mapserver since it uses FBOs.

#include "rkdevicecaps.h"
#include "llrender.h"
#include "rkrenderpass.h"        // F3.5a: RenderPassScope

/*
 Wrapper around OpenGL framebuffer objects for use in render-to-texture

 SAMPLE USAGE:

    LLRenderTarget target;

    ...

    //allocate a 256x256 RGBA render target with depth buffer
    target.allocate(256,256,TexelFormat::RGBA8,TRUE);

    //render to contents of offscreen buffer
    target.bindTarget();
    target.clear();
    ... <issue drawing commands> ...
    target.flush();

    ...

    //use target as a texture
    gRender.getTexUnit(INDEX)->bind(&target);
    ... <issue drawing commands> ...

*/

class LLRenderTarget
{
public:
    // Whether or not to use FBO implementation
    static bool sUseFBO;
    static U32 sBytesAllocated;
    static U32 sCurFBO;
    static U32 sCurResX;
    static U32 sCurResY;


    LLRenderTarget();
    ~LLRenderTarget();

    //allocate resources for rendering
    //must be called before use
    //multiple calls will release previously allocated resources
    // resX - width
    // resY - height
    // color_fmt - how the colour attachment stores texels (e.g. TexelFormat::RGB8; None: no colour attachment)
    // depth - if true, allocate a depth buffer
    // usage - deprecated, should always be TT_TEXTURE
    bool allocate(U32 resx, U32 resy, TexelFormat color_fmt, bool depth = false, LLTexUnit::eTextureType usage = LLTexUnit::TT_TEXTURE, LLTexUnit::eTextureMipGeneration generateMipMaps = LLTexUnit::TMG_NONE, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    //resize existing attachments to use new resolution and color format
    // CAUTION: if the GL runs out of memory attempting to resize, this render target will be undefined
    // DO NOT use for screen space buffers or for scratch space for an image that might be uploaded
    // DO use for render targets that resize often and aren't likely to ruin someone's day if they break
    void resize(U32 resx, U32 resy);

    //point this render target at a particular RKImageTex
    //   Intended usage:
    //      LLRenderTarget target;
    //      target.addColorAttachment(image);
    //      target.bindTarget();
    //      < issue GL calls>
    //      target.flush();
    //      target.releaseColorAttachment();
    //
    // attachment -- RKImageTex to render into
    // use_name -- optional texture name to target instead of attachment->getTexName()
    // NOTE: setColorAttachment and releaseColorAttachment cannot be used in conjuction with
    // addColorAttachment, allocateDepth, resize, etc.
    void setColorAttachment(RKImageTex* attachment, U32 use_name = 0);

    // detach from current color attachment
    void releaseColorAttachment();

    //add color buffer attachment
    //limit of 4 color attachments per render target
    bool addColorAttachment(TexelFormat color_fmt, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    //allocate a depth texture
    bool allocateDepth();

    //share depth buffer with provided render target
    void shareDepthBuffer(LLRenderTarget& target);

    //free any allocated resources
    //safe to call redundantly
    // asserts that this target is not currently bound or present in the RT stack
    void release();

    //bind target for rendering
    //applies appropriate viewport
    //  If an LLRenderTarget is currently bound, stores a reference to that LLRenderTarget
    //  and restores previous binding on flush() (maintains a stack of Render Targets)
    //  Asserts that this target is not currently bound in the stack
    void bindTarget(const char* file = __builtin_FILE(), int line = __builtin_LINE());

    //clear render targer, clears depth buffer if present,
    //uses scissor rect if in copy-to-texture mode
    // asserts that this target is currently bound
    void clear(ClearBits mask = ClearBits::All, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    //get applied viewport
    void getViewport(S32* viewport);

    //get X resolution
    U32 getWidth() const { return mResX; }

    //get Y resolution
    U32 getHeight() const { return mResY; }

    LLTexUnit::eTextureType getUsage(void) const { return mUsage; }

    U32 getTexture(U32 attachment = 0) const;
    U32 getNumTextures() const;

    U32 getDepth(void) const { return mDepth; }

    void bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options = LLTexUnit::TFO_BILINEAR);

    //flush rendering operations
    //must be called when rendering is complete
    //should be used 1:1 with bindTarget
    // call bindTarget once, do all your rendering, call flush once
    // If an LLRenderTarget was bound when bindTarget was called, binds that RenderTarget for rendering (maintains RT stack)
    // asserts  that this target is currently bound
    void flush(const char* file = __builtin_FILE(), int line = __builtin_LINE());

    //Returns TRUE if target is ready to be rendered into.
    //That is, if the target has been allocated with at least
    //one renderable attachment (i.e. color buffer, depth buffer).
    bool isComplete() const;

    // Returns true if this RenderTarget is bound somewhere in the stack
    bool isBoundInStack() const;

    static LLRenderTarget* getCurrentBoundTarget() { return sBoundTarget; }
    // F4.2: where a copy or a readback takes its pixels from is a value: the target of the innermost open pass (null: the back buffer). What the producer records is its id.
    static U32 idOf(const LLRenderTarget* target) { return target ? target->mFBO : 0; }
    static U32 currentId(const char* file = __builtin_FILE(), int line = __builtin_LINE());
    static void checkSource(const LLRenderTarget* named, const char* file, int line);        // RK_VK_PASS_CHECK: a copy that names its source (it must be the frame buffer that is current)      // RK_VK_PASS_CHECK: warns when the stack of open passes and sCurFBO disagree

    // F3.5a (rkrenderpass.h): RK_VK_PASS_CHECK=1 reports a target that is still bound at the end of the frame
    static void checkFrameEnd();

    // *HACK
    void swapFBORefs(LLRenderTarget& other);

    static LLRenderTarget* sBoundTarget;        // the innermost open pass (the top of the stack of open passes, which lives in llrendertarget.cpp: F3.5d)
    friend void RenderPass::begin(LLRenderTarget&, RenderPass::LoadOp, ClearBits, const F32*, const char*, int);
    // the number of colour attachments draws write to on the current target (the rest are not written until the target is bound again)
    static void setCurrentDrawBufferCount(S32 count);

protected:
    U32 mResX;
    U32 mResY;
    std::vector<U32> mTex;
    std::vector<TexelFormat> mInternalFormat;
    U32 mFBO;

    // F3.5a: where the pass was begun (bindTarget's call site), the draw count at that moment, the counters of the entry points and the pass check
    const char* mBindFile = nullptr;
    int mBindLine = 0;
    unsigned long long mBindDraw = 0;
    void bindImpl(const char* file, int line, bool restore, ClearBits clear_mask = ClearBits::None, const F32* clear_color = nullptr);
    void noteBind(const char* file, int line);
    void noteFlush(const char* file, int line);
    void noteClear(const char* file, int line);
    void noteRelease();
    static unsigned stackDepth();
    bool boundInStackBounded() const;

    U32 mDepth;
    bool mUseDepth;
    LLTexUnit::eTextureMipGeneration mGenerateMipMaps;
    U32 mMipLevels;

    LLTexUnit::eTextureType mUsage;
};

#endif

