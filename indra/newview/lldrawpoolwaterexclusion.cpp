/**
 * @file lldrawpool.cpp
 * @brief LLDrawPoolMaterials class implementation
 * @author Jonathan "Geenz" Goodman
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2013, Linden Research, Inc.
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

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolwaterexclusion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "rkrendercommonfunc.h"
#include "llvoavatar.h"
#include "lldrawpoolwater.h"

LLDrawPoolWaterExclusion::LLDrawPoolWaterExclusion() : LLRenderPass(LLDrawPool::POOL_WATEREXCLUSION)
{
    LL_INFOS("DPInvisible") << "Creating water exclusion draw pool" << LL_ENDL;
}


void LLDrawPoolWaterExclusion::render(S32 pass)
{                                             // render invisiprims
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; // LL_RECORD_BLOCK_TIME(FTM_RENDER_INVISIBLE);

    RKShaderProgram* color_program = &gDrawColorProgram;      // S6.6: the water planes and the invisible draws come as records of entries
    const bool pull = true;
    if (gPipeline.shadersLoaded())
    {
        color_program->bind();
    }


    RKRenderState::DepthScope depth(true);
    color_program->uniform4f(LLShaderMgr::DIFFUSE_COLOR, 1, 1, 1, 1);

    LLDrawPoolWater* pwaterpool = (LLDrawPoolWater*)gPipeline.getPool(LLDrawPool::POOL_WATER);
    if (pwaterpool)
    {
        // Just treat our water planes as double sided for the purposes of generating the exclusion mask.
        RKRenderState::CullScope cullface(false);
        // (the planes once: the edge water tiles are faces of the same pool, and a second push of the same entries wrote the same mask)
        pwaterpool->pushWaterPlanes();
    }

    color_program->uniform4f(LLShaderMgr::DIFFUSE_COLOR, 0, 0, 0, 1);

    static LLStaticHashedString waterSign("waterSign");
    color_program->uniform1f(waterSign, 1.f);

    if (pull)
    {
        // C5: the invisible draws are table entries like the other pass types: the worker draws the visible fragments' items with the colour program's pull twin bound now
        pushGeometryPackets(LLRenderPass::PASS_INVISIBLE);
    }

    if (gPipeline.shadersLoaded())
    {
        color_program->unbind();
    }
}
