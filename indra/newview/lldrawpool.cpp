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
#include <atomic>

#include "lldrawpool.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "rkgpuzonetimer.h"
#include "llviewercontrol.h"

#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolavatar.h"
#include "lldrawpoolbump.h"
#include "llfetchedgltfmaterial.h"
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
#include "rkshaderprogram.h"
#include "rkrendercommonfunc.h"
#include "llvoavatar.h"
#include "llviewershadermgr.h"
#include "rkvkbridge.h"
#include "llviewerregion.h"
#include "rkasyncpool.h"
#include "glm/glm.hpp"
#include "glm/gtc/type_ptr.hpp"
#include <chrono>
#include <map>
#include <set>

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

// S6.6: entries made from faces or draw infos for the pull programs (the generic RK_VERTEX_PULL rewrite): the arrays the buffer has are named, the model matrices are numbered in a table (0: the view alone)
namespace
{
struct RKPullBatch
{
    std::vector<RKVkBridge::GeoEntry> mEntries;
    std::vector<F32> mMatrices;
    std::vector<const LLMatrix4*> mKeys;
    std::vector<U8> mBlob;
    glm::mat4 mView, mProj;
    U32 mRefused = 0;

    RKPullBatch()
    {
        mView = gRender.getModelviewMatrix();
        mProj = gRender.getProjectionMatrix();
        matrix(nullptr);
    }
    U32 matrix(const LLMatrix4* model)
    {
        for (size_t k = 0; k < mKeys.size(); ++k)
        {
            if (mKeys[k] == model)
            {
                return (U32)k;
            }
        }
        glm::mat4 mv = mView;
        if (model)
        {
            mv = mView * glm::make_mat4((const F32*)model->mMatrix);
        }
        const glm::mat4 mvp = mProj * mv;
        const glm::mat4 n = glm::transpose(glm::inverse(mv));
        const size_t at = mMatrices.size();
        mMatrices.resize(at + 44);
        memcpy(&mMatrices[at], glm::value_ptr(mv), 64);
        memcpy(&mMatrices[at + 16], glm::value_ptr(mvp), 64);
        for (int col = 0; col < 3; ++col)
        {
            F32* d = &mMatrices[at + 32 + 4 * col];
            d[0] = n[col][0];
            d[1] = n[col][1];
            d[2] = n[col][2];
            d[3] = 0.f;
        }
        mKeys.push_back(model);
        return (U32)(at / 44);
    }
    bool add(const LLVertexBuffer* vb, U32 first_index, U32 index_count, const LLMatrix4* model)
    {
        if (!index_count)
        {
            return true;
        }
        if (!vb || !vb->getVertexBufferId() || !vb->getIndexBufferId() || vb->getIndicesType() != ComponentType::U16 || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX)
            || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0)
        {
            ++mRefused;
            return false;
        }
        RKVkBridge::GeoEntry e = {};
        e.mVertexBuffer = vb->getVertexBufferId();
        e.mIndexBuffer = vb->getIndexBufferId();
        e.mFirstIndex = first_index;
        e.mIndexCount = index_count;
        e.mNormalOffset = vb->hasDataType(LLVertexBuffer::TYPE_NORMAL) ? vb->getOffset(LLVertexBuffer::TYPE_NORMAL) : 0;
        e.mTexcoordOffset = vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) ? vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0) : 0;
        e.mColorOffset = vb->hasDataType(LLVertexBuffer::TYPE_COLOR) ? vb->getOffset(LLVertexBuffer::TYPE_COLOR) : 0;
        e.mTangentOffset = vb->hasDataType(LLVertexBuffer::TYPE_TANGENT) ? vb->getOffset(LLVertexBuffer::TYPE_TANGENT) : 0;
        e.mTexcoord1Offset = vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD1) ? vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD1) : 0;
        e.mTexcoord2Offset = vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD2) ? vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD2) : 0;
        e.mWeightOffset = vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT) ? vb->getOffset(LLVertexBuffer::TYPE_WEIGHT) : 0;
        if (!e.mTangentOffset && vb->hasDataType(LLVertexBuffer::TYPE_CLOTHWEIGHT))
        {
            e.mTangentOffset = vb->getOffset(LLVertexBuffer::TYPE_CLOTHWEIGHT);        // (the clothing array travels in the tangent slot)
        }
        e.mMatrix = matrix(model);
        mEntries.push_back(e);
        return true;
    }
    void send()
    {
        if (mRefused)
        {
            static U32 said = 0;
            if (said++ < 10)
            {
                LL_WARNS("VkScene") << mRefused << " draws whose buffers the packet path cannot read are not drawn" << LL_ENDL;
            }
        }
        if (mEntries.empty())
        {
            return;
        }
        const size_t entries_bytes = mEntries.size() * sizeof(RKVkBridge::GeoEntry), matrix_bytes = mMatrices.size() * sizeof(F32);
        mBlob.resize(entries_bytes + matrix_bytes);
        memcpy(mBlob.data(), mEntries.data(), entries_bytes);
        memcpy(mBlob.data() + entries_bytes, mMatrices.data(), matrix_bytes);
        RKVkBridge::ShadowPacketsHeader header;
        header.mKind = 32;
        header.mEntries = (U32)mEntries.size();
        header.mMatrices = (U32)(mMatrices.size() / 44);
        RKVkBridge::shadowPackets(header, mBlob.data(), mBlob.size());
    }
};
}

void LLFacePool::pushFacesPull(bool region_matrix)
{
    gRender.flush();
    RKPullBatch batch;
    for (LLFace* const& face : mDrawFace)
    {
        batch.add(face->getVertexBuffer(), (U32)face->getIndicesStart(), face->getIndicesCount(),
                  region_matrix ? &face->getDrawable()->getRegion()->mRenderMatrix : nullptr);
    }
    batch.send();
}

// the Linden trees: one vertex buffer a face, drawn whole (the buffer's own index count, whatever the face's index range says) under the region's render matrix
void LLFacePool::pushWholeBufferFacesPull()
{
    gRender.flush();
    RKPullBatch batch;
    for (LLFace* const& face : mDrawFace)
    {
        const LLVertexBuffer* vb = face->getVertexBuffer();
        LLDrawable* drawable = face->getDrawable();
        LLViewerRegion* region = drawable ? drawable->getRegion() : nullptr;
        if (!vb || !region)
        {
            continue;
        }
        batch.add(vb, 0, vb->getNumIndices(), &region->mRenderMatrix);
    }
    batch.send();
}

// S6.6d: one range of a buffer (a body mesh of an avatar) as a record of one entry, drawn with the pull program bound now under the modelview matrix on the stack
void rk_pull_draw_range(const LLVertexBuffer* vb, U32 first_index, U32 index_count)
{
    // (the draw the old way starts with setBuffer(): it flushes a buffer that is still mapped, which a body mesh can be)
    const_cast<LLVertexBuffer*>(vb)->setBuffer();
    gRender.syncMatrices();         // (the program's matrix uniforms that are not in the record, projection_matrix of the avatar shaders, are written as a draw writes them)
    gRender.flush();
    RKPullBatch batch;
    batch.add(vb, first_index, index_count, nullptr);
    batch.send();
}

