/**
 * @file llvopartgroup.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llvopartgroup.h"

#include "lldrawpoolalpha.h"

#include "llfasttimer.h"
#include "message.h"
#include "v2math.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewerpartsim.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "lldiag.h"        // <FS> LL_DIAG: the statistics lines are off unless LL_VK_DIAG is set

extern U64MicrosecondsImplicit gFrameTime;

void LLVOPartGroup::initClass()
{
}

//static
void LLVOPartGroup::restoreResources()
{

}

//static
void LLVOPartGroup::destroyResources()
{
}

bool ll_is_part_idx_allocated(S32 idx, S32* start, S32* end)
{
    /*while (start < end)
    {
        if (*start == idx)
        { //not allocated (in free list)
            return false;
        }
        ++start;
    }*/

    //allocated (not in free list)
    // <FS> Fix particle flashing
    //return false;

    while (start < end)
    {
        if (*start == idx)
        { //not allocated (in free list)
            return false;
        }
        ++start;
    }

    //allocated (not in free list)
    return true;
    // </FS>
}

LLVOPartGroup::LLVOPartGroup(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    :   LLAlphaObject(id, pcode, regionp),
        mViewerPartGroupp(NULL)
{
    setNumTEs(1);
    setTETexture(0, LLUUID::null);
    mbCanSelect = false;            // users can't select particle systems
}


LLVOPartGroup::~LLVOPartGroup()
{
}

bool LLVOPartGroup::isActive() const
{
    return false;
}

F32 LLVOPartGroup::getBinRadius()
{
    return mViewerPartGroupp->getBoxSide();
}

void LLVOPartGroup::updateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
    const LLVector3& pos_agent = getPositionAgent();

    LLVector4a scale;
    LLVector4a p;

    p.load3(pos_agent.mV);

    scale.splat(mScale.mV[0]+mViewerPartGroupp->getBoxSide()*0.5f);

    newMin.setSub(p, scale);
    newMax.setAdd(p,scale);
    if (mEvalValid)
    {   // <FS> stage 2: a record is not moved to another group when it leaves this one's box: the extents hold where the drawn particles are
        LLVector4a lo, hi, pad;
        lo.load3(mEvalMin.mV);
        hi.load3(mEvalMax.mV);
        pad.splat(mScale.mV[0]);
        lo.sub(pad);
        hi.add(pad);
        newMin.setMin(newMin, lo);
        newMax.setMax(newMax, hi);
    }

    llassert(newMin.isFinite3());
    llassert(newMax.isFinite3());

    llassert(p.isFinite3());
    mDrawable->setPositionGroup(p);
}

void LLVOPartGroup::idleUpdate(LLAgent &agent, const F64 &time)
{
}

void LLVOPartGroup::setPixelAreaAndAngle(LLAgent &agent)
{
    // mPixelArea is calculated during render
    F32 mid_scale = getMidScale();
    F32 range = (getRenderPosition()-LLViewerCamera::getInstance()->getOrigin()).length();

    if (range < 0.001f || isHUDAttachment())        // range == zero
    {
        mAppAngle = 180.f;
    }
    else
    {
        mAppAngle = (F32) atan2( mid_scale, range) * RAD_TO_DEG;
    }
}

void LLVOPartGroup::updateTextures()
{
    // Texture stats for particles need to be updated in a different way...
}


LLDrawable* LLVOPartGroup::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_PARTICLES);
    return mDrawable;
}

 const F32 MAX_PARTICLE_AREA_SCALE = 0.02f; // some tuned constant, limits on how much particle area to draw

 LLUUID LLVOPartGroup::getPartOwner(S32 idx)
 {
     LLUUID ret = LLUUID::null;

     if (idx < (S32) mViewerPartGroupp->mParticles.size())
     {
         ret = mViewerPartGroupp->mParticles[idx]->mPartSourcep->getOwnerUUID();
     }
     else if (idx - (S32) mViewerPartGroupp->mParticles.size() < (S32) mViewerPartGroupp->mRecords.size())
     {
         const LLViewerPartRecord& rec = mViewerPartGroupp->mRecords[idx - mViewerPartGroupp->mParticles.size()];
         if (rec.mSourcep.notNull())
         {
             ret = rec.mSourcep->getOwnerUUID();
         }
     }

     return ret;
 }

 LLUUID LLVOPartGroup::getPartSource(S32 idx)
 {
     LLUUID ret = LLUUID::null;

     if (idx < (S32) mViewerPartGroupp->mParticles.size())
     {
         LLViewerPart* part = mViewerPartGroupp->mParticles[idx];
         if (part && part->mPartSourcep.notNull() &&
             part->mPartSourcep->mSourceObjectp.notNull())
         {
             LLViewerObject* source = part->mPartSourcep->mSourceObjectp;
             ret = source->getID();
         }
     }
     else if (idx - (S32) mViewerPartGroupp->mParticles.size() < (S32) mViewerPartGroupp->mRecords.size())
     {
         const LLViewerPartRecord& rec = mViewerPartGroupp->mRecords[idx - mViewerPartGroupp->mParticles.size()];
         if (rec.mSourcep.notNull() && rec.mSourcep->mSourceObjectp.notNull())
         {
             ret = rec.mSourcep->mSourceObjectp->getID();
         }
     }

     return ret;
 }


F32 LLVOPartGroup::getPartSize(S32 idx)
{
    if (idx < (S32) mViewerPartGroupp->mParticles.size())
    {
        return mViewerPartGroupp->mParticles[idx]->mScale.mV[0];
    }
    for (const LLPartEval& r : mEval)
    {
        if (r.mIndex == idx)
        {
            return 2.f * r.mHalfX;
        }
    }
    {
        const S32 ri = idx - (S32)mViewerPartGroupp->mParticles.size();
        if (ri >= 0 && ri < (S32)mViewerPartGroupp->mRecords.size())
        {
            const LLViewerPartRecord& rec = mViewerPartGroupp->mRecords[ri];
            const F32 frac = (F32)(LLViewerPartSim::getSimTime() - rec.mT0) / rec.mMaxAge;
            return ((rec.mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK) ? fs_part_scale_at(rec.mStartScale, rec.mEndScale, frac) : rec.mStartScale).mV[0];
        }
    }

    return 0.f;
}

void LLVOPartGroup::getBlendFunc(S32 idx, LLRender::eBlendFactor& src, LLRender::eBlendFactor& dst)
{
    if (idx < (S32) mViewerPartGroupp->mParticles.size())
    {
        LLViewerPart* part = mViewerPartGroupp->mParticles[idx];
        src = (LLRender::eBlendFactor) part->mBlendFuncSource;
        dst = (LLRender::eBlendFactor) part->mBlendFuncDest;
    }
}

LLVector3 LLVOPartGroup::getCameraPosition() const
{
    return gAgentCamera.getCameraPositionAgent();
}

// <FS> GPU particles stage 1 (doc/vulkan/PLAN-gpu-particles.md): where a particle group rebuild spends its time (log tag Q1Census, every 3000 rebuilds)
static struct ParticleRebuildStats
{
    U64 mRebuilds = 0, mParticles = 0, mCount = 0, mSort = 0, mCorners = 0, mRest = 0, mTotal = 0, mGeomCalls = 0, mGeomParts = 0, mGeomCycles = 0, mRows = 0, mRowsCycles = 0, mRowsParticles = 0, mBailRibbon = 0, mBailRibbonParts = 0, mHud = 0, mHudParts = 0, mRGather = 0, mRClear = 0, mRSort = 0, mRBuffer = 0, mRWrite = 0, mRRibbons = 0, mGpuGroups = 0, mFastGroups = 0, mNoMulti = 0, mNoImpure = 0, mNoRibbon = 0, mCGroups = 0, mCClasses = 0, mCMulti = 0, mCTextures = 0, mCRuns = 0, mCParts = 0, mCMixedRuns = 0, mCClassRuns = 0;
} sPartStats;

