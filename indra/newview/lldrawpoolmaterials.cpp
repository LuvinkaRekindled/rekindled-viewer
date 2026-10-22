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
#include "llrendercommonfunc.h"
#include "llgpuzonetimer.h"
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
    U32 shader_idx[] =
    {
        0, //LLRenderPass::PASS_MATERIAL,
        //1, //LLRenderPass::PASS_MATERIAL_ALPHA,
        2, //LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        3, //LLRenderPass::PASS_MATERIAL_ALPHA_GLOW,
        4, //LLRenderPass::PASS_SPECMAP,
        //5, //LLRenderPass::PASS_SPECMAP_BLEND,
        6, //LLRenderPass::PASS_SPECMAP_MASK,
        7, //LLRenderPass::PASS_SPECMAP_GLOW,
        8, //LLRenderPass::PASS_NORMMAP,
        //9, //LLRenderPass::PASS_NORMMAP_BLEND,
        10, //LLRenderPass::PASS_NORMMAP_MASK,
        11, //LLRenderPass::PASS_NORMMAP_GLOW,
        12, //LLRenderPass::PASS_NORMSPEC,
        //13, //LLRenderPass::PASS_NORMSPEC_BLEND,
        14, //LLRenderPass::PASS_NORMSPEC_MASK,
        15, //LLRenderPass::PASS_NORMSPEC_GLOW,
    };

    U32 idx = shader_idx[pass];

    mShader = &(gDeferredMaterialProgram[idx]);

    if (rigged)
    {
        llassert(mShader->mRiggedVariant != nullptr);
        mShader = mShader->mRiggedVariant;
    }

    // <FS> S3 G4: the program of a pass is bound (deferred shader setup: lights, shadow maps, probes) only when something is drawn with it. A rigged pass with no draws binds nothing; a static pass
    // that the packets draw binds its pull program in renderDeferred, and this program only if a draw is left for it
    mUntouched = false;
    if (deferredPacketsEnabled())
    {
        static const U32 pass_types[] =
        {
            LLRenderPass::PASS_MATERIAL, LLRenderPass::PASS_MATERIAL_ALPHA_MASK, LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE, LLRenderPass::PASS_SPECMAP, LLRenderPass::PASS_SPECMAP_MASK,
            LLRenderPass::PASS_SPECMAP_EMISSIVE, LLRenderPass::PASS_NORMMAP, LLRenderPass::PASS_NORMMAP_MASK, LLRenderPass::PASS_NORMMAP_EMISSIVE, LLRenderPass::PASS_NORMSPEC,
            LLRenderPass::PASS_NORMSPEC_MASK, LLRenderPass::PASS_NORMSPEC_EMISSIVE,
        };
        static const bool lazy = !(getenv("LL_VK_POOL_LAZY") && getenv("LL_VK_POOL_LAZY")[0] == '0');
        if (lazy)
        {
            if (rigged)
            {
                const U32 type = pass_types[pass] + 1;
                // (S4 B: with the table serving the rigged draws the render map is empty on purpose: the pass still sends the table's draw record)
                if (gPipeline.beginRenderMap(type) == gPipeline.endRenderMap(type) && !(ll_scene_geo_rigged_env() && ll_scene_geo_serving() && ll_scene_geo_main_view()))
                {
                    mUntouched = true;
                    ll_scene_geo_count(4, 1);
                    return;
                }
                if (ll_rigged_packets_enabled() && gDeferredMaterialPullSkinProgram[idx].mProgramObject != 0)
                {
                    return;     // S4 A1: renderDeferred binds the rigged pull program, and this one only if a draw is left for it
                }
            }
            else if (gDeferredMaterialPullProgram[idx].mProgramObject != 0)
            {
                ll_scene_geo_count(5, 1);
                return;
            }
        }
    }

    gPipeline.bindDeferredShader(*mShader);
}

void LLDrawPoolMaterials::endDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MATERIAL;
    LL_CPU_ZONE("materials: endDeferredPass");

    if (!mUntouched)
    {
        mShader->unbind();      // (unbinds whatever program is current)
    }
    mUntouched = false;

    LLRenderPass::endRenderPass(pass);
}

// <FS> the index in gDeferredMaterialProgram[] of the program of each of the 12 deferred passes (the same table as in beginDeferredPass)
static const U32 sMaterialPassShader[] = { 0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15 };

