/**
 * @file llpose.cpp
 * @brief Implementation of LLPose class.
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
#include "linden_common.h"

#include "llpose.h"

#include "llmotion.h"
#include "llmath.h"
#include "llstl.h"
#include <atomic>

//-----------------------------------------------------------------------------
// Static
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// LLPose
//-----------------------------------------------------------------------------
LLPose::~LLPose()
{
}

//-----------------------------------------------------------------------------
// getFirstJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::getFirstJointState()
{
    mListIter = mJointMap.begin();
    if (mListIter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return mListIter->second;
    }
}

//-----------------------------------------------------------------------------
// getNextJointState()
//-----------------------------------------------------------------------------
LLJointState *LLPose::getNextJointState()
{
    mListIter++;
    if (mListIter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return mListIter->second;
    }
}

//-----------------------------------------------------------------------------
// addJointState()
//-----------------------------------------------------------------------------
bool LLPose::addJointState(const LLPointer<LLJointState>& jointState)
{
    if (mJointMap.find(jointState->getJoint()->getName()) == mJointMap.end())
    {
        mJointMap[jointState->getJoint()->getName()] = jointState;
    }
    return true;
}

//-----------------------------------------------------------------------------
// removeJointState()
//-----------------------------------------------------------------------------
bool LLPose::removeJointState(const LLPointer<LLJointState>& jointState)
{
    mJointMap.erase(jointState->getJoint()->getName());
    return true;
}

//-----------------------------------------------------------------------------
// removeAllJointStates()
//-----------------------------------------------------------------------------
bool LLPose::removeAllJointStates()
{
    mJointMap.clear();
    return true;
}

//-----------------------------------------------------------------------------
// findJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::findJointState(LLJoint *joint)
{
    joint_map_iterator iter = mJointMap.find(joint->getName());

    if (iter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return iter->second;
    }
}

//-----------------------------------------------------------------------------
// findJointState()
//-----------------------------------------------------------------------------
LLJointState* LLPose::findJointState(const std::string &name)
{
    joint_map_iterator iter = mJointMap.find(name);

    if (iter == mJointMap.end())
    {
        return NULL;
    }
    else
    {
        return iter->second;
    }
}

//-----------------------------------------------------------------------------
// setWeight()
//-----------------------------------------------------------------------------
void LLPose::setWeight(F32 weight)
{
    joint_map_iterator iter;
    for (joint_map_value_type& joint_pair : mJointMap)
    {
        joint_pair.second->setWeight(weight);
    }
    mWeight = weight;
}

//-----------------------------------------------------------------------------
// getWeight()
//-----------------------------------------------------------------------------
F32 LLPose::getWeight() const
{
    return mWeight;
}

//-----------------------------------------------------------------------------
// getNumJointStates()
//-----------------------------------------------------------------------------
S32 LLPose::getNumJointStates() const
{
    return (S32)mJointMap.size();
}

//-----------------------------------------------------------------------------
// LLJointStateBlender
//-----------------------------------------------------------------------------

LLJointStateBlender::LLJointStateBlender()
{
    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        mJointStates[i] = NULL;
        mPriorities[i] = S32_MIN;
        mAdditiveBlends[i] = false;
    }
}

LLJointStateBlender::~LLJointStateBlender()
{

}

//-----------------------------------------------------------------------------
// addJointState()
//-----------------------------------------------------------------------------
bool LLJointStateBlender::addJointState(const LLPointer<LLJointState>& joint_state, S32 priority, bool additive_blend)
{
    llassert(joint_state);

    if (!joint_state->getJoint())
        // this joint state doesn't point to an actual joint, so we don't care about applying it
        return false;

    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        if (mJointStates[i].isNull())
        {
            mJointStates[i] = joint_state;
            mPriorities[i] = priority;
            mAdditiveBlends[i] = additive_blend;
            return true;
        }
        else if (priority > mPriorities[i])
        {
            // we're at a higher priority than the current joint state in this slot
            // so shift everyone over
            // previous joint states (newer motions) with same priority should stay in place
            for (S32 j = JSB_NUM_JOINT_STATES - 1; j > i; j--)
            {
                mJointStates[j] = mJointStates[j - 1];
                mPriorities[j] = mPriorities[j - 1];
                mAdditiveBlends[j] = mAdditiveBlends[j - 1];
            }
            // now store ourselves in this slot
            mJointStates[i] = joint_state;
            mPriorities[i] = priority;
            mAdditiveBlends[i] = additive_blend;
            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// blendJointStates()
//-----------------------------------------------------------------------------
void LLJointStateBlender::blendJointStates(bool apply_now)
{
    // we need at least one joint to blend
    // if there is one, it will be in slot zero according to insertion logic
    // instead of resetting joint state to default, just leave it unchanged from last frame
    if (mJointStates[0].isNull())
    {
        return;
    }

    LLJoint* target_joint = apply_now ? mJointStates[0]->getJoint() : &mJointCache;

    const S32 POS_WEIGHT = 0;
    const S32 ROT_WEIGHT = 1;
    const S32 SCALE_WEIGHT = 2;

    F32             sum_weights[3];
    U32             sum_usage = 0;

    LLVector3       blended_pos = target_joint->getPosition();
    LLQuaternion    blended_rot = target_joint->getRotation();
    LLVector3       blended_scale = target_joint->getScale();

    LLVector3       added_pos;
    LLQuaternion    added_rot;
    LLVector3       added_scale;

    //S32               joint_state_index;

    sum_weights[POS_WEIGHT] = 0.f;
    sum_weights[ROT_WEIGHT] = 0.f;
    sum_weights[SCALE_WEIGHT] = 0.f;

    for(S32 joint_state_index = 0;
        joint_state_index < JSB_NUM_JOINT_STATES && mJointStates[joint_state_index].notNull();
        joint_state_index++)
    {
        LLJointState* jsp = mJointStates[joint_state_index];
        U32 current_usage = jsp->getUsage();
        F32 current_weight = jsp->getWeight();

        if (current_weight == 0.f)
        {
            continue;
        }

        if (mAdditiveBlends[joint_state_index])
        {
            if(current_usage & LLJointState::POS)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);

                // add in pos for this jointstate modulated by weight
                added_pos += jsp->getPosition() * (new_weight_sum - sum_weights[POS_WEIGHT]);
            }

            if(current_usage & LLJointState::SCALE)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);

                // add in scale for this jointstate modulated by weight
                added_scale += jsp->getScale() * (new_weight_sum - sum_weights[SCALE_WEIGHT]);
            }

            if (current_usage & LLJointState::ROT)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);

                // add in rotation for this jointstate modulated by weight
                added_rot = nlerp((new_weight_sum - sum_weights[ROT_WEIGHT]), added_rot, jsp->getRotation()) * added_rot;
            }
        }
        else
        {
            // blend two jointstates together

            // blend position
            if(current_usage & LLJointState::POS)
            {
                if(sum_usage & LLJointState::POS)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);

                    // blend positions from both
                    blended_pos = lerp(jsp->getPosition(), blended_pos, sum_weights[POS_WEIGHT] / new_weight_sum);
                    sum_weights[POS_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy position from current
                    blended_pos = jsp->getPosition();
                    sum_weights[POS_WEIGHT] = current_weight;
                }
            }

            // now do scale
            if(current_usage & LLJointState::SCALE)
            {
                if(sum_usage & LLJointState::SCALE)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);

                    // blend scales from both
                    blended_scale = lerp(jsp->getScale(), blended_scale, sum_weights[SCALE_WEIGHT] / new_weight_sum);
                    sum_weights[SCALE_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy scale from current
                    blended_scale = jsp->getScale();
                    sum_weights[SCALE_WEIGHT] = current_weight;
                }
            }

            // rotation
            if (current_usage & LLJointState::ROT)
            {
                if(sum_usage & LLJointState::ROT)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);

                    // blend rotations from both
                    blended_rot = nlerp(sum_weights[ROT_WEIGHT] / new_weight_sum, jsp->getRotation(), blended_rot);
                    sum_weights[ROT_WEIGHT] = new_weight_sum;
                }
                else
                {
                    // copy rotation from current
                    blended_rot = jsp->getRotation();
                    sum_weights[ROT_WEIGHT] = current_weight;
                }
            }

            // update resulting usage mask
            sum_usage = sum_usage | current_usage;
        }
    }

    if (!added_scale.isFinite())
    {
        added_scale.clearVec();
    }

    if (!blended_scale.isFinite())
    {
        blended_scale.setVec(1,1,1);
    }

    // apply transforms
    // SL-315
    target_joint->setPosition(blended_pos + added_pos);
    target_joint->setScale(blended_scale + added_scale);
    target_joint->setRotation(added_rot * blended_rot);

    if (apply_now)
    {
        // now clear joint states
        for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
        {
            mJointStates[i] = NULL;
        }
    }
}

//-----------------------------------------------------------------------------
// interpolate()
//-----------------------------------------------------------------------------
void LLJointStateBlender::interpolate(F32 u)
{
    // only interpolate if we have a joint state
    if (!mJointStates[0])
    {
        return;
    }
    LLJoint* target_joint = mJointStates[0]->getJoint();

    if (!target_joint)
    {
        return;
    }

    // SL-315
    target_joint->setPosition(lerp(target_joint->getPosition(), mJointCache.getPosition(), u));
    target_joint->setScale(lerp(target_joint->getScale(), mJointCache.getScale(), u));
    target_joint->setRotation(nlerp(u, target_joint->getRotation(), mJointCache.getRotation()));
}

//-----------------------------------------------------------------------------
// clear()
//-----------------------------------------------------------------------------
void LLJointStateBlender::clear()
{
    // now clear joint states
    for(S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        mJointStates[i] = NULL;
    }
}

//-----------------------------------------------------------------------------
// resetCachedJoint()
//-----------------------------------------------------------------------------
void LLJointStateBlender::resetCachedJoint()
{
    if (!mJointStates[0])
    {
        return;
    }
    LLJoint* source_joint = mJointStates[0]->getJoint();
    // SL-315
    mJointCache.setPosition(source_joint->getPosition());
    mJointCache.setScale(source_joint->getScale());
    mJointCache.setRotation(source_joint->getRotation());
}

//-----------------------------------------------------------------------------
// LLPoseBlender
//-----------------------------------------------------------------------------

LLPoseBlender::LLPoseBlender()
    : mNextPoseSlot(0)
{
}

LLPoseBlender::~LLPoseBlender()
{
    for_each(mJointStateBlenderPool.begin(), mJointStateBlenderPool.end(), DeletePairedPointer());
    mJointStateBlenderPool.clear();
}

//-----------------------------------------------------------------------------
// <FS> stage B1: the flat blender (see llpose.h)
//-----------------------------------------------------------------------------
namespace
{
int blend_flat_mode()
{
    static const int mode = getenv("LL_VK_BLENDFLAT") ? atoi(getenv("LL_VK_BLENDFLAT")) : 0;
    return mode;
}
std::atomic<U64> sFlatStates{ 0 }, sFlatInvalid{ 0 }, sFlatBlends{ 0 }, sFlatCompared{ 0 }, sFlatDiffer{ 0 }, sFlatDiffPos{ 0 }, sFlatDiffScale{ 0 }, sFlatDiffRot{ 0 }, sFlatOverridden{ 0 }, sFlatBitsOnly{ 0 };
}

struct LLPoseBlender::FlatBlend
{
    struct Slot
    {
        LLPointer<LLJointState> mState[JSB_NUM_JOINT_STATES];
        S32 mPrio[JSB_NUM_JOINT_STATES];
        bool mAdd[JSB_NUM_JOINT_STATES];
        S32 mCount = 0;
    };
    struct Result
    {
        LLVector3 mPos, mScale;
        LLQuaternion mRot;
        LLVector3 mStartPos;        // (compare mode) the joint's position before the blend
    };
    std::vector<Slot> mSlots;
    std::vector<Result> mResults;       // (compare mode)
    std::vector<S16> mActive;           // the numbers of the joints with states this frame, in the order they were first added
    FlatBlend() : mSlots(LL_CHARACTER_MAX_ANIMATED_JOINTS), mResults(LL_CHARACTER_MAX_ANIMATED_JOINTS) { mActive.reserve(LL_CHARACTER_MAX_ANIMATED_JOINTS); }
};

// the insertion of LLJointStateBlender::addJointState, on a slot
static bool flat_slot_add(LLPoseBlender::FlatBlend::Slot& s, const LLPointer<LLJointState>& joint_state, S32 priority, bool additive_blend)
{
    for (S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
    {
        if (s.mState[i].isNull())
        {
            s.mState[i] = joint_state;
            s.mPrio[i] = priority;
            s.mAdd[i] = additive_blend;
            return true;
        }
        else if (priority > s.mPrio[i])
        {
            for (S32 j = JSB_NUM_JOINT_STATES - 1; j > i; j--)
            {
                s.mState[j] = s.mState[j - 1];
                s.mPrio[j] = s.mPrio[j - 1];
                s.mAdd[j] = s.mAdd[j - 1];
            }
            s.mState[i] = joint_state;
            s.mPrio[i] = priority;
            s.mAdd[i] = additive_blend;
            return true;
        }
    }
    return false;
}

// the math of LLJointStateBlender::blendJointStates(apply_now = true), line for line, on a slot; the result goes to out (the caller applies or compares it)
static bool flat_slot_blend(const LLPoseBlender::FlatBlend::Slot& s, LLJoint* target_joint, LLPoseBlender::FlatBlend::Result& out)
{
    if (s.mState[0].isNull())
    {
        return false;
    }
    const S32 POS_WEIGHT = 0;
    const S32 ROT_WEIGHT = 1;
    const S32 SCALE_WEIGHT = 2;

    F32             sum_weights[3];
    U32             sum_usage = 0;

    LLVector3       blended_pos = target_joint->getPosition();
    LLQuaternion    blended_rot = target_joint->getRotation();
    LLVector3       blended_scale = target_joint->getScale();

    LLVector3       added_pos;
    LLQuaternion    added_rot;
    LLVector3       added_scale;

    sum_weights[POS_WEIGHT] = 0.f;
    sum_weights[ROT_WEIGHT] = 0.f;
    sum_weights[SCALE_WEIGHT] = 0.f;

    for (S32 joint_state_index = 0; joint_state_index < JSB_NUM_JOINT_STATES && s.mState[joint_state_index].notNull(); joint_state_index++)
    {
        LLJointState* jsp = s.mState[joint_state_index];
        U32 current_usage = jsp->getUsage();
        F32 current_weight = jsp->getWeight();

        if (current_weight == 0.f)
        {
            continue;
        }

        if (s.mAdd[joint_state_index])
        {
            if (current_usage & LLJointState::POS)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);
                added_pos += jsp->getPosition() * (new_weight_sum - sum_weights[POS_WEIGHT]);
            }
            if (current_usage & LLJointState::SCALE)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);
                added_scale += jsp->getScale() * (new_weight_sum - sum_weights[SCALE_WEIGHT]);
            }
            if (current_usage & LLJointState::ROT)
            {
                F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);
                added_rot = nlerp((new_weight_sum - sum_weights[ROT_WEIGHT]), added_rot, jsp->getRotation()) * added_rot;
            }
        }
        else
        {
            if (current_usage & LLJointState::POS)
            {
                if (sum_usage & LLJointState::POS)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[POS_WEIGHT]);
                    blended_pos = lerp(jsp->getPosition(), blended_pos, sum_weights[POS_WEIGHT] / new_weight_sum);
                    sum_weights[POS_WEIGHT] = new_weight_sum;
                }
                else
                {
                    blended_pos = jsp->getPosition();
                    sum_weights[POS_WEIGHT] = current_weight;
                }
            }
            if (current_usage & LLJointState::SCALE)
            {
                if (sum_usage & LLJointState::SCALE)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[SCALE_WEIGHT]);
                    blended_scale = lerp(jsp->getScale(), blended_scale, sum_weights[SCALE_WEIGHT] / new_weight_sum);
                    sum_weights[SCALE_WEIGHT] = new_weight_sum;
                }
                else
                {
                    blended_scale = jsp->getScale();
                    sum_weights[SCALE_WEIGHT] = current_weight;
                }
            }
            if (current_usage & LLJointState::ROT)
            {
                if (sum_usage & LLJointState::ROT)
                {
                    F32 new_weight_sum = llmin(1.f, current_weight + sum_weights[ROT_WEIGHT]);
                    blended_rot = nlerp(sum_weights[ROT_WEIGHT] / new_weight_sum, jsp->getRotation(), blended_rot);
                    sum_weights[ROT_WEIGHT] = new_weight_sum;
                }
                else
                {
                    blended_rot = jsp->getRotation();
                    sum_weights[ROT_WEIGHT] = current_weight;
                }
            }
            sum_usage = sum_usage | current_usage;
        }
    }

    if (!added_scale.isFinite())
    {
        added_scale.clearVec();
    }
    if (!blended_scale.isFinite())
    {
        blended_scale.setVec(1, 1, 1);
    }

    out.mPos = blended_pos + added_pos;
    out.mScale = blended_scale + added_scale;
    out.mRot = added_rot * blended_rot;
    return true;
}

//-----------------------------------------------------------------------------
// addMotion()
//-----------------------------------------------------------------------------
bool LLPoseBlender::addMotion(LLMotion* motion)
{
    LLPose* pose = motion->getPose();
    const int flat_mode = mFlatOn ? blend_flat_mode() : 0;
    if (flat_mode && !mFlat)
    {
        mFlat = std::make_shared<FlatBlend>();
    }

    for(LLJointState* jsp = pose->getFirstJointState(); jsp; jsp = pose->getNextJointState())
    {
        LLJoint *jointp = jsp->getJoint();
        if (flat_mode && jointp)
        {
            const S32 number = jointp->getJointNum();
            if (number >= 0 && number < (S32)LL_CHARACTER_MAX_ANIMATED_JOINTS)
            {
                FlatBlend::Slot& slot = mFlat->mSlots[number];
                const S32 priority = (jsp->getPriority() == LLJoint::USE_MOTION_PRIORITY) ? motion->getPriority() : jsp->getPriority();
                if (slot.mCount == 0)
                {
                    mFlat->mActive.push_back((S16)number);
                }
                if (flat_slot_add(slot, LLPointer<LLJointState>(jsp), priority, motion->getBlendType() == LLMotion::ADDITIVE_BLEND))
                {
                    slot.mCount = std::min<S32>(slot.mCount + 1, JSB_NUM_JOINT_STATES);
                }
                ++sFlatStates;
                if (flat_mode == 1)
                {
                    continue;       // (the pool below is not fed for a joint the flat slots took)
                }
            }
            else
            {
                ++sFlatInvalid;     // a joint without a number in range: the pool takes it
            }
        }
        LLJointStateBlender* joint_blender;
        if (mJointStateBlenderPool.find(jointp) == mJointStateBlenderPool.end())
        {
            // this is the first time we are animating this joint
            // so create new jointblender and add it to our pool
            joint_blender = new LLJointStateBlender();
            mJointStateBlenderPool[jointp] = joint_blender;
        }
        else
        {
            joint_blender = mJointStateBlenderPool[jointp];
        }

        if (jsp->getPriority() == LLJoint::USE_MOTION_PRIORITY)
        {
            joint_blender->addJointState(jsp, motion->getPriority(), motion->getBlendType() == LLMotion::ADDITIVE_BLEND);
        }
        else
        {
            joint_blender->addJointState(jsp, jsp->getPriority(), motion->getBlendType() == LLMotion::ADDITIVE_BLEND);
        }

        // add it to our list of active blenders
        if (std::find(mActiveBlenders.begin(), mActiveBlenders.end(), joint_blender) == mActiveBlenders.end())
        {
            mActiveBlenders.push_front(joint_blender);
        }
    }
    return true;
}

//-----------------------------------------------------------------------------
// blendAndApply()
//-----------------------------------------------------------------------------
void LLPoseBlender::blendAndApply()
{
    const int flat_mode = (mFlatOn && mFlat) ? blend_flat_mode() : 0;
    if (flat_mode && !mFlat->mActive.empty())
    {
        // the flat slots (mode 2: computed now, from the joints as they are before the pool blend, and compared with what the pool blend writes)
        for (S16 number : mFlat->mActive)
        {
            FlatBlend::Slot& slot = mFlat->mSlots[number];
            LLJoint* target = slot.mState[0].notNull() ? slot.mState[0]->getJoint() : nullptr;
            if (target)
            {
                FlatBlend::Result& result = mFlat->mResults[number];
                result.mStartPos = target->getPosition();
                if (flat_slot_blend(slot, target, result) && flat_mode == 1)
                {
                    target->setPosition(result.mPos);
                    target->setScale(result.mScale);
                    target->setRotation(result.mRot);
                    ++sFlatBlends;
                }
            }
            if (flat_mode == 1)
            {
                for (S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
                {
                    slot.mState[i] = NULL;
                }
                slot.mCount = 0;
            }
        }
        if (flat_mode == 1)
        {
            mFlat->mActive.clear();
        }
    }

    for (blender_list_t::iterator iter = mActiveBlenders.begin();
         iter != mActiveBlenders.end(); )
    {
        LLJointStateBlender* jsbp = *iter++;
        jsbp->blendJointStates();
    }

    // we're done now so there are no more active blenders for this frame
    mActiveBlenders.clear();

    if (flat_mode == 2)
    {
        for (S16 number : mFlat->mActive)
        {
            FlatBlend::Slot& slot = mFlat->mSlots[number];
            LLJoint* target = slot.mState[0].notNull() ? slot.mState[0]->getJoint() : nullptr;
            if (target)
            {
                const FlatBlend::Result& result = mFlat->mResults[number];
                ++sFlatCompared;
                // (the blender's setters do NOT apply attachment overrides: apply_attachment_overrides defaults to false; setRotation keeps the old value when the new one is not finite)
                LLVector3 override_value;
                LLUUID mesh_id;
                sFlatOverridden += target->hasAttachmentPosOverride(override_value, mesh_id) ? 1 : 0;
                // numeric equality, the rule of the setters themselves (setPosition and setRotation leave the joint alone when the new value is == the old one, so a -0.0 against a +0.0 keeps the old bits)
                const bool pos_differs = target->getPosition() != result.mPos;
                const bool scale_differs = target->getScale() != result.mScale;
                const bool rot_differs = result.mRot.isFinite() && target->getRotation() != result.mRot;
                if (!pos_differs && !scale_differs && !rot_differs
                    && (memcmp(&target->getPosition(), &result.mPos, sizeof(LLVector3)) != 0 || memcmp(&target->getScale(), &result.mScale, sizeof(LLVector3)) != 0
                        || (result.mRot.isFinite() && memcmp(&target->getRotation(), &result.mRot, sizeof(LLQuaternion)) != 0)))
                {
                    ++sFlatBitsOnly;       // equal as numbers, different bits (a negative zero): nothing downstream can tell
                }
                if (pos_differs || scale_differs || rot_differs)
                {
                    ++sFlatDiffer;
                    sFlatDiffPos += pos_differs ? 1 : 0;
                    sFlatDiffScale += scale_differs ? 1 : 0;
                    sFlatDiffRot += rot_differs ? 1 : 0;
                    static std::atomic<U32> said{ 0 };
                    if (said++ < 12)
                    {
                        std::string states;
                        for (S32 i = 0; i < JSB_NUM_JOINT_STATES && slot.mState[i].notNull(); i++)
                        {
                            states += " [prio " + std::to_string(slot.mPrio[i]) + (slot.mAdd[i] ? " add" : "") + " usage " + std::to_string(slot.mState[i]->getUsage()) + " weight " + std::to_string(slot.mState[i]->getWeight()) + " pos " +
                                std::to_string(slot.mState[i]->getPosition().mV[0]) + "," + std::to_string(slot.mState[i]->getPosition().mV[1]) + "," + std::to_string(slot.mState[i]->getPosition().mV[2]) + "]";
                        }
                        const LLVector3& now = target->getPosition();
                        LL_INFOS("BlendFlat") << "flat blend differs: joint " << number << " '" << target->getName() << "' joint number " << target->getJointNum() << " parent '" << (target->getParent() ? target->getParent()->getName() : std::string("none"))
                            << "' pos override " << (target->hasAttachmentPosOverride(override_value, mesh_id) ? "yes" : "no") << "; start (" << result.mStartPos.mV[0] << ", " << result.mStartPos.mV[1] << ", " << result.mStartPos.mV[2]
                            << ") flat result (" << result.mPos.mV[0] << ", " << result.mPos.mV[1] << ", " << result.mPos.mV[2] << ") joint now (" << now.mV[0] << ", " << now.mV[1] << ", " << now.mV[2] << "); states:" << states << LL_ENDL;
                    }
                }
            }
            for (S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
            {
                slot.mState[i] = NULL;
            }
            slot.mCount = 0;
        }
        mFlat->mActive.clear();
    }
    if (flat_mode)
    {
        static std::atomic<U64> calls{ 0 };
        if (++calls % 40000 == 0)
        {
            LL_INFOS("BlendFlat") << "flat blend, so far: joint states taken by the flat slots " << sFlatStates.load() << ", joints without a number in range (pool) " << sFlatInvalid.load() << ", joints blended and applied flat "
                << sFlatBlends.load() << ", compared with the pool blend " << sFlatCompared.load() << " (joints with an attachment override " << sFlatOverridden.load() << "), numerically different " << sFlatDiffer.load() << " (equal as numbers but different bits " << sFlatBitsOnly.load() << ")" 
                << " (position " << sFlatDiffPos.load() << ", scale " << sFlatDiffScale.load() << ", rotation " << sFlatDiffRot.load() << ")" << LL_ENDL;
        }
    }
}

//-----------------------------------------------------------------------------
// blendAndCache()
//-----------------------------------------------------------------------------
void LLPoseBlender::blendAndCache(bool reset_cached_joints)
{
    for (blender_list_t::iterator iter = mActiveBlenders.begin();
         iter != mActiveBlenders.end(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        if (reset_cached_joints)
        {
            jsbp->resetCachedJoint();
        }
        jsbp->blendJointStates(false);
    }
}

//-----------------------------------------------------------------------------
// interpolate()
//-----------------------------------------------------------------------------
void LLPoseBlender::interpolate(F32 u)
{
    for (blender_list_t::iterator iter = mActiveBlenders.begin();
         iter != mActiveBlenders.end(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        jsbp->interpolate(u);
    }
}

//-----------------------------------------------------------------------------
// clearBlenders()
//-----------------------------------------------------------------------------
void LLPoseBlender::clearBlenders()
{
    for (blender_list_t::iterator iter = mActiveBlenders.begin();
         iter != mActiveBlenders.end(); ++iter)
    {
        LLJointStateBlender* jsbp = *iter;
        jsbp->clear();
    }

    mActiveBlenders.clear();
    if (mFlat)
    {
        for (S16 number : mFlat->mActive)
        {
            FlatBlend::Slot& slot = mFlat->mSlots[number];
            for (S32 i = 0; i < JSB_NUM_JOINT_STATES; i++)
            {
                slot.mState[i] = NULL;
            }
            slot.mCount = 0;
        }
        mFlat->mActive.clear();
    }
}