bool ll_particle_sort_on();

// GPU particles stage 1: LL_VK_GPU_PARTICLES=0 builds the quads on the CPU as before (default on)
static bool fs_gpu_particles_on()
{
    static const bool on = []() { const char* v = getenv("LL_VK_GPU_PARTICLES"); return !v || atoi(v) != 0; }();
    return on;
}

static void fs_particle_stats_log()
{
    const double n = 3000.0, k = 1000.0;
    const double legacy = std::max<double>(1.0, n - (double)sPartStats.mRows);
    const double rr = std::max<double>(1.0, (double)sPartStats.mRows);
    if (sPartStats.mGpuGroups)
    {
        LL_DIAG("Q1Census") << "particle groups drawn from rows the sort pass wrote (nothing evaluated on the CPU): " << sPartStats.mFastGroups << " of " << sPartStats.mRows << " rebuilds" << LL_ENDL;
    }
    if (sPartStats.mCGroups)
    {
        const double g = (double)sPartStats.mCGroups;
        LL_DIAG("Q1Census") << "particle groups as the GPU sort would see them (" << sPartStats.mCGroups << " rebuilds): draw states (blend pair + fullbright) a group " << sPartStats.mCClasses / g << ", groups with more than one " << 100.0 * sPartStats.mCMulti / g
            << "%, textures a group " << sPartStats.mCTextures / g << ", draw infos (runs of one texture and state in the sorted order) a group " << sPartStats.mCRuns / g << " for " << sPartStats.mCParts / g << " particles, runs of one state (textures ignored) a group "
            << sPartStats.mCClassRuns / g << LL_ENDL;
    }
    LL_DIAG("Q1Census") << "rows path kcycles a rebuild: gather " << sPartStats.mRGather / rr / k << ", clearDrawMap " << sPartStats.mRClear / rr / k << ", sort " << sPartStats.mRSort / rr / k << ", buffer " << sPartStats.mRBuffer / rr / k
        << ", rows + draw infos " << sPartStats.mRWrite / rr / k << " (ribbon particles a rebuild " << sPartStats.mRRibbons / rr << ")" << LL_ENDL;
    LL_DIAG("Q1Census") << "particle group rebuild, kcycles a rebuild over 3000 (" << sPartStats.mRows << " as rows for the GPU, " << (U64)legacy << " as quads): rows " << (sPartStats.mRows ? sPartStats.mRowsCycles / (double)sPartStats.mRows / k : 0.0)
        << " (" << (sPartStats.mRows ? sPartStats.mRowsParticles / (double)sPartStats.mRows : 0.0) << " particles) | quads: total " << sPartStats.mTotal / legacy / k << " = face count " << sPartStats.mCount / legacy / k << " + sort " << sPartStats.mSort / legacy / k
        << " + corner math " << sPartStats.mCorners / legacy / k << " + the rest " << (sPartStats.mTotal - sPartStats.mCount - sPartStats.mSort - sPartStats.mCorners) / legacy / k
        << " (" << sPartStats.mParticles / legacy << " particles); of the quads: " << sPartStats.mBailRibbon << " groups left with a ribbon particle (" << (sPartStats.mBailRibbon ? sPartStats.mBailRibbonParts / (double)sPartStats.mBailRibbon : 0.0) << " particles each), "
        << sPartStats.mHud << " HUD groups (" << (sPartStats.mHud ? sPartStats.mHudParts / (double)sPartStats.mHud : 0.0) << " each); LLVOPartGroup::updateGeometry in the same window: " << sPartStats.mGeomCalls << " calls, " << (sPartStats.mGeomCalls ? sPartStats.mGeomCycles / (double)sPartStats.mGeomCalls / k : 0.0) << " kcycles a call" << LL_ENDL;
    sPartStats = ParticleRebuildStats();
}

bool LLVOPartGroup::updateGeometry(LLDrawable *drawable)
{
    LL_PROFILE_ZONE_SCOPED;
    struct GeomTimer { U64 t0 = __rdtsc(); ~GeomTimer() { sPartStats.mGeomCycles += __rdtsc() - t0; ++sPartStats.mGeomCalls; } } geom_timer;

    dirtySpatialGroup();

    S32 num_parts = mViewerPartGroupp->getCount();
    LLSpatialGroup* group = drawable->getSpatialGroup();
    if (!group && num_parts)
    {
        drawable->movePartition();
        group = drawable->getSpatialGroup();
    }

    if (group && group->isVisible())
    {
        dirtySpatialGroup();
    }

    if (!num_parts)
    {
        mEval.clear();
        mEvalValid = false;
        if (group && drawable->getNumFaces())
        {
            group->setState(LLSpatialGroup::GEOM_DIRTY);
        }
        drawable->setNumFaces(0, NULL, getTEImage(0));
        LLPipeline::sCompiles++;
        return true;
    }

    if (!(gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES)))
    {
        return true;
    }

    if (fs_gpu_particles_on() && getPartitionType() == LLViewerRegion::PARTITION_PARTICLE && updateRows(drawable, num_parts))
    {
        mDrawable->movePartition();
        LLPipeline::sCompiles++;
        return true;
    }
    return updateFacesLegacy(drawable, num_parts);
}

