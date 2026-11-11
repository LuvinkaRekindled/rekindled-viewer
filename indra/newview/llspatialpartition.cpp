/**
 * @file llspatialpartition.cpp
 * @brief LLSpatialGroup class implementation and supporting functions
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
#include "llgpuzonetimer.h"
#include "lloccensus.h"
#include "llasyncpool.h"

#include "llspatialpartition.h"
#include "llviewerpartsource.h"

#include "llappviewer.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llimageworker.h"
#include "llviewerwindow.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"
#include "llvolume.h"
#include "llvolumeoctree.h"
#include "llviewercamera.h"
#include "llface.h"
#include "llfloatertools.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llcamera.h"
#include "pipeline.h"
#include "llattcensus.h"
#include "llmeshrepository.h"
#include "llrender.h"
#include "lldrawpool.h"
#include <algorithm>
#include <chrono>
#include <list>
#include <mutex>
#include <unordered_set>
#include "lloctree.h"
#include "llphysicsshapebuilderutil.h"
#include "llvoavatar.h"
#include "llvolumemgr.h"
#include "llviewershadermgr.h"
#include "llcontrolavatar.h"

#include "llvotree.h"
// <FS:Beq> improved normals debug
#include "llformat.h"
#include "llselectmgr.h"
// </FS:Beq>

extern bool gShiftFrame;

static U32 sZombieGroups = 0;
U32 LLSpatialGroup::sNodeCount = 0;

bool LLSpatialGroup::sNoDelete = false;

static F32 sLastMaxTexPriority = 1.f;
static F32 sCurMaxTexPriority = 1.f;

// enable expensive sanity checks around redundant drawable and group insertion to LLCullResult
#define LL_DEBUG_CULL_RESULT 0

//static counter for frame to switch LOD on

void sg_assert(bool expr)
{
#if LL_OCTREE_PARANOIA_CHECK
    if (!expr)
    {
        LL_ERRS() << "Octree invalid!" << LL_ENDL;
    }
#endif
}

//returns:
//  0 if sphere and AABB are not intersecting
//  1 if they are
//  2 if AABB is entirely inside sphere

S32 LLSphereAABB(const LLVector3& center, const LLVector3& size, const LLVector3& pos, const F32 &rad)
{
    S32 ret = 2;

    LLVector3 min = center - size;
    LLVector3 max = center + size;
    for (U32 i = 0; i < 3; i++)
    {
        if (min.mV[i] > pos.mV[i] + rad ||
            max.mV[i] < pos.mV[i] - rad)
        {   //totally outside
            return 0;
        }

        if (min.mV[i] < pos.mV[i] - rad ||
            max.mV[i] > pos.mV[i] + rad)
        {   //intersecting
            ret = 1;
        }
    }

    return ret;
}

namespace LLDirtyGroups
{
static std::mutex sMutex;
static std::vector<LLSpatialGroup*> sList;

void add(LLSpatialGroup* g)
{
    if (g->mInDirtyReg.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    g->mDirtyIdx = (U32)sList.size();
    sList.push_back(g);
}

void forget(LLSpatialGroup* g)
{
    if (!g->mInDirtyReg.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    const U32 idx = g->mDirtyIdx;
    if (idx < sList.size() && sList[idx] == g)
    {
        LLSpatialGroup* last = sList.back();
        sList[idx] = last;
        last->mDirtyIdx = idx;
        sList.pop_back();
    }
}

std::vector<LLSpatialGroup*> snapshot()
{
    std::lock_guard<std::mutex> lock(sMutex);
    return sList;
}

void prune()
{
    std::lock_guard<std::mutex> lock(sMutex);
    for (size_t k = 0; k < sList.size();)
    {
        LLSpatialGroup* g = sList[k];
        if (g->isDead() || !g->hasState(LLSpatialGroup::MESH_DIRTY | LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::NEW_DRAWINFO))
        {
            g->mInDirtyReg.store(false, std::memory_order_release);
            LLSpatialGroup* last = sList.back();
            sList[k] = last;
            last->mDirtyIdx = (U32)k;
            sList.pop_back();
            continue;
        }
        ++k;
    }
}
}   // namespace LLDirtyGroups

LLSpatialGroup::~LLSpatialGroup()
{
    LLDirtyGroups::forget(this);
    /*if (sNoDelete)
    {
        LL_ERRS() << "Illegal deletion of LLSpatialGroup!" << LL_ENDL;
    }*/

    if (gDebugRender)
    {
        gPipeline.checkReferences(this);
    }

    if (hasState(DEAD))
    {
        sZombieGroups--;
    }

    sNodeCount--;

    if (hasState(FS_WAKING))
    {
        gPipeline.wakeForget(this);
    }

    clearDrawMap();
    ll_scene_fragment_free(mSceneFragment);
}

U32 LLSpatialGroup::sDrawMapsCleared = 0;

void LLSpatialGroup::clearDrawMap()
{
    ++mDrawMapSerial;
    // <FS> the draw infos go: builders on the async pool may be reading them, and the main view's render map may point to them
    if (!mDrawMap.empty())
    {
        ++sDrawMapsCleared;
        LLBuilderGuard::beforeMutation(getVisible(LLViewerCamera::CAMERA_WORLD) >= LLViewerOctreeEntryData::getCurrentFrame());
    }
    mDrawMap.clear();
    mParticleRuns.clear();
    mGeoHandle = 0;
    mGeoServedMask = 0;
    mGeoServedTris = 0;
    mGeoAllServed = false;
    ll_scene_fragment_free(mSceneFragment);     // <FS> S2: the retained entries of the draw infos go with them (the worker's table follows)
}

bool LLSpatialGroup::isHUDGroup() const
{
    if (hasState(DEAD))
        return false;

    LLSpatialPartition* part = (LLSpatialPartition*)mSpatialPartition;
    return part && part->isHUDPartition();
}

void LLSpatialGroup::validate()
{
    ll_assert_aligned(this,64);
#if LL_OCTREE_PARANOIA_CHECK

    sg_assert(!isState(DIRTY));
    sg_assert(!isDead());

    LLVector4a myMin;
    myMin.setSub(row().bounds[0], row().bounds[1]);
    LLVector4a myMax;
    myMax.setAdd(row().bounds[0], row().bounds[1]);

    validateDrawMap();

    for (element_iter i = getDataBegin(); i != getDataEnd(); ++i)
    {
        LLDrawable* drawable = *i;
        sg_assert(drawable->getSpatialGroup() == this);
        if (drawable->getSpatialBridge())
        {
            sg_assert(drawable->getSpatialBridge() == getSpatialPartition()->asBridge());
        }

        /*if (drawable->isSpatialBridge())
        {
            LLSpatialPartition* part = drawable->asPartition();
            if (!part)
            {
                LL_ERRS() << "Drawable reports it is a spatial bridge but not a partition." << LL_ENDL;
            }
            LLSpatialGroup* group = (LLSpatialGroup*) part->mOctree->getListener(0);
            group->validate();
        }*/
    }

    for (U32 i = 0; i < mOctreeNode->getChildCount(); ++i)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) mOctreeNode->getChild(i)->getListener(0);

        group->validate();

        //ensure all children are enclosed in this node
        LLVector4a center = group->row().bounds[0];
        LLVector4a size = group->row().bounds[1];

        LLVector4a min;
        min.setSub(center, size);
        LLVector4a max;
        max.setAdd(center, size);

        for (U32 j = 0; j < 3; j++)
        {
            sg_assert(min[j] >= myMin[j]-0.02f);
            sg_assert(max[j] <= myMax[j]+0.02f);
        }
    }

#endif
}

void LLSpatialGroup::validateDrawMap()
{
#if LL_OCTREE_PARANOIA_CHECK
    for (draw_map_t::iterator i = mDrawMap.begin(); i != mDrawMap.end(); ++i)
    {
        LLSpatialGroup::drawmap_elem_t& draw_vec = i->second;
        for (drawmap_elem_t::iterator j = draw_vec.begin(); j != draw_vec.end(); ++j)
        {
            LLDrawInfo& params = **j;

            params.validate();
        }
    }
#endif
}

// <FS> LL_VK_LEAVECENSUS=1: for the drawables of avatar bridge groups, how often updateInGroup keeps them and why it does not (the centre is outside the node, or the node is too small for them),
// by what they are (rigged, flexible, light, other). Reported with the scene census (fs_leave_census_report, every 600 frames).
namespace
{
    struct LeaveCensus
    {
        U64 mCalls = 0, mStayed = 0;
        U64 mOutside[4] = {}, mTooBig[4] = {};        // rigged, flexible, light, other
        F64 mRadiusSum[4] = {}, mNodeSum[4] = {};      // for the ones that were too big: bin radius and node size
    } sLeave;
}

static void fs_leave_census(LLSpatialGroup* group, LLDrawable* drawablep, bool inside, bool fits, bool stayed)
{
    static const bool on = getenv("LL_VK_LEAVECENSUS") != nullptr;
    if (!on || !group->getSpatialPartition() || group->getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_AVATAR)
    {
        return;
    }
    ++sLeave.mCalls;
    if (stayed)
    {
        ++sLeave.mStayed;
        return;
    }
    LLViewerObject* vobj = drawablep->getVObj().get();
    LLVOVolume* vol = vobj && vobj->getPCode() == LL_PCODE_VOLUME ? (LLVOVolume*)vobj : nullptr;
    const int kind = drawablep->isState(LLDrawable::RIGGED) ? 0 : (vol && vol->isFlexibleFast()) ? 1 : (vol && vol->getIsLight()) ? 2 : 3;
    if (!inside)
    {
        ++sLeave.mOutside[kind];
    }
    else if (!fits)
    {
        ++sLeave.mTooBig[kind];
        sLeave.mRadiusSum[kind] += drawablep->getBinRadius();
        sLeave.mNodeSum[kind] += group->getOctreeNode() ? group->getOctreeNode()->getSize()[0] : 0.f;
    }
}

void fs_leave_census_report(double frames)
{
    static const bool on = getenv("LL_VK_LEAVECENSUS") != nullptr;
    if (!on)
    {
        return;
    }
    static const char* const names[4] = { "rigged", "flexible", "light", "other" };
    const LeaveCensus& c = sLeave;
    std::string text;
    for (int k = 0; k < 4; ++k)
    {
        text += llformat(" | %s: centre outside the node %.2f, too big for it %.2f (bin radius %.2f m against node %.2f m)", names[k], c.mOutside[k] / frames, c.mTooBig[k] / frames,
            c.mTooBig[k] ? c.mRadiusSum[k] / (F64)c.mTooBig[k] : 0.0, c.mTooBig[k] ? c.mNodeSum[k] / (F64)c.mTooBig[k] : 0.0);
    }
    LL_INFOS("LeaveCensus") << "avatar bridge drawables a frame: updateInGroup calls " << c.mCalls / frames << ", stayed " << c.mStayed / frames << text << LL_ENDL;
    sLeave = LeaveCensus();
}

bool LLSpatialGroup::updateInGroup(LLDrawable *drawablep, bool immediate)
{
    LL_CPU_ZONE("octree: group updateInGroup");
    LL_PROFILE_ZONE_SCOPED;
    drawablep->updateSpatialExtents();

    OctreeNode* parent = mOctreeNode->getOctParent();

    // <FS> FSFlexibleStayInGroup (default on; LL_VK_FLEXI_STAY=0 also turns it off, for A/B laps). A flexible prim's extents change every update, so its bin radius crosses the "fits in this node"
    // threshold back and forth and the prim was taken out of its group and put in another again and again (lap rqf: ~8 attachment moves a frame left their group, each one dirtying two groups for a
    // full rebuild: ~10 avatar groups a frame; with the change laps abA/abB: rebuildPriorityGroups 0.78 -> 0.28 ms, loop -0.9 ms). The group's extents are worked out from what is in it (unbound),
    // so culling does not depend on the node a prim sits in: a flexible prim stays while its centre is inside the node.
    static LLCachedControl<bool> flexi_stay_setting(gSavedSettings, "FSFlexibleStayInGroup", true);
    static const bool flexi_stay_env = !getenv("LL_VK_FLEXI_STAY") || getenv("LL_VK_FLEXI_STAY")[0] != '0';
    bool stay_flexible = false;
    if (flexi_stay_setting && flexi_stay_env)
    {
        LLViewerObject* vobj = drawablep->getVObj().get();
        stay_flexible = vobj && vobj->getPCode() == LL_PCODE_VOLUME && ((LLVOVolume*)vobj)->isFlexibleFast();
    }

    const bool inside = mOctreeNode->isInside(drawablep->getPositionGroup());
    const bool fits = stay_flexible || mOctreeNode->contains(drawablep->getEntry()) ||
        (drawablep->getBinRadius() > mOctreeNode->getSize()[0] && parent && parent->getElementCount() >= gOctreeMaxCapacity);
    if (inside && fits)
    {
        unbound();
        setState(OBJECT_DIRTY);
        //setState(GEOM_DIRTY);
        fs_leave_census(this, drawablep, true, true, true);
        return true;
    }
    fs_leave_census(this, drawablep, inside, fits, false);

    return false;
}

void LLSpatialGroup::expandExtents(const LLVector4a* addingExtents, const LLXformMatrix& currentTransform)
{
    // Get coordinates of the adding extents
    const LLVector4a& min = addingExtents[0];
    const LLVector4a& max = addingExtents[1];

    // Get coordinates of all corners of the bounding box
    LLVector3 corners[] =
    {
        LLVector3(min[0], min[1], min[2]),
        LLVector3(min[0], min[1], max[2]),
        LLVector3(min[0], max[1], min[2]),
        LLVector3(min[0], max[1], max[2]),
        LLVector3(max[0], min[1], min[2]),
        LLVector3(max[0], min[1], max[2]),
        LLVector3(max[0], max[1], min[2]),
        LLVector3(max[0], max[1], max[2])
    };

    // New extents (to be expanded)
    LLVector3 extents[] =
    {
        LLVector3(row().extents[0].getF32ptr()),
        LLVector3(row().extents[1].getF32ptr())
    };

    LLQuaternion backwardRotation = ~currentTransform.getWorldRotation();
    for (LLVector3& corner : corners)
    {
        // Make coordinates relative to the current position
        corner -= currentTransform.getWorldPosition();
        // Rotate coordinates backward to the current rotation
        corner.rotVec(backwardRotation);
        // Expand root extents on the current corner
        for (int j = 0; j < 3; ++j)
        {
            if (corner[j] < extents[0][j])
                extents[0][j] = corner[j];
            if (corner[j] > extents[1][j])
                extents[1][j] = corner[j];
        }
    }

    // Set new expanded extents
    row().extents[0].load3(extents[0].mV);
    row().extents[1].load3(extents[1].mV);

    // Calculate new center and size
    row().bounds[0].setAdd(row().extents[0], row().extents[1]);
    row().bounds[0].mul(0.5f);
    row().bounds[1].setSub(row().extents[0], row().extents[1]);
    row().bounds[1].mul(0.5f);
    bumpPartitionEpoch();               // <FS> C4.3
}

bool LLSpatialGroup::addObject(LLDrawable *drawablep)
{
    if(!drawablep)
    {
        return false;
    }
    {
        drawablep->setGroup(this);
        setState(static_cast<U32>(OBJECT_DIRTY) | static_cast<U32>(GEOM_DIRTY));
        hizInvalidate();
        hizEntryMoved(drawablep->getSpatialExtents());
        gPipeline.markRebuild(this);
        if (drawablep->isSpatialBridge())
        {
            mBridgeList.push_back((LLSpatialBridge*) drawablep);
        }
        if (drawablep->getRadius() > 1.f)
        {
            setState(IMAGE_DIRTY);
        }
    }

    return true;
}

void LLSpatialGroup::rebuildGeom()
{
    // <FS> whatever asked for this rebuild, the draw infos are back after it. FS_WAKING stays: a group is in the wake queue exactly when it has that bit (wakeGroups clears it when it takes the group out, and
    // the destructor takes a waiting group out), so the entry cannot outlive the group; wakeGroups skips a group that is no longer FS_RELEASED.
    clearState(FS_RELEASED);
    if (!isDead())
    {
        LLRebuildBatch* batch = LLRebuildBatch::current();
        if (batch)
        {
            batch->mJustDeferred = nullptr;
        }
        { LL_CPU_ZONE("rebuild: partition rebuildGeom (the volume or simple geometry manager)"); getSpatialPartition()->rebuildGeom(this); }
        if (batch && batch->mJustDeferred == this)
        {   // its fill waits for the batch: so does the fragment, which reads the draw infos
            batch->mJustDeferred = nullptr;
            batch->fragmentLater(this);
        }
        else
        {
            LL_CPU_ZONE("rebuild: scene geometry fragment build");
            ll_scene_fragment_build(this);
        }

        if (hasState(LLSpatialGroup::MESH_DIRTY))
        {
            gPipeline.markMeshDirty(this);
        }
    }
}

void LLSpatialGroup::rebuildMesh()
{
    if (!isDead())
    {
        if (hasState(MESH_DIRTY))
        {
            // <FS> the vertex buffers of the group are repacked (their GL buffers may change)
            LLBuilderGuard::beforeMutation(getVisible(LLViewerCamera::CAMERA_WORLD) >= LLViewerOctreeEntryData::getCurrentFrame());
        }
        getSpatialPartition()->rebuildMesh(this);
    }
}

void LLSpatialPartition::rebuildGeom(LLSpatialGroup* group)
{
    LL_CPU_ZONE("octree: partition rebuildGeom");
    if (group->isDead() || !group->hasState(LLSpatialGroup::GEOM_DIRTY))
    {
        return;
    }

    if (group->changeLOD())
    {
        group->row().lastUpdateDistance = group->row().distance;
        group->row().lastUpdateViewAngle = group->row().viewAngle;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    group->clearDrawMap();

    //get geometry count
    U32 index_count = 0;
    U32 vertex_count = 0;

    addGeometryCount(group, vertex_count, index_count);

    if (vertex_count > 0 && index_count > 0)
    { //create vertex buffer containing volume geometry for this node
        {
            group->mBuilt = 1.f;
            if (group->mVertexBuffer.isNull() ||
                group->mVertexBuffer->getNumVerts() != vertex_count ||
                group->mVertexBuffer->getNumVerts() != index_count)
            {
                group->mVertexBuffer = new LLVertexBuffer(mVertexDataMask);
                if (!group->mVertexBuffer->allocateBuffer(vertex_count, index_count))
                {
                    LL_WARNS() << "Failed to allocate Vertex Buffer on rebuild to "
                        << vertex_count << " vertices and "
                        << index_count << " indices" << LL_ENDL;
                    group->mVertexBuffer = NULL;
                    group->mBufferMap.clear();
                }
            }
        }

        if (group->mVertexBuffer)
        {
            getGeometry(group);
        }
    }
    else
    {
        group->mVertexBuffer = NULL;
        group->mBufferMap.clear();
    }

    group->row().lastUpdateTime = gFrameTimeSeconds;
    group->clearState(LLSpatialGroup::GEOM_DIRTY);
}


void LLSpatialPartition::rebuildMesh(LLSpatialGroup* group)
{

}

LLSpatialGroup* LLSpatialGroup::getParent()
{
    return (LLSpatialGroup*)LLViewerOctreeGroup::getParent();
    }

bool LLSpatialGroup::removeObject(LLDrawable *drawablep, bool from_octree)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    if(!drawablep)
    {
        return false;
    }

    unbound();
    if (mOctreeNode && !from_octree)
    {
        drawablep->setGroup(NULL);
    }
    else
    {
        drawablep->setGroup(NULL);
        setState(GEOM_DIRTY);
        gPipeline.markRebuild(this);

        if (drawablep->isSpatialBridge())
        {
            for (bridge_list_t::iterator i = mBridgeList.begin(); i != mBridgeList.end(); ++i)
            {
                if (*i == drawablep)
                {
                    mBridgeList.erase(i);
                    break;
                }
            }
        }

        if (getElementCount() == 0)
        { //delete draw map on last element removal since a rebuild might never happen
            clearDrawMap();
        }
    }
    return true;
}

void LLSpatialGroup::shift(const LLVector4a &offset)
{
    LLVector4a t = mOctreeNode->getCenter();
    t.add(offset);
    mOctreeNode->setCenter(t);
    mOctreeNode->updateMinMax();
    row().bounds[0].add(offset);
    row().extents[0].add(offset);
    row().extents[1].add(offset);
    row().objectBounds[0].add(offset);
    row().objectExtents[0].add(offset);
    row().objectExtents[1].add(offset);
    bumpPartitionEpoch();               // <FS> C4.3
    if (LLVisSetHooks::sOn.load(std::memory_order_relaxed)) { LLVisSetHooks::invalidateAll(); }       // <FS> C4.5 S1b: every row of the partition moved

    if (!getSpatialPartition()->mRenderByGroup &&
        getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_TREE &&
        getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_TERRAIN &&
        getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_BRIDGE &&
        getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_AVATAR &&
        getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_CONTROL_AV)
    {
        setState(GEOM_DIRTY);
        gPipeline.markRebuild(this);
    }
}

class LLSpatialSetState : public OctreeTraveler
{
public:
    U32 mState;
    LLSpatialSetState(U32 state) : mState(state) { }
    virtual void visit(const OctreeNode* branch) { ((LLSpatialGroup*) branch->getListener(0))->setState(mState); }
};

class LLSpatialSetStateDiff : public LLSpatialSetState
{
public:
    LLSpatialSetStateDiff(U32 state) : LLSpatialSetState(state) { }

    virtual void traverse(const OctreeNode* n)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) n->getListener(0);

        if (!group->hasState(mState))
        {
            OctreeTraveler::traverse(n);
        }
    }
};

void LLSpatialGroup::setState(U32 state, S32 mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    llassert(state <= LLSpatialGroup::STATE_MASK);

    if (mode > STATE_MODE_SINGLE)
    {
        if (mode == STATE_MODE_DIFF)
        {
            LLSpatialSetStateDiff setter(state);
            setter.traverse(mOctreeNode);
        }
        else
        {
            LLSpatialSetState setter(state);
            setter.traverse(mOctreeNode);
        }
    }
    else
    {
        row().state |= state;
        if (state & (MESH_DIRTY | GEOM_DIRTY | NEW_DRAWINFO))
        {
            LLDirtyGroups::add(this);
        }
    }
}

class LLSpatialClearState : public OctreeTraveler
{
public:
    U32 mState;
    LLSpatialClearState(U32 state) : mState(state) { }
    virtual void visit(const OctreeNode* branch) { ((LLSpatialGroup*) branch->getListener(0))->clearState(mState); }
};

class LLSpatialClearStateDiff : public LLSpatialClearState
{
public:
    LLSpatialClearStateDiff(U32 state) : LLSpatialClearState(state) { }

    virtual void traverse(const OctreeNode* n)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) n->getListener(0);

        if (group->hasState(mState))
        {
            OctreeTraveler::traverse(n);
        }
    }
};

void LLSpatialGroup::clearState(U32 state, S32 mode)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    llassert(state <= LLSpatialGroup::STATE_MASK);

    if (mode > STATE_MODE_SINGLE)
    {
        if (mode == STATE_MODE_DIFF)
        {
            LLSpatialClearStateDiff clearer(state);
            clearer.traverse(mOctreeNode);
        }
        else
        {
            LLSpatialClearState clearer(state);
            clearer.traverse(mOctreeNode);
        }
    }
    else
    {
        row().state &= ~state;
    }
}

//======================================
//      Octree Listener Implementation
//======================================

LLSpatialGroup::LLSpatialGroup(OctreeNode* node, LLSpatialPartition* part) : LLOcclusionCullingGroup(node, part),
    mGeometryBytes(0),
    mSurfaceArea(0.f),
    mBuilt(0.f),
    mVertexBuffer(NULL)
{
    ll_assert_aligned(this,16);

    sNodeCount++;

    {   // <FS> C1b.2: the view caches are in the row (a new row is zeroed)
        LLGroupRows::Row& r = row();
        r.objectBoxSize = 1.f;
        r.distance = 0.f;
        r.depth = 0.f;
        r.lastUpdateDistance = -1.f;
        r.lastUpdateTime = gFrameTimeSeconds;
        r.viewAngle.splat(0.f);
        r.lastUpdateViewAngle.splat(-1.f);
    }

    sg_assert(mOctreeNode->getListenerCount() == 0);
    setState(SG_INITIAL_STATE_MASK);
    gPipeline.markRebuild(this);

    // let the reflection map manager know about this spatial group
    mReflectionProbe = gPipeline.mReflectionMapManager.registerSpatialGroup(this);

    row().radius = 1;
    row().pixelArea = 1024.f;
}

