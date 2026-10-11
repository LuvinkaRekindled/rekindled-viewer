/**
 * @file lldrawpool.cpp
 * @brief LLDrawPool class implementation
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

#include "lldrawpool.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "llgpuzonetimer.h"
#include "llviewercontrol.h"

#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolavatar.h"
#include "lldrawpoolbump.h"
#include "lldrawpoolmaterials.h"
#include "lldrawpoolpbropaque.h"
#include "lldrawpoolsimple.h"
#include "lldrawpoolsky.h"
#include "lldrawpooltree.h"
#include "lldrawpoolterrain.h"
#include "lldrawpoolwater.h"
#include "lldrawpoolwaterexclusion.h"
#include "llface.h"
#include "llviewerobjectlist.h" // For debug listing.
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "lldrawpoolwlsky.h"
#include "llglslshader.h"
#include "llglcommonfunc.h"
#include "llvoavatar.h"
#include "llviewershadermgr.h"
#include "llvkmirror.h"
#include "llasyncpool.h"
#include "glm/glm.hpp"
#include "glm/gtc/type_ptr.hpp"
#include <chrono>
#include <map>

S32 LLDrawPool::sNumDrawPools = 0;

//=============================
// Draw Pool Implementation
//=============================
LLDrawPool *LLDrawPool::createPool(const U32 type, LLViewerTexture *tex0)
{
    LLDrawPool *poolp = NULL;
    switch (type)
    {
    case POOL_SIMPLE:
        poolp = new LLDrawPoolSimple();
        break;
    case POOL_GRASS:
        poolp = new LLDrawPoolGrass();
        break;
    case POOL_ALPHA_MASK:
        poolp = new LLDrawPoolAlphaMask();
        break;
    case POOL_FULLBRIGHT_ALPHA_MASK:
        poolp = new LLDrawPoolFullbrightAlphaMask();
        break;
    case POOL_FULLBRIGHT:
        poolp = new LLDrawPoolFullbright();
        break;
    case POOL_GLOW:
        poolp = new LLDrawPoolGlow();
        break;
    case POOL_ALPHA_PRE_WATER:
        poolp = new LLDrawPoolAlpha(LLDrawPool::POOL_ALPHA_PRE_WATER);
        break;
    case POOL_ALPHA_POST_WATER:
        poolp = new LLDrawPoolAlpha(LLDrawPool::POOL_ALPHA_POST_WATER);
        break;
    case POOL_AVATAR:
    case POOL_CONTROL_AV:
        poolp = new LLDrawPoolAvatar(type);
        break;
    case POOL_TREE:
        poolp = new LLDrawPoolTree(tex0);
        break;
    case POOL_TERRAIN:
        poolp = new LLDrawPoolTerrain(tex0);
        break;
    case POOL_SKY:
        poolp = new LLDrawPoolSky();
        break;
    case POOL_VOIDWATER:
    case POOL_WATER:
        poolp = new LLDrawPoolWater();
        break;
    case POOL_BUMP:
        poolp = new LLDrawPoolBump();
        break;
    case POOL_MATERIALS:
        poolp = new LLDrawPoolMaterials();
        break;
    case POOL_WL_SKY:
        poolp = new LLDrawPoolWLSky();
        break;
    case POOL_GLTF_PBR:
        poolp = new LLDrawPoolGLTFPBR();
        break;
    case POOL_GLTF_PBR_ALPHA_MASK:
        poolp = new LLDrawPoolGLTFPBR(LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK);
        break;
    case POOL_WATEREXCLUSION:
        poolp = new LLDrawPoolWaterExclusion();
        break;
    default:
        LL_ERRS() << "Unknown draw pool type!" << LL_ENDL;
        return NULL;
    }

    llassert(poolp->mType == type);
    return poolp;
}

LLDrawPool::LLDrawPool(const U32 type)
{
    mType = type;
    sNumDrawPools++;
    mId = sNumDrawPools;
    mShaderLevel = 0;
    mSkipRender = false;
}

LLDrawPool::~LLDrawPool()
{

}

LLViewerTexture *LLDrawPool::getDebugTexture()
{
    return NULL;
}

//virtual
void LLDrawPool::beginRenderPass( S32 pass )
{
}

//virtual
S32  LLDrawPool::getNumPasses()
{
    return 1;
}

//virtual
void LLDrawPool::beginDeferredPass(S32 pass)
{

}

//virtual
void LLDrawPool::endDeferredPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumDeferredPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderDeferred(S32 pass)
{

}

//virtual
void LLDrawPool::beginPostDeferredPass(S32 pass)
{

}

//virtual
void LLDrawPool::endPostDeferredPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumPostDeferredPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderPostDeferred(S32 pass)
{

}

//virtual
void LLDrawPool::endRenderPass( S32 pass )
{
    //make sure channel 0 is active channel
    gGL.getTexUnit(0)->activate();
}

//virtual
void LLDrawPool::beginShadowPass(S32 pass)
{

}

//virtual
void LLDrawPool::endShadowPass(S32 pass)
{

}

//virtual
S32 LLDrawPool::getNumShadowPasses()
{
    return 0;
}

//virtual
void LLDrawPool::renderShadow(S32 pass)
{

}

//=============================
// Face Pool Implementation
//=============================
LLFacePool::LLFacePool(const U32 type)
: LLDrawPool(type)
{
    resetDrawOrders();
}

LLFacePool::~LLFacePool()
{
    destroy();
}

void LLFacePool::destroy()
{
    if (!mReferences.empty())
    {
        LL_INFOS() << mReferences.size() << " references left on deletion of draw pool!" << LL_ENDL;
    }
}

void LLFacePool::dirtyTextures(const std::set<LLViewerFetchedTexture*>& textures)
{
}

void LLFacePool::enqueue(LLFace* facep)
{
    mDrawFace.push_back(facep);
}

// virtual
bool LLFacePool::addFace(LLFace *facep)
{
    addFaceReference(facep);
    return true;
}

// virtual
bool LLFacePool::removeFace(LLFace *facep)
{
    removeFaceReference(facep);

    vector_replace_with_last(mDrawFace, facep);

    return true;
}

// Not absolutely sure if we should be resetting all of the chained pools as well - djs
void LLFacePool::resetDrawOrders()
{
    mDrawFace.resize(0);
}

LLViewerTexture *LLFacePool::getTexture()
{
    return NULL;
}

void LLFacePool::removeFaceReference(LLFace *facep)
{
    if (facep->getReferenceIndex() != -1)
    {
        if (facep->getReferenceIndex() != (S32)mReferences.size())
        {
            LLFace *back = mReferences.back();
            mReferences[facep->getReferenceIndex()] = back;
            back->setReferenceIndex(facep->getReferenceIndex());
        }
        mReferences.pop_back();
    }
    facep->setReferenceIndex(-1);
}

void LLFacePool::addFaceReference(LLFace *facep)
{
    if (-1 == facep->getReferenceIndex())
    {
        facep->setReferenceIndex(static_cast<S32>(mReferences.size()));
        mReferences.push_back(facep);
    }
}

void LLFacePool::pushFaceGeometry()
{
    for (LLFace* const& face : mDrawFace)
    {
        face->renderIndexed();
    }
}

bool LLFacePool::verify() const
{
    bool ok = true;

    for (std::vector<LLFace*>::const_iterator iter = mDrawFace.begin();
         iter != mDrawFace.end(); iter++)
    {
        const LLFace* facep = *iter;
        if (facep->getPool() != this)
        {
            LL_INFOS() << "Face in wrong pool!" << LL_ENDL;
            facep->printDebugInfo();
            ok = false;
        }
        else if (!facep->verify())
        {
            ok = false;
        }
    }

    return ok;
}

void LLFacePool::printDebugInfo() const
{
    LL_INFOS() << "Pool " << this << " Type: " << getType() << LL_ENDL;
}

bool LLFacePool::LLOverrideFaceColor::sOverrideFaceColor = false;

void LLFacePool::LLOverrideFaceColor::setColor(const LLColor4& color)
{
    gGL.diffuseColor4fv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(const LLColor4U& color)
{
    gGL.diffuseColor4ubv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(F32 r, F32 g, F32 b, F32 a)
{
    gGL.diffuseColor4f(r,g,b,a);
}


//=============================
// Render Pass Implementation
//=============================
LLRenderPass::LLRenderPass(const U32 type)
: LLDrawPool(type)
{

}

LLRenderPass::~LLRenderPass()
{

}

void LLRenderPass::renderGroup(LLSpatialGroup* group, U32 type, bool texture)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[type];

    for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
    {
        LLDrawInfo *pparams = *k;
        if (pparams)
        {
            pushBatch(*pparams, texture);
        }
    }
}

void LLRenderPass::renderRiggedGroup(LLSpatialGroup* group, U32 type, bool texture)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[type];
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
    {
        LLDrawInfo* pparams = *k;
        if (pparams)
        {
            if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                pushBatch(*pparams, texture);
            }
        }
    }
}

void LLRenderPass::pushBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    if (texture)
    {
        auto* begin = gPipeline.beginRenderMap(type);
        auto* end = gPipeline.endRenderMap(type);
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);

            pushBatch(*pparams, texture, batch_textures);
        }
    }
    else
    {
        pushUntexturedBatches(type);
    }
}

void LLRenderPass::pushUntexturedBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);

        pushUntexturedBatch(*pparams);
    }
}

void LLRenderPass::pushRiggedBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    if (texture)
    {
        const LLVOAvatar* lastAvatar = nullptr;
        U64 lastMeshId = 0;
        bool skipLastSkin = false;
        auto* begin = gPipeline.beginRenderMap(type);
        auto* end = gPipeline.endRenderMap(type);
        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);

            if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
            {
                pushBatch(*pparams, texture, batch_textures);
            }
        }
    }
    else
    {
        pushUntexturedRiggedBatches(type);
    }
}

void LLRenderPass::pushUntexturedRiggedBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);

        if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        {
            pushUntexturedBatch(*pparams);
        }
    }
}

void LLRenderPass::pushMaskBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);
        LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(pparams->mAlphaMaskCutoff);
        pushBatch(*pparams, texture, batch_textures);
    }
}

void LLRenderPass::pushRiggedMaskBatches(U32 type, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;

        LLCullResult::increment_iterator(i, end);

        llassert(pparams);

        LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(pparams->mAlphaMaskCutoff);

        if (uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        {
            pushBatch(*pparams, texture, batch_textures);
        }
    }
}

void LLRenderPass::applyModelMatrix(const LLDrawInfo& params)
{
    applyModelMatrix(params.mModelMatrix);
}

void LLRenderPass::applyModelMatrix(const LLMatrix4* model_matrix)
{
    if (model_matrix != gGLLastMatrix)
    {
        gGLLastMatrix = model_matrix;
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(gGLModelView);
        if (model_matrix)
        {
            gGL.multMatrix((GLfloat*) model_matrix->mMatrix);
        }
        gPipeline.mMatrixOpCount++;
    }
}

void LLRenderPass::pushBatch(LLDrawInfo& params, bool texture, bool batch_textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    llassert(texture);

    if (!params.mCount)
    {
        return;
    }

    if (LLPipeline::sShadowRender)
    {
        ++gShadowDrawStats.mTextured;
    }

    applyModelMatrix(params);

    bool tex_setup = false;

    {
        if (batch_textures && params.mTextureList.size() > 1)
        {
            for (U32 i = 0; i < params.mTextureList.size(); ++i)
            {
                if (params.mTextureList[i].notNull())
                {
                    gGL.getTexUnit(i)->bindFast(params.mTextureList[i]);
                }
            }
        }
        else
        { //not batching textures or batch has only 1 texture -- might need a texture matrix
            if (params.mTexture.notNull())
            {
                gGL.getTexUnit(0)->bindFast(params.mTexture);
                if (params.mTextureMatrix)
                {
                    tex_setup = true;
                    gGL.getTexUnit(0)->activate();
                    gGL.matrixMode(LLRender::MM_TEXTURE);
                    gGL.loadMatrix((GLfloat*) params.mTextureMatrix->mMatrix);
                    gPipeline.mTextureMatrixOps++;
                }
            }
            else
            {
                gGL.getTexUnit(0)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }
    }
    // <FS:Beq> FIRE-34518 bugsplat access violation - place guard on unchecked mVertexBuffer access
    if (params.mVertexBuffer == nullptr)
    {
        LL_WARNS() << "LLRenderPass::pushBatch: params.mVertexBuffer is nullptr. drawRange skipped." << LL_ENDL;
        return;
    }
    // </FS:Beq>
    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
    if (tex_setup)
    {
        gGL.matrixMode(LLRender::MM_TEXTURE0);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
}

LLShadowDrawStats gShadowDrawStats;
LLGeometryPacketStats gGeometryPacketStats;

// <FS> Render packets for the deferred geometry passes. Setting FSDeferredPackets, or FS_DEFERRED_PACKETS_AB=1 to alternate every 30 seconds (the log says which: tag ShadowPackets).
// static
bool LLRenderPass::deferredPacketsEnabled()
{
    static LLCachedControl<bool> setting(gSavedSettings, "FSDeferredPackets", false);
    static const bool alternate = getenv("FS_DEFERRED_PACKETS_AB") != nullptr;
    bool on = setting;
    if (alternate)
    {
        const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        on = ((seconds / 30) % 2) == 1;
        static int last = -1;
        if ((int)on != last)
        {
            last = (int)on;
            LL_INFOS("ShadowPackets") << "[phase] deferred packets now " << (on ? "ON" : "off") << LL_ENDL;
        }
    }
    return on && gDeferredDiffusePullProgram.mProgramObject != 0 && LLVkMirror::shadowAlphaPacketsAvailable();
}

// can this static draw be an entry of a geometry record? The pull shader reads 16 bit indices, position first, and the normal, texcoord 0 and colour arrays; the textures
// come from the bindless table: a texture matrix is not carried (such draws, and a batch of more than four textures, take the old path).
static bool geometry_packet_ok(const LLDrawInfo& p, bool batch_textures, const LLGeometryPassInfo* materials)
{
    const LLVertexBuffer* vb = p.mVertexBuffer.get();
    if (!p.mCount || p.mAvatar || p.mGLTFMaterial || !vb || !vb->getGLBuffer() || !vb->getGLIndices() || vb->getIndicesType() != GL_UNSIGNED_SHORT
        || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0
        || !vb->hasDataType(LLVertexBuffer::TYPE_NORMAL) || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) || !vb->hasDataType(LLVertexBuffer::TYPE_COLOR))
    {
        return false;
    }
    if (materials)
    {
        // the materials passes apply the texture matrix to every texcoord set, and need the maps their program samples
        if (p.mTextureMatrix)
        {
            return false;
        }
        if (materials->mNormalMap && (p.mNormalMap.isNull() || !vb->hasDataType(LLVertexBuffer::TYPE_TANGENT) || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD1)))
        {
            return false;
        }
        if (materials->mSpecularMap && (p.mSpecularMap.isNull() || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD2)))
        {
            return false;
        }
        return true;
    }
    if (batch_textures && p.mTextureList.size() > 1)
    {
        return p.mTextureList.size() <= 4;
    }
    return !(p.mTexture.notNull() && p.mTextureMatrix);
}

// <FS> A geometry packet build (see doc/vulkan/F2-start.md, "how parallel recording is made safe"): a pure function of a cull result, the camera matrices and what the draws point at. It can
// run on the async pool while the main thread does other work. What it must not do itself is left in the output for the main thread: the textures the entries name (marking a texture in
// use, regenerating a deleted one, its sampler options) are listed in mTextures in order of first use, and the entries carry the number in that list plus one (0 = none) until
// finishGeometryBuild() puts the names in.
struct LLGeometryBuild
{
    U32 mType = 0;
    bool mHasMaterials = false;
    LLGeometryPassInfo mInfo;
    LLCullResult* mCull = nullptr;
    unsigned mEpoch = 0;                // LLBuilderGuard::sEpoch when it was started
    F32 mView[16];                      // the matrices it was built for: the consumer checks them against the ones it has now
    F32 mProj[16];
    std::vector<LLVkMirror::GeoEntry> mEntries;
    std::vector<F32> mMatrices;
    std::vector<LLViewerTexture*> mTextures;
    std::vector<LLDrawInfo*> mFallback;
    LLAsyncPool::JobPtr mJob;
    bool mFinished = false;
};

static void build_geometry(LLGeometryBuild& out)
{
    const LLGeometryPassInfo* materials = out.mHasMaterials ? &out.mInfo : nullptr;
    const glm::mat4 view = glm::make_mat4(out.mView);
    const glm::mat4 proj = glm::make_mat4(out.mProj);
    std::vector<LLVkMirror::GeoEntry>& entries = out.mEntries;
    std::vector<F32>& matrices = out.mMatrices;
    std::unordered_map<const void*, U32> matrix_numbers;
    std::unordered_map<const LLViewerTexture*, U32> texture_numbers;
    entries.clear();
    matrices.clear();
    out.mTextures.clear();
    out.mFallback.clear();

    // the matrices the shader would get from LLRender::syncMatrices() for this model matrix: modelview, modelview * projection, normal matrix (inverse transpose)
    auto add_matrix = [&](const LLMatrix4* model) -> U32
    {
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
        for (int c = 0; c < 3; ++c)
        {
            F32* d = &matrices[at + 32 + 4 * c];
            d[0] = n[c][0];
            d[1] = n[c][1];
            d[2] = n[c][2];
            d[3] = 0.f;
        }
        return (U32)(at / 44);
    };
    add_matrix(nullptr);        // matrix 0: no model matrix

    // the number (plus one) of a texture in the list the main thread names later
    auto texture_ref = [&](LLViewerTexture* tex) -> U32
    {
        auto found = texture_numbers.find(tex);
        if (found != texture_numbers.end())
        {
            return found->second;
        }
        out.mTextures.push_back(tex);
        const U32 number = (U32)out.mTextures.size();
        texture_numbers.emplace(tex, number);
        return number;
    };

    auto begin = out.mCull->beginRenderMap(out.mType);
    auto end = out.mCull->endRenderMap(out.mType);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* params = *i;
        LLCullResult::increment_iterator(i, end);
        if (!params->mCount)
        {
            continue;       // nothing to draw (the old path returns)
        }
        if (!geometry_packet_ok(*params, materials == nullptr, materials))
        {
            out.mFallback.push_back(params);
            continue;
        }
        const LLVertexBuffer* vb = params->mVertexBuffer.get();
        LLVkMirror::GeoEntry e = {};
        e.mVertexBuffer = vb->getGLBuffer();
        e.mIndexBuffer = vb->getGLIndices();
        e.mFirstIndex = params->mOffset;
        e.mIndexCount = params->mCount;
        e.mNormalOffset = vb->getOffset(LLVertexBuffer::TYPE_NORMAL);
        e.mTexcoordOffset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
        e.mColorOffset = vb->getOffset(LLVertexBuffer::TYPE_COLOR);
        if (materials)
        {
            if (materials->mNormalMap)
            {
                e.mTangentOffset = vb->getOffset(LLVertexBuffer::TYPE_TANGENT);
                e.mTexcoord1Offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD1);
            }
            if (materials->mSpecularMap)
            {
                e.mTexcoord2Offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD2);
            }
        }
        if (params->mModelMatrix)
        {
            auto it = matrix_numbers.find(params->mModelMatrix);
            if (it == matrix_numbers.end())
            {
                it = matrix_numbers.emplace(params->mModelMatrix, add_matrix(params->mModelMatrix)).first;
            }
            e.mMatrix = it->second;
        }
        if (!materials && params->mTextureList.size() > 1)
        {
            for (size_t t = 0; t < params->mTextureList.size() && t < 4; ++t)
            {
                if (params->mTextureList[t].notNull())
                {
                    e.mTextures[t] = texture_ref(params->mTextureList[t].get());
                }
            }
        }
        else
        {
            e.mFlags |= LLVkMirror::GEO_ONE_TEXTURE;
            if (params->mTexture.notNull())
            {
                e.mTextures[0] = texture_ref(params->mTexture.get());
            }
            if (materials)
            {
                // the values the materials loop sets as uniforms before a draw
                if (materials->mNormalMap)
                {
                    e.mNormalMap = texture_ref(params->mNormalMap.get());
                }
                if (materials->mSpecularMap)
                {
                    e.mSpecularMap = texture_ref(params->mSpecularMap.get());
                }
                memcpy(e.mSpecular, params->mSpecColor.mV, sizeof(e.mSpecular));
                e.mEnvIntensity = params->mEnvIntensity;
                e.mCutoff = params->mAlphaMaskCutoff;
                e.mBrightness = params->mFullbright ? 1.f : 0.f;
            }
        }
        entries.push_back(e);
    }
}

// main thread: what the old path does for a texture at its bind (marks it in use, regenerates a deleted one, applies its filtering and wrap options: the worker reads those),
// in the order the old loop met them, and the names go into the entries
static void finish_geometry_build(LLGeometryBuild& b)
{
    if (b.mFinished)
    {
        return;
    }
    std::vector<U32> names(b.mTextures.size());
    for (size_t k = 0; k < b.mTextures.size(); ++k)
    {
        names[k] = gGL.getTexUnit(0)->prepareFast(b.mTextures[k]);
        ++gGeometryPacketStats.mTextures;
    }
    auto name_of = [&names](U32 number) -> U32 { return number ? names[number - 1] : 0u; };
    for (LLVkMirror::GeoEntry& e : b.mEntries)
    {
        for (U32& t : e.mTextures)
        {
            t = name_of(t);
        }
        e.mNormalMap = name_of(e.mNormalMap);
        e.mSpecularMap = name_of(e.mSpecularMap);
    }
    b.mFinished = true;
}

static bool geometry_build_verify()
{
    static const bool on = getenv("LL_VK_BUILDER_VERIFY") != nullptr;
    return on;
}

// the builds of this frame's deferred geometry passes (kickGeometryBuild .. finishGeometryBuilds), by pass type and the maps its program samples
// keyed by the cull result as well as the pass: an impostor drawn in the middle of a frame is a second renderGeomDeferred with a cull result of its own and asks for the same passes, and
// must not touch the main view's builds, which are still running on the async pool
static std::map<std::pair<const void*, U32>, std::shared_ptr<LLGeometryBuild>> sGeometryBuilds;

static U32 geometry_build_key(U32 type, const LLGeometryPassInfo* materials)
{
    return (type << 3) | (materials ? 1u : 0u) | ((materials && materials->mNormalMap) ? 2u : 0u) | ((materials && materials->mSpecularMap) ? 4u : 0u);
}

static void fill_geometry_build(LLGeometryBuild& b, U32 type, const LLGeometryPassInfo* materials, LLCullResult* cull)
{
    b.mType = type;
    b.mHasMaterials = materials != nullptr;
    if (materials)
    {
        b.mInfo = *materials;
    }
    b.mCull = cull;
    b.mEpoch = LLBuilderGuard::sEpoch.load();
    memcpy(b.mView, gGLModelView, sizeof(b.mView));
    memcpy(b.mProj, glm::value_ptr(gGL.getProjectionMatrix()), sizeof(b.mProj));
}

// static
void LLRenderPass::kickGeometryBuild(U32 type, const LLGeometryPassInfo* materials)
{
    LLCullResult* cull = gPipeline.getCull();
    if (!cull)
    {
        return;
    }
    const std::pair<const void*, U32> key((const void*)cull, geometry_build_key(type, materials));
    if (sGeometryBuilds.find(key) != sGeometryBuilds.end())
    {
        return;         // started earlier in the frame for this cull result (the early sort)
    }
    std::shared_ptr<LLGeometryBuild> build = std::make_shared<LLGeometryBuild>();
    fill_geometry_build(*build, type, materials, cull);
    // the job keeps the build alive itself (it is written by the pool thread until the job is done, whatever happens to the registry); the job lets go of it when it has run
    build->mJob = LLAsyncPool::shared().submit([build]() { build_geometry(*build); });
    sGeometryBuilds[key] = build;
}

// static
void LLRenderPass::finishGeometryBuilds(LLCullResult* cull)
{
    for (auto it = sGeometryBuilds.begin(); it != sGeometryBuilds.end(); )
    {
        if (cull && it->first.first != (const void*)cull)
        {
            ++it;       // another view's (an impostor drawn in between): its own pass takes it
            continue;
        }
        if (it->second->mJob)
        {
            it->second->mJob->wait();        // they read the cull result, which is cleared at the start of the next frame
        }
        it = sGeometryBuilds.erase(it);
    }
}

// static
void LLRenderPass::pushGeometryPackets(U32 type, std::vector<LLDrawInfo*>& fallback, const LLGeometryPassInfo* materials)
{
    LL_CPU_ZONE("packets: geometry (build + record)");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLCullResult* cull = gPipeline.getCull();

    // the build started for this pass at the top of the frame, if it is still the right one (same cull result, same matrices: anything else is built here, now)
    std::shared_ptr<LLGeometryBuild> build;
    auto found = sGeometryBuilds.find(std::make_pair((const void*)cull, geometry_build_key(type, materials)));
    if (found != sGeometryBuilds.end())
    {
        build = found->second;
        sGeometryBuilds.erase(found);
        {
            LL_CPU_ZONE("packets: wait for the geometry builder");
            build->mJob->wait();
        }
        if (build->mCull != cull || build->mEpoch != LLBuilderGuard::sEpoch.load() || memcmp(build->mView, gGLModelView, sizeof(build->mView)) != 0
            || memcmp(build->mProj, glm::value_ptr(gGL.getProjectionMatrix()), sizeof(build->mProj)) != 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("VkBuilder") << "a geometry build was made for other matrices or another cull result: built again" << LL_ENDL;
            }
            build.reset();
        }
    }
    if (!build)
    {
        build = std::make_shared<LLGeometryBuild>();
        fill_geometry_build(*build, type, materials, cull);
        build_geometry(*build);
    }
    else if (geometry_build_verify())
    {
        LLGeometryBuild again;
        fill_geometry_build(again, type, materials, cull);
        build_geometry(again);
        static std::atomic<U32> checked{0}, bad{0};
        ++checked;
        const bool same = again.mEntries.size() == build->mEntries.size() && again.mMatrices == build->mMatrices && again.mTextures == build->mTextures && again.mFallback == build->mFallback
            && (again.mEntries.empty() || memcmp(again.mEntries.data(), build->mEntries.data(), again.mEntries.size() * sizeof(again.mEntries[0])) == 0);
        if (!same && bad++ < 8)
        {
            LL_WARNS("VkBuilder") << "builder verify: the geometry packets of pass " << type << " built on the pool differ from the ones built now (" << build->mEntries.size() << " / "
                << again.mEntries.size() << " entries, " << build->mTextures.size() << " / " << again.mTextures.size() << " textures)" << LL_ENDL;
        }
        if (checked % 600 == 1)
        {
            LL_INFOS("VkBuilder") << "builder verify: " << checked << " geometry builds compared, " << bad << " differed" << LL_ENDL;
        }
    }

    finish_geometry_build(*build);
    fallback.insert(fallback.end(), build->mFallback.begin(), build->mFallback.end());
    const std::vector<LLVkMirror::GeoEntry>& entries = build->mEntries;
    const std::vector<F32>& matrices = build->mMatrices;
    if (!entries.empty())
    {
        LLVkMirror::ShadowPacketsHeader header;
        header.mKind = 3;
        header.mEntries = (U32)entries.size();
        header.mMatrices = (U32)(matrices.size() / 44);
        std::vector<U8> blob(entries.size() * sizeof(entries[0]) + matrices.size() * sizeof(F32));
        memcpy(blob.data(), entries.data(), entries.size() * sizeof(entries[0]));
        memcpy(blob.data() + entries.size() * sizeof(entries[0]), matrices.data(), matrices.size() * sizeof(F32));
        LLVkMirror::shadowPackets(header, blob.data(), blob.size());
        gGeometryPacketStats.mEntries += entries.size();
        gGeometryPacketStats.mMatrices += header.mMatrices;
        ++gGeometryPacketStats.mRecords;
    }
    gGeometryPacketStats.mFallback += fallback.size();
}

void LLShadowDrawStats::endStaticPass()
{
    mStaticDistinctVB += mPassVBs.size();

    ++mTablePasses;
    mTableEntries += mTable.size();
    mTableMax = std::max<U64>(mTableMax, mTable.size());
    std::unordered_set<U64> pairs;
    for (const Caster& c : mTable)
    {
        const LLVertexBuffer* vb = c.mVB;
        if (!vb->getGLBuffer() || !vb->getGLIndices())
        {
            ++mBadNames;
        }
        pairs.insert(((U64)vb->getGLBuffer() << 32) | vb->getGLIndices());
        if (vb->getIndicesType() != GL_UNSIGNED_SHORT)
        {
            ++mWideIndices;
        }
        if (!vb->hasDataType(LLVertexBuffer::TYPE_VERTEX))
        {
            ++mNoPosition;
        }
        else if (vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0)
        {
            ++mPositionNotFirst;
        }
        // the index range the draw reads must lie inside the index buffer, the vertex range inside the vertex buffer
        if ((U64)(c.mIndexOffset + c.mCount) * vb->getIndicesStride() > vb->getIndicesSize())
        {
            ++mBadRange;
        }
        if (c.mEnd >= vb->getNumVerts())
        {
            ++mBadIndexRange;
        }
        if (!c.mMatrix)
        {
            ++mNullMatrix;
        }
    }
    mDistinctPairs += pairs.size();
}

void LLRenderPass::pushUntexturedBatch(LLDrawInfo& params)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    if (!params.mCount)
    {
        return;
    }

    if (LLPipeline::sShadowRender)
    {
        LLShadowDrawStats& st = gShadowDrawStats;
        if (params.mAvatar)
        {
            ++st.mRiggedUntex;
        }
        else
        {
            ++st.mStaticUntex;
            if (st.mLastVB != params.mVertexBuffer.get())
            {
                ++st.mStaticVBChanges;
                st.mLastVB = params.mVertexBuffer.get();
            }
            if (st.mLastMatrix != params.mModelMatrix)
            {
                ++st.mStaticMatrixChanges;
                st.mLastMatrix = params.mModelMatrix;
            }
            st.mPassVBs.insert(params.mVertexBuffer.get());
            st.mTable.push_back({ params.mVertexBuffer.get(), params.mStart, params.mEnd, params.mCount, params.mOffset, params.mModelMatrix });
        }
    }

    applyModelMatrix(params);

    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
}

// static
bool LLRenderPass::uploadMatrixPalette(LLDrawInfo& params)
{
    // upload matrix palette to shader
    return uploadMatrixPalette(params.mAvatar, params.mSkinInfo);
}

//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) // <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (!avatar)
    {
        return false;
    }
    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());

    if (count == 0)
    {
        //skin info not loaded yet, don't render
        return false;
    }

    LLGLSLShader::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
        count,
        false,
        (GLfloat*)&(mpc.mGLMp[0]));

    return true;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    if (!avatar)
    {
        return false;
    }

    if (avatar == lastAvatar && skinInfo->mHash == lastMeshId)
    {
        return !skipLastSkin;
    }

    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());
    // skipLastSkin -> skin info not loaded yet, don't render
    skipLastSkin = !bool(count);
    lastAvatar = avatar;
    lastMeshId = skinInfo->mHash;

    if (!skipLastSkin)
    {
        LLGLSLShader::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
            count,
            false,
            (GLfloat*)&(mpc.mGLMp[0]));
    }

    return !skipLastSkin;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, const LLGLSLShader*& lastAvatarShader, bool& skipLastSkin)// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    if (!avatar)
    {
        return false;
    }

    if (avatar == lastAvatar && skinInfo->mHash == lastMeshId && lastAvatarShader == LLGLSLShader::sCurBoundShaderPtr)
    {
        return !skipLastSkin;
    }

    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());
    // skipLastSkin -> skin info not loaded yet, don't render
    skipLastSkin = !bool(count);
    lastAvatar = avatar;
    lastMeshId = skinInfo->mHash;
    lastAvatarShader = LLGLSLShader::sCurBoundShaderPtr;

    if (!skipLastSkin)
    {
        LLGLSLShader::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
            count,
            false,
            (GLfloat*)&(mpc.mGLMp[0]));
    }

    return !skipLastSkin;
}

void setup_texture_matrix(LLDrawInfo& params)
{
    if (params.mTextureMatrix)
    { //special case implementation of texture animation here because of special handling of textures for PBR batches
        gGL.getTexUnit(0)->activate();
        gGL.matrixMode(LLRender::MM_TEXTURE);
        gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
        gPipeline.mTextureMatrixOps++;
    }
}

void teardown_texture_matrix(LLDrawInfo& params)
{
    if (params.mTextureMatrix)
    {
        gGL.matrixMode(LLRender::MM_TEXTURE0);
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
}

void LLRenderPass::pushGLTFBatches(U32 type, bool textured)
{
    if (textured)
    {
        pushGLTFBatches(type);
    }
    else
    {
        pushUntexturedGLTFBatches(type);
    }
}

void LLRenderPass::pushGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushGLTFBatch(params);
    }
}

void LLRenderPass::pushUntexturedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushUntexturedGLTFBatch(params);
    }
}

// static
void LLRenderPass::pushGLTFBatch(LLDrawInfo& params)
{
    if (LLPipeline::sShadowRender)
    {
        ++gShadowDrawStats.mGLTF;
    }
    auto& mat = params.mGLTFMaterial;

    if (mat.notNull())
    {
        mat->bind(params.mTexture);
    }

    LLGLDisable cull_face(mat.notNull() && mat->mDoubleSided ? GL_CULL_FACE : 0);

    setup_texture_matrix(params);

    applyModelMatrix(params);

    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

    teardown_texture_matrix(params);
}

// static
void LLRenderPass::pushUntexturedGLTFBatch(LLDrawInfo& params)
{
    if (LLPipeline::sShadowRender)
    {
        ++gShadowDrawStats.mGLTF;
    }
    auto& mat = params.mGLTFMaterial;

    LLGLDisable cull_face(mat->mDoubleSided ? GL_CULL_FACE : 0);

    applyModelMatrix(params);

    params.mVertexBuffer->setBuffer();
    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
}

void LLRenderPass::pushRiggedGLTFBatches(U32 type, bool textured)
{
    if (textured)
    {
        pushRiggedGLTFBatches(type);
    }
    else
    {
        pushUntexturedRiggedGLTFBatches(type);
    }
}

void LLRenderPass::pushRiggedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushRiggedGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushRiggedGLTFBatch(params, lastAvatar, lastMeshId, skipLastSkin);
    }
}

void LLRenderPass::pushUntexturedRiggedGLTFBatches(U32 type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("pushRiggedGLTFBatch");
        LLDrawInfo& params = **i;
        LLCullResult::increment_iterator(i, end);

        pushUntexturedRiggedGLTFBatch(params, lastAvatar, lastMeshId, skipLastSkin);
    }
}


// static
void LLRenderPass::pushRiggedGLTFBatch(LLDrawInfo& params, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)
{
    if (uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
    {
        pushGLTFBatch(params);
    }
}

// static
void LLRenderPass::pushUntexturedRiggedGLTFBatch(LLDrawInfo& params, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)
{
    if (uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
    {
        pushUntexturedGLTFBatch(params);
    }
}