// <FS> K6: the terrain, tree and avatar body draws of a pass whose pull program is not ready are not drawn the old way any more (their classic loops are deleted); the draw is counted and the first
// one is logged, so a missing patch or mesh is not a silent one
void rk_pool_draw_dropped(const char* what, size_t count)
{
    static std::map<std::string, U64> totals;
    if (count == 0)
    {
        return;
    }
    U64& total = totals[what];
    if (total % 10000 == 0)
    {
        LL_WARNS("VkScene") << what << ": " << count << " draws are not drawn because their pull program is not ready (there is no old path any more); dropped so far " << total + count << LL_ENDL;
    }
    total += count;
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
    gRender.diffuseColor4fv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(const LLColor4U& color)
{
    gRender.diffuseColor4ubv(color.mV);
}

void LLFacePool::LLOverrideFaceColor::setColor(F32 r, F32 g, F32 b, F32 a)
{
    gRender.diffuseColor4f(r,g,b,a);
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

void LLRenderPass::applyModelMatrix(const RKDrawBatch& params)
{
    applyModelMatrix(params.mModelMatrix);
}

void LLRenderPass::applyModelMatrix(const LLMatrix4* model_matrix)
{
    if (model_matrix != gRenderLastMatrix)
    {
        gRenderLastMatrix = model_matrix;
        RKRenderMatrix::load(RKRenderMatrix::Kind::Modelview, gRenderModelView);
        if (model_matrix)
        {
            RKRenderMatrix::multiply(RKRenderMatrix::Kind::Modelview, (F32*) model_matrix->mMatrix);
        }
        gPipeline.mMatrixOpCount++;
    }
}

RKShadowDrawStats gShadowDrawStats;
RKGeometryPacketStats gGeometryPacketStats;

// <FS> The geometry records of the deferred passes are the only way these passes draw (the hardware floor: PLAN-twin-removal.md, section 8): the setting RKDeferredPackets and the A/B switch RK_DEFERRED_PACKETS_AB
// (twin era tools that turned the records off) are gone, and so are the checks that the pull programs exist (they are required: a program that cannot be made ends the shader load); what is left is the worker's packet path.
// static
bool LLRenderPass::deferredPacketsEnabled()
{
    return RKVkBridge::shadowAlphaPacketsAvailable();
}

// <FS> S4 A1: the rigged draws of the deferred diffuse / materials passes as records (the switches RK_VK_RIGGED_PACKETS and RK_VK_RIGGED_PACKETS_AB are gone with the other A/B tools: PLAN-twin-removal.md, section 8).
bool rk_rigged_packets_enabled()
{
    return LLRenderPass::deferredPacketsEnabled();
}

// S4 A2: the frame's shared palette list (see lldrawpool.h)
static constexpr U32 PALETTE_PAIR_CAP = 32768;      // palettes a frame's list can log (a crowd frame holds a few hundred)
static struct FramePalettes
{
    std::mutex mMutex;
    U64 mFrame = ~0ull;
    std::vector<F32> mData;
    size_t mSent = 0;
    std::unordered_map<const F32*, U32> mOffsets;       // by the palette cache's data pointer (stable for the frame)
    U64 mFlushedFloats = 0;                             // census: floats sent since the last log line
    U64 mRequests = 0, mAdded = 0;
    // <FS> PLAN-avatars.md rigged draws as rows, P1 (RK_VK_PALARENA: 1 default, 0 = the copies as before, 2 = also compares what the arena holds): the part of the list that fits is written straight into a palette
    // arena of the renderer (offsets 0 .. its capacity), named to the worker by a kind 39 record, no copy; what does not fit (or a frame with no arena free) goes into mData as before, at offsets after the capacity
    RKVkBridge::PaletteArena mArena;
    bool mArenaOn = false;
    std::atomic<U32> mArenaCursor{ 0 };                 // floats written (the palette jobs allocate with a compare and swap, the requests under the mutex)
    U32 mArenaSent = 0;                                 // floats named to the worker
    U64 mPreparedFrame = ~0ull;                         // the frame whose list the palette jobs were prepared for (the coming frame: gFrameCount + 1 during the idle)
    U64 mIdleRequests = 0;                              // requests that came in the idle phase while the coming frame's list was prepared (they are refused, not allowed to reset it)
    U32 mDataBase = 0;                                  // the list offset of mData[0]: the arena's capacity when an arena serves the frame, else 0
    U64 mArenaFrames = 0, mArenaFallbackFrames = 0, mArenaOverflows = 0, mArenaVerified = 0, mArenaWrong = 0;
    // <FS> P1 step 3 (palette ids, RK_VK_PALID: 1 default, 0 = offsets as before): every palette stamped into the frame's list is logged as a pair (its entry's id, its float offset); the pairs go to the worker (kind 40)
    // after the data they point into, the worker resolves each to an address once, and the records name a palette by its id
    U32 mPairs[2 * PALETTE_PAIR_CAP];
    std::atomic<U32> mPairCursor{ 0 };
    U32 mPairSent = 0;
} sFramePal;

static std::atomic<U64> sPairOverflows{ 0 };

static int palette_arena_mode()
{
    static const int mode = getenv("RK_VK_PALARENA") ? atoi(getenv("RK_VK_PALARENA")) : 1;
    return mode;
}

// <FS> avatar relative skinning: the anchor of a frame (see lldrawpool.h). Decided by the first caller of the frame (the palette job of the frame before, a pool thread, or the render), then read back from a ring
namespace
{
std::mutex sRigAnchorMutex;
struct RigAnchorSlot { U32 mFrame = ~0u; RKRigAnchor mValue; };
RigAnchorSlot sRigAnchorRing[8];
RKRigAnchor sRigAnchorCurrent;
}

RKRigAnchor rk_rig_anchor(U32 frame)
{
    std::lock_guard<std::mutex> lock(sRigAnchorMutex);
    RigAnchorSlot& slot = sRigAnchorRing[frame & 7u];
    if (slot.mFrame == frame)
    {
        return slot.mValue;
    }
    // (the camera is read from a pool thread too: the origin is a few floats and any value of a nearby frame does: the anchor only has to be near the avatars, and every palette says which one it used)
    const LLVector3 c = LLViewerCamera::getInstance()->getOrigin();
    const F32 reach = std::max(fabsf(c.mV[0]), std::max(fabsf(c.mV[1]), fabsf(c.mV[2])));
    RKRigAnchor next;
    const bool was = sRigAnchorCurrent.mActive;
    static const bool force = getenv("RK_VK_RIGFORCE") != nullptr;      // (a test switch: the anchor is always active, so a lap at ground level runs the relative palettes that are otherwise only made above 700 m)
    if (force || reach > (was ? 500.f : 700.f))
    {
        next.mActive = true;
        F32 away = 1.0e9f;
        if (was)
        {
            away = 0.f;
            for (int k = 0; k < 3; ++k)
            {
                away = std::max(away, fabsf(c.mV[k] - sRigAnchorCurrent.mA[k]));
            }
        }
        for (int k = 0; k < 3; ++k)
        {
            next.mA[k] = away > 96.f ? floorf(c.mV[k] / 64.f + 0.5f) * 64.f : sRigAnchorCurrent.mA[k];
        }
    }
    sRigAnchorCurrent = next;
    slot.mFrame = frame;
    slot.mValue = next;
    return next;
}

// <FS> P1 step 3: palette ids. An id is (generation << 24) | index, made for a palette entry the first time the entry is written into a frame's list and given back when the entry goes (the avatar's map
// destroys it). The generation (1..254) changes each time an index is reused, so an id that was given back never names the palette that took its index. Never 0, never ~0u.
namespace
{
struct PaletteIds
{
    std::mutex mMutex;
    std::vector<U8> mGen{ 0 };          // by index (index 0 is not used)
    std::vector<U32> mFree;
};
PaletteIds& palette_ids()
{
    static PaletteIds* ids = new PaletteIds();      // (not destroyed: entries are given back from avatar destructors, which may run after static destruction has begun)
    return *ids;
}
}

U32 rk_palette_id_alloc()
{
    PaletteIds& ids = palette_ids();
    std::lock_guard<std::mutex> lock(ids.mMutex);
    U32 index;
    if (!ids.mFree.empty())
    {
        index = ids.mFree.back();
        ids.mFree.pop_back();
    }
    else
    {
        if (ids.mGen.size() >= 0xFFFFFFu)
        {
            return 0;
        }
        index = (U32)ids.mGen.size();
        ids.mGen.push_back(1);
    }
    return ((U32)ids.mGen[index] << 24) | index;
}

void rk_palette_id_free(U32 id)
{
    if (!id)
    {
        return;
    }
    PaletteIds& ids = palette_ids();
    std::lock_guard<std::mutex> lock(ids.mMutex);
    const U32 index = id & 0xFFFFFFu;
    if (index == 0 || index >= ids.mGen.size())
    {
        return;
    }
    U8 gen = (U8)(ids.mGen[index] + 1);
    if (gen == 0 || gen >= 255)
    {
        gen = 1;
    }
    ids.mGen[index] = gen;
    ids.mFree.push_back(index);
}

// <FS> P3 census (RK_VK_RIGCENSUS=1): where the rigged draws of a frame go. 0-2 the G-buffer's per frame records (own avatar / other avatars / animesh), 3 left out of them (the table draws them), 4 not drawn
// (no skin / no palette), 5 fallback to the old path, 6 entries of the cascades' rigged shadow records, 7 builds of those records, 8 transient rigged alpha entries, 9 rigged draw infos in the render maps
// the cascade jobs push (read from the caster lists)
static std::atomic<U64> sRigCensus[12];
static std::atomic<U64> sRigReasonEntry[16], sRigReasonFallback[16];       // why the table did not take a rigged draw the per frame records built (rk_geometry_reject_reason, texture matrix not allowed as for table rows), and why a draw went to the old path
static std::atomic<U64> sRigWithModel{ 0 };
static std::atomic<U64> sStaticFallbackReason[16], sStaticFallbackType[64];       // J1 K6 S4: the non rigged draws of the per frame records that went to the old path, by reason (rk_geometry_reject_reason) and by pass type
static std::atomic<U64> sRigGltf[4];       // the rigged glTF draws that went to the old path: all / double sided / with a texture matrix / the glow pass without an emissive array
static bool rig_census_on()
{
    static const bool on = getenv("RK_VK_RIGCENSUS") != nullptr;       // (read once: the census is asked about for every rigged draw of every frame)
    return on;
}
void rk_rig_census_add(int what, U64 n)
{
    if (rig_census_on() && n)
    {
        sRigCensus[what].fetch_add(n, std::memory_order_relaxed);
    }
}
void rk_rig_census_log()
{
    static U64 last = 0;
    if (!rig_census_on() || gFrameCount < last + 1200)
    {
        return;
    }
    const double frames = (double)(gFrameCount - last);
    last = gFrameCount;
    double v[12];
    for (int k = 0; k < 12; ++k)
    {
        v[k] = (double)sRigCensus[k].exchange(0) / frames;
    }
    {
        std::string text;
        for (int k = 0; k < 16; ++k)
        {
            const U64 e = sRigReasonEntry[k].exchange(0), f = sRigReasonFallback[k].exchange(0);
            if (e || f)
            {
                text += " r" + std::to_string(k) + ": records " + std::to_string((double)e / frames) + " old path " + std::to_string((double)f / frames) + ";";
            }
        }
        LL_INFOS("RigCensus") << "rigged G-buffer draws a frame by reason the table did not take them (0 = it would, 12 = texture matrix, 2 avatar mismatch, 10 no skin)," << text << " with a model matrix " << (double)sRigWithModel.exchange(0) / frames
            << "; rigged glTF draws on the old path " << (double)sRigGltf[0].exchange(0) / frames << ": double sided " << (double)sRigGltf[1].exchange(0) / frames << ", texture matrix " << (double)sRigGltf[2].exchange(0) / frames
            << ", glow without emissive " << (double)sRigGltf[3].exchange(0) / frames << LL_ENDL;
    }
    {
        std::string text;
        for (int k = 0; k < 16; ++k)
        {
            const U64 f = sStaticFallbackReason[k].exchange(0);
            if (f)
            {
                text += " r" + std::to_string(k) + " " + std::to_string((double)f / frames) + ";";
            }
        }
        std::string types;
        for (int k = 0; k < 64; ++k)
        {
            const U64 f = sStaticFallbackType[k].exchange(0);
            if (f)
            {
                types += " type " + std::to_string(k) + " " + std::to_string((double)f / frames) + ";";
            }
        }
        LL_INFOS("RigCensus") << "non rigged draws a frame on the old path by reason (J1 K6 S4):" << text << " by pass type:" << types << LL_ENDL;
    }
    LL_INFOS("RigCensus") << "rigged draws per frame over " << (U64)frames << " frames: G-buffer records own " << v[0] << " others " << v[1] << " animesh " << v[2] << " | left out (table) " << v[3] << ", not drawn " << v[4]
        << ", old path " << v[5] << " | cascade shadow records: entries " << v[6] << " in " << v[7] << " builds | transient rigged alpha entries " << v[8] << LL_ENDL;
}

// the id of a palette entry (made on first use; two threads that race give one of the two back)
static U32 palette_id_of(std::atomic<U32>& cell)
{
    const U32 have = cell.load(std::memory_order_acquire);
    if (have)
    {
        return have;
    }
    const U32 fresh = rk_palette_id_alloc();
    if (!fresh)
    {
        return 0;
    }
    U32 expected = 0;
    if (cell.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
    {
        return fresh;
    }
    rk_palette_id_free(fresh);
    return expected;
}

// logs (id, offset) for the frame's list; false = the log is full (the palette is not named, the item is not drawn this frame, like any other failed request)
static bool palette_pair_push(U32 id, U32 offset)
{
    const U32 i = sFramePal.mPairCursor.fetch_add(1, std::memory_order_relaxed);
    if (i >= PALETTE_PAIR_CAP)
    {
        if (++sPairOverflows <= 5)
        {
            LL_WARNS("PalArena") << "the palette id log of a frame is full (" << PALETTE_PAIR_CAP << " palettes)" << LL_ENDL;
        }
        return false;
    }
    sFramePal.mPairs[2 * i] = id;
    sFramePal.mPairs[2 * i + 1] = offset;
    return true;
}

// the float offset a palette id was logged with in this frame's list (the diagnostics read palettes by it); ~0u = not found.
static U32 palette_real_offset(U32 value)
{
    const U32 n = std::min<U32>(sFramePal.mPairCursor.load(std::memory_order_acquire), PALETTE_PAIR_CAP);
    for (U32 i = n; i-- > 0; )
    {
        if (sFramePal.mPairs[2 * i] == value)
        {
            return sFramePal.mPairs[2 * i + 1];
        }
    }
    return ~0u;
}

U64 rk_frame_palette_frame()
{
    return gFrameCount;
}

static std::atomic<U64> sArenaJobWrites{ 0 };       // palettes the palette jobs wrote into an arena (census)

// <FS> P1: the list of a frame starts here (under the mutex): the vectors are emptied, an arena is taken for the frame when one is free
static void palette_frame_begin_locked(U64 frame)
{
    sFramePal.mFrame = frame;
    sFramePal.mData.clear();
    sFramePal.mOffsets.clear();
    sFramePal.mSent = 0;
    sFramePal.mArenaCursor.store(0, std::memory_order_relaxed);
    sFramePal.mArenaSent = 0;
    sFramePal.mPairCursor.store(0, std::memory_order_relaxed);
    sFramePal.mPairSent = 0;
    sFramePal.mArenaOn = palette_arena_mode() != 0 && RKVkBridge::paletteArenaAcquire((U32)frame, sFramePal.mArena);
    sFramePal.mDataBase = sFramePal.mArenaOn ? sFramePal.mArena.mFloats : 0;
    ++(sFramePal.mArenaOn ? sFramePal.mArenaFrames : sFramePal.mArenaFallbackFrames);
    if (palette_arena_mode() != 0 && ((sFramePal.mArenaFrames + sFramePal.mArenaFallbackFrames) % 1200) == 0)
    {
        LL_INFOS("PalArena") << "palette arenas: frames served from an arena " << sFramePal.mArenaFrames << ", frames with none free (copied as before) " << sFramePal.mArenaFallbackFrames << ", palettes that did not fit "
            << sFramePal.mArenaOverflows << ", written by the palette jobs " << sArenaJobWrites.load() << ", requests refused in the idle phase " << sFramePal.mIdleRequests << LL_ENDL;
    }
}

// <FS> P1 step 2 (the palette jobs write into the arena): main thread, before the jobs of the idle launch: the list of the coming frame (gFrameCount + 1) begins and takes its arena
void rk_frame_palette_prepare(U32 frame)
{
    std::lock_guard<std::mutex> lock(sFramePal.mMutex);
    if (sFramePal.mFrame == frame)
    {
        return;
    }
    palette_frame_begin_locked(frame);
    sFramePal.mPreparedFrame = frame;
}

// a palette job (any thread, no lock): the floats go into the prepared frame's arena at a slice of their own; false = not prepared for this frame, no arena, or it does not fit (the render's request appends it as before)
// <FS> velocity fold (PLAN-velocity-fold-and-skin-once.md): every palette in the list is followed by the palette of the frame before (same size, same anchor: mPrevGLMp, equal to the current one when there is no history): the
// rigged G-buffer vertex shaders read both and write the motion of the surface. prev_floats null: the current palette is written twice.
// (D3: the one definition of the scale, the palette arithmetic on the GPU takes it from here too) 0 = no flag for this avatar: no DLSS, nearer than 48 m only
F32 rk_palette_moving_scale(const LLVector3& avatar_position)
{
    if (!LLPipeline::sDLSS || !gPipeline.mRT)
    {
        return 0.f;
    }
    const F32 distance = (avatar_position - LLViewerCamera::getInstance()->getOrigin()).length();
    if (distance > 48.f)
    {
        return 0.f;
    }
    const F32 focal_px = 0.5f * (F32)gPipeline.mRT->deferredScreen.getHeight() * LLViewerCamera::sJitteredProjection[1][1];
    return 1.5f * focal_px / llmax(distance, 0.5f);
}

F32 rk_palette_moving(const F32* cur, const F32* prev, U32 floats, const LLVector3& avatar_position)
{
    if (!prev || prev == cur)
    {
        return 0.f;
    }
    const F32 scale = rk_palette_moving_scale(avatar_position);
    if (scale <= 0.f)
    {
        return 0.f;
    }
    F32 biggest = 0.f;
    for (U32 k = 0; k < floats; ++k)
    {
        biggest = llmax(biggest, fabsf(cur[k] - prev[k]));
    }
    return biggest * scale >= 0.3f ? 1.f : 0.f;
}

bool rk_frame_palette_write(const F32* floats, const F32* prev_floats, F32 moving, U32 count, U32 frame, U32& offset, std::atomic<U32>& id_cell)
{
    if (!count || sFramePal.mPreparedFrame != frame || !sFramePal.mArenaOn)
    {
        return false;
    }
    U32 cur = sFramePal.mArenaCursor.load(std::memory_order_relaxed);
    do
    {
        if ((U64)cur + 2 * (U64)count > sFramePal.mArena.mFloats)
        {
            return false;
        }
    }
    while (!sFramePal.mArenaCursor.compare_exchange_weak(cur, cur + 2 * count, std::memory_order_acq_rel));
    memcpy(sFramePal.mArena.mPtr + cur, floats, (size_t)count * sizeof(F32));
    memcpy(sFramePal.mArena.mPtr + cur + count, prev_floats ? prev_floats : floats, (size_t)count * sizeof(F32));
    sFramePal.mArena.mPtr[cur + count - 8] = moving;        // (the tail entry's second row, x: see rk_palette_moving)
    {
        const U32 id = palette_id_of(id_cell);
        if (!id || !palette_pair_push(id, cur))
        {
            return false;
        }
    }
    ++sArenaJobWrites;
    offset = cur;
    return true;
}

// census (a rigged outfit that disappears for a frame now and then): why a palette request fails. An item without a palette is not drawn (the old path draws nothing for it either)
static std::atomic<U64> sPalFailNoAvatar{ 0 }, sPalFailEmpty{ 0 }, sPalFailShort{ 0 }, sPalRequests{ 0 }, sPalFastRequests{ 0 };
bool rk_frame_palette(LLVOAvatar* avatar, const LLMeshSkinInfo* skin, U32& offset, U32& joints)
{
    ++sPalRequests;
    if (!avatar || !skin)
    {
        const U64 n = ++sPalFailNoAvatar;
        if (n <= 20 || (n % 200) == 0)
        {
            LL_INFOS("VkScene") << "[palette] request failed, frame " << gFrameCount << ": " << (avatar ? "an avatar without a skin" : "no avatar") << " (" << n << " so far)" << LL_ENDL;
        }
        return false;
    }
    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skin);       // (one at a time inside, and the entry is not written again in this frame)
    joints = (U32)mpc.mMatrixPalette.size();
    if (joints == 0 || mpc.mGLMp.size() < (size_t)(joints + 1) * 12)       // (the palette has one entry more than its mesh: the tail with the anchor, see rk_rig_anchor)
    {
        const U64 n = joints == 0 ? ++sPalFailEmpty : ++sPalFailShort;
        if (n <= 20 || (n % 200) == 0)
        {
            LL_INFOS("VkScene") << "[palette] request failed, frame " << gFrameCount << ": avatar " << (const void*)avatar << " skin " << (const void*)skin << " has " << joints << " joints and " << mpc.mGLMp.size() << " floats (" << (joints == 0 ? "no joints" : "matrices short") << ", " << n << " so far)" << LL_ENDL;
        }
        return false;
    }
    // <FS> PLAN-avatars.md stage B3b: the palette entry remembers where it sits in this frame's list (frame << 32 | offset): a request after the first one reads that and takes no lock
    const U64 slot = mpc.mListSlot.load(std::memory_order_acquire);
    if ((U32)(slot >> 32) == (U32)gFrameCount)
    {
        offset = (U32)slot;
        sPalFastRequests.fetch_add(1, std::memory_order_relaxed);
        static const bool verify = getenv("RK_VK_PALLIST") && atoi(getenv("RK_VK_PALLIST")) == 2;
        if (verify)
        {
            // compare mode: what the list holds at that offset is this palette, float for float
            static std::atomic<U64> checked{ 0 }, wrong{ 0 };
            std::lock_guard<std::mutex> check_lock(sFramePal.mMutex);
            const size_t n = (size_t)(joints + 1) * 12;
            const bool ok = sFramePal.mFrame == gFrameCount
                && ((sFramePal.mArenaOn && (size_t)offset + n <= sFramePal.mArenaCursor.load() && memcmp(sFramePal.mArena.mPtr + offset, mpc.mGLMp.data(), n * sizeof(F32)) == 0)
                    || (offset >= sFramePal.mDataBase && (size_t)(offset - sFramePal.mDataBase) + n <= sFramePal.mData.size() && memcmp(sFramePal.mData.data() + (offset - sFramePal.mDataBase), mpc.mGLMp.data(), n * sizeof(F32)) == 0));
            ++checked;
            if (!ok && ++wrong <= 6)
            {
                LL_WARNS("VkScene") << "[palette] list slot does not hold the palette: frame " << gFrameCount << " offset " << offset << " joints " << joints << LL_ENDL;
            }
            if (checked % 200000 == 0)
            {
                LL_INFOS("VkScene") << "[palette] list slots checked " << checked.load() << ", wrong " << wrong.load() << LL_ENDL;
            }
        }
        offset = mpc.mPaletteId.load(std::memory_order_acquire);        // (a stamped entry has an id: it was logged when it was stamped)
        return true;
    }
    std::lock_guard<std::mutex> lock(sFramePal.mMutex);
    if (sFramePal.mFrame != gFrameCount)
    {
        if (sFramePal.mPreparedFrame == gFrameCount + 1)
        {
            // a request in the idle phase while the coming frame's list is prepared (the palette jobs have written into it): it may not reset that list, whose offsets the entries already carry
            const U64 n = ++sFramePal.mIdleRequests;
            if (n <= 5)
            {
                LL_WARNS("PalArena") << "a palette request came in the idle phase of frame " << gFrameCount << " while the list of the coming frame is prepared (refused, " << n << " so far)" << LL_ENDL;
            }
            return false;
        }
        palette_frame_begin_locked(gFrameCount);
    }
    ++sFramePal.mRequests;
    const U64 again = mpc.mListSlot.load(std::memory_order_acquire);        // (another thread may have added it while this one waited)
    if ((U32)(again >> 32) == (U32)gFrameCount)
    {
        offset = mpc.mPaletteId.load(std::memory_order_acquire);
        return true;
    }
    const size_t need = (size_t)(joints + 1) * 12;
    const F32* prev_data = mpc.mPrevGLMp.size() >= need ? mpc.mPrevGLMp.data() : mpc.mGLMp.data();       // (the previous palette follows the current one: see rk_frame_palette_write)
    const F32 moving = rk_palette_moving(mpc.mGLMp.data(), prev_data, (U32)need, avatar->getRenderPosition());
    U32 at = 0;
    bool in_arena = false;
    if (sFramePal.mArenaOn)
    {
        U32 cur = sFramePal.mArenaCursor.load(std::memory_order_relaxed);
        while ((size_t)cur + 2 * need <= sFramePal.mArena.mFloats)
        {
            if (sFramePal.mArenaCursor.compare_exchange_weak(cur, cur + 2 * (U32)need, std::memory_order_acq_rel))
            {
                at = cur;
                in_arena = true;
                break;
            }
        }
    }
    if (in_arena)
    {
        // (no read back of what was written: the arena is device visible upload memory, a CPU read of it is uncached and very slow. A first compare mode that did it cost a crowd 20 fps: the content was right, 287 k
        // palettes compared, 0 wrong)
        memcpy(sFramePal.mArena.mPtr + at, mpc.mGLMp.data(), need * sizeof(F32));
        memcpy(sFramePal.mArena.mPtr + at + need, prev_data, need * sizeof(F32));
        sFramePal.mArena.mPtr[at + need - 8] = moving;
    }
    else
    {
        sFramePal.mArenaOverflows += sFramePal.mArenaOn;
        at = sFramePal.mDataBase + (U32)sFramePal.mData.size();
        const size_t first = sFramePal.mData.size();
        sFramePal.mData.insert(sFramePal.mData.end(), mpc.mGLMp.begin(), mpc.mGLMp.begin() + need);
        sFramePal.mData.insert(sFramePal.mData.end(), prev_data, prev_data + need);
        sFramePal.mData[first + need - 8] = moving;
    }
    ++sFramePal.mAdded;
    const U32 id = palette_id_of(mpc.mPaletteId);
    if (!id || !palette_pair_push(id, at))
    {
        return false;
    }
    mpc.mListSlot.store(((U64)(U32)gFrameCount << 32) | at, std::memory_order_release);
    offset = id;
    return true;
}

bool rk_frame_palette_copy(U32 offset, U32 joints, std::vector<F32>& out)
{
    std::lock_guard<std::mutex> lock(sFramePal.mMutex);
    if (sFramePal.mFrame != gFrameCount)
    {
        return false;
    }
    offset = palette_real_offset(offset);
    if (offset == ~0u)
    {
        return false;
    }
    const size_t n = (size_t)(joints + 1) * 12;
    if (sFramePal.mArenaOn && (size_t)offset + n <= sFramePal.mArenaCursor.load())
    {
        out.assign(sFramePal.mArena.mPtr + offset, sFramePal.mArena.mPtr + offset + n);
        return true;
    }
    if (offset < sFramePal.mDataBase || (size_t)(offset - sFramePal.mDataBase) + n > sFramePal.mData.size())
    {
        return false;
    }
    out.assign(sFramePal.mData.begin() + (offset - sFramePal.mDataBase), sFramePal.mData.begin() + (offset - sFramePal.mDataBase) + n);
    return true;
}

bool rk_frame_palette_first_joint(U32 offset, F32* out12)
{
    std::lock_guard<std::mutex> lock(sFramePal.mMutex);
    if (sFramePal.mFrame != gFrameCount)
    {
        return false;
    }
    offset = palette_real_offset(offset);
    if (offset == ~0u)
    {
        return false;
    }
    if (sFramePal.mArenaOn && (size_t)offset + 12 <= sFramePal.mArenaCursor.load())
    {
        memcpy(out12, sFramePal.mArena.mPtr + offset, 12 * sizeof(F32));
        return true;
    }
    if (offset < sFramePal.mDataBase || (size_t)(offset - sFramePal.mDataBase) + 12 > sFramePal.mData.size())
    {
        return false;
    }
    memcpy(out12, sFramePal.mData.data() + (offset - sFramePal.mDataBase), 12 * sizeof(F32));
    return true;
}

void rk_frame_palette_flush()
{
    std::lock_guard<std::mutex> lock(sFramePal.mMutex);
    if (sFramePal.mFrame != gFrameCount)
    {
        return;
    }
    const U32 arena_used = sFramePal.mArenaCursor.load(std::memory_order_acquire);
    // <FS> P1 step 3: the palettes stamped since the last flush are named by id (kind 40), after the data they point into (the worker resolves each offset when it reads the record)
    auto send_pairs = [&]()
    {
        const U32 total = std::min<U32>(sFramePal.mPairCursor.load(std::memory_order_acquire), PALETTE_PAIR_CAP);
        if (total <= sFramePal.mPairSent)
        {
            return;
        }
        RKVkBridge::ShadowPacketsHeader pair_header;
        pair_header.mKind = 40;
        pair_header.mPass = (U32)gFrameCount;
        pair_header.mEntries = total - sFramePal.mPairSent;
        RKVkBridge::shadowPackets(pair_header, sFramePal.mPairs + 2 * sFramePal.mPairSent, (size_t)pair_header.mEntries * 2 * sizeof(U32));
        sFramePal.mPairSent = total;
    };
    if (sFramePal.mArenaOn && arena_used > sFramePal.mArenaSent)
    {
        // <FS> P1: the part of the list that was written into the arena is named, not sent (kind 39)
        RKVkBridge::ShadowPacketsHeader arena_header;
        arena_header.mKind = 39;
        arena_header.mPass = (U32)gFrameCount;
        arena_header.mEntries = sFramePal.mArena.mIndex;
        arena_header.mFragment = sFramePal.mArenaSent;
        arena_header.mPaletteFloats = arena_used - sFramePal.mArenaSent;
        static const U32 no_blob = 0;
        RKVkBridge::shadowPackets(arena_header, &no_blob, sizeof(no_blob));
        sFramePal.mFlushedFloats += arena_header.mPaletteFloats;
        sFramePal.mArenaSent = arena_used;
    }
    if (sFramePal.mSent >= sFramePal.mData.size())
    {
        send_pairs();
        return;
    }
    RKVkBridge::ShadowPacketsHeader header;
    header.mKind = 27;
    header.mPass = (U32)gFrameCount;
    header.mFragment = sFramePal.mDataBase + (U32)sFramePal.mSent;
    header.mPaletteFloats = (U32)(sFramePal.mData.size() - sFramePal.mSent);
    RKVkBridge::shadowPackets(header, sFramePal.mData.data() + sFramePal.mSent, (size_t)header.mPaletteFloats * sizeof(F32));
    sFramePal.mFlushedFloats += header.mPaletteFloats;
    sFramePal.mSent = sFramePal.mData.size();
    send_pairs();
}

// A1 census (log tag RiggedPackets, every 600 frames): what the rigged records carried and what they left to the old path
static struct RiggedStats
{
    U64 mTris = 0, mRuns = 0, mRecords = 0, mEntries = 0, mFallback = 0, mNotDrawn = 0, mPalettes = 0, mPaletteFloats = 0, mInRenderMap = 0, mUncounted = 0, mFrames = 0;
    U64 mLast = 0;
} sRigged;

// can this static draw be an entry of a geometry record? The pull shader reads 16 bit indices, position first, and the normal, texcoord 0 and colour arrays; the textures
// come from the bindless table: a texture matrix is not carried (such draws, and a batch of more than four textures, take the old path).
// S6 X1 / X2: the arrays a pass type needs besides position, texcoord 0, colour and indices: the fullbright passes have no use for normals (their pull program does not read them), the glow pass needs the
// emissive array instead
U32 rk_pass_arrays(U32 type)
{
    switch (type)
    {
    case LLRenderPass::PASS_FULLBRIGHT:
    case LLRenderPass::PASS_FULLBRIGHT_RIGGED:
    case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
    case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
    case LLRenderPass::PASS_INVISIBLE:         // C5: positions only (the colour program reads nothing else)
    case LLRenderPass::PASS_ALPHA_INVISIBLE:   // J1 K6 B1: the same for the invisible alpha draws (the highlight view)
        return 0;
    case LLRenderPass::PASS_GLOW:
    case LLRenderPass::PASS_GLOW_RIGGED:
        return RK_PASS_ARRAY_EMISSIVE;
    case LLRenderPass::PASS_BUMP:
    case LLRenderPass::PASS_BUMP_RIGGED:
        return RK_PASS_ARRAY_NORMALS | RK_PASS_ARRAY_BUMP;
    case LLRenderPass::PASS_POST_BUMP:
    case LLRenderPass::PASS_POST_BUMP_RIGGED:
        return RK_PASS_ARRAY_EMBOSS;       // (J1 K4: positions, both texture coordinate sets and the bump image: no normals)
    default:
        return RK_PASS_ARRAY_NORMALS;
    }
}

static bool geometry_packet_ok(const RKDrawBatch& p, bool batch_textures, const RKGeometryPassInfo* materials, bool rigged = false, bool texture_matrix = false, U32 arrays = RK_PASS_ARRAY_NORMALS)
{
    const LLVertexBuffer* vb = p.mVertexBuffer.get();
    if (!p.mCount || (p.mAvatar != nullptr) != rigged || p.mGLTFMaterial || !vb || !vb->getVertexBufferId() || !vb->getIndexBufferId() || vb->getIndicesType() != ComponentType::U16
        || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0
        || ((arrays & RK_PASS_ARRAY_NORMALS) && !vb->hasDataType(LLVertexBuffer::TYPE_NORMAL)) || ((arrays & RK_PASS_ARRAY_EMISSIVE) && !vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
        || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) || !vb->hasDataType(LLVertexBuffer::TYPE_COLOR))
    {
        return false;
    }
    if ((arrays & RK_PASS_ARRAY_BUMP) && (!vb->hasDataType(LLVertexBuffer::TYPE_TANGENT) || p.mTextureList.size() > 1))
    {
        return false;       // (J1 K4: the bump pass needs the tangents and draws one texture)
    }
    if ((arrays & RK_PASS_ARRAY_EMBOSS) && (!vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD1) || p.mTextureList.size() > 1))
    {
        return false;       // (J1 K4: the emboss pass needs the second texture coordinate set and draws one image)
    }
    if (rigged && (p.mSkinInfo.isNull() || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4)))
    {
        return false;       // (no skin: the old path draws nothing, or cannot)
    }
    if (materials)
    {
        // the materials passes apply the texture matrix to every texcoord set, and need the maps their program samples (S5b: the alpha items carry the matrix in a slot)
        if (p.mTextureMatrix && !texture_matrix)
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
    return texture_matrix || !(p.mTexture.notNull() && p.mTextureMatrix);       // (S5a: the alpha items carry a texture matrix slot)
}

// S6 census: why geometry_packet_ok refuses a draw (0 = it does not)
int rk_geometry_reject_reason(const RKDrawBatch& p, bool batch_textures, const RKGeometryPassInfo* materials, bool rigged, bool texture_matrix, U32 arrays)
{
    const LLVertexBuffer* vb = p.mVertexBuffer.get();
    if (!p.mCount) return 1;
    if ((p.mAvatar != nullptr) != rigged) return 2;
    if (p.mGLTFMaterial) return 3;
    if (!vb || !vb->getVertexBufferId() || !vb->getIndexBufferId()) return 4;
    if (vb->getIndicesType() != ComponentType::U16) return 5;
    if (!vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0) return 6;
    if ((arrays & RK_PASS_ARRAY_NORMALS) && !vb->hasDataType(LLVertexBuffer::TYPE_NORMAL)) return 7;
    if ((arrays & RK_PASS_ARRAY_EMISSIVE) && !vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE)) return 14;
    if ((arrays & RK_PASS_ARRAY_BUMP) && (!vb->hasDataType(LLVertexBuffer::TYPE_TANGENT) || p.mTextureList.size() > 1)) return 15;
    if ((arrays & RK_PASS_ARRAY_EMBOSS) && (!vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD1) || p.mTextureList.size() > 1)) return 15;
    if (!vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0)) return 8;
    if (!vb->hasDataType(LLVertexBuffer::TYPE_COLOR)) return 9;
    if (rigged && (p.mSkinInfo.isNull() || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))) return 10;
    if (materials) return p.mTextureMatrix && !texture_matrix ? 12 : 11;
    if (batch_textures && p.mTextureList.size() > 1) return p.mTextureList.size() > 4 ? 13 : 0;
    if (!(texture_matrix || !(p.mTexture.notNull() && p.mTextureMatrix))) return 12;
    return 0;
}