void LLSpatialGroup::updateDistance(LLCamera &camera)
{
    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        LL_WARNS() << "Attempted to update distance for camera other than world camera!" << LL_ENDL;
        llassert(false);
        return;
    }

    if (gShiftFrame)
    {
        return;
    }

#if !LL_RELEASE_FOR_DOWNLOAD
    if (hasState(LLSpatialGroup::OBJECT_DIRTY))
    {
        LL_ERRS() << "Spatial group dirty on distance update." << LL_ENDL;
    }
#endif
    if (!isEmpty())
    {
        row().radius = getSpatialPartition()->mRenderByGroup ? row().objectBounds[1].getLength3().getF32() :
                        (F32) mOctreeNode->getSize().getLength3().getF32();
        row().distance = getSpatialPartition()->calcDistance(this, camera);
        row().pixelArea = getSpatialPartition()->calcPixelArea(this, camera);
    }
}

F32 LLSpatialPartition::calcDistance(LLSpatialGroup* group, LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    LLVector4a eye;
    LLVector4a origin;
    origin.load3(camera.getOrigin().mV);

    eye.setSub(group->row().objectBounds[0], origin);

    F32 dist = 0.f;

    if (group->mDrawMap.find(LLRenderPass::PASS_ALPHA) != group->mDrawMap.end())
    {
        LLVector4a v = eye;

        dist = eye.getLength3().getF32();
        eye.normalize3fast();

        if (!group->hasState(LLSpatialGroup::ALPHA_DIRTY))
        {
            if (!group->getSpatialPartition()->isBridge())
            {
                LLVector4a view_angle = eye;

                LLVector4a diff;
                diff.setSub(view_angle, group->row().lastUpdateViewAngle);

                if (diff.getLength3().getF32() > 0.64f)
                {
                    group->row().viewAngle = view_angle;
                    group->row().lastUpdateViewAngle = view_angle;
                    //for occasional alpha sorting within the group
                    //NOTE: If there is a trivial way to detect that alpha sorting here would not change the render order,
                    //not setting this node to dirty would be a very good thing
                    group->setState(LLSpatialGroup::ALPHA_DIRTY);
                    gPipeline.markRebuild(group);
                }
            }
        }

        //calculate depth of node for alpha sorting

        LLVector3 at = camera.getAtAxis();

        LLVector4a ata;
        ata.load3(at.mV);

        LLVector4a t = ata;
        //front of bounding box
        t.mul(0.25f);
        t.mul(group->row().objectBounds[1]);
        v.sub(t);

        group->row().depth = v.dot3(ata).getF32();
    }
    else
    {
        dist = eye.getLength3().getF32();
    }

    // <FS:Beq> This debug has measurable imapcty even when discarded
//#if !LL_RELEASE
    // LL_DEBUGS("RiggedBox") << "calcDistance, group " << group << " camera " << origin << " obj bounds "
    //                        << group->mObjectBounds[0] << ", " << group->mObjectBounds[1]
    //                        << " dist " << dist << " radius " << group->mRadius << LL_ENDL;
//#endif
    // </FS:Beq>

    if (dist < 16.f)
    {
        dist /= 16.f;
        dist *= dist;
        dist *= 16.f;
    }

    return dist;
}

F32 LLSpatialPartition::calcPixelArea(LLSpatialGroup* group, LLCamera& camera)
{
    return LLPipeline::calcPixelArea(group->row().objectBounds[0], group->row().objectBounds[1], camera);
}

F32 LLSpatialGroup::getUpdateUrgency() const
{
    if (!isVisible())
    {
        return 0.f;
    }
    else
    {
        F32 time = gFrameTimeSeconds-row().lastUpdateTime+4.f;
        return time + (row().objectBounds[1].dot3(row().objectBounds[1]).getF32()+1.f)/row().distance;
    }
}

bool LLSpatialGroup::changeLOD()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    if (hasState(static_cast<U32>(ALPHA_DIRTY) | static_cast<U32>(OBJECT_DIRTY)))
    {
        //a rebuild is going to happen, update distance and LoD
        return true;
    }

    if (getSpatialPartition()->mSlopRatio > 0.f)
    {
        F32 ratio = (row().distance - row().lastUpdateDistance)/(llmax(row().lastUpdateDistance, row().radius));

        // MAINT-8264 - this check is not robust if it needs to work
        // for bounding boxes much larger than the actual enclosed
        // objects, and using distance to box center is also
        // problematic. Consider the case that you have a large box
        // where the enclosed object is in one corner. As you zoom in
        // on the corner, the object gets much closer to the camera,
        // but the distance to the box center changes very little, and
        // an LOD change will not trigger, so object LOD gets "stuck"
        // at a too-low value. In the case of the above JIRA, the box
        // was large only due to another error, so this logic did not
        // need to be changed.

        if (fabsf(ratio) >= getSpatialPartition()->mSlopRatio)
        {
            return true;
        }
    }

    if (needsUpdate())
    {
        return true;
    }

    return false;
}

void LLSpatialGroup::handleInsertion(const TreeNode* node, LLViewerOctreeEntry* entry)
{
    addObject((LLDrawable*)entry->getDrawable());
    unbound();
    setState(OBJECT_DIRTY);
    if (LLVisSetHooks::sOn.load(std::memory_order_relaxed)) { LLVisSetHooks::noteBounds(this); }     // <FS> C4.5 S1b: its elements changed
}

void LLSpatialGroup::handleRemoval(const TreeNode* node, LLViewerOctreeEntry* entry)
{
    removeObject((LLDrawable*)entry->getDrawable(), true);
    if (LLVisSetHooks::sOn.load(std::memory_order_relaxed)) { LLVisSetHooks::noteBounds(this); }     // <FS> C4.5 S1b: its elements changed
    LLViewerOctreeGroup::handleRemoval(node, entry);
}

void LLSpatialGroup::handleDestruction(const TreeNode* node)
{
    if(isDead())
    {
        return;
    }
    setState(DEAD);
    if (LLVisSetHooks::sOn.load(std::memory_order_relaxed))
    {
        LLVisSetHooks::forget(this);        // <FS> C4.5 S1b: (this override does not call the base class one) the retained set must not keep a group that is gone
    }

    for (element_iter i = getDataBegin(); i != getDataEnd(); ++i)
    {
        LLViewerOctreeEntry* entry = *i;

        if (entry->getGroup() == this)
        {
            if(entry->hasDrawable())
            {
                ((LLDrawable*)entry->getDrawable())->setGroup(NULL);
            }
        }
    }

    clearDrawMap();
    mVertexBuffer = NULL;
    mBufferMap.clear();
    sZombieGroups++;
    mOctreeNode = NULL;
}

void LLSpatialGroup::handleChildAddition(const OctreeNode* parent, OctreeNode* child)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    if (child->getListenerCount() == 0)
    {
        new LLSpatialGroup(child, getSpatialPartition());
    }
    else
    {
        OCT_ERRS << "LLSpatialGroup redundancy detected." << LL_ENDL;
    }

    unbound();

    assert_states_valid(this);
}

//virtual
void LLSpatialGroup::rebound()
{
    if (!isDirty())
        return;

    super::rebound();

    if (mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_CONTROL_AV)
    {
        llassert(mSpatialPartition->mPartitionType == LLViewerRegion::PARTITION_CONTROL_AV);

        LLSpatialBridge* bridge = getSpatialPartition()->asBridge();
        if (bridge &&
            bridge->mDrawable &&
            bridge->mDrawable->getVObj() &&
            bridge->mDrawable->getVObj()->isRoot())
        {
            LLControlAvatar* controlAvatar = bridge->mDrawable->getVObj()->getControlAvatar();
            if (controlAvatar &&
                controlAvatar->mDrawable &&
                controlAvatar->mControlAVBridge &&
                controlAvatar->mControlAVBridge->mOctree)
            {
                LLSpatialGroup* root = (LLSpatialGroup*)controlAvatar->mControlAVBridge->mOctree->getListener(0);
                if (this == root)
                {
                    const LLVector4a* addingExtents = controlAvatar->mDrawable->getSpatialExtents();
                    const LLXformMatrix* currentTransform = bridge->mDrawable->getXform();
                    expandExtents(addingExtents, *currentTransform);
                }
            }
        }
    }
}

void LLSpatialGroup::destroyGLState(bool keep_occlusion)
{
    // <FS:Ansariel> Reset VB during TP
    bool is_tree_group = getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_TREE;

    setState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::IMAGE_DIRTY);

    if (!keep_occlusion)
    { //going to need a rebuild
        gPipeline.markRebuild(this);
    }

    row().lastUpdateTime = gFrameTimeSeconds;
    mVertexBuffer = NULL;
    mBufferMap.clear();

    clearDrawMap();

    if (!keep_occlusion)
    {
        hizInvalidate();        // <FS> S7 H3: the verdict awaited (if any) is out of date (it was: the query names were given back)
    }


    for (LLSpatialGroup::element_iter i = getDataBegin(); i != getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        if(!drawable)
        {
            continue;
        }
        for (S32 j = 0; j < drawable->getNumFaces(); j++)
        {
            LLFace* facep = drawable->getFace(j);
            if (facep)
            {
                facep->clearVertexBuffer();
            }
        }

        // <FS:Ansariel> Reset VB during TP
        if (is_tree_group && drawable->getVObj())
        {
            auto votree = dynamic_cast<LLVOTree*>(drawable->getVObj().get());
            if (votree)
                votree->destroyVB();
        }
        // </FS:Ansariel>
    }
}

//==============================================

LLSpatialPartition::LLSpatialPartition(U32 data_mask, bool render_by_group, LLViewerRegion* regionp)
: mRenderByGroup(render_by_group), mBridge(NULL)
{
    mRegionp = regionp;
    mPartitionType = LLViewerRegion::PARTITION_NONE;
    mVertexDataMask = data_mask;
    mDepthMask = false;
    mSlopRatio = 0.25f;
    mInfiniteFarClip = false;

    new LLSpatialGroup(mOctree, this);
}


LLSpatialPartition::~LLSpatialPartition()
{
    cleanup();
}

LLSpatialGroup *LLSpatialPartition::put(LLDrawable *drawablep, bool was_visible)
{
    LL_CPU_ZONE("octree: partition put");
    LL_PROFILE_ZONE_SCOPED;
    drawablep->updateSpatialExtents();

    //keep drawable from being garbage collected
    LLPointer<LLDrawable> ptr = drawablep;

    if(!drawablep->getGroup())
    {
    assert_octree_valid(mOctree);
        mOctree->insert(drawablep->getEntry());
    assert_octree_valid(mOctree);
    }

    LLSpatialGroup* group = drawablep->getSpatialGroup();
    //llassert(group != NULL);

    if (group && was_visible && group->isHizAwaiting())
    {
        group->hizInvalidate();
    }

    return group;
}

bool LLSpatialPartition::remove(LLDrawable *drawablep, LLSpatialGroup *curp)
{
    LL_CPU_ZONE("octree: partition remove");
    LL_PROFILE_ZONE_SCOPED;
    if (!curp->removeObject(drawablep))
    {
        OCT_ERRS << "Failed to remove drawable from octree!" << LL_ENDL;
    }
    else
    {
        drawablep->setGroup(NULL);
    }

    assert_octree_valid(mOctree);

    return true;
}

void LLSpatialPartition::move(LLDrawable *drawablep, LLSpatialGroup *curp, bool immediate)
{
    LL_CPU_ZONE("octree: partition move");
    LL_PROFILE_ZONE_SCOPED;
    LLAttCensus::Scope att_census(LLAttCensus::sPartitionMove, drawablep);
    // sanity check submitted by open source user bushing Spatula
    // who was seeing crashing here. (See VWR-424 reported by Bunny Mayne)
    if (!drawablep)
    {
        OCT_ERRS << "LLSpatialPartition::move was passed a bad drawable." << LL_ENDL;
        return;
    }

    bool was_visible = curp ? curp->isVisible() : false;

    if (curp && curp->getSpatialPartition() != this)
    {
        //keep drawable from being garbage collected
        LLPointer<LLDrawable> ptr = drawablep;
        if (curp->getSpatialPartition()->remove(drawablep, curp))
        {
            put(drawablep, was_visible);
            return;
        }
        else
        {
            OCT_ERRS << "Drawable lost between spatial partitions on outbound transition." << LL_ENDL;
        }
    }

    if (curp && curp->updateInGroup(drawablep, immediate))
    {
        // Already updated, don't need to do anything
        assert_octree_valid(mOctree);
        return;
    }

    if (LLAttCensus::sOn)
    {
        ++LLAttCensus::sPartitionLeftGroup[LLAttCensus::kindOf(drawablep)];
    }
    //keep drawable from being garbage collected
    LLPointer<LLDrawable> ptr = drawablep;
    if (curp && !remove(drawablep, curp))
    {
        OCT_ERRS << "Move couldn't find existing spatial group!" << LL_ENDL;
    }

    put(drawablep, was_visible);
}

class LLSpatialShift : public OctreeTraveler
{
public:
    const LLVector4a& mOffset;

    LLSpatialShift(const LLVector4a& offset) : mOffset(offset) { }
    virtual void visit(const OctreeNode* branch)
    {
        ((LLSpatialGroup*) branch->getListener(0))->shift(mOffset);
    }
};

void LLSpatialPartition::shift(const LLVector4a &offset)
{ //shift octree node bounding boxes by offset
    LLSpatialShift shifter(offset);
    shifter.traverse(mOctree);
}

class LLOctreeCull : public LLViewerOctreeCull
{
public:
    LLOctreeCull(LLCamera* camera) : LLViewerOctreeCull(camera) {}

    virtual bool earlyFail(LLViewerOctreeGroup* base_group)
    {
        if (LLPipeline::reflectionRender())
        {
            return false;
        }

        LLSpatialGroup* group = (LLSpatialGroup*)base_group;
        if (LLPipeline::useOcclusion() >= 2)         // <FS> checkOcclusion() does nothing below 2: no call
        {
            group->checkOcclusion();
        }
        LLOccCensus::Cam* census = LLOccCensus::cam(LLViewerCamera::sCurCameraID);      // <FS> H0 (LL_VK_OCC_CENSUS=1)
        if (census)
        {
            ++census->mCullEarlyFail;
        }

        // <FS> the cheap tests first: the group's own occlusion state before the walk to the octree node and its parent (cache misses)
        if (LLPipeline::useOcclusion() &&            //ignore occlusion if disabled
            group->isOcclusionState(LLSpatialGroup::OCCLUDED) &&
            group->getOctreeNode() &&
            group->getOctreeNode()->getParent())    //never occlusion cull the root node
        {
            if (census)
            {
                ++census->mCullDropped;
            }
            if (LLCullCensus::sOn && LLCullCensus::sActive.load(std::memory_order_relaxed) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD)
            {
                LLCullCensus::sOccDropped.fetch_add(1, std::memory_order_relaxed);      // <FS> C4.5 census
            }
            gPipeline.markOccluder(group);
            return true;
        }

        return false;
    }

    virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
    {
        LL_PROFILE_ZONE_SCOPED;
        S32 res = AABBInFrustumNoFarClipGroupBounds(group);
        if (res != 0)
        {
            res = llmin(res, AABBSphereIntersectGroupExtents(group));
        }
        return res;
    }

    virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
    {
        LL_PROFILE_ZONE_SCOPED;
        S32 res = AABBInFrustumNoFarClipObjectBounds(group);
        if (res != 0)
        {
            res = llmin(res, AABBSphereIntersectObjectExtents(group));
        }
        return res;
    }

    virtual void processGroup(LLViewerOctreeGroup* base_group)
    {
        LL_PROFILE_ZONE_SCOPED;
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;
        gPipeline.markNotCulled(group, *mCamera);
    }
};

class LLOctreeCullNoFarClip : public LLOctreeCull
{
public:
    LLOctreeCullNoFarClip(LLCamera* camera)
        : LLOctreeCull(camera) { }

    virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
    {
        return AABBInFrustumNoFarClipGroupBounds(group);
    }

    virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
    {
        S32 res = AABBInFrustumNoFarClipObjectBounds(group);
        return res;
    }
};

class LLOctreeCullShadow : public LLOctreeCull
{
public:
    LLOctreeCullShadow(LLCamera* camera)
        : LLOctreeCull(camera) { }

    virtual S32 frustumCheck(const LLViewerOctreeGroup* group)
    {
        return AABBInFrustumGroupBounds(group);
    }

    virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group)
    {
        // <FS> Adam, 2026-11-10: the shadow of his own avatar blinked for a frame now and then while he moved. A caster is in a cascade's pass only if its object bounds meet that cascade's frustum
        // (LLDrawable::isVisible reads the stamp this cull sets, LLPipeline::stateSort drops an avatar without it), and an avatar's bounds change with every step of its pose: the first split of
        // the cascades is a few metres from the camera, where a following avatar stands, so its bounds crossed the edge of the cascade 1 frustum from one frame to the next. The bounds of the avatar
        // partitions are padded by 2 m for this test (a shadow caster outside the frustum by less than that costs nothing).
        LLSpatialGroup* spatial = const_cast<LLSpatialGroup*>(static_cast<const LLSpatialGroup*>(group));
        const LLSpatialPartition* partition = spatial->getSpatialPartition();
        if (partition && (partition->mPartitionType == LLViewerRegion::PARTITION_AVATAR || partition->mPartitionType == LLViewerRegion::PARTITION_CONTROL_AV))
        {
            LLVector4a size = group->row().objectBounds[1];
            LLVector4a pad;
            pad.splat(2.f);
            size.add(pad);
            return mCamera->AABBInFrustum(group->row().objectBounds[0], size);
        }
        return AABBInFrustumObjectBounds(group);
    }
};

class LLOctreeCullVisExtents: public LLOctreeCullShadow
{
public:
    LLOctreeCullVisExtents(LLCamera* camera, LLVector4a& min, LLVector4a& max)
        : LLOctreeCullShadow(camera), mMin(min), mMax(max), mEmpty(true) { }

    virtual bool earlyFail(LLViewerOctreeGroup* base_group)
    {
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;

        if (LLPipeline::useOcclusion() &&            //ignore occlusion if disabled
            group->isOcclusionState(LLSpatialGroup::OCCLUDED) &&
            group->getOctreeNode()->getParent())    //never occlusion cull the root node
        {
            return true;
        }

        return false;
    }

    virtual void traverse(const OctreeNode* n)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) n->getListener(0);

        if (earlyFail(group))
        {
            return;
        }

        if ((mRes && group->hasState(LLSpatialGroup::SKIP_FRUSTUM_CHECK)) ||
            mRes == 2)
        {   //don't need to do frustum check
            OctreeTraveler::traverse(n);
        }
        else
        {
            mRes = frustumCheck(group);

            if (mRes)
            { //at least partially in, run on down
                OctreeTraveler::traverse(n);
            }

            mRes = 0;
        }
    }

    virtual void processGroup(LLViewerOctreeGroup* base_group)
    {
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;

        llassert(!group->hasState(LLSpatialGroup::DIRTY) && !group->isEmpty());

        if (mRes < 2)
        {
            if (AABBInFrustumObjectBounds(group) > 0)
            {
                mEmpty = false;
                const LLVector4a* exts = group->getObjectExtents();
                update_min_max(mMin, mMax, exts[0]);
                update_min_max(mMin, mMax, exts[1]);
            }
        }
        else
        {
            mEmpty = false;
            const LLVector4a* exts = group->getExtents();
            update_min_max(mMin, mMax, exts[0]);
            update_min_max(mMin, mMax, exts[1]);
        }
    }

    bool mEmpty;
    LLVector4a& mMin;
    LLVector4a& mMax;
};

class LLOctreeCullDetectVisible: public LLOctreeCullShadow
{
public:
    LLOctreeCullDetectVisible(LLCamera* camera)
        : LLOctreeCullShadow(camera), mResult(false) { }

    virtual bool earlyFail(LLViewerOctreeGroup* base_group)
    {
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;

        if (mResult || //already found a node, don't check any more
            (group->getOctreeNode()->getParent() && //never occlusion cull the root node
             LLPipeline::useOcclusion() &&           //ignore occlusion if disabled
             group->isOcclusionState(LLSpatialGroup::OCCLUDED)))
        {
            return true;
        }

        return false;
    }

    virtual void processGroup(LLViewerOctreeGroup* base_group)
    {
        if (base_group->isVisible())
        {
            mResult = true;
        }
    }

    bool mResult;
};

class LLOctreeSelect : public LLOctreeCull
{
public:
    LLOctreeSelect(LLCamera* camera, std::vector<LLDrawable*>* results)
        : LLOctreeCull(camera), mResults(results) { }

    virtual bool earlyFail(LLViewerOctreeGroup* group) { return false; }
    virtual void preprocess(LLViewerOctreeGroup* group) { }

    virtual void processGroup(LLViewerOctreeGroup* base_group)
    {
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;
        OctreeNode* branch = group->getOctreeNode();

        for (OctreeNode::const_element_iter i = branch->getDataBegin(); i != branch->getDataEnd(); ++i)
        {
            LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
            if(!drawable)
        {
                continue;
            }
            if (!drawable->isDead())
            {
                if (drawable->isSpatialBridge())
                {
                    drawable->setVisible(*mCamera, mResults, true);
                }
                else
                {
                    mResults->push_back(drawable);
                }
            }
        }
    }

    std::vector<LLDrawable*>* mResults;
};

void drawBox(const LLVector3& c, const LLVector3& r)
{
    LLVertexBuffer::unbind();

    gRender.begin(LLRender::TRIANGLE_STRIP);
    //left front
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,1))).mV);
    //right front
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,1,1))).mV);
    //right back
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,-1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,-1,1))).mV);
    //left back
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,-1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,-1,1))).mV);
    //left front
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,1))).mV);
    gRender.end();

    //bottom
    gRender.begin(LLRender::TRIANGLE_STRIP);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,-1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,-1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,-1,-1))).mV);
    gRender.end();

    //top
    gRender.begin(LLRender::TRIANGLE_STRIP);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,1,1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,1,1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(1,-1,1))).mV);
    gRender.vertex3fv((c+r.scaledVec(LLVector3(-1,-1,1))).mV);
    gRender.end();
}

void drawBox(const LLVector4a& c, const LLVector4a& r)
{
    drawBox(reinterpret_cast<const LLVector3&>(c), reinterpret_cast<const LLVector3&>(r));
}

void drawBoxOutline(const LLVector3& pos, const LLVector3& size)
{
    if (!pos.isFinite() || !size.isFinite())
        return;

    LLVector3 v1 = size.scaledVec(LLVector3( 1, 1,1));
    LLVector3 v2 = size.scaledVec(LLVector3(-1, 1,1));
    LLVector3 v3 = size.scaledVec(LLVector3(-1,-1,1));
    LLVector3 v4 = size.scaledVec(LLVector3( 1,-1,1));

    gRender.begin(LLRender::LINES);

    //top
    gRender.vertex3fv((pos+v1).mV);
    gRender.vertex3fv((pos+v2).mV);
    gRender.vertex3fv((pos+v2).mV);
    gRender.vertex3fv((pos+v3).mV);
    gRender.vertex3fv((pos+v3).mV);
    gRender.vertex3fv((pos+v4).mV);
    gRender.vertex3fv((pos+v4).mV);
    gRender.vertex3fv((pos+v1).mV);

    //bottom
    gRender.vertex3fv((pos-v1).mV);
    gRender.vertex3fv((pos-v2).mV);
    gRender.vertex3fv((pos-v2).mV);
    gRender.vertex3fv((pos-v3).mV);
    gRender.vertex3fv((pos-v3).mV);
    gRender.vertex3fv((pos-v4).mV);
    gRender.vertex3fv((pos-v4).mV);
    gRender.vertex3fv((pos-v1).mV);

    //right
    gRender.vertex3fv((pos+v1).mV);
    gRender.vertex3fv((pos-v3).mV);

    gRender.vertex3fv((pos+v4).mV);
    gRender.vertex3fv((pos-v2).mV);

    //left
    gRender.vertex3fv((pos+v2).mV);
    gRender.vertex3fv((pos-v4).mV);

    gRender.vertex3fv((pos+v3).mV);
    gRender.vertex3fv((pos-v1).mV);

    gRender.end();
}

