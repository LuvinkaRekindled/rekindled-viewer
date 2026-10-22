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
    LLShaderProgram* shader = &gDeferredEmissiveProgram;

    LLRenderState::BlendScope blend(true);
    gRender.flush();
    /// Get rid of z-fighting with non-glow pass.
    LLRenderState::PolygonOffsetScope polyOffset(true);
    gRender.setPolygonOffset(-1.0f, -1.0f);
    gRender.setSceneBlendType(LLRender::BT_ADD);

    LLRenderState::DepthScope depth(true, false);
    gRender.setColorMask(false, true);

    // <FS> S6 X2: the glow draws from the scene table (the emissive pull programs of the alpha glow, the same rows); what it cannot carry is drawn the old way
    if (!LLPipeline::sRenderingHUDs && deferredPacketsEnabled() && gAlphaGlowPullReady)
    {
        std::vector<LLDrawInfo*> fallback;
        gDeferredEmissivePullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_GLOW, fallback);
        if (!fallback.empty())
        {
            shader->bind();
            for (LLDrawInfo* params : fallback)
            {
                pushBatch(*params, true, true);
            }
        }
        shader = shader->mRiggedVariant;
        if (ll_rigged_packets_enabled())
        {
            fallback.clear();
            gDeferredEmissivePullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_GLOW_RIGGED, fallback, nullptr, true);
            if (!fallback.empty())
            {
                shader->bind();
                const LLVOAvatar* lastAvatar = nullptr;
                U64 lastMeshId = 0;
                bool skipLastSkin = false;
                for (LLDrawInfo* params : fallback)
                {
                    if (params->mSkinInfo.notNull() && uploadMatrixPalette(params->mAvatar, params->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                    {
                        pushBatch(*params, true, true);
                    }
                }
            }
        }
        else
        {
            shader->bind();
            pushRiggedBatches(LLRenderPass::PASS_GLOW_RIGGED, true, true);
        }
        gRender.setColorMask(true, false);
        gRender.setSceneBlendType(LLRender::BT_ALPHA);
        return;
    }

    //first pass -- static objects
    shader->bind();
    pushBatches(LLRenderPass::PASS_GLOW, true, true);

    // second pass -- rigged objects
    shader = shader->mRiggedVariant;
    shader->bind();
    pushRiggedBatches(LLRenderPass::PASS_GLOW_RIGGED, true, true);

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

    //render static
    if (deferredPacketsEnabled())
    {
        // <FS> render packets: the static draws as one record; what it cannot carry is drawn the old way
        std::vector<LLDrawInfo*> fallback;
        gDeferredDiffusePullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_SIMPLE, fallback);
        if (!fallback.empty())
        {
            gDeferredDiffuseProgram.bind();
            for (LLDrawInfo* params : fallback)
            {
                pushBatch(*params, true, true);
            }
        }
    }
    else
    {
        gDeferredDiffuseProgram.bind();
        pushBatches(LLRenderPass::PASS_SIMPLE, true, true);
    }

    //render rigged
    if (ll_rigged_packets_enabled())
    {
        // <FS> S4 A1: the rigged draws as one record (the rigged pull program); what it cannot carry is drawn the old way
        std::vector<LLDrawInfo*> fallback;
        gDeferredDiffusePullSkinProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_SIMPLE_RIGGED, fallback, nullptr, true);
        if (!fallback.empty())
        {
            gDeferredDiffuseProgram.bind(true);
            const LLVOAvatar* lastAvatar = nullptr;
            U64 lastMeshId = 0;
            bool skipLastSkin = false;
            for (LLDrawInfo* params : fallback)
            {
                if (params->mSkinInfo.notNull() && uploadMatrixPalette(params->mAvatar, params->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                {
                    pushBatch(*params, true, true);
                }
            }
        }
        return;
    }
    gDeferredDiffuseProgram.bind(true);
    pushRiggedBatches(LLRenderPass::PASS_SIMPLE_RIGGED, true, true);
}

