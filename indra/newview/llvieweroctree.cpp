/**
 * @file llvieweroctree.cpp
 * @brief LLViewerOctreeGroup class implementation and supporting functions
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
#include "llvieweroctree.h"
#include "lloccensus.h"
#include <deque>
#include <unordered_map>
#include "llviewerregion.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "llappviewer.h"
#include "llshaderprogram.h"
#include "llviewershadermgr.h"
#include "lldrawpoolwater.h"

//-----------------------------------------------------------------------------------
//static variables definitions
//-----------------------------------------------------------------------------------
U32 LLViewerOctreeEntryData::sCurVisible = 10; //reserve the low numbers for special use.
bool LLViewerOctreeDebug::sInDebug = false;

static LLTrace::CountStatHandle<S32> sOcclusionQueries("occlusion_queries", "Number of occlusion queries executed"),
                                     sNumObjectsOccluded("occluded_objects", "Count of objects being occluded by a query"),
                                     sNumObjectsUnoccluded("unoccluded_objects", "Count of objects being unoccluded by a query");

//-----------------------------------------------------------------------------------
//some global functions definitions
//-----------------------------------------------------------------------------------
typedef enum
{
    b000 = 0x00,
    b001 = 0x01,
    b010 = 0x02,
    b011 = 0x03,
    b100 = 0x04,
    b101 = 0x05,
    b110 = 0x06,
    b111 = 0x07,
} eLoveTheBits;

//contact Runitai Linden for a copy of the SL object used to write this table
//basically, you give the table a bitmask of the look-at vector to a node and it
//gives you a triangle fan index array
static U16 sOcclusionIndices[] =
{
     //000
        b111, b110, b010, b011, b001, b101, b100, b110,
     //001
        b011, b010, b000, b001, b101, b111, b110, b010,
     //010
        b101, b100, b110, b111, b011, b001, b000, b100,
     //011
        b001, b000, b100, b101, b111, b011, b010, b000,
     //100
        b110, b000, b010, b011, b111, b101, b100, b000,
     //101
        b010, b100, b000, b001, b011, b111, b110, b100,
     //110
        b100, b010, b110, b111, b101, b001, b000, b010,
     //111
        b000, b110, b100, b101, b001, b011, b010, b110,
};

U32 get_box_fan_indices(LLCamera* camera, const LLVector4a& center)
{
    LLVector4a origin;
    origin.load3(camera->getOrigin().mV);

    S32 cypher = center.greaterThan(origin).getGatheredBits() & 0x7;

    return cypher*8;
}

U8* get_box_fan_indices_ptr(LLCamera* camera, const LLVector4a& center)
{
    LLVector4a origin;
    origin.load3(camera->getOrigin().mV);

    S32 cypher = center.greaterThan(origin).getGatheredBits() & 0x7;

    return (U8*) (sOcclusionIndices+cypher*8);
}

//create a vertex buffer for efficiently rendering cubes
LLVertexBuffer* ll_create_cube_vb(U32 type_mask)
{
    LLVertexBuffer* ret = new LLVertexBuffer(type_mask);

    ret->allocateBuffer(8, 64);

    LLStrider<LLVector3> pos;
    LLStrider<U16> idx;

    ret->getVertexStrider(pos);
    ret->getIndexStrider(idx);

    pos[0] = LLVector3(-1,-1,-1);
    pos[1] = LLVector3(-1,-1, 1);
    pos[2] = LLVector3(-1, 1,-1);
    pos[3] = LLVector3(-1, 1, 1);
    pos[4] = LLVector3( 1,-1,-1);
    pos[5] = LLVector3( 1,-1, 1);
    pos[6] = LLVector3( 1, 1,-1);
    pos[7] = LLVector3( 1, 1, 1);

    for (U32 i = 0; i < 64; i++)
    {
        idx[i] = sOcclusionIndices[i];
    }

    ret->unmapBuffer();

    return ret;
}


#define LL_TRACK_PENDING_OCCLUSION_QUERIES 0

const F32 SG_OCCLUSION_FUDGE = 0.25f;
#define SG_DISCARD_TOLERANCE 0.01f


S32 AABBSphereIntersect(const LLVector3& min, const LLVector3& max, const LLVector3 &origin, const F32 &rad)
{
    return AABBSphereIntersectR2(min, max, origin, rad*rad);
}

S32 AABBSphereIntersectR2(const LLVector3& min, const LLVector3& max, const LLVector3 &origin, const F32 &r)
{
    F32 d = 0.f;
    F32 t;

    if ((min-origin).magVecSquared() < r &&
        (max-origin).magVecSquared() < r)
    {
        return 2;
    }

    for (U32 i = 0; i < 3; i++)
    {
        if (origin.mV[i] < min.mV[i])
        {
            t = min.mV[i] - origin.mV[i];
            d += t*t;
        }
        else if (origin.mV[i] > max.mV[i])
        {
            t = origin.mV[i] - max.mV[i];
            d += t*t;
        }

        if (d > r)
        {
            return 0;
        }
    }

    return 1;
}


S32 AABBSphereIntersect(const LLVector4a& min, const LLVector4a& max, const LLVector3 &origin, const F32 &rad)
{
    return AABBSphereIntersectR2(min, max, origin, rad*rad);
}

S32 AABBSphereIntersectR2(const LLVector4a& min, const LLVector4a& max, const LLVector3 &origin, const F32 &r)
{
    F32 d = 0.f;
    F32 t;

    LLVector4a origina;
    origina.load3(origin.mV);

    LLVector4a v;
    v.setSub(min, origina);

    if (v.dot3(v) < r)
    {
        v.setSub(max, origina);
        if (v.dot3(v) < r)
        {
            return 2;
        }
    }


    for (U32 i = 0; i < 3; i++)
    {
        if (origin.mV[i] < min[i])
        {
            t = min[i] - origin.mV[i];
            d += t*t;
        }
        else if (origin.mV[i] > max[i])
        {
            t = origin.mV[i] - max[i];
            d += t*t;
        }

        if (d > r)
        {
            return 0;
        }
    }

    return 1;
}

//-----------------------------------------------------------------------------------
//class LLViewerOctreeEntry definitions
//-----------------------------------------------------------------------------------
LLViewerOctreeEntry::LLViewerOctreeEntry()
:   mGroup(NULL),
    mBinRadius(0.f),
    mBinIndex(-1),
    mVisible(0)
{
    mPositionGroup.clear();
    mExtents[0].clear();
    mExtents[1].clear();

    for(S32 i = 0; i < NUM_DATA_TYPE; i++)
    {
        mData[i] = NULL;
    }
}

LLViewerOctreeEntry::~LLViewerOctreeEntry()
{
    llassert(!mGroup);
}

void LLViewerOctreeEntry::addData(LLViewerOctreeEntryData* data)
{
    //llassert(mData[data->getDataType()] == NULL);
    llassert(data != NULL);

    mData[data->getDataType()] = data;
}

void LLViewerOctreeEntry::removeData(LLViewerOctreeEntryData* data)
{
    //llassert(data->getDataType() != LLVOCACHEENTRY); //can not remove VOCache entry

    if(!mData[data->getDataType()])
    {
        return;
    }
    if(mData[data->getDataType()] != data)
    {
        return;
    }

    mData[data->getDataType()] = NULL;

    if(mGroup != NULL && !mData[LLDRAWABLE])
    {
        LLViewerOctreeGroup* group = mGroup;
        mGroup = NULL;
        group->removeFromGroup(data);

        llassert(mBinIndex == -1);
    }
}

//called by group handleDestruction() ONLY when group is destroyed by octree.
void LLViewerOctreeEntry::nullGroup()
{
    mGroup = NULL;
}

void LLViewerOctreeEntry::setGroup(LLViewerOctreeGroup* group)
{
    if(mGroup == group)
    {
        return;
    }

    if(mGroup)
    {
        LLViewerOctreeGroup* old_group = mGroup;
        mGroup = NULL;
        old_group->removeFromGroup(this);

        llassert(mBinIndex == -1);
    }

    mGroup = group;
}

//-----------------------------------------------------------------------------------
//class LLViewerOctreeEntryData definitions
//-----------------------------------------------------------------------------------
LLViewerOctreeEntryData::~LLViewerOctreeEntryData()
{
    if(mEntry)
    {
        mEntry->removeData(this);
    }
}

LLViewerOctreeEntryData::LLViewerOctreeEntryData(LLViewerOctreeEntry::eEntryDataType_t data_type)
    : mDataType(data_type),
      mEntry(NULL)
{
}

//virtual
void LLViewerOctreeEntryData::setOctreeEntry(LLViewerOctreeEntry* entry)
{
    llassert_always(mEntry.isNull());

    if(mEntry.notNull())
    {
        return;
    }

    if(!entry)
    {
        mEntry = new LLViewerOctreeEntry();
    }
    else
    {
        mEntry = entry;
    }
    mEntry->addData(this);
}

void LLViewerOctreeEntryData::removeOctreeEntry()
{
    if(mEntry)
    {
        mEntry->removeData(this);
        mEntry = NULL;
    }
}

void LLViewerOctreeEntryData::setSpatialExtents(const LLVector3& min, const LLVector3& max)
{
    mEntry->mExtents[0].load3(min.mV);
    mEntry->mExtents[1].load3(max.mV);
}

void LLViewerOctreeEntryData::setSpatialExtents(const LLVector4a& min, const LLVector4a& max)
{
    mEntry->mExtents[0] = min;
    mEntry->mExtents[1] = max;
}

void LLViewerOctreeEntryData::setPositionGroup(const LLVector4a& pos)
{
    mEntry->mPositionGroup = pos;
}

const LLVector4a* LLViewerOctreeEntryData::getSpatialExtents() const
{
    return mEntry->getSpatialExtents();
}

//virtual
void LLViewerOctreeEntryData::setGroup(LLViewerOctreeGroup* group)
{
    mEntry->setGroup(group);
}

void LLViewerOctreeEntryData::shift(const LLVector4a &shift_vector)
{
    mEntry->mExtents[0].add(shift_vector);
    mEntry->mExtents[1].add(shift_vector);
    mEntry->mPositionGroup.add(shift_vector);
}

LLViewerOctreeGroup* LLViewerOctreeEntryData::getGroup()const
{
    return mEntry.notNull() ? mEntry->mGroup : NULL;
}

const LLVector4a& LLViewerOctreeEntryData::getPositionGroup() const
{
    return mEntry->getPositionGroup();
}

//virtual
bool LLViewerOctreeEntryData::isVisible() const
{
    if(mEntry)
    {
        return mEntry->mVisible == sCurVisible;
    }
    return false;
}

//virtual
bool LLViewerOctreeEntryData::isRecentlyVisible() const
{
    if(!mEntry)
    {
        return false;
    }

    if(isVisible())
    {
        return true;
    }
    if(getGroup() && getGroup()->isRecentlyVisible())
    {
        setVisible();
        return true;
    }

    return false;
}

void LLViewerOctreeEntryData::setVisible() const
{
    if(mEntry)
    {
        mEntry->mVisible = sCurVisible;
    }
}

void LLViewerOctreeEntryData::resetVisible() const
{
    if(mEntry)
    {
        mEntry->mVisible = 0;
    }
}
//-----------------------------------------------------------------------------------
//class LLViewerOctreeGroup definitions
//-----------------------------------------------------------------------------------

LLViewerOctreeGroup::~LLViewerOctreeGroup()
{
    //empty here
}

LLViewerOctreeGroup::LLViewerOctreeGroup(OctreeNode* node)
:   mOctreeNode(node),
    mAnyVisible(0),
    mState(CLEAN)
{
    LLVector4a tmp;
    tmp.splat(0.f);
    mExtents[0] = mExtents[1] = mObjectBounds[0] = mObjectBounds[1] =
        mObjectExtents[0] = mObjectExtents[1] = tmp;

    mBounds[0] = node->getCenter();
    mBounds[1] = node->getSize();

    mOctreeNode->addListener(this);
}

bool LLViewerOctreeGroup::hasElement(LLViewerOctreeEntryData* data)
{
    if(!data->getEntry())
    {
        return false;
    }
    return std::find(getDataBegin(), getDataEnd(), data->getEntry()) != getDataEnd();
}

bool LLViewerOctreeGroup::removeFromGroup(LLViewerOctreeEntryData* data)
{
    return removeFromGroup(data->getEntry());
}

bool LLViewerOctreeGroup::removeFromGroup(LLViewerOctreeEntry* entry)
{
    llassert(entry != NULL);
    llassert(!entry->getGroup());

    if(isDead()) //group is about to be destroyed, not need to double delete the entry.
    {
        entry->setBinIndex(-1);
        return true;
    }

    unbound();
    setState(OBJECT_DIRTY);

    if (mOctreeNode)
    {
        if (!mOctreeNode->remove(entry)) //this could cause *this* pointer to be destroyed, so no more function calls after this.
        {
            OCT_ERRS << "Could not remove LLVOCacheEntry from LLVOCacheOctreeGroup" << LL_ENDL;
            return false;
        }
    }

    return true;
}

//virtual
void LLViewerOctreeGroup::unbound()
{
    LL_PROFILE_ZONE_SCOPED;
    if (isDirty())
    {
        return;
    }

    setState(DIRTY);

    //all the parent nodes need to rebound this child
    if (mOctreeNode)
    {
        OctreeNode* parent = (OctreeNode*) mOctreeNode->getParent();
        while (parent != NULL)
        {
            LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) parent->getListener(0);
            if (!group || group->isDirty())
            {
                return;
            }

            group->setState(DIRTY);
            parent = (OctreeNode*) parent->getParent();
        }
    }
}

//virtual
void LLViewerOctreeGroup::rebound()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    if (!isDirty())
    {
        return;
    }

    if (mOctreeNode->getChildCount() == 1 && mOctreeNode->getElementCount() == 0)
    {
        LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) mOctreeNode->getChild(0)->getListener(0);
        group->rebound();

        //copy single child's bounding box
        mBounds[0] = group->mBounds[0];
        mBounds[1] = group->mBounds[1];
        mExtents[0] = group->mExtents[0];
        mExtents[1] = group->mExtents[1];

        group->setState(SKIP_FRUSTUM_CHECK);
    }
    else if (mOctreeNode->getChildCount() == 0)
    { //copy object bounding box if this is a leaf
        boundObjects(true, mExtents[0], mExtents[1]);
        mBounds[0] = mObjectBounds[0];
        mBounds[1] = mObjectBounds[1];
    }
    else
    {
        LLVector4a& newMin = mExtents[0];
        LLVector4a& newMax = mExtents[1];
        LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) mOctreeNode->getChild(0)->getListener(0);
        group->clearState(SKIP_FRUSTUM_CHECK);
        group->rebound();
        //initialize to first child
        newMin = group->mExtents[0];
        newMax = group->mExtents[1];

        //first, rebound children
        for (U32 i = 1; i < mOctreeNode->getChildCount(); i++)
        {
            group = (LLViewerOctreeGroup*) mOctreeNode->getChild(i)->getListener(0);
            group->clearState(SKIP_FRUSTUM_CHECK);
            group->rebound();
            const LLVector4a& max = group->mExtents[1];
            const LLVector4a& min = group->mExtents[0];

            newMax.setMax(newMax, max);
            newMin.setMin(newMin, min);
        }

        boundObjects(false, newMin, newMax);

        mBounds[0].setAdd(newMin, newMax);
        mBounds[0].mul(0.5f);
        mBounds[1].setSub(newMax, newMin);
        mBounds[1].mul(0.5f);
    }

    clearState(DIRTY);

    return;
}

//virtual
void LLViewerOctreeGroup::handleInsertion(const TreeNode* node, LLViewerOctreeEntry* obj)
{
    obj->setGroup(this);
    unbound();
    setState(OBJECT_DIRTY);
}

//virtual
void LLViewerOctreeGroup::handleRemoval(const TreeNode* node, LLViewerOctreeEntry* obj)
{
    unbound();
    setState(OBJECT_DIRTY);

    obj->setGroup(NULL); //this could cause *this* pointer to be destroyed. So no more function calls after this.
}

//virtual
void LLViewerOctreeGroup::handleDestruction(const TreeNode* node)
{
    if (isDead())
    {
        return;
    }
    setState(DEAD);
    for (OctreeNode::element_iter i = mOctreeNode->getDataBegin(); i != mOctreeNode->getDataEnd(); ++i)
    {
        LLViewerOctreeEntry* obj = *i;
        if (obj && obj->getGroup() == this)
        {
            obj->nullGroup();
        }
    }
    mOctreeNode = NULL;
}

//virtual
void LLViewerOctreeGroup::handleStateChange(const TreeNode* node)
{
    //drop bounding box upon state change
    if (mOctreeNode != node)
    {
        mOctreeNode = (OctreeNode*) node;
    }
    unbound();
}

//virtual
void LLViewerOctreeGroup::handleChildAddition(const OctreeNode* parent, OctreeNode* child)
{
    if (child->getListenerCount() == 0)
    {
        new LLViewerOctreeGroup(child);
    }
    else
    {
        OCT_ERRS << "LLViewerOctreeGroup redundancy detected." << LL_ENDL;
    }

    unbound();

    ((LLViewerOctreeGroup*)child->getListener(0))->unbound();
}

//virtual
void LLViewerOctreeGroup::handleChildRemoval(const OctreeNode* parent, const OctreeNode* child)
{
    unbound();
}

LLViewerOctreeGroup* LLViewerOctreeGroup::getParent()
{
    if (isDead())
    {
        return NULL;
    }

    if(!mOctreeNode)
    {
        return NULL;
    }

    OctreeNode* parent = mOctreeNode->getOctParent();

    if (parent)
    {
        return (LLViewerOctreeGroup*) parent->getListener(0);
    }

    return NULL;
}

//virtual
bool LLViewerOctreeGroup::boundObjects(bool empty, LLVector4a& minOut, LLVector4a& maxOut)
{
    const OctreeNode* node = mOctreeNode;

    if (node->isEmpty())
    {   //don't do anything if there are no objects
        if (empty && mOctreeNode->getParent())
        {   //only root is allowed to be empty
            OCT_ERRS << "Empty leaf found in octree." << LL_ENDL;
        }
        return false;
    }

    LLVector4a& newMin = mObjectExtents[0];
    LLVector4a& newMax = mObjectExtents[1];

    if (hasState(OBJECT_DIRTY))
    { //calculate new bounding box
        clearState(OBJECT_DIRTY);

        //initialize bounding box to first element
        OctreeNode::const_element_iter i = node->getDataBegin();
        LLViewerOctreeEntry* entry = *i;
        const LLVector4a* minMax = entry->getSpatialExtents();

        newMin = minMax[0];
        newMax = minMax[1];

        for (++i; i != node->getDataEnd(); ++i)
        {
            entry = *i;
            minMax = entry->getSpatialExtents();

            update_min_max(newMin, newMax, minMax[0]);
            update_min_max(newMin, newMax, minMax[1]);
        }

        mObjectBounds[0].setAdd(newMin, newMax);
        mObjectBounds[0].mul(0.5f);
        mObjectBounds[1].setSub(newMax, newMin);
        mObjectBounds[1].mul(0.5f);
    }

    if (empty)
    {
        minOut = newMin;
        maxOut = newMax;
    }
    else
    {
        minOut.setMin(minOut, newMin);
        maxOut.setMax(maxOut, newMax);
    }

    return true;
}

//virtual
bool LLViewerOctreeGroup::isVisible() const
{
    return mVisible[LLViewerCamera::sCurCameraID] >= LLViewerOctreeEntryData::getCurrentFrame();
}

//virtual
bool LLViewerOctreeGroup::isRecentlyVisible() const
{
    return false;
}

void LLViewerOctreeGroup::setVisible()
{
    mVisible[LLViewerCamera::sCurCameraID] = LLViewerOctreeEntryData::getCurrentFrame();

    if(LLViewerCamera::sCurCameraID < LLViewerCamera::CAMERA_WATER0)
    {
        mAnyVisible = LLViewerOctreeEntryData::getCurrentFrame();
    }
}

void LLViewerOctreeGroup::checkStates()
{
#if LL_OCTREE_PARANOIA_CHECK
    //LLOctreeStateCheck checker;
    //checker.traverse(mOctreeNode);
#endif
}

//-------------------------------------------------------------------------------------------
//occulsion culling functions and classes
//-------------------------------------------------------------------------------------------
std::set<U32> LLOcclusionCullingGroup::sPendingQueries;

static std::queue<U32> sFreeQueries;

// <FS> S7 H1: the groups by Hi-Z id, and the batches sent whose results have not been applied yet
static std::unordered_map<U32, LLOcclusionCullingGroup*> sHizGroups;
static U32 sHizNextId = 0;
namespace
{
struct HizPending { U32 mSerial; U32 mFrame; std::vector<U32> mIds; };
std::deque<HizPending> sHizPending;
}

#define QUERY_POOL_SIZE 1024

U32 LLOcclusionCullingGroup::getNewOcclusionQueryObjectName()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;

    if (sFreeQueries.empty())
    {
        //seed 1024 query names into the free query pool
        U32 queries[1024];
        gRender.genQueries(1024, queries);
        for (int i = 0; i < 1024; ++i)
        {
            sFreeQueries.push(queries[i]);
        }
    }

    // pull from pool
    U32 ret = sFreeQueries.front();
    sFreeQueries.pop();
    return ret;
}

void LLOcclusionCullingGroup::releaseOcclusionQueryObjectName(U32 name)
{
    if (name != 0)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
        sFreeQueries.push(name);
    }
}

//=====================================
//      Occlusion State Set/Clear
//=====================================
class LLSpatialSetOcclusionState : public OctreeTraveler
{
public:
    U32 mState;
    LLSpatialSetOcclusionState(U32 state) : mState(state) { }
    virtual void visit(const OctreeNode* branch)
    {
        LLOcclusionCullingGroup* group = (LLOcclusionCullingGroup*)branch->getListener(0);
        if(group)
        {
            group->setOcclusionState(mState);
        }
    }
};

class LLSpatialSetOcclusionStateDiff : public LLSpatialSetOcclusionState
{
public:
    LLSpatialSetOcclusionStateDiff(U32 state) : LLSpatialSetOcclusionState(state) { }

    virtual void traverse(const OctreeNode* n)
    {
        LLOcclusionCullingGroup* group = (LLOcclusionCullingGroup*) n->getListener(0);

        if (group && !group->isOcclusionState(mState))
        {
            OctreeTraveler::traverse(n);
        }
    }
};


LLOcclusionCullingGroup::LLOcclusionCullingGroup(OctreeNode* node, LLViewerOctreePartition* part) :
    LLViewerOctreeGroup(node),
    mSpatialPartition(part)
{
    part->mLODSeed = (part->mLODSeed+1)%part->mLODPeriod;
    mLODHash = part->mLODSeed;

    OctreeNode* oct_parent = node->getOctParent();
    LLOcclusionCullingGroup* parent = oct_parent ? (LLOcclusionCullingGroup*) oct_parent->getListener(0) : NULL;

    for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
    {
        mOcclusionQuery[i] = 0;
        mOcclusionCheckCount[i] = 0;
        mOcclusionIssued[i] = 0;
        mOcclusionState[i] = parent ? SG_STATE_INHERIT_MASK & parent->mOcclusionState[i] : 0;
        mVisible[i] = 0;
    }
}

LLOcclusionCullingGroup::~LLOcclusionCullingGroup()
{
    releaseOcclusionQueryObjectNames();
    if (mHizId)
    {
        sHizGroups.erase(mHizId);
    }
    delete mHizHist;
}

bool LLOcclusionCullingGroup::needsUpdate()
{
    return LLDrawable::getCurrentFrame() % mSpatialPartition->mLODPeriod == mLODHash;
}

bool LLOcclusionCullingGroup::isRecentlyVisible() const
{
    const S32 MIN_VIS_FRAME_RANGE = 2;
    return (LLDrawable::getCurrentFrame() - mVisible[LLViewerCamera::sCurCameraID]) < MIN_VIS_FRAME_RANGE ;
}

bool LLOcclusionCullingGroup::isAnyRecentlyVisible() const
{
    const S32 MIN_VIS_FRAME_RANGE = 2;
    return (LLDrawable::getCurrentFrame() - mAnyVisible) < MIN_VIS_FRAME_RANGE ;
}

//virtual
void LLOcclusionCullingGroup::handleChildAddition(const OctreeNode* parent, OctreeNode* child)
{
    if (!child->hasListeners())
    {
        new LLOcclusionCullingGroup(child, mSpatialPartition);
    }
    else
    {
        OCT_ERRS << "LLOcclusionCullingGroup redundancy detected." << LL_ENDL;
    }

    unbound();

    ((LLViewerOctreeGroup*)child->getListener(0))->unbound();
}

void LLOcclusionCullingGroup::releaseOcclusionQueryObjectNames()
{
    for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; ++i)
    {
        if (mOcclusionQuery[i])
        {
            releaseOcclusionQueryObjectName(mOcclusionQuery[i]);
            mOcclusionQuery[i] = 0;
        }
    }
}

void LLOcclusionCullingGroup::setOcclusionState(U32 state, S32 mode /* = STATE_MODE_SINGLE */ )
{
    switch (mode)
    {
    case STATE_MODE_SINGLE:
        if (state & OCCLUDED)
        {
            add(sNumObjectsOccluded, 1);
        }
        mOcclusionState[LLViewerCamera::sCurCameraID] |= state;
        if ((state & DISCARD_QUERY) && mOcclusionQuery[LLViewerCamera::sCurCameraID])
        {
            releaseOcclusionQueryObjectName(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
            mOcclusionQuery[LLViewerCamera::sCurCameraID] = 0;
        }
        break;

    case STATE_MODE_DIFF:
        if (mOctreeNode)
        {
            LLSpatialSetOcclusionStateDiff setter(state);
            setter.traverse(mOctreeNode);
        }
        break;

    case STATE_MODE_BRANCH:
        if (mOctreeNode)
        {
            LLSpatialSetOcclusionState setter(state);
            setter.traverse(mOctreeNode);
        }
        break;

    case STATE_MODE_ALL_CAMERAS:
        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            mOcclusionState[i] |= state;

            if ((state & DISCARD_QUERY) && mOcclusionQuery[i])
            {
                releaseOcclusionQueryObjectName(mOcclusionQuery[i]);
                mOcclusionQuery[i] = 0;
            }
        }
        break;

    default:
        break;
    }
}

