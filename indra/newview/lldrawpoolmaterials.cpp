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

#include "lldrawpoolmaterials.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "rkrendercommonfunc.h"
#include "rkgpuzonetimer.h"
#include "llvoavatar.h"

LLDrawPoolMaterials::LLDrawPoolMaterials()
:  LLRenderPass(LLDrawPool::POOL_MATERIALS)
{

}

void LLDrawPoolMaterials::prerender()
{
    mShaderLevel = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT);
}

S32 LLDrawPoolMaterials::getNumDeferredPasses()
{
    // 12 render passes times 2 (one for each rigged and non rigged)
    return 12*2;
}

void LLDrawPoolMaterials::beginDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;
    LL_CPU_ZONE("materials: beginDeferredPass");

    bool rigged = false;
    if (pass >= 12)
    {
        rigged = true;
        pass -= 12;
    }
    // <FS> the draws of a pass are one scene table record, drawn by renderDeferred with the pull program of the pass (bound there, when something is drawn with it); there is no program of the vertex buffer path
    // (it was the old Material Shader of the pass, bound only where the records were not available: they are the only way these passes draw, PLAN-twin-removal.md, section 8)
    mUntouched = false;
    if (rigged)
    {
        // (the table draws the rigged materials passes in the main view only: pushGeometryPackets returns in any other view, so there is nothing to bind there; the render map is not asked, 2026-11-12)
        if (!(ll_scene_geo_serving() && ll_scene_geo_main_view()))
        {
            mUntouched = true;
            ll_scene_geo_count(4, 1);
        }
        return;
    }
    ll_scene_geo_count(5, 1);
}

void LLDrawPoolMaterials::endDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;
    LL_CPU_ZONE("materials: endDeferredPass");

    if (!mUntouched)
    {
        LLShaderProgram::unbind();      // (the pull program renderDeferred bound)
    }
    mUntouched = false;

    LLRenderPass::endRenderPass(pass);
}

// <FS> the index in gDeferredMaterialProgram[] of the program of each of the 12 deferred passes (the same table as in beginDeferredPass)
static const U32 sMaterialPassShader[] = { 0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15 };


void LLDrawPoolMaterials::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;
    LL_CPU_ZONE("materials: renderDeferred");
    static const U32 type_list[] =
    {
        LLRenderPass::PASS_MATERIAL,
        //LLRenderPass::PASS_MATERIAL_ALPHA,
        LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP,
        //LLRenderPass::PASS_SPECMAP_BLEND,
        LLRenderPass::PASS_SPECMAP_MASK,
        LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,
        //LLRenderPass::PASS_NORMMAP_BLEND,
        LLRenderPass::PASS_NORMMAP_MASK,
        LLRenderPass::PASS_NORMMAP_EMISSIVE,
        LLRenderPass::PASS_NORMSPEC,
        //LLRenderPass::PASS_NORMSPEC_BLEND,
        LLRenderPass::PASS_NORMSPEC_MASK,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE,
    };

    bool rigged = false;
    if (pass >= 12)
    {
        rigged = true;
        pass -= 12;
    }

    llassert(pass < sizeof(type_list)/sizeof(U32));

    U32 type = type_list[pass];
    if (rigged)
    {
        type += 1;
    }
    if (mUntouched)
    {
        return;         // <FS> S3 G4: beginDeferredPass found nothing to draw
    }

    // <FS> J1 K6 A3: the draws of this pass are one scene table record (the pull program of this pass bound; S4 A1: the rigged draws too, with the rigged pull program)
    if (rigged ? ll_rigged_packets_enabled() : true)
    {
        const U32 idx = sMaterialPassShader[pass];
        LLShaderProgram* pull = rigged ? &gDeferredMaterialPullSkinProgram[idx] : &gDeferredMaterialPullProgram[idx];
        {
            LL_CPU_ZONE("materials: bind pull program");
            // the native G-buffer programs have no texture slot and read none of what bindDeferredShader sets (shadow matrices, clip planes, SSAO, sun and moon direction, probe LOD; sunlight_color and
            // moonlight_color are in the block but no G-buffer file reads them): the plain bind, as the simple pool has it (AUDIT-shaders-2026-11-18.md section 1 item 10)
            pull->bind();
        }
        {
            LL_CPU_ZONE("materials: push packets");
            pushGeometryPackets(type, rigged);
        }
    }
}