void drawBoxOutline(const LLVector4a& pos, const LLVector4a& size)
{
    drawBoxOutline(reinterpret_cast<const LLVector3&>(pos), reinterpret_cast<const LLVector3&>(size));
}


void LLSpatialPartition::restoreResources()
{
}

bool LLSpatialPartition::getVisibleExtents(LLCamera& camera, LLVector3& visMin, LLVector3& visMax)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;
    LLVector4a visMina, visMaxa;
    visMina.load3(visMin.mV);
    visMaxa.load3(visMax.mV);

    {
        LLSpatialGroup* group = (LLSpatialGroup*) mOctree->getListener(0);
        group->rebound();
    }

    LLOctreeCullVisExtents vis(&camera, visMina, visMaxa);
    vis.traverse(mOctree);

    visMin.set(visMina.getF32ptr());
    visMax.set(visMaxa.getF32ptr());
    return vis.mEmpty;
}

// <FS> the cascades' cull in pieces (see the declarations in llspatialpartition.h)
void LLSpatialPartition::cullShadowWhole(LLCamera& camera)
{
    LL_CPU_ZONE("updateCull: partition cull");
    LLOctreeCullShadow culler(&camera);
    culler.traverse(mOctree);
}

void LLSpatialPartition::cullShadowSplit(LLCamera& camera, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task)
{
    LLOctreeCullShadow culler(&camera);
    culler.traverseTop(mOctree, levels, descend_more, on_task);
}

void LLSpatialPartition::cullShadowTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task)
{
    LL_CPU_ZONE("updateCull: partition cull");
    LLOctreeCullShadow culler(&camera);
    culler.traverseTask(task);
}

// <FS> the cascades' point cloud in pieces (see the declarations in llspatialpartition.h)
void LLSpatialPartition::visibleExtentsWhole(LLCamera& camera, LLVector4a& min, LLVector4a& max, bool& empty)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;
    LLOctreeCullVisExtents vis(&camera, min, max);
    vis.traverse(mOctree);
    empty = empty && vis.mEmpty;
}

void LLSpatialPartition::visibleExtentsSplit(LLCamera& camera, LLVector4a& min, LLVector4a& max, bool& empty, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task)
{
    LLOctreeCullVisExtents vis(&camera, min, max);
    vis.traverseTop(mOctree, levels, descend_more, on_task);
    empty = empty && vis.mEmpty;
}

void LLSpatialPartition::visibleExtentsTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task, LLVector4a& min, LLVector4a& max, bool& empty)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;
    LLOctreeCullVisExtents vis(&camera, min, max);
    vis.traverseTask(task);
    empty = empty && vis.mEmpty;
}

bool LLSpatialPartition::visibleObjectsInFrustum(LLCamera& camera)
{
    LLOctreeCullDetectVisible vis(&camera);
    vis.traverse(mOctree);
    return vis.mResult;
}

S32 LLSpatialPartition::cull(LLCamera &camera, std::vector<LLDrawable *>* results, bool for_select)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;
#if LL_OCTREE_PARANOIA_CHECK
    ((LLSpatialGroup*)mOctree->getListener(0))->checkStates();
#endif
    {
        LLSpatialGroup* group = (LLSpatialGroup*) mOctree->getListener(0);
        group->rebound();
    }

#if LL_OCTREE_PARANOIA_CHECK
    ((LLSpatialGroup*)mOctree->getListener(0))->validate();
#endif

        LLOctreeSelect selecter(&camera, results);
        selecter.traverse(mOctree);

    return 0;
}

extern bool gCubeSnapshot;

S32 LLSpatialPartition::cull(LLCamera &camera, bool do_occlusion)
{
    LL_CPU_ZONE("octree: partition cull (render)");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;
#if LL_OCTREE_PARANOIA_CHECK
    ((LLSpatialGroup*)mOctree->getListener(0))->checkStates();
#endif
    LLSpatialGroup* group = (LLSpatialGroup*) mOctree->getListener(0);
    group->rebound();

#if LL_OCTREE_PARANOIA_CHECK
    ((LLSpatialGroup*)mOctree->getListener(0))->validate();
#endif

    if (LLPipeline::shadowRender())
    {
        LLOctreeCullShadow culler(&camera);
        culler.traverse(mOctree);
    }
    else if (mInfiniteFarClip || (!LLPipeline::sUseFarClip && !gCubeSnapshot))
    {
        LLOctreeCullNoFarClip culler(&camera);
        culler.traverse(mOctree);
    }
    else
    {
        LLOctreeCull culler(&camera);
        culler.traverse(mOctree);
    }

    return 0;
}

void LLSpatialPartition::cullSplit(LLCamera& camera, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const LLViewerOctreeCull::SplitTask&)>& on_task)
{
    LLSpatialGroup* group = (LLSpatialGroup*) mOctree->getListener(0);
    group->rebound();

    if (mInfiniteFarClip || (!LLPipeline::sUseFarClip && !gCubeSnapshot))
    {
        LLOctreeCullNoFarClip culler(&camera);
        culler.traverseTop(mOctree, levels, descend_more, on_task);
    }
    else
    {
        LLOctreeCull culler(&camera);
        culler.traverseTop(mOctree, levels, descend_more, on_task);
    }
}

void LLSpatialPartition::cullTask(LLCamera& camera, const LLViewerOctreeCull::SplitTask& task)
{
    if (mInfiniteFarClip || (!LLPipeline::sUseFarClip && !gCubeSnapshot))
    {
        LLOctreeCullNoFarClip culler(&camera);
        culler.traverseTask(task);
    }
    else
    {
        LLOctreeCull culler(&camera);
        culler.traverseTask(task);
    }
}

// <FS> C4.5 S1a: the main view's cull verdicts without the side effects (see LLSpatialPartition::CullCollect). Base is LLOctreeCull or LLOctreeCullNoFarClip: the same frustum tests as the real walk.
template <class Base, bool Shadow = false>
class LLOctreeCullCollect : public Base
{
public:
    LLOctreeCullCollect(LLCamera* camera, LLSpatialPartition::CullCollect& out) : Base(camera), mOut(out) {}

    virtual bool earlyFail(LLViewerOctreeGroup* base_group) override
    {
        // (LLOctreeCull::earlyFail without markOccluder and the census counters)
        if (Shadow)
        {
            return false;       // (the cascades' culls run with the occlusion test off)
        }
        if (LLPipeline::reflectionRender())
        {
            return false;
        }
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;
        if (LLPipeline::useOcclusion() >= 2)
        {
            group->checkOcclusion();
        }
        return LLPipeline::useOcclusion() &&
            group->isOcclusionState(LLSpatialGroup::OCCLUDED) &&
            group->getOctreeNode() &&
            group->getOctreeNode()->getParent();
    }

    virtual void traverse(const OctreeNode* n) override
    {
        LLViewerOctreeGroup* group = (LLViewerOctreeGroup*)n->getListener(0);
        LLSpatialPartition::CullCollect::NodeInfo info;
        // (the entry mRes the walk can tell apart: a node reads it only as 2 (wholly inside) or as non-zero with the frustum-skip flag; the carry-over of 0 or 1 from an earlier sibling that early-failed changes nothing)
        info.mEntryRes = (S8)(this->mRes == 2 ? 2 : ((this->mRes && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)) ? 1 : 0));
        const void* key = (const void*)group;
        if (earlyFail(group))
        {
            info.mOutcome = 1;
            info.mResOut = info.mEntryRes;
            mOut.mNodes[key] = info;
            mOut.mOrder.push_back({ (LLSpatialGroup*)group, 0 });
            return;
        }
        if (this->mRes == 2 || (this->mRes && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)))
        {
            info.mOutcome = 3;
            info.mResOut = info.mEntryRes;
            mOut.mNodes[key] = info;
            OctreeTraveler::traverse(n);
        }
        else
        {
            this->mRes = this->frustumCheck(group);
            info.mOutcome = this->mRes ? 3 : 2;
            info.mResOut = (S8)this->mRes;
            mOut.mNodes[key] = info;
            if (this->mRes)
            {
                OctreeTraveler::traverse(n);
            }
            this->mRes = 0;
        }
    }

    virtual void processGroup(LLViewerOctreeGroup* base_group) override
    {
        LLSpatialGroup* group = (LLSpatialGroup*)base_group;
        if (group->isEmpty())
        {
            return;
        }
        const bool by_group = group->getSpatialPartition()->mRenderByGroup;
        (by_group ? mOut.mVisible : mOut.mDrawable).push_back(group);
        mOut.mOrder.push_back({ group, (U8)(by_group ? 2 : 1) });
        mOut.mNodes[(const void*)(LLViewerOctreeGroup*)group].mMember = by_group ? 2 : 1;
    }

    void start(const OctreeNode* n, S32 res)
    {
        this->mRes = res;
        traverse(n);
    }

private:
    LLSpatialPartition::CullCollect& mOut;
};

void LLSpatialPartition::collectCull(LLCamera& camera, CullCollect& out)
{
    out.mDrawable.clear();
    out.mVisible.clear();
    out.mOrder.clear();
    out.mNodes.clear();
    if (mInfiniteFarClip || (!LLPipeline::sUseFarClip && !gCubeSnapshot))
    {
        LLOctreeCullCollect<LLOctreeCullNoFarClip> culler(&camera, out);
        culler.traverse(mOctree);
    }
    else
    {
        LLOctreeCullCollect<LLOctreeCull> culler(&camera, out);
        culler.traverse(mOctree);
    }
}

void LLSpatialPartition::collectSubtree(LLCamera& camera, const OctreeNode* node, S32 entry_res, CullCollect& out)
{
    if (mInfiniteFarClip || (!LLPipeline::sUseFarClip && !gCubeSnapshot))
    {
        LLOctreeCullCollect<LLOctreeCullNoFarClip> culler(&camera, out);
        culler.start(node, entry_res);
    }
    else
    {
        LLOctreeCullCollect<LLOctreeCull> culler(&camera, out);
        culler.start(node, entry_res);
    }
}

void LLSpatialPartition::collectCullShadow(LLCamera& camera, CullCollect& out)
{
    out.mDrawable.clear();
    out.mVisible.clear();
    out.mOrder.clear();
    out.mNodes.clear();
    LLOctreeCullCollect<LLOctreeCullShadow, true> culler(&camera, out);
    culler.traverse(mOctree);
}

void LLSpatialPartition::collectSubtreeShadow(LLCamera& camera, const OctreeNode* node, S32 entry_res, CullCollect& out)
{
    LLOctreeCullCollect<LLOctreeCullShadow, true> culler(&camera, out);
    culler.start(node, entry_res);
}