// <FS> GPU particles stage 1b / 2: the cuts of updateGeometry without a face a particle, and the particles of the group (the stepped ones and the records at this frame's clock) as LLPartEval rows.
// False: a stepped particle of the group shows the parcel media (its face takes the media texture): the quads path (not while the group has records, which the quads path cannot draw).
bool LLVOPartGroup::updateRows(LLDrawable* drawable, S32 num_parts)
{
    F32 tot_area = 0;
    F32 max_area = LLViewerPartSim::getMaxPartCount() * MAX_PARTICLE_AREA_SCALE;
    F32 pixel_meter_ratio = LLViewerCamera::getInstance()->getPixelMeterRatio();
    pixel_meter_ratio *= pixel_meter_ratio;

    LLViewerPartSim::checkParticleCount(static_cast<U32>(mViewerPartGroupp->mParticles.size()));

    mDepth = 0.f;
    const LLVector3 camera_agent = getCameraPosition();
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 origin = camera->getOrigin();
    const LLVector3 at_axis = camera->getAtAxis();
    F32 max_scale = 0.f;
    F32 drawn_area = 0.f;
    static std::vector<LLViewerTexture*> textures;
    textures.clear();
    mEval.clear();
    LLViewerTexture* last_texture = nullptr;
    bool any_texture = false;
    LLVector3 box_min(FLT_MAX, FLT_MAX, FLT_MAX), box_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const LLViewerPartGroup::part_list_t& list = mViewerPartGroupp->mParticles;
    const S32 particles = (S32)list.size();
    const bool has_records = !mViewerPartGroupp->mRecords.empty();
    bool stop = false;
    mPure = false;
    // G1 step 2: a group of records only, with the GPU sort on: nothing is evaluated here. The extents are the box of the group's path aggregates (and the sources' positions now), the texture
    // priority comes from the classes, the cuts (size, area) are the shader's
    bool alone = false;
    if (ll_particle_sort_on() && has_records && particles == 0 && !mViewerPartGroupp->mNoGpu)
    {
        // (the draws of the sort pass are one buffer: a group alone in its spatial group; with another part group the CPU makes the rows of both and sorts them together)
        LLSpatialGroup* sg = drawable->getSpatialGroup();
        if (sg)
        {
            U32 live = 0;
            for (LLSpatialGroup::element_iter it = sg->getDataBegin(); it != sg->getDataEnd(); ++it)
            {
                LLDrawable* d = (LLDrawable*)(*it)->getDrawable();
                live += (d && !d->isDead()) ? 1 : 0;
            }
            alone = live == 1;
        }
    }
    const bool fast_path = alone && mViewerPartGroupp->syncStore();
    bool fast_box = false;
    if (fast_path)
    {
        LLViewerPartGroup* g = mViewerPartGroupp;
        if (++g->mAggAge % 120 == 0)
        {
            g->rebuildAggregates();     // (the aggregates only grow between: made again from the live records twice a second or so)
        }
        LLVector3 lo, hi;
        fast_box = g->pathBox(lo, hi);
        if (fast_box)
        {
            box_min = lo;
            box_max = hi;
        }
        max_scale = g->mMaxScale;
        for (U32 c = 0; c < g->mClasses.size() && c < 4; ++c)
        {
            if (g->mClassCount[c])
            {
                LLViewerTexture* tex = g->mClasses[c].mTex.get();
                bool known = false;
                for (LLViewerTexture* t : textures)
                {
                    known = known || t == tex;
                }
                if (!known)
                {
                    textures.push_back(tex);
                }
            }
        }
        const LLVector3 center = fast_box ? (lo + hi) * 0.5f : camera_agent;
        const F32 d2 = (center - camera_agent).lengthSquared();
        drawn_area = max_scale * max_scale * (d2 > 1.f ? 1.f / d2 : 1.f);
    }

    const F32 NEAR_PART_DIST_SQ = 5.f*5.f;  // Only discard particles > 5 m from the camera
    const F32 MIN_PART_AREA = .005f*.005f;  // only less than 5 mm x 5 mm at 1 m from camera
    // the cuts for a particle of either kind: 0 = no more (the area budget), 1 = not this one (too small and far), 2 = draw it
    auto visit = [&](const LLVector3& pos, F32 sx, F32 sy) -> int
    {
        const F32 camera_dist_squared = (pos - camera_agent).lengthSquared();
        const F32 inv_camera_dist_squared = camera_dist_squared > 1.f ? 1.f / camera_dist_squared : 1.f;
        const F32 area = sx * sy * inv_camera_dist_squared;
        tot_area = llmax(tot_area, area);
        if (tot_area > max_area)
        {
            return 0;
        }
        if (camera_dist_squared > NEAR_PART_DIST_SQ && area < MIN_PART_AREA)
        {
            return 1;
        }
        drawn_area = tot_area;
        return 2;
    };
    auto add_texture = [&](LLViewerTexture* texture)
    {
        if (!any_texture || texture != last_texture)
        {
            bool known = false;
            for (LLViewerTexture* t : textures)
            {
                if (t == texture)
                {
                    known = true;
                    break;
                }
            }
            if (!known)
            {
                textures.push_back(texture);
            }
            last_texture = texture;
            any_texture = true;
        }
    };
    auto grow = [&](const LLVector3& p)
    {
        for (int k = 0; k < 3; ++k)
        {
            box_min.mV[k] = llmin(box_min.mV[k], p.mV[k]);
            box_max.mV[k] = llmax(box_max.mV[k], p.mV[k]);
        }
    };

    for (S32 i = 0; i < particles && !fast_path; i++)
    {
        const LLViewerPart* part = list[i];

        max_scale = llmax(max_scale, part->mScale.mV[0], part->mScale.mV[1]);

        if (part->mFlags & LLPartData::LL_PART_RIBBON_MASK)
        {   //include ribbon segment length in scale
            const LLVector3* pos_agent = NULL;
            if (part->mParent)
            {
                pos_agent = &(part->mParent->mPosAgent);
            }
            else if (part->mPartSourcep.notNull())
            {
                pos_agent = &(part->mPartSourcep->mPosAgent);
            }

            if (pos_agent)
            {
                max_scale = llmax(max_scale, (*pos_agent - part->mPosAgent).length());
            }
        }

        const int verdict = visit(part->mPosAgent, part->mScale.mV[0], part->mScale.mV[1]);
        if (verdict == 0)
        {
            stop = true;
            break;
        }
        if (verdict == 1)
        {
            continue;
        }

        LLViewerTexture* texture = part->mImagep.get();
        if (!has_records && (!any_texture || texture != last_texture) && texture && texture->hasParcelMedia())
        {
            return false;
        }
        add_texture(texture);

        LLPartEval row;
        row.mPos = part->mPosAgent;
        row.mColor = part->mColor;
        row.mGlow = part->mGlow;
        row.mTex = texture;
        row.mBlendSrc = part->mBlendFuncSource;
        row.mBlendDst = part->mBlendFuncDest;
        row.mFullbright = (part->mFlags & LLPartData::LL_PART_EMISSIVE_MASK) != 0;
        row.mIndex = i;
        row.mDist = (part->mPosAgent - origin) * at_axis;
        if (part->mFlags & LLPartData::LL_PART_RIBBON_MASK)
        {
            // as LLVOPartGroup::getGeometry: the piece joins this particle to its parent (or to its source object, or to nothing)
            row.mRibbon = true;
            LLVector3 ppos = part->mPosAgent, paxis = part->mAxis;
            F32 pscale = part->mScale.mV[0];
            if (part->mParent)
            {
                ppos = part->mParent->mPosAgent;
                paxis = part->mParent->mAxis;
                pscale = part->mParent->mScale.mV[0];
                row.mPGlow = part->mParent->mGlow;
                row.mPColor = part->mParent->mColor;
            }
            else
            {
                if (part->mPartSourcep.notNull() && part->mPartSourcep->mSourceObjectp.notNull())
                {
                    LLVector3 v = LLVector3(0, 0, 1);
                    v *= part->mPartSourcep->mSourceObjectp->getRenderRotation();
                    paxis = v;
                    ppos = part->mPartSourcep->mPosAgent;
                    pscale = part->mStartScale.mV[0];
                }
                row.mPGlow = LLColor4U(0, 0, 0, (U8)ll_round(255.f * part->mStartGlow));
                row.mPColor = part->mStartColor;
            }
            row.mAxisHalf = part->mAxis * (0.5f * part->mScale.mV[0]);
            row.mPPos = ppos;
            row.mPAxisHalf = paxis * (0.5f * pscale);
            grow(ppos);
        }
        else
        {
            row.mVel = ((part->mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK) && !part->mVelocity.isExactlyZero()) ? part->mVelocity : LLVector3(0.f, 0.f, 0.f);
            row.mHalfX = 0.5f * part->mScale.mV[0];
            row.mHalfY = 0.5f * part->mScale.mV[1];
        }
        grow(part->mPosAgent);
        mEval.push_back(row);
    }
    if (!stop && !fast_path)
    {
        const F64 now = LLViewerPartSim::getSimTime();
        const S32 record_count = (S32)mViewerPartGroupp->mRecords.size();
        for (S32 ri = 0; ri < record_count; ri++)
        {
            const LLViewerPartRecord& rec = mViewerPartGroupp->mRecords[ri];
            const F32 age = (F32)(now - rec.mT0);
            if (age > rec.mMaxAge)
            {
                continue;       // (the simulation's sweep takes it out)
            }
            const F32 frac = age / rec.mMaxAge;
            LLVector3 pos, vel;
            rec.evaluate(now, pos, vel);
            const LLVector2 scale = (rec.mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK) ? fs_part_scale_at(rec.mStartScale, rec.mEndScale, frac) : rec.mStartScale;

            max_scale = llmax(max_scale, scale.mV[0], scale.mV[1]);

            const int verdict = visit(pos, scale.mV[0], scale.mV[1]);
            if (verdict == 0)
            {
                break;
            }
            if (verdict == 1)
            {
                continue;
            }

            LLViewerTexture* texture = rec.mImagep.get();
            add_texture(texture);

            LLPartEval row;
            row.mPos = pos;
            row.mColor = (rec.mFlags & LLPartData::LL_PART_INTERP_COLOR_MASK) ? fs_part_color_at(rec.mStartColor, rec.mEndColor, frac) : rec.mStartColor;
            row.mGlow = LLColor4U(0, 0, 0, (U8)ll_round(lerp(rec.mStartGlow, rec.mEndGlow, frac) * 255.f));
            row.mTex = texture;
            row.mBlendSrc = rec.mBlendSrc;
            row.mBlendDst = rec.mBlendDst;
            row.mFullbright = (rec.mFlags & LLPartData::LL_PART_EMISSIVE_MASK) != 0;
            row.mIndex = particles + ri;
            row.mDist = (pos - origin) * at_axis;
            if (rec.mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK)
            {
                row.mVel = vel.isExactlyZero() ? LLVector3(0.f, 0.f, 0.f) : vel;
            }
            row.mHalfX = 0.5f * scale.mV[0];
            row.mHalfY = 0.5f * scale.mV[1];
            grow(pos);
            mEval.push_back(row);
        }
    }
    mPixelArea = drawn_area * pixel_meter_ratio;
    if (fast_path)
    {
        mPure = true;       // (the record store holds every particle of the group: the draws can read it)
    }

    //record max scale (used to stretch bounding box for visibility culling)
    mScale.set(max_scale, max_scale, max_scale);
    mEvalValid = !mEval.empty() || fast_box;
    if (mEvalValid)
    {
        mEvalMin = box_min;
        mEvalMax = box_max;
    }

    // the faces: one a distinct texture, so the texture priority loop sees the texture a few times, not once a particle
    const S32 wanted = (S32)textures.size();
    S32 have = drawable->getNumFaces();
    if (wanted > have)
    {
        drawable->setNumFaces(wanted, NULL, getTEImage(0));
    }
    else if (have > 2 * wanted + 8)
    {
        drawable->setNumFaces(wanted, NULL, getTEImage(0));     // (the faces of the quads path, one a particle)
    }
    have = drawable->getNumFaces();
    const F32 area_scale = 10.f; // scale area to increase priority a bit
    for (S32 f = 0; f < have; ++f)
    {
        LLFace* facep = drawable->getFace(f);
        if (!facep)
        {
            continue;
        }
        if (f >= wanted)
        {
            facep->setTexture((LLViewerTexture*)nullptr);
            continue;
        }
        if (facep->getViewerObject() != this)
        {
            facep->setViewerObject(this);
        }
        if (facep->getTEOffset() != f)
        {
            facep->setTEOffset(f);
        }
        facep->setTexture(textures[f]);
        facep->setVirtualSize(mPixelArea*area_scale);
    }
    mRowsMode = true;
    return true;
}

