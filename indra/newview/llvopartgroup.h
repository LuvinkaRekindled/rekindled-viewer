/**
 * @file llvopartgroup.h
 * @brief Group of particle systems
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

#ifndef LL_LLVOPARTGROUP_H
#define LL_LLVOPARTGROUP_H

#include "llviewerobject.h"
#include "v3math.h"
#include "v3color.h"
#include "llframetimer.h"
#include "llviewerpartsim.h"
#include "llvertexbuffer.h"

class LLViewerPartGroup;

// <FS> GPU particles stage 1b / 2: one particle that is drawn this frame, ready for its row (position, velocity of a follow-velocity particle, half size, colour, glow, what the draw batches by); a ribbon
// piece also has the half axis of its own end and the point and half axis of the end it joins
struct LLPartEval
{
    LLVector3 mPos;
    LLVector3 mVel;
    LLVector3 mAxisHalf, mPPos, mPAxisHalf;
    F32 mHalfX = 0.f, mHalfY = 0.f;
    F32 mDist = 0.f;                // along the camera's at axis
    LLColor4U mColor, mGlow, mPColor, mPGlow;
    LLViewerTexture* mTex = nullptr;
    S32 mSlot = -1;                 // G0: the slot of the record in the group's store (-1: a stepped particle)
    S32 mIndex = 0;                 // the pick number: a place in the group's particle list, then in its records
    U8 mBlendSrc = 0, mBlendDst = 0;
    bool mFullbright = false;
    bool mRibbon = false;
};

class LLVOPartGroup : public LLAlphaObject
{
public:

    static void initClass();
    static void restoreResources();
    static void destroyResources();

    enum
    {
        // G0: the record store of a group (llviewerpartsim.cpp, fs_store_write): the arrays hold the fields of a record
        RECORD_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_TANGENT |
                            LLVertexBuffer::MAP_WEIGHT4 |
                            LLVertexBuffer::MAP_TEXCOORD0 |
                            LLVertexBuffer::MAP_TEXCOORD1 |
                            LLVertexBuffer::MAP_TEXCOORD2 |
                            LLVertexBuffer::MAP_COLOR |
                            LLVertexBuffer::MAP_EMISSIVE,
        VERTEX_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_TEXCOORD0 |
                            LLVertexBuffer::MAP_COLOR |
                            LLVertexBuffer::MAP_EMISSIVE |
                            LLVertexBuffer::MAP_TEXTURE_INDEX
    };

    LLVOPartGroup(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);

    /*virtual*/ bool    isActive() const; // Whether this object needs to do an idleUpdate.
    void idleUpdate(LLAgent &agent, const F64 &time);

    virtual F32 getBinRadius();
    virtual void updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax);
    virtual U32 getPartitionType() const;

    /*virtual*/ bool lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                          S32 face,
                                          bool pick_transparent,
                                          bool pick_rigged,
                                          bool pick_unselectable,
                                          S32* face_hit,
                                          LLVector4a* intersection,
                                          LLVector2* tex_coord,
                                          LLVector4a* normal,
                                          LLVector4a* tangent);

    /*virtual*/ void setPixelAreaAndAngle(LLAgent &agent);
    /*virtual*/ void updateTextures();

    /*virtual*/ LLDrawable* createDrawable(LLPipeline *pipeline);
    /*virtual*/ bool        updateGeometry(LLDrawable *drawable);
    void        getGeometry(const LLViewerPart& part,
                                LLStrider<LLVector4a>& verticesp);

                void        getGeometry(S32 idx,
                                LLStrider<LLVector4a>& verticesp,
                                LLStrider<LLVector3>& normalsp,
                                LLStrider<LLVector2>& texcoordsp,
                                LLStrider<LLColor4U>& colorsp,
                                LLStrider<LLColor4U>& emissivep,
                                LLStrider<U16>& indicesp);

    void updateFaceSize(S32 idx) { }
    F32 getPartSize(S32 idx);
    void getBlendFunc(S32 idx, LLRender::eBlendFactor& src, LLRender::eBlendFactor& dst);
    LLUUID getPartOwner(S32 idx);
    LLUUID getPartSource(S32 idx);

    void setViewerPartGroup(LLViewerPartGroup *part_groupp)     { mViewerPartGroupp = part_groupp; }
    LLViewerPartGroup* getViewerPartGroup() { return mViewerPartGroupp; }

    // <FS> GPU particles stage 1b (doc/vulkan/PLAN-gpu-particles.md): in rows mode the drawable has no face a particle: mVisible holds the indices of the particles that are drawn (after the size and
    // budget cuts of updateGeometry), the faces are one a distinct texture (they carry the texture to the texture priority loop). A group the rows path cannot build gets its faces from
    // updateFacesLegacy (the quads path).
    bool updateRows(LLDrawable* drawable, S32 num_parts);
    bool updateFacesLegacy(LLDrawable* drawable, S32 num_parts);
    bool mRowsMode = false;
    bool mPure = false;             // G0: every particle of the group is a record and the store holds them (updateRows)
    std::vector<LLPartEval> mEval;
    LLVector3 mEvalMin, mEvalMax;           // the box of the drawn particles (the group's extents take it in: a record does not stay in the group's box)
    bool mEvalValid = false;

protected:
    ~LLVOPartGroup();

    LLViewerPartGroup *mViewerPartGroupp;

    virtual LLVector3 getCameraPosition() const;

};


class LLVOHUDPartGroup : public LLVOPartGroup
{
public:
    LLVOHUDPartGroup(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp) :
      LLVOPartGroup(id, pcode, regionp)
    {
    }
protected:
    LLDrawable* createDrawable(LLPipeline *pipeline);
    U32 getPartitionType() const;
    virtual LLVector3 getCameraPosition() const;
};

#endif // LL_LLVOPARTGROUP_H