// <FS> C4.5 S1b: the retained set. Per partition, what the collecting walk found (the nodes it visited with their entry mRes and outcome, the member groups), kept between frames and changed by the events the hooks
// report: an occlusion state change of a group (the walk of its subtree is redone from the group), a change of what a frustum verdict reads of a group (bounds, extents, object bounds, the frustum-skip flag, its
// elements), a destroy (the group is dropped at once). Nothing here changes what the cull lists: LL_VK_VISSET=2 compares the retained set with a full collecting walk every frame and logs every difference.
namespace LLVisSet
{
struct PartState
{
    std::unordered_map<const void*, LLSpatialPartition::CullCollect::NodeInfo> mNodes;
    std::unordered_set<LLSpatialGroup*> mDrawable, mVisible;
    bool mValid = false;                                        // current for the camera of this frame (replayable)
    std::list<LLSpatialPartition::CullCollect::Item> mOrder;    // what the cull does to each, in the order the walk reaches them: a re-walked subtree takes the place of its old entries
    std::unordered_map<LLSpatialGroup*, std::list<LLSpatialPartition::CullCollect::Item>::iterator> mPos;
    bool mReorder = false;                                      // an entry could not be put in its place (its subtree had none): the list is made again from the nodes
    std::vector<LLSpatialGroup*> mFlat;                         // the list as an array for the cull jobs (made when the list changed)
    std::vector<U8> mFlatKind;
    std::vector<S32> mFlatLod;
    bool mFlatDirty = true;
    std::unordered_set<LLSpatialGroup*> mPending;               // the groups that joined this frame: they get the set's slot (and so its stamp) when the frame is claimed or the real walk is over, not before (their stored stamp is what the walk must see)
    U32 mSlot = 0;                                              // the slot of the partition in LLRetainedStamps (and in the rows of its members): shared by its sets
    int mSet = 0;                                               // 0 the main view's, 1 to 4 the sun cascades'
    U32 mCam = 0;                                               // the camera the set stands for
};
struct Ev { LLViewerOctreeGroup* mGroup; U8 mKind; };
static constexpr int kSets = 5;
static std::atomic<U64> sEvByType[16];                      // census: bounds events by partition type
static std::unordered_map<LLSpatialPartition*, PartState> sStateArr[kSets];
static std::mutex sMutex;
static std::vector<Ev> sEventsArr[kSets];
static std::atomic<bool> sInvalidateArr[kSets] = { true, true, true, true, true };
static std::atomic<bool> sSetLive[kSets];                   // the set has been updated lately: the hooks keep its events (a cascade set of a session without sun shadows keeps none)
static PartState& get_state(int set, LLSpatialPartition* part)
{
    PartState& st = sStateArr[set][part];
    st.mSet = set;
    st.mCam = set == 0 ? (U32)LLViewerCamera::CAMERA_WORLD : (U32)LLViewerCamera::CAMERA_SUN_SHADOW0 + (U32)set - 1;
    return st;
}
struct Hist { S32 mFrame[4] = {}; U8 mKind[4] = {}; U8 mN = 0; };
static std::unordered_map<const void*, Hist> sRecent;       // group -> its last four events (for the log of a difference)
static S32 sLastRebuildFrame = 0;
static std::unordered_set<LLViewerOctreeGroup*> sStale;      // groups that were told hizInvalidate and have not had the discard made (hizCollect resets the flag when it sends the box)
struct Stats
{
    std::atomic<U64> mReplayParts{ 0 }, mReplayGroups{ 0 };
    U64 mReorders = 0, mNodeEntryOnly = 0, mNodeReal = 0, mNodeMissing = 0, mNodeExtra = 0, mFrames = 0, mRebuilds = 0, mOccEvents = 0, mBoundsEvents = 0, mRewalks = 0, mUnder = 0, mAbsent = 0, mNodesErased = 0, mNodesWalked = 0, mCompares = 0, mDiffFrames = 0, mNodeDiffs = 0, mMemberDiffs = 0, mNodesFull = 0;
    double mUpdateMs = 0.0, mFullMs = 0.0;
};
static Stats sStats;

static bool sSlotUsed[256] = {};
static std::unordered_map<LLSpatialPartition*, std::pair<U32, U32>> sPartSlot;       // partition -> (slot, sets that use it)
static U32 acquire_slot(LLSpatialPartition* part)
{
    auto& e = sPartSlot[part];
    if (!e.first)
    {
        for (U32 i = 1; i < 256; ++i)
        {
            if (!sSlotUsed[i])
            {
                sSlotUsed[i] = true;
                for (U32 cam = 0; cam < 9; ++cam) { LLRetainedStamps::sFrame[i][cam].store(0); }
                e.first = i;
                break;
            }
        }
    }
    if (e.first)
    {
        ++e.second;
    }
    return e.first;
}
static void release_slot(LLSpatialPartition* part)
{
    auto it = sPartSlot.find(part);
    if (it != sPartSlot.end() && --it->second.second == 0)
    {
        sSlotUsed[it->second.first] = false;
        sPartSlot.erase(it);
    }
}
// a group leaves its set: its stamp is written back (what the set's replays stood for), then it is a plain group again
static void clear_member(LLSpatialGroup* g, U32 cam)
{
    LLGroupRows::Row& r = g->row();
    const U32 bit = 0x100u << cam;
    if (r.retainedSlot & bit)
    {
        const S32 f = LLRetainedStamps::sFrame[r.retainedSlot & 255u][cam].load(std::memory_order_relaxed);
        r.visible[cam] = std::max(r.visible[cam], f);
        if (cam < (U32)LLViewerCamera::CAMERA_WATER0)
        {
            r.anyVisible = std::max(r.anyVisible, f);
        }
        r.retainedSlot &= ~bit;
        if (!(r.retainedSlot >> 8))
        {
            r.retainedSlot = 0;
        }
    }
}

static void enumerate(const OctreeNode* node, std::vector<const void*>& out)
{
    out.push_back(node->getListener(0));
    for (U32 i = 0; i < node->getChildCount(); ++i)
    {
        enumerate(node->getChild(i), out);
    }
}

static void erase_node(PartState& st, const void* key, bool write_back = true)
{
    auto it = st.mNodes.find(key);
    if (it == st.mNodes.end())
    {
        return;
    }
    st.mPending.erase((LLSpatialGroup*)(LLViewerOctreeGroup*)key);
    if (write_back && it->second.mMember)
    {
        clear_member((LLSpatialGroup*)(LLViewerOctreeGroup*)key, st.mCam);
    }
    {
        auto pit = st.mPos.find((LLSpatialGroup*)(LLViewerOctreeGroup*)key);
        if (pit != st.mPos.end())
        {
            st.mOrder.erase(pit->second);
            st.mPos.erase(pit);
        }
    }
    st.mFlatDirty = true;
    if (it->second.mMember == 1) { st.mDrawable.erase((LLSpatialGroup*)(LLViewerOctreeGroup*)key); }
    else if (it->second.mMember == 2) { st.mVisible.erase((LLSpatialGroup*)(LLViewerOctreeGroup*)key); }
    st.mNodes.erase(it);
}

// the new entries go before `where` (the end of the list for a whole partition)
static void adopt(PartState& st, LLSpatialPartition::CullCollect& c, std::list<LLSpatialPartition::CullCollect::Item>::iterator where, LLCamera& camera, S32 frame)
{
    for (const auto& kv : c.mNodes) { st.mNodes[kv.first] = kv.second; }
    for (LLSpatialGroup* g : c.mDrawable) { st.mDrawable.insert(g); }
    for (LLSpatialGroup* g : c.mVisible) { st.mVisible.insert(g); }
    for (const auto& item : c.mOrder)
    {
        LLSpatialPartition::CullCollect::Item it = item;
        if (it.mKind)
        {
            LLSpatialGroup* g = it.mGroup;
            it.mLod = g->row().lodHash;
            st.mPending.insert(g);
            gPipeline.replayAdopted(g, camera, st.mSet == 0);     // (its wake and its distance: what markNotCulled does for a group it reaches)
        }
        st.mPos[it.mGroup] = st.mOrder.insert(where, it);
    }
    st.mFlatDirty = true;
}

// the order of the walk made again from the nodes: preorder, a node dropped as occluded first and alone, a member before the nodes below it
static void reorder(PartState& st, LLSpatialPartition* part)
{
    std::unordered_map<LLSpatialGroup*, LLSpatialPartition::CullCollect::Item> old;
    for (const auto& item : st.mOrder) { old[item.mGroup] = item; }
    st.mOrder.clear();
    st.mPos.clear();
    std::function<void(const OctreeNode*)> dfs = [&](const OctreeNode* n)
    {
        const void* key = n->getListener(0);
        auto it = st.mNodes.find(key);
        if (it == st.mNodes.end())
        {
            return;
        }
        LLSpatialGroup* g = (LLSpatialGroup*)(LLViewerOctreeGroup*)key;
        const auto& info = it->second;
        if (info.mOutcome == 1)
        {
            st.mPos[g] = st.mOrder.insert(st.mOrder.end(), LLSpatialPartition::CullCollect::Item{ g, 0 });
            return;
        }
        if (info.mMember)
        {
            LLSpatialPartition::CullCollect::Item item = old.count(g) ? old[g] : LLSpatialPartition::CullCollect::Item{ g, info.mMember };
            item.mKind = info.mMember;
            st.mPos[g] = st.mOrder.insert(st.mOrder.end(), item);
        }
        if (info.mOutcome == 3)
        {
            for (U32 i = 0; i < n->getChildCount(); ++i)
            {
                dfs(n->getChild(i));
            }
        }
    };
    dfs(part->mOctree);
    st.mReorder = false;
    st.mFlatDirty = true;
}

static void rebuild(LLCamera& camera, LLSpatialPartition* part, PartState& st)
{
    static LLSpatialPartition::CullCollect c;
    if (!st.mSlot)
    {
        st.mSlot = acquire_slot(part);
    }
    for (const auto& item : st.mOrder)
    {
        if (item.mKind && item.mGroup)
        {
            clear_member(item.mGroup, st.mCam);      // (the stamp is written back: a group that is no longer a member keeps what the replays stood for)
        }
    }
    st.mPending.clear();
    if (st.mSet) { part->collectCullShadow(camera, c); } else { part->collectCull(camera, c); }
    st.mNodes.clear();
    st.mDrawable.clear();
    st.mVisible.clear();
    st.mOrder.clear();
    st.mPos.clear();
    st.mReorder = false;
    adopt(st, c, st.mOrder.end(), camera, (S32)LLViewerOctreeEntryData::getCurrentFrame());
    st.mValid = st.mSlot != 0;
    ++sStats.mRebuilds;
    sLastRebuildFrame = (S32)LLFrameTimer::getFrameCount();
}

// the events of the frame, applied (and the roots of the tracked partitions rebounded, what the cull does first: that makes the events of this frame's rebounds).
// A frame whose camera signature changed leaves every state invalid (the cull walks); the first frame the camera is the same again rebuilds them from a collecting walk, the ones after apply events.
void update(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts, bool camera_changed, bool will_replay, int set)
{
    auto& sState = sStateArr[set];
    auto& sEvents = sEventsArr[set];
    sSetLive[set].store(true, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    static double sec_ms[6] = {};       // setup, stale discards, events (swap, merge, sort), rewalks, flat array, rest
    static U64 stale_total = 0, stale_frames = 0;
    auto sec_t = t0;
    auto lap = [&](int k) { const auto now = std::chrono::steady_clock::now(); sec_ms[k] += std::chrono::duration<double, std::milli>(now - sec_t).count(); sec_t = now; };
    ++sStats.mFrames;
    const S32 frame = (S32)LLFrameTimer::getFrameCount();
    // the partitions that are tracked now (a partition that went away takes its state with it)
    std::unordered_set<LLSpatialPartition*> alive(parts.begin(), parts.end());
    for (LLSpatialPartition* part : parts) { part->mVisTracked.store(true, std::memory_order_relaxed); }
    for (auto it = sState.begin(); it != sState.end();)
    {
        if (alive.count(it->first))
        {
            ++it;
            continue;
        }
        for (const auto& item : it->second.mOrder)
        {
            if (item.mKind && item.mGroup)
            {
                clear_member(item.mGroup, it->second.mCam);
            }
        }
        if (it->second.mSlot) { release_slot(it->first); }
        it = sState.erase(it);
    }
    for (LLSpatialPartition* part : parts)
    {
        ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();
    }
    const bool inval = camera_changed || sInvalidateArr[set].exchange(false);
    if (inval)
    {
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sEvents.clear();
        }
        for (LLSpatialPartition* part : parts) { get_state(set, part).mValid = false; }
        sStats.mUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return;
    }
    lap(0);
    stale_total += sStale.size();
    ++stale_frames;
    // the discards of awaited verdicts, which the walk makes when it visits a group (checkOcclusion): for the groups the last walk visited, now
    for (auto it = sStale.begin(); set == 0 && it != sStale.end();)
    {
        LLViewerOctreeGroup* g = *it;
        if (g->isDead())
        {
            it = sStale.erase(it);
            continue;
        }
        LLSpatialGroup* sg = (LLSpatialGroup*)g;
        if (!sg->hizNeedsMainThread())
        {
            it = sStale.erase(it);          // (the verdict arrived or the box was sent again: nothing is left to discard)
            continue;
        }
        auto sit = sState.find(sg->getSpatialPartition());
        if (sit != sState.end() && sit->second.mValid && sit->second.mNodes.count((const void*)g))
        {
            sg->checkOcclusion();
            it = sStale.erase(it);
            continue;
        }
        ++it;
    }
    lap(1);
    std::vector<Ev> events;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        events.swap(sEvents);
    }
    std::unordered_set<LLSpatialPartition*> rebuilt;
    for (LLSpatialPartition* part : parts)
    {
        PartState& st = get_state(set, part);
        if (!st.mValid)
        {
            rebuild(camera, part, st);
            rebuilt.insert(part);
        }
    }
    // shallowest first: a rewalk of a group covers every event below it
    auto depth = [](LLViewerOctreeGroup* g) { int d = 0; for (LLViewerOctreeGroup* a = g->getParent(); a; a = a->getParent()) { ++d; } return d; };
    std::unordered_map<LLViewerOctreeGroup*, U8> uniq;
    for (const Ev& e : events)
    {
        uniq[e.mGroup] |= (U8)(1u << e.mKind);
        if (e.mKind == 0) { ++sStats.mOccEvents; } else { ++sStats.mBoundsEvents; }
        {
            Hist& h = sRecent[(const void*)e.mGroup];
            h.mFrame[h.mN & 3] = frame;
            h.mKind[h.mN & 3] = e.mKind;
            ++h.mN;
        }
    }
    std::vector<std::pair<int, LLViewerOctreeGroup*>> order;
    order.reserve(uniq.size());
    for (const auto& kv : uniq)
    {
        if (!kv.first->isDead())
        {
            order.push_back({ depth(kv.first), kv.first });
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    lap(2);
    std::unordered_set<const void*> rewalked;
    static LLSpatialPartition::CullCollect c;
    std::vector<const void*> old_nodes;
    for (const auto& pr : order)
    {
        LLViewerOctreeGroup* g = pr.second;
        bool under = false;
        for (LLViewerOctreeGroup* a = g; a; a = a->getParent())
        {
            if (rewalked.count((const void*)a)) { under = true; break; }
        }
        if (under)
        {
            ++sStats.mUnder;
            continue;
        }
        LLSpatialGroup* sg = (LLSpatialGroup*)g;
        LLSpatialPartition* part = sg->getSpatialPartition();
        auto sit = sState.find(part);
        if (sit == sState.end() || !sg->getOctreeNode() || rebuilt.count(part))
        {
            continue;       // (a partition that is not tracked, or one just built from a walk that has these events in it)
        }
        PartState& st = sit->second;
        S32 entry = 0;
        auto nit = st.mNodes.find((const void*)g);
        if (nit != st.mNodes.end())
        {
            entry = nit->second.mEntryRes;
        }
        else
        {
            // not visited by the last walk: it can only be reached through its parent now (a new group; a parent whose verdict changed has an event of its own)
            LLViewerOctreeGroup* p = g->getParent();
            auto pit = p ? st.mNodes.find((const void*)p) : st.mNodes.end();
            if (pit == st.mNodes.end() || pit->second.mOutcome != 3)
            {
                ++sStats.mAbsent;
                continue;
            }
            const bool first_child = p->getOctreeNode() && p->getOctreeNode()->getChildCount() && p->getOctreeNode()->getChild(0)->getListener(0) == (void*)g;
            entry = pit->second.mResOut == 2 ? 2 : (first_child ? pit->second.mResOut : 0);
        }
        old_nodes.clear();
        enumerate(sg->getOctreeNode(), old_nodes);
        // the place of the subtree in the list: the first of its old entries (preorder), marked by a null entry that stays while they go
        std::list<LLSpatialPartition::CullCollect::Item>::iterator marker = st.mOrder.end();
        bool have_marker = false;
        for (const void* k : old_nodes)
        {
            auto pit = st.mPos.find((LLSpatialGroup*)(LLViewerOctreeGroup*)k);
            if (pit != st.mPos.end())
            {
                marker = st.mOrder.insert(pit->second, LLSpatialPartition::CullCollect::Item{ nullptr, 255 });
                have_marker = true;
                break;
            }
        }
        for (const void* k : old_nodes) { erase_node(st, k); }
        sStats.mNodesErased += old_nodes.size();
        c.mDrawable.clear(); c.mVisible.clear(); c.mOrder.clear(); c.mNodes.clear();
        if (set) { part->collectSubtreeShadow(camera, sg->getOctreeNode(), entry, c); } else { part->collectSubtree(camera, sg->getOctreeNode(), entry, c); }
        sStats.mNodesWalked += c.mNodes.size();
        if (have_marker)
        {
            adopt(st, c, marker, camera, (S32)LLViewerOctreeEntryData::getCurrentFrame());
            st.mOrder.erase(marker);
        }
        else
        {
            adopt(st, c, st.mOrder.end(), camera, (S32)LLViewerOctreeEntryData::getCurrentFrame());
            if (!c.mOrder.empty())
            {
                st.mReorder = true;     // (nothing of the subtree was in the list: where its entries belong is found from the nodes)
            }
        }
        rewalked.insert((const void*)g);
        ++sStats.mRewalks;
    }
    lap(3);
    {
        // (the update's own cost, once a minute or so, for the runs that do not verify: sStats.mUpdateMs is what verify() prints from)
        static U64 calls = 0;
        static double ms_total = 0.0, ms_still = 0.0, ms_max = 0.0;
        static U64 still_calls = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms_total += ms;
        ++calls;
        ++still_calls;
        ms_still += ms;
        ms_max = std::max(ms_max, ms);
        if (calls % 3000 == 0)
        {
            LL_INFOS("VisSet") << "retained set update on frames the camera did not change: " << ms_still / (double)still_calls << " ms a frame on average, the slowest " << ms_max << " ms; of it setup " << sec_ms[0] / (double)still_calls
                << ", stale discards " << sec_ms[1] / (double)still_calls << " (" << (double)stale_total / (double)std::max<U64>(1, stale_frames) << " groups in the set), events " << sec_ms[2] / (double)still_calls << ", rewalks " << sec_ms[3] / (double)still_calls
                << ", flat array " << sec_ms[4] / (double)still_calls << " (" << still_calls << " frames; events a frame "
                << (double)(sStats.mOccEvents + sStats.mBoundsEvents) / (double)std::max<U64>(1, sStats.mFrames) << ", rewalks " << (double)sStats.mRewalks / (double)std::max<U64>(1, sStats.mFrames) << ")" << LL_ENDL;
            {
                static U32 last_frame = 0;
                const U32 now_frame = LLFrameTimer::getFrameCount();
                const double frames = (double)std::max<U32>(1, now_frame - last_frame);
                last_frame = now_frame;
                std::string by_type;
                for (int ty = 0; ty < 16; ++ty)
                {
                    const U64 n = sEvByType[ty].exchange(0);
                    if (n) { by_type += " type " + std::to_string(ty) + ": " + std::to_string((double)n / frames); }
                }
                LL_INFOS("VisSet") << "bounds events a frame by partition type (all sets together, every partition):" << by_type << LL_ENDL;
            }
            ms_still = 0.0;
            still_calls = 0;
            ms_max = 0.0;
            for (double& x : sec_ms) { x = 0.0; }
            stale_total = stale_frames = 0;
        }
    }
    for (auto& kv : sState)
    {
        PartState& st = kv.second;
        if (st.mReorder) { reorder(st, kv.first); ++sStats.mReorders; }
        if (st.mValid && st.mFlatDirty)
        {
            st.mFlat.clear();
            st.mFlatKind.clear();
            st.mFlatLod.clear();
            st.mFlat.reserve(st.mOrder.size());
            st.mFlatKind.reserve(st.mOrder.size());
            st.mFlatLod.reserve(st.mOrder.size());
            for (const auto& item : st.mOrder)
            {
                st.mFlat.push_back(item.mGroup);
                st.mFlatKind.push_back(item.mKind);
                st.mFlatLod.push_back(item.mLod);
            }
            st.mFlatDirty = false;
        }
    }
    lap(4);
    if (will_replay)
    {
        // the replay of every valid set stands for the stamp of its members on this frame
        for (auto& kv : sState)
        {
            if (kv.second.mValid && kv.second.mSlot)
            {
                LLRetainedStamps::sFrame[kv.second.mSlot][kv.second.mCam].store((S32)LLViewerOctreeEntryData::getCurrentFrame(), std::memory_order_relaxed);
            }
        }
        finishFrame();
    }
    sStats.mUpdateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// the retained set against a full collecting walk of every partition; every difference is logged (the first ones in full)
void verify(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts)
{
    auto& sState = sStateArr[0];
    static LLSpatialPartition::CullCollect c;
    bool bad = false;
    ++sStats.mCompares;
    const S32 frame = (S32)LLFrameTimer::getFrameCount();
    for (LLSpatialPartition* part : parts)
    {
        auto sit = sState.find(part);
        if (sit == sState.end() || !sit->second.mValid)
        {
            continue;
        }
        PartState& st = sit->second;
        const auto t0 = std::chrono::steady_clock::now();
        part->collectCull(camera, c);
        sStats.mFullMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        sStats.mNodesFull += c.mNodes.size();
        U64 node_diffs = 0, member_diffs = 0;
        auto recent = [&](const void* k)
        {
            for (LLViewerOctreeGroup* a = (LLViewerOctreeGroup*)k; a && !a->isDead(); a = a->getParent())
            {
                auto r = sRecent.find((const void*)a);
                if (r != sRecent.end() && frame - r->second.mFrame[(r->second.mN - 1) & 3] <= 3) { return true; }
            }
            return false;
        };
        auto history = [&](const void* k)
        {
            std::string out;
            auto r = sRecent.find(k);
            if (r != sRecent.end())
            {
                for (U32 j = 0; j < std::min<U32>(4, r->second.mN); ++j)
                {
                    const U32 i = (r->second.mN - 1 - j) & 3;
                    out += std::string(r->second.mKind[i] ? "bounds@" : "occ@") + std::to_string(frame - r->second.mFrame[i]) + "ago ";
                }
            }
            return out.empty() ? std::string("none") : out;
        };
        // the topmost ancestor (the group itself last) whose retained node differs from the walk's
        auto divergence = [&](LLViewerOctreeGroup* g)
        {
            std::vector<LLViewerOctreeGroup*> chain;
            for (LLViewerOctreeGroup* a = g; a && !a->isDead(); a = a->getParent()) { chain.push_back(a); }
            for (size_t ci = chain.size(); ci-- > 0;)
            {
                const void* k = (const void*)chain[ci];
                auto w = c.mNodes.find(k);
                auto r = st.mNodes.find(k);
                const bool differ = (w == c.mNodes.end()) != (r == st.mNodes.end()) || (w != c.mNodes.end() && (w->second.mEntryRes != r->second.mEntryRes || w->second.mResOut != r->second.mResOut || w->second.mOutcome != r->second.mOutcome || w->second.mMember != r->second.mMember));
                if (differ)
                {
                    return "first differing node " + std::to_string((int)ci) + " levels above the group (walk " + (w == c.mNodes.end() ? std::string("not visited") : std::to_string((int)w->second.mEntryRes) + "/" + std::to_string((int)w->second.mResOut) + "/" + std::to_string((int)w->second.mOutcome))
                        + ", retained " + (r == st.mNodes.end() ? std::string("none") : std::to_string((int)r->second.mEntryRes) + "/" + std::to_string((int)r->second.mResOut) + "/" + std::to_string((int)r->second.mOutcome)) + ", its events " + history(k) + ")";
                }
            }
            return std::string("no differing node on its path");
        };
        for (const auto& kv : c.mNodes)
        {
            auto it = st.mNodes.find(kv.first);
            const bool same = it != st.mNodes.end() && it->second.mEntryRes == kv.second.mEntryRes && it->second.mResOut == kv.second.mResOut && it->second.mOutcome == kv.second.mOutcome && it->second.mMember == kv.second.mMember;
            if (!same)
            {
                ++node_diffs;
                if (it == st.mNodes.end()) { ++sStats.mNodeMissing; }
                else if (it->second.mResOut == kv.second.mResOut && it->second.mOutcome == kv.second.mOutcome && it->second.mMember == kv.second.mMember) { ++sStats.mNodeEntryOnly; }
                else { ++sStats.mNodeReal; }
                static int logged = 0;
                if (++logged <= 12)
                {
                    LL_WARNS("VisSet") << "retained set differs from the walk: partition type " << part->mPartitionType << " node " << kv.first << " walk (entry " << (int)kv.second.mEntryRes << ", out " << (int)kv.second.mResOut << ", outcome "
                        << (int)kv.second.mOutcome << ", member " << (int)kv.second.mMember << ") retained "
                        << (it == st.mNodes.end() ? std::string("none") : "(entry " + std::to_string((int)it->second.mEntryRes) + ", out " + std::to_string((int)it->second.mResOut) + ", outcome " + std::to_string((int)it->second.mOutcome) + ", member " + std::to_string((int)it->second.mMember) + ")")
                        << "; an event for it or a group above it in the last 3 frames: " << (recent(kv.first) ? "yes" : "no") << LL_ENDL;
                }
            }
        }
        {
            U64 extra = 0;
            for (const auto& kv : st.mNodes) { if (!c.mNodes.count(kv.first)) { ++extra; } }
            node_diffs += extra;
            sStats.mNodeExtra += extra;
        }
        std::unordered_set<LLSpatialGroup*> wd(c.mDrawable.begin(), c.mDrawable.end()), wv(c.mVisible.begin(), c.mVisible.end());
        member_diffs += (wd != st.mDrawable ? 1 : 0) + (wv != st.mVisible ? 1 : 0);
        if (wd != st.mDrawable || wv != st.mVisible)
        {
            static int logged_m = 0;
            auto note = [&](const char* what, LLSpatialGroup* g)
            {
                if (++logged_m <= 14)
                {
                    LLViewerOctreeGroup* par = g->getParent();
                    LL_WARNS("VisSet") << "member difference: " << what << " group " << (void*)g << " partition type " << part->mPartitionType << " elements " << g->getElementCount() << " dead " << g->isDead() << " occluded " << g->isOcclusionState(LLSpatialGroup::OCCLUDED)
                        << " parent occluded " << (par && ((LLSpatialGroup*)par)->isOcclusionState(LLSpatialGroup::OCCLUDED)) << " dirty " << g->isDirty() << " released " << g->hasState(LLSpatialGroup::FS_RELEASED) << " hiz awaited " << g->isHizAwaiting() << " frames since the last rebuild " << (frame - sLastRebuildFrame) << " its events " << history((const void*)(LLViewerOctreeGroup*)g)
                        << "; " << divergence((LLViewerOctreeGroup*)g) << "; an event for it or a group above it in the last 3 frames: " << (recent((const void*)(LLViewerOctreeGroup*)g) ? "yes" : "no")
                        << " walk node (entry/out/outcome) " << (c.mNodes.count((const void*)(LLViewerOctreeGroup*)g) ? std::to_string((int)c.mNodes[(const void*)(LLViewerOctreeGroup*)g].mEntryRes) + "/" + std::to_string((int)c.mNodes[(const void*)(LLViewerOctreeGroup*)g].mResOut) + "/" + std::to_string((int)c.mNodes[(const void*)(LLViewerOctreeGroup*)g].mOutcome) : std::string("not visited"))
                        << " retained node " << (st.mNodes.count((const void*)(LLViewerOctreeGroup*)g) ? std::to_string((int)st.mNodes[(const void*)(LLViewerOctreeGroup*)g].mEntryRes) + "/" + std::to_string((int)st.mNodes[(const void*)(LLViewerOctreeGroup*)g].mResOut) + "/" + std::to_string((int)st.mNodes[(const void*)(LLViewerOctreeGroup*)g].mOutcome) : std::string("none")) << LL_ENDL;
                }
            };
            for (LLSpatialGroup* g : wd) { if (!st.mDrawable.count(g)) note("walk lists it, retained does not (drawable)", g); }
            for (LLSpatialGroup* g : st.mDrawable) { if (!wd.count(g)) note("retained lists it, walk does not (drawable)", g); }
            for (LLSpatialGroup* g : wv) { if (!st.mVisible.count(g)) note("walk lists it, retained does not (visible)", g); }
            for (LLSpatialGroup* g : st.mVisible) { if (!wv.count(g)) note("retained lists it, walk does not (visible)", g); }
        }
        sStats.mNodeDiffs += node_diffs;
        sStats.mMemberDiffs += member_diffs;
        bad = bad || node_diffs || member_diffs;
    }
    sStats.mDiffFrames += bad ? 1 : 0;
    if (sStats.mCompares % 600 == 0)
    {
        const double n = (double)std::max<U64>(1, sStats.mFrames);
        LL_INFOS("VisSet") << "retained set check over " << sStats.mCompares << " frames: " << sStats.mDiffFrames << " frames differ (" << sStats.mNodeDiffs << " node differences: entry mRes only " << sStats.mNodeEntryOnly << ", outcome or member " << sStats.mNodeReal << ", missing in the retained set " << sStats.mNodeMissing << ", extra there " << sStats.mNodeExtra << "; " << sStats.mMemberDiffs << " member lists); a frame: "
            << sStats.mOccEvents / n << " occlusion events, " << sStats.mBoundsEvents / n << " bounds events, " << sStats.mRewalks / n << " subtree rewalks (" << sStats.mUnder / n << " under another, " << sStats.mAbsent / n << " ignored as not visited), nodes walked "
            << sStats.mNodesWalked / n << " (full walk " << sStats.mNodesFull / n << "), nodes dropped " << sStats.mNodesErased / n << "; rebuilds " << sStats.mRebuilds << "; order made again from the nodes " << sStats.mReorders << " times; partitions replayed " << sStats.mReplayParts.load() / n << " a frame with " << sStats.mReplayGroups.load() / n << " groups; update " << sStats.mUpdateMs / n << " ms, a full collecting walk " << sStats.mFullMs / n << " ms" << LL_ENDL;
    }
    if (sRecent.size() > 20000)
    {
        sRecent.clear();
    }
}
bool replayable(LLSpatialPartition* part, int set)
{
    auto& sState = sStateArr[set];
    auto it = sState.find(part);
    return it != sState.end() && it->second.mValid;
}

size_t replayCount(LLSpatialPartition* part, int set)
{
    auto& sState = sStateArr[set];
    auto it = sState.find(part);
    return it == sState.end() ? 0 : it->second.mFlat.size();
}

void replay(LLSpatialPartition* part, LLCamera& camera, size_t begin, size_t end, int set)
{
    auto& sState = sStateArr[set];
    auto it = sState.find(part);
    if (it == sState.end())
    {
        return;
    }
    PartState& st = it->second;
    end = std::min(end, st.mFlat.size());
    const U32 frame = (U32)LLViewerOctreeEntryData::getCurrentFrame();
    const U32 period = std::max<U32>(1, part->mLODPeriod);
    const U32 bucket = frame % period;
    for (size_t k = begin; k < end; ++k)
    {
        LLSpatialGroup* g = st.mFlat[k];
        if (!g)
        {
            continue;
        }
        const U8 kind = st.mFlatKind[k];
        if (set)
        {
            if (kind)
            {
                gPipeline.replayMemberShadow(g, kind);
            }
        }
        else if (kind == 0)
        {
            gPipeline.markOccluder(g);
        }
        else
        {
            // (what markNotCulled did, minus what the set already holds: the stamp is the set's, the distance was worked out when the group joined; the list it goes on, and the occlusion test of
            // a group whose turn it is by its LOD hash: the walk's test of a stamp older than the last frame is made after its setVisible, so it never holds)
            gPipeline.replayMember(g, kind, (U32)st.mFlatLod[k] == bucket);
        }
    }
    if (begin == 0)
    {
        sStats.mReplayParts.fetch_add(1, std::memory_order_relaxed);
    }
    sStats.mReplayGroups.fetch_add(end > begin ? end - begin : 0, std::memory_order_relaxed);
}

void finishFrame()
{
    for (int set = 0; set < kSets; ++set)
    {
        for (auto& kv : sStateArr[set])
        {
            PartState& st = kv.second;
            if (st.mPending.empty())
            {
                continue;
            }
            for (LLSpatialGroup* g : st.mPending)
            {
                U32& r = g->row().retainedSlot;
                r = st.mSlot | (r & ~255u) | (0x100u << st.mCam);
            }
            st.mPending.clear();
        }
    }
}

void checkOrder(LLCamera& camera, const std::vector<LLSpatialPartition*>& parts, int set)
{
    auto& sState = sStateArr[set];
    static LLSpatialPartition::CullCollect c;
    static int logged = 0;
    const S32 frame = (S32)LLFrameTimer::getFrameCount();
    for (LLSpatialPartition* part : parts)
    {
        auto sit = sState.find(part);
        if (sit == sState.end() || !sit->second.mValid)
        {
            continue;
        }
        PartState& st = sit->second;
        if (set) { part->collectCullShadow(camera, c); } else { part->collectCull(camera, c); }
        size_t i = 0;
        auto it = st.mOrder.begin();
        for (; i < c.mOrder.size() && it != st.mOrder.end(); ++i, ++it)
        {
            if (c.mOrder[i].mGroup != it->mGroup)
            {
                break;
            }
        }
        if (i == c.mOrder.size() && it == st.mOrder.end())
        {
            continue;
        }
        if (++logged > 10)
        {
            continue;
        }
        auto history = [&](const void* k)
        {
            std::string out;
            auto r = sRecent.find(k);
            if (r != sRecent.end())
            {
                for (U32 j = 0; j < std::min<U32>(4, r->second.mN); ++j)
                {
                    const U32 idx = (r->second.mN - 1 - j) & 3;
                    out += std::string(r->second.mKind[idx] ? "bounds@" : "occ@") + std::to_string(frame - r->second.mFrame[idx]) + "ago ";
                }
            }
            return out.empty() ? std::string("none") : out;
        };
        auto where = [&](LLSpatialGroup* g)
        {
            size_t pos = 0;
            for (const auto& item : st.mOrder) { if (item.mGroup == g) { return std::to_string(pos); } ++pos; }
            return std::string("not in the retained list");
        };
        LLSpatialGroup* want = i < c.mOrder.size() ? c.mOrder[i].mGroup : nullptr;
        LLSpatialGroup* have = it != st.mOrder.end() ? it->mGroup : nullptr;
        size_t want_pos_walk = 0;
        for (size_t k = 0; want && k < c.mOrder.size(); ++k) { if (c.mOrder[k].mGroup == have) { want_pos_walk = k; break; } }
        LL_WARNS("VisSet") << "retained ORDER differs from the walk: set " << set << " partition type " << part->mPartitionType << " first difference at " << i << " (walk " << c.mOrder.size() << " entries, retained " << st.mOrder.size() << "); the walk has "
            << (void*)want << " there (kind " << (want ? (int)c.mOrder[i].mKind : -1) << ", retained position " << (want ? where(want) : std::string("-")) << ", events " << (want ? history((const void*)(LLViewerOctreeGroup*)want) : std::string("-"))
            << "), the retained list has " << (void*)have << " (kind " << (have ? (int)it->mKind : -1) << ", walk position " << want_pos_walk << ", events " << (have ? history((const void*)(LLViewerOctreeGroup*)have) : std::string("-"))
            << "); orders made again from the nodes so far " << sStats.mReorders << ", rewalks " << sStats.mRewalks << ", rebuilds " << sStats.mRebuilds << ", frames since the last rebuild " << (frame - sLastRebuildFrame) << LL_ENDL;
    }
}

U32 checkStamps(S32 frame, int set)
{
    auto& sState = sStateArr[set];
    U32 bad = 0;
    for (auto& kv : sState)
    {
        PartState& st = kv.second;
        if (!st.mValid)
        {
            continue;
        }
        for (size_t k = 0; k < st.mFlat.size(); ++k)
        {
            LLSpatialGroup* g = st.mFlat[k];
            if (g && st.mFlatKind[k] && g->row().visible[st.mCam] != frame)
            {
                if (++bad <= 4)
                {
                    LL_WARNS("VisSet") << "stamp check: a member of a retained set (partition type " << kv.first->mPartitionType << ") was not stamped by the real walk on frame " << frame << ", its stored stamp is "
                        << g->row().visible[st.mCam] << LL_ENDL;
                }
            }
        }
    }
    return bad;
}
}   // namespace LLVisSet

void LLVisSetHooks::noteOcc(LLViewerOctreeGroup* group)
{
    LLViewerOctreePartition* part = group->getOctreePartition();
    if (!part || !part->mVisTracked.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(LLVisSet::sMutex);
    LLVisSet::sEventsArr[0].push_back({ group, 0 });
}

void LLVisSetHooks::noteBounds(LLViewerOctreeGroup* group)
{
    LLViewerOctreePartition* part = group->getOctreePartition();
    if (part)
    {
        LLVisSet::sEvByType[std::min<U32>(15, part->mPartitionType)].fetch_add(1, std::memory_order_relaxed);      // (census: every partition, tracked or walked)
    }
    if (!part || !part->mVisTracked.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(LLVisSet::sMutex);
    for (int set = 0; set < LLVisSet::kSets; ++set)
    {
        if (!LLVisSet::sSetLive[set].load(std::memory_order_relaxed))
        {
            continue;
        }
        auto& ev = LLVisSet::sEventsArr[set];
        if (ev.size() > 200000)
        {
            ev.clear();                                                         // (a set nobody updates: it starts over from a walk when it is next used)
            LLVisSet::sInvalidateArr[set].store(true);
            LLVisSet::sSetLive[set].store(false);
            continue;
        }
        ev.push_back({ group, 1 });
    }
}

void LLVisSetHooks::noteStale(LLViewerOctreeGroup* group)
{
    LLViewerOctreePartition* part = group->getOctreePartition();
    if (!part || !part->mVisTracked.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(LLVisSet::sMutex);
    LLVisSet::sStale.insert(group);
}

void LLVisSetHooks::forget(LLViewerOctreeGroup* group)
{
    {
        std::lock_guard<std::mutex> lock(LLVisSet::sMutex);
        for (int set = 0; set < LLVisSet::kSets; ++set)
        {
            auto& ev = LLVisSet::sEventsArr[set];
            ev.erase(std::remove_if(ev.begin(), ev.end(), [group](const LLVisSet::Ev& e) { return e.mGroup == group; }), ev.end());
        }
        LLVisSet::sStale.erase(group);
    }
    for (int set = 0; set < LLVisSet::kSets; ++set)
    {
        for (auto& kv : LLVisSet::sStateArr[set])
        {
            LLVisSet::erase_node(kv.second, (const void*)group);
        }
    }
    LLVisSet::sRecent.erase((const void*)group);
}

void LLVisSetHooks::invalidateAll()
{
    for (int set = 0; set < LLVisSet::kSets; ++set)
    {
        LLVisSet::sInvalidateArr[set].store(true);
    }
}

void pushVerts(LLFace* face)
{
    if (face)
    {
        llassert(face->verify());
        face->renderIndexed();
    }
}

void pushVerts(LLDrawable* drawable)
{
    for (S32 i = 0; i < drawable->getNumFaces(); ++i)
    {
        pushVerts(drawable->getFace(i));
    }
}

void pushVerts(LLVolume* volume)
{
    LLVertexBuffer::unbind();
    for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
    {
        const LLVolumeFace& face = volume->getVolumeFace(i);
        LLVertexBuffer::drawElements(LLRender::TRIANGLES, face.mPositions, NULL, face.mNumIndices, face.mIndices);
    }
}

void pushBufferVerts(LLVertexBuffer* buffer)
{
    if (buffer)
    {
        buffer->setBuffer();
        buffer->drawRange(LLRender::TRIANGLES, 0, buffer->getNumVerts()-1, buffer->getNumIndices(), 0);
    }
}

void pushBufferVerts(LLSpatialGroup* group, bool push_alpha = true)
{
    if (group->getSpatialPartition()->mRenderByGroup)
    {
        if (!group->mDrawMap.empty() && !group->mDrawMap.begin()->second.empty())
        {
            LLDrawInfo* params = *(group->mDrawMap.begin()->second.begin());
            LLRenderPass::applyModelMatrix(*params);

            if (push_alpha)
            {
                pushBufferVerts(group->mVertexBuffer);
            }

            for (LLSpatialGroup::buffer_map_t::iterator i = group->mBufferMap.begin(); i != group->mBufferMap.end(); ++i)
            {
                for (LLSpatialGroup::buffer_texture_map_t::iterator j = i->second.begin(); j != i->second.end(); ++j)
                {
                    for (LLSpatialGroup::buffer_list_t::iterator k = j->second.begin(); k != j->second.end(); ++k)
                    {
                        pushBufferVerts(*k);
                    }
                }
            }
        }
    }
    /*else
    {
        //const LLVector4a* bounds = group->getBounds();
        //drawBox(bounds[0], bounds[1]);
    }*/
}

// return false if drawable is rigged and:
//  - a linked rigged drawable has a different spatial group
//  - a linked rigged drawable face has the wrong draw order index
bool check_rigged_group(LLDrawable* drawable)
{
#if 0
    if (drawable->isState(LLDrawable::RIGGED))
    {
        LLSpatialGroup* group = drawable->getSpatialGroup();
        LLDrawable* root = drawable->getRoot();

        if (root->isState(LLDrawable::RIGGED) && root->getSpatialGroup() != group)
        {
            LL_WARNS() << "[root->isState(LLDrawable::RIGGED) and root->getSpatialGroup() != group] is true"
                " (" << root->getSpatialGroup() << " != " << group << ")" << LL_ENDL;
            llassert(false);
            return false;
        }

        S32 last_draw_index = -1;
        if (root->isState(LLDrawable::RIGGED))
        {
            for (auto& face : root->getFaces())
            {
                if ((S32) face->getDrawOrderIndex() <= last_draw_index)
                {
                    LL_WARNS() << "[(S32)face->getDrawOrderIndex() <= last_draw_index] is true"
                        " (" << (S32)face->getDrawOrderIndex() << " <= " << last_draw_index << ")" << LL_ENDL;
                    llassert(false);
                    return false;
                }
                last_draw_index = face->getDrawOrderIndex();
            }
        }

        for (auto& child : root->getVObj()->getChildren())
        {
            if (child->mDrawable->isState(LLDrawable::RIGGED))
            {
                for (auto& face : child->mDrawable->getFaces())
                {
                    if ((S32) face->getDrawOrderIndex() <= last_draw_index)
                    {
                        LL_WARNS() << "[(S32)face->getDrawOrderIndex() <= last_draw_index] is true"
                            " (" << (S32)face->getDrawOrderIndex() << " <= " << last_draw_index << ")" << LL_ENDL;
                        llassert(false);
                        return false;
                    }
                    last_draw_index = face->getDrawOrderIndex();
                }
            }

            if (child->mDrawable->getSpatialGroup() != group)
            {
                LL_WARNS() << "[child->mDrawable->getSpatialGroup() != group] is true"
                    " (" << child->mDrawable->getSpatialGroup() << " != " << group << ")" << LL_ENDL;
                llassert(false);
                return false;
            }
        }
    }
#endif
    return true;
}

void renderOctree(LLSpatialGroup* group)
{
    //render solid object bounding box, color
    //coded by buffer usage and activity
    gRender.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);
    LLVector4 col;
    if (group->mBuilt > 0.f)
    {
        group->mBuilt -= 2.f * gFrameIntervalSeconds.value();
        col.setVec(0.1f,0.1f,1,0.1f);

        {
            LLRenderState::DepthScope gl_depth(false, false);
            gRender.setPolygonMode(LLRenderState::PolygonMode::Line);

            gRender.diffuseColor4f(1,0,0,group->mBuilt);
            gRender.flush();
            gRender.setLineWidth(5.f); // <FS> Line width OGL core profile fix by Rye Mutt

            const LLVector4a* bounds = group->getObjectBounds();
            drawBoxOutline(bounds[0], bounds[1]);
            gRender.flush();
            gRender.setLineWidth(1.f); // <FS> Line width OGL core profile fix by Rye Mutt
            gRender.flush();

            const LLVOAvatar* lastAvatar = nullptr;
            U64 lastMeshId = 0;
            bool skipLastSkin = false;

            for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
            {
                LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
                if(!drawable || drawable->getNumFaces() == 0)
                {
                    continue;
                }

                llassert(check_rigged_group(drawable));

                if (!group->getSpatialPartition()->isBridge())
                {
                    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                    LLVector3 trans = drawable->getRegion()->getOriginAgent();
                    LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, trans.mV[0], trans.mV[1], trans.mV[2]);
                }

                LLFace* face = drawable->getFace(0);
                bool rigged = face->isState(LLFace::RIGGED);
                gDebugProgram.bind(rigged);

                gRender.diffuseColor4f(1, 0, 0, 1);

                if (rigged)
                {
                    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                    LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, gRenderModelView);
                    if (!LLRenderPass::uploadMatrixPalette(face->mAvatar, face->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                    {
                        continue;
                    }
                }
                for (S32 j = 0; j < drawable->getNumFaces(); j++)
                {
                    LLFace* face = drawable->getFace(j);
                    if (face && face->getVertexBuffer())
                    {
                        LLVOVolume* vol = drawable->getVOVolume();

                        if (gFrameTimeSeconds - face->row().lastUpdateTime < 0.5f)
                        {
                            if (vol && vol->isShrinkWrapped())
                            {
                                gRender.diffuseColor4f(0, 1, 1, group->mBuilt);
                            }
                            else
                            {
                                gRender.diffuseColor4f(0, 1, 0, group->mBuilt);
                            }
                        }
                        else if (gFrameTimeSeconds - face->row().lastMoveTime < 0.5f)
                        {
                            if (vol && vol->isShrinkWrapped())
                            {
                                gRender.diffuseColor4f(1, 1, 0, group->mBuilt);
                            }
                            else
                            {
                                gRender.diffuseColor4f(1, 0, 0, group->mBuilt);
                            }
                        }
                        else
                        {
                            continue;
                        }

                        face->getVertexBuffer()->setBuffer();
                        face->getVertexBuffer()->draw(LLRender::TRIANGLES, face->getIndicesCount(), face->getIndicesStart());
                    }
                }

                if (rigged)
                {
                    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
                }

                if (!group->getSpatialPartition()->isBridge())
                {
                    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
                }
            }
            gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
            gDebugProgram.bind(); // make sure non-rigged variant is bound
            gRender.diffuseColor4f(1,1,1,1);
        }
    }

    gRender.diffuseColor4fv(col.mV);
    LLVector4a fudge;
    fudge.splat(0.001f);

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    {
        //draw opaque outline
        gRender.diffuseColor4f(0,1,1,1);

        const LLVector4a* bounds = group->getBounds();
        drawBoxOutline(bounds[0], bounds[1]);
    }
}

std::set<LLSpatialGroup*> visible_selected_groups;



void renderXRay(LLSpatialGroup* group, LLCamera* camera)
{
    bool render_objects = (!LLPipeline::useOcclusion() || !group->isOcclusionState(LLSpatialGroup::OCCLUDED)) && group->isVisible() &&
                            !group->isEmpty();

    if (render_objects)
    {
        pushBufferVerts(group, false);

        bool selected = false;

        for (LLSpatialGroup::element_iter iter = group->getDataBegin(); iter != group->getDataEnd(); ++iter)
        {
            LLDrawable* drawable = (LLDrawable*)(*iter)->getDrawable();
            if (drawable->getVObj().notNull() && drawable->getVObj()->isSelected())
            {
                selected = true;
                break;
            }
        }

        if (selected)
        { //store for rendering occlusion volume as overlay

            if (!group->getSpatialPartition()->isBridge())
            {
                visible_selected_groups.insert(group);
            }
            else
            {
                visible_selected_groups.insert(group->getSpatialPartition()->asBridge()->getSpatialGroup());
            }
        }
    }
}

void renderCrossHairs(LLVector3 position, F32 size, LLColor4 color)
{
    gRender.color4fv(color.mV);
    gRender.begin(LLRender::LINES);
    {
        gRender.vertex3fv((position - LLVector3(size, 0.f, 0.f)).mV);
        gRender.vertex3fv((position + LLVector3(size, 0.f, 0.f)).mV);
        gRender.vertex3fv((position - LLVector3(0.f, size, 0.f)).mV);
        gRender.vertex3fv((position + LLVector3(0.f, size, 0.f)).mV);
        gRender.vertex3fv((position - LLVector3(0.f, 0.f, size)).mV);
        gRender.vertex3fv((position + LLVector3(0.f, 0.f, size)).mV);
    }
    gRender.end();
}

void renderUpdateType(LLDrawable* drawablep)
{
    LLViewerObject* vobj = drawablep->getVObj();
    if (!vobj || OUT_UNKNOWN == vobj->getLastUpdateType())
    {
        return;
    }
    LLRenderState::BlendScope blend(true);
    switch (vobj->getLastUpdateType())
    {
    case OUT_FULL:
        gRender.diffuseColor4f(0,1,0,0.5f);
        break;
    case OUT_TERSE_IMPROVED:
        gRender.diffuseColor4f(0,1,1,0.5f);
        break;
    case OUT_FULL_COMPRESSED:
        if (vobj->getLastUpdateCached())
        {
            gRender.diffuseColor4f(1,0,0,0.5f);
        }
        else
        {
            gRender.diffuseColor4f(1,1,0,0.5f);
        }
        break;
    case OUT_FULL_CACHED:
        gRender.diffuseColor4f(0,0,1,0.5f);
        break;
    default:
        LL_WARNS() << "Unknown update_type " << vobj->getLastUpdateType() << LL_ENDL;
        break;
    };
    S32 num_faces = drawablep->getNumFaces();
    if (num_faces)
    {
        for (S32 i = 0; i < num_faces; ++i)
        {
            pushVerts(drawablep->getFace(i));
        }
    }
}

void renderBoundingBox(LLDrawable* drawable, bool set_color = true)
{
    if (set_color)
    {
        if (drawable->isSpatialBridge())
        {
            gRender.diffuseColor4f(1,0.5f,0,1); // orange
        }
        else if (drawable->getVOVolume())
        {
            if (drawable->isRoot())
            {
                gRender.diffuseColor4f(1,1,0,1); // yellow
            }
            else
            {
                gRender.diffuseColor4f(0,1,0,1); // green
            }
        }
        else if (drawable->getVObj())
        {
            switch (drawable->getVObj()->getPCode())
            {
                case LLViewerObject::LL_VO_SURFACE_PATCH:
                        gRender.diffuseColor4f(0,1,1,1); // cyan
                        break;
                case LLViewerObject::LL_VO_CLOUDS:
                        // no longer used
                        break;
                case LLViewerObject::LL_VO_PART_GROUP:
                case LLViewerObject::LL_VO_HUD_PART_GROUP:
                        gRender.diffuseColor4f(0,0,1,1); // blue
                        break;
                case LLViewerObject::LL_VO_VOID_WATER:
                case LLViewerObject::LL_VO_WATER:
                        gRender.diffuseColor4f(0,0.5f,1,1); // medium blue
                        break;
                case LL_PCODE_LEGACY_TREE:
                        gRender.diffuseColor4f(0,0.5f,0,1); // dark green
                        break;
                default:
                        LLControlAvatar *cav = dynamic_cast<LLControlAvatar*>(drawable->getVObj()->asAvatar());
                        if (cav)
                        {
                            bool has_pos_constraint = (cav->mPositionConstraintFixup != LLVector3());
                            bool has_scale_constraint = (cav->mScaleConstraintFixup != 1.0f);
                            if (has_pos_constraint || has_scale_constraint)
                            {
                                gRender.diffuseColor4f(1,0,0,1);
                            }
                            else
                            {
                                gRender.diffuseColor4f(0,1,0.5,1);
                            }
                        }
                        else
                        {
                            gRender.diffuseColor4f(1,0,1,1); // magenta
                        }
                        break;
            }
        }
        else
        {
            gRender.diffuseColor4f(1,0,0,1);
        }
    }

    const LLVector4a* ext;
    LLVector4a pos, size;

    if (drawable->getVOVolume())
    {
        //render face bounding boxes
        for (S32 i = 0; i < drawable->getNumFaces(); i++)
        {
            LLFace* facep = drawable->getFace(i);
            if (facep)
            {
                ext = facep->row().extents;

                pos.setAdd(ext[0], ext[1]);
                pos.mul(0.5f);
                size.setSub(ext[1], ext[0]);
                size.mul(0.5f);

                drawBoxOutline(pos,size);
            }
        }
    }

    //render drawable bounding box
    ext = drawable->getSpatialExtents();

    pos.setAdd(ext[0], ext[1]);
    pos.mul(0.5f);
    size.setSub(ext[1], ext[0]);
    size.mul(0.5f);

    LLViewerObject* vobj = drawable->getVObj();
    if (vobj && vobj->onActiveList())
    {
        gRender.flush();
        gRender.setLineWidth(llmax(4.f*sinf(gFrameTimeSeconds*2.f)+1.f, 1.f)); // <FS> Line width OGL core profile fix by Rye Mutt
        //gRender.setLineWidth(4.f*(sinf(gFrameTimeSeconds*2.f)*0.25f+0.75f)); // <FS> Line width OGL core profile fix by Rye Mutt
        drawBoxOutline(pos,size);
        gRender.flush();
        gRender.setLineWidth(1.f); // <FS> Line width OGL core profile fix by Rye Mutt
    }
    else
    {
        drawBoxOutline(pos,size);
    }
}
// *TODO: LLDrawables which are not part of LLVOVolumes fall into a different
// code path which uses a shader - it was tested to be faster than mapping a
// vertex buffer in the terrain case. Consider using it for LLVOVolumes as well
// to simplify and speed up this debug code. Alternatively, a compute shader is
// likely faster. -Cosmic,2023-09-28
void renderNormals(LLDrawable *drawablep)
{
    if (!drawablep->isVisible())
        return;

    LLVertexBuffer::unbind();

    LLViewerObject* obj = drawablep->getVObj();
    LLVOVolume *vol = drawablep->getVOVolume();

    if (obj)
    {
        // <FS:Beq> FIX and improve renderNormals debug
        static LLCachedControl<bool> showSelectedOnly(gSavedSettings, "OnlyShowSelectedNormals");
        if (showSelectedOnly && !obj->isSelected())
        {
            obj->setDebugText("");
            return;
        }
        // </FS:Beq>

        LLRenderState::BlendScope blend(true);
        LLRenderState::DepthScope gl_depth(true, false);

        // Drawable's normals & tangents are stored in model space, i.e. before any scaling is applied.
        //
        // SL-13490, using pos + normal to compute the 2nd vertex of a normal line segment doesn't
        // work when there's a non-uniform scale in the mix. Normals require MVP-inverse-transpose
        // transform. We get that effect here by pre-applying the inverse scale (twice, because
        // one forward scale will be re-applied via the MVP in the vertex shader)

        LLVector4a inv_scale;
        float scale_len;
        if (vol)
        {
            LLVector3  scale_v3 = vol->getScale();
            LLVector4a obj_scale(scale_v3.mV[VX], scale_v3.mV[VY], scale_v3.mV[VZ]);
            obj_scale.normalize3();

            // Create inverse-scale vector for normals
            inv_scale.set(1.0f / scale_v3.mV[VX], 1.0f / scale_v3.mV[VY], 1.0f/ scale_v3.mV[VZ], 0.0f);
            inv_scale.mul(inv_scale);  // Squared, to apply inverse scale twice

            inv_scale.normalize3fast();
            scale_len = scale_v3.length();
        }
        else
        {
            inv_scale.set(1.0, 1.0, 1.0, 0.0);
            scale_len = 1.0;
        }

        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        if (vol)
        {
            LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (F32 *) vol->getRelativeXform().mMatrix);
        }

        gRender.getTexUnit(0)->bindWhite();

        // Normals &tangent line segments get scaled along with the object. Divide by scale length
        // to keep the as-viewed lengths (relatively) constant with the debug setting length
        float draw_length = gSavedSettings.getF32("RenderDebugNormalScale") / scale_len;

        std::vector<LLVolumeFace>* faces = nullptr;
        std::vector<LLFace*>* drawable_faces = nullptr;
        if (vol)
        {
            LLVolume* volume = vol->getVolume();
            faces = &volume->getVolumeFaces();
        }
        else
        {
            drawable_faces = &drawablep->getFaces();
        }

        if (faces)
        {
            for (auto it = faces->begin(); it != faces->end(); ++it)
            {
                const LLVolumeFace& face = *it;

                gRender.flush();
                gRender.diffuseColor4f(1, 1, 0, 1);
                gRender.begin(LLRender::LINES);
                for (S32 j = 0; j < face.mNumVertices; ++j)
                {
                    LLVector4a n, p;

                    n.setMul(face.mNormals[j], 1.0);
                    n.mul(inv_scale);  // Pre-scale normal, so it's left with an inverse-transpose xform after MVP
                    n.normalize3fast();
                    n.mul(draw_length);
                    p.setAdd(face.mPositions[j], n);

                    gRender.vertex3fv(face.mPositions[j].getF32ptr());
                    gRender.vertex3fv(p.getF32ptr());
                }
                gRender.end();

                // Tangents are simple vectors and do not require reorientation via pre-scaling
                if (face.mTangents)
                {
                    gRender.flush();
                    gRender.diffuseColor4f(0, 1, 1, 1);
                    gRender.begin(LLRender::LINES);
                    for (S32 j = 0; j < face.mNumVertices; ++j)
                    {
                        LLVector4a t, p;

                        t.setMul(face.mTangents[j], 1.0f);
                        t.normalize3fast();
                        t.mul(draw_length);
                        p.setAdd(face.mPositions[j], t);

                        gRender.vertex3fv(face.mPositions[j].getF32ptr());
                        gRender.vertex3fv(p.getF32ptr());
                    }
                    gRender.end();
                }
            }
        }
        else if (drawable_faces)
        {
            // *HACK: Prepare to restore previous shader as other debug code depends on a simpler shader being present
            llassert(LLShaderProgram::sCurBoundShaderPtr == &gDebugProgram);
            LLShaderProgram* prev_shader = LLShaderProgram::sCurBoundShaderPtr;
            for (auto it = drawable_faces->begin(); it != drawable_faces->end(); ++it)
            {
                LLFace* facep = *it;
                LLFace& face = **it;
                LLVertexBuffer* buf = face.getVertexBuffer();
                if (!buf) { continue; }
                U32 mask_vn = LLVertexBuffer::TYPE_VERTEX | LLVertexBuffer::TYPE_NORMAL;
                if ((buf->getTypeMask() & mask_vn) != mask_vn) { continue; }

                LLShaderProgram* shader;
                if ((buf->getTypeMask() & LLVertexBuffer::TYPE_TANGENT) != LLVertexBuffer::TYPE_TANGENT)
                {
                    shader = &gNormalDebugProgram[NORMAL_DEBUG_SHADER_DEFAULT];
                }
                else
                {
                    shader = &gNormalDebugProgram[NORMAL_DEBUG_SHADER_WITH_TANGENTS];
                }
                shader->bind();

                shader->uniform1f(LLShaderMgr::DEBUG_NORMAL_DRAW_LENGTH, draw_length);

                LLRenderPass::applyModelMatrix(&facep->getDrawable()->getRegion()->mRenderMatrix);

                buf->setBuffer();
                // *NOTE: The render type in the vertex shader is TRIANGLES, but gets converted to LINES in the geometry shader
                // *NOTE: For terrain normal debug, this seems to also include vertices for water, which is technically not part of the terrain. Should fix that at some point.
                buf->drawRange(LLRender::TRIANGLES, face.getGeomIndex(), face.getGeomIndex() + face.getGeomCount()-1, face.getIndicesCount(), face.getIndicesStart());
            }
            if (prev_shader)
            {
                prev_shader->bind();
            }
        }

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }
}

S32 get_physics_detail(const LLVolumeParams& volume_params, const LLVector3& scale)
{
    const S32 DEFAULT_DETAIL = 1;
    const F32 LARGE_THRESHOLD = 5.f;
    const F32 MEGA_THRESHOLD = 25.f;

    S32 detail = DEFAULT_DETAIL;
    F32 avg_scale = (scale[0]+scale[1]+scale[2])/3.f;

    if (avg_scale > LARGE_THRESHOLD)
    {
        detail += 1;
        if (avg_scale > MEGA_THRESHOLD)
        {
            detail += 1;
        }
    }

    return detail;
}

// <FS:Beq> Restore physics shape rendering in edit view.
void renderMeshBaseHullWithOutline(LLVOVolume* volume, U32 data_mask, LLColor4& color, LLColor4& line_color)
{
    LLUUID mesh_id = volume->getVolume()->getParams().getSculptID();
    LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);

    const LLVector3 center(0,0,0);
    const LLVector3 size(0.25f,0.25f,0.25f);

    if (decomp)
    {
        if (!decomp->mBaseHullMesh.empty())
        {
            gRender.diffuseColor4fv(color.mV);
            LLVertexBuffer::drawArrays(LLRender::TRIANGLES, decomp->mBaseHullMesh.mPositions);

            gRender.setPolygonMode(LLRenderState::PolygonMode::Line);
            gRender.diffuseColor4fv(line_color.mV);
            LLVertexBuffer::drawArrays(LLRender::TRIANGLES, decomp->mBaseHullMesh.mPositions);
            gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
        }
        else
        {
            gMeshRepo.buildPhysicsMesh(*decomp);
            gRender.diffuseColor4f(0,1,1,1);
            drawBoxOutline(center, size);
        }

    }
    else
    {
        gRender.diffuseColor3f(1,0,1);
        drawBoxOutline(center, size);
    }
}
// </FS:Beq>

void renderMeshBaseHull(LLVOVolume* volume, U32 data_mask, LLColor4& color)
{
    LLUUID mesh_id = volume->getVolume()->getParams().getSculptID();
    LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);

    const LLVector3 center(0,0,0);
    const LLVector3 size(0.25f,0.25f,0.25f);

    if (decomp)
    {
        if (!decomp->mBaseHullMesh.empty())
        {
            gRender.diffuseColor4fv(color.mV);
            LLVertexBuffer::drawArrays(LLRender::TRIANGLES, decomp->mBaseHullMesh.mPositions);
        }
        else
        {
            gMeshRepo.buildPhysicsMesh(*decomp);
            gRender.diffuseColor4f(0,1,1,1);
            drawBoxOutline(center, size);
        }

    }
    else
    {
        gRender.diffuseColor3f(1,0,1);
        drawBoxOutline(center, size);
    }
}

// <FS:Beq> restore physics shape in edit view display
void render_hull_with_outline(LLModel::PhysicsMesh& mesh, const LLColor4& color, const LLColor4& line_color)
{
    gRender.diffuseColor4fv(color.mV);
    LLVertexBuffer::drawArrays(LLRender::TRIANGLES, mesh.mPositions);
    gRender.setPolygonMode(LLRenderState::PolygonMode::Line);
    gRender.setLineWidth(3.f); // <FS> Line width OGL core profile fix by Rye Mutt
    gRender.diffuseColor4fv(line_color.mV);
    LLVertexBuffer::drawArrays(LLRender::TRIANGLES, mesh.mPositions);
    gRender.setLineWidth(1.f); // <FS> Line width OGL core profile fix by Rye Mutt
    gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
}
// </FS:Beq>

void render_hull(LLModel::PhysicsMesh& mesh, const LLColor4& color)
{
    gRender.diffuseColor4fv(color.mV);
    LLVertexBuffer::drawArrays(LLRender::TRIANGLES, mesh.mPositions);
}

void renderPhysicsShape(LLDrawable* drawable, LLVOVolume* volume, bool wireframe)
{
    U8 physics_type = volume->getPhysicsShapeType();

    if (physics_type == LLViewerObject::PHYSICS_SHAPE_NONE || volume->isFlexible())
    {
        return;
    }

    //not allowed to return at this point without rendering *something*

    F32 threshold = gSavedSettings.getF32("ObjectCostHighThreshold");
    F32 cost = volume->getObjectCost();

    LLColor4 low = gSavedSettings.getColor4("ObjectCostLowColor");
    LLColor4 mid = gSavedSettings.getColor4("ObjectCostMidColor");
    LLColor4 high = gSavedSettings.getColor4("ObjectCostHighColor");

    F32 normalizedCost = 1.f - exp( -(cost / threshold) );

    LLColor4 color;
    if ( normalizedCost <= 0.5f )
    {
        color = lerp( low, mid, 2.f * normalizedCost );
    }
    else
    {
        color = lerp( mid, high, 2.f * ( normalizedCost - 0.5f ) );
    }

    if (wireframe)
    {
        color = color * 0.5f;
    }

    U32 data_mask = LLVertexBuffer::MAP_VERTEX;

    LLVolumeParams volume_params = volume->getVolume()->getParams();

    LLPhysicsVolumeParams physics_params(volume_params,
        physics_type == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);

    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification physics_spec;

    //<FS:Beq> FIRE-23053 Physics shape display fixes
    //  LLPhysicsShapeBuilderUtil::determinePhysicsShape(physics_params, volume->getScale(), physics_spec);
    LLUUID mesh_id;
    LLModel::Decomposition* decomp = nullptr;
    bool hasConvexDecomp = false;

    // If we are a mesh and the mesh has a hul decomp (is analysed) then set hasDecomp to true
    if (volume->isMesh()){
        mesh_id = volume_params.getSculptID();
        decomp = gMeshRepo.getDecomposition(mesh_id);
        if (decomp && !decomp->mHull.empty()){ hasConvexDecomp = true; }
    }

    LLPhysicsShapeBuilderUtil::determinePhysicsShape(physics_params, volume->getScale(), hasConvexDecomp, physics_spec);
    //</FS:Beq>

    U32 type = physics_spec.getType();

    LLVector3 center(0,0,0);
    LLVector3 size(0.25f,0.25f,0.25f);

    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
    LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (F32*) volume->getRelativeXform().mMatrix);

