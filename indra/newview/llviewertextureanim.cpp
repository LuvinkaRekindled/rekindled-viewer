/**
 * @file llviewertextureanim.cpp
 * @brief LLViewerTextureAnim class implementation
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

#include "llviewerprecompiledheaders.h"

#include "llviewertextureanim.h"
#include "llvovolume.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"
#include "lltextureentry.h"
#include <algorithm>
#include <chrono>
#include "rkvkbridge.h"
#include "rkvktexanim.h"

#include "llmath.h"
#include "llerror.h"

std::vector<LLViewerTextureAnim*> LLViewerTextureAnim::sInstanceList;
std::vector<LLViewerTextureAnim*> LLViewerTextureAnim::sDirtyList;

LLViewerTextureAnim::LLViewerTextureAnim(LLVOVolume* vobj) : LLTextureAnim()
{
    mVObj = vobj;
    mLastFrame = -1.f;  // Force an update initially
    mLastTime = 0.f;
    mOffS = mOffT = 0;
    mScaleS = mScaleT = 1;
    mRot = 0;

    mInstanceIndex = static_cast<S32>(sInstanceList.size());
    sInstanceList.push_back(this);
    markDirty();        // <FS> a new animation: its parameters are made at the next updateClass
}

LLViewerTextureAnim::~LLViewerTextureAnim()
{
    if (mDirty)
    {
        sDirtyList.erase(std::remove(sDirtyList.begin(), sDirtyList.end(), this), sDirtyList.end());
        mDirty = false;
    }
    S32 end_idx = static_cast<S32>(sInstanceList.size()) - 1;

    if (end_idx != mInstanceIndex)
    {
        sInstanceList[mInstanceIndex] = sInstanceList[end_idx];
        sInstanceList[mInstanceIndex]->mInstanceIndex = mInstanceIndex;
    }

    sInstanceList.pop_back();
}

void LLViewerTextureAnim::reset()
{
    LLTextureAnim::reset();
    mTimer.reset();
}

//static
// <FS> true when a camera other than the main view saw the object's spatial group in the last few frames: the near sun shadow cascades (its shadow falls into view) and the water
// reflection cameras (it shows in a reflection). Those frames are drawn with the object's current texture animation, so it keeps animating.
static bool seen_by_other_cameras(LLDrawable* drawable)
{
    LLSpatialGroup* group = drawable->getSpatialGroup();
    if (!group)
    {
        return true;        // not in a group (yet): treat as seen
    }
    const S32 now = LLDrawable::getCurrentFrame();
    constexpr S32 RANGE = 4;
    static const LLViewerCamera::eCameraID ids[] = { LLViewerCamera::CAMERA_SUN_SHADOW0, LLViewerCamera::CAMERA_SUN_SHADOW1, LLViewerCamera::CAMERA_WATER0, LLViewerCamera::CAMERA_WATER1 };
    for (LLViewerCamera::eCameraID id : ids)
    {
        if (now - group->getVisible(id) < RANGE)
        {
            return true;
        }
    }
    return false;
}

LLViewerTextureAnim::Stats LLViewerTextureAnim::sStats;

static void report_texture_anim(LLViewerTextureAnim::Stats& st, S32 mode)
{
    if (st.mFrames == 0)
    {
        return;
    }
    LL_INFOS("TexAnim") << "texture animation, " << st.mFrames << " frames, " << (mode == 1 ? "shader" : (mode == 2 ? "main thread, compared" : "main thread")) << ": update loop " << (double)st.mNs / 1.0e6 / st.mFrames << " ms a frame, animations "
        << st.mInstances / st.mFrames << " a frame (" << st.mHidden / st.mFrames << " not in view), main thread calls " << st.mCpuCalls / st.mFrames << ", shader path checks " << st.mGpuChecked / st.mFrames
        << ", faces on the shader path " << st.mGpuFaces / st.mFrames << ", small faces left alone " << st.mSmallFaces / st.mFrames << ", parameter writes " << st.mGpuWrites
        << "; texture entry changes seen (all animations made dirty) " << st.mEpochMoves << ", verify: " << st.mVerifyChecked << " checks, " << st.mVerifyMissed << " found stale (a missed event)"
        << "; compare: " << st.mCompared << " faces, " << st.mMismatched << " differ by more than 0.001, largest difference " << st.mMaxDiff << LL_ENDL;
    st = LLViewerTextureAnim::Stats();
}

void LLViewerTextureAnim::updateClass()
{
    // <FS> an object that is not in view does not need its animated texture advanced this frame: the frame counter is a function of the timer (and the smooth
    // modes accumulate the time since their last call), so the next call that happens brings it to the right state. Seen again, it can be one frame late at most.
    static LLCachedControl<bool> skip_hidden(gSavedSettings, "RKSkipHiddenTextureAnim", true);
    static LLCachedControl<bool> on_gpu(gSavedSettings, "RKTexAnimOnGPU", true);
    const bool skip = skip_hidden;
    // <FS> RKTexAnimOnGPU (rkvktexanim.h): 1 = the faces' matrix slots hold the animation's parameters and the vertex shader makes the matrices against this clock; 0 = the matrices
    // are made here; 2 = as 0, with the shader's formula compared against them. rt-debug.txt texanim=N decides while it is set (a same-lap A/B), the setting otherwise.
    RKVkTexAnim::setClock(LLFrameTimer::getElapsedSeconds());
    const S32 live = RKVkBridge::rtTexAnim();
    const S32 mode = live >= 0 ? llmin(live, 2) : (on_gpu ? 1 : 0);
    const auto update_start = std::chrono::steady_clock::now();
    Stats& st = sStats;
    static S32 last_mode = mode;
    if (mode != last_mode)
    {
        report_texture_anim(st, last_mode);         // (a report is always of one mode)
        last_mode = mode;
    }
    ++st.mFrames;

    // <FS> 2026-11-28: on the GPU path only the animations whose parameters may have changed are visited (see markDirty). The old visit of every animation every frame is kept for the other modes. An animation in a cold
    // group (the expiry modes of the pipeline, on by default) is skipped by animateTexturesGpu and stays dirty, so it is tried again every frame until its group is warm, as before. RK_VK_TEXANIM_VERIFY=1: every clean
    // animation in view is also checked, read only, against what its parameters would be now; one that differs is a change no event reported (logged, counted in the TexAnim line, and made dirty so the picture is right).
    static bool event_mode = false;
    static U32 last_epoch = 0;
    static const bool verify = getenv("RK_VK_TEXANIM_VERIFY") != nullptr;
    if (mode == 1)
    {
        if (!event_mode || last_epoch != LLTextureEntry::sChangeEpoch)
        {
            if (event_mode)
            {
                ++st.mEpochMoves;
            }
            for (LLViewerTextureAnim* anim : sInstanceList)
            {
                anim->markDirty();
            }
        }
        event_mode = true;
        st.mInstances += (U32)sInstanceList.size();
        const size_t n = sDirtyList.size();
        size_t keep = 0;
        for (size_t k = 0; k < n; ++k)
        {
            LLViewerTextureAnim* anim = sDirtyList[k];
            LLVOVolume* vobj = anim->mVObj;
            bool done = false;
            if (skip && vobj->mDrawable.notNull() && !vobj->mDrawable->isVisible() && !seen_by_other_cameras(vobj->mDrawable))
            {
                ++st.mHidden;       // (stays dirty: written when it is in view)
            }
            else
            {
                ++st.mGpuChecked;
                done = vobj->animateTexturesGpu(true);
            }
            if (done)
            {
                anim->mDirty = false;
            }
            else
            {
                sDirtyList[keep++] = anim;
            }
        }
        sDirtyList.erase(sDirtyList.begin() + keep, sDirtyList.begin() + n);        // (what a visit added to the list stays)
        if (verify)
        {
            static U32 reported = 0;
            for (LLViewerTextureAnim* anim : sInstanceList)
            {
                LLVOVolume* vobj = anim->mVObj;
                if (anim->mDirty || (skip && vobj->mDrawable.notNull() && !vobj->mDrawable->isVisible() && !seen_by_other_cameras(vobj->mDrawable)))
                {
                    continue;
                }
                ++st.mVerifyChecked;
                if (vobj->texAnimGpuStale())
                {
                    ++st.mVerifyMissed;
                    if (reported++ < 20)
                    {
                        LL_WARNS("TexAnim") << "texture animation event missed: object " << vobj->getID() << ", mode " << (U32)anim->mMode << ", rate " << anim->mRate << ", face " << (S32)anim->mFace << ", faces "
                            << (vobj->mDrawable.notNull() ? vobj->mDrawable->getNumFaces() : -1) << ", texture entry epoch " << LLTextureEntry::sChangeEpoch << LL_ENDL;
                    }
                    anim->markDirty();
                }
            }
        }
        last_epoch = LLTextureEntry::sChangeEpoch;      // (after the visits: the entries a visit changed itself, an animation that bakes its last state, are not a reason to visit all again)
        st.mNs += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
        if (st.mFrames >= 600)
        {
            report_texture_anim(st, mode);
        }
        return;
    }
    event_mode = false;

    for (std::vector<LLViewerTextureAnim*>::iterator iter = sInstanceList.begin(); iter != sInstanceList.end(); ++iter)
    {
        LLViewerTextureAnim* anim = *iter;
        LLVOVolume* vobj = anim->mVObj;
        ++st.mInstances;
        if (skip && vobj->mDrawable.notNull() && !vobj->mDrawable->isVisible() && !seen_by_other_cameras(vobj->mDrawable))
        {
            ++st.mHidden;
            continue;
        }
        if (mode == 1)
        {
            ++st.mGpuChecked;
            vobj->animateTexturesGpu();
            continue;
        }
        if (anim->mGpuWritten)
        {
            // the matrices of this animation's faces hold parameters: made matrices again, and written again at the next frame change
            vobj->resolveTexAnimMatrices();
            anim->mGpuWritten = false;
            anim->forceRecalc();
        }
        ++st.mCpuCalls;
        vobj->animateTextures();
        if (mode == 2)
        {
            vobj->compareTexAnimGpu();
        }
    }
    st.mNs += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - update_start).count();
    if (st.mFrames >= 600)
    {
        report_texture_anim(st, mode);
    }
}

S32 LLViewerTextureAnim::animateTextures(F32 &off_s, F32 &off_t,
                                        F32 &scale_s, F32 &scale_t,
                                        F32 &rot)
{
    S32 result = 0;
    if (!(mMode & ON))
    {
        mLastTime = 0.f;
        mLastFrame = -1.f;
        return result;
    }


    F32 num_frames = 1.0;
    F32 full_length = 1.0;

    if (mLength)
    {
        num_frames = mLength;
    }
    else
    {
        num_frames = llmax(1.f, (F32)(mSizeX * mSizeY));
    }

    if (mMode & PING_PONG)
    {
        if (mMode & SMOOTH)
        {
            full_length = 2.f*num_frames;
        }
        else if (mMode & LOOP)
        {
            full_length = 2.f*num_frames - 2.f;
            full_length = llmax(1.f, full_length);
        }
        else
        {
            full_length = 2.f*num_frames - 1.f;
            full_length = llmax(1.f, full_length);
        }
    }
    else
    {
        full_length = num_frames;
    }


    F32 frame_counter;
    if (mMode & SMOOTH)
    {
        frame_counter = mTimer.getElapsedTimeAndResetF32() * mRate + (F32)mLastTime;
    }
    else
    {
        frame_counter = mTimer.getElapsedTimeF32() * mRate;
    }
    mLastTime = frame_counter;

    if (mMode & LOOP)
    {
        frame_counter  = fmod(frame_counter, full_length);
    }
    else
    {
        frame_counter = llmin(full_length - 1.f, frame_counter);
    }

    if (!(mMode & SMOOTH))
    {
        frame_counter = (F32)llfloor(frame_counter + 0.01f);
        // account for 0.01, we shouldn't step over full length
        frame_counter = llmin(full_length - 1.f, frame_counter);
    }

    if (mMode & PING_PONG)
    {
        if (frame_counter >= num_frames)
        {
            if (mMode & SMOOTH)
            {
                frame_counter = num_frames - (frame_counter - num_frames);
            }
            else
            {
                frame_counter = (num_frames - 1.99f) - (frame_counter - num_frames);
            }
        }
    }

    if (mMode & REVERSE)
    {
        if (mMode & SMOOTH)
        {
            frame_counter = num_frames - frame_counter;
        }
        else
        {
            frame_counter = (num_frames - 0.99f) - frame_counter;
        }
    }

    frame_counter += mStart;

    if (!(mMode & SMOOTH))
    {
        frame_counter = (F32)ll_round(frame_counter);
    }

    //
    // Now that we've calculated the frame time, do an update.
    // Will we correctly update stuff if the texture anim has
    // changed, but not the frame counter?
    //
    if (mLastFrame != frame_counter)
    {
        mLastFrame = frame_counter;
        if (mMode & ROTATE)
        {
            result |= ROTATE;
            mRot = rot = frame_counter;
        }
        else if (mMode & SCALE)
        {
            result |= SCALE;
            mScaleS = scale_s = frame_counter;
            mScaleT = scale_t = frame_counter;
        }
        else
        {
            result |= TRANSLATE;
            F32 x_frame;
            S32 y_frame;
            F32 x_pos;
            F32 y_pos;

            if (  (mSizeX)
                &&(mSizeY))
            {
                result |= SCALE;
                mScaleS = scale_s = 1.f/mSizeX;
                mScaleT = scale_t = 1.f/mSizeY;
                x_frame = fmodf(frame_counter, mSizeX);
                y_frame = (S32)(frame_counter / mSizeX);
                x_pos = x_frame * scale_s;
                y_pos = y_frame * scale_t;
                mOffS = off_s = (-0.5f + 0.5f*scale_s)+ x_pos;
                mOffT = off_t = (0.5f - 0.5f*scale_t) - y_pos;
            }
            else
            {
                mScaleS = scale_s = 1.f;
                mScaleT = scale_t = 1.f;
                x_pos = frame_counter * scale_s;
                mOffS = off_s = (-0.5f + 0.5f*scale_s)+ x_pos;
                mOffT = off_t = 0.f;
            }
        }
    }
    return result;
}
