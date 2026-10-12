/**
 * @file llviewerjointmesh.h
 * @brief Declaration of LLViewerJointMesh class
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

#ifndef LL_LLVIEWERJOINTMESH_H
#define LL_LLVIEWERJOINTMESH_H

#include "llviewerjoint.h"
#include "llviewertexture.h"
#include "llavatarjointmesh.h"
#include "llpolymesh.h"
#include "v4color.h"
#include <vector>

class LLDrawable;
class LLFace;
class LLCharacter;
class LLViewerTexLayerSet;

//-----------------------------------------------------------------------------
// class LLViewerJointMesh
//-----------------------------------------------------------------------------
class LLViewerJointMesh : public LLAvatarJointMesh, public LLViewerJoint
{
public:
    // Constructor
    LLViewerJointMesh();

    // Destructor
    virtual ~LLViewerJointMesh();

    // Render time method to upload batches of joint matrices
    void uploadJointMatrices();
    // the palette uploadJointMatrices sends (3 rows x 15 joints of 4 floats), with or without the current modelview (the ray scene wants world space)
    void computeJointPalette(F32* mat, bool with_modelview);
    // ray tracing: names this mesh (once per frame) to the ray scene, which skins it on the GPU
    void noteRayShadow();
    // motion vectors: the world space palette (the layout of computeJointPalette) of this frame and of the frame before. Computed once per frame, on the first call of the frame.
    // The previous palette equals the current one when there is no history (first time, or the mesh was not asked for in the last frame): no motion.
    void updatePaletteHistory();
    const F32* getWorldPalette() const { return mWorldPalette; }
    const F32* getPreviousWorldPalette() const { return mPrevWorldPalette; }
    bool hasPreviousWorldPalette() const { return mPaletteHasPrev; }
    // motion vectors: the body meshes the main view's G-buffer drew this frame (recorded in drawShape while sRecordVelocity is set), drawn again by the avatar velocity pass
    struct VelocityDraw
    {
        class LLVertexBuffer* mBuffer;
        U32 mStart, mEnd, mCount, mOffset;
        const LLViewerJointMesh* mMesh;
        class LLVOAvatar* mAvatar;
    };
    static bool sRecordVelocity;
    static std::vector<VelocityDraw> sVelocityDraws;

    // overloaded from base class
    U32 drawShape( F32 pixelArea, bool first_pass, bool is_dummy ) override;

    // necessary because MS's compiler warns on function inheritance via dominance in the diamond inheritance here.
    // warns even though LLViewerJoint holds the only non virtual implementation.
    U32 render(F32 pixelArea, bool first_pass = true, bool is_dummy = false) override { return LLViewerJoint::render(pixelArea, first_pass, is_dummy); }

    void updateFaceSizes(U32 &num_vertices, U32& num_indices, F32 pixel_area) override;
    void updateFaceData(LLFace *face, F32 pixel_area, bool damp_wind = false, bool terse_update = false) override;
    bool updateLOD(F32 pixel_area, bool activate) override;
    void updateJointGeometry() override;
    void dump() override;

    bool isAnimatable() const override { return false; }

private:
    F32 mWorldPalette[45 * 4] = {};
    F32 mPrevWorldPalette[45 * 4] = {};
    U32 mPaletteFrame = 0;
    bool mPaletteComputed = false;
    bool mPaletteHasPrev = false;
};

#endif // LL_LLVIEWERJOINTMESH_H