    if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::USER_MESH)
    {
        //<FS:Beq> FIRE-23053 Physics shape display fixes
        //LLUUID mesh_id = volume->getVolume()->getParams().getSculptID();
        //LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
        //</FS:Beq>

        if (decomp)
        { //render a physics based mesh

            gRender.getTexUnit(0)->bindWhite();

            if (!decomp->mHull.empty())
            { //decomposition exists, use that

                if (decomp->mMesh.empty())
                {
                    gMeshRepo.buildPhysicsMesh(*decomp);
                }

                for (U32 i = 0; i < decomp->mMesh.size(); ++i)
                {
                    render_hull(decomp->mMesh[i], color);
                }
            }
            else if (!decomp->mPhysicsShapeMesh.empty())
            {
                //decomp has physics mesh, render that mesh
                gRender.diffuseColor4fv(color.mV);

                LLVertexBuffer::drawArrays(LLRender::TRIANGLES, decomp->mPhysicsShapeMesh.mPositions);
            }
            else
            { //no mesh or decomposition, render base hull
                renderMeshBaseHull(volume, data_mask, color);

                if (decomp->mPhysicsShapeMesh.empty())
                {
                    //attempt to fetch physics shape mesh if available
                    gMeshRepo.fetchPhysicsShape(mesh_id);
                }
            }
        }
        else
        {
            gRender.diffuseColor3f(1,1,0);
            drawBoxOutline(center, size);
        }
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::USER_CONVEX ||
        type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::PRIM_CONVEX)
    {
        if (volume->isMesh())
        {
            renderMeshBaseHull(volume, data_mask, color);
        }
        else
        {
            LLVolumeParams volume_params = volume->getVolume()->getParams();
            S32 detail = get_physics_detail(volume_params, volume->getScale());
            LLVolume* phys_volume = LLPrimitive::sVolumeManager->refVolume(volume_params, detail);

            if (!phys_volume->mHullPoints)
            { //build convex hull
                std::vector<LLVector3> pos;
                std::vector<U16> index;

                S32 index_offset = 0;

                for (S32 i = 0; i < phys_volume->getNumVolumeFaces(); ++i)
                {
                    const LLVolumeFace& face = phys_volume->getVolumeFace(i);
                    if (index_offset + face.mNumVertices > 65535)
                    {
                        continue;
                    }

                    for (S32 j = 0; j < face.mNumVertices; ++j)
                    {
                        pos.push_back(LLVector3(face.mPositions[j].getF32ptr()));
                    }

                    for (S32 j = 0; j < face.mNumIndices; ++j)
                    {
                        index.push_back(face.mIndices[j]+index_offset);
                    }

                    index_offset += face.mNumVertices;
                }

                if (!pos.empty() && !index.empty() && LLConvexDecomposition::getInstance() ) // ND: FIRE-3427
                {
                    LLCDMeshData mesh;
                    mesh.mIndexBase = &index[0];
                    mesh.mVertexBase = pos[0].mV;
                    mesh.mNumVertices = static_cast<int>(pos.size());
                    mesh.mVertexStrideBytes = 12;
                    mesh.mIndexStrideBytes = 6;
                    mesh.mIndexType = LLCDMeshData::INT_16;

                    mesh.mNumTriangles = static_cast<int>(index.size())/3;

                    LLCDMeshData res;

                    LLConvexDecomposition::getInstance()->generateSingleHullMeshFromMesh( &mesh, &res );

                    //copy res into phys_volume
                    phys_volume->mHullPoints = (LLVector4a*) ll_aligned_malloc_16(sizeof(LLVector4a)*res.mNumVertices);
                    phys_volume->mNumHullPoints = res.mNumVertices;

                    S32 idx_size = (res.mNumTriangles*3*2+0xF) & ~0xF;
                    phys_volume->mHullIndices = (U16*) ll_aligned_malloc_16(idx_size);
                    phys_volume->mNumHullIndices = res.mNumTriangles*3;

                    const F32* v = res.mVertexBase;

                    for (S32 i = 0; i < res.mNumVertices; ++i)
                    {
                        F32* p = (F32*) ((U8*)v+i*res.mVertexStrideBytes);
                        phys_volume->mHullPoints[i].load3(p);
                    }

                    if (res.mIndexType == LLCDMeshData::INT_16)
                    {
                        for (S32 i = 0; i < res.mNumTriangles; ++i)
                        {
                            U16* idx = (U16*) (((U8*)res.mIndexBase)+i*res.mIndexStrideBytes);

                            phys_volume->mHullIndices[i*3+0] = idx[0];
                            phys_volume->mHullIndices[i*3+1] = idx[1];
                            phys_volume->mHullIndices[i*3+2] = idx[2];
                        }
                    }
                    else
                    {
                        for (S32 i = 0; i < res.mNumTriangles; ++i)
                        {
                            U32* idx = (U32*) (((U8*)res.mIndexBase)+i*res.mIndexStrideBytes);

                            phys_volume->mHullIndices[i*3+0] = (U16) idx[0];
                            phys_volume->mHullIndices[i*3+1] = (U16) idx[1];
                            phys_volume->mHullIndices[i*3+2] = (U16) idx[2];
                        }
                    }
                }
            }

            // <FS:Ansariel> Crash fix due to invalid calls to drawElements by Drake Arconis
            //if (phys_volume->mHullPoints)
            if (phys_volume->mHullPoints && phys_volume->mHullIndices && phys_volume->mNumHullPoints > 0 && phys_volume->mNumHullIndices > 0)
            // </FS:Ansariel>
            {
                //render hull
                gRender.diffuseColor4fv(color.mV);

                LLVertexBuffer::unbind();
                LLVertexBuffer::drawElements(LLRender::TRIANGLES, phys_volume->mHullPoints, NULL, phys_volume->mNumHullIndices, phys_volume->mHullIndices);
            }
            else
            {
                gRender.diffuseColor4f(1,0,1,1);
                drawBoxOutline(center, size);
            }

            LLPrimitive::sVolumeManager->unrefVolume(phys_volume);
        }
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::BOX)
    {
        if (!wireframe)
        {
            LLVector3 center = physics_spec.getCenter();
            LLVector3 scale = physics_spec.getScale();
            LLVector3 vscale = volume->getScale() * 2.f;
            scale.set(scale[0] / vscale[0], scale[1] / vscale[1], scale[2] / vscale[2]);

            gRender.diffuseColor4fv(color.mV);
            drawBox(center, scale);
        }
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::SPHERE)
    {
        if (!wireframe)
        {
            LLVolumeParams volume_params;
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
            volume_params.setBeginAndEndS(0.f, 1.f);
            volume_params.setBeginAndEndT(0.f, 1.f);
            volume_params.setRatio(1, 1);
            volume_params.setShear(0, 0);
            LLVolume* sphere = LLPrimitive::sVolumeManager->refVolume(volume_params, 3);

            gRender.diffuseColor4fv(color.mV);
            pushVerts(sphere);
            LLPrimitive::sVolumeManager->unrefVolume(sphere);
        }
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::CYLINDER)
    {
        if (!wireframe)
        {
            LLVolumeParams volume_params;
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
            volume_params.setBeginAndEndS(0.f, 1.f);
            volume_params.setBeginAndEndT(0.f, 1.f);
            volume_params.setRatio(1, 1);
            volume_params.setShear(0, 0);
            LLVolume* cylinder = LLPrimitive::sVolumeManager->refVolume(volume_params, 3);

            gRender.diffuseColor4fv(color.mV);
            pushVerts(cylinder);
            LLPrimitive::sVolumeManager->unrefVolume(cylinder);
        }
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::PRIM_MESH)
    {
        LLVolumeParams volume_params = volume->getVolume()->getParams();
        S32 detail = get_physics_detail(volume_params, volume->getScale());

        LLVolume* phys_volume = LLPrimitive::sVolumeManager->refVolume(volume_params, detail);

        gRender.diffuseColor4fv(color.mV);
        pushVerts(phys_volume);

        LLPrimitive::sVolumeManager->unrefVolume(phys_volume);
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::PRIM_CONVEX)
    {
        LLVolumeParams volume_params = volume->getVolume()->getParams();
        S32 detail = get_physics_detail(volume_params, volume->getScale());

        LLVolume* phys_volume = LLPrimitive::sVolumeManager->refVolume(volume_params, detail);

        if (phys_volume->mHullPoints && phys_volume->mHullIndices)
        {

            llassert(LLShaderProgram::sCurBoundShader != 0);
            LLVertexBuffer::unbind();

            gRender.diffuseColor4fv(color.mV);

            gRender.begin(LLRender::TRIANGLES);
            for (S32 hull_i = 0; hull_i < phys_volume->mNumHullIndices; ++hull_i)
            {
                gRender.vertex3fv(phys_volume->mHullPoints[phys_volume->mHullIndices[hull_i]].getF32ptr());
            }
            gRender.end();
        }
        else
        {
            gRender.diffuseColor3f(1,0,1);
            drawBoxOutline(center, size);
            gMeshRepo.buildHull(volume_params, detail);
        }
        LLPrimitive::sVolumeManager->unrefVolume(phys_volume);
    }
    else if (type == LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::SCULPT)
    {
        //TODO: implement sculpted prim physics display
    }
    else
    {
        LL_ERRS() << "Unhandled type" << LL_ENDL;
    }

    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
}

