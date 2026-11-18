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

class RKRenderPresetDefault
{
protected:
    RKRenderState::BlendScope mBlend;
    RKRenderState::CullScope mCullFace;
public:
    RKRenderPresetDefault()
        :
        // Disable
        mBlend(false),
        mCullFace(false)
    { }
};

class RKRenderPresetObjectSelect
{
protected:
    RKRenderState::BlendScope mBlend;
    RKRenderState::CullScope mCullFace;
public:
    RKRenderPresetObjectSelect()
        : mBlend(false),
          mCullFace(true)
    { }
};

//----------------------------------------------------------------------------

class RKRenderPresetUIDefault
{
protected:
    RKRenderState::BlendScope mBlend;
    RKRenderState::CullScope mCullFace;
    RKRenderState::DepthScope mDepthTest;
public:
    RKRenderPresetUIDefault()
        : mBlend(true),
          mCullFace(false),
          mDepthTest(false, true, RKRenderState::CompareOp::LessEqual)
    {}
};

//----------------------------------------------------------------------------

class RKRenderPresetPipeline
{
protected:
    RKRenderState::CullScope mCullFace;
    RKRenderState::DepthScope mDepthTest;
public:
    RKRenderPresetPipeline()
        : mCullFace(true),
          mDepthTest(true, true, RKRenderState::CompareOp::LessEqual)
    { }
};

class RKRenderPresetPipelineAlpha // : public RKRenderPresetPipeline
{
protected:
    RKRenderState::BlendScope mBlend;
public:
    RKRenderPresetPipelineAlpha()
        : mBlend(true)
    { }
};

class RKRenderPresetPipelineSelection
{
protected:
    RKRenderState::CullScope mCullFace;
public:
    RKRenderPresetPipelineSelection()
        : mCullFace(false)
    {}
};

class RKRenderPresetPipelineSkyBox
{
protected:
    RKRenderState::CullScope mCullFace;
    RKRenderSquashToFarClip mSquashClip;
public:
    RKRenderPresetPipelineSkyBox();
   ~RKRenderPresetPipelineSkyBox();
};

class RKRenderPresetPipelineDepthTestSkyBox : public RKRenderPresetPipelineSkyBox
{
public:
    RKRenderPresetPipelineDepthTestSkyBox(bool depth_test, bool depth_write);

    RKRenderState::DepthScope mDepth;
};

class RKRenderPresetPipelineBlendSkyBox : public RKRenderPresetPipelineDepthTestSkyBox
{
public:
    RKRenderPresetPipelineBlendSkyBox(bool depth_test, bool depth_write);
    RKRenderState::BlendScope mBlend;
};

class RKRenderPresetTracker
{
protected:
    RKRenderState::CullScope mCullFace;
    RKRenderState::BlendScope mBlend;
public:
    RKRenderPresetTracker() :
        mCullFace(true),
        mBlend(true)
    { }
};

//----------------------------------------------------------------------------

//----------------------------------------------------------------------------

#endif