// <FS> the geometry records of the deferred passes start building (on the async pool) before the pools draw: the same passes, programs and map flags renderDeferred() will ask for
void LLDrawPoolMaterials::kickGeometryBuilds()
{
    static const U32 type_list[] =
    {
        LLRenderPass::PASS_MATERIAL, LLRenderPass::PASS_MATERIAL_ALPHA_MASK, LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE, LLRenderPass::PASS_SPECMAP, LLRenderPass::PASS_SPECMAP_MASK,
        LLRenderPass::PASS_SPECMAP_EMISSIVE, LLRenderPass::PASS_NORMMAP, LLRenderPass::PASS_NORMMAP_MASK, LLRenderPass::PASS_NORMMAP_EMISSIVE, LLRenderPass::PASS_NORMSPEC,
        LLRenderPass::PASS_NORMSPEC_MASK, LLRenderPass::PASS_NORMSPEC_EMISSIVE,
    };
    if (!deferredPacketsEnabled())
    {
        return;
    }
    kickGeometryBuild(LLRenderPass::PASS_SIMPLE);
    const bool rigged_too = ll_rigged_packets_enabled();       // S4 A1
    if (rigged_too)
    {
        kickGeometryBuild(LLRenderPass::PASS_SIMPLE_RIGGED, nullptr, true);
    }
    if (gAlphaGlowPullReady)
    {
        kickGeometryBuild(LLRenderPass::PASS_GLOW);             // S6 X2
        if (rigged_too)
        {
            kickGeometryBuild(LLRenderPass::PASS_GLOW_RIGGED, nullptr, true);
        }
    }
    if (gFullbrightPullReady)
    {
        kickGeometryBuild(LLRenderPass::PASS_FULLBRIGHT);       // S6 X1 (drawn after the lighting)
        if (rigged_too && gFullbrightPullSkinReady)
        {
            kickGeometryBuild(LLRenderPass::PASS_FULLBRIGHT_RIGGED, nullptr, true);
        }
    }
    for (U32 pass = 0; pass < sizeof(type_list) / sizeof(type_list[0]); ++pass)
    {
        const U32 idx = sMaterialPassShader[pass];
        LLGeometryPassInfo info;
        info.mNormalMap = (idx & 0x8) != 0;
        info.mSpecularMap = (idx & 0x4) != 0;
        if (gDeferredMaterialPullProgram[idx].mProgramObject != 0)
        {
            kickGeometryBuild(type_list[pass], &info);
        }
        if (rigged_too && gDeferredMaterialPullSkinProgram[idx].mProgramObject != 0)
        {
            kickGeometryBuild(type_list[pass] + 1, &info, true);
        }
    }
}

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

    // <FS> render packets: the static draws as one record (the pull program of this pass bound instead of the old one); what it cannot carry is drawn the old way below
    // (S4 A1: the rigged draws too, with the rigged pull program)
    std::vector<LLDrawInfo*> packet_fallback;
    bool packets_used = false;
    if (rigged ? ll_rigged_packets_enabled() : deferredPacketsEnabled())
    {
        const U32 idx = sMaterialPassShader[pass];
        LLShaderProgram* pull = rigged ? &gDeferredMaterialPullSkinProgram[idx] : &gDeferredMaterialPullProgram[idx];
        if (pull->mProgramObject != 0)
        {
            LLGeometryPassInfo info;
            info.mNormalMap = (idx & 0x8) != 0;
            info.mSpecularMap = (idx & 0x4) != 0;
            {
                LL_CPU_ZONE("materials: bind pull program");
                gPipeline.bindDeferredShader(*pull);
            }
            {
                LL_CPU_ZONE("materials: push packets");
                pushGeometryPackets(type, packet_fallback, &info, rigged);
            }
            packets_used = true;
            if (packet_fallback.empty())
            {
                return;
            }
            gPipeline.bindDeferredShader(*mShader);
        }
    }

    LLCullResult::drawinfo_iterator begin = gPipeline.beginRenderMap(type);
    LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(type);

    F32 lastIntensity = 0.f;
    F32 lastFullbright = 0.f;
    F32 lastMinimumAlpha = 0.f;
    LLVector4 lastSpecular = LLVector4(0, 0, 0, 0);

    S32 intensity = mShader->getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY);
    S32 brightness = mShader->getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS);
    S32 minAlpha = mShader->getUniformLocation(LLShaderMgr::MINIMUM_ALPHA);
    S32 specular = mShader->getUniformLocation(LLShaderMgr::SPECULAR_COLOR);

    S32 diffuseChannel = mShader->enableTexture(LLShaderMgr::DIFFUSE_MAP);
    S32 specChannel = mShader->enableTexture(LLShaderMgr::SPECULAR_MAP);
    S32 normChannel = mShader->enableTexture(LLShaderMgr::BUMP_MAP);

    LLTexture* lastNormalMap = nullptr;
    LLTexture* lastSpecMap = nullptr;
    LLTexture* lastDiffuse = nullptr;

    gRender.getTexUnit(diffuseChannel)->bindWhiteFast();

    if (intensity > -1)
    {
        LLShaderProgram::uniform1fAt(intensity, lastIntensity);
    }

    if (brightness > -1)
    {
        LLShaderProgram::uniform1fAt(brightness, lastFullbright);
    }

    if (minAlpha > -1)
    {
        LLShaderProgram::uniform1fAt(minAlpha, lastMinimumAlpha);
    }

    if (specular > -1)
    {
        LLShaderProgram::uniform4fvAt(specular, 1, lastSpecular.mV);
    }

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    auto draw_one = [&](LLDrawInfo& params)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_MATERIAL("materials draw loop");

        if (specular > -1 && params.mSpecColor != lastSpecular)
        {
            lastSpecular = params.mSpecColor;
            LLShaderProgram::uniform4fvAt(specular, 1, lastSpecular.mV);
        }

        if (intensity != -1 && lastIntensity != params.mEnvIntensity)
        {
            lastIntensity = params.mEnvIntensity;
            LLShaderProgram::uniform1fAt(intensity, lastIntensity);
        }

        if (minAlpha > -1 && lastMinimumAlpha != params.mAlphaMaskCutoff)
        {
            lastMinimumAlpha = params.mAlphaMaskCutoff;
            LLShaderProgram::uniform1fAt(minAlpha, lastMinimumAlpha);
        }

        F32 fullbright = params.mFullbright ? 1.f : 0.f;
        if (brightness > -1 && lastFullbright != fullbright)
        {
            lastFullbright = fullbright;
            LLShaderProgram::uniform1fAt(brightness, lastFullbright);
        }

        if (normChannel > -1 && params.mNormalMap != lastNormalMap)
        {
            lastNormalMap = params.mNormalMap;
            llassert(lastNormalMap);
            gRender.getTexUnit(normChannel)->bindFast(lastNormalMap);
        }

        if (specChannel > -1 && params.mSpecularMap != lastSpecMap)
        {
            lastSpecMap = params.mSpecularMap;
            llassert(lastSpecMap);
            gRender.getTexUnit(specChannel)->bindFast(lastSpecMap);
        }

        if (params.mTexture != lastDiffuse)
        {
            lastDiffuse = params.mTexture;
            if (lastDiffuse)
            {
                gRender.getTexUnit(diffuseChannel)->bindFast(lastDiffuse);
            }
            else
            {
                gRender.getTexUnit(diffuseChannel)->bindWhiteFast();
            }
        }

        // upload matrix palette to shader
        if (rigged)
        {
            if (!uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                return;
            }
        }

        applyModelMatrix(params);

        bool tex_setup = false;

        //not batching textures or batch has only 1 texture -- might need a texture matrix
        if (params.mTextureMatrix)
        {

            LLRenderMatrix::load(LLRenderMatrix::Kind::Texture0, (F32*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;

            tex_setup = true;
        }

        /*if (params.mGroup)  // TOO LATE
        {
            params.mGroup->rebuildMesh();
        }*/

        params.mVertexBuffer->setBuffer();
        params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

        if (tex_setup)
        {
            LLRenderMatrix::identity(LLRenderMatrix::Kind::Texture0);      // tex_setup: the texture matrix of unit 0 was made the current one above
        }
    };

    if (packets_used)
    {
        for (LLDrawInfo* params : packet_fallback)
        {
            if (rigged && params->mSkinInfo.isNull())
            {
                continue;       // (no skin: nothing to draw with a palette)
            }
            draw_one(*params);
        }
        return;
    }

    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo& params = **i;

        LLCullResult::increment_iterator(i, end);

        draw_one(params);
    }
}