void renderPhysicsShapes(LLSpatialGroup* group, bool wireframe)
{
    for (OctreeNode::const_element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        if(!drawable)
    {
            continue;
        }

        if (drawable->isSpatialBridge())
        {
            LLSpatialBridge* bridge = drawable->asPartition()->asBridge();

            if (bridge)
            {
                LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (F32*)bridge->mDrawable->getRenderMatrix().mMatrix);
                bridge->renderPhysicsShapes(wireframe);
                LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
            }
        }
        else
        {
            LLVOVolume* volume = drawable->getVOVolume();
            if (volume && !volume->isAttachment() && volume->getPhysicsShapeType() != LLViewerObject::PHYSICS_SHAPE_NONE )
            {
                if (!group->getSpatialPartition()->isBridge())
                {
                    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                    LLVector3 trans = drawable->getRegion()->getOriginAgent();
                    LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, trans.mV[0], trans.mV[1], trans.mV[2]);
                    renderPhysicsShape(drawable, volume, wireframe);
                    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
                }
                else
                {
                    renderPhysicsShape(drawable, volume, wireframe);
                }
            }
            else
            {
#if 0
                LLViewerObject* object = drawable->getVObj();
                if (object && object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH)
                {
                    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                    LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (F32*) object->getRegion()->mRenderMatrix.mMatrix);
                    //push face vertices for terrain
                    for (S32 i = 0; i < drawable->getNumFaces(); ++i)
                    {
                        LLFace* face = drawable->getFace(i);
                        if (face)
                        {
                            LLVertexBuffer* buff = face->getVertexBuffer();
                            if (buff)
                            {
                                gRender.setPolygonMode(LLRenderState::PolygonMode::Line);

                                buff->setBuffer();
                                gRender.diffuseColor4f(0.2f, 0.5f, 0.3f, 0.5f);
                                buff->draw(LLRender::TRIANGLES, buff->getNumIndices(), 0);

                                gRender.diffuseColor4f(0.2f, 1.f, 0.3f, 0.75f);
                                gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
                                buff->draw(LLRender::TRIANGLES, buff->getNumIndices(), 0);
                            }
                        }
                    }
                    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
                }
#endif
            }
        }
    }
}

void renderTexturePriority(LLDrawable* drawable)
{
    for (int face=0; face<drawable->getNumFaces(); ++face)
    {
        LLFace *facep = drawable->getFace(face);

        LLVector4 cold(0,0,0.25f);
        LLVector4 hot(1,0.25f,0.25f);

        LLVector4 boost_cold(0,0,0,0);
        LLVector4 boost_hot(0,1,0,1);

        LLRenderState::BlendScope blend(false);

        if (facep)
        {
            F32 vsize = facep->getPixelArea();

            if (vsize > sCurMaxTexPriority)
            {
                sCurMaxTexPriority = vsize;
            }

            F32 t = vsize/sLastMaxTexPriority;

            LLVector4 col = lerp(cold, hot, t);
            gRender.diffuseColor4fv(col.mV);
        }
        //else
        //{
        //  gRender.diffuseColor4f(1,0,1,1);
        //}

        LLVector4a center;
        center.setAdd(facep->row().extents[1],facep->row().extents[0]);
        center.mul(0.5f);
        LLVector4a size;
        size.setSub(facep->row().extents[1],facep->row().extents[0]);
        size.mul(0.5f);
        size.add(LLVector4a(0.01f));
        drawBox(center, size);
    }
}

void renderPoints(LLDrawable* drawablep)
{
    LLRenderState::DepthScope depth(false, false);
    if (drawablep->getNumFaces())
    {
        gRender.begin(LLRender::POINTS);
        gRender.diffuseColor3f(1,1,1);
        for (S32 i = 0; i < drawablep->getNumFaces(); i++)
        {
            LLFace * face = drawablep->getFace(i);
            if (face)
            {
                gRender.vertex3fv(face->row().centerLocal.mV);
            }
        }
        gRender.end();
    }
}

void renderTexelDensity(LLDrawable* drawable)
{
    if (LLViewerTexture::sDebugTexelsMode == LLViewerTexture::DEBUG_TEXELS_OFF
        || LLViewerTexture::sCheckerBoardImagep.isNull())
    {
        return;
    }

    LLRenderState::BlendScope _(true);

    LLMatrix4 checkerboard_matrix;
    S32 discard_level = -1;

    for (S32 f = 0; f < drawable->getNumFaces(); f++)
    {
        LLFace* facep = drawable->getFace(f);
        LLVertexBuffer* buffer = facep->getVertexBuffer();
        LLViewerTexture* texturep = facep->getTexture();

        if (texturep == NULL) continue;

        switch(LLViewerTexture::sDebugTexelsMode)
        {
        case LLViewerTexture::DEBUG_TEXELS_CURRENT:
            discard_level = -1;
            break;
        case LLViewerTexture::DEBUG_TEXELS_DESIRED:
            {
                LLViewerFetchedTexture* fetched_texturep = dynamic_cast<LLViewerFetchedTexture*>(texturep);
                discard_level = fetched_texturep ? fetched_texturep->getDesiredDiscardLevel() : -1;
                break;
            }
        default:
        case LLViewerTexture::DEBUG_TEXELS_FULL:
            discard_level = 0;
            break;
        }

        checkerboard_matrix.initScale(LLVector3((F32)texturep->getWidth(discard_level) / 8.f, (F32)texturep->getHeight(discard_level) / 8.f, 1.f));

        gRender.getTexUnit(0)->bind(LLViewerTexture::sCheckerBoardImagep, true);
        LLRenderMatrix::load(LLRenderMatrix::Kind::Texture0, (F32*)&checkerboard_matrix.mMatrix);

        if (buffer && (facep->getGeomCount() >= 3))
        {
            buffer->setBuffer();
            U16 start = facep->getGeomStart();
            U16 end = start + facep->getGeomCount()-1;
            U32 count = facep->getIndicesCount();
            U16 offset = facep->getIndicesStart();
            buffer->drawRange(LLRender::TRIANGLES, start, end, count, offset);
        }

        LLRenderMatrix::identity(LLRenderMatrix::Kind::Texture0);
    }

    //S32 num_textures = llmax(1, (S32)params->mTextureList.size());

    //for (S32 i = 0; i < num_textures; i++)
    //{
    //  LLViewerTexture* texturep = params->mTextureList.empty() ? params->mTexture.get() : params->mTextureList[i].get();
    //  if (texturep == NULL) continue;

    //  LLMatrix4 checkboard_matrix;
    //  S32 discard_level = -1;
    //  switch(LLViewerTexture::sDebugTexelsMode)
    //  {
    //  case LLViewerTexture::DEBUG_TEXELS_CURRENT:
    //      discard_level = -1;
    //      break;
    //  case LLViewerTexture::DEBUG_TEXELS_DESIRED:
    //      {
    //          LLViewerFetchedTexture* fetched_texturep = dynamic_cast<LLViewerFetchedTexture*>(texturep);
    //          discard_level = fetched_texturep ? fetched_texturep->getDesiredDiscardLevel() : -1;
    //          break;
    //      }
    //  default:
    //  case LLViewerTexture::DEBUG_TEXELS_FULL:
    //      discard_level = 0;
    //      break;
    //  }

    //  checkboard_matrix.initScale(LLVector3(texturep->getWidth(discard_level) / 8, texturep->getHeight(discard_level) / 8, 1.f));
    //  gRender.getTexUnit(i)->activate();

    //  glMatrixMode(GL_TEXTURE);
    //  glPushMatrix();
    //  glLoadIdentity();
    //  //gRender.matrixMode(LLRender::MM_TEXTURE);
    //  glLoadMatrixf((F32*) checkboard_matrix.mMatrix);

    //  gRender.getTexUnit(i)->bind(LLViewerTexture::sCheckerBoardImagep, true);

    //  pushVerts(params, LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_NORMAL );

    //  glPopMatrix();
    //  glMatrixMode(GL_MODELVIEW);
    //  //gRender.matrixMode(LLRender::MM_MODELVIEW);
    //}
}


void renderLights(LLDrawable* drawablep)
{
    if (!drawablep->isLight())
    {
        return;
    }

    if (drawablep->getNumFaces())
    {
        LLRenderState::BlendScope blend(true);
        gRender.diffuseColor4f(0,1,1,0.5f);

        for (S32 i = 0; i < drawablep->getNumFaces(); i++)
        {
            LLFace * face = drawablep->getFace(i);
            if (face)
            {
                pushVerts(face);
            }
        }

        const LLVector4a* ext = drawablep->getSpatialExtents();

        LLVector4a pos;
        pos.setAdd(ext[0], ext[1]);
        pos.mul(0.5f);
        LLVector4a size;
        size.setSub(ext[1], ext[0]);
        size.mul(0.5f);

        {
            LLRenderState::DepthScope depth(false, true);
            gRender.diffuseColor4f(1,1,1,1);
            drawBoxOutline(pos, size);
        }

        gRender.diffuseColor4f(1,1,0,1);
        F32 rad = drawablep->getVOVolume()->getLightRadius();
        drawBoxOutline(pos, LLVector4a(rad));
    }
}

class LLRenderOctreeRaycast : public LLOctreeTriangleRayIntersect
{
public:
    LLRenderOctreeRaycast(const LLVector4a& start, const LLVector4a& dir, F32* closest_t)
        : LLOctreeTriangleRayIntersect(start, dir, nullptr, closest_t, NULL, NULL, NULL, NULL)
    {

    }