// static. S5c: a draw with a glTF blend material as an entry of the alpha items: the pull shader reads 16 bit indices, position first, and the normal, texcoord 0, colour and tangent arrays
bool LLRenderPass::pbrAlphaEntry(const RKDrawBatch& params, RKVkBridge::GeoEntry& e, bool rigged)
{
    e = {};
    const LLVertexBuffer* vb = params.mVertexBuffer.get();
    if (!params.mCount || (params.mAvatar != nullptr) != rigged || (rigged && (params.mSkinInfo.isNull() || params.mModelMatrix || !vb || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))) || !vb || !vb->getVertexBufferId() || !vb->getIndexBufferId() || vb->getIndicesType() != ComponentType::U16
        || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0
        || !vb->hasDataType(LLVertexBuffer::TYPE_NORMAL) || !vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) || !vb->hasDataType(LLVertexBuffer::TYPE_COLOR) || !vb->hasDataType(LLVertexBuffer::TYPE_TANGENT))
    {
        return false;
    }
    e.mVertexBuffer = vb->getVertexBufferId();
    e.mIndexBuffer = vb->getIndexBufferId();
    e.mFirstIndex = params.mOffset;
    e.mIndexCount = params.mCount;
    e.mNormalOffset = vb->getOffset(LLVertexBuffer::TYPE_NORMAL);
    e.mTexcoordOffset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
    e.mColorOffset = vb->getOffset(LLVertexBuffer::TYPE_COLOR);
    e.mTangentOffset = vb->getOffset(LLVertexBuffer::TYPE_TANGENT);
    e.mEmissiveOffset = vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE) ? vb->getOffset(LLVertexBuffer::TYPE_EMISSIVE) : 0;       // S5e: the glow draw
    if (rigged)
    {
        e.mFlags |= RKVkBridge::GEO_RIGGED;
        e.mWeightOffset = vb->getOffset(LLVertexBuffer::TYPE_WEIGHT4);
    }
    return true;
}

