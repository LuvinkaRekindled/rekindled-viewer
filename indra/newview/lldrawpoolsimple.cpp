/**
 * @file lldrawpoolsimple.cpp
 * @brief LLDrawPoolSimple class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolsimple.h"

#include "llviewercamera.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llviewershadermgr.h"
#include "llrender.h"
#include "gltfscenemanager.h"

static LLTrace::BlockTimerStatHandle FTM_RENDER_SIMPLE_DEFERRED("Deferred Simple");
static LLTrace::BlockTimerStatHandle FTM_RENDER_GRASS_DEFERRED("Deferred Grass");


void LLDrawPoolGlow::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    LLRenderState::BlendScope blend(true);
    gRender.flush();
    /// Get rid of z-fighting with non-glow pass.
    LLRenderState::PolygonOffsetScope polyOffset(true);
    gRender.setPolygonOffset(-1.0f, -1.0f);
    gRender.setSceneBlendType(LLRender::BT_ADD);

    LLRenderState::DepthScope depth(true, false);
    gRender.setColorMask(false, true);

    // <FS> J1 K6 A3: the glow draws come from the scene table (the emissive pull programs of the alpha glow, the same rows); a HUD has no glow pool
    if (!LLPipeline::sRenderingHUDs && gAlphaGlowPullReady)
    {
        gDeferredEmissivePullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_GLOW);
        if (ll_rigged_packets_enabled())
        {
            gDeferredEmissivePullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_GLOW_RIGGED, true);
        }
    }

    gRender.setColorMask(true, false);
    gRender.setSceneBlendType(LLRender::BT_ALPHA);
}

LLDrawPoolSimple::LLDrawPoolSimple() :
    LLRenderPass(POOL_SIMPLE)
{
}

static LLTrace::BlockTimerStatHandle FTM_RENDER_ALPHA_MASK("Alpha Mask");

LLDrawPoolAlphaMask::LLDrawPoolAlphaMask() :
    LLRenderPass(POOL_ALPHA_MASK)
{
}

LLDrawPoolFullbrightAlphaMask::LLDrawPoolFullbrightAlphaMask() :
    LLRenderPass(POOL_FULLBRIGHT_ALPHA_MASK)
{
}

//===============================
//DEFERRED IMPLEMENTATION
//===============================

S32 LLDrawPoolSimple::getNumDeferredPasses()
{
    return 1;
}

void LLDrawPoolSimple::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_SIMPLE_DEFERRED);
    LLRenderState::BlendScope blend(false);

    // <FS> J1 K6 A3: the static and the rigged draws, each as one draw record of the scene table
    gDeferredDiffusePullProgram.bind();
    pushGeometryPackets(LLRenderPass::PASS_SIMPLE);

    if (ll_rigged_packets_enabled())
    {
        gDeferredDiffusePullSkinProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_SIMPLE_RIGGED, true);
    }
}

static LLTrace::BlockTimerStatHandle FTM_RENDER_ALPHA_MASK_DEFERRED("Deferred Alpha Mask");


void LLDrawPoolAlphaMask::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_ALPHA_MASK_DEFERRED);

    // <FS> J1 K6 A3: the alpha masked draws from the scene table (the cutoff of each draw in its row)
    if (gDiffuseMaskPullReady)
    {
        gDeferredDiffuseAlphaMaskPullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_ALPHA_MASK);
        if (ll_rigged_packets_enabled() && gDiffuseMaskPullSkinReady)
        {
            gDeferredDiffuseAlphaMaskPullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_ALPHA_MASK_RIGGED, true);
        }
    }
}

// grass drawpool
LLDrawPoolGrass::LLDrawPoolGrass() :
 LLRenderPass(POOL_GRASS)
{

}

void LLDrawPoolGrass::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // <FS> J1 K6 A3: the grass draws from the scene table (the alpha masked diffuse pull program, the 0.5 cutoff in each row)
    if (gDiffuseMaskPullReady)
    {
        gDeferredDiffuseAlphaMaskPullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_GRASS);
    }
}


// Fullbright drawpool
LLDrawPoolFullbright::LLDrawPoolFullbright() :
    LLRenderPass(POOL_FULLBRIGHT)
{
}

void LLDrawPoolFullbright::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_FULLBRIGHT);

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    if (LLPipeline::sRenderingHUDs)
    {
        // <FS> J1 K6 S1: the HUD's fullbright draws from the scene table (the HUD's cull result was told to the worker before the passes); a HUD has no rigged draws
        if (ll_scene_geo_hud_view() && ll_scene_geo_prepared(gPipeline.getCull()) && ll_scene_geo_hud_type(ll_scene_geo_type_number(LLRenderPass::PASS_FULLBRIGHT)))
        {
            gHUDFullbrightPullProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT);
        }
        return;
    }

    // <FS> J1 K6 A3: the opaque fullbright draws from the scene table (the pull program, the same entries as the diffuse pass)
    if (gFullbrightPullReady)
    {
        gDeferredFullbrightPullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT);
        if (ll_rigged_packets_enabled() && gFullbrightPullSkinReady)
        {
            gDeferredFullbrightPullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT_RIGGED, true);
        }
    }
}

void LLDrawPoolFullbrightAlphaMask::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_FULLBRIGHT);

    // render unrigged unlit GLTF
    LL::GLTFSceneManager::instance().render(true, false, true);
    LL::GLTFSceneManager::instance().render(true, true, true);

    LLRenderState::BlendScope blend(false);

    if (LLPipeline::sRenderingHUDs)
    {
        // <FS> J1 K6 S1: the HUD's alpha masked fullbright draws from the scene table
        if (ll_scene_geo_hud_view() && ll_scene_geo_prepared(gPipeline.getCull()) && ll_scene_geo_hud_type(ll_scene_geo_type_number(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK)))
        {
            gHUDFullbrightMaskPullProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK);
        }
        return;
    }

    // <FS> J1 K6 A3: the alpha masked fullbright draws from the scene table
    if (gFullbrightMaskPullReady)
    {
        gDeferredFullbrightAlphaMaskPullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK);
        if (ll_rigged_packets_enabled() && gFullbrightMaskPullSkinReady)
        {
            gDeferredFullbrightAlphaMaskPullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, true);
        }
    }
}
