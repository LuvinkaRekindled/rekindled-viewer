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

    // <FS> S6 X4: the glTF draws from the scene table (pbr pull program, rows with the PBR values); what it cannot carry (double sided, texture matrix) is drawn the old way
    if (deferredPacketsEnabled() && gPbrOpaquePullReady)
    {
        std::vector<LLDrawInfo*> fallback;
        gDeferredPBROpaquePullProgram.bind();
        pushGeometryPackets(mRenderType, fallback);
        if (!fallback.empty())
        {
            gDeferredPBROpaqueProgram.bind();
            for (LLDrawInfo* params : fallback)
            {
                pushGLTFBatch(*params);
            }
        }

        LL::GLTFSceneManager::instance().render(true, true);

        if (ll_rigged_packets_enabled() && gPbrOpaquePullSkinReady)
        {
            fallback.clear();
            gDeferredPBROpaquePullSkinProgram.bind();
            pushGeometryPackets(mRenderType + 1, fallback, nullptr, true);
            if (!fallback.empty())
            {
                gDeferredPBROpaqueProgram.bind(true);
                const LLVOAvatar* lastAvatar = nullptr;
                U64 lastMeshId = 0;
                bool skipLastSkin = false;
                for (LLDrawInfo* params : fallback)
                {
                    pushRiggedGLTFBatch(*params, lastAvatar, lastMeshId, skipLastSkin);
                }
            }
        }
        else
        {
            gDeferredPBROpaqueProgram.bind(true);
            pushRiggedGLTFBatches(mRenderType + 1);
        }
        return;
    }

    gDeferredPBROpaqueProgram.bind();
    pushGLTFBatches(mRenderType);

    LL::GLTFSceneManager::instance().render(true, true);

    gDeferredPBROpaqueProgram.bind(true);
    pushRiggedGLTFBatches(mRenderType + 1);
}

S32 LLDrawPoolGLTFPBR::getNumPostDeferredPasses()
{
    return 1;
}

void LLDrawPoolGLTFPBR::renderPostDeferred(S32 pass)
{
    if (LLPipeline::sRenderingHUDs)
    {
        gHUDPBROpaqueProgram.bind();
        pushGLTFBatches(mRenderType);
    }
    else if (mRenderType == LLPipeline::RENDER_TYPE_PASS_GLTF_PBR) // HACK -- don't render glow except for the non-alpha masked implementation
    {
        gRender.setColorMask(false, true);
        if (deferredPacketsEnabled() && gAlphaGlowPbrReady)
        {   // S6 X4: the glTF glow from the scene table (the PBR glow pull programs of the alpha glow, the same rows)
            std::vector<LLDrawInfo*> fallback;
            gPBRGlowPullProgram.bind();
            pushGeometryPackets(LLRenderPass::PASS_GLTF_GLOW, fallback);
            if (!fallback.empty())
            {
                gPBRGlowProgram.bind();
                for (LLDrawInfo* params : fallback)
                {
                    pushGLTFBatch(*params);
                }
            }
            if (ll_rigged_packets_enabled())
            {
                fallback.clear();
                gPBRGlowPullSkinProgram.bind();
                pushGeometryPackets(LLRenderPass::PASS_GLTF_GLOW_RIGGED, fallback, nullptr, true);
                if (!fallback.empty())
                {
                    gPBRGlowProgram.bind(true);
                    const LLVOAvatar* lastAvatar = nullptr;
                    U64 lastMeshId = 0;
                    bool skipLastSkin = false;
                    for (LLDrawInfo* params : fallback)
                    {
                        pushRiggedGLTFBatch(*params, lastAvatar, lastMeshId, skipLastSkin);
                    }
                }
            }
            else
            {
                gPBRGlowProgram.bind(true);
                pushRiggedGLTFBatches(LLRenderPass::PASS_GLTF_GLOW_RIGGED);
            }
            gRender.setColorMask(true, false);
            return;
        }
        gPBRGlowProgram.bind();
        pushGLTFBatches(LLRenderPass::PASS_GLTF_GLOW);

        gPBRGlowProgram.bind(true);
        pushRiggedGLTFBatches(LLRenderPass::PASS_GLTF_GLOW_RIGGED);

        gRender.setColorMask(true, false);
    }
}

