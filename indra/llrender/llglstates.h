/**
 * @file llglstates.h
 * @brief LLGL states definitions
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

//THIS HEADER SHOULD ONLY BE INCLUDED FROM llgl.h
#ifndef LL_LLGLSTATES_H
#define LL_LLGLSTATES_H

#include "llimagegl.h"

//----------------------------------------------------------------------------

class LLGLSDefault
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
public:
    LLGLSDefault()
        :
        // Disable
        mBlend(false),
        mCullFace(false)
    { }
};

class LLGLSObjectSelect
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
public:
    LLGLSObjectSelect()
        : mBlend(false),
          mCullFace(true)
    { }
};

//----------------------------------------------------------------------------

class LLGLSUIDefault
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
    LLRenderState::DepthScope mDepthTest;
public:
    LLGLSUIDefault()
        : mBlend(true),
          mCullFace(false),
          mDepthTest(false, true, LLRenderState::CompareOp::LessEqual)
    {}
};

//----------------------------------------------------------------------------

class LLGLSPipeline
{
protected:
    LLRenderState::CullScope mCullFace;
    LLRenderState::DepthScope mDepthTest;
public:
    LLGLSPipeline()
        : mCullFace(true),
          mDepthTest(true, true, LLRenderState::CompareOp::LessEqual)
    { }
};

class LLGLSPipelineAlpha // : public LLGLSPipeline
{
protected:
    LLRenderState::BlendScope mBlend;
public:
    LLGLSPipelineAlpha()
        : mBlend(true)
    { }
};

class LLGLSPipelineSelection
{
protected:
    LLRenderState::CullScope mCullFace;
public:
    LLGLSPipelineSelection()
        : mCullFace(false)
    {}
};

class LLGLSPipelineSkyBox
{
protected:
    LLRenderState::CullScope mCullFace;
    LLGLSquashToFarClip mSquashClip;
public:
    LLGLSPipelineSkyBox();
   ~LLGLSPipelineSkyBox();
};

class LLGLSPipelineDepthTestSkyBox : public LLGLSPipelineSkyBox
{
public:
    LLGLSPipelineDepthTestSkyBox(bool depth_test, bool depth_write);

    LLRenderState::DepthScope mDepth;
};

class LLGLSPipelineBlendSkyBox : public LLGLSPipelineDepthTestSkyBox
{
public:
    LLGLSPipelineBlendSkyBox(bool depth_test, bool depth_write);
    LLRenderState::BlendScope mBlend;
};

class LLGLSTracker
{
protected:
    LLRenderState::CullScope mCullFace;
    LLRenderState::BlendScope mBlend;
public:
    LLGLSTracker() :
        mCullFace(true),
        mBlend(true)
    { }
};

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

#endif
