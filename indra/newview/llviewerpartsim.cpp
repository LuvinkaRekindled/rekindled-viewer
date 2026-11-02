/**
 * @file llviewerpartsim.cpp
 * @brief LLViewerPart class implementation
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

#include "llviewerpartsim.h"

#include "llviewercontrol.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llvopartgroup.h"
#include "llworld.h"
#include "llmutelist.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llviewertexture.h"

const F32 PART_SIM_BOX_SIDE = 16.f;

//static
S32 LLViewerPartSim::sMaxParticleCount = 0;
std::atomic<S32> LLViewerPartSim::sParticleCount{0}; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
F64 LLViewerPartSim::sSimTime = 0.0;
F32 LLViewerPartSim::sSimDt = 0.f;
std::atomic<S32> LLViewerPartSim::sRecordCount{0};
std::atomic<S32> LLViewerPartSim::sParticleCount2{0}; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
// This controls how greedy individual particle burst sources are allowed to be, and adapts according to how near the particle-count limit we are.
F32 LLViewerPartSim::sParticleAdaptiveRate = 0.0625f;
F32 LLViewerPartSim::sParticleBurstRate = 0.5f;

//static
const S32 LLViewerPartSim::MAX_PART_COUNT = 8192;
const F32 LLViewerPartSim::PART_THROTTLE_THRESHOLD = 0.9f;
const F32 LLViewerPartSim::PART_ADAPT_RATE_MULT = 2.0f;

//static
const F32 LLViewerPartSim::PART_THROTTLE_RESCALE = PART_THROTTLE_THRESHOLD / (1.0f-PART_THROTTLE_THRESHOLD);
const F32 LLViewerPartSim::PART_ADAPT_RATE_MULT_RECIP = 1.0f/PART_ADAPT_RATE_MULT;


U32 LLViewerPart::sNextPartID = 1;

F32 calc_desired_size(LLViewerCamera* camera, LLVector3 pos, LLVector2 scale)
{
    F32 desired_size = (pos - camera->getOrigin()).magVec();
    desired_size /= 4;
    return llclamp(desired_size, scale.magVec()*0.5f, PART_SIM_BOX_SIDE*2);
}

LLViewerPart::LLViewerPart() :
    mPartID(0),
    mLastUpdateTime(0.f),
    mSkipOffset(0.f),
    mVPCallback(nullptr),
    mImagep(nullptr)
{
    mPartSourcep = nullptr;
    mParent      = nullptr;
    mChild       = nullptr;
    ++LLViewerPartSim::sParticleCount2 ;
}

LLViewerPart::~LLViewerPart()
{
    if (mPartSourcep.notNull() && mPartSourcep->mLastPart == this)
    {
        mPartSourcep->mLastPart = NULL;
    }

    //patch up holes in the ribbon
    if (mParent)
    {
        llassert(mParent->mChild == this);
        mParent->mChild = mChild;
    }

    if (mChild)
    {
        llassert (mChild->mParent == this);
        mChild->mParent = mParent;
    }

    mPartSourcep = NULL;

    --LLViewerPartSim::sParticleCount2 ;
}

void LLViewerPart::init(LLPointer<LLViewerPartSource> sourcep, LLViewerTexture *imagep, LLVPCallback cb)
{
    mPartID = LLViewerPart::sNextPartID;
    LLViewerPart::sNextPartID++;
    mFlags = 0x00f;
    mLastUpdateTime = 0.f;
    mMaxAge = 10.f;
    mSkipOffset = 0.0f;

    mVPCallback = cb;
    mPartSourcep = sourcep;

    mImagep = imagep;
}


/////////////////////////////
//
// LLViewerPartGroup implementation
//
//


LLViewerPartGroup::LLViewerPartGroup(const LLVector3 &center_agent, const F32 box_side, bool hud)
 : mHud(hud)
{
    mVOPartGroupp = NULL;
    mUniformParticles = true;

    mRegionp = LLWorld::getInstance()->getRegionFromPosAgent(center_agent);
    llassert_always(center_agent.isFinite());

    if (!mRegionp)
    {
        //LL_WARNS() << "No region at position, using agent region!" << LL_ENDL;
        mRegionp = gAgent.getRegion();
    }
    mCenterAgent = center_agent;
    mBoxRadius = F_SQRT3*box_side*0.5f;

    if (mHud)
    {
        mVOPartGroupp = (LLVOPartGroup *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_HUD_PART_GROUP, getRegion());
    }
    else
    {
    mVOPartGroupp = (LLVOPartGroup *)gObjectList.createObjectViewer(LLViewerObject::LL_VO_PART_GROUP, getRegion());
    }
    mVOPartGroupp->setViewerPartGroup(this);
    mVOPartGroupp->setPositionAgent(getCenterAgent());

    mBoxSide = box_side;

    F32 scale = box_side * 0.5f;

    mVOPartGroupp->setScale(LLVector3(scale,scale,scale));

    //gPipeline.addObject(mVOPartGroupp);
    gPipeline.createObject(mVOPartGroupp);

    LLSpatialGroup* group = mVOPartGroupp->mDrawable->getSpatialGroup();

    if (group != NULL)
    {
        LLVector3 center(group->getOctreeNode()->getCenter().getF32ptr());
        LLVector3 size(group->getOctreeNode()->getSize().getF32ptr());
        size += LLVector3(0.01f, 0.01f, 0.01f);
        mMinObjPos = center - size;
        mMaxObjPos = center + size;
    }
    else
    {
        // Not sure what else to set the obj bounds to when the drawable has no spatial group.
        LLVector3 extents(mBoxRadius, mBoxRadius, mBoxRadius);
        mMinObjPos = center_agent - extents;
        mMaxObjPos = center_agent + extents;
    }

    mSkippedTime = 0.f;

    static U32 id_seed = 0;
    mID = ++id_seed;
}

LLViewerPartGroup::~LLViewerPartGroup()
{
    cleanup();

    S32 count = (S32) mParticles.size();
    for(S32 i = 0 ; i < count ; i++)
    {
        delete mParticles[i] ;
    }
    mParticles.clear();

    const S32 records = (S32) mRecords.size();
    mRecords.clear();
    mDeadAt.clear();
    LLViewerPartSim::sRecordCount -= records;
    LLViewerPartSim::sParticleCount -= count + records; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
}

void LLViewerPartGroup::cleanup()
{
    if (mVOPartGroupp)
    {
        if (!mVOPartGroupp->isDead())
        {
            gObjectList.killObject(mVOPartGroupp);
        }
        mVOPartGroupp = NULL;
    }
}

bool LLViewerPartGroup::posInGroup(const LLVector3 &pos, const F32 desired_size)
{
    if ((pos.mV[VX] < mMinObjPos.mV[VX])
        || (pos.mV[VY] < mMinObjPos.mV[VY])
        || (pos.mV[VZ] < mMinObjPos.mV[VZ]))
    {
        return false;
    }

    if ((pos.mV[VX] > mMaxObjPos.mV[VX])
        || (pos.mV[VY] > mMaxObjPos.mV[VY])
        || (pos.mV[VZ] > mMaxObjPos.mV[VZ]))
    {
        return false;
    }

    if (desired_size > 0 &&
        (desired_size < mBoxRadius*0.5f ||
        desired_size > mBoxRadius*2.f))
    {
        return false;
    }

    return true;
}


bool LLViewerPartGroup::addPart(LLViewerPart* part, F32 desired_size)
{
    if (part->mFlags & LLPartData::LL_PART_HUD && !mHud)
    {
        return false;
    }

    bool uniform_part = part->mScale.mV[0] == part->mScale.mV[1] &&
                    !(part->mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK);

    if (!posInGroup(part->mPosAgent, desired_size) ||
        (mUniformParticles && !uniform_part) ||
        (!mUniformParticles && uniform_part))
    {
        return false;
    }

    gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);

    mParticles.push_back(part);
    part->mSkipOffset=mSkippedTime;
    ++LLViewerPartSim::sParticleCount; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
    return true;
}

// <FS> GPU particles stage 2: the same placement rules as addPart, the particle kept as a record (the caller deletes the LLViewerPart)
bool LLViewerPartGroup::addRecord(const LLViewerPart& part, F32 desired_size)
{
    if (mHud || (part.mFlags & LLPartData::LL_PART_HUD))
    {
        return false;
    }

    bool uniform_part = part.mScale.mV[0] == part.mScale.mV[1] &&
                    !(part.mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK);

    if (!posInGroup(part.mPosAgent, desired_size) ||
        (mUniformParticles && !uniform_part) ||
        (!mUniformParticles && uniform_part))
    {
        return false;
    }

    gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);

    LLViewerPartRecord rec;
    rec.mPos0 = part.mPosAgent;
    rec.mVel0 = part.mVelocity;
    rec.mAccel = part.mAccel;
    rec.mT0 = LLViewerPartSim::sSimTime - (F64)LLViewerPartSim::sSimDt;     // (the stepping gives a particle its first full step in the frame it was made in)
    rec.mMaxAge = part.mMaxAge;
    rec.mFlags = part.mFlags;
    rec.mStartColor = part.mStartColor;
    rec.mEndColor = part.mEndColor;
    rec.mStartScale = part.mStartScale;
    rec.mEndScale = part.mEndScale;
    rec.mStartGlow = part.mStartGlow;
    rec.mEndGlow = part.mEndGlow;
    rec.mBlendSrc = part.mBlendFuncSource;
    rec.mBlendDst = part.mBlendFuncDest;
    rec.mImagep = part.mImagep;
    rec.mSourcep = part.mPartSourcep;
    mDeadAt.push_back(rec.mT0 + (F64)rec.mMaxAge);
    mRecords.push_back(std::move(rec));
    ++LLViewerPartSim::sParticleCount;
    ++LLViewerPartSim::sRecordCount;
    return true;
}

bool LLViewerPartGroup::expireRecords()
{
    const F64 now = LLViewerPartSim::sSimTime;
    bool gone = false;
    for (size_t i = 0; i < mDeadAt.size();)
    {
        if (now > mDeadAt[i])
        {
            mDeadAt[i] = mDeadAt.back();
            mDeadAt.pop_back();
            if (i + 1 != mRecords.size())
            {
                mRecords[i] = std::move(mRecords.back());
            }
            mRecords.pop_back();
            --LLViewerPartSim::sParticleCount;
            --LLViewerPartSim::sRecordCount;
            gone = true;
        }
        else
        {
            ++i;
        }
    }
    return gone;
}


// <FS> GPU particles stage 2 census (doc/vulkan/PLAN-gpu-particles.md): what the simulation steps, by behaviour (log tag Q1Census, every 600 frames)
static struct PartSimCensus
{
    U64 mRecs = 0, mFrames = 0, mStepped = 0, mSpawned = 0, mSrcCycles = 0, mGrpCycles = 0, mGroups = 0, mGroupsSkipped = 0, mSources = 0;
    U64 mFollowSrc = 0, mWind = 0, mTargetPos = 0, mTargetLinear = 0, mBounce = 0, mCallback = 0, mRibbon = 0, mFollowVel = 0, mInterpColor = 0, mInterpScale = 0, mEmissive = 0;
    U64 mAnalytic = 0, mAnalyticSrc = 0, mParented = 0, mAgeSum = 0;
    U64 mMaxAgeMs = 0;
    // closed form against the stepped state (LL_VK_PARTSIM_COMPARE=1): the largest differences of the window
    U64 mCmpN = 0, mCmpPosOver = 0;
    F32 mCmpPos = 0.f, mCmpVel = 0.f, mCmpColor = 0.f, mCmpScale = 0.f, mCmpGlow = 0.f, mCmpPosAge = 0.f;
} sSimCensus;

static bool fs_partsim_compare_on()
{
    static const bool on = []() { const char* v = getenv("LL_VK_PARTSIM_COMPARE"); return v && atoi(v) != 0; }();
    return on;
}

// a particle with no per-frame input at the age it was just stepped to: the closed form of what the stepping did (position and velocity of a constant acceleration, colour / scale / glow linear in age / max age)
static void fs_partsim_compare(const LLViewerPart* part)
{
    const F32 age = part->mLastUpdateTime;
    const LLVector3 p = part->mSpawnPos + part->mSpawnVel * age + part->mAccel * (0.5f * age * age);
    const LLVector3 v = part->mSpawnVel + part->mAccel * age;
    const F32 frac = age / part->mMaxAge;
    LLColor4 color = part->mStartColor;
    if (part->mFlags & LLPartData::LL_PART_INTERP_COLOR_MASK)
    {
        color = fs_part_color_at(part->mStartColor, part->mEndColor, frac);
    }
    LLVector2 scale = part->mStartScale;
    if (part->mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK)
    {
        scale = fs_part_scale_at(part->mStartScale, part->mEndScale, frac);
    }
    const F32 glow = (F32)(U8)ll_round(lerp(part->mStartGlow, part->mEndGlow, frac) * 255.f);

    ++sSimCensus.mCmpN;
    const F32 dpos = (p - part->mPosAgent).length();
    if (dpos > sSimCensus.mCmpPos)
    {
        sSimCensus.mCmpPos = dpos;
        sSimCensus.mCmpPosAge = age;
    }
    sSimCensus.mCmpPosOver += dpos > 0.001f;
    sSimCensus.mCmpVel = llmax(sSimCensus.mCmpVel, (v - part->mVelocity).length());
    F32 dcol = 0.f;
    for (int k = 0; k < 4; ++k)
    {
        dcol = llmax(dcol, fabsf(color.mV[k] - part->mColor.mV[k]));
    }
    sSimCensus.mCmpColor = llmax(sSimCensus.mCmpColor, dcol);
    sSimCensus.mCmpScale = llmax(sSimCensus.mCmpScale, fabsf(scale.mV[0] - part->mScale.mV[0]), fabsf(scale.mV[1] - part->mScale.mV[1]));
    sSimCensus.mCmpGlow = llmax(sSimCensus.mCmpGlow, fabsf(glow - (F32)part->mGlow.mV[3]));
}

void LLViewerPartGroup::updateParticles(const F32 lastdt)
{
    F32 dt;

    LLVector3 gravity(0.f, 0.f, GRAVITY);

    LLViewerPartSim::checkParticleCount(static_cast<U32>(mParticles.size()));

    const bool compare_on = fs_partsim_compare_on();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLViewerRegion *regionp = getRegion();
    // <FS:Beq> FIRE-34600 - Bugsplat AVX2 particle count mismatch    
    // S32 end = (S32) mParticles.size();
    bool changed = false;
    // </FS:Beq>
    for (S32 i = 0 ; i < (S32)mParticles.size();)
    {
        LLVector3 a(0.f, 0.f, 0.f);
        LLViewerPart* part = mParticles[i] ;
        {
            const U32 fl = part->mFlags;
            ++sSimCensus.mStepped;
            sSimCensus.mFollowSrc += (fl & LLPartData::LL_PART_FOLLOW_SRC_MASK) != 0;
            sSimCensus.mWind += (fl & LLPartData::LL_PART_WIND_MASK) != 0;
            sSimCensus.mTargetPos += (fl & LLPartData::LL_PART_TARGET_POS_MASK) != 0;
            sSimCensus.mTargetLinear += (fl & LLPartData::LL_PART_TARGET_LINEAR_MASK) != 0;
            sSimCensus.mBounce += (fl & LLPartData::LL_PART_BOUNCE_MASK) != 0;
            sSimCensus.mCallback += part->mVPCallback != nullptr;
            sSimCensus.mRibbon += (fl & LLPartData::LL_PART_RIBBON_MASK) != 0;
            sSimCensus.mFollowVel += (fl & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK) != 0;
            sSimCensus.mInterpColor += (fl & LLPartData::LL_PART_INTERP_COLOR_MASK) != 0;
            sSimCensus.mInterpScale += (fl & LLPartData::LL_PART_INTERP_SCALE_MASK) != 0;
            sSimCensus.mEmissive += (fl & LLPartData::LL_PART_EMISSIVE_MASK) != 0;
            const bool stateful = (fl & (LLPartData::LL_PART_WIND_MASK | LLPartData::LL_PART_TARGET_POS_MASK | LLPartData::LL_PART_BOUNCE_MASK)) != 0 || part->mVPCallback != nullptr;
            const bool source_driven = (fl & (LLPartData::LL_PART_FOLLOW_SRC_MASK | LLPartData::LL_PART_TARGET_LINEAR_MASK)) != 0;
            sSimCensus.mAnalytic += !stateful;
            sSimCensus.mAnalyticSrc += (!stateful && !source_driven);
            sSimCensus.mParented += part->mParent != nullptr;
            sSimCensus.mAgeSum += (U64)(part->mMaxAge * 1000.f);
        }

        dt = lastdt + mSkippedTime - part->mSkipOffset;
        part->mSkipOffset = 0.f;

        // Update current time
        const F32 cur_time = part->mLastUpdateTime + dt;
        const F32 frac = cur_time / part->mMaxAge;

        // "Drift" the object based on the source object
        if (part->mFlags & LLPartData::LL_PART_FOLLOW_SRC_MASK)
        {
            part->mPosAgent = part->mPartSourcep->mPosAgent;
            part->mPosAgent += part->mPosOffset;
        }

        // Do a custom callback if we have one...
        if (part->mVPCallback)
        {
            (*part->mVPCallback)(*part, dt);
        }

        if (part->mFlags & LLPartData::LL_PART_WIND_MASK)
        {
            part->mVelocity *= 1.f - 0.1f*dt;
            part->mVelocity += 0.1f*dt*regionp->mWind.getVelocity(regionp->getPosRegionFromAgent(part->mPosAgent));
        }

        // Now do interpolation towards a target
        if (part->mFlags & LLPartData::LL_PART_TARGET_POS_MASK)
        {
            F32 remaining = part->mMaxAge - part->mLastUpdateTime;
            F32 step = dt / remaining;

            step = llclamp(step, 0.f, 0.1f);
            step *= 5.f;
            // we want a velocity that will result in reaching the target in the
            // Interpolate towards the target.
            LLVector3 delta_pos = part->mPartSourcep->mTargetPosAgent - part->mPosAgent;

            delta_pos /= remaining;

            part->mVelocity *= (1.f - step);
            part->mVelocity += step*delta_pos;
        }


        if (part->mFlags & LLPartData::LL_PART_TARGET_LINEAR_MASK)
        {
            LLVector3 delta_pos = part->mPartSourcep->mTargetPosAgent - part->mPartSourcep->mPosAgent;
            part->mPosAgent = part->mPartSourcep->mPosAgent;
            part->mPosAgent += frac*delta_pos;
            part->mVelocity = delta_pos;
        }
        else
        {
            // Do velocity interpolation
            part->mPosAgent += dt*part->mVelocity;
            part->mPosAgent += 0.5f*dt*dt*part->mAccel;
            part->mVelocity += part->mAccel*dt;
        }

        // Do a bounce test
        if (part->mFlags & LLPartData::LL_PART_BOUNCE_MASK)
        {
            // Need to do point vs. plane check...
            // For now, just check relative to object height...
            F32 dz = part->mPosAgent.mV[VZ] - part->mPartSourcep->mPosAgent.mV[VZ];
            if (dz < 0)
            {
                part->mPosAgent.mV[VZ] += -2.f*dz;
                part->mVelocity.mV[VZ] *= -0.75f;
            }
        }


        // Reset the offset from the source position
        if (part->mFlags & LLPartData::LL_PART_FOLLOW_SRC_MASK)
        {
            part->mPosOffset = part->mPosAgent;
            part->mPosOffset -= part->mPartSourcep->mPosAgent;
        }

        // Do color interpolation
        if (part->mFlags & LLPartData::LL_PART_INTERP_COLOR_MASK)
        {
            part->mColor.setVec(part->mStartColor);
            // note: LLColor4's v%k means multiply-alpha-only,
            //       LLColor4's v*k means multiply-rgb-only
            part->mColor *= 1.f - frac; // rgb*k
            part->mColor %= 1.f - frac; // alpha*k
            part->mColor += frac%(frac*part->mEndColor); // rgb,alpha
        }

        // Do scale interpolation
        if (part->mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK)
        {
            part->mScale.setVec(part->mStartScale);
            part->mScale *= 1.f - frac;
            part->mScale += frac*part->mEndScale;
        }

        // Do glow interpolation
        part->mGlow.mV[3] = (U8) ll_round(lerp(part->mStartGlow, part->mEndGlow, frac)*255.f);

        // Set the last update time to now.
        part->mLastUpdateTime = cur_time;
        if (compare_on && part->mSpawnKnown && part->mVPCallback == nullptr
            && !(part->mFlags & (LLPartData::LL_PART_WIND_MASK | LLPartData::LL_PART_TARGET_POS_MASK | LLPartData::LL_PART_TARGET_LINEAR_MASK | LLPartData::LL_PART_BOUNCE_MASK | LLPartData::LL_PART_FOLLOW_SRC_MASK)))
        {
            fs_partsim_compare(part);
        }


        // Kill dead particles (either flagged dead, or too old)
        if ((part->mLastUpdateTime > part->mMaxAge) || (LLViewerPart::LL_PART_DEAD_MASK == part->mFlags))
        {
            // <FS:Beq> FIRE-34600 - Bugsplat AVX2 particle count mismatch
            // mParticles[i] = mParticles.back() ;
            // mParticles.pop_back() ;
            // delete part ;
            vector_replace_with_last(mParticles, mParticles.begin() + i);
            --LLViewerPartSim::sParticleCount; 
            delete part ;
            changed = true; 
            // </FS:Beq>
        }
        else
        {
            F32 desired_size = calc_desired_size(camera, part->mPosAgent, part->mScale);
            if (!posInGroup(part->mPosAgent, desired_size))
            {
                // Transfer particles between groups
                // <FS:Beq> FIRE-34600 - Bugsplat AVX2 particle count mismatch
                // LLViewerPartSim::getInstance()->put(part) ;
                // mParticles[i] = mParticles.back() ;
                // mParticles.pop_back() ;                
                vector_replace_with_last(mParticles, mParticles.begin() + i); 
                LLViewerPartSim::getInstance()->put(part) ; 
                // Note: put() uses addpart when succesful, this increase sParticleCount by 1
                // even though it has stayed the same. If it is not succesful then we need to decrease by 1
                // so a decrement here works for both cases.
                --LLViewerPartSim::sParticleCount; 
                changed = true;
                // </FS:Beq>
            }
            else
            {
                i++ ;
            }
        }
    }

    if (expireRecords())
    {
        changed = true;
    }

    // <FS:Beq> FIRE-34600 - Bugsplat AVX2 particle count mismatch
    // S32 removed = end - (S32)mParticles.size();
    // if (removed > 0)
    // {
    //     // we removed one or more particles, so flag this group for update
    //     if (mVOPartGroupp.notNull())
    //     {
    //         gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);
    //     }
    //     LLViewerPartSim::decPartCount(removed);
    // }    
    if (changed)
    {
        if (mVOPartGroupp.notNull())
        {
            gPipeline.markRebuild(mVOPartGroupp->mDrawable, LLDrawable::REBUILD_ALL);
        }
    }
    // </FS:Beq>

    // Kill the viewer object if this particle group is empty
    if (mParticles.empty() && mRecords.empty())
    {
        gObjectList.killObject(mVOPartGroupp);
        mVOPartGroupp = NULL;
    }

    LLViewerPartSim::checkParticleCount() ;
}


void LLViewerPartGroup::shift(const LLVector3 &offset)
{
    mCenterAgent += offset;
    mMinObjPos += offset;
    mMaxObjPos += offset;

    for (S32 i = 0 ; i < (S32)mParticles.size(); i++)
    {
        mParticles[i]->mPosAgent += offset;
        mParticles[i]->mSpawnPos += offset;
    }
    for (LLViewerPartRecord& rec : mRecords)
    {
        rec.mPos0 += offset;
    }
}

void LLViewerPartGroup::removeParticlesByID(const U32 source_id)
{
    for (S32 i = 0; i < (S32)mParticles.size(); i++)
    {
        if(mParticles[i]->mPartSourcep->getID() == source_id)
        {
            mParticles[i]->mFlags = LLViewerPart::LL_PART_DEAD_MASK;
        }
    }
    for (size_t i = 0; i < mRecords.size(); i++)
    {
        if (mRecords[i].mSourcep.notNull() && mRecords[i].mSourcep->getID() == source_id)
        {
            mDeadAt[i] = -1.0;
        }
    }
}

//////////////////////////////////
//
// LLViewerPartSim implementation
//
//

//static
void LLViewerPartSim::checkParticleCount(U32 size)
{
    // <FS:Beq> FIRE-34600 - bugsplat AVX2 particle count mismatch
    // if(LLViewerPartSim::sParticleCount2 != LLViewerPartSim::sParticleCount)
    // {
    //     LL_ERRS() << "sParticleCount: " << LLViewerPartSim::sParticleCount << " ; sParticleCount2: " << LLViewerPartSim::sParticleCount2 << LL_ENDL ;
    // }
    //
    // if(size > (U32)LLViewerPartSim::sParticleCount2)
    // {
    //     LL_ERRS() << "current particle size: " << LLViewerPartSim::sParticleCount2 << " array size: " << size << LL_ENDL ; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
    // }
    if(LLViewerPartSim::sParticleCount2 + LLViewerPartSim::sRecordCount != LLViewerPartSim::sParticleCount)
    {
        static int fail_count{0};
        if(fail_count > 10)
        {
            LL_ERRS() << "sParticleCount: " << LLViewerPartSim::sParticleCount << " ; sParticleCount2: " << LLViewerPartSim::sParticleCount2 << LL_ENDL ;
        }
        else
        {
            LL_WARNS() << "sParticleCount: " << LLViewerPartSim::sParticleCount << " ; sParticleCount2: " << LLViewerPartSim::sParticleCount2 << LL_ENDL ;
        }
        fail_count++;
    }

    if(size > (U32)LLViewerPartSim::sParticleCount2)
    {
        static int size_mismatch_count{0};
        if(size_mismatch_count > 10)
        {
            LL_ERRS() << "current particle size: " << LLViewerPartSim::sParticleCount2 << " array size: " << size << LL_ENDL ;
        }
        else
        {
            LL_WARNS() << "current particle size: " << LLViewerPartSim::sParticleCount2 << " array size: " << size << LL_ENDL ;
        }
        size_mismatch_count++;
    }
    // </FS:Beq>
}

LLViewerPartSim::LLViewerPartSim()
{
    sMaxParticleCount = llmin(gSavedSettings.getS32("RenderMaxPartCount"), LL_MAX_PARTICLE_COUNT);
    static U32 id_seed = 0;
    mID = ++id_seed;
}

//enable/disable particle system
void LLViewerPartSim::enable(bool enabled)
{
    if(!enabled && sMaxParticleCount > 0)
    {
        sMaxParticleCount = 0; //disable
    }
    else if(enabled && sMaxParticleCount < 1)
    {
        sMaxParticleCount = llmin(gSavedSettings.getS32("RenderMaxPartCount"), LL_MAX_PARTICLE_COUNT);
    }

    return;
}

void LLViewerPartSim::destroyClass()
{
    S32 i;
    S32 count;

    // Kill all of the groups (and particles)
    count = (S32) mViewerPartGroups.size();
    for (i = 0; i < count; i++)
    {
        delete mViewerPartGroups[i];
    }
    mViewerPartGroups.clear();

    // Kill all of the sources
    mViewerPartSources.clear();
}

//static
bool LLViewerPartSim::shouldAddPart()
{
    if (sParticleCount >= MAX_PART_COUNT)
    {
        return false;
    }

    if ( sParticleCount > PART_THROTTLE_THRESHOLD*sMaxParticleCount)
    {
        F32 frac = (F32)sParticleCount/(F32)sMaxParticleCount;
        frac -= PART_THROTTLE_THRESHOLD;
        frac *= PART_THROTTLE_RESCALE;
        if (ll_frand() < frac)
        {
            // Skip...
            return false;
        }
    }

    // Check frame rate, and don't add more if the viewer is really slow
    const F32 MIN_FRAME_RATE_FOR_NEW_PARTICLES = 4.f;
    if (gFPSClamped < MIN_FRAME_RATE_FOR_NEW_PARTICLES)
    {
        return false;
    }

    return true;
}

// <FS> GPU particles stage 2: LL_VK_GPU_PARTICLE_SIM=1 (and the rows of stage 1 on) keeps the particles without per-frame input as records
static bool fs_partsim_records_on()
{
    static const bool on = []()
    {
        const char* rows = getenv("LL_VK_GPU_PARTICLES");
        const char* sim = getenv("LL_VK_GPU_PARTICLE_SIM");
        return (!rows || atoi(rows) != 0) && sim && atoi(sim) != 0;
    }();
    return on;
}

static bool fs_part_is_record(const LLViewerPart& part)
{
    const U32 excluded = LLPartData::LL_PART_WIND_MASK | LLPartData::LL_PART_TARGET_POS_MASK | LLPartData::LL_PART_TARGET_LINEAR_MASK | LLPartData::LL_PART_BOUNCE_MASK
        | LLPartData::LL_PART_FOLLOW_SRC_MASK | LLPartData::LL_PART_RIBBON_MASK | LLPartData::LL_PART_HUD;
    return !(part.mFlags & excluded) && part.mVPCallback == nullptr && part.mParent == nullptr && part.mMaxAge > 0.f
        && !(part.mImagep.notNull() && part.mImagep->hasParcelMedia());
}

void LLViewerPartSim::addPart(LLViewerPart* part)
{
    ++sSimCensus.mSpawned;
    part->mSpawnPos = part->mPosAgent;
    part->mSpawnVel = part->mVelocity;
    part->mSpawnKnown = true;
    if (LLViewerPartSim::sParticleCount < MAX_PART_COUNT)
    {
        if (fs_partsim_records_on() && fs_part_is_record(*part))
        {
            put(part, true);
        }
        else
        {
            put(part);
        }
    }
    else
    {
        //delete the particle if can not add it in
        delete part ;
        part = NULL ;
    }
}


LLViewerPartGroup *LLViewerPartSim::put(LLViewerPart* part, bool as_record)
{
    const F32 MAX_MAG = 1000000.f*1000000.f; // 1 million
    LLViewerPartGroup *return_group = NULL ;
    if (part->mPosAgent.magVecSquared() > MAX_MAG || !part->mPosAgent.isFinite())
    {
#if 0 && !LL_RELEASE_FOR_DOWNLOAD
        LL_WARNS() << "LLViewerPartSim::put Part out of range!" << LL_ENDL;
        LL_WARNS() << part->mPosAgent << LL_ENDL;
#endif
    }
    else
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        F32 desired_size = calc_desired_size(camera, part->mPosAgent, part->mScale);

        S32 count = (S32) mViewerPartGroups.size();
        for (S32 i = 0; i < count; i++)
        {
            if (as_record ? mViewerPartGroups[i]->addRecord(*part, desired_size) : mViewerPartGroups[i]->addPart(part, desired_size))
            {
                // We found a spatial group that we fit into, add us and exit
                return_group = mViewerPartGroups[i];
                break ;
            }
        }

        // Hmm, we didn't fit in any of the existing spatial groups
        // Create a new one...
        if(!return_group)
        {
            llassert_always(part->mPosAgent.isFinite());
            LLViewerPartGroup *groupp = createViewerPartGroup(part->mPosAgent, desired_size, part->mFlags & LLPartData::LL_PART_HUD);
            groupp->mUniformParticles = (part->mScale.mV[0] == part->mScale.mV[1] &&
                                    !(part->mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK));
            if (!(as_record ? groupp->addRecord(*part, -1.f) : groupp->addPart(part)))
            {
                LL_WARNS() << "LLViewerPartSim::put - Particle didn't go into its box!" << LL_ENDL;
                LL_INFOS() << groupp->getCenterAgent() << LL_ENDL;
                LL_INFOS() << part->mPosAgent << LL_ENDL;
                mViewerPartGroups.pop_back() ;
                delete groupp;
                groupp = NULL ;
            }
            return_group = groupp;
        }
    }

    if(!return_group) //failed to insert the particle
    {
        delete part ;
        part = NULL ;
    }
    else if (as_record)
    {
        delete part;        // (kept as a record)
        part = NULL;
    }

    return return_group ;
}

LLViewerPartGroup *LLViewerPartSim::createViewerPartGroup(const LLVector3 &pos_agent, const F32 desired_size, bool hud)
{
    //find a box that has a center position divisible by PART_SIM_BOX_SIDE that encompasses
    //pos_agent
    LLViewerPartGroup *groupp = new LLViewerPartGroup(pos_agent, desired_size, hud);
    mViewerPartGroups.push_back(groupp);
    return groupp;
}


void LLViewerPartSim::shift(const LLVector3 &offset)
{
    S32 i;
    S32 count;

    count = (S32) mViewerPartSources.size();
    for (i = 0; i < count; i++)
    {
        mViewerPartSources[i]->mPosAgent += offset;
        mViewerPartSources[i]->mTargetPosAgent += offset;
        mViewerPartSources[i]->mLastUpdatePosAgent += offset;
    }

    count = (S32) mViewerPartGroups.size();
    for (i = 0; i < count; i++)
    {
        mViewerPartGroups[i]->shift(offset);
    }
}

static LLTrace::BlockTimerStatHandle FTM_SIMULATE_PARTICLES("Simulate Particles");

void LLViewerPartSim::updateSimulation()
{
    static LLFrameTimer update_timer;

    const F32 dt = llmin(update_timer.getElapsedTimeAndResetF32(), 0.1f);
    sSimTime += (F64)dt;
    sSimDt = dt;

    // <FS:LO> Dont suspend partical processing while particles are hidden, just skip over drawing them
    /*if (!(gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES)))
    {
        return;
    }*/
    // </FS:LO>

    LL_RECORD_BLOCK_TIME(FTM_SIMULATE_PARTICLES);

    // Start at a random particle system so the same
    // particle system doesn't always get first pick at the
    // particles.  Theoretically we'd want to do this in distance
    // order or something, but sorting particle sources will be a big
    // pain.
    S32 i;
    S32 count = (S32) mViewerPartSources.size();
    S32 start = (S32)ll_frand((F32)count);
    S32 dir = 1;
    S32 deldir = 0;
    if (ll_frand() > 0.5f)
    {
        dir = -1;
        deldir = -1;
    }

    const unsigned long long census_t0 = __rdtsc();
    sSimCensus.mSources += (U64)count;
    S32 num_updates = 0;
    for (i = start; num_updates < count;)
    {
        if (i >= count)
        {
            i = 0;
        }
        if (i < 0)
        {
            i = count - 1;
        }

        if (!mViewerPartSources[i]->isDead())
        {
            bool upd = true;
            LLViewerObject* vobj = mViewerPartSources[i]->mSourceObjectp;

            if (vobj && vobj->isAvatar() && ((LLVOAvatar*)vobj)->isInMuteList())
            {
                upd = false;
            }

            if(vobj && vobj->isOwnerInMuteList(mViewerPartSources[i]->getOwnerUUID()))
            {
                upd = false;
            }

            if (upd && vobj && (vobj->getPCode() == LL_PCODE_VOLUME))
            {
                if(vobj->getAvatar() && vobj->getAvatar()->isTooComplex() && vobj->getAvatar()->isTooSlow())
                {
                    upd = false;
                }

                LLVOVolume* vvo = (LLVOVolume *)vobj;
                if (!LLPipeline::sRenderAttachedParticles && vvo && vvo->isAttachment())
                {
                    upd = false;
                }
            }

            if (upd)
            {
                mViewerPartSources[i]->update(dt);
            }
        }

        if (mViewerPartSources[i]->isDead())
        {
            mViewerPartSources.erase(mViewerPartSources.begin() + i);
            count--;
            i+=deldir;
        }
        else
        {
             i += dir;
        }
        num_updates++;
    }

    const unsigned long long census_t1 = __rdtsc();
    sSimCensus.mSrcCycles += census_t1 - census_t0;
    count = (S32) mViewerPartGroups.size();
    sSimCensus.mGroups += (U64)count;
    for (i = 0; i < count; i++)
    {
        sSimCensus.mRecs += mViewerPartGroups[i]->mRecords.size();
        LLViewerObject* vobj = mViewerPartGroups[i]->mVOPartGroupp;

        S32 visirate = 1;
        if (vobj && !vobj->isDead() && vobj->mDrawable && !vobj->mDrawable->isDead())
        {
            LLSpatialGroup* group = vobj->mDrawable->getSpatialGroup();
            if (group && !group->isVisible()) // && !group->isState(LLSpatialGroup::OBJECT_DIRTY))
            {
                visirate = 8;
            }
        }

        if ((LLDrawable::getCurrentFrame()+mViewerPartGroups[i]->mID)%visirate == 0)
        {
            // <FS:CR> FIRE-11593: Opensim "4096 Bug" Fix by Latif Khalifa
            // <vobj && !vobj->isDead())
            if (vobj && !vobj->isDead() && vobj->mDrawable)
            {
                gPipeline.markRebuild(vobj->mDrawable, LLDrawable::REBUILD_ALL);
            }
            mViewerPartGroups[i]->updateParticles(dt * visirate);
            mViewerPartGroups[i]->mSkippedTime=0.0f;
            if (!mViewerPartGroups[i]->getCount())
            {
                delete mViewerPartGroups[i];
                mViewerPartGroups.erase(mViewerPartGroups.begin() + i);
                i--;
                count--;
            }
        }
        else
        {
            mViewerPartGroups[i]->mSkippedTime+=dt;
            ++sSimCensus.mGroupsSkipped;
        }

    }
    sSimCensus.mGrpCycles += __rdtsc() - census_t1;
    if (++sSimCensus.mFrames % 600 == 0)
    {
        const double f = 600.0, k = 1000.0, st = std::max<double>(1.0, (double)sSimCensus.mStepped);
        LL_INFOS("Q1Census") << "particle simulation per frame over 600: sources loop " << sSimCensus.mSrcCycles / f / k << " kcycles (" << sSimCensus.mSources / f << " sources), groups loop " << sSimCensus.mGrpCycles / f / k << " kcycles ("
            << sSimCensus.mGroups / f << " groups, " << sSimCensus.mGroupsSkipped / f << " skipped as not visible), particles stepped " << sSimCensus.mStepped / f << ", records " << sSimCensus.mRecs / f << ", spawned " << sSimCensus.mSpawned / f
            << "; share of the stepped particles: analytic (no wind / target / bounce / callback) " << 100.0 * sSimCensus.mAnalytic / st << "%, of those with nothing from the source either " << 100.0 * sSimCensus.mAnalyticSrc / st
            << "%; follow source " << 100.0 * sSimCensus.mFollowSrc / st << "%, wind " << 100.0 * sSimCensus.mWind / st << "%, target pos " << 100.0 * sSimCensus.mTargetPos / st << "%, target linear " << 100.0 * sSimCensus.mTargetLinear / st
            << "%, bounce " << 100.0 * sSimCensus.mBounce / st << "%, callback " << 100.0 * sSimCensus.mCallback / st << "%, ribbon " << 100.0 * sSimCensus.mRibbon / st << "% (with a parent " << 100.0 * sSimCensus.mParented / st
            << "%), follow velocity " << 100.0 * sSimCensus.mFollowVel / st << "%, interp colour " << 100.0 * sSimCensus.mInterpColor / st << "%, interp scale " << 100.0 * sSimCensus.mInterpScale / st << "%, emissive " << 100.0 * sSimCensus.mEmissive / st
            << "%; mean max age " << sSimCensus.mAgeSum / st / 1000.0 << " s" << LL_ENDL;
        if (sSimCensus.mCmpN)
        {
            LL_INFOS("Q1Census") << "closed form against the stepped state, " << sSimCensus.mCmpN << " particle steps compared: largest position difference " << sSimCensus.mCmpPos << " m (at age " << sSimCensus.mCmpPosAge << " s; "
                << sSimCensus.mCmpPosOver << " steps over 1 mm), velocity " << sSimCensus.mCmpVel << " m/s, colour " << sSimCensus.mCmpColor << " (1.0 = full scale), scale " << sSimCensus.mCmpScale << " m, glow " << sSimCensus.mCmpGlow << " of 255" << LL_ENDL;
        }
        sSimCensus = PartSimCensus();
    }
    if (LLDrawable::getCurrentFrame()%16==0)
    {
        if (sParticleCount > sMaxParticleCount * 0.875f
             && sParticleAdaptiveRate < 2.0f)
        {
            sParticleAdaptiveRate *= PART_ADAPT_RATE_MULT;
        }
        else
        {
            if ( sParticleCount < sMaxParticleCount * 0.5f
                && sParticleAdaptiveRate > 0.03125f)
            {
                sParticleAdaptiveRate *= PART_ADAPT_RATE_MULT_RECIP;
            }
        }
    }

    updatePartBurstRate() ;

    //LL_INFOS() << "Particles: " << sParticleCount << " Adaptive Rate: " << sParticleAdaptiveRate << LL_ENDL;
}