class LLSpatialClearOcclusionState : public OctreeTraveler
{
public:
    U32 mState;

    LLSpatialClearOcclusionState(U32 state) : mState(state) { }
    virtual void visit(const OctreeNode* branch)
    {
        LLOcclusionCullingGroup* group = (LLOcclusionCullingGroup*)branch->getListener(0);
        if(group)
        {
            group->clearOcclusionState(mState);
        }
    }
};

class LLSpatialClearOcclusionStateDiff : public LLSpatialClearOcclusionState
{
public:
    LLSpatialClearOcclusionStateDiff(U32 state) : LLSpatialClearOcclusionState(state) { }

    virtual void traverse(const OctreeNode* n)
    {
        LLOcclusionCullingGroup* group = (LLOcclusionCullingGroup*) n->getListener(0);

        if (group && group->isOcclusionState(mState))
        {
            OctreeTraveler::traverse(n);
        }
    }
};

void LLOcclusionCullingGroup::clearOcclusionState(U32 state, S32 mode /* = STATE_MODE_SINGLE */)
{
    switch (mode)
    {
        case STATE_MODE_SINGLE:
            if (state & OCCLUDED)
            {
                add(sNumObjectsUnoccluded, 1);
            }
            mOcclusionState[LLViewerCamera::sCurCameraID] &= ~state;
            break;

        case STATE_MODE_DIFF:
            if (mOctreeNode)
            {
                LLSpatialClearOcclusionStateDiff clearer(state);
                clearer.traverse(mOctreeNode);
            }
            break;

        case STATE_MODE_BRANCH:
            if (mOctreeNode)
            {
                LLSpatialClearOcclusionState clearer(state);
                clearer.traverse(mOctreeNode);
            }
            break;

        case STATE_MODE_ALL_CAMERAS:
            for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
            {
                mOcclusionState[i] &= ~state;
            }
            break;

        default:
            break;
    }
}