    void visit(const LLOctreeNode<LLVolumeTriangle, LLVolumeTriangle*>* branch)
    {
        LLVolumeOctreeListener* vl = (LLVolumeOctreeListener*) branch->getListener(0);

        LLVector3 center, size;

        if (branch->isEmpty())
        {
            gRender.diffuseColor3f(1.f,0.2f,0.f);
            center.set(branch->getCenter().getF32ptr());
            size.set(branch->getSize().getF32ptr());
        }
        else
        {
            gRender.diffuseColor3f(0.75f, 1.f, 0.f);
            center.set(vl->mBounds[0].getF32ptr());
            size.set(vl->mBounds[1].getF32ptr());
        }

        drawBoxOutline(center, size);

        for (U32 i = 0; i < 2; i++)
        {
            LLRenderState::DepthScope depth(true, false, i == 1 ? LLRenderState::CompareOp::LessEqual : LLRenderState::CompareOp::Greater);

            if (i == 1)
            {
                gRender.diffuseColor4f(0,1,1,0.5f);
            }
            else
            {
                gRender.diffuseColor4f(0,0.5f,0.5f, 0.25f);
                drawBoxOutline(center, size);
            }

            if (i == 1)
            {
                gRender.flush();
                gRender.setLineWidth(3.f); // <FS> Line width OGL core profile fix by Rye Mutt
            }

            gRender.begin(LLRender::TRIANGLES);
            for (LLOctreeNode<LLVolumeTriangle, LLVolumeTriangle*>::const_element_iter iter = branch->getDataBegin();
                    iter != branch->getDataEnd();
                    ++iter)
            {
                const LLVolumeTriangle* tri = *iter;

                gRender.vertex3fv(tri->mV[0]->getF32ptr());
                gRender.vertex3fv(tri->mV[1]->getF32ptr());
                gRender.vertex3fv(tri->mV[2]->getF32ptr());
            }
            gRender.end();

            if (i == 1)
            {
                gRender.flush();
                gRender.setLineWidth(1.f); // <FS> Line width OGL core profile fix by Rye Mutt
            }
        }
    }
};

void renderOctreeRaycast(const LLVector4a& start, const LLVector4a& end, const LLVolumeOctree* octree)
{
    F32 t = 1.f;
    LLRenderOctreeRaycast render(start, end, &t);
    render.traverse(octree);
}

void renderRaycast(LLDrawable* drawablep)
{
    if (drawablep->getNumFaces())
    {
        LLRenderState::BlendScope blend(true);
        gRender.diffuseColor4f(0,1,1,0.5f);

        LLVOVolume* vobj = drawablep->getVOVolume();
        if (vobj && !vobj->isDead())
        {
            //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            //pushVerts(drawablep->getFace(gDebugRaycastFaceHit), LLVertexBuffer::MAP_VERTEX);
            //glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            LLVolume* volume = vobj->getVolume();

            bool transform = true;
            if (drawablep->isState(LLDrawable::RIGGED))
            {
                volume = vobj->getRiggedVolume();
                transform = false;
            }

            if (volume)
            {
                LLVector3 trans = drawablep->getRegion()->getOriginAgent();

                for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
                {
                    const LLVolumeFace& face = volume->getVolumeFace(i);

                    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                    LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, trans.mV[0], trans.mV[1], trans.mV[2]);
                    LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (F32*) vobj->getRelativeXform().mMatrix);

                    LLVector4a start, end;
                    if (transform)
                    {
                        LLVector3 v_start(gDebugRaycastStart.getF32ptr());
                        LLVector3 v_end(gDebugRaycastEnd.getF32ptr());

                        v_start = vobj->agentPositionToVolume(v_start);
                        v_end = vobj->agentPositionToVolume(v_end);

                        start.load3(v_start.mV);
                        end.load3(v_end.mV);
                    }
                    else
                    {
                        start = gDebugRaycastStart;
                        end = gDebugRaycastEnd;
                    }

                    LLVector4a dir;
                    dir.setSub(end, start);

                    gRender.flush();
                    gRender.setPolygonMode(LLRenderState::PolygonMode::Line);

                    {
                        //render face positions
                        //gRender.diffuseColor4f(0,1,1,0.5f);
                        //LLVertexBuffer::drawElements(LLRender::TRIANGLES, face.mPositions, nullptr, face.mNumIndices, face.mIndices);
                    }

                    if (!volume->isUnique())
                    {
                        if (!face.getOctree())
                        {
                            ((LLVolumeFace*) &face)->createOctree();
                        }

                        renderOctreeRaycast(start, end, face.getOctree());
                    }

                    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
                    gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
                }
            }
        }
        else if (drawablep->isAvatar())
        {
            if (drawablep->getVObj() == gDebugRaycastObject)
            {
                LLRenderState::DepthScope depth(false);
                LLVOAvatar* av = (LLVOAvatar*) drawablep->getVObj().get();
                av->renderCollisionVolumes();
            }
        }

        if (drawablep->getVObj() == gDebugRaycastObject)
        {
            // draw intersection point
            LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
            LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, gRenderModelView);
            LLVector3 translate(gDebugRaycastIntersection.getF32ptr());
            LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, translate.mV[0], translate.mV[1], translate.mV[2]);
            LLCoordFrame orient;
            LLVector4a debug_binormal;

            debug_binormal.setCross3(gDebugRaycastNormal, gDebugRaycastTangent);
            debug_binormal.mul(gDebugRaycastTangent.getF32ptr()[3]);

            LLVector3 normal(gDebugRaycastNormal.getF32ptr());
            LLVector3 binormal(debug_binormal.getF32ptr());

            orient.lookDir(normal, binormal);
            LLMatrix4 rotation;
            orient.getRotMatrixToParent(rotation);
            LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, (float*)rotation.mMatrix);

            gRender.diffuseColor4f(1,0,0,0.5f);
            drawBox(LLVector3(0, 0, 0), LLVector3(0.1f, 0.022f, 0.022f));
            gRender.diffuseColor4f(0,1,0,0.5f);
            drawBox(LLVector3(0, 0, 0), LLVector3(0.021f, 0.1f, 0.021f));
            gRender.diffuseColor4f(0,0,1,0.5f);
            drawBox(LLVector3(0, 0, 0), LLVector3(0.02f, 0.02f, 0.1f));
            LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);

            // draw bounding box of prim
            const LLVector4a* ext = drawablep->getSpatialExtents();

            LLVector4a pos;
            pos.setAdd(ext[0], ext[1]);
            pos.mul(0.5f);
            LLVector4a size;
            size.setSub(ext[1], ext[0]);
            size.mul(0.5f);

            LLRenderState::DepthScope depth(false, true);
            gRender.diffuseColor4f(0,0.5f,0.5f,1);
            drawBoxOutline(pos, size);
        }
    }
}


void renderAvatarCollisionVolumes(LLVOAvatar* avatar)
{
    avatar->renderCollisionVolumes();
}

void renderAvatarBones(LLVOAvatar* avatar)
{
    avatar->renderBones();
}

void renderAgentTarget(LLVOAvatar* avatar)
{
    // render these for self only (why, i don't know)
    if (avatar->isSelf())
    {
        renderCrossHairs(avatar->getPositionAgent(), 0.2f, LLColor4(1, 0, 0, 0.8f));
        renderCrossHairs(avatar->mDrawable->getPositionAgent(), 0.2f, LLColor4(0, 1, 0, 0.8f));
        renderCrossHairs(avatar->mRoot->getWorldPosition(), 0.2f, LLColor4(1, 1, 1, 0.8f));
        renderCrossHairs(avatar->mPelvisp->getWorldPosition(), 0.2f, LLColor4(0, 0, 1, 0.8f));
    }
}

static void setTextureAreaDebugText(LLDrawable* drawablep)
{
    LLVOVolume* vobjp = drawablep->getVOVolume();

    if (vobjp)
    {
        if (drawablep->mDistanceWRTCamera < 32.f)
        {
            std::ostringstream str;

            //for (S32 i = 0; i < vobjp->getNumTEs(); ++i)
            S32 i = 0;
            {
                if (i < drawablep->getNumFaces())
                {
                    LLFace* facep = drawablep->getFace(i);

                    if (facep)
                    {
                        LLViewerTexture* imagep = facep->getTexture();

                        if (imagep)
                        {
                            str << llformat("D - %.2f", sqrtf(imagep->getMaxVirtualSize()));
                        }

                        imagep = vobjp->getTENormalMap(i);

                        if (imagep && imagep != LLViewerFetchedTexture::sDefaultImagep)
                        {
                            str << llformat("\nN - %.2f", sqrtf(imagep->getMaxVirtualSize()));
                        }

                        imagep = vobjp->getTESpecularMap(i);

                        if (imagep && imagep != LLViewerFetchedTexture::sDefaultImagep)
                        {
                            str << llformat("\nS - %.2f", sqrtf(imagep->getMaxVirtualSize()));
                        }

                        str << "\n\n";
                    }

                    vobjp->setDebugText(str.str());
                }
            }
        }
        else
        {
            vobjp->setDebugText(".");
        }
    }
}

class LLOctreeRenderNonOccluded : public OctreeTraveler
{
public:
    LLCamera* mCamera;
    LLOctreeRenderNonOccluded(LLCamera* camera): mCamera(camera) {}

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        const LLVector4a* bounds = group->getBounds();
        if (!mCamera || mCamera->AABBInFrustumNoFarClip(bounds[0], bounds[1]))
        {
            node->accept(this);

            for (U32 i = 0; i < node->getChildCount(); i++)
            {
                traverse(node->getChild(i));
            }

            //draw tight fit bounding boxes for spatial group
            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCTREE))
            {
                group->rebuildGeom();
                group->rebuildMesh();

                renderOctree(group);
            }
        }
    }

    virtual void visit(const OctreeNode* branch)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) branch->getListener(0);
        const LLVector4a* bounds = group->getBounds();
        if (group->hasState(LLSpatialGroup::GEOM_DIRTY) || (mCamera && !mCamera->AABBInFrustumNoFarClip(bounds[0], bounds[1])))
        {
            return;
        }

        group->rebuildGeom();
        group->rebuildMesh();

        if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_BBOXES))
        {
            if (!group->isEmpty())
            {
                gRender.diffuseColor3f(0,0,1);
                const LLVector4a* obj_bounds = group->getObjectBounds();
                drawBoxOutline(obj_bounds[0], obj_bounds[1]);
            }
        }

        for (OctreeNode::const_element_iter i = branch->getDataBegin(); i != branch->getDataEnd(); ++i)
        {
            LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
            if(!drawable || drawable->isDead())
        {
                continue;
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_BBOXES))
            {
                renderBoundingBox(drawable);
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_NORMALS))
            {
                renderNormals(drawable);
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
            {
                setTextureAreaDebugText(drawable);
            }

            /*if (drawable->getVOVolume() && gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_PRIORITY))
            {
                renderTexturePriority(drawable);
            }*/

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_POINTS))
            {
                renderPoints(drawable);
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_LIGHTS))
            {
                renderLights(drawable);
            }

            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST))
            {
                renderRaycast(drawable);
            }
            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_UPDATE_TYPE))
            {
                renderUpdateType(drawable);
            }
            if(gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXEL_DENSITY))
            {
                renderTexelDensity(drawable);
            }

            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(drawable->getVObj().get());

            if (avatar && gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_AVATAR_VOLUME))
            {
                renderAvatarCollisionVolumes(avatar);
            }

            if (avatar && gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_AVATAR_JOINTS))
            {
                renderAvatarBones(avatar);
            }

            if (avatar && gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_AGENT_TARGET))
            {
                renderAgentTarget(avatar);
            }

#if 0
            if (gDebugRender)
            {
                for (U32 i = 0; i < drawable->getNumFaces(); ++i)
                {
                    LLFace* facep = drawable->getFace(i);
                    if (facep)
                    {
                        U8 index = facep->getTextureIndex();
                        if (facep->mDrawInfo)
                        {
                            if (index < FACE_DO_NOT_BATCH_TEXTURES)
                            {
                                if (facep->mDrawInfo->mTextureList.size() <= index)
                                {
                                    LL_ERRS() << "Face texture index out of bounds." << LL_ENDL;
                                }
                                else if (facep->mDrawInfo->mTextureList[index] != facep->getTexture())
                                {
                                    LL_ERRS() << "Face texture index incorrect." << LL_ENDL;
                                }
                            }
                        }
                    }
                }
            }
#endif
        }

        // (K6 B1: Batch Size and Texture Animation are drawn from the scene table once a frame: ll_render_table_debug)
    }
};

class LLOctreeRenderXRay : public OctreeTraveler
{
public:
    LLCamera* mCamera;
    LLOctreeRenderXRay(LLCamera* camera): mCamera(camera) {}

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        const LLVector4a* bounds = group->getBounds();
        if (!mCamera || mCamera->AABBInFrustumNoFarClip(bounds[0], bounds[1]))
        {
            node->accept(this);

            for (U32 i = 0; i < node->getChildCount(); i++)
            {
                traverse(node->getChild(i));
            }

            //render visibility wireframe
            if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCCLUSION))
            {
                group->rebuildGeom();
                group->rebuildMesh();

                gRender.flush();
                LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
                gRenderLastMatrix = NULL;
                LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, gRenderModelView);
                renderXRay(group, mCamera);
                gRenderLastMatrix = NULL;
                LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
            }
        }
    }

    virtual void visit(const OctreeNode* node) {}

};

class LLOctreeRenderPhysicsShapes : public OctreeTraveler
{
public:
    LLCamera* mCamera;
    bool mWireframe;

    LLOctreeRenderPhysicsShapes(LLCamera* camera, bool wireframe): mCamera(camera), mWireframe(wireframe) {}

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        const LLVector4a* bounds = group->getBounds();
        if (!mCamera || mCamera->AABBInFrustumNoFarClip(bounds[0], bounds[1]))
        {
            node->accept(this);

            for (U32 i = 0; i < node->getChildCount(); i++)
            {
                traverse(node->getChild(i));
            }

            group->rebuildGeom();
            group->rebuildMesh();

            renderPhysicsShapes(group, mWireframe);
        }
    }

    virtual void visit(const OctreeNode* branch)
    {

    }
};

class LLOctreePushBBoxVerts : public OctreeTraveler
{
public:
    LLCamera* mCamera;
    LLOctreePushBBoxVerts(LLCamera* camera): mCamera(camera) {}

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        const LLVector4a* bounds = group->getBounds();
        if (!mCamera || mCamera->AABBInFrustum(bounds[0], bounds[1]))
        {
            node->accept(this);

            for (U32 i = 0; i < node->getChildCount(); i++)
            {
                traverse(node->getChild(i));
            }
        }
    }

    virtual void visit(const OctreeNode* branch)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) branch->getListener(0);

        const LLVector4a* bounds = group->getBounds();
        if (group->hasState(LLSpatialGroup::GEOM_DIRTY) || (mCamera && !mCamera->AABBInFrustumNoFarClip(bounds[0], bounds[1])))
        {
            return;
        }

        for (OctreeNode::const_element_iter i = branch->getDataBegin(); i != branch->getDataEnd(); ++i)
        {
            LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
            if(!drawable)
        {
                continue;
            }
            renderBoundingBox(drawable, false);
        }
    }
};

void LLSpatialPartition::renderIntersectingBBoxes(LLCamera* camera)
{
    LLOctreePushBBoxVerts pusher(camera);
    pusher.traverse(mOctree);
}

class LLOctreeStateCheck : public OctreeTraveler
{
public:
    U32 mInheritedMask[LLViewerCamera::NUM_CAMERAS];

    LLOctreeStateCheck()
    {
        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            mInheritedMask[i] = 0;
        }
    }

    // <FS> K6: a node without a group, or a group that has no row of its own any more (a dead one in the octree), is skipped: reading its row crashed the debug views (reading address FFFFFFFFFFFFFFFF in visit)
    static bool usable(const LLSpatialGroup* group)
    {
        return group && group->ownsRow();
    }

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        node->accept(this);


        U32 temp[LLViewerCamera::NUM_CAMERAS];

        const bool have_row = usable(group);
        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            temp[i] = mInheritedMask[i];
            if (have_row)
            {
                mInheritedMask[i] |= group->row().occlusion[i] & LLSpatialGroup::OCCLUDED;
            }
        }

        for (U32 i = 0; i < node->getChildCount(); i++)
        {
            traverse(node->getChild(i));
        }

        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            mInheritedMask[i] = temp[i];
        }
    }


    virtual void visit(const OctreeNode* state)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) state->getListener(0);
        if (!usable(group))
        {
            return;
        }

        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            if (mInheritedMask[i] && !(group->row().occlusion[i] & mInheritedMask[i]))
            {
                // <FS> K6: a debug view must not end the viewer: the failed check is logged (the first ones), not a fatal error
                static U32 told = 0;
                if (told++ < 5)
                {
                    LL_WARNS("SpatialDebug") << "Spatial group failed inherited mask test." << LL_ENDL;
                }
            }
        }

        if (group->hasState(LLSpatialGroup::DIRTY))
        {
            assert_parent_state(group, LLSpatialGroup::DIRTY);
        }
    }

    void assert_parent_state(LLSpatialGroup* group, U32 state)
    {
        LLSpatialGroup* parent = group->getParent();
        while (parent && usable(parent))
        {
            if (!parent->hasState(state))
            {
                static U32 told = 0;
                if (told++ < 5)
                {
                    LL_WARNS("SpatialDebug") << "Spatial group failed parent state check." << LL_ENDL;
                }
            }
            parent = parent->getParent();
        }
    }
};


void LLSpatialPartition::renderPhysicsShapes(bool wireframe)
{
    LLSpatialBridge* bridge = asBridge();
    LLCamera* camera = LLViewerCamera::getInstance();

    if (bridge)
    {
        camera = NULL;
    }

    gRender.flush();
    gRender.getTexUnit(0)->bindWhite();
    LLOctreeRenderPhysicsShapes render_physics(camera, wireframe);
    render_physics.traverse(mOctree);
    gRender.flush();
}

void LLSpatialPartition::renderDebug()
{
    if (!gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCTREE |
                                      LLPipeline::RENDER_DEBUG_OCCLUSION |
                                      LLPipeline::RENDER_DEBUG_LIGHTS |
                                      LLPipeline::RENDER_DEBUG_BATCH_SIZE |
                                      LLPipeline::RENDER_DEBUG_UPDATE_TYPE |
                                      LLPipeline::RENDER_DEBUG_BBOXES |
                                      LLPipeline::RENDER_DEBUG_NORMALS |
                                      LLPipeline::RENDER_DEBUG_POINTS |
                                      LLPipeline::RENDER_DEBUG_TEXTURE_AREA |
                                      LLPipeline::RENDER_DEBUG_TEXTURE_ANIM |
                                      LLPipeline::RENDER_DEBUG_RAYCAST |
                                      LLPipeline::RENDER_DEBUG_AVATAR_VOLUME |
                                      LLPipeline::RENDER_DEBUG_AVATAR_JOINTS |
                                      LLPipeline::RENDER_DEBUG_AGENT_TARGET |
                                      LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA |
                                      LLPipeline::RENDER_DEBUG_TEXEL_DENSITY))
    {
        return;
    }

    gDebugProgram.bind();

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_PRIORITY))
    {
        //sLastMaxTexPriority = lerp(sLastMaxTexPriority, sCurMaxTexPriority, gFrameIntervalSeconds);
        sLastMaxTexPriority = (F32) LLViewerCamera::getInstance()->getScreenPixelArea();
        sCurMaxTexPriority = 0.f;
    }

    LLRenderState::CullScope cullface(false);
    LLRenderState::BlendScope blend(true);
    gRender.setSceneBlendType(LLRender::BT_ALPHA);
    gRender.getTexUnit(0)->bindWhite();
    gPipeline.disableLights();

    LLSpatialBridge* bridge = asBridge();
    LLCamera* camera = LLViewerCamera::getInstance();

    if (bridge)
    {
        camera = NULL;
    }

    LLOctreeStateCheck checker;
    checker.traverse(mOctree);

    LLOctreeRenderNonOccluded render_debug(camera);
    render_debug.traverse(mOctree);

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCCLUSION))
    {
        {
            LLRenderState::CullScope cull(true);

            LLRenderState::BlendScope blend(true);
            LLRenderState::DepthScope depth_under(true, false, LLRenderState::CompareOp::Greater);
            gRender.setPolygonMode(LLRenderState::PolygonMode::Line);
            gRender.diffuseColor4f(0.5f, 0.0f, 0, 0.25f);

            // F3.2: LLGLEnable offset(GL_POLYGON_OFFSET_LINE) had no effect on the render state block (never mapped); dropped
            gRender.setPolygonOffset(-1.f, -1.f);

            LLOctreeRenderXRay xray(camera);
            xray.traverse(mOctree);

            gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);
        }
    }
    gDebugProgram.unbind();
}

void LLSpatialGroup::drawObjectBox(LLColor4 col)
{
    gRender.diffuseColor4fv(col.mV);
    LLVector4a size;
    size = row().objectBounds[1];
    size.mul(1.01f);
    size.add(LLVector4a(0.001f));
    drawBox(row().objectBounds[0], size);
}

bool LLSpatialPartition::isHUDPartition()
{
    return mPartitionType == LLViewerRegion::PARTITION_HUD ;
}

bool LLSpatialPartition::isVisible(const LLVector3& v)
{
    if (!LLViewerCamera::getInstance()->sphereInFrustum(v, 4.0f))
    {
        return false;
    }

    return true;
}

// <FS:ND> Class to watch for any octree changes while iterating. Will catch child insertion/removal as well as data insertion/removal.
// Template so it can be used for than LLOctreeNode< LLDrawable > if needed

template< typename T, typename T_PTR > class ndOctreeListener: public LLOctreeListener< T, T_PTR >
{
    typedef LLOctreeNode< T, T_PTR > tNode;
    typedef std::vector< LLPointer< LLTreeListener< T > > > tListener;

    tNode *mNode;
    bool mNodeIsDead;
    bool mNodeChildrenChanged;
    bool mNodeDataChanged;

    virtual void handleInsertion(const LLTreeNode<T>* node, T* data)
    { mNodeDataChanged = true; }

    virtual void handleRemoval(const LLTreeNode<T>* node, T* data)
    { mNodeDataChanged = true; }

    virtual void handleDestruction(const LLTreeNode<T>* node)
    { mNodeIsDead = true; }

    virtual void handleStateChange(const LLTreeNode<T>* node)
    { }

    virtual void handleChildAddition(const tNode* parent, tNode* child)
    { mNodeChildrenChanged = true; }

    virtual void handleChildRemoval(const tNode* parent, const tNode* child)
    { mNodeChildrenChanged = true; }

public:
    ndOctreeListener( OctreeNode *aNode )
        : mNode( aNode )
        , mNodeIsDead( false )
        , mNodeChildrenChanged( false )
        , mNodeDataChanged( false )
    {
        if( mNode )
            mNode->addListener( this );
        else
            mNodeIsDead = true;
    }

    ~ndOctreeListener()
    { removeObserver(); }

    bool getNodeIsDead() const
    { return mNodeIsDead; }

    bool getNodeChildrenChanged() const
    { return mNodeChildrenChanged; }

    bool getNodeDataChanged() const
    { return mNodeDataChanged; }

    // FS:ND This is kind of hackery, poking into the internals of mNode like that. But there's no removeListener function.
    // To keep change locality for merges I decided to put the implemention here.
    // This is what you get for making your member public/protected.
    void removeObserver()
    {
        if( mNode && !getNodeIsDead() )
        {
            for( typename tListener::iterator itr = mNode->mListeners.begin(); itr != mNode->mListeners.end(); ++itr )
            {
                if( (*itr).get() == this )
                {
                    mNode->mListeners.erase( itr );
                    break;
                }
            }
        }
        mNode = 0;
        mNodeIsDead = true;
    }
};

typedef ndOctreeListener< LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry> > ndDrawableOctreeListener;
typedef LLPointer< ndDrawableOctreeListener > ndDrawableOctreeListenerPtr;

// </FS:ND>