void LLViewerPartSim::updatePartBurstRate()
{
    if (!(LLDrawable::getCurrentFrame() & 0xf))
    {
        if (sParticleCount >= MAX_PART_COUNT) //set rate to zero
        {
            sParticleBurstRate = 0.0f ;
        }
        else if(sParticleCount > 0)
        {
            if(sParticleBurstRate > 0.0000001f)
            {
                F32 total_particles = sParticleCount / sParticleBurstRate ; //estimated
                F32 new_rate = llclamp(0.9f * sMaxParticleCount / total_particles, 0.0f, 1.0f) ;
                F32 delta_rate_threshold = llmin(0.1f * llmax(new_rate, sParticleBurstRate), 0.1f) ;
                F32 delta_rate = llclamp(new_rate - sParticleBurstRate, -1.0f * delta_rate_threshold, delta_rate_threshold) ;

                sParticleBurstRate = llclamp(sParticleBurstRate + 0.5f * delta_rate, 0.0f, 1.0f) ;
            }
            else
            {
                sParticleBurstRate += 0.0000001f ;
            }
        }
        else
        {
            sParticleBurstRate += 0.00125f ;
        }
    }
}

void LLViewerPartSim::addPartSource(LLPointer<LLViewerPartSource> sourcep)
{
    if (!sourcep)
    {
        LL_WARNS() << "Null part source!" << LL_ENDL;
        return;
    }
    sourcep->setStart() ;
    mViewerPartSources.push_back(sourcep);
}

