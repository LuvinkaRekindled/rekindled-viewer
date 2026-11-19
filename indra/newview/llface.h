/**
 * @file llface.h
 * @brief LLFace class definition
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

#ifndef LL_LLFACE_H
#define LL_LLFACE_H

#include "rkfacetable.h"
#include "llstrider.h"
#include "llrender.h"
#include "v2math.h"
#include "v3math.h"
#include "v4math.h"
#include "m4math.h"
#include "v4coloru.h"
#include "llquaternion.h"
#include "xform.h"
#include "llvertexbuffer.h"
#include "llviewertexture.h"
#include "lldrawable.h"
#include "lljoint.h"
#include "llmodel.h"// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh

class LLFacePool;
class LLVolume;
class LLViewerTexture;
class LLTextureEntry;
class LLVertexProgram;
class LLViewerTexture;
class LLGeometryManager;
class RKDrawBatch;
// class LLMeshSkinInfo;// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh

const F32 MIN_ALPHA_SIZE = 1024.f;
// <FS:minerjr>
//const F32 MIN_TEX_ANIM_SIZE = 512.f;
// Reduce the cut off point of the animated textures down to the size when textures get discarded/set to invalid size.
// This should prevent "on screen" animated textures from stopping.
const F32 MIN_TEX_ANIM_SIZE = 16.f;
// </FS:minerjr>
const U8 FACE_DO_NOT_BATCH_TEXTURES = 255;

class alignas(16) LLFace
{
    LL_ALIGN_NEW
public:
    LLFace(const LLFace& rhs) = delete;
    const LLFace& operator=(const LLFace& rhs) = delete;

    enum EMasks
    {
        LIGHT           = 0x0001,
        GLOBAL          = 0x0002,
        FULLBRIGHT      = 0x0004,
        HUD_RENDER      = 0x0008,
        USE_FACE_COLOR  = 0x0010,
        TEXTURE_ANIM    = 0x0020,
        RIGGED          = 0x0040,
        PARTICLE        = 0x0080,
    };

public:
    LLFace(LLDrawable* drawablep, LLViewerObject* objp)
    {
        LL_PROFILE_ZONE_SCOPED;
        mRowId = RKFaceRows::acquire(this);     // <FS> C3.1: a zeroed row: the geometry slice, the state, the colour ... (rkfacetable.h)
        if (!mRowId)
        {
            LL_ERRS() << "the face table is full (" << RKFaceRows::kCapacity << " faces)" << LL_ENDL;
        }
        init(drawablep, objp);
    }
    ~LLFace()  { destroy(); RKFaceRows::release(mRowId, 0); }

    const LLMatrix4& getWorldMatrix()   const   { return mVObjp->getWorldMatrix(mXform); }
    const LLMatrix4& getRenderMatrix() const;
    U32             getIndicesCount()   const   { return row().indicesCount; };
    S32             getIndicesStart()   const   { return row().indicesIndex; };
    U16             getGeomCount()      const   { return row().geomCount; }      // vertex count for this face
    U16             getGeomIndex()      const   { return row().geomIndex; }      // index into draw pool
    U16             getGeomStart()      const   { return row().geomIndex; }      // index into draw pool
    void            setTextureIndex(U8 index);
    U8              getTextureIndex() const     { return row().textureIndex; }
    void            setTexture(U32 ch, LLViewerTexture* tex);
    void            setTexture(LLViewerTexture* tex) ;
    void            setDiffuseMap(LLViewerTexture* tex);
    void            setNormalMap(LLViewerTexture* tex);
    void            setSpecularMap(LLViewerTexture* tex);
    void            setAlternateDiffuseMap(LLViewerTexture* tex);
    void            switchTexture(U32 ch, LLViewerTexture* new_texture);
    void            dirtyTexture();
    LLXformMatrix*  getXform()          const   { return mXform; }
    bool            hasGeometry()       const   { return row().geomCount > 0; }
    LLVector3       getPositionAgent()  const;
    LLVector2       surfaceToTexture(LLVector2 surface_coord, const LLVector4a& position, const LLVector4a& normal);
    void            getPlanarProjectedParams(LLQuaternion* face_rot, LLVector3* face_pos, F32* scale) const;
    bool            calcAlignedPlanarGLTF(const LLFace* align_to, LLVector2* res_st_offset,
                                        LLVector2* res_st_scale, F32* res_st_rot, S32 gltf_info_index = 0) const;
    bool            calcAlignedPlanarTE(const LLFace* align_to, LLVector2* st_offset,
                                        LLVector2* st_scale, F32* st_rot, LLRender::eTexIndex map = LLRender::DIFFUSE_MAP) const;

    U32             getState()          const   { return row().state; }
    void            setState(U32 state)         { row().state |= state; }
    void            clearState(U32 state)       { row().state &= ~state; }
    bool            isState(U32 state)  const   { return (row().state & state) != 0; }
    void            setVirtualSize(F32 size) { row().vSize = size; }
    void            setPixelArea(F32 area)  { row().pixelArea = area; }
    F32             getVirtualSize() const { return row().vSize; }
    F32             getPixelArea() const { return row().pixelArea; }

    S32             getIndexInTex(U32 ch) const      { llassert(ch < LLRender::NUM_TEXTURE_CHANNELS); return mIndexInTex[ch]; }
    void            setIndexInTex(U32 ch, S32 index) { llassert(ch < LLRender::NUM_TEXTURE_CHANNELS); mIndexInTex[ch] = index; }

    void            setWorldMatrix(const LLMatrix4& mat);
    const LLTextureEntry* getTextureEntry() const { return mVObjp ? mVObjp->getTE(row().teOffset) : nullptr; }

    LLFacePool*     getPool()           const   { return mDrawPoolp; }
    U32             getPoolType()       const   { return mPoolType; }
    LLDrawable*     getDrawable()       const   { return mDrawablep; }
    void            stampTextures();    // <FS> native-vk: something the texture priority reads from this face changed (see LLViewerTexture::mFaceStamp)
    // <FS> R3b2: set on the thread that fills the faces of a render batch when the caller has stamped the batch's faces itself (the stamps are main thread work: they raise the counter of a texture the faces share)
    static thread_local bool sStampedByCaller;
    static thread_local std::vector<LLFace*>* sStampSink;      // <FS> set while an attachment job runs: updateCenterAgent lists the face here and the main thread stamps it when the job's writes are replayed
    bool            hasSelectionBuffer() const { return mVertexBufferGLTF.notNull(); }
    LLViewerObject* getViewerObject()   const   { return mVObjp; }
    S32             getLOD()            const   { return mVObjp.notNull() ? mVObjp->getLOD() : 0; }
    void            setPoolType(U32 type)       { mPoolType = type; }
    S32             getTEOffset()       const   { return row().teOffset; }
    LLViewerTexture*    getTexture(U32 ch = LLRender::DIFFUSE_MAP) const;

    void            setViewerObject(LLViewerObject* object);
    void            setPool(LLFacePool *pool, LLViewerTexture *texturep);
    void            setPool(LLFacePool* pool);

    void            setDrawable(LLDrawable *drawable);
    void            setTEOffset(const S32 te_offset);

    void            renderIndexed();

    void            setFaceColor(const LLColor4& color); // override material color
    void            unsetFaceColor(); // switch back to material color
    const LLColor4& getFaceColor() const { return row().faceColor; }


    //for volumes
    void updateRebuildFlags();
    bool canRenderAsMask(); // logic helper
    bool getGeometryVolume(const LLVolume& volume,
                            S32 face_index,
                            const LLMatrix4& mat_vert,
                            const LLMatrix3& mat_normal,
                            U16 index_offset,
                            bool force_rebuild = false,
                            bool no_debug_assert = false,
                            bool rebuild_for_gltf = false);

    // For avatar
    U16          getGeometryAvatar(
                                    LLStrider<LLVector3> &vertices,
                                    LLStrider<LLVector3> &normals,
                                    LLStrider<LLVector2> &texCoords,
                                    LLStrider<F32>       &vertex_weights,
                                    // <FS:Ansariel> Vectorized Weight4Strider and ClothWeightStrider by Drake Arconis
                                    //LLStrider<LLVector4> &clothing_weights);
                                    LLStrider<LLVector4a> &clothing_weights);

    // For volumes, etc.
    U16             getGeometry(LLStrider<LLVector3> &vertices,
                                LLStrider<LLVector3> &normals,
                                LLStrider<LLVector2> &texCoords,
                                LLStrider<U16>  &indices);

    S32 getColors(LLStrider<LLColor4U> &colors);
    S32 getIndices(LLStrider<U16> &indices);

    void        setSize(S32 numVertices, S32 num_indices = 0, bool align = false);

    bool        genVolumeBBoxes(const LLVolume &volume, S32 f,
                                    const LLMatrix4& mat_vert_in, bool global_volume = false);

    void        init(LLDrawable* drawablep, LLViewerObject* objp);
    void        destroy();
    void        update();

    void        updateCenterAgent(); // Update center when xform has changed.
    void        renderSelectedUV();

    void        renderSelected(LLViewerTexture *image, const LLColor4 &color);
    void        renderOneWireframe(const LLColor4 &color, F32 fogCfx, bool wireframe_selection, bool bRenderHiddenSelections, bool shader);

    F32         getKey()                    const   { return row().distance; }

    S32         getReferenceIndex()         const   { return row().referenceIndex; }
    void        setReferenceIndex(const S32 index)  { row().referenceIndex = index; }

    bool        verify(const U32* indices_array = NULL) const;
    void        printDebugInfo() const;

    void        setGeomIndex(U16 idx);
    void        setIndicesIndex(S32 idx);

    F32         getTextureVirtualSize() ;
    void        resetVirtualSize();

    void        setHasMedia(bool has_media)  { row().hasMedia = has_media ;}
    bool        hasMedia() const ;

    void        setMediaAllowed(bool is_media_allowed)  { row().isMediaAllowed = is_media_allowed; }
    bool        isMediaAllowed() const { return row().isMediaAllowed; }

    bool        switchTexture() ;

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    bool        isDefaultTexture(U32 nChannel) const;
    void        setDefaultTexture(U32 nChannel, bool fShowDefault) const;
// [/SL:KB]

    //vertex buffer tracking
    void setVertexBuffer(LLVertexBuffer* buffer);
    void clearVertexBuffer(); //sets mVertexBuffer to NULL
    LLVertexBuffer* getVertexBuffer()   const   { return mVertexBuffer; }
    S32 getRiggedIndex(U32 type) const;

    // used to preserve draw order of faces that are batched together.
    // Allows content creators to manipulate linked sets and face ordering
    // for consistent alpha sorting results, particularly for rigged attachments
    void setDrawOrderIndex(U32 index) { row().drawOrderIndex = index; }
    U32 getDrawOrderIndex() const { return row().drawOrderIndex; }

    // return true if this face is in an alpha draw pool
    bool isInAlphaPool() const;
public: //aligned members

    // <FS> C3.2: the bounds group (row().extents, row().riggedExtents, row().centerLocal, row().centerAgent, row().texExtents) is in the face's row: row().extents[2] (bounding box of the face in drawable space), row().riggedExtents[2]
    // (cached bounding box of a rigged face in world space, calculated on demand by calcPixelArea, may be stale), row().centerLocal, row().centerAgent, row().texExtents[2]
    RKFaceRow& row() const { return RKFaceRows::row(mRowId); }

private:
    friend class LLViewerTextureList;
    friend class RKTexRecs;
    F32         adjustPartialOverlapPixelArea(F32 cos_angle_to_view_dir, F32 radius );
    bool        calcPixelArea(F32& cos_angle_to_view_dir, F32& radius) ;
public:
    static F32 calcImportanceToCamera(F32 to_view_dir, F32 dist);
    static F32 adjustPixelArea(F32 importance, F32 pixel_area) ;

public:


    LLMatrix4*  mTextureMatrix;
    LLMatrix4*  mSpecMapMatrix;
    LLMatrix4*  mNormalMapMatrix;
    LLVOAvatar* mAvatar = nullptr;
    bool mRigStore = false;     // <FS> D4 1b-3 (RK_VK_RIGSTORE=2): the face's vertex buffer is a shared cache buffer (rkrigstore.h): getGeometryVolume does not write it
    LLConstPointer<LLMeshSkinInfo> mSkinInfo;// <FS:Beq/> be defensive about UAF with skinInfo during LocalMesh

    // return mSkinInfo->mHash or 0 if mSkinInfo is null
    U64 getSkinHash();

    // true if face was recently in the main camera frustum according to LLViewerTextureList updates
    // value of gFrameCount the last time the face was touched by LLViewerTextureList::updateImageDecodePriority
    // <FS> the record of this face for each texture channel in RKTexRecs (0: none)
    F32 getBoundingSphereRadius() const { return row().boundingSphereRadius; }

private:
    // <FS> C3.1: this face's row in the face table (rkfacetable.h): the geometry slice (geomCount, geomIndex, indicesCount, indicesIndex), textureIndex, referenceIndex, state, faceColor, teOffset, boundingSphereRadius
    U32 mRowId = 0;

    LLPointer<LLVertexBuffer> mVertexBuffer;
    LLPointer<LLVertexBuffer> mVertexBufferGLTF;

    LLFacePool* mDrawPoolp;
    U32         mPoolType;

    S32         mIndexInTex[LLRender::NUM_TEXTURE_CHANNELS];

    LLXformMatrix* mXform;

    LLPointer<LLViewerTexture> mTexture[LLRender::NUM_TEXTURE_CHANNELS];

    // mDrawablep is not supposed to be null, don't use LLPointer because
    // mDrawablep owns LLFace and LLPointer is a good way to either cause a
    // memory leak or a 'delete each other' situation if something deletes
    // drawable wrongly.
    LLDrawable* mDrawablep;
    // LLViewerObject technically owns drawable, but also it should be strictly managed
    LLPointer<LLViewerObject> mVObjp;

    std::vector<S32> mRiggedIndex;

    // gFrameTimeSeconds when row().pixelArea was last updated

    // virtual size of face in texture area  (row().pixelArea adjusted by texture repeats)
    // used to determine desired resolution of texture

    // pixel area face covers on screen

    //importance factor, in the range [0, 1.0].
    //1.0: the most important.
    //based on the distance from the face to the view point and the angle from the face center to the view direction.


// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    mutable bool                       mShowDiffTexture;
    mutable LLPointer<LLViewerTexture> mOrigDiffTexture;
// [/SL:KB]

protected:
    static bool sSafeRenderSelect;

public:
    struct CompareDistanceGreater
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            return !lhs || (rhs && (lhs->row().distance > rhs->row().distance)); // farthest = first
        }
    };

    struct CompareTexture
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            return lhs->getTexture() < rhs->getTexture();
        }
    };

    struct CompareBatchBreaker
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            const LLTextureEntry* lte = lhs->getTextureEntry();
            const LLTextureEntry* rte = rhs->getTextureEntry();

            if(lhs->getTexture() != rhs->getTexture())
            {
                return lhs->getTexture() < rhs->getTexture();
            }
            else
            {
                return lte->getBumpShinyFullbright() < rte->getBumpShinyFullbright();
            }
        }
    };

    struct CompareTextureAndGeomCount
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            return lhs->getTexture() == rhs->getTexture() ?
                lhs->getGeomCount() < rhs->getGeomCount() :  //smallest = first
                lhs->getTexture() > rhs->getTexture();
        }
    };

    struct CompareTextureAndLOD
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            return lhs->getTexture() == rhs->getTexture() ?
                lhs->getLOD() < rhs->getLOD() :
                lhs->getTexture() < rhs->getTexture();
        }
    };

    struct CompareTextureAndTime
    {
        bool operator()(const LLFace* const& lhs, const LLFace* const& rhs)
        {
            return lhs->getTexture() == rhs->getTexture() ?
                lhs->row().lastUpdateTime < rhs->row().lastUpdateTime :
                lhs->getTexture() < rhs->getTexture();
        }
    };
};

#endif // LL_LLFACE_H