bool LLOcclusionCullingGroup::earlyFail(LLCamera* camera, const LLVector4a* bounds)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    if (camera->getOrigin().isExactlyZero())
    {
        return false;
    }

    const F32 vel = SG_OCCLUSION_FUDGE*2.f;
    LLVector4a fudge;
    fudge.splat(vel);

    const LLVector4a& c = bounds[0];
    LLVector4a r;
    r.setAdd(bounds[1], fudge);

    /*if (r.magVecSquared() > 1024.0*1024.0)
    {
        return true;
    }*/

    LLVector4a e;
    e.load3(camera->getOrigin().mV);

    LLVector4a min;
    min.setSub(c,r);
    LLVector4a max;
    max.setAdd(c,r);

    S32 lt = e.lessThan(min).getGatheredBits() & 0x7;
    if (lt)
    {
        return false;
    }

    S32 gt = e.greaterThan(max).getGatheredBits() & 0x7;
    if (gt)
    {
        return false;
    }

    return true;
}

U32 LLOcclusionCullingGroup::getLastOcclusionIssuedTime()
{
    return mOcclusionIssued[LLViewerCamera::sCurCameraID];
}

void LLOcclusionCullingGroup::checkOcclusion()
{
    if (LLPipeline::sUseOcclusion < 2) return;  // 0 - NoOcclusion, 1 = ReadOnly, 2 = ModifyOcclusionState  TODO: DJH 11-2021 ENUM this

    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    LLOccCensus::Cam* census = LLOccCensus::cam(LLViewerCamera::sCurCameraID);      // <FS> H0 (LL_VK_OCC_CENSUS=1)
    if (census)
    {
        ++census->mChkCalls;
    }
    LLOcclusionCullingGroup* parent = (LLOcclusionCullingGroup*)getParent();
    if (parent && parent->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
    {   //if the parent has been marked as occluded, the child is implicitly occluded
        if (census)
        {
            ++census->mChkParentOccluded;
        }
        clearOcclusionState(QUERY_PENDING | DISCARD_QUERY);
        return;
    }

    if (!mOcclusionQuery[LLViewerCamera::sCurCameraID] && mHizSerial && isOcclusionState(QUERY_PENDING) && isOcclusionState(DISCARD_QUERY) && hizMode() == 1)
    {   // <FS> H1: a Hi-Z result is awaited and the group asked to be discarded (a drawable moved in it): as the query version below, it is visible until the next box has a result
        clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        clearOcclusionState(QUERY_PENDING | DISCARD_QUERY);
        mHizSerial = 0;
        return;
    }

    if (mOcclusionQuery[LLViewerCamera::sCurCameraID] && isOcclusionState(QUERY_PENDING))
    {
        struct PendingLap       // <FS> H0: time of the pending branch
        {
            LLOccCensus::Cam* mC; U64 mT0;
            explicit PendingLap(LLOccCensus::Cam* c) : mC(c), mT0(c ? LLOccCensus::now() : 0) { if (c) ++c->mChkPending; }
            ~PendingLap() { if (mC) mC->mChkTicks += LLOccCensus::now() - mT0; }
        } pending_lap(census);
        if (isOcclusionState(DISCARD_QUERY))
        {   // delete the query to avoid holding onto hundreds of pending queries
            releaseOcclusionQueryObjectName(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
            mOcclusionQuery[LLViewerCamera::sCurCameraID] = 0;
            // mark non-occluded
            if (census)
            {
                ++census->mChkDiscard;
            }
            clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
            clearOcclusionState(QUERY_PENDING | DISCARD_QUERY);
        }
        else
        {
            bool available;
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("co - query available");
                available = gRender.queryReady(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
                mOcclusionCheckCount[LLViewerCamera::sCurCameraID]++;
            }

            static LLCachedControl<U32> occlusion_timeout(gSavedSettings, "RenderOcclusionTimeout", 4);

            if (census)
            {
                const int slot = LLOccCensus::age_slot((U32)gFrameCount, (U32)mOcclusionIssued[LLViewerCamera::sCurCameraID]);
                if (available)
                {
                    ++census->mChkAvailable;
                    ++census->mAge[slot];
                }
                else if (mOcclusionCheckCount[LLViewerCamera::sCurCameraID] > occlusion_timeout)
                {
                    ++census->mChkForced;
                    ++census->mAgeForced[slot];
                }
                else
                {
                    ++census->mChkNotReady;
                }
            }

            if (available || mOcclusionCheckCount[LLViewerCamera::sCurCameraID] > occlusion_timeout)
            {
                mOcclusionCheckCount[LLViewerCamera::sCurCameraID] = 0;
                U64 query_result;       // a boolean (the any-samples kind)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("co - query result");
                    // Ping watchdog before a blocking GL_QUERY_RESULT GPU call
                    LLAppViewer::instance()->pingMainloopTimeout("Display:CullQueryResult");
                    query_result = gRender.queryResult(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
                }
#if LL_TRACK_PENDING_OCCLUSION_QUERIES
                sPendingQueries.erase(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
#endif

                if (census)
                {
                    ++(query_result > 0 ? census->mChkVisible : census->mChkOccluded);
                    if (mHizHist && hizMode() == 2 && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD)
                    {
                        // the Hi-Z result that belongs to this query (same box, same frame)
                        const U32 issued = (U32)mOcclusionIssued[LLViewerCamera::sCurCameraID];
                        if (mHizHist->mFrame[issued & 7] == issued && issued)
                        {
                            const bool hiz_occ = mHizHist->mBit[issued & 7] != 0, q_occ = query_result == 0;
                            if (hiz_occ && q_occ) ++LLOccCensus::sCmpBothOcc;
                            else if (!hiz_occ && !q_occ) ++LLOccCensus::sCmpBothVis;
                            else if (hiz_occ) { ++LLOccCensus::sCmpHizOccQVis; ++LLOccCensus::sRiskLod[std::min<U32>(mHizHist->mLod[issued & 7], 15)]; ++LLOccCensus::sMarginRisk[LLOccCensus::margin_bin(mHizHist->mMargin[issued & 7])]; }
                            else { ++LLOccCensus::sCmpHizVisQOcc; ++LLOccCensus::sMarginLost[LLOccCensus::margin_bin(mHizHist->mMargin[issued & 7])]; }
                        }
                        else
                        {
                            ++LLOccCensus::sCmpNoHist;
                        }
                    }
                }
                if (query_result > 0)
                {
                    clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
                }
                else
                {
                    setOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
                }
                clearOcclusionState(QUERY_PENDING);
            }
        }
    }
    else if (mSpatialPartition->isOcclusionEnabled() && isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
    {   //check occlusion has been issued for occluded node that has not had a query issued
        assert_states_valid(this);
        //clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        assert_states_valid(this);
    }
}

// <FS> S7 H1
U32 LLOcclusionCullingGroup::hizMode()
{
    static const U32 mode = []() { const char* e = getenv("LL_VK_HIZ"); return e ? (U32)atoi(e) : 1u; }();
    return mode;
}

bool LLOcclusionCullingGroup::hizCollect(LLCamera* camera, U32 mode, std::vector<LLVkBridge::HizBox>& boxes, std::vector<LLOcclusionCullingGroup*>& groups)
{
    if (!(mSpatialPartition->isOcclusionEnabled() && LLPipeline::sUseOcclusion > 1))
    {
        return false;
    }
    const LLVector4a* b = mBounds;
    F32 fudge_z = SG_OCCLUSION_FUDGE;
    const bool water = mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_WATER || mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_VOIDWATER;
    if (LLPipeline::RENDER_TYPE_VOIDWATER == mSpatialPartition->mDrawableType)
    {
        fudge_z = 1.f;
    }
    if (earlyFail(camera, b))
    {
        if (mode == 1)
        {   // as doOcclusion: the camera is inside the box, nothing to test
            setOcclusionState(LLOcclusionCullingGroup::DISCARD_QUERY);
            clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
            mHizSerial = 0;
        }
        return false;
    }
    if (isOcclusionState(QUERY_PENDING) && !isOcclusionState(DISCARD_QUERY))
    {
        return false;
    }
    if (!mHizId)
    {
        mHizId = ++sHizNextId;
        sHizGroups[mHizId] = this;
    }
    LLVkBridge::HizBox box;
    box.mCenter[0] = b[0][0]; box.mCenter[1] = b[0][1]; box.mCenter[2] = b[0][2];
    box.mFlags = water ? 1u : 0u;
    box.mHalf[0] = b[1][0] + SG_OCCLUSION_FUDGE; box.mHalf[1] = b[1][1] + SG_OCCLUSION_FUDGE; box.mHalf[2] = b[1][2] + fudge_z;
    box.mPad = 0.f;
    boxes.push_back(box);
    groups.push_back(this);
    if (mode == 1)
    {
        mOcclusionIssued[LLViewerCamera::sCurCameraID] = gFrameCount;
        setOcclusionState(LLOcclusionCullingGroup::QUERY_PENDING);
        clearOcclusionState(LLOcclusionCullingGroup::DISCARD_QUERY);
    }
    return true;
}

void LLOcclusionCullingGroup::hizSend(std::vector<LLVkBridge::HizBox>& boxes, std::vector<LLOcclusionCullingGroup*>& groups, const F32* view_proj)
{
    struct Lap { U64 mT0 = LLOccCensus::sOn ? LLOccCensus::now() : 0; ~Lap() { if (LLOccCensus::sOn) LLOccCensus::sHizTicksSend += LLOccCensus::now() - mT0; } } lap;
    const U32 mode = hizMode();
    const U32 serial = boxes.empty() ? 0 : LLVkBridge::hizSend(boxes.data(), (U32)boxes.size(), view_proj);
    if (LLOccCensus::sOn)
    {
        ++(serial || boxes.empty() ? LLOccCensus::sHizSends : LLOccCensus::sHizRefused);
        LLOccCensus::sHizBoxes += boxes.size();
    }
    if (!serial)
    {
        if (mode == 1)
        {   // nothing was sent: the groups are not waiting for a result (the next pass sends them again)
            for (LLOcclusionCullingGroup* g : groups)
            {
                g->clearOcclusionState(QUERY_PENDING);
                g->mHizSerial = 0;
            }
        }
        return;
    }
    HizPending p;
    p.mSerial = serial;
    p.mFrame = (U32)gFrameCount;
    p.mIds.reserve(groups.size());
    for (LLOcclusionCullingGroup* g : groups)
    {
        p.mIds.push_back(g->mHizId);
        if (mode == 1)
        {
            g->mHizSerial = serial;
        }
    }
    sHizPending.push_back(std::move(p));
}

void LLOcclusionCullingGroup::hizApply()
{
    const U32 mode = hizMode();
    if (!mode || sHizPending.empty())
    {
        return;
    }
    struct Lap { U64 mT0 = LLOccCensus::sOn ? LLOccCensus::now() : 0; ~Lap() { if (LLOccCensus::sOn) LLOccCensus::sHizTicksApply += LLOccCensus::now() - mT0; } } lap;
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;        // the occlusion state arrays are per camera
    static std::vector<U32> words;
    while (!sHizPending.empty())
    {
        HizPending& p = sHizPending.front();
        words.resize(3 * p.mIds.size());
        S32 r = LLVkBridge::hizRead(p.mSerial, (U32)p.mIds.size(), words.data());
        if (r == 0)
        {
            if ((U32)gFrameCount - p.mFrame > 60)
            {
                r = -1;         // never published (the worker could not run the record)
            }
            else
            {
                break;
            }
        }
        const bool census = LLOccCensus::sOn;
        if (r < 0)
        {
            if (census)
            {
                ++LLOccCensus::sHizLost;
            }
            for (U32 id : p.mIds)
            {
                auto it = sHizGroups.find(id);
                if (it != sHizGroups.end() && it->second->mHizSerial == p.mSerial)
                {
                    it->second->mHizSerial = 0;
                    if (mode == 1)
                    {
                        it->second->clearOcclusionState(QUERY_PENDING);
                    }
                }
            }
            LLVkBridge::hizDone(p.mSerial);
            sHizPending.pop_front();
            continue;
        }
        if (census)
        {
            ++LLOccCensus::sHizApplied;
            ++LLOccCensus::sHizLag[LLOccCensus::age_slot((U32)gFrameCount, p.mFrame)];
        }
        for (size_t i = 0; i < p.mIds.size(); ++i)
        {
            auto it = sHizGroups.find(p.mIds[i]);
            if (it == sHizGroups.end())
            {
                if (census)
                {
                    ++LLOccCensus::sHizDead;
                }
                continue;
            }
            LLOcclusionCullingGroup* g = it->second;
            const U32 word = words[3 * i];
            float minz, maxd;
            memcpy(&minz, &words[3 * i + 1], 4);
            memcpy(&maxd, &words[3 * i + 2], 4);
            const bool occluded = (word & 1u) != 0;
            if (census)
            {
                LLOccCensus::sHizOccluded += occluded;
                ++LLOccCensus::sHizLod[std::min<U32>((word >> 8) & 0xff, 15)];
                LLOccCensus::sHizWhy[0] += (word >> 16) & 1u;
                LLOccCensus::sHizWhy[1] += (word >> 17) & 1u;
                LLOccCensus::sHizWhy[2] += (word >> 18) & 1u;
            }
            if (mode == 2)
            {
                if (!g->mHizHist)
                {
                    g->mHizHist = new HizHist();
                }
                g->mHizHist->mFrame[p.mFrame & 7] = p.mFrame;
                g->mHizHist->mBit[p.mFrame & 7] = occluded ? 1 : 0;
                g->mHizHist->mLod[p.mFrame & 7] = (U8)((word >> 8) & 0xff);
                g->mHizHist->mMargin[p.mFrame & 7] = minz - maxd;
                continue;
            }
            if (g->mHizSerial != p.mSerial)
            {
                if (census)
                {
                    ++LLOccCensus::sHizSuperseded;
                }
                continue;
            }
            g->mHizSerial = 0;
            if (occluded)
            {
                g->setOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
            }
            else
            {
                g->clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
            }
            g->clearOcclusionState(QUERY_PENDING);
        }
        LLVkBridge::hizDone(p.mSerial);
        sHizPending.pop_front();
    }
    LLViewerCamera::sCurCameraID = saved_camera;
}

void LLOcclusionCullingGroup::doOcclusion(LLCamera* camera, const LLVector4a* shift)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    if (mSpatialPartition->isOcclusionEnabled() && LLPipeline::sUseOcclusion > 1)
    {
        //move mBounds to the agent space if necessary
        LLVector4a bounds[2];
        bounds[0] = mBounds[0];
        bounds[1] = mBounds[1];
        if(shift != NULL)
        {
            bounds[0].add(*shift);
        }

        F32 OCCLUSION_FUDGE_Z = SG_OCCLUSION_FUDGE; //<-- #Solution #2
        if (LLPipeline::RENDER_TYPE_VOIDWATER == mSpatialPartition->mDrawableType)
        {
            OCCLUSION_FUDGE_Z = 1.;
        }

        LLOccCensus::Cam* census = LLOccCensus::cam(LLViewerCamera::sCurCameraID);      // <FS> H0 (LL_VK_OCC_CENSUS=1)
        if (census)
        {
            ++census->mDoCalls;
            census->mDoVoCache += shift != NULL;
        }
        if (earlyFail(camera, bounds))
        {
            if (census)
            {
                ++census->mDoEarlyFail;
            }
            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - early fail");
            setOcclusionState(LLOcclusionCullingGroup::DISCARD_QUERY);
            assert_states_valid(this);
            clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
            assert_states_valid(this);
        }
        else
        {
            if (census && isOcclusionState(QUERY_PENDING) && !isOcclusionState(DISCARD_QUERY))
            {
                ++census->mDoSkipPending;       // a query of this group is still in flight: no new one
            }
            if (!isOcclusionState(QUERY_PENDING) || isOcclusionState(DISCARD_QUERY))
            {
                if (census)
                {
                    ++census->mDoIssued;
                    census->mDoDiscardReissue += isOcclusionState(QUERY_PENDING);
                    census->mDoWater += mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_WATER || mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_VOIDWATER;
                }
                { //no query pending, or previous query to be discarded
                    LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - render");

                    if (!mOcclusionQuery[LLViewerCamera::sCurCameraID])
                    {
                        mOcclusionQuery[LLViewerCamera::sCurCameraID] = getNewOcclusionQueryObjectName();
                    }

                    // Depth clamp all water to avoid it being culled as a result of being
                    // behind the far clip plane, and in the case of edge water to avoid
                    // it being culled while still visible.
                    bool const use_depth_clamp = (mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_WATER ||
                                                mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_VOIDWATER);

                    LLRenderState::DepthClampScope clamp(true, use_depth_clamp);


#if LL_TRACK_PENDING_OCCLUSION_QUERIES
                    sPendingQueries.insert(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
#endif
                    add(sOcclusionQueries, 1);

                    {
                        LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - push");

                        //store which frame this query was issued on
                        mOcclusionIssued[LLViewerCamera::sCurCameraID] = gFrameCount;

                        {
                            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("glBeginQuery");

                            //get an occlusion query that hasn't been used in awhile
                            releaseOcclusionQueryObjectName(mOcclusionQuery[LLViewerCamera::sCurCameraID]);
                            mOcclusionQuery[LLViewerCamera::sCurCameraID] = getNewOcclusionQueryObjectName();
                            gRender.beginQuery(LLRender::QUERY_ANY_SAMPLES, mOcclusionQuery[LLViewerCamera::sCurCameraID]);
                        }

                        LLShaderProgram* shader = LLShaderProgram::sCurBoundShaderPtr;
                        llassert(shader);

                        shader->uniform3fv(LLShaderMgr::BOX_CENTER, 1, bounds[0].getF32ptr());
                        shader->uniform3f(LLShaderMgr::BOX_SIZE, bounds[1][0]+SG_OCCLUSION_FUDGE,
                                                                 bounds[1][1]+SG_OCCLUSION_FUDGE,
                                                                 bounds[1][2]+OCCLUSION_FUDGE_Z);

                        if (!use_depth_clamp && mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_VOIDWATER)
                        {
                            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - draw water");

                            LLRenderSquashToFarClip squash;
                            if (camera->getOrigin().isExactlyZero())
                            { //origin is invalid, draw entire box
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, 0);
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, b111*8);
                            }
                            else
                            {
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, get_box_fan_indices(camera, bounds[0]));
                            }
                        }
                        else
                        {
                            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - draw");
                            if (camera->getOrigin().isExactlyZero())
                            { //origin is invalid, draw entire box
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, 0);
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, b111*8);
                            }
                            else
                            {
                                gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, get_box_fan_indices(camera, bounds[0]));
                            }
                        }

                        {
                            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("glEndQuery");
                            gRender.endQuery(LLRender::QUERY_ANY_SAMPLES);
                        }
                    }
                }

                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("doOcclusion - set state");
                    setOcclusionState(LLOcclusionCullingGroup::QUERY_PENDING);
                    clearOcclusionState(LLOcclusionCullingGroup::DISCARD_QUERY);
                }
            }
        }
    }
}
// <FS> H0 (doc/vulkan/S7-design.md): the per frame numbers of the occlusion queries, one log line per camera that did anything (LL_VK_OCC_CENSUS=1)
void LLOccCensus::report()
{
    using namespace LLOccCensus;
    LARGE_INTEGER qf, qn;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qn);
    const U64 tsc = __rdtsc();
    const double window_qpc = (double)std::max<long long>(1, qn.QuadPart - sQpc0);
    const double ms_per_tick = 1000.0 * window_qpc / (double)qf.QuadPart / (double)std::max<U64>(1, tsc - sTsc0);
    const double frames = (double)std::max<U64>(1, sFrames);
    LL_INFOS("OccCensus") << "H0 window: " << sFrames << " world passes, " << window_qpc * 1000.0 / (double)qf.QuadPart / frames << " ms wall each; time of the probe occlusion "
        << (double)sTicksProbes * ms_per_tick / frames << " ms, processOccluders " << (double)sTicksCache * ms_per_tick / frames << " ms (" << (double)sCachePasses / frames << " calls with groups)"
        << "; probe queries per frame: calls " << (double)sProbeCalls / frames << ", eye inside " << (double)sProbeInside / frames << ", first query " << (double)sProbeNew / frames
        << ", result read " << (double)sProbeReady / frames << " (visible " << (double)sProbeVisible / frames << ", occluded " << (double)sProbeOccluded / frames << "), still pending " << (double)sProbePending / frames
        << ", cube draws " << (double)sProbeIssued / frames << LL_ENDL;
    {
        std::string lag, lod, risk;
        for (int a = 0; a < AGES; ++a)
        {
            lag += llformat("%s%.1f", a ? " " : "", (double)sHizLag[a] / frames);
        }
        for (int a = 0; a < 12; ++a)
        {
            lod += llformat("%s%.1f", a ? " " : "", (double)sHizLod[a] / frames);
            risk += llformat("%s%.2f", a ? " " : "", (double)sRiskLod[a] / frames);
        }
        std::string margin_risk, margin_lost;
        for (int a = 0; a < 8; ++a)
        {
            margin_risk += llformat("%s%.2f", a ? " " : "", (double)sMarginRisk[a] / frames);
            margin_lost += llformat("%s%.2f", a ? " " : "", (double)sMarginLost[a] / frames);
            sMarginRisk[a] = sMarginLost[a] = 0;
        }
        const double cmp_total = (double)(sCmpBothOcc + sCmpBothVis + sCmpHizOccQVis + sCmpHizVisQOcc);
        LL_INFOS("OccCensus") << "Hi-Z (mode " << LLOcclusionCullingGroup::hizMode() << ") main thread ms per frame: collect boxes " << (double)sHizTicksCollect * ms_per_tick / frames << ", send " << (double)sHizTicksSend * ms_per_tick / frames
            << ", apply results " << (double)sHizTicksApply * ms_per_tick / frames << " | per frame: batches sent " << (double)sHizSends / frames << " (refused " << (double)sHizRefused / frames << "), boxes " << (double)sHizBoxes / frames
            << ", results applied " << (double)sHizApplied / frames << " batches, lost " << (double)sHizLost / frames << ", groups gone " << (double)sHizDead / frames << ", superseded " << (double)sHizSuperseded / frames
            << ", OCCLUDED by Hi-Z " << (double)sHizOccluded / frames << "; forced visible: behind near plane " << (double)sHizWhy[0] / frames << ", off screen " << (double)sHizWhy[1] / frames << ", beyond far " << (double)sHizWhy[2] / frames
            << " | result age in frames when applied (0 1 2 3 4 5 6 7+): " << lag << " | pyramid level used (0..11): " << lod
            << " | compare with the queries issued with the same boxes (mode 2): both occluded " << (double)sCmpBothOcc / frames << ", both visible " << (double)sCmpBothVis / frames
            << ", HI-Z OCCLUDED / QUERY SAW SAMPLES (pop-in risk) " << (double)sCmpHizOccQVis / frames << " (" << (cmp_total > 0 ? 100.0 * (double)sCmpHizOccQVis / cmp_total : 0.0) << " %), Hi-Z visible / query occluded (lost saving) "
            << (double)sCmpHizVisQOcc / frames << " (" << (cmp_total > 0 ? 100.0 * (double)sCmpHizVisQOcc / cmp_total : 0.0) << " %), no Hi-Z result to compare " << (double)sCmpNoHist / frames
            << "; pop-in risk by pyramid level: " << risk << " | margin (nearest box depth minus largest pyramid depth; bins <-1e-2, -1e-2..-1e-3, -1e-3..-1e-4, -1e-4..0, 0..1e-4, 1e-4..1e-3, 1e-3..1e-2, >1e-2) of the pop-in risk cases: " << margin_risk
            << " and of the lost saving cases: " << margin_lost << LL_ENDL;
        sHizTicksCollect = sHizTicksSend = sHizTicksApply = 0;
        sHizSends = sHizRefused = sHizBoxes = sHizApplied = sHizOccluded = sHizLost = sHizSuperseded = sHizDead = 0;
        sCmpBothOcc = sCmpBothVis = sCmpHizOccQVis = sCmpHizVisQOcc = sCmpNoHist = 0;
        for (int a = 0; a < AGES; ++a) sHizLag[a] = 0;
        for (int a = 0; a < 16; ++a) { sHizLod[a] = 0; sRiskLod[a] = 0; }
        sHizWhy[0] = sHizWhy[1] = sHizWhy[2] = 0;
    }
    static const char* const names[CAMS] = { "world", "cam1", "cam2", "cam3", "cam4", "cam5", "cam6", "cam7", "cam8" };
    for (int i = 0; i < CAMS; ++i)
    {
        const Cam& c = sCam[i];
        if (!c.mDoCalls && !c.mChkCalls && !c.mCullEarlyFail && !c.mSortGroups && !sPasses[i])
        {
            continue;
        }
        std::string ages, ages_forced;
        for (int a = 0; a < AGES; ++a)
        {
            ages += llformat("%s%.1f", a ? " " : "", (double)c.mAge[a] / frames);
            ages_forced += llformat("%s%.1f", a ? " " : "", (double)c.mAgeForced[a] / frames);
        }
        const double reads = (double)(c.mChkVisible + c.mChkOccluded);
        LL_INFOS("OccCensus") << "camera " << i << " (" << names[i] << ") per frame: doOcclusion passes " << (double)sPasses[i] / frames << ", list " << (double)sListGroups[i] / frames << " groups ("
            << (double)sListDead[i] / frames << " dead, " << (double)sListOccluded[i] / frames << " OCCLUDED already), time of the pass " << (double)sTicksPass[i] * ms_per_tick / frames << " ms of which the group loop "
            << (double)sTicksGroups[i] * ms_per_tick / frames << " ms"
            << " | group->doOcclusion: calls " << (double)c.mDoCalls / frames << " (object cache " << (double)c.mDoVoCache / frames << "), early fail " << (double)c.mDoEarlyFail / frames << ", kept the pending query "
            << (double)c.mDoSkipPending / frames << ", issued (cube draws + begin/end) " << (double)c.mDoIssued / frames << " (replacing a pending one " << (double)c.mDoDiscardReissue / frames << ", water "
            << (double)c.mDoWater / frames << ")"
            << " | checkOcclusion: calls " << (double)c.mChkCalls / frames << ", parent occluded " << (double)c.mChkParentOccluded / frames << ", pending branch " << (double)c.mChkPending / frames << " ("
            << (double)c.mChkTicks * ms_per_tick / frames << " ms), discard " << (double)c.mChkDiscard / frames << ", ready " << (double)c.mChkAvailable / frames << ", forced by the timeout "
            << (double)c.mChkForced / frames << ", not ready yet " << (double)c.mChkNotReady / frames << ", results: visible " << (double)c.mChkVisible / frames << ", OCCLUDED " << (double)c.mChkOccluded / frames
            << " (" << (reads > 0 ? 100.0 * (double)c.mChkOccluded / reads : 0.0) << " % of the results)"
            << " | age of a ready result in frames (0 1 2 3 4 5 6 7+): " << ages << "; forced reads by age: " << ages_forced
            << " | cull earlyFail calls " << (double)c.mCullEarlyFail / frames << ", groups dropped as OCCLUDED " << (double)c.mCullDropped / frames
            << " | stateSort groups " << (double)c.mSortGroups / frames << ", OCCLUDED " << (double)c.mSortOccluded / frames << " (elements " << (double)c.mSortOccludedElems / frames << " of "
            << (double)c.mSortElems / frames << ")" << LL_ENDL;
    }
    for (int i = 0; i < CAMS; ++i)
    {
        sCam[i] = Cam();
        sPasses[i] = sListGroups[i] = sListDead[i] = sListOccluded[i] = sTicksPass[i] = sTicksGroups[i] = 0;
    }
    sTicksProbes = sTicksCache = sCachePasses = 0;
    sProbeCalls = sProbeInside = sProbeNew = sProbeReady = sProbePending = sProbeVisible = sProbeOccluded = sProbeIssued = 0;
    sFrames = 0;
    sQpc0 = qn.QuadPart;
    sTsc0 = tsc;
}
//-------------------------------------------------------------------------------------------
//end of occulsion culling functions and classes
//-------------------------------------------------------------------------------------------