// <FS> D4 stage 1b-1 (RK_VK_ROWCONST: 0 off, 1 a rigged draw whose vertices all have one colour carries it in its entry and the pull shader does not read the colour array, 2 the same and the shader also reads the
// array and paints a vertex magenta where the two differ: a check that needs no pose). The test of the row path the shared store will use (the colour is one constant a face there). Entries are built when a fragment
// is, not every frame; the scan reads the draw's range of the colour array.
static int rk_rowconst_mode()
{
    static const int mode = getenv("RK_VK_ROWCONST") ? atoi(getenv("RK_VK_ROWCONST")) : 0;
    return mode;
}
static void rk_rowconst_try(const LLVertexBuffer* vb, const RKDrawBatch& params, RKVkBridge::GeoEntry& e)
{
    static std::atomic<U64> considered{ 0 }, qualified{ 0 }, unreadable{ 0 };
    static const bool diag = getenv("RK_VK_DIAG") != nullptr;
    LLStrider<LLColor4U> col;
    const S32 n = (S32)params.mEnd - (S32)params.mStart + 1;
    if (n <= 0 || !vb->hasDataType(LLVertexBuffer::TYPE_COLOR) || !const_cast<LLVertexBuffer*>(vb)->getColorStrider(col, params.mStart, n))
    {
        ++unreadable;
        return;
    }
    const U32 first = col[0].asRGBA();
    bool same = true;
    for (S32 i = 1; i < n && same; ++i)
    {
        same = col[i].asRGBA() == first;
    }
    const U64 c = ++considered;
    if (same)
    {
        e.mFlags |= RKVkBridge::GEO_CONST_COLOR | (rk_rowconst_mode() == 2 ? RKVkBridge::GEO_VERIFY_COLOR : 0u);
        e.mConstColor = rk_rowconst_mode() == 3 ? 0xFF00FF00u : first;      // (mode 3: every constant is bright green: proof that the flag, the row field and the shader's branch work)
        ++qualified;
    }
    if (diag && c % 2000 == 0)
    {
        LL_INFOS("RowConst") << "rigged G-buffer draws looked at: " << c << ", with one colour for every vertex (carried in the entry) " << qualified.load() << ", colour array unreadable " << unreadable.load() << LL_ENDL;
    }
}

