/**
 * @file llviewertextureanim.h
 * @brief LLViewerTextureAnim class header file
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
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

#ifndef LL_LLVIEWERTEXTUREANIM_H
#define LL_LLVIEWERTEXTUREANIM_H

#include "lltextureanim.h"
#include "llframetimer.h"

class LLVOVolume;

class LLViewerTextureAnim : public LLTextureAnim
{
private:
    static std::vector<LLViewerTextureAnim*> sInstanceList;
    S32 mInstanceIndex;

public:
    static void updateClass();

    LLViewerTextureAnim(LLVOVolume* vobj);
    virtual ~LLViewerTextureAnim();

    /*virtual*/ void reset();

    S32 animateTextures(F32 &off_s, F32 &off_t, F32 &scale_s, F32 &scale_t, F32 &rotate);

    // <FS> RKTexAnimOnGPU (rkvktexanim.h): the origin of the frame counter, in frame time seconds. The counter is (clock - origin) * rate in the non smooth modes; a smooth mode adds
    // the time it has already accumulated (mLastTime, in frames).
    F64 getTimerOrigin() const { return mTimer.startTime() - ((mMode & SMOOTH) && mRate != 0.f ? mLastTime / (F64)mRate : 0.0); }
    void forceRecalc() { mLastFrame = -1.f; }     // (the next animateTextures call returns a result and the faces get their matrices written again)
    bool mGpuWritten = false;                      // this animation's faces were last written by the GPU path (parameters in their matrix slots)

    // <FS> 2026-11-28: on the GPU path the animation's parameters are written when an input of them changed, not by a visit of every animation every frame. Dirty: the parameters have to be made again at the next
    // updateClass. Set by: the animation's creation and every message that changes it (LLVOVolume), a new face of its volume (LLFace::init), and any change of a texture entry's offset, scale or rotation (all animations,
    // LLTextureEntry::sChangeEpoch). An animation that is off or has no rate (the main thread path) stays dirty: it is visited every frame as before.
    void markDirty() { if (!mDirty) { mDirty = true; sDirtyList.push_back(this); } }
    bool mDirty = false;
    static std::vector<LLViewerTextureAnim*> sDirtyList;
    friend class LLVOVolume;

    struct Stats
    {
        U32 mFrames = 0, mInstances = 0, mHidden = 0, mCpuCalls = 0, mGpuChecked = 0, mGpuFaces = 0, mGpuWrites = 0, mSmallFaces = 0, mCompared = 0, mMismatched = 0, mEpochMoves = 0, mVerifyChecked = 0, mVerifyMissed = 0;
        F32 mMaxDiff = 0.f;
        U64 mNs = 0;                    // time spent in updateClass (the whole loop), nanoseconds
    };
    static Stats sStats;
    enum
    {
        TRANSLATE = 0x01 // Result code JUST for animateTextures
    };

    F32 mOffS;
    F32 mOffT;
    F32 mScaleS;
    F32 mScaleT;
    F32 mRot;

protected:
    LLVOVolume* mVObj;
    struct Timer : public LLFrameTimer
    {
        F64 startTime() const { return mStartTime; }
    };
    Timer mTimer;
    F64 mLastTime;
    F32 mLastFrame;
};
#endif