//-----------------------------------------------------------------------------------
//class LLViewerOctreePartition definitions
//-----------------------------------------------------------------------------------
LLViewerOctreePartition::LLViewerOctreePartition() :
    mRegionp(NULL),
    mOcclusionEnabled(true),
    mDrawableType(0),
    mLODSeed(0),
    mLODPeriod(1)
{
    LLVector4a center, size;
    center.splat(0.f);
    size.splat(1.f);

    mOctree = new OctreeRoot(center,size, NULL);
}

LLViewerOctreePartition::~LLViewerOctreePartition()
{
    cleanup();
}

void LLViewerOctreePartition::cleanup()
{
    delete mOctree;
    mOctree = nullptr;
}

bool LLViewerOctreePartition::isOcclusionEnabled()
{
    return mOcclusionEnabled || LLPipeline::sUseOcclusion > 2;
}


//-----------------------------------------------------------------------------------
//class LLViewerOctreeCull definitions
//-----------------------------------------------------------------------------------

//virtual
bool LLViewerOctreeCull::earlyFail(LLViewerOctreeGroup* group)
{
    return false;
}

//virtual
void LLViewerOctreeCull::traverse(const OctreeNode* n)
{
    LL_PROFILE_ZONE_SCOPED;
    LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) n->getListener(0);

    if (earlyFail(group))
    {
        return;
    }

    if (mRes == 2 ||
        (mRes && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)))
    {   //fully in, just add everything
        LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("AllInside");
        OctreeTraveler::traverse(n);
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("Check inside?");
        mRes = frustumCheck(group);

        if (mRes)
        { //at least partially in, run on down
            LL_PROFILE_ZONE_NAMED_CATEGORY_OCTREE("PartiallyIn");
            OctreeTraveler::traverse(n);
        }

        mRes = 0;
    }
}