// the quads path's faces: one a particle (as LL did)
bool LLVOPartGroup::updateFacesLegacy(LLDrawable* drawable, S32 num_parts)
{
    mRowsMode = false;
    mEval.clear();
    mEvalValid = false;
    LLFace* facep;
    if (num_parts > drawable->getNumFaces())
    {
        drawable->setNumFacesFast(num_parts+num_parts/4, NULL, getTEImage(0));
    }

    F32 tot_area = 0;

    F32 max_area = LLViewerPartSim::getMaxPartCount() * MAX_PARTICLE_AREA_SCALE;
    F32 pixel_meter_ratio = LLViewerCamera::getInstance()->getPixelMeterRatio();
    pixel_meter_ratio *= pixel_meter_ratio;

    LLViewerPartSim::checkParticleCount(static_cast<U32>(mViewerPartGroupp->mParticles.size()));

    S32 count=0;
    mDepth = 0.f;
    S32 i = 0 ;
    LLVector3 camera_agent = getCameraPosition();

    F32 max_scale = 0.f;


    for (i = 0 ; i < (S32)mViewerPartGroupp->mParticles.size(); i++)
    {
        const LLViewerPart *part = mViewerPartGroupp->mParticles[i];


        //remember the largest particle
        max_scale = llmax(max_scale, part->mScale.mV[0], part->mScale.mV[1]);

        if (part->mFlags & LLPartData::LL_PART_RIBBON_MASK)
        { //include ribbon segment length in scale
            const LLVector3* pos_agent = NULL;
            if (part->mParent)
            {
                pos_agent = &(part->mParent->mPosAgent);
            }
            else if (part->mPartSourcep.notNull())
            {
                pos_agent = &(part->mPartSourcep->mPosAgent);
            }

            if (pos_agent)
            {
                F32 dist = (*pos_agent-part->mPosAgent).length();

                max_scale = llmax(max_scale, dist);
            }
        }

        LLVector3 part_pos_agent(part->mPosAgent);
        LLVector3 at(part_pos_agent - camera_agent);


        F32 camera_dist_squared = at.lengthSquared();
        F32 inv_camera_dist_squared;
        if (camera_dist_squared > 1.f)
            inv_camera_dist_squared = 1.f / camera_dist_squared;
        else
            inv_camera_dist_squared = 1.f;

        llassert(llfinite(inv_camera_dist_squared));
        llassert(!llisnan(inv_camera_dist_squared));

        F32 area = part->mScale.mV[0] * part->mScale.mV[1] * inv_camera_dist_squared;
        tot_area = llmax(tot_area, area);

        if (tot_area > max_area)
        {
            break;
        }

        count++;

        facep = drawable->getFace(i);
        if (!facep)
        {
            LL_WARNS() << "No face found for index " << i << "!" << LL_ENDL;
            continue;
        }

        facep->setTEOffset(i);
        const F32 NEAR_PART_DIST_SQ = 5.f*5.f;  // Only discard particles > 5 m from the camera
        const F32 MIN_PART_AREA = .005f*.005f;  // only less than 5 mm x 5 mm at 1 m from camera

        if (camera_dist_squared > NEAR_PART_DIST_SQ && area < MIN_PART_AREA)
        {
            facep->setSize(0, 0);
            continue;
        }

        facep->setSize(4, 6);

        facep->setViewerObject(this);

        if (part->mFlags & LLPartData::LL_PART_EMISSIVE_MASK)
        {
            facep->setState(LLFace::FULLBRIGHT);
        }
        else
        {
            facep->clearState(LLFace::FULLBRIGHT);
        }

        facep->row().centerLocal = part->mPosAgent;
        facep->setFaceColor(part->mColor);
        facep->setTexture(part->mImagep);

        //check if this particle texture is replaced by a parcel media texture.
        if(part->mImagep.notNull() && part->mImagep->hasParcelMedia())
        {
            part->mImagep->getParcelMedia()->addMediaToFace(facep) ;
        }

        mPixelArea = tot_area * pixel_meter_ratio;
        const F32 area_scale = 10.f; // scale area to increase priority a bit
        facep->setVirtualSize(mPixelArea*area_scale);
    }
    for (i = count; i < drawable->getNumFaces(); i++)
    {
        LLFace* facep = drawable->getFace(i);
        if (!facep)
        {
            LL_WARNS() << "No face found for index " << i << "!" << LL_ENDL;
            continue;
        }
        facep->setTEOffset(i);
        facep->setSize(0, 0);
    }

    //record max scale (used to stretch bounding box for visibility culling)

    mScale.set(max_scale, max_scale, max_scale);

    mDrawable->movePartition();
    LLPipeline::sCompiles++;
    return true;
}


