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
#include "rkcullshadow.h"
#include <deque>
#if LL_WINDOWS
#include <intrin.h>
#endif
#include <array>
#include <unordered_map>
#include "llviewerregion.h"
#include "pipeline.h"
#include "llviewercontrol.h"
#include "llappviewer.h"
#include "rkshaderprogram.h"
#include "llviewershadermgr.h"
#include "lldrawpoolwater.h"
#include "llspatialpartition.h"
#include "llviewertexturelist.h"
#include "rkviewertexturerecs.h"

//-----------------------------------------------------------------------------------
//static variables definitions
//-----------------------------------------------------------------------------------
U32 LLViewerOctreeEntryData::sCurVisible = 10; //reserve the low numbers for special use.

// 2026-11-18 (inventory item 4 of doc/vulkan/PLAN-visible-set-owner.md): the stamp counter used to be a counter of its own, ticked once a display after the geometry update, that happened to keep in step with gFrameCount (the
// display's own counter) at an offset of 10 (the reserved low numbers); a reader that compared a stamp with gFrameCount, or measured seconds a frame on one and applied it to the other, was right only while the two stayed
// in step. It follows gFrameCount now: the same tick site, and the value is gFrameCount + 10 (never less than the old value + 1: it only goes up), so the offset is a constant by construction. The counters of the idle
// loop (LLFrameTimer, which runs ahead while the login screen draws) and of the image deletion (RKImageTex) stay what they are: timers, not stamps.
void LLViewerOctreeEntryData::incrementVisible()
{
    sCurVisible = std::max(sCurVisible + 1, (U32)gFrameCount + 10u);
}

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
    RKCullShadow::forget(this);         // <FS> S7 C1b: the slot is freed
    RKGroupRows::release(mRowId, DEAD);     // <FS> C1b: the row goes back (a stale pointer to this group reads "dead")
}

LLViewerOctreeGroup::LLViewerOctreeGroup(OctreeNode* node)
:   mOctreeNode(node)
{
    mRowId = RKGroupRows::acquire(this);    // <FS> C1b: a cleared row: stamps 0, state CLEAN, boxes 0
    if (!mRowId)
    {
        LL_ERRS() << "the group table is full (" << RKGroupRows::kCapacity << " groups)" << LL_ENDL;
    }
    static_assert(RKGroupRows::kCameras == LLViewerCamera::NUM_CAMERAS, "the group row holds one stamp per camera");

    row().bounds[0] = node->getCenter();
    row().bounds[1] = node->getSize();

    mOctreeNode->addListener(this);
    RKCullShadow::added(this);          // <FS> S7 C1b
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

void LLViewerOctreeGroup::bumpPartitionEpoch() const
{
    if (LLViewerOctreePartition* part = getOctreePartition())
    {
        part->mEpoch.fetch_add(1, std::memory_order_relaxed);
    }
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
    bumpPartitionEpoch();               // <FS> C4.3
    RKCullShadow::touch(this);          // <FS> S7 C1b: the bounds will change at the next rebound

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
            group->bumpPartitionEpoch();    // <FS> C4.3
            RKCullShadow::touch(group);
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
        row().bounds[0] = group->row().bounds[0];
        row().bounds[1] = group->row().bounds[1];
        row().extents[0] = group->row().extents[0];
        row().extents[1] = group->row().extents[1];

        if (!group->hasState(SKIP_FRUSTUM_CHECK))
        {
            RKCullShadow::touch(group);     // <FS> S7 C1b: the flag shows in its row
        }
        group->setState(SKIP_FRUSTUM_CHECK);
    }
    else if (mOctreeNode->getChildCount() == 0)
    { //copy object bounding box if this is a leaf
        boundObjects(true, row().extents[0], row().extents[1]);
        row().bounds[0] = row().objectBounds[0];
        row().bounds[1] = row().objectBounds[1];
    }
    else
    {
        LLVector4a& newMin = row().extents[0];
        LLVector4a& newMax = row().extents[1];
        LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) mOctreeNode->getChild(0)->getListener(0);
        if (group->hasState(SKIP_FRUSTUM_CHECK))
        {
            RKCullShadow::touch(group);     // <FS> S7 C1b
        }
        group->clearState(SKIP_FRUSTUM_CHECK);
        group->rebound();
        //initialize to first child
        newMin = group->row().extents[0];
        newMax = group->row().extents[1];

        //first, rebound children
        for (U32 i = 1; i < mOctreeNode->getChildCount(); i++)
        {
            group = (LLViewerOctreeGroup*) mOctreeNode->getChild(i)->getListener(0);
            if (group->hasState(SKIP_FRUSTUM_CHECK))
            {
                RKCullShadow::touch(group);     // <FS> S7 C1b
            }
            group->clearState(SKIP_FRUSTUM_CHECK);
            group->rebound();
            const LLVector4a& max = group->row().extents[1];
            const LLVector4a& min = group->row().extents[0];

            newMax.setMax(newMax, max);
            newMin.setMin(newMin, min);
        }

        boundObjects(false, newMin, newMax);

        row().bounds[0].setAdd(newMin, newMax);
        row().bounds[0].mul(0.5f);
        row().bounds[1].setSub(newMax, newMin);
        row().bounds[1].mul(0.5f);
    }

    clearState(DIRTY);
    bumpPartitionEpoch();               // <FS> C4.3: the bounds are new (a result computed while this group was stale must not be kept under the final epoch)

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
    bumpPartitionEpoch();               // <FS> C4.3
    RKCullShadow::touch(this);          // <FS> S7 C1b: the row shows it dead until the slot is freed
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