//------------------------------------------
//agent space group culling
S32 LLViewerOctreeCull::AABBInFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustumNoFarClip(group->mBounds[0], group->mBounds[1]);
}

S32 LLViewerOctreeCull::AABBSphereIntersectGroupExtents(const LLViewerOctreeGroup* group)
{
    return AABBSphereIntersect(group->mExtents[0], group->mExtents[1], mCamera->getOrigin(), mCamera->mFrustumCornerDist);
}

S32 LLViewerOctreeCull::AABBInFrustumGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustum(group->mBounds[0], group->mBounds[1]);
}
//------------------------------------------

//------------------------------------------
//agent space object set culling
S32 LLViewerOctreeCull::AABBInFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustumNoFarClip(group->mObjectBounds[0], group->mObjectBounds[1]);
}

S32 LLViewerOctreeCull::AABBSphereIntersectObjectExtents(const LLViewerOctreeGroup* group)
{
    return AABBSphereIntersect(group->mObjectExtents[0], group->mObjectExtents[1], mCamera->getOrigin(), mCamera->mFrustumCornerDist);
}

S32 LLViewerOctreeCull::AABBInFrustumObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustum(group->mObjectBounds[0], group->mObjectBounds[1]);
}
//------------------------------------------

//------------------------------------------
//local regional space group culling
S32 LLViewerOctreeCull::AABBInRegionFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustumNoFarClip(group->mBounds[0], group->mBounds[1]);
}