bool LLVOPartGroup::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                          S32 face,
                                          bool pick_transparent,
                                          bool pick_rigged,
                                          bool pick_unselectable,
                                          S32* face_hit,
                                          LLVector4a* intersection,
                                          LLVector2* tex_coord,
                                          LLVector4a* normal,
                                          LLVector4a* bi_normal)
{
    LLVector4a dir;
    dir.setSub(end, start);

    F32 closest_t = 2.f;
    bool ret = false;

    if (mEval.empty() && mPure && mViewerPartGroupp)
    {
        // G1 step 2: nothing was evaluated for the frame: the records of the group are evaluated for the ray (the same law as the shader's, LLViewerPartRecord::evaluate)
        const F64 now = LLViewerPartSim::getSimTime();
        const S32 base = (S32)mViewerPartGroupp->mParticles.size();
        for (S32 ri = 0; ri < (S32)mViewerPartGroupp->mRecords.size(); ++ri)
        {
            const LLViewerPartRecord& rec = mViewerPartGroupp->mRecords[ri];
            const F32 age = (F32)(now - rec.mT0);
            if (age > rec.mMaxAge)
            {
                continue;
            }
            LLVector3 pos, vel;
            rec.evaluate(now, pos, vel);
            const F32 frac = age / rec.mMaxAge;
            const LLVector2 scale = (rec.mFlags & LLPartData::LL_PART_INTERP_SCALE_MASK) ? fs_part_scale_at(rec.mStartScale, rec.mEndScale, frac) : rec.mStartScale;
            LLViewerPart tmp;
            tmp.mPosAgent = pos;
            tmp.mScale = scale;
            tmp.mVelocity = vel;
            tmp.mFlags = ((rec.mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK) && !vel.isExactlyZero()) ? LLPartData::LL_PART_FOLLOW_VELOCITY_MASK : 0;
            LLVector4a v[4];
            LLStrider<LLVector4a> verticesp;
            verticesp = v;
            getGeometry(tmp, verticesp);
            F32 a, b, t;
            if ((LLTriangleRayIntersect(v[0], v[1], v[2], start, dir, a, b, t) || LLTriangleRayIntersect(v[1], v[3], v[2], start, dir, a, b, t)) && t >= 0.f && t <= 1.f && t < closest_t)
            {
                ret = true;
                closest_t = t;
                if (face_hit)
                {
                    *face_hit = base + ri;
                }
                if (intersection)
                {
                    LLVector4a intersect = dir;
                    intersect.mul(closest_t);
                    intersection->setAdd(intersect, start);
                }
            }
        }
    }
    for (const LLPartEval& r : mEval)
    {
        const S32 idx = r.mIndex;
        LLVector4a v[4];
        if (r.mRibbon)
        {
            LLVector4a pos, axis, ppos, paxis;
            pos.load3(r.mPos.mV);
            axis.load3(r.mAxisHalf.mV);
            ppos.load3(r.mPPos.mV);
            paxis.load3(r.mPAxisHalf.mV);
            v[0].setAdd(ppos, paxis);
            v[1].setSub(ppos, paxis);
            v[2].setAdd(pos, axis);
            v[3].setSub(pos, axis);
        }
        else
        {
            // the quad of a drawn particle, made as getGeometry makes it from a part
            LLViewerPart tmp;
            tmp.mPosAgent = r.mPos;
            tmp.mScale = LLVector2(2.f * r.mHalfX, 2.f * r.mHalfY);
            tmp.mVelocity = r.mVel;
            tmp.mFlags = r.mVel.isExactlyZero() ? 0 : LLPartData::LL_PART_FOLLOW_VELOCITY_MASK;
            LLStrider<LLVector4a> verticesp;
            verticesp = v;
            getGeometry(tmp, verticesp);
        }

        F32 a,b,t;
        if (LLTriangleRayIntersect(v[0], v[1], v[2], start, dir, a,b,t) ||
            LLTriangleRayIntersect(v[1], v[3], v[2], start, dir, a,b,t))
        {
            if (t >= 0.f &&
                t <= 1.f &&
                t < closest_t)
            {
                ret = true;
                closest_t = t;
                if (face_hit)
                {
                    *face_hit = idx;
                }

                if (intersection)
                {
                    LLVector4a intersect = dir;
                    intersect.mul(closest_t);
                    intersection->setAdd(intersect, start);
                }
            }
        }
    }

    return ret;
}

