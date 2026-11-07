/**
 * @file xform.cpp
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

#include "linden_common.h"

#include "xform.h"

LLXform::LLXform()
{
    init();
}

LLXform::~LLXform()
{
}

// Link optimization - don't inline these LL_WARNS()
void LLXform::warn(const char* const msg)
{
    LL_WARNS() << msg << LL_ENDL;
}

LLXform* LLXform::getRoot() const
{
    const LLXform* root = this;
    while(root->mParent)
    {
        root = root->mParent;
    }
    return (LLXform*)root;
}

bool LLXform::isRoot() const
{
    return (!mParent);
}

bool LLXform::isRootEdit() const
{
    return (!mParent);
}

LLXformMatrix::~LLXformMatrix()
{
}

void LLXformMatrix::update()
{
    if (mParent)
    {
        mWorldPosition = lpos();
        if (mParent->getScaleChildOffset())
        {
            mWorldPosition.scaleVec(mParent->getScale());
        }
        mWorldPosition *= mParent->getWorldRotation();
        mWorldPosition += mParent->getWorldPosition();
        mWorldRotation = lrot() * mParent->getWorldRotation();
    }
    else
    {
        mWorldPosition = lpos();
        mWorldRotation = lrot();
    }
}

// <FS> PLAN-avatars.md stage C: update() with the parent's world position, rotation, scale and scale-child-offset given (from the avatar's flat world array: the parent xform is not read), then the matrix and the
// bounds of updateMatrix(update_bounds)
void LLXformMatrix::updateMatrixFromParent(const LLVector3& parent_pos, const LLQuaternion& parent_rot, const LLVector3& parent_scale, bool parent_scale_child_offset, bool update_bounds)
{
    mWorldPosition = lpos();
    if (parent_scale_child_offset)
    {
        mWorldPosition.scaleVec(parent_scale);
    }
    mWorldPosition *= parent_rot;
    mWorldPosition += parent_pos;
    mWorldRotation = lrot() * parent_rot;
    finishMatrix(update_bounds);
}

void LLXformMatrix::updateMatrix(bool update_bounds)
{
    update();
    finishMatrix(update_bounds);
}

void LLXformMatrix::finishMatrix(bool update_bounds)
{
    mWorldMatrix.initAll(lscale(), mWorldRotation, mWorldPosition);

    if (update_bounds && (mChanged & MOVED))
    {
        mMin.mV[0] = mMax.mV[0] = mWorldMatrix.mMatrix[3][0];
        mMin.mV[1] = mMax.mV[1] = mWorldMatrix.mMatrix[3][1];
        mMin.mV[2] = mMax.mV[2] = mWorldMatrix.mMatrix[3][2];

        F32 f0 = (fabs(mWorldMatrix.mMatrix[0][0])+fabs(mWorldMatrix.mMatrix[1][0])+fabs(mWorldMatrix.mMatrix[2][0])) * 0.5f;
        F32 f1 = (fabs(mWorldMatrix.mMatrix[0][1])+fabs(mWorldMatrix.mMatrix[1][1])+fabs(mWorldMatrix.mMatrix[2][1])) * 0.5f;
        F32 f2 = (fabs(mWorldMatrix.mMatrix[0][2])+fabs(mWorldMatrix.mMatrix[1][2])+fabs(mWorldMatrix.mMatrix[2][2])) * 0.5f;

        mMin.mV[0] -= f0;
        mMin.mV[1] -= f1;
        mMin.mV[2] -= f2;

        mMax.mV[0] += f0;
        mMax.mV[1] += f1;
        mMax.mV[2] += f2;
    }
}

void LLXformMatrix::getMinMax(LLVector3& min, LLVector3& max) const
{
    min = mMin;
    max = mMax;
}