//virtual
void LLViewerOctreeGroup::handleParentChange(const OctreeNode* node)
{
    bumpPartitionEpoch();               // <FS> C4.3
    RKCullShadow::touch(this);          // <FS> S7 C1b: the row names the parent
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

    LLVector4a& newMin = row().objectExtents[0];
    LLVector4a& newMax = row().objectExtents[1];

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

        row().objectBounds[0].setAdd(newMin, newMax);
        row().objectBounds[0].mul(0.5f);
        row().objectBounds[1].setSub(newMax, newMin);
        row().objectBounds[1].mul(0.5f);
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
    return getVisible(LLViewerCamera::sCurCameraID) >= LLViewerOctreeEntryData::getCurrentFrame();
}

//virtual
bool LLViewerOctreeGroup::isRecentlyVisible() const
{
    return false;
}

void LLViewerOctreeGroup::setVisible()
{
    row().visible[LLViewerCamera::sCurCameraID] = LLViewerOctreeEntryData::getCurrentFrame();

    if(LLViewerCamera::sCurCameraID < LLViewerCamera::CAMERA_WATER0)
    {
        row().anyVisible = LLViewerOctreeEntryData::getCurrentFrame();
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
// <FS> S7 H3: the Hi-Z targets by id, the boxes of the pass being collected, and the batches sent whose results have not been applied yet
namespace
{
std::unordered_map<U32, RKHizTarget*> sHizTargets;
U32 sHizNextId = 0;
U32 sHizBatchSeq = 0;
std::vector<RKVkBridge::HizBox> sHizBoxes;
std::vector<RKHizTarget*> sHizBoxTargets;
struct HizPending { U32 mSerial; U32 mFrame; std::vector<U32> mIds; F32 mEye[3] = {}; F32 mAt[3] = {}; };     // mEye, mAt: the camera the boxes were tested with (plan 8.38)
std::deque<HizPending> sHizPending;
}

RKHizTarget::~RKHizTarget()
{
    if (mHizId)
    {
        sHizTargets.erase(mHizId);
    }
}

RKHizTarget* RKHiz::sWatch = nullptr;

void RKHiz::begin()
{
    sHizBoxes.clear();
    sHizBoxTargets.clear();
    ++sHizBatchSeq;
}

void RKHiz::add(RKHizTarget* target, const LLVector4a& center, F32 half_x, F32 half_y, F32 half_z, bool water)
{
    if (target->mHizBatch == sHizBatchSeq)
    {
        return;         // a target is in a batch once (the occlusion list can name a group twice)
    }
    target->mHizBatch = sHizBatchSeq;
    if (!target->mHizId)
    {
        target->mHizId = ++sHizNextId;
        sHizTargets[target->mHizId] = target;
    }
    RKVkBridge::HizBox box;
    box.mCenter[0] = center[0]; box.mCenter[1] = center[1]; box.mCenter[2] = center[2];
    box.mFlags = water ? 1u : 0u;
    box.mHalf[0] = half_x; box.mHalf[1] = half_y; box.mHalf[2] = half_z;
    box.mPad = 0.f;
    if (target == sWatch)
    {
        RKVkBridge::sWatchBox0.store(box.mCenter[0]);
        RKVkBridge::sWatchBox1.store(box.mCenter[1]);
        RKVkBridge::sWatchBox2.store(box.mCenter[2]);
        RKVkBridge::sWatchBoxOn.store(1);
    }
    sHizBoxes.push_back(box);
    sHizBoxTargets.push_back(target);
}

void RKHiz::send(const F32* view_proj)
{
    if (sHizBoxes.empty())
    {
        return;
    }
    const U32 serial = RKVkBridge::hizSend(sHizBoxes.data(), (U32)sHizBoxes.size(), view_proj);
    if (!serial)
    {
        return;         // nothing was sent: the targets are not awaiting anything, the next pass adds them again
    }
    LLTrace::add(sOcclusionQueries, (S32)sHizBoxes.size());
    HizPending p;
    p.mSerial = serial;
    p.mFrame = (U32)gFrameCount;
    {
        const LLViewerCamera* cam = LLViewerCamera::getInstance();
        const LLVector3& eye = cam->getOrigin();
        const LLVector3& at = cam->getAtAxis();
        for (int a = 0; a < 3; ++a) { p.mEye[a] = eye.mV[a]; p.mAt[a] = at.mV[a]; }
    }
    p.mIds.reserve(sHizBoxTargets.size());
    for (RKHizTarget* t : sHizBoxTargets)
    {
        p.mIds.push_back(t->mHizId);
        t->mHizSerial = serial;
    }
    sHizPending.push_back(std::move(p));
}

namespace
{
const bool sHizKeepVerdicts = getenv("RK_VK_BLINK") != nullptr;         // (diagnostic: the last verdict of every target, for the blink detector)
std::unordered_map<U32, std::array<U32, 5>> sHizLastVerdict;
}

bool RKHiz::lastVerdict(const RKHizTarget* target, U32* out5)
{
    if (!sHizKeepVerdicts || !target->mHizId)
    {
        return false;
    }
    auto it = sHizLastVerdict.find(target->mHizId);
    if (it == sHizLastVerdict.end())
    {
        return false;
    }
    for (int k = 0; k < 5; ++k)
    {
        out5[k] = it->second[k];
    }
    return true;
}

void RKHiz::apply()
{
    if (sHizPending.empty())
    {
        return;
    }
    static std::vector<U32> words;
    while (!sHizPending.empty())
    {
        HizPending& p = sHizPending.front();
        const size_t all_boxes = p.mIds.size();
        words.resize(3 * all_boxes);
        S32 r = RKVkBridge::hizRead(p.mSerial, (U32)all_boxes, words.data());
        if (r == 0)
        {
            // (2026-11-17: the worker takes the records in order: one that a later record has been published for is lost, not late. It held every verdict behind it for up to 60 frames)
            bool later_published = false;
            for (size_t k = 1; k < sHizPending.size() && !later_published; ++k)
            {
                const HizPending& q = sHizPending[k];
                static std::vector<U32> probe;
                const size_t q_boxes = q.mIds.size();
                probe.resize(3 * q_boxes);
                later_published = RKVkBridge::hizRead(q.mSerial, (U32)q_boxes, probe.data()) == 1;
            }
            if (later_published || (U32)gFrameCount - p.mFrame > 60)
            {
                r = -1;         // never published (the worker could not run the record)
            }
            else
            {
                break;
            }
        }
        if (r < 0)
        {
            for (U32 id : p.mIds)
            {
                auto it = sHizTargets.find(id);
                if (it != sHizTargets.end() && it->second->mHizSerial == p.mSerial)
                {
                    it->second->mHizSerial = 0;
                }
            }
            RKVkBridge::hizDone(p.mSerial);
            sHizPending.pop_front();
            continue;
        }
        // <FS> plan 8.38: a verdict is only good for the camera it was tested with. The results arrive 2 to 4 frames after the boxes were sent; while the camera moves fast (a zoom, a flight through foliage)
        // an occluded verdict that was right for its frame is applied where the camera is now, and the group vanishes for a few frames (the own avatar for 15 frames, 2026-11-02). A batch whose camera has
        // moved more than RK_VK_HIZ_STALE_M metres (0.5) or turned more than RK_VK_HIZ_STALE_DEG degrees (2) since is stale: its occluded verdicts count as visible (its visible ones are always safe).
        bool stale = false;
        {
            static const F32 stale_m = getenv("RK_VK_HIZ_STALE_M") ? (F32)atof(getenv("RK_VK_HIZ_STALE_M")) : 0.5f;
            static const F32 stale_cos = cosf((getenv("RK_VK_HIZ_STALE_DEG") ? (F32)atof(getenv("RK_VK_HIZ_STALE_DEG")) : 2.f) * 3.14159265f / 180.f);
            const LLViewerCamera* cam = LLViewerCamera::getInstance();
            const LLVector3& eye = cam->getOrigin();
            const LLVector3& at = cam->getAtAxis();
            const F32 dx = eye.mV[0] - p.mEye[0], dy = eye.mV[1] - p.mEye[1], dz = eye.mV[2] - p.mEye[2];
            const F32 dot = at.mV[0] * p.mAt[0] + at.mV[1] * p.mAt[1] + at.mV[2] * p.mAt[2];
            stale = (dx * dx + dy * dy + dz * dz > stale_m * stale_m) || dot < stale_cos;
        }
        for (size_t i = 0; i < p.mIds.size(); ++i)
        {
            auto it = sHizTargets.find(p.mIds[i]);
            if (it == sHizTargets.end())
            {
                continue;
            }
            RKHizTarget* t = it->second;
            if (t->mHizSerial != p.mSerial)
            {
                continue;       // a later box of the target is the one awaited (it was discarded and added again)
            }
            t->mHizSerial = 0;
            const U32 word = words[3 * i];
            const bool occluded = (word & 1u) != 0 && !stale;
            if (t == sWatch)
            {
                // the watcher: one line when the shader's answer or the stale flag changes (the numbers behind the answer: the box's nearest depth against the farthest depth of the pyramid texels it covers; reversed depth, 1 at the near plane)
                static U32 last_key = ~0u;
                const U32 key = (word & 1u) | (stale ? 2u : 0u);
                if (key != last_key)
                {
                    last_key = key;
                    F32 minz, maxd;
                    memcpy(&minz, &words[3 * i + 1], sizeof(F32));
                    memcpy(&maxd, &words[3 * i + 2], sizeof(F32));
                    const LLViewerCamera* wcam = LLViewerCamera::getInstance();
                    const LLVector3 d = wcam->getOrigin() - LLVector3(p.mEye);
                    const F32 turned = acosf(llclamp(wcam->getAtAxis() * LLVector3(p.mAt), -1.f, 1.f)) * RAD_TO_DEG;
                    LL_INFOS("VkWatch") << "viewer, frame " << gFrameCount << ", Hi-Z verdict for the watched group (boxes sent in frame " << p.mFrame << "): the shader says " << ((word & 1u) ? "OCCLUDED" : "visible") << ", word " << word
                        << " (pyramid level " << ((word >> 8) & 0xFFu) << ", special case bits " << (word >> 16) << "), the box's nearest depth " << minz << ", the farthest depth of the pyramid texels it covers " << maxd
                        << ", stale " << (stale ? 1 : 0) << " (camera moved " << d.length() << " m, turned " << turned << " deg since the boxes were sent), the group's occluded state before " << (t->mHizOccludedOnce ? "one occluded verdict waiting" : "none waiting") << LL_ENDL;
                }
            }
            if (sHizKeepVerdicts)
            {
                sHizLastVerdict[p.mIds[i]] = { word, words[3 * i + 1], words[3 * i + 2], (U32)gFrameCount, stale ? 1u : 0u };
            }
            static const bool ignore = getenv("RK_VK_HIZ_IGNORE") != nullptr;       // (a test: the verdicts are computed and not used)
            t->hizResult(ignore ? false : occluded);
        }
        RKVkBridge::hizDone(p.mSerial);
        sHizPending.pop_front();
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


namespace
{
// <FS> one bit per group row: the world camera's OCCLUDED state is set (see LLOcclusionCullingGroup::worldOccluded)
std::atomic<U64> sWorldOccBits[RKGroupRows::kCapacity / 64];
inline void world_occ_bit(U32 row_id, bool on)
{
    if (row_id && row_id < RKGroupRows::kCapacity)
    {
        const U64 bit = 1ull << (row_id & 63u);
        if (on) { sWorldOccBits[row_id >> 6].fetch_or(bit, std::memory_order_relaxed); }
        else { sWorldOccBits[row_id >> 6].fetch_and(~bit, std::memory_order_relaxed); }
    }
}
}

void LLOcclusionCullingGroup::worldOccluded(std::vector<LLOcclusionCullingGroup*>& out)
{
    out.clear();
    if (!RKGroupRows::sChunks)
    {
        return;
    }
    for (U32 w = 0; w < RKGroupRows::kCapacity / 64; ++w)
    {
        U64 bits = sWorldOccBits[w].load(std::memory_order_relaxed);
        while (bits)
        {
            unsigned long b;
#if LL_WINDOWS
            _BitScanForward64(&b, bits);
#else
            b = (unsigned long)__builtin_ctzll(bits);
#endif
            bits &= bits - 1;
            const U32 id = (w << 6) + (U32)b;
            RKGroupRows::Row* chunk = RKGroupRows::sChunks[id >> RKGroupRows::kChunkShift];
            const void* owner = chunk ? chunk[id & (RKGroupRows::kChunkRows - 1)].owner : nullptr;
            if (!owner || !(chunk[id & (RKGroupRows::kChunkRows - 1)].occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
            {
                world_occ_bit(id, false);       // (a group that is gone, a row handed out again, a state cleared where nothing noted it)
                continue;
            }
            out.push_back(static_cast<LLOcclusionCullingGroup*>(const_cast<LLViewerOctreeGroup*>(static_cast<const LLViewerOctreeGroup*>(owner))));
        }
    }
}

LLOcclusionCullingGroup::LLOcclusionCullingGroup(OctreeNode* node, LLViewerOctreePartition* part) :
    LLViewerOctreeGroup(node),
    mSpatialPartition(part)
{
    part->mLODSeed = (part->mLODSeed+1)%part->mLODPeriod;
    row().lodHash = part->mLODSeed;

    OctreeNode* oct_parent = node->getOctParent();
    LLOcclusionCullingGroup* parent = oct_parent ? (LLOcclusionCullingGroup*) oct_parent->getListener(0) : NULL;

    for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
    {
        row().occlusion[i] = parent ? SG_STATE_INHERIT_MASK & parent->row().occlusion[i] : 0;
        row().visible[i] = 0;
    }
    world_occ_bit(mRowId, (row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED) != 0);
}

LLOcclusionCullingGroup::~LLOcclusionCullingGroup()
{
}

bool LLOcclusionCullingGroup::needsUpdate()
{
    return LLDrawable::getCurrentFrame() % mSpatialPartition->mLODPeriod == row().lodHash;
}

bool LLOcclusionCullingGroup::isRecentlyVisible() const
{
    const S32 MIN_VIS_FRAME_RANGE = 2;
    return (LLDrawable::getCurrentFrame() - getVisible(LLViewerCamera::sCurCameraID)) < MIN_VIS_FRAME_RANGE ;
}

bool LLOcclusionCullingGroup::isAnyRecentlyVisible() const
{
    const S32 MIN_VIS_FRAME_RANGE = 2;
    return (LLDrawable::getCurrentFrame() - getAnyVisible()) < MIN_VIS_FRAME_RANGE ;
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

void LLOcclusionCullingGroup::setOcclusionState(U32 state, S32 mode /* = STATE_MODE_SINGLE */ )
{
    switch (mode)
    {
    case STATE_MODE_SINGLE:
        if (state & OCCLUDED)
        {
            add(sNumObjectsOccluded, 1);
        }
        if ((state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !(row().occlusion[LLViewerCamera::sCurCameraID] & OCCLUDED))
        {
            RKCullShadow::touch(this);      // <FS> S7 C1b
        }
        if (mSpatialPartition && (state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !(row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
        {
            RKTexRecs::groupOcclusion(static_cast<LLSpatialGroup*>(this), true);       // <FS> H6: the texture records of its faces are filled again
        }
        row().occlusion[LLViewerCamera::sCurCameraID] |= state;
        if ((state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD)
        {
            world_occ_bit(mRowId, true);
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
        if ((state & OCCLUDED) && !(row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
        {
            RKCullShadow::touch(this);      // <FS> S7 C1b
        }
        if (mSpatialPartition && (state & OCCLUDED) && !(row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
        {
            RKTexRecs::groupOcclusion(static_cast<LLSpatialGroup*>(this), true);       // <FS> H6
        }
        for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
        {
            row().occlusion[i] |= state;
        }
        if (state & OCCLUDED)
        {
            world_occ_bit(mRowId, true);
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
            if ((state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && (row().occlusion[LLViewerCamera::sCurCameraID] & OCCLUDED))
            {
                RKCullShadow::touch(this);  // <FS> S7 C1b
            }
            if (mSpatialPartition && (state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && (row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
            {
                LLViewerTextureList::wakeGroupTextures(static_cast<LLSpatialGroup*>(this));     // <FS> H6: it came into view: its textures are looked at again
                RKTexRecs::groupOcclusion(static_cast<LLSpatialGroup*>(this), false);
            }
            row().occlusion[LLViewerCamera::sCurCameraID] &= ~state;
            if ((state & OCCLUDED) && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD)
            {
                world_occ_bit(mRowId, false);
            }
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
            if ((state & OCCLUDED) && (row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
            {
                RKCullShadow::touch(this);  // <FS> S7 C1b
            }
            if (mSpatialPartition && (state & OCCLUDED) && (row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED))
            {
                LLViewerTextureList::wakeGroupTextures(static_cast<LLSpatialGroup*>(this));     // <FS> H6
                RKTexRecs::groupOcclusion(static_cast<LLSpatialGroup*>(this), false);
            }
            for (U32 i = 0; i < LLViewerCamera::NUM_CAMERAS; i++)
            {
                row().occlusion[i] &= ~state;
            }
            if (state & OCCLUDED)
            {
                world_occ_bit(mRowId, false);
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

// <FS> 2026-11-27: all or nothing, visible wins. A group that is visible has no occluded group above it: the viewer marks a subtree with the state of its top, and the worker prunes everything under an occluded
// node whatever the state of the nodes below, so a visible group under an occluded one is listed by the viewer and never drawn. Every place that makes a group visible calls this: the top-most occluded group
// above it, and everything under that, become visible together (the groups above ask for their next verdict as top-most groups and decide again). A visible verdict of a group that arrived after the verdict that
// occluded its parent (they come 1 to 4 frames late) left exactly that state, and nothing marked the group again, because its parent already had the bit.
static void rk_hiz_visible_above(LLOcclusionCullingGroup* g)
{
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
    LLOcclusionCullingGroup* top = nullptr;
    for (LLViewerOctreeGroup* a = g->getParent(); a; a = a->getParent())
    {
        LLOcclusionCullingGroup* o = static_cast<LLOcclusionCullingGroup*>(a);
        if (o->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
        {
            top = o;
        }
        o->mHizOccludedOnce = false;
    }
    if (top)
    {
        top->clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
    }
    LLViewerCamera::sCurCameraID = saved_camera;
}

void LLOcclusionCullingGroup::checkOcclusion()
{
    if (LLPipeline::useOcclusion() < 2) return;  // 0 - NoOcclusion, 1 = ReadOnly, 2 = ModifyOcclusionState  TODO: DJH 11-2021 ENUM this

    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    LLOcclusionCullingGroup* parent = (LLOcclusionCullingGroup*)getParent();
    if (parent && parent->isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
    {   //if the parent has been marked as occluded, the child is implicitly occluded: no verdict of its own is awaited
        mHizSerial = 0;
        mHizStale = false;
        return;
    }

    if (mHizStale && mHizSerial)
    {   // a verdict is awaited and the group asked to be discarded (an object was added): visible until the next box has a result
        clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        rk_hiz_visible_above(this);
        mHizSerial = 0;
        mHizStale = false;
    }
}

void LLOcclusionCullingGroup::hizCollect(LLCamera* camera, const LLVector4a* shift)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_OCTREE;
    if (!(mSpatialPartition->isOcclusionEnabled() && LLPipeline::useOcclusion() > 1))
    {
        return;
    }
    // <FS> 2026-11-20: the root of an octree is never occlusion culled (the walk's earlyFail says so, and the re-test of held groups skips it). A verdict for its box hid the whole partition behind a wall: two agreeing
    // "occluded" verdicts marked the root and, through the subtree state, every group under it, and nobody asked again (the root is skipped, the groups under it are skipped because their parent is occluded): the Linden
    // trees of the house's garden stayed gone after the camera came out, until an object was added (hizEntryMoved clears the ancestors). The root asks for no box and holds no occluded state.
    if (!mOctreeNode || !mOctreeNode->getParent())
    {
        mHizSerial = 0;
        mHizStale = false;
        mHizOccludedOnce = false;
        if (isOcclusionState(LLOcclusionCullingGroup::OCCLUDED))
        {
            clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        }
        return;
    }
    //move row().bounds to the agent space if necessary
    LLVector4a bounds[2];
    bounds[0] = row().bounds[0];
    bounds[1] = row().bounds[1];
    if (shift != NULL)
    {
        bounds[0].add(*shift);
    }

    F32 OCCLUSION_FUDGE_Z = SG_OCCLUSION_FUDGE; //<-- #Solution #2
    if (LLPipeline::RENDER_TYPE_VOIDWATER == mSpatialPartition->mDrawableType)
    {
        OCCLUSION_FUDGE_Z = 1.;
    }

    if (earlyFail(camera, bounds))
    {   // the camera is inside the box: nothing to test, visible
        mHizSerial = 0;
        mHizStale = true;
        clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        rk_hiz_visible_above(this);
        return;
    }
    if (mHizSerial && !mHizStale)
    {
        return;         // a verdict is awaited
    }
    mHizStale = false;
    // Water is depth clamped (as the cubes were) so it is not rejected for being behind the far clip plane
    const bool water = mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_WATER || mSpatialPartition->mDrawableType == LLPipeline::RENDER_TYPE_VOIDWATER;
    RKHiz::add(this, bounds[0], bounds[1][0] + SG_OCCLUSION_FUDGE, bounds[1][1] + SG_OCCLUSION_FUDGE, bounds[1][2] + OCCLUSION_FUDGE_Z, water);
}

void LLOcclusionCullingGroup::hizEntryMoved(const LLVector4a* entry_extents)
{
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
    // <FS> 2026-11-27: all or nothing, visible wins. An object that is added or moved into this group has no verdict of its own, so nothing that holds an occluded state above it may hide it: the top-most
    // occluded group of the chain and everything under it (the viewer marks a subtree with the state of its top, and the worker prunes everything under an occluded node) become visible together, until two
    // agreeing verdicts decide again. Judged node by node, a leaf could be cleared under an ancestor that stayed occluded (the viewer listed it, the worker pruned it), and a sibling kept its mark when the
    // group above it was cleared.
    LLOcclusionCullingGroup* top = nullptr;
    for (LLViewerOctreeGroup* g = this; g; g = g->getParent())
    {
        LLOcclusionCullingGroup* o = static_cast<LLOcclusionCullingGroup*>(g);
        if (o->row().occlusion[LLViewerCamera::CAMERA_WORLD] & OCCLUDED)
        {
            top = o;
        }
        o->mHizOccludedOnce = false;
    }
    if (top)
    {
        top->clearOcclusionState(OCCLUDED, STATE_MODE_DIFF);
    }
    LLViewerCamera::sCurCameraID = saved_camera;
}

//virtual
void LLOcclusionCullingGroup::hizResult(bool occluded)
{
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;        // the occlusion state arrays are per camera, the boxes were the world camera's
    if (occluded && (!mOctreeNode || !mOctreeNode->getParent()))
    {
        occluded = false;       // (the root is never occlusion culled, see hizCollect: a verdict still in flight from before is read as visible)
    }
    if (occluded)
    {
        // <FS> plan 8.38: occluded on two agreeing verdicts, so one wrong or stale answer never hides the group; visible on one
        if (mHizOccludedOnce)
        {
            setOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        }
        mHizOccludedOnce = true;
    }
    else
    {
        mHizOccludedOnce = false;
        clearOcclusionState(LLOcclusionCullingGroup::OCCLUDED, LLOcclusionCullingGroup::STATE_MODE_DIFF);
        rk_hiz_visible_above(this);
    }
    LLViewerCamera::sCurCameraID = saved_camera;
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

    static std::atomic<U32> sNextPartitionId{ 0 };
    mId = ++sNextPartitionId;           // <FS> C4.3
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
    return mOcclusionEnabled || LLPipeline::useOcclusion() > 2;
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
void LLViewerOctreeCull::traverseTop(const OctreeNode* n, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const SplitTask&)>& on_task)
{
    LLViewerOctreeGroup* group = (LLViewerOctreeGroup*) n->getListener(0);

    if (earlyFail(group))
    {
        return;
    }

    // `transparent`: the node is wholly inside by the flag of the node and not by its parent's verdict (mRes 1): what mRes is after its subtree depends on what its children do, so it cannot be a task.
    // Its own work is done here and its children are looked at as if it were not a level (a chain of such nodes is how the root of a sparse octree looks).
    auto descend = [&](bool transparent)
    {
        if (!transparent && (levels <= 0 || !descend_more(n)))
        {
            on_task({ n, mRes });
            return;
        }
        n->accept(this);
        for (U32 i = 0; i < n->getChildCount(); i++)
        {
            traverseTop(n->getChild(i), transparent ? levels : levels - 1, descend_more, on_task);
        }
    };

    if (mRes == 2 || (mRes && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)))
    {
        descend(mRes != 2);
    }
    else
    {
        mRes = frustumCheck(group);
        if (mRes)
        {
            descend(false);
        }
        mRes = 0;
    }
}

void LLViewerOctreeCull::traverseTask(const SplitTask& task)
{
    mRes = task.mRes;
    OctreeTraveler::traverse(task.mNode);
}

S32 LLViewerOctreeCull::AABBInFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustumNoFarClip(group->row().bounds[0], group->row().bounds[1]);
}

// <FS> 2026-11-26: the far limit of the world cull. AABBSphereIntersect answers 2 ("wholly in", the walk then takes every node below it without a test) when the box's MINIMUM and MAXIMUM corners are inside the sphere,
// but those two are not always the farthest: a node can be called wholly in with children well past the view distance, and those were drawn (the far plane of the projection is 1024, nothing else clips them). 2 is now
// given only when the farthest corner is inside as well; anything that touches the sphere is still in (1, its children are tested one by one), so a group that crosses the view distance is still taken and drawn
// whole, and one that lies wholly beyond it is not. The worker's copy, cull_sphere_res (rkvkworker.cpp), must stay the same.
static S32 rk_sphere_whole_only_if_farthest_corner_in(const LLVector4a& min, const LLVector4a& max, const LLVector3& origin, F32 radius)
{
    const S32 res = AABBSphereIntersect(min, max, origin, radius);
    if (res != 2)
    {
        return res;
    }
    F32 farthest2 = 0.f;
    for (U32 i = 0; i < 3; ++i)
    {
        const F32 t = llmax(fabsf(min[i] - origin.mV[i]), fabsf(max[i] - origin.mV[i]));
        farthest2 += t * t;
    }
    return farthest2 < radius * radius ? 2 : 1;
}

S32 LLViewerOctreeCull::AABBSphereIntersectGroupExtents(const LLViewerOctreeGroup* group)
{
    return rk_sphere_whole_only_if_farthest_corner_in(group->row().extents[0], group->row().extents[1], mCamera->getOrigin(), mCamera->mFrustumCornerDist);
}

S32 LLViewerOctreeCull::AABBInFrustumGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustum(group->row().bounds[0], group->row().bounds[1]);
}
//------------------------------------------

//------------------------------------------
//agent space object set culling
S32 LLViewerOctreeCull::AABBInFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustumNoFarClip(group->row().objectBounds[0], group->row().objectBounds[1]);
}

S32 LLViewerOctreeCull::AABBSphereIntersectObjectExtents(const LLViewerOctreeGroup* group)
{
    return rk_sphere_whole_only_if_farthest_corner_in(group->row().objectExtents[0], group->row().objectExtents[1], mCamera->getOrigin(), mCamera->mFrustumCornerDist);
}

S32 LLViewerOctreeCull::AABBInFrustumObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInFrustum(group->row().objectBounds[0], group->row().objectBounds[1]);
}
//------------------------------------------

//------------------------------------------
//local regional space group culling
S32 LLViewerOctreeCull::AABBInRegionFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustumNoFarClip(group->row().bounds[0], group->row().bounds[1]);
}

S32 LLViewerOctreeCull::AABBInRegionFrustumGroupBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustum(group->row().bounds[0], group->row().bounds[1]);
}

S32 LLViewerOctreeCull::AABBRegionSphereIntersectGroupExtents(const LLViewerOctreeGroup* group, const LLVector3& shift)
{
    return AABBSphereIntersect(group->row().extents[0], group->row().extents[1], mCamera->getOrigin() - shift, mCamera->mFrustumCornerDist);
}
//------------------------------------------

//------------------------------------------
//local regional space object culling
S32 LLViewerOctreeCull::AABBInRegionFrustumObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustum(group->row().objectBounds[0], group->row().objectBounds[1]);
}

S32 LLViewerOctreeCull::AABBInRegionFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group)
{
    return mCamera->AABBInRegionFrustumNoFarClip(group->row().objectBounds[0], group->row().objectBounds[1]);
}

S32 LLViewerOctreeCull::AABBRegionSphereIntersectObjectExtents(const LLViewerOctreeGroup* group, const LLVector3& shift)
{
    return AABBSphereIntersect(group->row().objectExtents[0], group->row().objectExtents[1], mCamera->getOrigin() - shift, mCamera->mFrustumCornerDist);
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