void LLVOPartGroup::getGeometry(const LLViewerPart& part,
                                LLStrider<LLVector4a>& verticesp)
{
    if (part.mFlags & LLPartData::LL_PART_RIBBON_MASK)
    {
        LLVector4a axis, pos, paxis, ppos;
        F32 scale, pscale;

        pos.load3(part.mPosAgent.mV);
        axis.load3(part.mAxis.mV);
        scale = part.mScale.mV[0];

        if (part.mParent)
        {
            ppos.load3(part.mParent->mPosAgent.mV);
            paxis.load3(part.mParent->mAxis.mV);
            pscale = part.mParent->mScale.mV[0];
        }
        else
        { //use source object as position

            if (part.mPartSourcep->mSourceObjectp.notNull())
            {
                LLVector3 v = LLVector3(0,0,1);
                v *= part.mPartSourcep->mSourceObjectp->getRenderRotation();
                paxis.load3(v.mV);
                ppos.load3(part.mPartSourcep->mPosAgent.mV);
                pscale = part.mStartScale.mV[0];
            }
            else
            { //no source object, no parent, nothing to draw
                ppos = pos;
                pscale = scale;
                paxis = axis;
            }
        }

        LLVector4a p0, p1, p2, p3;

        scale *= 0.5f;
        pscale *= 0.5f;

        axis.mul(scale);
        paxis.mul(pscale);

        p0.setAdd(pos, axis);
        p1.setSub(pos,axis);
        p2.setAdd(ppos, paxis);
        p3.setSub(ppos, paxis);

        (*verticesp++) = p2;
        (*verticesp++) = p3;
        (*verticesp++) = p0;
        (*verticesp++) = p1;
    }
    else
    {
        LLVector4a part_pos_agent;
        part_pos_agent.load3(part.mPosAgent.mV);
        LLVector4a camera_agent;
        camera_agent.load3(getCameraPosition().mV);
        LLVector4a at;
        at.setSub(part_pos_agent, camera_agent);
        LLVector4a up(0, 0, 1);
        LLVector4a right;

        right.setCross3(at, up);
        right.normalize3fast();

        up.setCross3(right, at);
        up.normalize3fast();

        if (part.mFlags & LLPartData::LL_PART_FOLLOW_VELOCITY_MASK && !part.mVelocity.isExactlyZero())
        {
            LLVector4a normvel;
            normvel.load3(part.mVelocity.mV);
            normvel.normalize3fast();
            LLVector2 up_fracs;
            up_fracs.mV[0] = normvel.dot3(right).getF32();
            up_fracs.mV[1] = normvel.dot3(up).getF32();
            up_fracs.normalize();
            LLVector4a new_up;
            LLVector4a new_right;

            //new_up = up_fracs.mV[0] * right + up_fracs.mV[1]*up;
            LLVector4a t = right;
            t.mul(up_fracs.mV[0]);
            new_up = up;
            new_up.mul(up_fracs.mV[1]);
            new_up.add(t);

            //new_right = up_fracs.mV[1] * right - up_fracs.mV[0]*up;
            t = right;
            t.mul(up_fracs.mV[1]);
            new_right = up;
            new_right.mul(up_fracs.mV[0]);
            t.sub(new_right);

            up = new_up;
            right = t;
            up.normalize3fast();
            right.normalize3fast();
        }

        right.mul(0.5f*part.mScale.mV[0]);
        up.mul(0.5f*part.mScale.mV[1]);


        //HACK -- the verticesp->mV[3] = 0.f here are to set the texture index to 0 (particles don't use texture batching, maybe they should)
        // this works because there is actually a 4th float stored after the vertex position which is used as a texture index
        // also, somebody please VECTORIZE THIS

        LLVector4a ppapu;
        LLVector4a ppamu;

        ppapu.setAdd(part_pos_agent, up);
        ppamu.setSub(part_pos_agent, up);

        verticesp->setSub(ppapu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setSub(ppamu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setAdd(ppapu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
        verticesp->setAdd(ppamu, right);
        (*verticesp++).getF32ptr()[3] = 0.f;
    }
}



void LLVOPartGroup::getGeometry(S32 idx,
                                LLStrider<LLVector4a>& verticesp,
                                LLStrider<LLVector3>& normalsp,
                                LLStrider<LLVector2>& texcoordsp,
                                LLStrider<LLColor4U>& colorsp,
                                LLStrider<LLColor4U>& emissivep,
                                LLStrider<U16>& indicesp)
{
    if (idx >= (S32) mViewerPartGroupp->mParticles.size())
    {
        return;
    }

    const LLViewerPart &part = *((LLViewerPart*) (mViewerPartGroupp->mParticles[idx]));

    getGeometry(part, verticesp);

    LLColor4U pcolor;
    LLColor4U color = part.mColor;

    LLColor4U pglow;

    if (part.mFlags & LLPartData::LL_PART_RIBBON_MASK)
    { //make sure color blends properly
        if (part.mParent)
        {
            pglow = part.mParent->mGlow;
            pcolor = part.mParent->mColor;
        }
        else
        {
            pglow = LLColor4U(0, 0, 0, (U8) ll_round(255.f*part.mStartGlow));
            pcolor = part.mStartColor;
        }
    }
    else
    {
        pglow = part.mGlow;
        pcolor = color;
    }

    *colorsp++ = pcolor;
    *colorsp++ = pcolor;
    *colorsp++ = color;
    *colorsp++ = color;

    //if (pglow.mV[3] || part.mGlow.mV[3])
    { //only write glow if it is not zero
        *emissivep++ = pglow;
        *emissivep++ = pglow;
        *emissivep++ = part.mGlow;
        *emissivep++ = part.mGlow;
    }


    if (!(part.mFlags & LLPartData::LL_PART_EMISSIVE_MASK))
    { //not fullbright, needs normal
        LLVector3 normal = -LLViewerCamera::getInstance()->getXAxis();
        *normalsp++   = normal;
        *normalsp++   = normal;
        *normalsp++   = normal;
        *normalsp++   = normal;
    }
}

U32 LLVOPartGroup::getPartitionType() const
{
    return LLViewerRegion::PARTITION_PARTICLE;
}

LLParticlePartition::LLParticlePartition(LLViewerRegion* regionp)
: LLSpatialPartition(static_cast<U32>(LLDrawPoolAlpha::VERTEX_DATA_MASK) | static_cast<U32>(LLVertexBuffer::MAP_TEXTURE_INDEX), true, regionp)
{
    mRenderPass = LLRenderPass::PASS_ALPHA;
    mDrawableType = LLPipeline::RENDER_TYPE_PARTICLES;
    mPartitionType = LLViewerRegion::PARTITION_PARTICLE;
    mSlopRatio = 0.f;
    mLODPeriod = 1;
}

LLHUDParticlePartition::LLHUDParticlePartition(LLViewerRegion* regionp) :
    LLParticlePartition(regionp)
{
    mDrawableType = LLPipeline::RENDER_TYPE_HUD_PARTICLES;
    mPartitionType = LLViewerRegion::PARTITION_HUD_PARTICLE;
}

void LLParticlePartition::rebuildGeom(LLSpatialGroup* group)
{
    LL_PROFILE_ZONE_SCOPED;
    LL_PROFILE_GPU_ZONE("particle vbo");
    if (group->isDead() || !group->hasState(LLSpatialGroup::GEOM_DIRTY))
    {
        return;
    }

    if (group->changeLOD())
    {
        group->row().lastUpdateDistance = group->row().distance;
        group->row().lastUpdateViewAngle = group->row().viewAngle;
    }

    const unsigned long long ps_t0 = __rdtsc();
    if (mPartitionType != LLViewerRegion::PARTITION_PARTICLE)
    {
        ++sPartStats.mHud;
    }
    if (mPartitionType == LLViewerRegion::PARTITION_PARTICLE && fs_gpu_particles_on() && rebuildRows(group))
    {
        group->row().lastUpdateTime = gFrameTimeSeconds;
        group->clearState(LLSpatialGroup::GEOM_DIRTY);
        sPartStats.mRowsCycles += __rdtsc() - ps_t0;
        ++sPartStats.mRows;
        if (++sPartStats.mRebuilds % 3000 == 0)
        {
            fs_particle_stats_log();
        }
        return;
    }
    group->clearDrawMap();

    //get geometry count
    U32 index_count = 0;
    U32 vertex_count = 0;

    addGeometryCount(group, vertex_count, index_count);
    sPartStats.mCount += __rdtsc() - ps_t0;
    sPartStats.mParticles += vertex_count / 4;
    if (mPartitionType != LLViewerRegion::PARTITION_PARTICLE)
    {
        sPartStats.mHudParts += vertex_count / 4;
    }


    if (vertex_count > 0 && index_count > 0)
    {
        group->mBuilt = 1.f;
        if (group->mVertexBuffer.isNull() ||
            group->mVertexBuffer->getNumVerts() < vertex_count || group->mVertexBuffer->getNumIndices() < index_count)
        {
            group->mVertexBuffer = new LLVertexBuffer(LLVOPartGroup::VERTEX_DATA_MASK);
            group->mVertexBuffer->allocateBuffer(vertex_count, index_count);

            // initialize index and texture coordinates only when buffer is reallocated
            U16* indicesp = (U16*)group->mVertexBuffer->mapIndexBuffer(0, index_count);

            U16 geom_idx = 0;
            for (U32 i = 0; i < index_count; i += 6)
            {
                *indicesp++ = geom_idx + 0;
                *indicesp++ = geom_idx + 1;
                *indicesp++ = geom_idx + 2;

                *indicesp++ = geom_idx + 1;
                *indicesp++ = geom_idx + 3;
                *indicesp++ = geom_idx + 2;

                geom_idx += 4;
            }

            LLStrider<LLVector2> texcoordsp;

            group->mVertexBuffer->getTexCoord0Strider(texcoordsp);

            for (U32 i = 0; i < vertex_count; i += 4)
            {
                *texcoordsp++ = LLVector2(0.f, 1.f);
                *texcoordsp++ = LLVector2(0.f, 0.f);
                *texcoordsp++ = LLVector2(1.f, 1.f);
                *texcoordsp++ = LLVector2(1.f, 0.f);
            }

        }


        getGeometry(group);
    }
    else
    {
        group->mVertexBuffer = NULL;
        group->mBufferMap.clear();
    }

    group->row().lastUpdateTime = gFrameTimeSeconds;
    group->clearState(LLSpatialGroup::GEOM_DIRTY);
    sPartStats.mTotal += __rdtsc() - ps_t0;
    if (++sPartStats.mRebuilds % 3000 == 0)
    {
        fs_particle_stats_log();
    }
}

// G1: the draws of a pure record group sorted on the GPU: one draw info a draw class, over the class's range of the sorted order (the classes' live counts give the ranges)
static void fs_make_class_draws(LLSpatialGroup* group, LLViewerPartGroup* parts, U32 render_pass)
{
    // the rows the sort pass wrote, class after class (the classes' live counts give the ranges), are drawn as rows of stage 1
    LLVertexBuffer* rows = parts->mRows.get();
    group->mVertexBuffer = rows;
    group->mPendingDraws[render_pass];       // (C5 J2: the draw map keeps its entry, empty: the alpha group lists find the group by it; the draws are the group's particle runs)
    group->mParticleRuns.clear();
    U32 first = 0;
    for (U32 c = 0; c < parts->mClasses.size() && c < 4; ++c)
    {
        const U32 cnt = parts->mClassCount[c];
        if (!cnt)
        {
            continue;
        }
        const LLViewerPartGroup::ClassKey& key = parts->mClasses[c];
        LLParticleRun run;
        run.mTexture = key.mTex.get();
        run.mFirst = first;
        run.mRows = cnt;
        run.mBlendSrc = (U8)key.mSrc;
        run.mBlendDst = (U8)key.mDst;
        run.mFullbright = key.mFullbright;
        run.mGlow = parts->mClassGlow[c] > 0;       // (a class without a glow in it draws no glow pass: the second draw over every particle was most of the GPU cost)
        group->mParticleRuns.push_back(std::move(run));
        first += cnt;
    }
}

// <FS> GPU particles stage 1 (doc/vulkan/PLAN-gpu-particles.md): the group's particles as rows (position, velocity for a follow-velocity particle, half size, colour, glow) in the vertex buffer, in
// back to front order; the alpha pull shaders make the camera facing quad of each (alphaPullV.glsl, emissivePullV.glsl: flags & GEO_PARTICLE_ROWS). Draws by texture / blend / fullbright as the quads
// path did. A group with a ribbon particle keeps the quads path (false: nothing changed).
bool LLParticlePartition::rebuildRows(LLSpatialGroup* group)
{
    struct Entry { F32 mDist; const LLPartEval* mRow; };
    static std::vector<Entry> entries;
    entries.clear();
    U32 ribbons = 0;
    U32 live_drawables = 0;
    LLVOPartGroup* only_obj = nullptr;
    unsigned long long rt = __rdtsc();
    auto lap = [&](U64& into) { const unsigned long long now = __rdtsc(); into += now - rt; rt = now; };

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 origin = camera->getOrigin();
    const LLVector3 at_axis = camera->getAtAxis();
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();
        if (!drawablep || drawablep->isDead())
        {
            continue;
        }
        LLVOPartGroup* obj = static_cast<LLVOPartGroup*>(drawablep->getVObj().get());
        LLViewerPartGroup* parts = obj->getViewerPartGroup();
        if (!parts)
        {
            return false;
        }
        if (!obj->mRowsMode)
        {
            return false;       // (a group with a drawable that has the quads path's faces: the quads path takes the whole group)
        }
        ++live_drawables;
        only_obj = obj;
        obj->mDepth = 0.f;
        U32 count = 0;
        for (const LLPartEval& r : obj->mEval)
        {
            if (r.mRibbon)
            {
                ++ribbons;
            }
            obj->mDepth += r.mDist;
            ++count;
            entries.push_back({ r.mDist, &r });
        }
        if (count)
        {
            obj->mDepth /= count;
        }
    }

    lap(sPartStats.mRGather);
    group->clearDrawMap();
    lap(sPartStats.mRClear);
    if (ll_particle_sort_on() && live_drawables == 1 && only_obj && only_obj->mPure && only_obj->mEval.empty() && only_obj->getViewerPartGroup() && !only_obj->getViewerPartGroup()->mRecords.empty())
    {
        // G1 step 2: nothing was evaluated on the CPU (updateRows took the fast path): the group's records are drawn from their store, the sort and the evaluation are the GPU's
        LLViewerPartGroup* parts = only_obj->getViewerPartGroup();
        group->mBuilt = 1.f;
        if (group->changeLOD())
        {
            group->row().lastUpdateDistance = group->row().distance;
            group->row().lastUpdateViewAngle = group->row().viewAngle;
        }
        LLVector3 lo, hi;
        if (parts->pathBox(lo, hi))
        {
            only_obj->mDepth = ((lo + hi) * 0.5f - origin) * at_axis;
        }
        fs_make_class_draws(group, parts, mRenderPass);
        lap(sPartStats.mRWrite);
        sPartStats.mRowsParticles += (U32)parts->mRecords.size();
        ++sPartStats.mGpuGroups;
        ++sPartStats.mFastGroups;
        return true;
    }
    const U32 n = (U32)entries.size();
    if (!n)
    {
        group->mVertexBuffer = NULL;
        group->mBufferMap.clear();
        return true;
    }
    sPartStats.mRRibbons += ribbons;
    const U32 rows = n + ribbons;           // (a ribbon particle is two rows: itself and the point it joins to)
    group->mBuilt = 1.f;
    if (group->changeLOD())
    {
        group->row().lastUpdateDistance = group->row().distance;
        group->row().lastUpdateViewAngle = group->row().viewAngle;
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.mDist > b.mDist; });
    lap(sPartStats.mRSort);

    if (group->mVertexBuffer.isNull() || group->mVertexBuffer->getTypeMask() != LLVOPartGroup::VERTEX_DATA_MASK || group->mVertexBuffer->getNumVerts() < rows || group->mVertexBuffer->getNumIndices() < 6 * rows)
    {
        const U32 capacity = rows + rows / 4 + 8;         // (a row is 48 bytes: room to grow without a new buffer every time a particle is born)
        group->mVertexBuffer = new LLVertexBuffer(LLVOPartGroup::VERTEX_DATA_MASK);
        group->mVertexBuffer->allocateBuffer(capacity, 6 * capacity);
        // (the draws read no indices; the buffer keeps one quad's so it is a whole buffer to everything that looks at it)
        U16* indicesp = (U16*)group->mVertexBuffer->mapIndexBuffer(0, 6);
        indicesp[0] = 0; indicesp[1] = 1; indicesp[2] = 2; indicesp[3] = 1; indicesp[4] = 3; indicesp[5] = 2;
    }
    LLVertexBuffer* buffer = group->mVertexBuffer;
    lap(sPartStats.mRBuffer);

    LLStrider<LLVector4a> positionp;
    LLStrider<LLVector3> velocityp;
    LLStrider<LLVector2> sizep;
    LLStrider<LLColor4U> colorp;
    LLStrider<LLColor4U> glowp;
    buffer->getVertexStrider(positionp, 0, rows);
    buffer->getNormalStrider(velocityp, 0, rows);
    buffer->getTexCoord0Strider(sizep, 0, rows);
    buffer->getColorStrider(colorp, 0, rows);
    buffer->getEmissiveStrider(glowp, 0, rows);
    auto write_row = [&](const LLVector3& pos, F32 kind, const LLVector3& v, F32 hx, F32 hy, const LLColor4U& color, const LLColor4U& glow)
    {
        LLVector4a position;
        position.load3(pos.mV);
        position.getF32ptr()[3] = kind;     // 0 a quad, 1 the first row of a ribbon piece, 2 its second row (the point it joins to)
        *positionp++ = position;
        *velocityp++ = v;
        *sizep++ = LLVector2(hx, hy);
        *colorp++ = color;
        *glowp++ = glow;
    };

    group->mPendingDraws[mRenderPass];       // (C5 J2: kept, empty: the alpha group lists find the group by it)
    std::vector<LLParticleRun>& runs = group->mParticleRuns;
    runs.clear();
    LLParticleRun* info = nullptr;
    U32 row = 0;
    for (U32 k = 0; k < n; ++k)
    {
        const LLPartEval& r = *entries[k].mRow;

        U32 added = 1;
        if (r.mRibbon)
        {
            write_row(r.mPos, 1.f, r.mAxisHalf, 0.f, 0.f, r.mColor, r.mGlow);
            write_row(r.mPPos, 2.f, r.mPAxisHalf, 0.f, 0.f, r.mPColor, r.mPGlow);
            added = 2;
        }
        else
        {
            write_row(r.mPos, 0.f, r.mVel, r.mHalfX, r.mHalfY, r.mColor, r.mGlow);
        }

        LLViewerTexture* texture = r.mTex;
        const bool fullbright = r.mFullbright;
        LLRender::eBlendFactor bf_src = (LLRender::eBlendFactor)r.mBlendSrc;
        LLRender::eBlendFactor bf_dst = (LLRender::eBlendFactor)r.mBlendDst;
        const bool glow = r.mGlow.mV[3] != 0;
        if (info && info->mTexture.get() == texture && info->mFullbright == fullbright && info->mBlendDst == (U8)bf_dst && info->mBlendSrc == (U8)bf_src)
        {
            info->mRows += added;
            info->mGlow = info->mGlow || glow;
        }
        else
        {
            runs.emplace_back();
            info = &runs.back();
            info->mTexture = texture;
            info->mFirst = row;
            info->mRows = added;
            info->mBlendSrc = (U8)bf_src;
            info->mBlendDst = (U8)bf_dst;
            info->mFullbright = fullbright;
            info->mGlow = glow;
        }
        row += added;
    }
    lap(sPartStats.mRWrite);
    sPartStats.mRowsParticles += n;
    {
        // GPU sort design (doc/vulkan/PLAN-gpu-particles.md): how many draw states (blend pair + fullbright) and textures a group has, and how many runs the back to front order makes of them
        U32 classes = 0, textures = 0;
        U32 class_keys[8] = {};
        const LLViewerTexture* tex_keys[16] = {};
        for (U32 k = 0; k < n; ++k)
        {
            const LLPartEval& r = *entries[k].mRow;
            const U32 key = ((U32)r.mBlendSrc << 8) | ((U32)r.mBlendDst << 1) | (r.mFullbright ? 1u : 0u);
            bool known = false;
            for (U32 c = 0; c < classes && c < 8; ++c) { if (class_keys[c] == key) { known = true; break; } }
            if (!known) { if (classes < 8) { class_keys[classes] = key; } ++classes; }
            bool tknown = false;
            for (U32 t = 0; t < textures && t < 16; ++t) { if (tex_keys[t] == r.mTex) { tknown = true; break; } }
            if (!tknown) { if (textures < 16) { tex_keys[textures] = r.mTex; } ++textures; }
        }
        ++sPartStats.mCGroups;
        sPartStats.mCClasses += classes;
        sPartStats.mCMulti += classes > 1;
        sPartStats.mCTextures += textures;
        sPartStats.mCRuns += runs.size();
        sPartStats.mCParts += n;
        // runs of one draw state (texture ignored): a group sorted back to front, how often does the state change
        U32 state_runs = 0, last_key = ~0u;
        for (U32 k = 0; k < n; ++k)
        {
            const LLPartEval& r = *entries[k].mRow;
            const U32 key = ((U32)r.mBlendSrc << 8) | ((U32)r.mBlendDst << 1) | (r.mFullbright ? 1u : 0u);
            if (key != last_key) { ++state_runs; last_key = key; }
        }
        sPartStats.mCClassRuns += state_runs;
    }
    return true;
}

void LLParticlePartition::addGeometryCount(LLSpatialGroup* group, U32& vertex_count, U32& index_count)
{
    mFaceList.clear();

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();

        if (!drawablep || drawablep->isDead())
        {
            continue;
        }

        LLAlphaObject* obj = (LLAlphaObject*) drawablep->getVObj().get();
        LLVOPartGroup* part_group = static_cast<LLVOPartGroup*>(obj);
        if (part_group->mRowsMode)
        {
            part_group->updateFacesLegacy(drawablep, part_group->getViewerPartGroup()->getCount());     // (a group of the quads path with a drawable that was made as rows: it gets its faces now)
        }
        obj->mDepth = 0.f;

        U32 count = 0;
        for (S32 j = 0; j < drawablep->getNumFaces(); ++j)
        {
            drawablep->updateFaceSize(j);

            LLFace* facep = drawablep->getFace(j);
            if ( !facep || !facep->hasGeometry())
            {
                continue;
            }

            vertex_count += facep->getGeomCount();
            index_count += facep->getIndicesCount();

            count++;
            facep->row().distance = (facep->row().centerLocal - camera->getOrigin()) * camera->getAtAxis();
            obj->mDepth += facep->row().distance;

            mFaceList.push_back(facep);
            llassert(facep->getIndicesCount() < 65536);
        }

        obj->mDepth /= count;
    }
}