// static. S3: the per-draw part of build_geometry, with the textures as pointers (the fragments of the scene tables keep them)
bool LLRenderPass::geometryEntry(const RKDrawBatch& params, const RKGeometryPassInfo* materials, RKVkBridge::GeoEntry& e, LLViewerTexture* textures[6], bool rigged, bool texture_matrix, U32 arrays)
{
    e = {};
    for (int t = 0; t < 6; ++t)
    {
        textures[t] = nullptr;
    }
    if (!params.mCount || !geometry_packet_ok(params, materials == nullptr, materials, rigged, texture_matrix, arrays) || (rigged && params.mModelMatrix))
    {
        return false;
    }
    const LLVertexBuffer* vb = params.mVertexBuffer.get();
    e.mVertexBuffer = vb->getVertexBufferId();
    e.mIndexBuffer = vb->getIndexBufferId();
    e.mFirstIndex = params.mOffset;
    e.mIndexCount = params.mCount;
    {
        const LLColor4U debug_color = params.getDebugColor();        // K6 B1: the Batch Size debug view's colour of this draw, made once here (a CRC of the draw info) and read by the debug shader from the row
        memcpy(&e.mDebugColor, debug_color.mV, sizeof(e.mDebugColor));
    }
    e.mNormalOffset = vb->hasDataType(LLVertexBuffer::TYPE_NORMAL) ? vb->getOffset(LLVertexBuffer::TYPE_NORMAL) : 0;
    e.mTexcoordOffset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
    e.mColorOffset = vb->getOffset(LLVertexBuffer::TYPE_COLOR);
    if (rigged && rk_rowconst_mode() != 0)
    {
        rk_rowconst_try(vb, params, e);
    }
    if ((texture_matrix || (arrays & RK_PASS_ARRAY_EMISSIVE)) && vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
    {
        e.mEmissiveOffset = vb->getOffset(LLVertexBuffer::TYPE_EMISSIVE);       // S5e: (the alpha items) the glow draw
    }
    if (rigged)
    {
        e.mFlags |= RKVkBridge::GEO_RIGGED;
        e.mWeightOffset = vb->getOffset(LLVertexBuffer::TYPE_WEIGHT4);
    }
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
    if (!materials && params.mTextureList.size() > 1)
    {
        for (size_t t = 0; t < params.mTextureList.size() && t < 4; ++t)
        {
            if (params.mTextureList[t].notNull())
            {
                textures[t] = params.mTextureList[t].get();
                e.mTextures[t] = (U32)t + 1;
            }
        }
    }
    else
    {
        e.mFlags |= RKVkBridge::GEO_ONE_TEXTURE;
        if (params.mTexture.notNull())
        {
            textures[0] = params.mTexture.get();
            e.mTextures[0] = 1;
        }
        if (materials)
        {
            if (materials->mNormalMap)
            {
                textures[4] = params.mNormalMap.get();
                e.mNormalMap = 5;
            }
            if (materials->mSpecularMap)
            {
                textures[5] = params.mSpecularMap.get();
                e.mSpecularMap = 6;
            }
            memcpy(e.mSpecular, params.mSpecColor.mV, sizeof(e.mSpecular));
            e.mEnvIntensity = params.mEnvIntensity;
            e.mCutoff = params.mAlphaMaskCutoff;
            e.mBrightness = params.mFullbright ? 1.f : 0.f;
        }
    }
    e.mCutoff = params.mAlphaMaskCutoff;        // S6 X3: (the alpha masked passes without materials read it from the row too)
    if (arrays & RK_PASS_ARRAY_BUMP)
    {
        // J1 K4: the deferred bump pass: the tangent array and the bump image of the draw (made when the pass draws: LLDrawPoolBump::refreshBumpImage; here only the entry of it, a null one is the stand-in)
        e.mTangentOffset = vb->getOffset(LLVertexBuffer::TYPE_TANGENT);
        textures[4] = LLDrawPoolBump::bumpImageFor(params.mBump, params.mTexture.get(), false);
    }
    if (arrays & RK_PASS_ARRAY_EMBOSS)
    {
        // J1 K4: the post deferred emboss pass: the draw's bump image is the one texture of the row (the pass samples it twice), the second coordinate set's array is named; a draw with no image is not drawn (the old
        // pass skips it): no row, the old pass decides
        e.mTexcoord1Offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD1);
        textures[0] = LLDrawPoolBump::bumpImageFor(params.mBump, params.mTexture.get(), false);
        if (!textures[0])
        {
            return false;
        }
        e.mTextures[0] = 1;
    }
    return true;
}

