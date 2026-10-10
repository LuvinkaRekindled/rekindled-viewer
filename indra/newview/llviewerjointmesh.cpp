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
#include "llglheaders.h"
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
#include "llvkmirror.h"

#include <unordered_set>

#if !LL_DARWIN && !LL_LINUX
extern PFNGLWEIGHTPOINTERARBPROC glWeightPointerARB;
extern PFNGLWEIGHTFVARBPROC glWeightfvARB;
extern PFNGLVERTEXBLENDARBPROC glVertexBlendARB;
#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// LLViewerJointMesh
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------


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
        LLMatrix4 joint_mat = *reference_mesh->mJointRenderData[joint_num]->mWorldMatrix;

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
        memset(mat, 0, sizeof(GLfloat)*45*4);

        for (joint_num = 0; joint_num < num_joints; joint_num++)
        {
            gJointMatUnaligned[joint_num].transpose();

            for (S32 axis = 0; axis < NUM_AXES; axis++)
            {
                F32* vector = gJointMatUnaligned[joint_num].mMatrix[axis];
                U32 offset = LL_CHARACTER_MAX_JOINTS_PER_MESH*axis+joint_num;
                memcpy(mat+offset*4, vector, sizeof(GLfloat)*4);
            }
        }
}

void LLViewerJointMesh::uploadJointMatrices()
{
    GLfloat mat[45*4];
    computeJointPalette(mat, true);
        stop_glerror();
        if (LLGLSLShader::sCurBoundShaderPtr)
        {
            LLGLSLShader::sCurBoundShaderPtr->uniform4fv(LLViewerShaderMgr::AVATAR_MATRIX, 45, mat);
        }
        stop_glerror();
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
    LLVertexBuffer* buff = mFace ? mFace->getVertexBuffer() : nullptr;
    LLDrawable* drawable = mFace ? mFace->getDrawable() : nullptr;
    if (!buff || !drawable || !mMesh || !buff->getGLBuffer() || !buff->getGLIndices() || buff->getIndicesType() != GL_UNSIGNED_SHORT
        || !buff->hasDataType(LLVertexBuffer::TYPE_VERTEX) || buff->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0 || mMesh->mFaceVertexCount == 0 || mMesh->mFaceIndexCount < 3)
    {
        return;
    }
    LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(drawable->getVObj().get());
    if (!avatar)
    {
        return;
    }
    LLVkMirror::ShadowBodyEntry e = {};
    e.mVertexBuffer = buff->getGLBuffer();
    e.mIndexBuffer = buff->getGLIndices();
    e.mFirstIndex = mMesh->mFaceIndexOffset;
    e.mIndexCount = mMesh->mFaceIndexCount;
    e.mVertexFirst = mMesh->mFaceVertexOffset;
    e.mVertexCount = mMesh->mFaceVertexCount;
    e.mAvatarId = (U32)(((uintptr_t)avatar) >> 4);
    e.mMeshId = (U32)(((uintptr_t)this) >> 4);
    {
        const LLVector3 avatar_position = avatar->getRenderPosition();
        e.mAvatarPos[0] = avatar_position.mV[0];
        e.mAvatarPos[1] = avatar_position.mV[1];
        e.mAvatarPos[2] = avatar_position.mV[2];
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
    LLVkMirror::ShadowPacketsHeader header;
    header.mKind = 4;
    header.mEntries = 1;
    header.mPaletteFloats = (U32)palette.size();
    std::vector<U8> blob(sizeof(e) + palette.size() * sizeof(F32));
    memcpy(blob.data(), &e, sizeof(e));
    if (!palette.empty())
    {
        memcpy(blob.data() + sizeof(e), palette.data(), palette.size() * sizeof(F32));
    }
    LLVkMirror::shadowPackets(header, blob.data(), blob.size());
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
        LLGLSLShader::sCurBoundShaderPtr == NULL)
    {
        return 0;
    }

    U32 triangle_count = 0;

    if (LLDrawPoolAvatar::sShadowPass == LLDrawPoolAvatar::SHADOW_PASS_AVATAR_OPAQUE && LLVkMirror::rtShadowMode())
    {
        noteRayShadow();
    }

    S32 diffuse_channel = LLDrawPoolAvatar::sDiffuseChannel;

    stop_glerror();

    //----------------------------------------------------------------
    // setup current color
    //----------------------------------------------------------------
    if (is_dummy)
        gGL.diffuseColor4fv(LLVOAvatar::getDummyColor().mV);
    else
        gGL.diffuseColor4fv(mColor.mV);

    stop_glerror();


    //----------------------------------------------------------------
    // setup current texture
    //----------------------------------------------------------------
    llassert( !(mTexture.notNull() && mLayerSet) );  // mutually exclusive

    LLViewerTexLayerSet *layerset = dynamic_cast<LLViewerTexLayerSet*>(mLayerSet);
    if (mTestImageName)
    {
        gGL.getTexUnit(diffuse_channel)->bindManual(LLTexUnit::TT_TEXTURE, mTestImageName);

        if (mIsTransparent)
        {
            gGL.diffuseColor4f(1.f, 1.f, 1.f, 1.f);
        }
        else
        {
            gGL.diffuseColor4f(0.7f, 0.6f, 0.3f, 1.f);
        }
    }
    else if( !is_dummy && layerset )
    {
        if( layerset->hasComposite() )
        {
            gGL.getTexUnit(diffuse_channel)->bind(layerset->getViewerComposite());
        }
        else
        {
            gGL.getTexUnit(diffuse_channel)->bind(LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT));
        }
    }
    else if ( !is_dummy && mTexture.notNull() )
    {
        gGL.getTexUnit(diffuse_channel)->bind(mTexture);
    }
    else
    {
        gGL.getTexUnit(diffuse_channel)->bind(LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT));
    }

    U32 start = mMesh->mFaceVertexOffset;
    U32 end = start + mMesh->mFaceVertexCount - 1;
    U32 count = mMesh->mFaceIndexCount;
    U32 offset = mMesh->mFaceIndexOffset;

    LLVertexBuffer* buff = mFace->getVertexBuffer();

    if (mMesh->hasWeights())
    {
        if (first_pass)
        {
            uploadJointMatrices();
        }

        buff->setBuffer();
        buff->drawRange(LLRender::TRIANGLES, start, end, count, offset);
    }
    else
    {
        gGL.pushMatrix();
        LLMatrix4 jointToWorld = getWorldMatrix();
        gGL.multMatrix((GLfloat*)jointToWorld.mMatrix);
        buff->setBuffer();
        buff->drawRange(LLRender::TRIANGLES, start, end, count, offset);
        gGL.popMatrix();
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