void LLViewerPartSim::removeLastCreatedSource()
{
    mViewerPartSources.pop_back();
}

void LLViewerPartSim::cleanupRegion(LLViewerRegion *regionp)
{
    for (group_list_t::iterator i = mViewerPartGroups.begin(); i != mViewerPartGroups.end(); )
    {
        group_list_t::iterator iter = i++;

        if ((*iter)->getRegion() == regionp)
        {
            delete *iter;
            i = mViewerPartGroups.erase(iter);
        }
    }
}

void LLViewerPartSim::clearParticlesByID(const U32 system_id)
{
    for (group_list_t::iterator g = mViewerPartGroups.begin(); g != mViewerPartGroups.end(); ++g)
    {
        (*g)->removeParticlesByID(system_id);
    }
    for (source_list_t::iterator i = mViewerPartSources.begin(); i != mViewerPartSources.end(); ++i)
    {
        if ((*i)->getID() == system_id)
        {
            (*i)->setDead();
            break;
        }
    }

}

void LLViewerPartSim::clearParticlesByOwnerID(const LLUUID& task_id)
{
    for (source_list_t::iterator iter = mViewerPartSources.begin(); iter != mViewerPartSources.end(); ++iter)
    {
        if ((*iter)->getOwnerUUID() == task_id)
        {
            clearParticlesByID((*iter)->getID());
        }
    }
}

