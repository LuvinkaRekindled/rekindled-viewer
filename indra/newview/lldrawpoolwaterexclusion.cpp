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
#include "llrendercommonfunc.h"
#include "llvoavatar.h"
#include "lldrawpoolwater.h"

LLDrawPoolWaterExclusion::LLDrawPoolWaterExclusion() : LLRenderPass(LLDrawPool::POOL_WATEREXCLUSION)
{
    LL_INFOS("DPInvisible") << "Creating water exclusion draw pool" << LL_ENDL;
}


void LLDrawPoolWaterExclusion::render(S32 pass)
{                                             // render invisiprims
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; // LL_RECORD_BLOCK_TIME(FTM_RENDER_INVISIBLE);

    LLShaderProgram* color_program = ll_pull_twin(&gDrawColorProgram);      // S6.6: the water planes and the invisible draws come as records of entries
    const bool pull = color_program != nullptr;
    if (!pull)
    {
        color_program = &gDrawColorProgram;
    }
    if (gPipeline.shadersLoaded())
    {
        color_program->bind();
    }


    LLRenderState::DepthScope depth(true);
    color_program->uniform4f(LLShaderMgr::DIFFUSE_COLOR, 1, 1, 1, 1);

    LLDrawPoolWater* pwaterpool = (LLDrawPoolWater*)gPipeline.getPool(LLDrawPool::POOL_WATER);
    if (pwaterpool)
    {
        // Just treat our water planes as double sided for the purposes of generating the exclusion mask.
        LLRenderState::CullScope cullface(false);
        pwaterpool->pushWaterPlanes(0, pull);

        // Take care of the edge water tiles.
        pwaterpool->pushWaterPlanes(1, pull);
    }

    color_program->uniform4f(LLShaderMgr::DIFFUSE_COLOR, 0, 0, 0, 1);

    static LLStaticHashedString waterSign("waterSign");
    color_program->uniform1f(waterSign, 1.f);

    if (pull)
    {
        static const bool inv_table = !getenv("LL_VK_INV_TABLE") || getenv("LL_VK_INV_TABLE")[0] != '0';
        if (inv_table && deferredPacketsEnabled())
        {
            // C5: the invisible draws are table entries like the other pass types: the worker draws the visible fragments' items with the colour program's pull twin bound now, the render map's rest is built here
            // (a draw the packet layout refuses is not drawn: the pull batch refused the same ones)
            std::vector<LLDrawInfo*> refused;
            pushGeometryPackets(LLRenderPass::PASS_INVISIBLE, refused);
        }
        else
        {
            pushBatchesPull(LLRenderPass::PASS_INVISIBLE);
        }
    }
    else
    {
        pushBatches(LLRenderPass::PASS_INVISIBLE, false, false);
    }


    if (gPipeline.shadersLoaded())
    {
        color_program->unbind();
    }
}