void LLParticlePartition::getGeometry(LLSpatialGroup* group)
{
    LL_PROFILE_ZONE_SCOPED;

    const unsigned long long ps_t0 = __rdtsc();
    std::sort(mFaceList.begin(), mFaceList.end(), LLFace::CompareDistanceGreater());
    sPartStats.mSort += __rdtsc() - ps_t0;

    group->clearDrawMap();

    LLVertexBuffer* buffer = group->mVertexBuffer;

    LLStrider<LLVector4a> verticesp;
    LLStrider<LLVector3> normalsp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLColor4U> emissivep;

    buffer->getVertexStrider(verticesp);
    buffer->getNormalStrider(normalsp);
    buffer->getColorStrider(colorsp);
    buffer->getEmissiveStrider(emissivep);

    S32 geom_idx = 0;
    S32 indices_idx = 0;

    LLSpatialGroup::drawmap_elem_t& draw_vec = group->mPendingDraws[mRenderPass];

    for (std::vector<LLFace*>::iterator i = mFaceList.begin(); i != mFaceList.end(); ++i)
    {
        LLFace* facep = *i;
        LLAlphaObject* object = (LLAlphaObject*) facep->getViewerObject();

        facep->setGeomIndex(geom_idx);
        facep->setIndicesIndex(indices_idx);

        LLStrider<LLVector4a> cur_vert = verticesp + geom_idx;
        LLStrider<LLVector3> cur_norm = normalsp + geom_idx;
        LLStrider<LLColor4U> cur_col = colorsp + geom_idx;
        LLStrider<LLColor4U> cur_glow = emissivep + geom_idx;

        // not actually used
        LLStrider<LLVector2> cur_tc;
        LLStrider<U16> cur_idx;


        geom_idx += 4;
        indices_idx += 6;

        LLColor4U* start_glow = cur_glow.get();

        const unsigned long long ps_c0 = __rdtsc();
        object->getGeometry(facep->getTEOffset(), cur_vert, cur_norm, cur_tc, cur_col, cur_glow, cur_idx);
        sPartStats.mCorners += __rdtsc() - ps_c0;

        bool has_glow = false;

        if (cur_glow.get() != start_glow)
        {
            has_glow = true;
        }

        llassert(facep->getGeomCount() == 4);
        llassert(facep->getIndicesCount() == 6);

        S32 idx = static_cast<S32>(draw_vec.size()) - 1;

        bool fullbright = facep->isState(LLFace::FULLBRIGHT);

        bool batched = false;

        LLRender::eBlendFactor bf_src = LLRender::BF_SOURCE_ALPHA;
        LLRender::eBlendFactor bf_dst = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;

        object->getBlendFunc(facep->getTEOffset(), bf_src, bf_dst);


        if (idx >= 0)
        {
            LLDrawBatch* info = draw_vec[idx];

            if (info->mTexture == facep->getTexture() &&
                info->mHasGlow == has_glow &&
                info->mFullbright == fullbright &&
                info->mBlendFuncDst == bf_dst &&
                info->mBlendFuncSrc == bf_src)
            {
                if (draw_vec[idx]->mEnd == facep->getGeomIndex()-1)
                {
                    batched = true;
                    info->mCount += facep->getIndicesCount();
                    info->mEnd += facep->getGeomCount();
                }
                else if (draw_vec[idx]->mStart == facep->getGeomIndex()+facep->getGeomCount()+1)
                {
                    batched = true;
                    info->mCount += facep->getIndicesCount();
                    info->mStart -= facep->getGeomCount();
                    info->mOffset = facep->getIndicesStart();
                }
            }
        }

        if (!batched)
        {
            U32 start = facep->getGeomIndex();
            U32 end = start + facep->getGeomCount()-1;
            U32 offset = facep->getIndicesStart();
            U32 count = facep->getIndicesCount();
            LLDrawBatch* info = new LLDrawBatch(start,end,count,offset,facep->getTexture(),
                buffer, fullbright);

            info->mBlendFuncDst = bf_dst;
            info->mBlendFuncSrc = bf_src;
            info->mHasGlow = has_glow;
            draw_vec.push_back(info);
        }
    }

    mFaceList.clear();
}

F32 LLParticlePartition::calcPixelArea(LLSpatialGroup* group, LLCamera& camera)
{
    return 1024.f;
}

U32 LLVOHUDPartGroup::getPartitionType() const
{
    return LLViewerRegion::PARTITION_HUD_PARTICLE;
}

LLDrawable* LLVOHUDPartGroup::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);
    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);
    return mDrawable;
}

LLVector3 LLVOHUDPartGroup::getCameraPosition() const
{
    return LLVector3(-1,0,0);
}