// J1 K6 A2: a deferred geometry pass draws from the scene table and from nothing else: the draw record names the pass type, the worker draws the commands of the fragments the view's prepare listed with the program the
// pool bound. (The per frame records built from the render map, the fallback draws and the async builds that made them are gone: every view that draws is a view the table serves, every draw a fragment item.)
void LLRenderPass::pushGeometryPackets(U32 type, bool rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLCullResult* cull = gPipeline.getCull();
    const int tn = rk_scene_geo_type_number(type);
    if (tn < 0 || !(rk_scene_geo_serving() && rk_scene_geo_main_view()))
    {
        return;
    }
    if (!rk_scene_geo_prepared(cull))
    {
        if (cull && cull->mGeoElided)
        {
            rk_scene_geo_count(3, 1);       // postSort left the table's draws out of this render map and the table was not told: they are missing from this frame (should stay 0)
        }
        return;
    }
    RK_CPU_ZONE("packets: scene table draw record");
    {
        // RK_VK_PROBE=1: the program each kind of scene table record is pushed with (once per type): a wrong or missing one gets the record dropped by the worker
        static const bool probe = getenv("RK_VK_PROBE") != nullptr;
        static std::set<std::pair<int, std::string>> told;
        RKShaderProgram* cur = RKShaderProgram::sCurBoundShaderPtr;
        if (probe && told.insert({ tn, cur ? cur->mName : std::string("(none)") }).second)
        {
            LL_INFOS("VkProbe") << "scene table record of type " << tn << (rigged ? " (rigged)" : "") << " pushed with the bound program: " << (cur ? cur->mName : std::string("(none)")) << LL_ENDL;
        }
    }
    {
        // <FS> velocity fold: the programs that write motion (rigged, and the ones that read the motion table of a moving model matrix) take the camera matrices of this frame and the last from here (the bound program's loose block)
        extern bool rk_motion_fold_uniforms(RKShaderProgram* program);
        rk_motion_fold_uniforms(RKShaderProgram::sCurBoundShaderPtr);
    }
    RKVkBridge::ShadowPacketsHeader header;
    header.mKind = 22;
    header.mPass = (U32)tn;
    F32 matrices[32];
    memcpy(matrices, gRenderModelView, 16 * sizeof(F32));
    memcpy(matrices + 16, glm::value_ptr(gRender.getProjectionMatrix()), 16 * sizeof(F32));
    RKVkBridge::shadowPackets(header, matrices, sizeof(matrices));
    rk_scene_geo_count(1, 1);
    extern bool rk_scene_geo_type_has_double_sided(int tn);
    if (rk_scene_geo_type_has_double_sided(tn))
    {
        // P3: the type's double sided rows (glTF materials): their own draw, culling off (the worker draws nothing when the frame has none)
        RKRenderState::CullScope cull_face(false, true);
        header.mFragment = 1;
        RKVkBridge::shadowPackets(header, matrices, sizeof(matrices));
    }
}