S32 LLViewerOctreeCull::AABBInRegionFrustumGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustum(group->mBounds[0], group->mBounds[1]);
}

S32 LLViewerOctreeCull::AABBRegionSphereIntersectGroupExtents(const LLViewerOctreeGroup* group, const LLVector3& shift)
{
    return AABBSphereIntersect(group->mExtents[0], group->mExtents[1], mCamera->getOrigin() - shift, mCamera->mFrustumCornerDist);
}
//------------------------------------------

//------------------------------------------
//local regional space object culling
S32 LLViewerOctreeCull::AABBInRegionFrustumObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustum(group->mObjectBounds[0], group->mObjectBounds[1]);
}

S32 LLViewerOctreeCull::AABBInRegionFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustumNoFarClip(group->mObjectBounds[0], group->mObjectBounds[1]);
}

S32 LLViewerOctreeCull::AABBRegionSphereIntersectObjectExtents(const LLViewerOctreeGroup* group, const LLVector3& shift)
{
    return AABBSphereIntersect(group->mObjectExtents[0], group->mObjectExtents[1], mCamera->getOrigin() - shift, mCamera->mFrustumCornerDist);
}
//------------------------------------------
//check if the objects projection large enough

bool LLViewerOctreeCull::checkProjectionArea(const LLVector4a& center, const LLVector4a& size, const LLVector3& shift, F32 pixel_threshold, F32 near_radius)
{
    LLVector3 local_orig = mCamera->getOrigin() - shift;
    LLVector4a origin;
    origin.load3(local_orig.mV);

    LLVector4a lookAt;
    lookAt.setSub(center, origin);
    F32 distance = lookAt.getLength3().getF32();
    if(distance <= near_radius)
    {
        return true; //always load close-by objects
    }

    // treat object as if it were near_radius meters closer than it actually was.
    // this allows us to get some temporal coherence on visibility...objects that can be reached quickly will tend to be visible
    distance -= near_radius;

    F32 squared_rad = size.dot3(size).getF32();
    return squared_rad / distance > pixel_threshold;
}

