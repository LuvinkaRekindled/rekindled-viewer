/**
 * @file xform.h
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

#ifndef LL_XFORM_H
#define LL_XFORM_H

#include "v3math.h"
#include "m4math.h"
#include "llquaternion.h"
// <AW: opensim-limits>
//constexpr F32 MAX_OBJECT_Z        = 4096.f; // should match REGION_HEIGHT_METERS, Pre-havok4: 768.f
//constexpr F32 DEFAULT_MAX_PRIM_SCALE = 64.f;
//const F32 DEFAULT_MAX_PRIM_SCALE_NO_MESH = 10.f;
//constexpr F32 MIN_PRIM_SCALE = 0.01f;
//constexpr F32 MAX_PRIM_SCALE = 65536.f;   // something very high but not near FLT_MAX
constexpr F32 SL_MAX_OBJECT_Z       = 4096.f;
constexpr F32 SL_MIN_OBJECT_Z       = 0.f;
constexpr F32 SL_DEFAULT_MAX_PRIM_SCALE = 64.f;
constexpr F32 SL_DEFAULT_MAX_PRIM_SCALE_NO_MESH = 10.f;
constexpr F32 SL_MIN_PRIM_SCALE = 0.01f;
constexpr F32 SL_MAX_PRIM_SCALE = 65536.f;  // something very high but not near FLT_MAX

constexpr F32 OS_MAX_OBJECT_Z       = 10000.f;
constexpr F32 OS_MIN_OBJECT_Z       = -10000.f;
constexpr F32 OS_DEFAULT_MAX_PRIM_SCALE = 256.f;
constexpr F32 OS_MIN_PRIM_SCALE = 0.001f;
constexpr F32 OS_MAX_PRIM_SCALE = 65536.f;  // something very high but not near FLT_MAX
// <AW: opensim-limits>
// <FS> PLAN-avatars.md stage C: the local position, rotation and scale of a joint's xform as one slot of the avatar's flat array (the array is where they live; the xform's own fields are not used while the slot is set)
struct RKXformLocals
{
    LLVector3    mPos;
    LLQuaternion mRot;
    LLVector3    mScale;
};

class LLXform
{
protected:
    LLVector3     mPosition;
    LLQuaternion  mRotation;
    LLVector3     mScale;
    // the local data: the slot of the avatar's flat array while this is a joint's xform bound to one, else the fields
    LLVector3&          lpos()          { return mLocals ? mLocals->mPos : mPosition; }
    const LLVector3&    lpos() const    { return mLocals ? mLocals->mPos : mPosition; }
    LLQuaternion&       lrot()          { return mLocals ? mLocals->mRot : mRotation; }
    const LLQuaternion& lrot() const    { return mLocals ? mLocals->mRot : mRotation; }
    LLVector3&          lscale()        { return mLocals ? mLocals->mScale : mScale; }
    const LLVector3&    lscale() const  { return mLocals ? mLocals->mScale : mScale; }

    //RN: TODO: move these world transform members to LLXformMatrix
    // as they are *never* updated or accessed in the base class
    LLVector3     mWorldPosition;
    LLQuaternion  mWorldRotation;

    LLXform*      mParent;
    U32           mChanged;

    bool          mScaleChildOffset;
    RKXformLocals*      mLocals = nullptr;      // <FS> stage C: when set, the local position, rotation and scale are this slot's (the setters and getters follow it)
    const LLVector3*    mViewWorldPos = nullptr;
    const LLQuaternion* mViewWorldRot = nullptr;

public:
    typedef enum e_changed_flags
    {
        UNCHANGED   = 0x00,
        TRANSLATED  = 0x01,
        ROTATED     = 0x02,
        SCALED      = 0x04,
        SHIFTED     = 0x08,
        GEOMETRY    = 0x10,
        TEXTURE     = 0x20,
        MOVED       = TRANSLATED|ROTATED|SCALED,
        SILHOUETTE  = 0x40,
        ALL_CHANGED = 0x7f
    }EChangedFlags;

    void init()
    {
        mParent  = NULL;
        mChanged = UNCHANGED;
        mPosition.setVec(0,0,0);
        mRotation.loadIdentity();
        mScale.   setVec(1,1,1);
        mWorldPosition.clearVec();
        mWorldRotation.loadIdentity();
        mScaleChildOffset = false;
        mViewWorldPos = nullptr;
        mViewWorldRot = nullptr;
        mLocals = nullptr;
    }

     LLXform();
    virtual ~LLXform();

    void getLocalMat4(LLMatrix4 &mat) const { mat.initAll(lscale(), lrot(), lpos()); }

    inline bool setParent(LLXform *parent);

    inline void setPosition(const LLVector3& pos);
    inline void setPosition(const F32 x, const F32 y, const F32 z);
    inline void setPositionX(const F32 x);
    inline void setPositionY(const F32 y);
    inline void setPositionZ(const F32 z);
    inline void addPosition(const LLVector3& pos);


    inline void setScale(const LLVector3& scale);
    inline void setScale(const F32 x, const F32 y, const F32 z);
    inline void setRotation(const LLQuaternion& rot);
    inline void setRotation(const F32 x, const F32 y, const F32 z);
    inline void setRotation(const F32 x, const F32 y, const F32 z, const F32 s);

    // Above functions must be inline for speed, but also
    // need to emit warnings.  LL_WARNS() causes inline LLError::CallSite
    // static objects that make more work for the linker.
    // Avoid inline LL_WARNS() by calling this function.
    void warn(const char* const msg);

    void        setChanged(const U32 bits)                  { mChanged |= bits; }
    bool        isChanged() const                           { return mChanged; }
    bool        isChanged(const U32 bits) const             { return mChanged & bits; }
    void        clearChanged()                              { mChanged = 0; }
    void        clearChanged(U32 bits)                      { mChanged &= ~bits; }

    void        setScaleChildOffset(bool scale)             { mScaleChildOffset = scale; }
    bool        getScaleChildOffset() const                 { return mScaleChildOffset; }

    LLXform* getParent() const { return mParent; }
    LLXform* getRoot() const;
    virtual bool isRoot() const;
    virtual bool isRootEdit() const;

    const LLVector3&    getPosition()  const        { return lpos(); }
    const LLVector3&    getScale() const            { return lscale(); }
    const LLQuaternion& getRotation() const         { return lrot(); }
    const LLVector3&    getPositionW() const        { return mViewWorldPos ? *mViewWorldPos : mWorldPosition; }
    const LLQuaternion& getWorldRotation() const    { return mViewWorldRot ? *mViewWorldRot : mWorldRotation; }
    const LLVector3&    getWorldPosition() const    { return mViewWorldPos ? *mViewWorldPos : mWorldPosition; }
    // <FS> PLAN-avatars.md stage C: a joint's xform while the joint is a view of the avatar's flat world array: the world position and rotation are the array's slot (set by the avatar's pass, cleared by LLJoint::touch)
    void                setLocals(RKXformLocals* slot) { mLocals = slot; }      // (the caller copies the values over first)
    RKXformLocals*      getLocals() const { return mLocals; }
    void                releaseLocals() { if (mLocals) { mPosition = mLocals->mPos; mRotation = mLocals->mRot; mScale = mLocals->mScale; mLocals = nullptr; } }      // (the fields get the last values back)
    void                setWorldView(const LLVector3* pos, const LLQuaternion* rot) { mViewWorldPos = pos; mViewWorldRot = rot; }
};

class LLXformMatrix : public LLXform
{
public:
    LLXformMatrix() : LLXform() {};
    virtual ~LLXformMatrix();

    const LLMatrix4&    getWorldMatrix() const      { return mWorldMatrix; }
    void setWorldMatrix (const LLMatrix4& mat)   { mWorldMatrix = mat; }

    void init()
    {
        mWorldMatrix.setIdentity();
        mMin.clearVec();
        mMax.clearVec();

        LLXform::init();
    }

    void update();
    void updateMatrix(bool update_bounds = true);
    void updateMatrixFromParent(const LLVector3& parent_pos, const LLQuaternion& parent_rot, const LLVector3& parent_scale, bool parent_scale_child_offset, bool update_bounds = true);     // <FS> stage C: updateMatrix with the parent's world data given
    void finishMatrix(bool update_bounds);
    void getMinMax(LLVector3& min,LLVector3& max) const;

protected:
    LLMatrix4   mWorldMatrix;
    LLVector3   mMin;
    LLVector3   mMax;

};

bool LLXform::setParent(LLXform* parent)
{
    // Validate and make sure we're not creating a loop
    if (parent == mParent)
    {
        return true;
    }
    if (parent)
    {
        LLXform *cur_par = parent->mParent;
        while (cur_par)
        {
            if (cur_par == this)
            {
                //warn("LLXform::setParent Creating loop when setting parent!");
                return false;
            }
            cur_par = cur_par->mParent;
        }
    }
    mParent = parent;
    return true;
}

void LLXform::setPosition(const LLVector3& pos)
{
    setChanged(TRANSLATED);
    if (pos.isFinite())
        lpos() = pos;
    else
    {
        lpos().clearVec();
        warn("Non Finite in LLXform::setPosition(LLVector3)");
    }
}

void LLXform::setPosition(const F32 x, const F32 y, const F32 z)
{
    setChanged(TRANSLATED);
    if (llfinite(x) && llfinite(y) && llfinite(z))
        lpos().setVec(x,y,z);
    else
    {
        lpos().clearVec();
        warn("Non Finite in LLXform::setPosition(F32,F32,F32)");
    }
}

void LLXform::setPositionX(const F32 x)
{
    setChanged(TRANSLATED);
    if (llfinite(x))
        lpos().mV[VX] = x;
    else
    {
        lpos().mV[VX] = 0.f;
        warn("Non Finite in LLXform::setPositionX");
    }
}

void LLXform::setPositionY(const F32 y)
{
    setChanged(TRANSLATED);
    if (llfinite(y))
        lpos().mV[VY] = y;
    else
    {
        lpos().mV[VY] = 0.f;
        warn("Non Finite in LLXform::setPositionY");
    }
}

void LLXform::setPositionZ(const F32 z)
{
    setChanged(TRANSLATED);
    if (llfinite(z))
        lpos().mV[VZ] = z;
    else
    {
        lpos().mV[VZ] = 0.f;
        warn("Non Finite in LLXform::setPositionZ");
    }
}

void LLXform::addPosition(const LLVector3& pos)
{
    setChanged(TRANSLATED);
    if (pos.isFinite())
        lpos() += pos;
    else
        warn("Non Finite in LLXform::addPosition");
}

void LLXform::setScale(const LLVector3& scale)
{
    setChanged(SCALED);
    if (scale.isFinite())
        lscale() = scale;
    else
    {
        lscale().setVec(1.f, 1.f, 1.f);
        warn("Non Finite in LLXform::setScale");
    }
}
void LLXform::setScale(const F32 x, const F32 y, const F32 z)
{
    setChanged(SCALED);
    if (llfinite(x) && llfinite(y) && llfinite(z))
        lscale().setVec(x,y,z);
    else
    {
        lscale().setVec(1.f, 1.f, 1.f);
        warn("Non Finite in LLXform::setScale");
    }
}
void LLXform::setRotation(const LLQuaternion& rot)
{
    setChanged(ROTATED);
    if (rot.isFinite())
        lrot() = rot;
    else
    {
        lrot().loadIdentity();
        warn("Non Finite in LLXform::setRotation");
    }
}
void LLXform::setRotation(const F32 x, const F32 y, const F32 z)
{
    setChanged(ROTATED);
    if (llfinite(x) && llfinite(y) && llfinite(z))
    {
        lrot().setQuat(x,y,z);
    }
    else
    {
        lrot().loadIdentity();
        warn("Non Finite in LLXform::setRotation");
    }
}
void LLXform::setRotation(const F32 x, const F32 y, const F32 z, const F32 s)
{
    setChanged(ROTATED);
    if (llfinite(x) && llfinite(y) && llfinite(z) && llfinite(s))
    {
        lrot().mQ[VX] = x; lrot().mQ[VY] = y; lrot().mQ[VZ] = z; lrot().mQ[VS] = s;
    }
    else
    {
        lrot().loadIdentity();
        warn("Non Finite in LLXform::setRotation");
    }
}

#endif
