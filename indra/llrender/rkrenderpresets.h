/**
 * @file rkrenderpresets.h
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

//THIS HEADER SHOULD ONLY BE INCLUDED FROM rkdevicecaps.h
#ifndef LL_LLGLSTATES_H
#define LL_LLGLSTATES_H

#include "rkimagetex.h"

//----------------------------------------------------------------------------

class LLRenderPresetDefault
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
public:
    LLRenderPresetDefault()
        :
        // Disable
        mBlend(false),
        mCullFace(false)
    { }
};

class LLRenderPresetObjectSelect
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
public:
    LLRenderPresetObjectSelect()
        : mBlend(false),
          mCullFace(true)
    { }
};

//----------------------------------------------------------------------------

class LLRenderPresetUIDefault
{
protected:
    LLRenderState::BlendScope mBlend;
    LLRenderState::CullScope mCullFace;
    LLRenderState::DepthScope mDepthTest;
public:
    LLRenderPresetUIDefault()
        : mBlend(true),
          mCullFace(false),
          mDepthTest(false, true, LLRenderState::CompareOp::LessEqual)
    {}
};

//----------------------------------------------------------------------------

class LLRenderPresetPipeline
{
protected:
    LLRenderState::CullScope mCullFace;
    LLRenderState::DepthScope mDepthTest;
public:
    LLRenderPresetPipeline()
        : mCullFace(true),
          mDepthTest(true, true, LLRenderState::CompareOp::LessEqual)
    { }
};

class LLRenderPresetPipelineAlpha // : public LLRenderPresetPipeline
{
protected:
    LLRenderState::BlendScope mBlend;
public:
    LLRenderPresetPipelineAlpha()
        : mBlend(true)
    { }
};

class LLRenderPresetPipelineSelection
{
protected:
    LLRenderState::CullScope mCullFace;
public:
    LLRenderPresetPipelineSelection()
        : mCullFace(false)
    {}
};

class LLRenderPresetPipelineSkyBox
{
protected:
    LLRenderState::CullScope mCullFace;
    LLRenderSquashToFarClip mSquashClip;
public:
    LLRenderPresetPipelineSkyBox();
   ~LLRenderPresetPipelineSkyBox();
};

class LLRenderPresetPipelineDepthTestSkyBox : public LLRenderPresetPipelineSkyBox
{
public:
    LLRenderPresetPipelineDepthTestSkyBox(bool depth_test, bool depth_write);

    LLRenderState::DepthScope mDepth;
};

class LLRenderPresetPipelineBlendSkyBox : public LLRenderPresetPipelineDepthTestSkyBox
{
public:
    LLRenderPresetPipelineBlendSkyBox(bool depth_test, bool depth_write);
    LLRenderState::BlendScope mBlend;
};

class LLRenderPresetTracker
{
protected:
    LLRenderState::CullScope mCullFace;
    LLRenderState::BlendScope mBlend;
public:
    LLRenderPresetTracker() :
        mCullFace(true),
        mBlend(true)
    { }
};

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

#endif