static LLTrace::BlockTimerStatHandle FTM_RENDER_ALPHA_MASK_DEFERRED("Deferred Alpha Mask");


void LLDrawPoolAlphaMask::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_ALPHA_MASK_DEFERRED);
    LLShaderProgram* shader = &gDeferredDiffuseAlphaMaskProgram;

    //render static
    shader->bind();
    pushMaskBatches(LLRenderPass::PASS_ALPHA_MASK, true, true);

    //render rigged
    shader->bind(true);
    pushRiggedMaskBatches(LLRenderPass::PASS_ALPHA_MASK_RIGGED, true, true);
}

// grass drawpool
LLDrawPoolGrass::LLDrawPoolGrass() :
 LLRenderPass(POOL_GRASS)
{

}

void LLDrawPoolGrass::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    {
        gDeferredNonIndexedDiffuseAlphaMaskProgram.bind();
        gDeferredNonIndexedDiffuseAlphaMaskProgram.setMinimumAlpha(0.5f);

        //render grass
        LLRenderPass::pushBatches(LLRenderPass::PASS_GRASS, getVertexDataMask());
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

    LLShaderProgram* shader = nullptr;
    if (LLPipeline::sRenderingHUDs)
    {
        shader = &gHUDFullbrightProgram;
    }
    else
    {
        shader = &gDeferredFullbrightProgram;
    }

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    // <FS> S6 X1: the opaque fullbright draws from the scene table (the pull program, the same entries as the diffuse pass); what it cannot carry is drawn the old way. HUDs stay on the old path.
    if (!LLPipeline::sRenderingHUDs && deferredPacketsEnabled() && gFullbrightPullReady)
    {
        std::vector<LLDrawInfo*> fallback;
        gDeferredFullbrightPullProgram.bind();
        pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT, fallback);
        if (!fallback.empty())
        {
            shader->bind();
            for (LLDrawInfo* params : fallback)
            {
                pushBatch(*params, true, true);
            }
        }
        if (ll_rigged_packets_enabled() && gFullbrightPullSkinReady)
        {
            fallback.clear();
            gDeferredFullbrightPullSkinProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_FULLBRIGHT_RIGGED, fallback, nullptr, true);
            if (!fallback.empty())
            {
                shader->bind(true);
                const LLVOAvatar* lastAvatar = nullptr;
                U64 lastMeshId = 0;
                bool skipLastSkin = false;
                for (LLDrawInfo* params : fallback)
                {
                    if (params->mSkinInfo.notNull() && uploadMatrixPalette(params->mAvatar, params->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                    {
                        pushBatch(*params, true, true);
                    }
                }
            }
            return;
        }
        shader->bind(true);
        pushRiggedBatches(LLRenderPass::PASS_FULLBRIGHT_RIGGED, true, true);
        return;
    }

    // render static
    shader->bind();
    pushBatches(LLRenderPass::PASS_FULLBRIGHT, true, true);

    if (!LLPipeline::sRenderingHUDs)
    {
        // render rigged
        shader->bind(true);
        pushRiggedBatches(LLRenderPass::PASS_FULLBRIGHT_RIGGED, true, true);
    }
}

void LLDrawPoolFullbrightAlphaMask::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_FULLBRIGHT);

    // render unrigged unlit GLTF
    LL::GLTFSceneManager::instance().render(true, false, true);
    LL::GLTFSceneManager::instance().render(true, true, true);

    LLShaderProgram* shader = nullptr;
    if (LLPipeline::sRenderingHUDs)
    {
        shader = &gHUDFullbrightAlphaMaskProgram;
    }
    else
    {
        shader = &gDeferredFullbrightAlphaMaskProgram;
    }

    LLRenderState::BlendScope blend(false);

    // render static
    shader->bind();
    pushMaskBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, true, true);

    if (!LLPipeline::sRenderingHUDs)
    {
        // render rigged
        shader->bind(true);
        pushRiggedMaskBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, true, true);
    }
}