void RKShadowDrawStats::endStaticPass()
{
    mStaticDistinctVB += mPassVBs.size();

    ++mTablePasses;
    mTableEntries += mTable.size();
    mTableMax = std::max<U64>(mTableMax, mTable.size());
    std::unordered_set<U64> pairs;
    for (const Caster& c : mTable)
    {
        const LLVertexBuffer* vb = c.mVB;
        if (!vb->getVertexBufferId() || !vb->getIndexBufferId())
        {
            ++mBadNames;
        }
        pairs.insert(((U64)vb->getVertexBufferId() << 32) | vb->getIndexBufferId());
        if (vb->getIndicesType() != ComponentType::U16)
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

//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo) // <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (!avatar || !skinInfo)       // <FS> C2.4c/d: a gone avatar or skin info (handles) reads as null: the draw is skipped
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

    RKShaderProgram::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
        count,
        false,
        (F32*)mpc.absPalette().data());

    return true;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, bool& skipLastSkin)// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(RKShaderProgram::sCurBoundShaderPtr);

    if (!avatar || !skinInfo)       // <FS> C2.4c/d: a gone avatar or skin info (handles) reads as null: the draw is skipped
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
        RKShaderProgram::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
            count,
            false,
            (F32*)mpc.absPalette().data());
    }

    return !skipLastSkin;
}