//virtual
bool LLViewerOctreeCull::checkObjects(const OctreeNode* branch, const LLViewerOctreeGroup* group)
{
    if (branch->getElementCount() == 0) //no elements
    {
        return false;
    }
    else if (branch->getChildCount() == 0) //leaf state, already checked tightest bounding box
    {
        return true;
    }
    else if (mRes == 1 && !frustumCheckObjects(group)) //no objects in frustum
    {
        return false;
    }

    return true;
}

//virtual
void LLViewerOctreeCull::preprocess(LLViewerOctreeGroup* group)
{
}

//virtual
void LLViewerOctreeCull::processGroup(LLViewerOctreeGroup* group)
{
}

//virtual
void LLViewerOctreeCull::visit(const OctreeNode* branch)
{
    LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) branch->getListener(0);

    preprocess(group);

    if (checkObjects(branch, group))
    {
        processGroup(group);
    }
}

//--------------------------------------------------------------
//class LLViewerOctreeDebug
//virtual
void LLViewerOctreeDebug::visit(const OctreeNode* branch)
{
#if 0
    LL_INFOS() << "Node: " << (U32)branch << " # Elements: " << branch->getElementCount() << " # Children: " << branch->getChildCount() << LL_ENDL;
    for (U32 i = 0; i < branch->getChildCount(); i++)
    {
        LL_INFOS() << "Child " << i << " : " << (U32)branch->getChild(i) << LL_ENDL;
    }
#endif
    LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) branch->getListener(0);
    processGroup(group);
}

//virtual
void LLViewerOctreeDebug::processGroup(LLViewerOctreeGroup* group)
{
#if 0
    const LLVector4a* vec4 = group->getBounds();
    LLVector3 vec[2];
    vec[0].set(vec4[0].getF32ptr());
    vec[1].set(vec4[1].getF32ptr());
    LL_INFOS() << "Bounds: " << vec[0] << " : " << vec[1] << LL_ENDL;

    vec4 = group->getExtents();
    vec[0].set(vec4[0].getF32ptr());
    vec[1].set(vec4[1].getF32ptr());
    LL_INFOS() << "Extents: " << vec[0] << " : " << vec[1] << LL_ENDL;

    vec4 = group->getObjectBounds();
    vec[0].set(vec4[0].getF32ptr());
    vec[1].set(vec4[1].getF32ptr());
    LL_INFOS() << "ObjectBounds: " << vec[0] << " : " << vec[1] << LL_ENDL;

    vec4 = group->getObjectExtents();
    vec[0].set(vec4[0].getF32ptr());
    vec[1].set(vec4[1].getF32ptr());
    LL_INFOS() << "ObjectExtents: " << vec[0] << " : " << vec[1] << LL_ENDL;
#endif
}
//--------------------------------------------------------------
