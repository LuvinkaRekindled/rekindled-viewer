/**
 * @file lldrawpoolterrain.cpp
 * @brief LLDrawPoolTerrain class implementation
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

#include "lldrawpoolterrain.h"

#include "llfasttimer.h"

#include "llagent.h"
#include "llviewercontrol.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llviewerregion.h"
#include "llvlcomposition.h"
#include "llviewerparcelmgr.h"      // for gRenderParcelOwnership
#include "llviewerparceloverlay.h"
#include "llvosurfacepatch.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h" // To get alpha gradients
#include "llworld.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llrender.h"
#include "llenvironment.h"
#include "llvkbridge.h"
#include "llvertexbuffer.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include "llsettingsvo.h"

const F32 DETAIL_SCALE = 1.f/16.f;
int DebugDetailMap = 0;

S32 LLDrawPoolTerrain::sPBRDetailMode = 0;
F32 LLDrawPoolTerrain::sDetailScale = DETAIL_SCALE;
F32 LLDrawPoolTerrain::sPBRDetailScale = DETAIL_SCALE;
static LLShaderProgram* sShader = NULL;
static bool sPull = false;      // S6.6a: the pass being drawn is the pull variant (drawPull)
static const bool pull_wanted = []() { const char* e = getenv("LL_VK_TERRAIN_PULL"); return !(e && e[0] == '0'); }();
static const bool pull_wanted_pbr = pull_wanted;
static LLTrace::BlockTimerStatHandle FTM_SHADOW_TERRAIN("Terrain Shadow");


LLDrawPoolTerrain::LLDrawPoolTerrain(LLViewerTexture *texturep) :
    LLFacePool(POOL_TERRAIN),
    mTexturep(texturep)
{
    // Hack!

    // <FS:PP> Attempt to speed up things a little
    // sDetailScale = 1.f/gSavedSettings.getF32("RenderTerrainScale");
    // sPBRDetailScale = 1.f/gSavedSettings.getF32("RenderTerrainPBRScale");
    // sPBRDetailMode = gSavedSettings.getS32("RenderTerrainPBRDetail");
    static LLCachedControl<F32> RenderTerrainScale(gSavedSettings, "RenderTerrainScale");
    static LLCachedControl<F32> RenderTerrainPBRScale(gSavedSettings, "RenderTerrainPBRScale");
    static LLCachedControl<S32> RenderTerrainPBRDetail(gSavedSettings, "RenderTerrainPBRDetail");
    sDetailScale = 1.f/RenderTerrainScale;
    sPBRDetailScale = 1.f/RenderTerrainPBRScale;
    sPBRDetailMode = RenderTerrainPBRDetail();
    // </FS:PP>
    mAlphaRampImagep = LLViewerTextureManager::getFetchedTexture(IMG_ALPHA_GRAD);


    //gRender.getTexUnit(0)->bind(mAlphaRampImagep.get());
    mAlphaRampImagep->setAddressMode(LLTexUnit::TAM_CLAMP);

    m2DAlphaRampImagep = LLViewerTextureManager::getFetchedTexture(IMG_ALPHA_GRAD_2D);

    //gRender.getTexUnit(0)->bind(m2DAlphaRampImagep.get());
    m2DAlphaRampImagep->setAddressMode(LLTexUnit::TAM_CLAMP);

    mTexturep->setBoostLevel(LLBaseTexture::BOOST_TERRAIN);

    //gRender.getTexUnit(0)->bindWhite();
}

LLDrawPoolTerrain::~LLDrawPoolTerrain()
{
    llassert( gPipeline.findPool( getType(), getTexture() ) == NULL );
}

U32 LLDrawPoolTerrain::getVertexDataMask()
{
    if (LLPipeline::sShadowRender)
    {
        return LLVertexBuffer::MAP_VERTEX;
    }
    else if (LLShaderProgram::sCurBoundShaderPtr)
    {
        return VERTEX_DATA_MASK & ~(LLVertexBuffer::MAP_TEXCOORD2 | LLVertexBuffer::MAP_TEXCOORD3);
    }
    else
    {
        return VERTEX_DATA_MASK;
    }
}

void LLDrawPoolTerrain::prerender()
{
    static LLCachedControl<S32> render_terrain_pbr_detail(gSavedSettings, "RenderTerrainPBRDetail");
    sPBRDetailMode = render_terrain_pbr_detail;
}

void LLDrawPoolTerrain::boostTerrainDetailTextures()
{
    // Hack! Get the region that this draw pool is rendering from!
    LLViewerRegion *regionp = mDrawFace[0]->getDrawable()->getVObj()->getRegion();
    LLVLComposition *compp = regionp->getComposition();
    compp->boost();
}

void LLDrawPoolTerrain::beginDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_TERRAIN);
    LLFacePool::beginRenderPass(pass);
}

void LLDrawPoolTerrain::endDeferredPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_TERRAIN);
    LLFacePool::endRenderPass(pass);
    sShader->unbind();
}

void LLDrawPoolTerrain::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_TERRAIN);
    if (mDrawFace.empty())
    {
        return;
    }

    boostTerrainDetailTextures();

    renderFullShader();

    // Special-case for land ownership feedback
    // <FS:Ansariel> Use faster LLCachedControls for frequently visited locations
    //if (gSavedSettings.getBOOL("ShowParcelOwners"))
    static LLCachedControl<bool> showParcelOwners(gSavedSettings, "ShowParcelOwners");
    if (showParcelOwners)
    // </FS:Ansariel>
    {
        hilightParcelOwners();
    }

}

void LLDrawPoolTerrain::beginShadowPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_SHADOW_TERRAIN);
    LLFacePool::beginRenderPass(pass);
    gRender.getTexUnit(0)->bindWhite();
    gDeferredShadowProgram.bind();

    LLEnvironment& environment = LLEnvironment::instance();
    gDeferredShadowProgram.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
}

void LLDrawPoolTerrain::endShadowPass(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_SHADOW_TERRAIN);
    LLFacePool::endRenderPass(pass);
    gDeferredShadowProgram.unbind();
}

void LLDrawPoolTerrain::renderShadow(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_SHADOW_TERRAIN);
    if (mDrawFace.empty())
    {
        return;
    }
    //LLGLEnable offset(GL_POLYGON_OFFSET);
    //glCullFace(GL_FRONT);
    drawLoop();
    //glCullFace(GL_BACK);
}


void LLDrawPoolTerrain::drawLoop()
{
    if (!mDrawFace.empty())
    {
        for (std::vector<LLFace*>::iterator iter = mDrawFace.begin();
             iter != mDrawFace.end(); iter++)
        {
            LLFace *facep = *iter;

            LLRenderPass::applyModelMatrix(&facep->getDrawable()->getRegion()->mRenderMatrix);

            facep->renderIndexed();
        }
    }
}

// S6.6a: the faces of the pool as one record of geometry entries (kind 32): the patch's vertex buffer is read by address, the region's render matrix is the entry's model matrix
void LLDrawPoolTerrain::drawPull()
{
    gRender.flush();
    static thread_local std::vector<LLVkBridge::GeoEntry> entries;
    static thread_local std::vector<F32> matrices;
    static thread_local std::vector<const LLMatrix4*> keys;
    static thread_local std::vector<U8> blob;
    entries.clear();
    matrices.clear();
    keys.clear();
    const glm::mat4 view = gRender.getModelviewMatrix();
    const glm::mat4 proj = gRender.getProjectionMatrix();
    // the matrices the shader would get from LLRender::syncMatrices() for a model matrix: modelview, modelview * projection, the normal matrix as three vec4 columns (matrix 0: the view alone)
    auto matrix = [&](const LLMatrix4* model) -> U32
    {
        for (size_t k = 0; k < keys.size(); ++k)
        {
            if (keys[k] == model)
            {
                return (U32)k;
            }
        }
        glm::mat4 mv = view;
        if (model)
        {
            mv = view * glm::make_mat4((const F32*)model->mMatrix);
        }
        const glm::mat4 mvp = proj * mv;
        const glm::mat4 n = glm::transpose(glm::inverse(mv));
        const size_t at = matrices.size();
        matrices.resize(at + 44);
        memcpy(&matrices[at], glm::value_ptr(mv), 64);
        memcpy(&matrices[at + 16], glm::value_ptr(mvp), 64);
        for (int col = 0; col < 3; ++col)
        {
            F32* d = &matrices[at + 32 + 4 * col];
            d[0] = n[col][0];
            d[1] = n[col][1];
            d[2] = n[col][2];
            d[3] = 0.f;
        }
        keys.push_back(model);
        return (U32)(at / 44);
    };
    matrix(nullptr);
    static U32 not_drawn = 0;
    for (LLFace* facep : mDrawFace)
    {
        const LLVertexBuffer* vb = facep->getVertexBuffer();
        if (!facep->getIndicesCount())
        {
            continue;
        }
        if (!vb || !vb->getVertexBufferId() || !vb->getIndexBufferId() || vb->getIndicesType() != ComponentType::U16 || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX)
            || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0 || !vb->hasDataType(LLVertexBuffer::TYPE_NORMAL) || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD1))
        {
            if (not_drawn++ < 10)
            {
                LL_WARNS("VkScene") << "a terrain face its buffer cannot give to the packet path is not drawn (buffer " << (void*)vb << ")" << LL_ENDL;
            }
            continue;
        }
        LLVkBridge::GeoEntry e = {};
        e.mVertexBuffer = vb->getVertexBufferId();
        e.mIndexBuffer = vb->getIndexBufferId();
        e.mFirstIndex = facep->getIndicesStart();
        e.mIndexCount = facep->getIndicesCount();
        e.mNormalOffset = vb->getOffset(LLVertexBuffer::TYPE_NORMAL);
        e.mTangentOffset = vb->hasDataType(LLVertexBuffer::TYPE_TANGENT) ? vb->getOffset(LLVertexBuffer::TYPE_TANGENT) : 0;
        e.mTexcoord1Offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD1);
        e.mMatrix = matrix(&facep->getDrawable()->getRegion()->mRenderMatrix);
        entries.push_back(e);
    }
    if (entries.empty())
    {
        return;
    }
    const size_t entries_bytes = entries.size() * sizeof(LLVkBridge::GeoEntry), matrix_bytes = matrices.size() * sizeof(F32);
    blob.resize(entries_bytes + matrix_bytes);
    memcpy(blob.data(), entries.data(), entries_bytes);
    memcpy(blob.data() + entries_bytes, matrices.data(), matrix_bytes);
    LLVkBridge::ShadowPacketsHeader header;
    header.mKind = 32;
    header.mEntries = (U32)entries.size();
    header.mMatrices = (U32)(matrices.size() / 44);
    LLVkBridge::shadowPackets(header, blob.data(), blob.size());
}

void LLDrawPoolTerrain::renderFullShader()
{
    const bool use_local_materials = gLocalTerrainMaterials.makeMaterialsReady(true, false);
    // Hack! Get the region that this draw pool is rendering from!
    LLViewerRegion *regionp = mDrawFace[0]->getDrawable()->getVObj()->getRegion();
    LLVLComposition *compp = regionp->getComposition();
    const bool use_textures = !use_local_materials && (compp->getMaterialType() == LLTerrainMaterials::Type::TEXTURE);

    if (use_textures)
    {
        // Use textures
        sPull = pull_wanted && gTerrainPullReady;
        sShader = sPull ? &gDeferredTerrainPullProgram : &gDeferredTerrainProgram;
        sShader->bind();
        renderFullShaderTextures();
    }
    else
    {
        // Use materials
        U32 paint_type = use_local_materials ? gLocalTerrainMaterials.getPaintType() : compp->getPaintType();
        paint_type = llclamp(paint_type, 0, TERRAIN_PAINT_TYPE_COUNT);
        sPull = pull_wanted_pbr && gTerrainPbrPullReady;
        sShader = sPull ? &gDeferredPBRTerrainPullProgram[paint_type] : &gDeferredPBRTerrainProgram[paint_type];
        sShader->bind();
        renderFullShaderPBR(use_local_materials);
    }
}

void LLDrawPoolTerrain::renderFullShaderTextures()
{
    // Hack! Get the region that this draw pool is rendering from!
    LLViewerRegion *regionp = mDrawFace[0]->getDrawable()->getVObj()->getRegion();
    LLVLComposition *compp = regionp->getComposition();

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    LLViewerTexture *detail_texture0p = (LLPipeline::sRenderTextures) ? compp->mDetailTextures[0] : LLViewerFetchedTexture::sDefaultDiffuseImagep;
    LLViewerTexture *detail_texture1p = (LLPipeline::sRenderTextures) ? compp->mDetailTextures[1] : LLViewerFetchedTexture::sDefaultDiffuseImagep;
    LLViewerTexture *detail_texture2p = (LLPipeline::sRenderTextures) ? compp->mDetailTextures[2] : LLViewerFetchedTexture::sDefaultDiffuseImagep;
    LLViewerTexture *detail_texture3p = (LLPipeline::sRenderTextures) ? compp->mDetailTextures[3] : LLViewerFetchedTexture::sDefaultDiffuseImagep;
// [/SL:KB]
//  LLViewerTexture *detail_texture0p = compp->mDetailTextures[0];
//  LLViewerTexture *detail_texture1p = compp->mDetailTextures[1];
//  LLViewerTexture *detail_texture2p = compp->mDetailTextures[2];
//  LLViewerTexture *detail_texture3p = compp->mDetailTextures[3];

    LLVector3d region_origin_global = gAgent.getRegion()->getOriginGlobal();
    F32 offset_x = (F32)fmod(region_origin_global.mdV[VX], 1.0/(F64)sDetailScale)*sDetailScale;
    F32 offset_y = (F32)fmod(region_origin_global.mdV[VY], 1.0/(F64)sDetailScale)*sDetailScale;

    LLVector4 tp0, tp1;

    tp0.setVec(sDetailScale, 0.0f, 0.0f, offset_x);
    tp1.setVec(0.0f, sDetailScale, 0.0f, offset_y);

    //
    // detail texture 0
    //
    S32 detail0 = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0);
    gRender.getTexUnit(detail0)->bind(detail_texture0p);
    gRender.getTexUnit(detail0)->setTextureAddressMode(LLTexUnit::TAM_WRAP);

    LLShaderProgram* shader = LLShaderProgram::sCurBoundShaderPtr;
    llassert(shader);

    shader->uniform4fv(LLShaderMgr::OBJECT_PLANE_S, 1, tp0.mV);
    shader->uniform4fv(LLShaderMgr::OBJECT_PLANE_T, 1, tp1.mV);

    LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();

    //
    // detail texture 1
    //
    S32 detail1 = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL1);
    gRender.getTexUnit(detail1)->bind(detail_texture1p);
    gRender.getTexUnit(detail1)->setTextureAddressMode(LLTexUnit::TAM_WRAP);

    // detail texture 2
    //
    S32 detail2 = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL2);
    gRender.getTexUnit(detail2)->bind(detail_texture2p);
    gRender.getTexUnit(detail2)->setTextureAddressMode(LLTexUnit::TAM_WRAP);


    // detail texture 3
    //
    S32 detail3 = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL3);
    gRender.getTexUnit(detail3)->bind(detail_texture3p);
    gRender.getTexUnit(detail3)->setTextureAddressMode(LLTexUnit::TAM_WRAP);

    //
    // Alpha Ramp
    //
    S32 alpha_ramp = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_ALPHARAMP);
    gRender.getTexUnit(alpha_ramp)->bind(m2DAlphaRampImagep);
    gRender.getTexUnit(alpha_ramp)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);

    // GL_BLEND disabled by default
    if (sPull)
    {
        drawPull();
    }
    else
    {
        drawLoop();
    }

    // Disable multitexture
    sShader->disableTexture(LLViewerShaderMgr::TERRAIN_ALPHARAMP);
    sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0);
    sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL1);
    sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL2);
    sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL3);

    gRender.getTexUnit(alpha_ramp)->bindWhite();
    gRender.getTexUnit(alpha_ramp)->clear();

    gRender.getTexUnit(detail3)->bindWhite();
    gRender.getTexUnit(detail3)->clear();

    gRender.getTexUnit(detail2)->bindWhite();
    gRender.getTexUnit(detail2)->clear();

    gRender.getTexUnit(detail1)->bindWhite();
    gRender.getTexUnit(detail1)->clear();

    //----------------------------------------------------------------------------
    // Restore Texture Unit 0 defaults

    gRender.getTexUnit(detail0)->bindWhite();
}

// *TODO: Investigate use of bindFast for PBR terrain textures
void LLDrawPoolTerrain::renderFullShaderPBR(bool use_local_materials)
{
    // Hack! Get the region that this draw pool is rendering from!
    LLViewerRegion *regionp = mDrawFace[0]->getDrawable()->getVObj()->getRegion();
    LLVLComposition *compp = regionp->getComposition();
    LLPointer<LLFetchedGLTFMaterial> (*fetched_materials)[LLVLComposition::ASSET_COUNT] = &compp->mDetailRenderMaterials;

    constexpr U32 terrain_material_count = LLVLComposition::ASSET_COUNT;
#ifdef SHOW_ASSERT
    constexpr U32 shader_material_count = 1 + LLViewerShaderMgr::TERRAIN_DETAIL3_BASE_COLOR - LLViewerShaderMgr::TERRAIN_DETAIL0_BASE_COLOR;
    llassert(shader_material_count == terrain_material_count);
#endif

    if (use_local_materials)
    {
        // Override region terrain with the global local override terrain
        fetched_materials = &gLocalTerrainMaterials.mDetailRenderMaterials;
    }
    const LLGLTFMaterial* materials[terrain_material_count];
    for (U32 i = 0; i < terrain_material_count; ++i)
    {
        materials[i] = (*fetched_materials)[i].get();
        if (!materials[i]) { materials[i] = &LLGLTFMaterial::sDefault; }
    }

    U32 paint_type = use_local_materials ? gLocalTerrainMaterials.getPaintType() : compp->getPaintType();
    paint_type = llclamp(paint_type, 0, TERRAIN_PAINT_TYPE_COUNT);

    S32 detail_basecolor[terrain_material_count];
    S32 detail_normal[terrain_material_count];
    S32 detail_metalrough[terrain_material_count];
    S32 detail_emissive[terrain_material_count];

    for (U32 i = 0; i < terrain_material_count; ++i)
    {
        LLViewerTexture* detail_basecolor_texturep = nullptr;
        LLViewerTexture* detail_normal_texturep = nullptr;
        LLViewerTexture* detail_metalrough_texturep = nullptr;
        LLViewerTexture* detail_emissive_texturep = nullptr;

        const LLFetchedGLTFMaterial* fetched_material = (*fetched_materials)[i].get();
        if (fetched_material)
        {
            detail_basecolor_texturep = fetched_material->mBaseColorTexture;
            detail_normal_texturep = fetched_material->mNormalTexture;
            detail_metalrough_texturep = fetched_material->mMetallicRoughnessTexture;
            detail_emissive_texturep = fetched_material->mEmissiveTexture;
        }

        detail_basecolor[i] = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_BASE_COLOR + i);
        if (detail_basecolor_texturep)
        {
            gRender.getTexUnit(detail_basecolor[i])->bind(detail_basecolor_texturep);
        }
        else
        {
            gRender.getTexUnit(detail_basecolor[i])->bind(LLViewerFetchedTexture::sWhiteImagep);
        }
        gRender.getTexUnit(detail_basecolor[i])->setTextureAddressMode(LLTexUnit::TAM_WRAP);

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_NORMAL)
        {
            detail_normal[i] = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_NORMAL + i);
            if (detail_normal_texturep)
            {
                gRender.getTexUnit(detail_normal[i])->bind(detail_normal_texturep);
            }
            else
            {
                gRender.getTexUnit(detail_normal[i])->bind(LLViewerFetchedTexture::sFlatNormalImagep);
            }
            gRender.getTexUnit(detail_normal[i])->setTextureAddressMode(LLTexUnit::TAM_WRAP);
        }

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
        {
            detail_metalrough[i] = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_METALLIC_ROUGHNESS + i);
            if (detail_metalrough_texturep)
            {
                gRender.getTexUnit(detail_metalrough[i])->bind(detail_metalrough_texturep);
            }
            else
            {
                gRender.getTexUnit(detail_metalrough[i])->bind(LLViewerFetchedTexture::sWhiteImagep);
            }
            gRender.getTexUnit(detail_metalrough[i])->setTextureAddressMode(LLTexUnit::TAM_WRAP);
        }

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_EMISSIVE)
        {
            detail_emissive[i] = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_EMISSIVE + i);
            if (detail_emissive_texturep)
            {
                gRender.getTexUnit(detail_emissive[i])->bind(detail_emissive_texturep);
            }
            else
            {
                gRender.getTexUnit(detail_emissive[i])->bind(LLViewerFetchedTexture::sWhiteImagep);
            }
            gRender.getTexUnit(detail_emissive[i])->setTextureAddressMode(LLTexUnit::TAM_WRAP);
        }
    }

    LLShaderProgram* shader = LLShaderProgram::sCurBoundShaderPtr;
    llassert(shader);

    // Like for PBR materials, PBR terrain texture transforms are defined by
    // the KHR_texture_transform spec, but with the following notable
    // differences:
    //   1) The PBR UV origin is defined as the Southwest corner of the region,
    //      with positive U facing East and positive V facing South.
    //   2) There is an additional scaling factor RenderTerrainPBRScale. If
    //      we've done our math right, RenderTerrainPBRScale should not affect the
    //      overall behavior of KHR_texture_transform
    //   3) There is only one texture transform per material, whereas
    //      KHR_texture_transform supports one texture transform per texture info.
    //      i.e. this isn't fully compliant with KHR_texture_transform, but is
    //      compliant when all texture infos used by a material have the same
    //      texture transform.
    LLGLTFMaterial::TextureTransform::PackTight transforms_packed[terrain_material_count];
    for (U32 i = 0; i < terrain_material_count; ++i)
    {
        const LLFetchedGLTFMaterial* fetched_material = (*fetched_materials)[i].get();
        LLGLTFMaterial::TextureTransform transform;
        if (fetched_material)
        {
            transform = fetched_material->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
#ifdef SHOW_ASSERT
            // Assert condition where the contents of the texture transforms
            // differ per texture info - we currently don't support this case.
            for (U32 ti = 1; ti < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++ti)
            {
                llassert(fetched_material->mTextureTransform[0] == fetched_material->mTextureTransform[ti]);
            }
#endif
        }
        // *NOTE: Notice here we are combining the scale from
        // RenderTerrainPBRScale into the KHR_texture_transform. This only
        // works if the scale is uniform and no other transforms are
        // applied to the terrain UVs.
        transform.mScale.mV[VX] *= sPBRDetailScale;
        transform.mScale.mV[VY] *= sPBRDetailScale;

        transform.getPackedTight(transforms_packed[i]);
    }
    const U32 transform_param_count = LLGLTFMaterial::TextureTransform::PACK_TIGHT_SIZE * terrain_material_count;
    constexpr U32 vec4_size = 4;
    const U32 transform_vec4_count = (transform_param_count + (vec4_size - 1)) / vec4_size;
    llassert(transform_vec4_count == 5); // If false, need to update shader
    shader->uniform4fv(LLShaderMgr::TERRAIN_TEXTURE_TRANSFORMS, transform_vec4_count, (F32*)transforms_packed);

    LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();

    //
    // Alpha Ramp or paint map
    //
    S32 alpha_ramp = -1;
    S32 paint_map = -1;
    if (paint_type == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE)
    {
        alpha_ramp = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_ALPHARAMP);
        gRender.getTexUnit(alpha_ramp)->bind(m2DAlphaRampImagep);
        gRender.getTexUnit(alpha_ramp)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }
    else if (paint_type == TERRAIN_PAINT_TYPE_PBR_PAINTMAP)
    {
        paint_map = sShader->enableTexture(LLViewerShaderMgr::TERRAIN_PAINTMAP);
        LLViewerTexture* tex_paint_map = use_local_materials ? gLocalTerrainMaterials.getPaintMap() : compp->getPaintMap();
        // If no paintmap is available, fall back to rendering just material slot 1 (by binding the appropriate image)
        if (!tex_paint_map) { tex_paint_map = LLViewerTexture::sBlackImagep.get(); }
        // This is a paint map for four materials, but we save a channel by
        // storing the paintmap as the "difference" between slot 1 and the
        // other 3 slots.
        llassert(tex_paint_map->getComponents() == 3);
        gRender.getTexUnit(paint_map)->bind(tex_paint_map);
        gRender.getTexUnit(paint_map)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);

        shader->uniform1f(LLShaderMgr::REGION_SCALE, regionp->getWidth());
    }

    //
    // GLTF uniforms
    //

    LLColor4 base_color_factors[terrain_material_count];
    F32 metallic_factors[terrain_material_count];
    F32 roughness_factors[terrain_material_count];
    LLColor3 emissive_colors[terrain_material_count];
    F32 minimum_alphas[terrain_material_count];
    for (U32 i = 0; i < terrain_material_count; ++i)
    {
        const LLGLTFMaterial* material = materials[i];

        base_color_factors[i] = material->mBaseColor;
        metallic_factors[i] = material->mMetallicFactor;
        roughness_factors[i] = material->mRoughnessFactor;
        emissive_colors[i] = material->mEmissiveColor;
        // glTF 2.0 Specification 3.9.4. Alpha Coverage
        // mAlphaCutoff is only valid for LLGLTFMaterial::ALPHA_MODE_MASK
        // Use 0 here due to GLTF terrain blending (LLGLTFMaterial::bind uses
        // -1 for easier debugging)
        F32 min_alpha = -0.0f;
        if (material->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK)
        {
            // dividing the alpha cutoff by transparency here allows the shader to compare against
            // the alpha value of the texture without needing the transparency value
            min_alpha = material->mAlphaCutoff/material->mBaseColor.mV[3];
        }
        minimum_alphas[i] = min_alpha;
    }
    shader->uniform4fv(LLShaderMgr::TERRAIN_BASE_COLOR_FACTORS, terrain_material_count, (F32*)base_color_factors);
    if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
    {
        shader->uniform4f(LLShaderMgr::TERRAIN_METALLIC_FACTORS, metallic_factors[0], metallic_factors[1], metallic_factors[2], metallic_factors[3]);
        shader->uniform4f(LLShaderMgr::TERRAIN_ROUGHNESS_FACTORS, roughness_factors[0], roughness_factors[1], roughness_factors[2], roughness_factors[3]);
    }
    if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_EMISSIVE)
    {
        shader->uniform3fv(LLShaderMgr::TERRAIN_EMISSIVE_COLORS, terrain_material_count, (F32*)emissive_colors);
    }
    shader->uniform4f(LLShaderMgr::TERRAIN_MINIMUM_ALPHAS, minimum_alphas[0], minimum_alphas[1], minimum_alphas[2], minimum_alphas[3]);

    // GL_BLEND disabled by default
    if (sPull)
    {
        drawPull();
    }
    else
    {
        drawLoop();
    }

    // Disable multitexture

    if (paint_type == TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE)
    {
        sShader->disableTexture(LLViewerShaderMgr::TERRAIN_ALPHARAMP);

        gRender.getTexUnit(alpha_ramp)->bindWhite();
        gRender.getTexUnit(alpha_ramp)->clear();
    }
    else if (paint_type == TERRAIN_PAINT_TYPE_PBR_PAINTMAP)
    {
        sShader->disableTexture(LLViewerShaderMgr::TERRAIN_PAINTMAP);

        gRender.getTexUnit(paint_map)->bindWhite();
        gRender.getTexUnit(paint_map)->clear();
    }

    for (U32 i = 0; i < terrain_material_count; ++i)
    {
        sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_BASE_COLOR + i);
        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_NORMAL)
        {
            sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_NORMAL + i);
        }
        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
        {
            sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_METALLIC_ROUGHNESS + i);
        }
        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_EMISSIVE)
        {
            sShader->disableTexture(LLViewerShaderMgr::TERRAIN_DETAIL0_EMISSIVE + i);
        }

        gRender.getTexUnit(detail_basecolor[i])->bindWhite();
        gRender.getTexUnit(detail_basecolor[i])->clear();

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_NORMAL)
        {
            gRender.getTexUnit(detail_normal[i])->bindWhite();
            gRender.getTexUnit(detail_normal[i])->clear();
        }

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS)
        {
            gRender.getTexUnit(detail_metalrough[i])->bindWhite();
            gRender.getTexUnit(detail_metalrough[i])->clear();
        }

        if (sPBRDetailMode >= TERRAIN_PBR_DETAIL_EMISSIVE)
        {
            gRender.getTexUnit(detail_emissive[i])->bindWhite();
            gRender.getTexUnit(detail_emissive[i])->clear();
        }
    }
}

void LLDrawPoolTerrain::hilightParcelOwners()
{
    { //use fullbright shader for highlighting
        LLShaderProgram* old_shader = sShader;
        sShader->unbind();
        sShader = &gDeferredHighlightProgram;
        sShader->bind();
        gRender.diffuseColor4f(1, 1, 1, 1);
        LLRenderState::PolygonOffsetScope polyOffset(true);
        gRender.setPolygonOffset(-1.0f, -1.0f);
        renderOwnership();
        sShader = old_shader;
        sShader->bind();
    }

}

void LLDrawPoolTerrain::renderOwnership()
{
    LLRenderPresetPipelineAlpha gls_pipeline_alpha;

    llassert(!mDrawFace.empty());

    // Each terrain pool is associated with a single region.
    // We need to peek back into the viewer's data to find out
    // which ownership overlay texture to use.
    LLFace                  *facep              = mDrawFace[0];
    LLDrawable              *drawablep          = facep->getDrawable();
    const LLViewerObject    *objectp                = drawablep->getVObj();
    const LLVOSurfacePatch  *vo_surface_patchp  = (LLVOSurfacePatch *)objectp;
    LLSurfacePatch          *surface_patchp     = vo_surface_patchp->getPatch();
    LLSurface               *surfacep           = surface_patchp->getSurface();
    LLViewerRegion          *regionp            = surfacep->getRegion();
    LLViewerParcelOverlay   *overlayp           = regionp->getParcelOverlay();
    LLViewerTexture         *texturep           = overlayp->getTexture();

    gRender.getTexUnit(0)->bind(texturep);

    // *NOTE: Because the region is 256 meters wide, but has 257 pixels, the
    // texture coordinates for pixel 256x256 is not 1,1. This makes the
    // ownership map not line up with the selection. We address this with
    // a texture matrix multiply.
    LLRenderMatrix::push(LLRenderMatrix::Kind::Texture0);

    const F32 TEXTURE_FUDGE = 257.f / 256.f;
    LLRenderMatrix::scale(LLRenderMatrix::Kind::Texture0, TEXTURE_FUDGE, TEXTURE_FUDGE, 1.f);
    for (std::vector<LLFace*>::iterator iter = mDrawFace.begin();
         iter != mDrawFace.end(); iter++)
    {
        LLFace *facep = *iter;
        facep->renderIndexed();
    }

    LLRenderMatrix::pop(LLRenderMatrix::Kind::Texture0);
}


void LLDrawPoolTerrain::dirtyTextures(const std::set<LLViewerFetchedTexture*>& textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLViewerFetchedTexture* tex = LLViewerTextureManager::staticCastToFetchedTexture(mTexturep) ;
    if (tex && textures.find(tex) != textures.end())
    {
        for (std::vector<LLFace*>::iterator iter = mReferences.begin();
             iter != mReferences.end(); iter++)
        {
            LLFace *facep = *iter;
            gPipeline.markTextured(facep->getDrawable());
        }
    }
}

LLViewerTexture *LLDrawPoolTerrain::getTexture()
{
    return mTexturep;
}

LLViewerTexture *LLDrawPoolTerrain::getDebugTexture()
{
    return mTexturep;
}


LLColor3 LLDrawPoolTerrain::getDebugColor() const
{
    return LLColor3(0.f, 0.f, 1.f);
}
