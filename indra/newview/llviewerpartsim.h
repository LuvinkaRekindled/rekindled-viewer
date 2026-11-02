/**
 * @file llviewerpartsim.h
 * @brief LLViewerPart class header file
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

#ifndef LL_LLVIEWERPARTSIM_H
#define LL_LLVIEWERPARTSIM_H

#include "llframetimer.h"
#include "llpointer.h"
#include "llpartdata.h"
#include "llviewerpartsource.h"

class LLViewerTexture;
class LLVertexBuffer;
class LLViewerPart;
class LLViewerRegion;
class LLVOPartGroup;

#define LL_MAX_PARTICLE_COUNT 8192

typedef void (*LLVPCallback)(LLViewerPart &part, const F32 dt);

///////////////////
//
// An individual particle
//


class LLViewerPart : public LLPartData
{
public:
    ~LLViewerPart();
public:
    LLViewerPart();

    void init(LLPointer<LLViewerPartSource> sourcep, LLViewerTexture *imagep, LLVPCallback cb);


    U32                 mPartID;                    // Particle ID used primarily for moving between groups
    F32                 mLastUpdateTime;            // Last time the particle was updated
    F32                 mSkipOffset;                // Offset against current group mSkippedTime

    LLVPCallback        mVPCallback;                // Callback function for more complicated behaviors
    LLPointer<LLViewerPartSource> mPartSourcep;     // Particle source used for this object

    LLViewerPart*       mParent;                    // particle to connect to if this is part of a particle ribbon
    LLViewerPart*       mChild;                     // child particle for clean reference destruction

    // Current particle state (possibly used for rendering)
    LLPointer<LLViewerTexture>  mImagep;
    LLVector3       mPosAgent;
    LLVector3       mVelocity;
    LLVector3       mAccel;
    LLVector3       mSpawnPos;                  // <FS> GPU particles stage 2 (PLAN-gpu-particles.md): the state when the source made the particle: a particle with no per-frame input is a function of these and its age
    LLVector3       mSpawnVel;
    bool            mSpawnKnown = false;
    LLVector3       mAxis;
    LLColor4        mColor;
    LLVector2       mScale;
    F32             mStartGlow;
    F32             mEndGlow;
    LLColor4U       mGlow;


    static U32      sNextPartID;
};



// <FS> GPU particles stage 2 (doc/vulkan/PLAN-gpu-particles.md): a particle with no per-frame input (no wind, target, bounce, follow source, ribbon, callback) is not stepped: its position,
// velocity, colour, scale and glow are functions of its age (the closed form step 1 compared against the stepping: colour, scale and glow equal, position within the stepping's float rounding).
// The group keeps it as a record; LLVOPartGroup::updateRows evaluates it.
struct LLViewerPartRecord
{
    LLVector3 mPos0, mVel0, mAccel;
    F64 mT0 = 0.0;                          // the simulation clock (LLViewerPartSim::getSimTime) when the first step began
    F32 mMaxAge = 0.f;
    U32 mFlags = 0;
    LLColor4 mStartColor, mEndColor;
    LLVector2 mStartScale, mEndScale;
    F32 mStartGlow = 0.f, mEndGlow = 0.f;
    U8 mBlendSrc = 0, mBlendDst = 0;
    LLPointer<LLViewerTexture> mImagep;
    LLPointer<LLViewerPartSource> mSourcep;
    // the laws beyond the free flight (each with the source's position of the frame; the stepping did them one step at a time)
    enum Mode : U8
    {
        FREE = 0,           // pos0 + vel0 * age + accel * age^2 / 2
        FOLLOW,             // the source's position + vel0 * age + accel * age^2 / 2 (the stepping puts the particle at its source plus its offset each step: the offset is the free flight from zero)
        TARGET_LINEAR,      // the source's position + age / max age of the way to the source's target
        TARGET_POS          // the stepping's pull towards the target (velocity moves towards the speed that gets there in the time left) solved: the velocity error falls with (time left)^4
    };
    U8 mClass = 0;          // G1: the draw class of the group (texture, blend pair, fullbright) the record belongs to: the sort puts a class together, the draw is one a class
    U8 mSrcLocal = 0;       // G0: the number of the source in the group's list (mSources), what the shader's source table is indexed by
    U8 mMode = FREE;
    bool mBounce = false;   // bounces at the height of the floor (FREE: the source's height when the particle was made; FOLLOW: the source's): z mirrored, vertical speed * -0.75
    F32 mFloorZ = 0.f;
    LLVector3 mTarget0;     // TARGET_POS: the target when the particle was made (the particle keeps its place relative to a target that moves)

    // position and velocity at the simulation clock `now`
    void evaluate(F64 now, LLVector3& pos, LLVector3& vel) const;
};

// the stepping's interpolations as they are written there (start * (1 - f) + end * f, a channel at a time), so the closed form and the stepping agree to the last bit
inline LLColor4 fs_part_color_at(const LLColor4& start, const LLColor4& end, F32 f)
{
    LLColor4 c;
    for (int k = 0; k < 4; ++k)
    {
        c.mV[k] = start.mV[k] * (1.f - f) + end.mV[k] * f;
    }
    return c;
}

inline LLVector2 fs_part_scale_at(const LLVector2& start, const LLVector2& end, F32 f)
{
    return LLVector2(start.mV[0] * (1.f - f) + f * end.mV[0], start.mV[1] * (1.f - f) + f * end.mV[1]);
}

class LLViewerPartGroup
{
public:
    LLViewerPartGroup(const LLVector3 &center,
                      const F32 box_radius,
                      bool hud);
    virtual ~LLViewerPartGroup();

    void cleanup();

    bool addPart(LLViewerPart* part, const F32 desired_size = -1.f);

    void updateParticles(const F32 lastdt);

    bool posInGroup(const LLVector3 &pos, const F32 desired_size = -1.f);

    void shift(const LLVector3 &offset);

    F32 getBoxRadius() { return mBoxRadius; }
    F32 getBoxSide() { return mBoxSide; }

    typedef std::vector<LLViewerPart*>  part_list_t;
    part_list_t mParticles;

    // <FS> GPU particles stage 2: the particles that are functions of their age (mDeadAt: the clock after which each is gone, -1 = removed by its source's id)
    std::vector<LLViewerPartRecord> mRecords;
    std::vector<F64> mDeadAt;
    bool addRecord(const LLViewerPart& part, const F32 desired_size, int mode);
    bool expireRecords();                   // true: some went

    // <FS> GPU particles G0: the records for the GPU. mStore: a vertex buffer with a record a slot (slot = the record's place in mRecords), its index data is the order of the slots of the frame
    // (written by LLParticlePartition::rebuildRows); mSources: the distinct sources of the records (at most 255; more and the group is drawn from rows).
    std::vector<LLPointer<LLViewerPartSource> > mSources;
    // G1: up to four draw classes; the GPU sort orders the store by (class, distance back to front), the CPU knows each class's range from the live counts
    struct ClassKey { LLPointer<LLViewerTexture> mTex; U8 mSrc = 0, mDst = 0; bool mFullbright = false; };
    std::vector<ClassKey> mClasses;
    U32 mClassCount[4] = {};
    U32 mClassGlow[4] = {};                 // ... of which records with a glow (start or end above 0): a class without any draws no glow pass
    // G1 step 2: what the group's extents, texture priority and draws need without evaluating the records: how far each kind of record can be from where it starts (a ball round its spawn point,
    // round its source for the follow source law, between the source and its target for the linear law, round the target for the pull), kept as grow-only aggregates and made again from the live
    // records now and then
    struct SrcAgg { F32 mFollowReach = 0.f, mTpReach = 0.f; bool mTl = false; };
    std::vector<SrcAgg> mSrcAgg;            // parallel to mSources
    LLVector3 mBoxLo, mBoxHi;               // the balls round the spawn points of the free flights (and the pull's)
    bool mBoxValid = false;
    F32 mMaxScale = 0.f;                    // the largest scale of any record
    U32 mAggAge = 0;
    void noteRecordReach(const LLViewerPartRecord& rec);
    void rebuildAggregates();
    bool pathBox(LLVector3& lo, LLVector3& hi) const;   // the box of the group's particles now (false: no records)
    LLPointer<LLVertexBuffer> mStore;
    LLPointer<LLVertexBuffer> mRows;        // G1: the rows the GPU writes from the store each frame (sorted, evaluated), what the draws read
    U32 mStoreCap = 0;
    std::vector<U32> mDirtySlots;           // slots whose record is to be written to the store
    bool mStoreAll = true;                  // every slot is to be written (a new store)
    bool mNoGpu = false;                    // too many sources: rows
    bool syncStore();                       // the store has room for the records and holds them; false: no store (the records are drawn from rows)

    const LLVector3 &getCenterAgent() const     { return mCenterAgent; }
    S32 getCount() const                    { return (S32) (mParticles.size() + mRecords.size()); }
    LLViewerRegion *getRegion() const       { return mRegionp; }

    void removeParticlesByID(const U32 source_id);

    LLPointer<LLVOPartGroup> mVOPartGroupp;

    bool mUniformParticles;
    U32 mID;

    F32 mSkippedTime;
    bool mHud;

protected:
    LLVector3 mCenterAgent;
    F32 mBoxRadius;
    F32 mBoxSide;
    LLVector3 mMinObjPos;
    LLVector3 mMaxObjPos;

    LLViewerRegion *mRegionp;
};

class LLViewerPartSim : public LLSingleton<LLViewerPartSim>
{
    LLSINGLETON(LLViewerPartSim);
public:
    void destroyClass();

    typedef std::vector<LLViewerPartGroup *> group_list_t;
    typedef std::vector<LLPointer<LLViewerPartSource> > source_list_t;

    void enable(bool enabled);

    void shift(const LLVector3 &offset);

    void updateSimulation();
    void sortStores();                      // G1: one worker record a frame (kind 36): the GPU sorts the order of every record store (main view only)

    void addPartSource(LLPointer<LLViewerPartSource> sourcep);

    void cleanupRegion(LLViewerRegion *regionp);

    static bool shouldAddPart(); // Just decides whether this particle should be added or not (for particle count capping)
    F32 maxRate() // Return maximum particle generation rate
    {
        if (sParticleCount >= MAX_PART_COUNT)
        {
            return 1.f;
        }
        if (sParticleCount > PART_THROTTLE_THRESHOLD*sMaxParticleCount)
        {
            return (((F32)sParticleCount/(F32)sMaxParticleCount)-PART_THROTTLE_THRESHOLD)*PART_THROTTLE_RESCALE;
        }
        return 0.f;
    }
    static F64 getSimTime() { return sSimTime; }
    F32 getRefRate() { return sParticleAdaptiveRate; }
    F32 getBurstRate() {return sParticleBurstRate; }
    void addPart(LLViewerPart* part);
    void updatePartBurstRate() ;
    void clearParticlesByID(const U32 system_id);
    void clearParticlesByOwnerID(const LLUUID& task_id);
    void removeLastCreatedSource();

    const source_list_t* getParticleSystemList() const { return &mViewerPartSources; }

    friend class LLViewerPartGroup;

    bool aboveParticleLimit() const { return sParticleCount > sMaxParticleCount; }
    static void setMaxPartCount(const S32 max_parts)    { sMaxParticleCount = max_parts; }
    static S32  getMaxPartCount()                       { return sMaxParticleCount; }

    // <FS:Beq> FIRE-34600 - bugsplat AVX2 particle count mismatch
    // Deprecate these and use native operators for consistency
    // static void incPartCount(const S32 count)           { sParticleCount += count; }
    // static void decPartCount(const S32 count)           { sParticleCount -= count; }
    // </FS:Beq>
    U32 mID;

protected:
    LLViewerPartGroup *createViewerPartGroup(const LLVector3 &pos_agent, const F32 desired_size, bool hud);
    LLViewerPartGroup *put(LLViewerPart* part, bool as_record = false, int record_mode = 0);

    group_list_t mViewerPartGroups;
    source_list_t mViewerPartSources;
    LLFrameTimer mSimulationTimer;

    static S32 sMaxParticleCount;
    static F64 sSimTime;                    // <FS> GPU particles stage 2: the simulation clock (the sum of the frames' dt, each at most 0.1 s)
    static F32 sSimDt;
    static std::atomic<S32> sRecordCount;   // ... of sParticleCount, the particles that are records
    static std::atomic<S32> sParticleCount; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch
    static F32 sParticleAdaptiveRate;
    static F32 sParticleBurstRate;

    static const S32 MAX_PART_COUNT;
    static const F32 PART_THROTTLE_THRESHOLD;
    static const F32 PART_THROTTLE_RESCALE;
    static const F32 PART_ADAPT_RATE_MULT;
    static const F32 PART_ADAPT_RATE_MULT_RECIP;

//debug use only
public:
    static std::atomic<S32> sParticleCount2; // <FS:Beq/> FIRE-34600 - bugsplat AVX2 particle count mismatch

    static void checkParticleCount(U32 size = 0) ;
};

#endif // LL_LLVIEWERPARTSIM_H