LL_ALIGN_PREFIX(16)
class LLOctreeIntersect : public LLOctreeTraveler<LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry>>
{
public:
    LL_ALIGN_16(LLVector4a mStart);
    LL_ALIGN_16(LLVector4a mEnd);

    S32       *mFaceHit;
    LLVector4a *mIntersection;
    LLVector2 *mTexCoord;
    LLVector4a *mNormal;
    LLVector4a *mTangent;
    LLDrawable* mHit;
    bool mPickTransparent;
    bool mPickRigged;
    bool mPickUnselectable;
    bool mPickReflectionProbe;

    LLOctreeIntersect(const LLVector4a& start, const LLVector4a& end, bool pick_transparent, bool pick_rigged, bool pick_unselectable, bool pick_reflection_probe,
                      S32* face_hit, LLVector4a* intersection, LLVector2* tex_coord, LLVector4a* normal, LLVector4a* tangent)
        : mStart(start),
          mEnd(end),
          mFaceHit(face_hit),
          mIntersection(intersection),
          mTexCoord(tex_coord),
          mNormal(normal),
          mTangent(tangent),
          mHit(NULL),
          mPickTransparent(pick_transparent),
          mPickRigged(pick_rigged),
          mPickUnselectable(pick_unselectable),
          mPickReflectionProbe(pick_reflection_probe)
    {
    }

    virtual void visit(const OctreeNode* branch)
    {
        // <FS:ND> Make sure we catch any changes to this node while we iterate over it
        ndDrawableOctreeListenerPtr nodeObserver = new ndDrawableOctreeListener ( const_cast<OctreeNode*>(branch) );

        // for (OctreeNode::const_element_iter i = branch->getDataBegin(); i != branch->getDataEnd(); ++i)
        for (OctreeNode::const_element_iter i = branch->getDataBegin(); i != branch->getDataEnd(); )
        // </FS:ND>
        {
            check(*i);

            // <FS:ND> Check for any change that happened during check, it is possible the tree changes due to calling it.
            // If it does, we need to restart again as pointers might be invalidated.

            if( !nodeObserver->getNodeDataChanged() )
                ++i;
            else
            {
                i = branch->getDataBegin();
                LL_WARNS() << "Warning, resetting data iterator to branch->getDataBegin due to tree change." << LL_ENDL;
            }

            // FS:ND Can this really happen? I seriously hope not.
            if( nodeObserver->getNodeIsDead() )
            {
                LL_WARNS() << "Warning, node died. Exiting iteration" << LL_ENDL;
                break;
            }

            // </FS:ND>
        }

        nodeObserver->removeObserver();
    }

    virtual LLDrawable* check(const OctreeNode* node)
    {
        node->accept(this);

        // <FS:ND> Make sure we catch any changes to this node while we iterate over it
        ndDrawableOctreeListenerPtr nodeObserver = new ndDrawableOctreeListener ( const_cast<OctreeNode*>(node) );

        // for (U32 i = 0; i < node->getChildCount(); i++)
        for (U32 i = 0; i < node->getChildCount(); )
        // </FS:ND>
        {
            const OctreeNode* child = node->getChild(i);

            LLVector3 res;

            LLSpatialGroup* group = (LLSpatialGroup*) child->getListener(0);

            LLVector4a size;
            LLVector4a center;

            const LLVector4a* bounds = group->getBounds();
            size = bounds[1];
            center = bounds[0];

            LLVector4a local_start = mStart;
            LLVector4a local_end   = mEnd;

            if (group->getSpatialPartition()->isBridge())
            {
                LLMatrix4 local_matrix = group->getSpatialPartition()->asBridge()->mDrawable->getRenderMatrix();
                local_matrix.invert();

                LLMatrix4a local_matrix4a;
                local_matrix4a.loadu(local_matrix);

                local_matrix4a.affineTransform(mStart, local_start);
                local_matrix4a.affineTransform(mEnd, local_end);
            }

            if (LLLineSegmentBoxIntersect(local_start, local_end, center, size))
            {
                check(child);
            }

            // <FS:ND> Check for any change that happened during check, it is possible the tree changes due to calling it.
            // If it does, do we need to restart again as pointers might be invalidated? Child insertion/removal happens it seems, but restarting
            // iteration leads into endless recursion.

            if( !nodeObserver->getNodeChildrenChanged() )
                ++i;
            else
            {
                ++i;
                LL_WARNS() << "Warning, child nodes changed during tree iteration." << LL_ENDL;
            }

            // FS:ND Can this really happen? I seriously hope not.
            if( nodeObserver->getNodeIsDead() )
            {
                LL_WARNS() << "Warning, node died. Exiting iteration" << LL_ENDL;
                break;
            }

            // </FS:ND>
        }

        nodeObserver->removeObserver();
        return mHit;
    }

    virtual bool check(LLViewerOctreeEntry* entry)
    {
        LLDrawable* drawable = (LLDrawable*)entry->getDrawable();

        if (!drawable || !gPipeline.hasRenderType(drawable->getRenderType()) || !drawable->isVisible())
        {
            return false;
        }

        if (drawable->isSpatialBridge())
        {
            LLSpatialPartition *part = drawable->asPartition();
            LLSpatialBridge* bridge = part->asBridge();
            if (bridge && gPipeline.hasRenderType(bridge->mDrawableType))
            {
                check(part->mOctree);
            }
        }
        else
        {
            LLViewerObject* vobj = drawable->getVObj();

            if (vobj &&
                (!vobj->isReflectionProbe() || mPickReflectionProbe))
            {
                if (vobj->getClickAction() == CLICK_ACTION_IGNORE && !LLFloater::isVisible(gFloaterTools))
                {
                    return false;
                }

                LLVector4a intersection;
                bool skip_check = false;
                if (vobj->isAvatar())
                {
                    LLVOAvatar* avatar = (LLVOAvatar*) vobj;
                    if ((mPickRigged) || ((avatar->isSelf()) && (LLFloater::isVisible(gFloaterTools))))
                    {
                        LLViewerObject* hit = avatar->lineSegmentIntersectRiggedAttachments(mStart, mEnd, -1, mPickTransparent, mPickRigged, mPickUnselectable, mFaceHit, &intersection, mTexCoord, mNormal, mTangent);
                        if (hit)
                        {
                            mEnd = intersection;
                            if (mIntersection)
                            {
                                *mIntersection = intersection;
                            }

                            mHit = hit->mDrawable;
                            skip_check = true;
                        }

                    }
                }

                if (!skip_check && vobj->lineSegmentIntersect(mStart, mEnd, -1,
                    (mPickReflectionProbe && vobj->isReflectionProbe()) ? true : mPickTransparent, // always pick transparent when picking selection probe
                    mPickRigged, mPickUnselectable, mFaceHit, &intersection, mTexCoord, mNormal, mTangent))
                {
                    mEnd = intersection;  // shorten ray so we only find CLOSER hits
                    if (mIntersection)
                    {
                        *mIntersection = intersection;
                    }

                    mHit = vobj->mDrawable;
                }
            }
        }

        return false;
    }
} LL_ALIGN_POSTFIX(16);

LLDrawable* LLSpatialPartition::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                                     bool pick_transparent,
                                                     bool pick_rigged,
                                                     bool pick_unselectable,
                                                     bool pick_reflection_probe,
                                                     S32* face_hit,                   // return the face hit
                                                     LLVector4a* intersection,         // return the intersection point
                                                     LLVector2* tex_coord,            // return the texture coordinates of the intersection point
                                                     LLVector4a* normal,               // return the surface normal at the intersection point
                                                     LLVector4a* tangent            // return the surface tangent at the intersection point
    )

{
    LLOctreeIntersect intersect(start, end, pick_transparent, pick_rigged, pick_unselectable, pick_reflection_probe, face_hit, intersection, tex_coord, normal, tangent);
    LLDrawable* drawable = intersect.check(mOctree);

    return drawable;
}

LLDrawable* LLSpatialGroup::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
    bool pick_transparent,
    bool pick_rigged,
    bool pick_unselectable,
    bool pick_reflection_probe,
    S32* face_hit,                   // return the face hit
    LLVector4a* intersection,         // return the intersection point
    LLVector2* tex_coord,            // return the texture coordinates of the intersection point
    LLVector4a* normal,               // return the surface normal at the intersection point
    LLVector4a* tangent         // return the surface tangent at the intersection point
)

{
    LLOctreeIntersect intersect(start, end, pick_transparent, pick_rigged, pick_unselectable, pick_reflection_probe, face_hit, intersection, tex_coord, normal, tangent);
    LLDrawable* drawable = intersect.check(getOctreeNode());

    return drawable;
}

LLDrawInfo::LLDrawInfo(U16 start, U16 end, U32 count, U32 offset,
                       LLViewerTexture* texture, LLVertexBuffer* buffer,
                       bool fullbright, U8 bump)
:   mVertexBuffer(buffer),
    mTexture(texture),
    mStart(start),
    mEnd(end),
    mCount(count),
    mOffset(offset),
    mFullbright(fullbright),
    mBump(bump),
    mBlendFuncSrc(LLRender::BF_SOURCE_ALPHA),
    mBlendFuncDst(LLRender::BF_ONE_MINUS_SOURCE_ALPHA),
    mHasGlow(false),
    mEnvIntensity(0.0f),
    mAlphaMaskCutoff(0.5f)
{
    mVertexBuffer->validateRange(mStart, mEnd, mCount, mOffset);
}

LLDrawInfo::~LLDrawInfo()
{
    if (gDebugRender)
    {
        gPipeline.checkReferences(this);
    }
}

LLColor4U LLDrawInfo::getDebugColor() const
{
    LLColor4U color;

    LLCRC hash;
    hash.update((U8*)this + sizeof(S32), sizeof(LLDrawInfo) - sizeof(S32));

    *((U32*) color.mV) = hash.getCRC();

    color.mV[3] = 200;

    return color;
}

void LLDrawInfo::validate()
{
    mVertexBuffer->validateRange(mStart, mEnd, mCount, mOffset);
}

U64 LLDrawInfo::getSkinHash()
{
    return mSkinInfo ? mSkinInfo->mHash : 0;
}

LLCullResult::LLCullResult()
{
    mVisibleGroupsAllocated = 0;
    mAlphaGroupsAllocated = 0;
    mRiggedAlphaGroupsAllocated = 0;
    mOcclusionGroupsAllocated = 0;
    mDrawableGroupsAllocated = 0;
    mVisibleListAllocated = 0;
    mVisibleBridgeAllocated = 0;

    mVisibleGroups.clear();
    mVisibleGroups.push_back(NULL);
    mVisibleGroupsEnd = &mVisibleGroups[0];
    mAlphaGroups.clear();
    mAlphaGroups.push_back(NULL);
    mAlphaGroupsEnd = &mAlphaGroups[0];
    mRiggedAlphaGroups.clear();
    mRiggedAlphaGroups.push_back(NULL);
    mRiggedAlphaGroupsEnd = &mRiggedAlphaGroups[0];
    mOcclusionGroups.clear();
    mOcclusionGroups.push_back(NULL);
    mOcclusionGroupsEnd = &mOcclusionGroups[0];
    mDrawableGroups.clear();
    mDrawableGroups.push_back(NULL);
    mDrawableGroupsEnd = &mDrawableGroups[0];
    mVisibleList.clear();
    mVisibleList.push_back(NULL);
    mVisibleListEnd = &mVisibleList[0];
    mVisibleBridge.clear();
    mVisibleBridge.push_back(NULL);
    mVisibleBridgeEnd = &mVisibleBridge[0];

    for (U32 i = 0; i < LLRenderPass::NUM_RENDER_TYPES; i++)
    {
        mRenderMap[i].clear();
        mRenderMap[i].push_back(NULL);
        mRenderMapEnd[i] = &mRenderMap[i][0];
        mRenderMapAllocated[i] = 0;
    }

    memset( mRenderMapSize, 0, sizeof(mRenderMapSize) ); // <FS:ND> Initialize with 0.

    clear();
}

template <class T, class V>
void LLCullResult::pushBack(T& head, U32& count, V* val)
{
    head[count] = val;
    head.push_back(NULL);
    count++;
}

// <FS> the lists a cull fills (groups, drawables, bridges), without the render maps and the scene table fields that a cull never touches: the results of the pieces of a split cull (LL_VK_SC_SPLIT)
void LLCullResult::clearLists()
{
    mVisibleGroupsSize = 0;
    mVisibleGroupsEnd = &mVisibleGroups[0];
    mAlphaGroupsSize = 0;
    mAlphaGroupsEnd = &mAlphaGroups[0];
    mRiggedAlphaGroupsSize = 0;
    mRiggedAlphaGroupsEnd = &mRiggedAlphaGroups[0];
    mOcclusionGroupsSize = 0;
    mOcclusionGroupsEnd = &mOcclusionGroups[0];
    mDrawableGroupsSize = 0;
    mDrawableGroupsEnd = &mDrawableGroups[0];
    mVisibleListSize = 0;
    mVisibleListEnd = &mVisibleList[0];
    mVisibleBridgeSize = 0;
    mVisibleBridgeEnd = &mVisibleBridge[0];
    mLodGroups.clear();
}

void LLCullResult::clear()
{
    mAlphaListsSkipped = false;
    if (mLazyTypes)
    {
        for (U32 t = 0; t < LLRenderPass::NUM_RENDER_TYPES; ++t) { mLazyGroups[t].clear(); mLazyDone[t] = false; }
        mLazyTypes = false;
    }
    mVisibleGroupsSize = 0;
    mVisibleGroupsEnd = &mVisibleGroups[0];

    mAlphaGroupsSize = 0;
    mAlphaGroupsEnd = &mAlphaGroups[0];

    mRiggedAlphaGroupsSize = 0;
    mRiggedAlphaGroupsEnd = &mRiggedAlphaGroups[0];

    mOcclusionGroupsSize = 0;
    mOcclusionGroupsEnd = &mOcclusionGroups[0];

    mDrawableGroupsSize = 0;
    mDrawableGroupsEnd = &mDrawableGroups[0];

    mVisibleListSize = 0;
    mVisibleListEnd = &mVisibleList[0];

    mVisibleBridgeSize = 0;
    mVisibleBridgeEnd = &mVisibleBridge[0];

    mSceneGroups.clear();
    mAlphaFragGroups.clear();
    mRigSources.clear();
    mRigFromFragments = false;
    mRigAlphaOn = false;
    mAlphaResSkipped = false;
    mLodGroups.clear();
    mGeoHandles.clear();
    mGeoElided = false;

    for (U32 i = 0; i < LLRenderPass::NUM_RENDER_TYPES; i++)
    {
        drawinfo_list_t& render_map = mRenderMap[i];
        U32 render_map_size = llmin((U32)render_map.size(), mRenderMapSize[i]);
        for (U32 j = 0; j < render_map_size; j++)
        {
            render_map[j] = 0;
        }
        mRenderMapSize[i] = 0;
        mRenderMapEnd[i] = &render_map.front();
    }
}

void LLCullResult::clearRenderLists()
{
    mAlphaListsSkipped = false;
    if (mLazyTypes)
    {
        for (U32 t = 0; t < LLRenderPass::NUM_RENDER_TYPES; ++t) { mLazyGroups[t].clear(); mLazyDone[t] = false; }
        mLazyTypes = false;
    }
    mAlphaGroupsSize = 0;
    mAlphaGroupsEnd = &mAlphaGroups[0];
    mRiggedAlphaGroupsSize = 0;
    mRiggedAlphaGroupsEnd = &mRiggedAlphaGroups[0];
    mSceneGroups.clear();
    mAlphaFragGroups.clear();
    mRigSources.clear();
    mRigFromFragments = false;
    mRigAlphaOn = false;
    mAlphaResSkipped = false;
    mGeoHandles.clear();
    mGeoElided = false;
    for (U32 i = 0; i < LLRenderPass::NUM_RENDER_TYPES; i++)
    {
        drawinfo_list_t& render_map = mRenderMap[i];
        U32 render_map_size = llmin((U32)render_map.size(), mRenderMapSize[i]);
        for (U32 j = 0; j < render_map_size; j++)
        {
            render_map[j] = 0;
        }
        mRenderMapSize[i] = 0;
        mRenderMapEnd[i] = &render_map.front();
    }
}

LLCullResult::sg_iterator LLCullResult::beginVisibleGroups()
{
    return &mVisibleGroups[0];
}

LLCullResult::sg_iterator LLCullResult::endVisibleGroups()
{
    return mVisibleGroupsEnd;
}

LLCullResult::sg_iterator LLCullResult::beginAlphaGroups()
{
    return &mAlphaGroups[0];
}

LLCullResult::sg_iterator LLCullResult::endAlphaGroups()
{
    return mAlphaGroupsEnd;
}

LLCullResult::sg_iterator LLCullResult::beginRiggedAlphaGroups()
{
    return &mRiggedAlphaGroups[0];
}

LLCullResult::sg_iterator LLCullResult::endRiggedAlphaGroups()
{
    return mRiggedAlphaGroupsEnd;
}

LLCullResult::sg_iterator LLCullResult::beginOcclusionGroups()
{
    return &mOcclusionGroups[0];
}

LLCullResult::sg_iterator LLCullResult::endOcclusionGroups()
{
    return mOcclusionGroupsEnd;
}

LLCullResult::sg_iterator LLCullResult::beginDrawableGroups()
{
    return &mDrawableGroups[0];
}

LLCullResult::sg_iterator LLCullResult::endDrawableGroups()
{
    return mDrawableGroupsEnd;
}

LLCullResult::drawable_iterator LLCullResult::beginVisibleList()
{
    return &mVisibleList[0];
}

LLCullResult::drawable_iterator LLCullResult::endVisibleList()
{
    return mVisibleListEnd;
}

LLCullResult::bridge_iterator LLCullResult::beginVisibleBridge()
{
    return &mVisibleBridge[0];
}

LLCullResult::bridge_iterator LLCullResult::endVisibleBridge()
{
    return mVisibleBridgeEnd;
}

void LLCullResult::materializeLazy(U32 type)
{
    mLazyDone[type] = true;
    for (LLSpatialGroup* group : mLazyGroups[type])
    {
        if (group->isDead())
        {
            continue;
        }
        auto found = group->mDrawMap.find(type);
        if (found == group->mDrawMap.end())
        {
            continue;
        }
        for (const LLPointer<LLDrawInfo>& info : found->second)
        {
            if (info->mVertexBuffer.isNull())
            {
                continue;       // (a draw info of a group that is being rebuilt: no buffer yet)
            }
            pushDrawInfo(type, info.get());
        }
    }
}

LLCullResult::drawinfo_iterator LLCullResult::beginRenderMap(U32 type)
{
    if (mLazyTypes && !mLazyDone[type] && !mLazyGroups[type].empty())
    {
        materializeLazy(type);
    }
    if (mAlphaListsSkipped && (type == LLRenderPass::PASS_ALPHA || type == LLRenderPass::PASS_ALPHA_RIGGED))
    {
        static std::atomic<int> logged{ 0 };
        if (logged.fetch_add(1) < 6)
        {
            LL_WARNS("RMCensus") << "a reader asks the main view's render map list of pass type " << type << ", which postSort did not fill (LL_VK_RM_ALPHA=0 fills it)" << LL_ENDL;
        }
    }
    return &mRenderMap[type][0];
}

LLCullResult::drawinfo_iterator LLCullResult::endRenderMap(U32 type)
{
    if (mLazyTypes && !mLazyDone[type] && !mLazyGroups[type].empty())
    {
        materializeLazy(type);
    }
    return mRenderMapEnd[type];
}

void LLCullResult::pushVisibleGroup(LLSpatialGroup* group)
{
    if (mVisibleGroupsSize < mVisibleGroupsAllocated)
    {
        mVisibleGroups[mVisibleGroupsSize] = group;
    }
    else
    {
        pushBack(mVisibleGroups, mVisibleGroupsAllocated, group);
    }
    ++mVisibleGroupsSize;
    mVisibleGroupsEnd = &mVisibleGroups[mVisibleGroupsSize];
}

void LLCullResult::pushAlphaGroup(LLSpatialGroup* group)
{
    if (mAlphaGroupsSize < mAlphaGroupsAllocated)
    {
        mAlphaGroups[mAlphaGroupsSize] = group;
    }
    else
    {
        pushBack(mAlphaGroups, mAlphaGroupsAllocated, group);
    }
    ++mAlphaGroupsSize;
    mAlphaGroupsEnd = &mAlphaGroups[mAlphaGroupsSize];
}

void LLCullResult::pushRiggedAlphaGroup(LLSpatialGroup* group)
{
    if (mRiggedAlphaGroupsSize < mRiggedAlphaGroupsAllocated)
    {
        mRiggedAlphaGroups[mRiggedAlphaGroupsSize] = group;
    }
    else
    {
        pushBack(mRiggedAlphaGroups, mRiggedAlphaGroupsAllocated, group);
    }
    ++mRiggedAlphaGroupsSize;
    mRiggedAlphaGroupsEnd = &mRiggedAlphaGroups[mRiggedAlphaGroupsSize];
}

void LLCullResult::pushOcclusionGroup(LLSpatialGroup* group)
{
    if (mOcclusionGroupsSize < mOcclusionGroupsAllocated)
    {
        mOcclusionGroups[mOcclusionGroupsSize] = group;
    }
    else
    {
        pushBack(mOcclusionGroups, mOcclusionGroupsAllocated, group);
    }
    ++mOcclusionGroupsSize;
    mOcclusionGroupsEnd = &mOcclusionGroups[mOcclusionGroupsSize];
}

void LLCullResult::pushDrawableGroup(LLSpatialGroup* group)
{
#if LL_DEBUG_CULL_RESULT
    // group must NOT be in the drawble groups list already
    llassert(std::find(&mDrawableGroups[0], mDrawableGroupsEnd, group) == mDrawableGroupsEnd);
#endif
    if (mDrawableGroupsSize < mDrawableGroupsAllocated)
    {
        mDrawableGroups[mDrawableGroupsSize] = group;
    }
    else
    {
        pushBack(mDrawableGroups, mDrawableGroupsAllocated, group);
    }
    ++mDrawableGroupsSize;
    mDrawableGroupsEnd = &mDrawableGroups[mDrawableGroupsSize];
}

void LLCullResult::pushDrawable(LLDrawable* drawable)
{
#if LL_DEBUG_CULL_RESULT
    // drawable must NOT be in the visible list already
    llassert(std::find(&mVisibleList[0], mVisibleListEnd, drawable) == mVisibleListEnd);
#endif
    if (mVisibleListSize < mVisibleListAllocated)
    {
        mVisibleList[mVisibleListSize] = drawable;
    }
    else
    {
        pushBack(mVisibleList, mVisibleListAllocated, drawable);
    }
    ++mVisibleListSize;
    mVisibleListEnd = &mVisibleList[mVisibleListSize];
}

void LLCullResult::pushBridge(LLSpatialBridge* bridge)
{
    if (mVisibleBridgeSize < mVisibleBridgeAllocated)
    {
        mVisibleBridge[mVisibleBridgeSize] = bridge;
    }
    else
    {
        pushBack(mVisibleBridge, mVisibleBridgeAllocated, bridge);
    }
    ++mVisibleBridgeSize;
    mVisibleBridgeEnd = &mVisibleBridge[mVisibleBridgeSize];
}

void LLCullResult::pushDrawInfo(U32 type, LLDrawInfo* draw_info)
{
    if (mRenderMapSize[type] < mRenderMapAllocated[type])
    {
        mRenderMap[type][mRenderMapSize[type]] = draw_info;
    }
    else
    {
        pushBack(mRenderMap[type], mRenderMapAllocated[type], draw_info);
    }
    ++mRenderMapSize[type];
    mRenderMapEnd[type] = &(mRenderMap[type][mRenderMapSize[type]]);
}


void LLCullResult::assertDrawMapsEmpty()
{
    for (U32 i = 0; i < LLRenderPass::NUM_RENDER_TYPES; i++)
    {
        if (mRenderMapSize[i] != 0)
        {
            LL_ERRS() << "Stale LLDrawInfo's in LLCullResult!"
                << " (mRenderMapSize[" << i << "] = " << mRenderMapSize[i] << ")" << LL_ENDL;
        }
    }
}
