 /**
 * @file llviewerjointmesh.cpp
 * @brief Implementation of LLViewerJointMesh class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

//-----------------------------------------------------------------------------
// Header Files
//-----------------------------------------------------------------------------
#include "llviewerprecompiledheaders.h"

#include "llfasttimer.h"
#include "llrender.h"

#include "llapr.h"
#include "llbox.h"
#include "lldrawable.h"
#include "lldrawpoolavatar.h"
#include "lldrawpoolbump.h"
#include "lldynamictexture.h"
#include "llface.h"
#include "rkrenderheaders.h"
#include "llviewertexlayer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"
#include "llviewerjointmesh.h"
#include "llvoavatar.h"
#include "llsky.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llmath.h"
#include "v4math.h"
#include "m3math.h"
#include "m4math.h"
#include "llmatrix4a.h"
#include "llperfstats.h"
#include "rkvkbridge.h"

#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <atomic>
#include "llappviewer.h"
#include "rkdiag.h"        // <FS> LL_DIAG: the statistics lines are off unless LL_VK_DIAG is set

extern bool gCubeSnapshot;

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// LLViewerJointMesh
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------


// the body meshes the main view drew lately (main thread, ray scene naming): the avatar is held while a mesh is in the list, and a mesh that is destroyed (an avatar that rebuilds its
// skeleton frees and remakes its meshes while it lives on) takes itself out in its destructor: no entry can point at a freed mesh
namespace
{
struct BodyMeshRecord
{
    LLPointer<LLVOAvatar> mAvatar;
    U32 mFrame = 0;
};
auto& sRecentBodyMeshes = *new std::unordered_map<LLViewerJointMesh*, BodyMeshRecord>();     // never destroyed: mesh destructors may run at exit
constexpr U32 kBodyMeshMemory = 2;      // frames a mesh is named again after the last time the main view drew it (the meshes are drawn every frame: a short memory keeps stale data out)
}

//-----------------------------------------------------------------------------
// LLViewerJointMesh()
//-----------------------------------------------------------------------------
LLViewerJointMesh::LLViewerJointMesh()
    :
    LLAvatarJointMesh()
{
}


//-----------------------------------------------------------------------------
// ~LLViewerJointMesh()
// Class Destructor
//-----------------------------------------------------------------------------
LLViewerJointMesh::~LLViewerJointMesh()
{
    sRecentBodyMeshes.erase(this);
}

const S32 NUM_AXES = 3;

// register layoud
// rotation X 0-n
// rotation Y 0-n
// rotation Z 0-n
// pivot parent 0-n -- child = n+1

static LLMatrix4    gJointMatUnaligned[LL_CHARACTER_MAX_JOINTS_PER_MESH];
static LLMatrix3    gJointRotUnaligned[LL_CHARACTER_MAX_JOINTS_PER_MESH];
static LLVector4    gJointPivot[LL_CHARACTER_MAX_JOINTS_PER_MESH];

//-----------------------------------------------------------------------------
// uploadJointMatrices()
//-----------------------------------------------------------------------------
void LLViewerJointMesh::computeJointPalette(F32* mat, bool with_modelview)
{
    S32 joint_num;
    LLPolyMesh *reference_mesh = mMesh->getReferenceMesh();

    //calculate joint matrices
    size_t num_joints = llmin(reference_mesh->mJointRenderData.size(), LL_CHARACTER_MAX_JOINTS_PER_MESH);
    for (joint_num = 0; joint_num < num_joints; joint_num++)
    {
        LLMatrix4 joint_mat = reference_mesh->mJointRenderData[joint_num]->getWorldMatrix();

        if (with_modelview)
        {
            joint_mat *= LLDrawPoolAvatar::getModelView();
        }
        gJointMatUnaligned[joint_num] = joint_mat;
        gJointRotUnaligned[joint_num] = joint_mat.getMat3();
    }

    bool last_pivot_uploaded{ false };
    S32 j = 0;

    //upload joint pivots
    for (joint_num = 0; joint_num < reference_mesh->mJointRenderData.size(); joint_num++)
    {
        LLSkinJoint *sj = reference_mesh->mJointRenderData[joint_num]->mSkinJoint;
        if (sj)
        {
            if (!last_pivot_uploaded)
            {
                LLVector4 parent_pivot(sj->mRootToParentJointSkinOffset);
                parent_pivot.mV[VW] = 0.f;
                gJointPivot[j++] = parent_pivot;
                if (j >= LL_CHARACTER_MAX_JOINTS_PER_MESH)
                {
                    break;
                }
            }

            LLVector4 child_pivot(sj->mRootToJointSkinOffset);
            child_pivot.mV[VW] = 0.f;

            gJointPivot[j++] = child_pivot;
            if (j >= LL_CHARACTER_MAX_JOINTS_PER_MESH)
            {
                break;
            }

            last_pivot_uploaded = true;
        }
        else
        {
            last_pivot_uploaded = false;
        }
    }

    //add pivot point into transform
    for (S32 i = 0; i < j; i++)
    {
        LLVector3 pivot;
        pivot = LLVector3(gJointPivot[i]);
        pivot = pivot * gJointRotUnaligned[i];
        gJointMatUnaligned[i].translate(pivot);
    }

    // the palette
        memset(mat, 0, sizeof(F32)*45*4);

        for (joint_num = 0; joint_num < num_joints; joint_num++)
        {
            gJointMatUnaligned[joint_num].transpose();

            for (S32 axis = 0; axis < NUM_AXES; axis++)
            {
                F32* vector = gJointMatUnaligned[joint_num].mMatrix[axis];
                U32 offset = LL_CHARACTER_MAX_JOINTS_PER_MESH*axis+joint_num;
                memcpy(mat+offset*4, vector, sizeof(F32)*4);
            }
        }
}

void LLViewerJointMesh::uploadJointMatrices()
{
    F32 mat[45*4];
    computeJointPalette(mat, true);
        if (RKShaderProgram::sCurBoundShaderPtr)
        {
            RKShaderProgram::sCurBoundShaderPtr->uniform4fv(LLViewerShaderMgr::AVATAR_MATRIX, 45, mat);
            // <FS> velocity fold: the body part writes its own motion: the world space palettes of this frame and of the last (the history follows the frames the main view draws the mesh:
            // updatePaletteHistory) and the camera matrices of both frames. Not in the shadow, impostor and probe renders (their targets have no motion attachment).
            if (mMesh && mMesh->hasWeights() && mPaletteComputed && mPaletteFrame == gFrameCount && LLDrawPoolAvatar::sShadowPass < 0 && !LLPipeline::sShadowRender && !LLPipeline::sImpostorRender && !gCubeSnapshot)
            {
                extern bool ll_motion_fold_uniforms(RKShaderProgram* program);
                if (ll_motion_fold_uniforms(RKShaderProgram::sCurBoundShaderPtr))
                {
                    static LLStaticHashedString palette_world_s("matrixPaletteWorld"), palette_prev_s("matrixPalettePrev");
                    RKShaderProgram::sCurBoundShaderPtr->uniform4fv(palette_world_s, 45, mWorldPalette);
                    RKShaderProgram::sCurBoundShaderPtr->uniform4fv(palette_prev_s, 45, mPrevWorldPalette);
                }
            }
        }
}

static bool rt_body_log_wanted()
{
    static const bool on = getenv("LL_VK_RT_BODYLOG") != nullptr;
    return on;
}


void LLViewerJointMesh::noteRecentBodyMeshes()
{
    if (!RKVkBridge::rtShadowMode())
    {
        return;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    for (auto it = sRecentBodyMeshes.begin(); it != sRecentBodyMeshes.end(); )
    {
        BodyMeshRecord& record = it->second;
        if (frame > record.mFrame + kBodyMeshMemory || record.mAvatar.isNull() || record.mAvatar->isDead())
        {
            it = sRecentBodyMeshes.erase(it);
            continue;
        }
        if (record.mFrame != frame)
        {
            it->first->noteRayShadow();         // (once per frame per mesh: noteRayShadow keeps count)
        }
        ++it;
    }
}

// <FS> Motion vectors (doc/vulkan/PLAN-ray-tracing.md, "Design notes for item 6"): the body palette in world space, this frame and last frame
void LLViewerJointMesh::updatePaletteHistory()
{
    if (mPaletteComputed && mPaletteFrame == gFrameCount)
    {
        return;
    }
    const bool had_last_frame = mPaletteComputed && mPaletteFrame == gFrameCount - 1;
    if (had_last_frame)
    {
        memcpy(mPrevWorldPalette, mWorldPalette, sizeof(mWorldPalette));
    }
    computeJointPalette(mWorldPalette, false);
    if (!had_last_frame)
    {
        memcpy(mPrevWorldPalette, mWorldPalette, sizeof(mWorldPalette));
    }
    mPaletteHasPrev = had_last_frame;
    mPaletteFrame = gFrameCount;
    mPaletteComputed = true;

    static std::atomic<U32> with_history{0}, without_history{0};
    ++(mPaletteHasPrev ? with_history : without_history);
    const U32 total = with_history + without_history;
    if (total % 20000 == 0)
    {
        LL_DIAG("VkMotion") << "body palettes computed: " << with_history << " with a previous palette, " << without_history << " without (new, or not asked for in the last frame)" << LL_ENDL;
    }
}

// <FS> Ray tracing (doc/vulkan/PLAN-rt-shadows.md, stage C): the ray scene skins the body on the GPU, from the same buffer and the same joints as the shadow maps draw it. One small
// record per mesh and frame (the four cascades draw it four times); nothing is drawn by it.
void LLViewerJointMesh::noteRayShadow()
{
    static U32 last_frame = ~0u;
    static std::unordered_set<const LLViewerJointMesh*> done;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != last_frame)
    {
        done.clear();
        last_frame = frame;
    }
    if (!done.insert(this).second)
    {
        return;
    }
    if (!mValid || !mVisible)
    {
        return;                             // not the level of detail in use (drawShape draws nothing for it): its offsets into the shared face buffer belong to another mesh by now
    }
    LLVertexBuffer* buff = mFace ? mFace->getVertexBuffer() : nullptr;
    LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
    if (!buff || !drawable || !mMesh || !buff->getVertexBufferId() || !buff->getIndexBufferId() || buff->getIndicesType() != ComponentType::U16
        || !buff->hasDataType(LLVertexBuffer::TYPE_VERTEX) || buff->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0 || mMesh->mFaceVertexCount == 0 || mMesh->mFaceIndexCount < 3)
    {
        return;
    }
    LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(drawable->getVObj().get());
    if (!avatar || (!avatar->isFullyLoaded() && RKVkBridge::rtLoadingGates()))
    {
        return;                             // (an avatar still loading in is not traced until it is loaded)
    }
    RKVkBridge::ShadowBodyEntry e = {};
    e.mVertexBuffer = buff->getVertexBufferId();
    e.mIndexBuffer = buff->getIndexBufferId();
    e.mFirstIndex = mMesh->mFaceIndexOffset;
    e.mIndexCount = mMesh->mFaceIndexCount;
    e.mVertexFirst = mMesh->mFaceVertexOffset;
    e.mVertexCount = mMesh->mFaceVertexCount;
    e.mAvatarId = (U32)(((uintptr_t)avatar) >> 4);
    e.mMeshId = (U32)(((uintptr_t)this) >> 4);
    e.mSelf = avatar->isSelf() ? 1u : 0u;
    {
        const LLVector3 avatar_position = avatar->getRenderPosition();
        e.mAvatarPos[0] = avatar_position.mV[0];
        e.mAvatarPos[1] = avatar_position.mV[1];
        e.mAvatarPos[2] = avatar_position.mV[2];
    }
    // every body mesh is cut by the alpha of its diffuse texture, found as drawShape finds it (the main view draws them all with the alpha test, minalpha 0.2): a head, an upper body or arms that a
    // system alpha layer hides under a mesh body, or the base body of a goldfish avatar, have alpha 0 there and must cast nothing (2026-11-11: the base head under Adam's mesh head showed in the sun
    // shadow on the lit side, and a fish avatar's shadow still had the base arms). Hair, eyelashes and skirt keep the cut of the old shadow shader (0.5); the rest is cut at the main view's 0.2.
    {
        const S32 mesh_id = LLAvatarJointMesh::getMeshID();
        const bool masked = true;
        const bool soft_cut = mesh_id == LLAvatarAppearanceDefines::MESH_ID_HAIR || mesh_id == LLAvatarAppearanceDefines::MESH_ID_EYELASH || mesh_id == LLAvatarAppearanceDefines::MESH_ID_SKIRT;
        if (buff->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0) && !mTestImageName)
        {
            LLViewerTexLayerSet* layerset = dynamic_cast<LLViewerTexLayerSet*>(mLayerSet);
            LLTexture* tex = nullptr;
            if (layerset)
            {
                if (layerset->hasComposite())
                {
                    tex = layerset->getViewerComposite();
                }
                else
                {
                    tex = LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT);
                }
            }
            else if (mTexture.notNull())
            {
                tex = mTexture.get();
            }
            if (masked && tex)
            {
                e.mAlphaFlags = RKVkBridge::ALPHA_TREE | RKVkBridge::ALPHA_NO_INDEX | RKVkBridge::ALPHA_HAS_TEXCOORD;
                e.mTexture = gRender.getTexUnit(0)->prepareFast(tex);
                e.mCutoff = soft_cut ? 0.5f : 0.2f;
            }
            e.mTexcoordOffset = buff->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
            if (tex)
            {
                e.mMaterial = rt_body_material(dynamic_cast<LLViewerTexture*>(tex), mColor.mV, masked);
                if (buff->hasDataType(LLVertexBuffer::TYPE_NORMAL))
                {
                    e.mNormalOffset = buff->getOffset(LLVertexBuffer::TYPE_NORMAL);
                }
            }
        }
    }
    std::vector<F32> palette;
    if (mMesh->hasWeights())
    {
        if (!buff->hasDataType(LLVertexBuffer::TYPE_WEIGHT))
        {
            return;
        }
        e.mMode = 1;
        e.mWeightOffset = buff->getOffset(LLVertexBuffer::TYPE_WEIGHT);
        e.mJoints = LL_CHARACTER_MAX_JOINTS_PER_MESH;
        F32 mat[45 * 4];
        computeJointPalette(mat, false);
        palette.resize(12 * LL_CHARACTER_MAX_JOINTS_PER_MESH);
        for (S32 j = 0; j < LL_CHARACTER_MAX_JOINTS_PER_MESH; ++j)
        {
            for (S32 axis = 0; axis < 3; ++axis)
            {
                memcpy(&palette[j * 12 + axis * 4], mat + (LL_CHARACTER_MAX_JOINTS_PER_MESH * axis + j) * 4, 4 * sizeof(F32));
            }
        }
    }
    else
    {
        e.mMode = 2;
        const LLMatrix4 joint_to_world = getWorldMatrix();
        memcpy(e.mModel, joint_to_world.mMatrix, sizeof(e.mModel));
    }
    RKVkBridge::ShadowPacketsHeader header;
    header.mKind = 4;
    header.mEntries = 1;
    header.mPaletteFloats = (U32)palette.size();
    std::vector<U8> blob(sizeof(e) + palette.size() * sizeof(F32));
    memcpy(blob.data(), &e, sizeof(e));
    if (!palette.empty())
    {
        memcpy(blob.data() + sizeof(e), palette.data(), palette.size() * sizeof(F32));
    }
    RKVkBridge::shadowPackets(header, blob.data(), blob.size());
}


//--------------------------------------------------------------------
// DrawElementsBLEND and utility code
//--------------------------------------------------------------------

// compare_int is used by the qsort function to sort the index array
int compare_int(const void *a, const void *b)
{
    if (*(U32*)a < *(U32*)b)
    {
        return -1;
    }
    else if (*(U32*)a > *(U32*)b)
    {
        return 1;
    }
    else return 0;
}

//--------------------------------------------------------------------
// LLViewerJointMesh::drawShape()
//--------------------------------------------------------------------
U32 LLViewerJointMesh::drawShape( F32 pixelArea, bool first_pass, bool is_dummy)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mValid || !mMesh || !mFace || !mVisible ||
        !mFace->getVertexBuffer() ||
        mMesh->getNumFaces() == 0 ||
        mMesh->mFaceVertexCount == 0 ||
        RKShaderProgram::sCurBoundShaderPtr == NULL)
    {
        return 0;
    }

    U32 triangle_count = 0;

    // the ray scene wants the avatars every frame: named by the shadow passes (which the far sun cascades run only every few frames) and by the main view
    if (RKVkBridge::rtShadowMode()
        && (LLDrawPoolAvatar::sShadowPass == LLDrawPoolAvatar::SHADOW_PASS_AVATAR_OPAQUE || (LLDrawPoolAvatar::sShadowPass < 0 && !LLPipeline::sShadowRender && !LLPipeline::sImpostorRender && !gCubeSnapshot)))
    {
        noteRayShadow();
        if (LLDrawPoolAvatar::sShadowPass < 0)
        {
            // drawn by the main view: remembered, so that the next frames name it before their scene call (noteRecentBodyMeshes)
            LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
            LLVOAvatar* avatar = drawable ? dynamic_cast<LLVOAvatar*>(drawable->getVObj().get()) : nullptr;
            if (avatar)
            {
                BodyMeshRecord& record = sRecentBodyMeshes[this];
                record.mAvatar = avatar;
                record.mFrame = LLFrameTimer::getFrameCount();
            }
        }
        if (rt_body_log_wanted())
        {
            // debug (LL_VK_RT_BODYLOG=1): how often each mesh of the viewer's own avatar is reached from the shadow passes and from the main view, logged every 600 frames
            static std::unordered_map<S32, std::array<U32, 2>> reached;
            static U32 last_report = 0;
            LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
            LLVOAvatar* own = drawable ? dynamic_cast<LLVOAvatar*>(drawable->getVObj().get()) : nullptr;
            const U32 frame_now = LLFrameTimer::getFrameCount();
            if (own && own->isSelf())
            {
                ++reached[LLAvatarJointMesh::getMeshID() * 16 + (mMesh && mMesh->hasWeights() ? 1 : 0)][LLDrawPoolAvatar::sShadowPass >= 0 ? 0 : 1];
            }
            if (frame_now >= last_report + 600)
            {
                for (const auto& kv : reached)
                {
                    LL_INFOS("VkRtMaterials") << "own body mesh id " << kv.first / 16 << " (skinned " << (kv.first & 15) << "): reached from shadow passes " << kv.second[0] << ", from the main view " << kv.second[1] << " times in " << (frame_now - last_report) << " frames" << LL_ENDL;
                }
                reached.clear();
                last_report = frame_now;
            }
        }
    }

    // motion vectors: the history of the body palette follows the frames in which the main view draws the mesh
    if (mMesh->hasWeights() && LLDrawPoolAvatar::sShadowPass < 0 && !LLPipeline::sShadowRender && !LLPipeline::sImpostorRender && !gCubeSnapshot)
    {
        updatePaletteHistory();
    }

    S32 diffuse_channel = LLDrawPoolAvatar::sDiffuseChannel;


    //----------------------------------------------------------------
    // setup current color
    //----------------------------------------------------------------
    if (is_dummy)
        gRender.diffuseColor4fv(LLVOAvatar::getDummyColor().mV);
    else
        gRender.diffuseColor4fv(mColor.mV);



    //----------------------------------------------------------------
    // setup current texture
    //----------------------------------------------------------------
    llassert( !(mTexture.notNull() && mLayerSet) );  // mutually exclusive

    LLViewerTexLayerSet *layerset = dynamic_cast<LLViewerTexLayerSet*>(mLayerSet);
    if (mTestImageName)
    {
        gRender.getTexUnit(diffuse_channel)->bindManual(LLTexUnit::TT_TEXTURE, mTestImageName);

        if (mIsTransparent)
        {
            gRender.diffuseColor4f(1.f, 1.f, 1.f, 1.f);
        }
        else
        {
            gRender.diffuseColor4f(0.7f, 0.6f, 0.3f, 1.f);
        }
    }
    else if( !is_dummy && layerset )
    {
        if( layerset->hasComposite() )
        {
            gRender.getTexUnit(diffuse_channel)->bind(layerset->getViewerComposite());
        }
        else
        {
            gRender.getTexUnit(diffuse_channel)->bind(LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT));
        }
    }
    else if ( !is_dummy && mTexture.notNull() )
    {
        gRender.getTexUnit(diffuse_channel)->bind(mTexture);
    }
    else
    {
        gRender.getTexUnit(diffuse_channel)->bind(LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT));
    }

    U32 start = mMesh->mFaceVertexOffset;
    U32 end = start + mMesh->mFaceVertexCount - 1;
    U32 count = mMesh->mFaceIndexCount;
    U32 offset = mMesh->mFaceIndexOffset;

    LLVertexBuffer* buff = mFace->getVertexBuffer();

    {
        // debug (LL_VK_BODYLOG=1): what a body mesh is drawn with, logged when it changes (a body mesh that is drawn wrong through the pull twin is compared with the ones that are not)
        static const bool wanted = getenv("LL_VK_BODYLOG") != nullptr;
        if (wanted && LLDrawPoolAvatar::sShadowPass < 0 && !LLPipeline::sShadowRender && !LLPipeline::sImpostorRender)
        {
            static std::unordered_map<const LLViewerJointMesh*, std::string> told;
            LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
            LLVOAvatar* avatar = drawable ? dynamic_cast<LLVOAvatar*>(drawable->getVObj().get()) : nullptr;
            std::ostringstream sig;
            sig << "mesh " << LLAvatarJointMesh::getMeshID() << " lod/skinned " << (mMesh->hasWeights() ? 1 : 0) << " texture "
                << (mTestImageName ? "test" : (!is_dummy && layerset) ? (layerset->hasComposite() ? "composite" : "composite-missing") : (!is_dummy && mTexture.notNull()) ? "texture" : "default")
                << " color " << mColor.mV[0] << "," << mColor.mV[1] << "," << mColor.mV[2] << "," << mColor.mV[3] << " transparent " << (mIsTransparent ? 1 : 0)
                << " range " << start << ".." << end << " idx " << offset << "+" << count
                << " types " << (buff ? buff->getTypeMask() : 0) << " program " << (RKShaderProgram::sCurBoundShaderPtr ? RKShaderProgram::sCurBoundShaderPtr->mName : std::string("none"))
                << " minalpha " << LLDrawPoolAvatar::sMinimumAlpha;
            std::string& was = told[this];
            if (was != sig.str())
            {
                was = sig.str();
                LL_INFOS("BodyLog") << (avatar ? avatar->getFullname() : std::string("?")) << (avatar && avatar->isSelf() ? " (self)" : "") << " " << was << LL_ENDL;
            }
        }
    }

    {
        // debug (LL_VK_POSELAG=1): how far the viewer's own avatar is, when its mesh is drawn, from where its joints are (root joint world position against the render position of the avatar, which
        // the camera and the views follow): a pose that is a frame old shows as a difference while the avatar moves. Logged every 300 frames of the main view.
        static const bool wanted = getenv("LL_VK_POSELAG") != nullptr;
        if (wanted && LLDrawPoolAvatar::sShadowPass < 0 && !LLPipeline::sShadowRender && !LLPipeline::sImpostorRender && !gCubeSnapshot)
        {
            LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
            LLVOAvatar* avatar = drawable ? dynamic_cast<LLVOAvatar*>(drawable->getVObj().get()) : nullptr;
            static U32 last_frame = 0, frames = 0, lagging = 0;
            static F32 worst = 0.f, sum = 0.f, speed_sum = 0.f;
            static LLVector3 last_pos, last_offset;
            if (avatar && avatar->isSelf() && last_frame != LLFrameTimer::getFrameCount())
            {
                last_frame = LLFrameTimer::getFrameCount();
                const LLVector3 root = avatar->getRootJoint()->getWorldPosition();
                const LLVector3 offset = root - avatar->getRenderPosition();
                const F32 diff = (offset - last_offset).length();       // (the offset is constant while the pose and the position are of the same moment)
                last_offset = offset;
                const F32 speed = (avatar->getRenderPosition() - last_pos).length();
                last_pos = avatar->getRenderPosition();
                ++frames;
                sum += diff;
                speed_sum += speed;
                lagging += (diff > 0.05f) ? 1 : 0;
                worst = llmax(worst, diff);
                if (frames >= 300)
                {
                    LL_INFOS("PoseLag") << "own avatar over 300 frames: the offset of the root joint to the render position changes by " << sum / frames << " m a frame on average, worst " << worst << " m, " << lagging << " frames over 5 cm; the avatar moved " << speed_sum / frames << " m a frame on average" << LL_ENDL;
                    frames = lagging = 0;
                    worst = sum = speed_sum = 0.f;
                }
            }
        }
    }

    if (mMesh->hasWeights())
    {
        if (first_pass)
        {
            uploadJointMatrices();
        }

        if (RKShaderProgram::sCurBoundShaderPtr && RKShaderProgram::sCurBoundShaderPtr->isVertexPull())
        {
            ll_pull_draw_range(buff, offset, count);        // S6.6d
        }
        else
        {
            ll_pool_draw_dropped("avatar body part (skinned)", 1);        // (the old direct draw is gone: no pull program bound)
        }
    }
    else
    {
        RKRenderMatrix::push(RKRenderMatrix::Kind::Modelview);
        LLMatrix4 jointToWorld = getWorldMatrix();
        RKRenderMatrix::multiply(RKRenderMatrix::Kind::Modelview, (F32*)jointToWorld.mMatrix);
        if (RKShaderProgram::sCurBoundShaderPtr && RKShaderProgram::sCurBoundShaderPtr->isVertexPull())
        {
            ll_pull_draw_range(buff, offset, count);        // S6.6d
        }
        else
        {
            ll_pool_draw_dropped("avatar body part (rigid)", 1);        // (the old direct draw is gone: no pull program bound)
        }
        RKRenderMatrix::pop(RKRenderMatrix::Kind::Modelview);
    }
    gPipeline.addTrianglesDrawn(count);

    triangle_count += count;

    return triangle_count;
}

//-----------------------------------------------------------------------------
// updateFaceSizes()
//-----------------------------------------------------------------------------
void LLViewerJointMesh::updateFaceSizes(U32 &num_vertices, U32& num_indices, F32 pixel_area)
{
    //bump num_vertices to next multiple of 4
    num_vertices = (num_vertices + 0x3) & ~0x3;

    // Do a pre-alloc pass to determine sizes of data.
    if (mMesh && mValid)
    {
        mMesh->mFaceVertexOffset = num_vertices;
        mMesh->mFaceVertexCount = mMesh->getNumVertices();
        mMesh->mFaceIndexOffset = num_indices;
        mMesh->mFaceIndexCount = mMesh->getSharedData()->mNumTriangleIndices;

        mMesh->getReferenceMesh()->mCurVertexCount = mMesh->mFaceVertexCount;

        num_vertices += mMesh->getNumVertices();
        num_indices += mMesh->mFaceIndexCount;
    }
}

//-----------------------------------------------------------------------------
// updateFaceData()
//-----------------------------------------------------------------------------

void LLViewerJointMesh::updateFaceData(LLFace *face, F32 pixel_area, bool damp_wind, bool terse_update)
{
    //IF THIS FUNCTION BREAKS, SEE LLPOLYMESH CONSTRUCTOR AND CHECK ALIGNMENT OF INPUT ARRAYS

    mFace = face;

    if (!mFace->getVertexBuffer())
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED;

    LLStrider<LLVector3> verticesp;
    LLStrider<LLVector3> normalsp;
    LLStrider<LLVector2> tex_coordsp;
    LLStrider<F32>       vertex_weightsp;
    // <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
    //LLStrider<LLVector4> clothing_weightsp;
    LLStrider<LLVector4a> clothing_weightsp;
    LLStrider<U16> indicesp;

    // Copy data into the faces from the polymesh data.
    if (mMesh && mValid)
    {
        const U32 num_verts = mMesh->getNumVertices();

        if (num_verts)
        {
            face->getVertexBuffer()->getIndexStrider(indicesp);
            face->getGeometryAvatar(verticesp, normalsp, tex_coordsp, vertex_weightsp, clothing_weightsp);

            verticesp += mMesh->mFaceVertexOffset;
            normalsp += mMesh->mFaceVertexOffset;

            F32* v = (F32*) verticesp.get();
            F32* n = (F32*) normalsp.get();

            U32 words = num_verts*4;

            LLVector4a::memcpyNonAliased16(v, (F32*) mMesh->getCoords(), words*sizeof(F32));
            LLVector4a::memcpyNonAliased16(n, (F32*) mMesh->getNormals(), words*sizeof(F32));


            if (!terse_update)
            {
                vertex_weightsp += mMesh->mFaceVertexOffset;
                clothing_weightsp += mMesh->mFaceVertexOffset;
                tex_coordsp += mMesh->mFaceVertexOffset;

                F32* tc = (F32*) tex_coordsp.get();
                F32* vw = (F32*) vertex_weightsp.get();
                F32* cw = (F32*) clothing_weightsp.get();

                //S32 tc_size = (num_verts*2*sizeof(F32)+0xF) & ~0xF;
                //LLVector4a::memcpyNonAliased16(tc, (F32*) mMesh->getTexCoords(), tc_size);
                //S32 vw_size = (num_verts*sizeof(F32)+0xF) & ~0xF;
                //LLVector4a::memcpyNonAliased16(vw, (F32*) mMesh->getWeights(), vw_size);

                // Both allocated in LLPolyMeshSharedData::allocateVertexData(unsigned int)

                memcpy(tc, mMesh->getTexCoords(), num_verts*2*sizeof(F32) );
                memcpy(vw, mMesh->getWeights(), num_verts*sizeof(F32) );

                LLVector4a::memcpyNonAliased16(cw, (F32*) mMesh->getClothingWeights(), num_verts*4*sizeof(F32));
            }

            const U32 idx_count = mMesh->getNumFaces()*3;

            indicesp += mMesh->mFaceIndexOffset;

            U16* __restrict idx = indicesp.get();
            S32* __restrict src_idx = (S32*) mMesh->getFaces();

            const S32 offset = (S32) mMesh->mFaceVertexOffset;

            for (U32 i = 0; i < idx_count; ++i)
            {
                *(idx++) = *(src_idx++)+offset;
            }
        }
    }
}



//-----------------------------------------------------------------------------
// updateLOD()
//-----------------------------------------------------------------------------
bool LLViewerJointMesh::updateLOD(F32 pixel_area, bool activate)
{
    bool valid = mValid;
    setValid(activate, true);
    return (valid != activate);
}

void LLViewerJointMesh::updateJointGeometry()
{
}

void LLViewerJointMesh::dump()
{
    if (mValid)
    {
        LL_INFOS() << "Usable LOD " << mName << LL_ENDL;
    }
}

// End
