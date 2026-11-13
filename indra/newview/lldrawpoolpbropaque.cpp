/**
 * @file lldrawpoolpbropaque.cpp
 * @brief LLDrawPoolGLTFPBR class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#include "lldrawpool.h"
#include "lldrawpoolpbropaque.h"
#include "llviewershadermgr.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "gltfscenemanager.h"

LLDrawPoolGLTFPBR::LLDrawPoolGLTFPBR(U32 type) :
    LLRenderPass(type)
{
    if (type == LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK)
    {
        mRenderType = LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK;
    }
    else
    {
        mRenderType = LLPipeline::RENDER_TYPE_PASS_GLTF_PBR;
    }
}

S32 LLDrawPoolGLTFPBR::getNumDeferredPasses()
{
    return 1;
}

void LLDrawPoolGLTFPBR::renderDeferred(S32 pass)
{
    llassert(!LLPipeline::sRenderingHUDs);

    if (mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK)
    {
        LL::GLTFSceneManager::instance().renderOpaque();
    }

    // <FS> J1 K6 A3: the glTF draws are scene table records (pbr pull programs, rows with the PBR values)
    gDeferredPBROpaquePullProgram.bind();
    pushGeometryPackets(mRenderType);

    LL::GLTFSceneManager::instance().render(true, true);

    if (ll_rigged_packets_enabled())
    {
        gDeferredPBROpaquePullSkinProgram.bind();
        pushGeometryPackets(mRenderType + 1, true);
    }
}

S32 LLDrawPoolGLTFPBR::getNumPostDeferredPasses()
{
    return 1;
}

void LLDrawPoolGLTFPBR::renderPostDeferred(S32 pass)
{
    if (LLPipeline::sRenderingHUDs)
    {
        // <FS> J1 K6 S1: the HUD's glTF opaque / mask draws from the scene table (the HUD's cull result was told to the worker before the passes)
        if (ll_scene_geo_hud_view() && ll_scene_geo_prepared(gPipeline.getCull()) && ll_scene_geo_hud_type(ll_scene_geo_type_number(mRenderType)))
        {
            gHUDPBROpaquePullProgram.bind();
            pushGeometryPackets(mRenderType);
        }
    }
    else if (mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR) // HACK -- don't render glow except for the non-alpha masked implementation
    {
        gRender.setColorMask(false, true);
        if (gAlphaGlowPbrReady)
        {   // J1 K6 A3: the glTF glow from the scene table (the PBR glow pull programs of the alpha glow, the same rows)
            // <FS> Rekindled (2026-11-10): the glTF opaque glow writes the glow too: see LLDrawPoolGlow::renderPostDeferred
            if (LLPipeline::sRenderGlow && (ll_scene_geo_cull_has_type(gPipeline.getCull(), LLRenderPass::PASS_GLTF_GLOW) || (ll_rigged_packets_enabled() && ll_scene_geo_cull_has_type(gPipeline.getCull(), LLRenderPass::PASS_GLTF_GLOW_RIGGED))))
            {
                LLPipeline::sGlowDrawn = true;
            }
            gPBRGlowPullProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_GLTF_GLOW);
            if (ll_rigged_packets_enabled())
            {
                gPBRGlowPullSkinProgram.bind();
                pushGeometryPackets(LLRenderPass::PASS_GLTF_GLOW_RIGGED, true);
            }
        }
        gRender.setColorMask(true, false);
    }
}