// Returns true if rendering should proceed
//static
bool LLRenderPass::uploadMatrixPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skinInfo, const LLVOAvatar*& lastAvatar, U64& lastMeshId, const RKShaderProgram*& lastAvatarShader, bool& skipLastSkin)// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    llassert(skinInfo);
    llassert(RKShaderProgram::sCurBoundShaderPtr);

    if (!avatar || !skinInfo)       // <FS> C2.4c/d: a gone avatar or skin info (handles) reads as null: the draw is skipped
    {
        return false;
    }

    if (avatar == lastAvatar && skinInfo->mHash == lastMeshId && lastAvatarShader == RKShaderProgram::sCurBoundShaderPtr)
    {
        return !skipLastSkin;
    }

    const LLVOAvatar::MatrixPaletteCache& mpc = avatar->updateSkinInfoMatrixPalette(skinInfo);
    U32 count = static_cast<U32>(mpc.mMatrixPalette.size());
    // skipLastSkin -> skin info not loaded yet, don't render
    skipLastSkin = !bool(count);
    lastAvatar = avatar;
    lastMeshId = skinInfo->mHash;
    lastAvatarShader = RKShaderProgram::sCurBoundShaderPtr;

    if (!skipLastSkin)
    {
        RKShaderProgram::sCurBoundShaderPtr->uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX,
            count,
            false,
            (F32*)mpc.absPalette().data());
    }

    return !skipLastSkin;
}

