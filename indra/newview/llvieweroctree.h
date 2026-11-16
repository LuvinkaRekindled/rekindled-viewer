/**
 * @file llvieweroctree.h
 * @brief LLViewerObjectOctree.cpp header file, defining all supporting classes.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_VIEWEROCTREE_H
#define LL_VIEWEROCTREE_H

#include <atomic>
#include <functional>
#include <vector>
#include <map>

#include "v2math.h"
#include "v3math.h"
#include "v4math.h"
#include "m4math.h"
#include "llvector4a.h"
#include "llquaternion.h"
#include "lloctree.h"
#include "llviewercamera.h"
#include "llgrouptable.h"       // <FS> C1b: the group's boxes, state word and per-camera stamps are one row of the group table
#include "llhiz.h"
#include "llvkbridge.h"

class LLViewerRegion;
class LLViewerOctreeEntryData;
class LLViewerOctreeGroup;
class LLViewerOctreeEntry;
class LLViewerOctreePartition;

typedef LLOctreeListener<LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry>> OctreeListener;
typedef LLTreeNode<LLViewerOctreeEntry> TreeNode;
typedef LLOctreeNode<LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry>> OctreeNode;
typedef LLOctreeRoot<LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry>> OctreeRoot;
typedef LLOctreeTraveler<LLViewerOctreeEntry, LLPointer<LLViewerOctreeEntry>> OctreeTraveler;

#if LL_OCTREE_PARANOIA_CHECK
#define assert_octree_valid(x) x->validate()
#define assert_states_valid(x) ((LLViewerOctreeGroup*) x->mSpatialPartition->mOctree->getListener(0))->checkStates()
#else
#define assert_octree_valid(x)
#define assert_states_valid(x)
#endif

// get index buffer for binary encoded axis vertex buffer given a box at center being viewed by given camera
U32 get_box_fan_indices(LLCamera* camera, const LLVector4a& center);
U8* get_box_fan_indices_ptr(LLCamera* camera, const LLVector4a& center);

S32 AABBSphereIntersect(const LLVector4a& min, const LLVector4a& max, const LLVector3 &origin, const F32 &rad);
S32 AABBSphereIntersectR2(const LLVector4a& min, const LLVector4a& max, const LLVector3 &origin, const F32 &radius_squared);

S32 AABBSphereIntersect(const LLVector3& min, const LLVector3& max, const LLVector3 &origin, const F32 &rad);
S32 AABBSphereIntersectR2(const LLVector3& min, const LLVector3& max, const LLVector3 &origin, const F32 &radius_squared);

//defines data needed for octree of an entry
//LL_ALIGN_PREFIX(16)
class LLViewerOctreeEntry : public LLRefCount
{
    LL_ALIGN_NEW
    friend class LLViewerOctreeEntryData;

public:
    typedef enum
    {
        LLDRAWABLE = 0,
        LLVOCACHEENTRY,
        NUM_DATA_TYPE
    }eEntryDataType_t;

protected:
    virtual ~LLViewerOctreeEntry();

public:
    LLViewerOctreeEntry();

    void nullGroup(); //called by group handleDestruction() only
    void setGroup(LLViewerOctreeGroup* group);
    void removeData(LLViewerOctreeEntryData* data);

    LLViewerOctreeEntryData* getDrawable() const {return mData[LLDRAWABLE];}
    bool                     hasDrawable() const {return mData[LLDRAWABLE] != NULL;}
    LLViewerOctreeEntryData* getVOCacheEntry() const {return mData[LLVOCACHEENTRY];}
    bool                     hasVOCacheEntry() const {return mData[LLVOCACHEENTRY] != NULL;}

    const LLVector4a* getSpatialExtents() const {return mExtents;}
    const LLVector4a& getPositionGroup() const  {return mPositionGroup;}
    LLViewerOctreeGroup* getGroup()const        {return mGroup;}

    F32  getBinRadius() const                   {return mBinRadius;}
    S32  getBinIndex() const                    {return mBinIndex; }
    void setBinIndex(S32 index) const           {mBinIndex = index; }

private:
    void addData(LLViewerOctreeEntryData* data);

private:
    LLViewerOctreeEntryData*    mData[NUM_DATA_TYPE]; //do not use LLPointer here.
    LLViewerOctreeGroup*        mGroup;

    //aligned members
    LL_ALIGN_16(LLVector4a      mExtents[2]);
    LL_ALIGN_16(LLVector4a      mPositionGroup);
    F32                         mBinRadius;
    mutable S32                 mBinIndex;
    mutable U32                 mVisible;

} ;//LL_ALIGN_POSTFIX(16);

//defines an abstract class for entry data
//LL_ALIGN_PREFIX(16)
class LLViewerOctreeEntryData : public LLRefCount
{
protected:
    virtual ~LLViewerOctreeEntryData();

public:
    LLViewerOctreeEntryData(const LLViewerOctreeEntryData& rhs)
    {
        *this = rhs;
    }
    LLViewerOctreeEntryData(LLViewerOctreeEntry::eEntryDataType_t data_type);

    LLViewerOctreeEntry::eEntryDataType_t getDataType() const {return mDataType;}
    LLViewerOctreeEntry* getEntry() {return mEntry;}

    virtual void setOctreeEntry(LLViewerOctreeEntry* entry);
    void         removeOctreeEntry();

    F32                  getBinRadius() const   {return mEntry->getBinRadius();}
    const LLVector4a*    getSpatialExtents() const;
    LLViewerOctreeGroup* getGroup()const;
    const LLVector4a&    getPositionGroup() const;

    void setBinRadius(F32 rad)  {mEntry->mBinRadius = rad;}
    void setSpatialExtents(const LLVector3& min, const LLVector3& max);
    void setSpatialExtents(const LLVector4a& min, const LLVector4a& max);
    void setPositionGroup(const LLVector4a& pos);

    virtual void setGroup(LLViewerOctreeGroup* group);
    void         shift(const LLVector4a &shift_vector);

    U32          getVisible() const {return mEntry ? mEntry->mVisible : 0;}
    void         setVisible() const;
    void         resetVisible() const;
    virtual bool isVisible() const;
    virtual bool isRecentlyVisible() const;

    static S32 getCurrentFrame() { return sCurVisible; }

protected:
    LLVector4a& getGroupPosition()  {return mEntry->mPositionGroup;}
    void        initVisible(U32 visible) {mEntry->mVisible = visible;}

    static void incrementVisible() {sCurVisible++;}
protected:
    LLPointer<LLViewerOctreeEntry>        mEntry;
    LLViewerOctreeEntry::eEntryDataType_t mDataType;
    static  U32                           sCurVisible; // Counter for what value of mVisible means currently visible
};//LL_ALIGN_POSTFIX(16);


//defines an octree group for an octree node, which contains multiple entries.
//LL_ALIGN_PREFIX(16)
// <FS> C4.5 R2: per retained set (the slot in the rows of its members) the frame its replay last ran: that is the world stamp of a member, so a replay does not write a stamp per group
struct LLRetainedStamps
{
    static inline std::atomic<S32> sFrame[256][9];       // [partition slot][camera]: the frame the replay of the set of that partition for that camera last ran
};

// stage 8b (LL_VK_KSTAMP=1, needs LL_VK_KEPT=2): the world stamp of a group in the worker's kept set is the frame the set was applied, derived from the set's bits by the group's slot (mCullId - 1), not written per group; a group that leaves
// the set has its stamp written back by the pass that applies the next set (the same fold as a group leaving a retained set)
struct LLKeptStamps
{
    struct View
    {
        const U32* mBits = nullptr;     // one bit a slot (ConsumerList::mKept)
        U32 mWords = 0;
        S32 mFrame = 0;
        bool has(U32 slot) const { return (slot >> 5) < mWords && ((mBits[slot >> 5] >> (slot & 31u)) & 1u); }
    };
    static inline std::atomic<const View*> sView{ nullptr };       // the applied set (null: none), read by every thread
};

class LLViewerOctreeGroup
:   public OctreeListener
{
    LL_ALIGN_NEW
    friend class LLViewerOctreeCull;
protected:
    virtual ~LLViewerOctreeGroup();

public:
    enum
    {
        CLEAN              = 0x00000000,
        DIRTY              = 0x00000001,
        OBJECT_DIRTY       = 0x00000002,
        SKIP_FRUSTUM_CHECK = 0x00000004,
        DEAD               = 0x00000008,
        INVALID_STATE      = 0x00000010,
    };

public:
    typedef OctreeNode::element_iter element_iter;
    typedef OctreeNode::element_list element_list;

    LLViewerOctreeGroup(OctreeNode* node);
    LLViewerOctreeGroup(const LLViewerOctreeGroup& rhs) = delete;       // <FS> C1b: a copy would share the row
    LLViewerOctreeGroup& operator=(const LLViewerOctreeGroup& rhs) = delete;

    bool removeFromGroup(LLViewerOctreeEntryData* data);
    bool removeFromGroup(LLViewerOctreeEntry* entry);

    virtual void unbound();
    virtual void rebound();

    bool isDead()                           { return hasState(DEAD); }

    void setVisible();
    bool isVisible() const;
    virtual bool isRecentlyVisible() const;
    // (retainedSlot: bits 0-7 the slot of the group's partition, bit 8 + c set when the group is a member of the replayed set for camera c: the stamp for that camera is the set's frame, not written per group)
    S32  retainedFrame(LLViewerCamera::eCameraID id) const { const U32 r = row().retainedSlot; return (r & (0x100u << id)) ? LLRetainedStamps::sFrame[r & 255u][id].load(std::memory_order_relaxed) : 0; }
    S32  keptFrame() const { const LLKeptStamps::View* k = LLKeptStamps::sView.load(std::memory_order_relaxed); return (k && mCullId && k->has(mCullId - 1)) ? k->mFrame : 0; }
    S32  getVisible(LLViewerCamera::eCameraID id) const
    {
        S32 v = row().visible[id];
        if (row().retainedSlot) { v = std::max(v, retainedFrame(id)); }
        if (id == LLViewerCamera::CAMERA_WORLD) { v = std::max(v, keptFrame()); }
        return v;
    }
    S32  getAnyVisible() const
    {
        S32 v = std::max(row().anyVisible, keptFrame());
        const U32 r = row().retainedSlot;
        if (r)
        {
            for (U32 c = 0; c < LLViewerCamera::CAMERA_WATER0; ++c)
            {
                if (r & (0x100u << c)) { v = std::max(v, LLRetainedStamps::sFrame[r & 255u][c].load(std::memory_order_relaxed)); }
            }
        }
        return v;
    }
    bool isEmpty() const { return mOctreeNode->isEmpty(); }

    U32  getState()                {return row().state; }
    bool isDirty() const           {return row().state & DIRTY;}
    bool hasState(U32 state) const {return row().state & state;}
    void setState(U32 state)       {row().state |= state;}
    void clearState(U32 state)     {row().state &= ~state;}

    // <FS> C1b: this group's row in the group table (what getBounds / getExtents / the stamps read)
    LLGroupRows::Row&   row() const { return LLGroupRows::row(mRowId); }
    U32                 getRowId() const { return mRowId; }
    bool                ownsRow() const { return LLGroupRows::ownedBy(mRowId, this); }

    //LISTENER FUNCTIONS
    virtual void handleInsertion(const TreeNode* node, LLViewerOctreeEntry* obj);
    virtual void handleRemoval(const TreeNode* node, LLViewerOctreeEntry* obj);
    virtual void handleDestruction(const TreeNode* node);
    virtual void handleStateChange(const TreeNode* node);
    virtual void handleChildAddition(const OctreeNode* parent, OctreeNode* child);
    virtual void handleChildRemoval(const OctreeNode* parent, const OctreeNode* child);
    virtual void handleParentChange(const OctreeNode* node);        // <FS> S7 C1b

    OctreeNode*          getOctreeNode() {return mOctreeNode;}

    // <FS> C4.3: the partition this group belongs to (nullptr for a plain group), and the bump of its change epoch: a change of this group's bounds, state or place in the tree that a result retained
    // for the partition (the cascades' visible extents) must not outlive. Bumped when a group goes dirty (unbound), when it is rebounded, on destruction, on a parent change, by shift and expandExtents.
    virtual LLViewerOctreePartition* getOctreePartition() const { return nullptr; }
    void bumpPartitionEpoch() const;
    LLViewerOctreeGroup* getParent();

    const LLVector4a* getBounds() const        {return row().bounds;}
    const LLVector4a* getExtents() const       {return row().extents;}
    const LLVector4a* getObjectBounds() const  {return row().objectBounds;}
    const LLVector4a* getObjectExtents() const {return row().objectExtents;}

    //octree wrappers to make code more readable
    element_iter getDataBegin() { return mOctreeNode->getDataBegin(); }
    element_iter getDataEnd() { return mOctreeNode->getDataEnd(); }
    U32 getElementCount() const { return mOctreeNode->getElementCount(); }
    bool hasElement(LLViewerOctreeEntryData* data);

protected:
    void checkStates();
private:
    virtual bool boundObjects(bool empty, LLVector4a& minOut, LLVector4a& maxOut);

protected:
    U32         mRowId = 0;     // <FS> C1b: the row of this group in the group table (bounds, object bounds, extents, object extents, state, visible stamps, occlusion state, LOD hash)
    OctreeNode* mOctreeNode;

public:
    // <FS> S7 C1 (LL_VK_CULL_SHADOW, llcullshadow.h): the group's id in the flat table, the build it first appeared in, the stamp of the last frame the CPU cull kept it
    U32         mCullId = 0;                // slot + 1 in the table (0: none)
    bool        mCullDirty = false;         // in the list of rows to refresh
    U32         mCullFirstSerial = 0;
    U32         mCullCpuStamp = 0;
    U32         mCullUseStamp = 0;          // C2: the last frame the flat version kept it

};//LL_ALIGN_POSTFIX(16);

// <FS> C4.5 census (LL_VK_MAINCENSUS=1, off by default): what the main view's partition cull does, counted by the walks on every thread while sActive is set (pipeline.cpp sets it around the cull) and read once a frame
struct LLCullCensus
{
    static inline bool sOn = false;
    static inline std::atomic<bool> sActive{ false };
    static inline std::atomic<U64> sNodes{ 0 }, sEarlyFail{ 0 }, sOccDropped{ 0 }, sAllInside{ 0 }, sFrustumTests{ 0 }, sFrustumOut{ 0 };
    static inline std::atomic<U64> sMarkCalls{ 0 }, sMarkEmpty{ 0 }, sMarkDrawable{ 0 }, sMarkVisible{ 0 }, sMarkTicks{ 0 };
    static inline std::atomic<U32> sOccChanges[16] = {};      // changes of a group's world OCCLUDED state (a verdict, a discard, the eye inside the box), by partition type
    // what those changes are (step 0 of the C4.5 design): set / cleared, by who made them (0 a verdict, 1 checkOcclusion's discard, 2 the eye inside the box, 3 hizEntryMoved), whether the group is a leaf, the nodes below it,
    // whether it holds an avatar (or a bridge) or another moving drawable, and how long the group had been in the state it left (frames: <= 2, <= 8, <= 60, longer, first time seen), for the state left = visible / occluded
    static inline std::atomic<U64> sOccSet{ 0 }, sOccClear{ 0 }, sOccSrc[4] = {}, sOccLeaf{ 0 }, sOccSubtree{ 0 }, sOccAvatar{ 0 }, sOccActive{ 0 }, sOccFirst{ 0 }, sOccDwell[2][4] = {};
    static void noteOcc(class LLOcclusionCullingGroup* group, bool now_occluded, int source);       // pipeline.cpp
};

// <FS> C4.5 S1b (LL_VK_VISSET >= 1): the events that change what the main view's cull lists, told to the retained set (llspatialpartition.cpp, LLVisSet). Every call site tests sOn first.
struct LLVisSetHooks
{
    static inline std::atomic<bool> sOn{ false };
    static void noteOcc(LLViewerOctreeGroup* group);        // the group's world OCCLUDED state changed (its subtree may have: the walk of it is redone from the group)
    static void noteBounds(LLViewerOctreeGroup* group);     // the group's bounds, extents, object bounds, frustum-skip flag or elements changed
    static void forget(LLViewerOctreeGroup* group);         // the group is destroyed
    static void noteStale(LLViewerOctreeGroup* group);      // hizInvalidate: the discard of an awaited verdict is made the next time the walk visits the group (checkOcclusion): the retained set makes it itself
    static void invalidateAll();                            // everything moved (a region shift): the retained set is rebuilt
    // stage 8d (LL_VK_HIZROWS=2): the table's rows are the Hi-Z's boxes and the worker's verdicts, by slot, are applied here (pipeline.cpp, with the table): sHizRows says so
    static inline const bool sHizRows = []() { const char* e = getenv("LL_VK_HIZROWS"); return e && atoi(e) == 2; }();
    static void hizRowsState(LLViewerOctreeGroup* group, bool occluded);    // the group's world OCCLUDED state is now this (any setter): the mirror of what is applied follows it
    static void hizRowsReset(LLViewerOctreeGroup* group);                   // the verdicts that led to the state are out of date (an object was added or moved into the group): two new agreeing verdicts are needed to occlude it again
    static void hizRowsInvalidate(LLViewerOctreeGroup* group);              // ... and the group is visible at once (hizInvalidate: the old discard of an awaited verdict, made by the event)
    static void hizRowsApply(const U32* occ, const U32* info, U32 rows, bool stale);      // one batch of the worker's verdicts: word operations, the groups whose state changes are set or cleared
};

//octree group which has capability to support occlusion culling
//LL_ALIGN_PREFIX(16)
class LLOcclusionCullingGroup : public LLViewerOctreeGroup, public LLHizTarget
{
public:
    typedef enum
    {
        OCCLUDED                = 0x00010000,
        ACTIVE_OCCLUSION        = 0x00040000,
        EARLY_FAIL              = 0x00100000,
    } eOcclusionState;

    typedef enum
    {
        STATE_MODE_SINGLE = 0,      //set one node
        STATE_MODE_BRANCH,          //set entire branch
        STATE_MODE_DIFF,            //set entire branch as long as current state is different
        STATE_MODE_ALL_CAMERAS,     //used for occlusion state, set state for all cameras
    } eSetStateMode;

protected:
    virtual ~LLOcclusionCullingGroup();

public:
    LLOcclusionCullingGroup(OctreeNode* node, LLViewerOctreePartition* part);
    /*virtual*/ LLViewerOctreePartition* getOctreePartition() const override { return mSpatialPartition; }       // <FS> C4.3
    LLOcclusionCullingGroup(const LLOcclusionCullingGroup& rhs) = delete;       // <FS> C1b
    LLOcclusionCullingGroup& operator=(const LLOcclusionCullingGroup& rhs) = delete;

    void setOcclusionState(U32 state, S32 mode = STATE_MODE_SINGLE);
    void clearOcclusionState(U32 state, S32 mode = STATE_MODE_SINGLE);
    void checkOcclusion(); // a group that asked to be discarded while its verdict was awaited is visible again; a group under an occluded parent is occluded with it
    // adds the group's box to the Hi-Z batch when it has none awaited (shift: the object cache's region origin, to agent space)
    void hizCollect(LLCamera* camera, const LLVector4a* shift = NULL);
    void hizInvalidate() { mHizStale = true; if (LLVisSetHooks::sHizRows && mCullId) { LLVisSetHooks::hizRowsInvalidate(this); } if (mHizSerial && LLVisSetHooks::sOn.load(std::memory_order_relaxed)) { LLVisSetHooks::noteStale(this); } }       // (only a verdict that is awaited has a discard to make: with none, hizCollect resets the flag before it sends the next box)  // the group changed (an object was added): the verdict awaited, if any, is out of date
    bool hizIsStale() const { return mHizStale; }
    // <FS> an object (its extents) is now in this group: a held occluded verdict of the group or of a group above it was tested without the object, so where the object is outside the box it covered, the
    // verdict says nothing about it. The occluded state is dropped there (this group and the ones above, one node each: the siblings keep theirs) and the next two agreeing verdicts decide again. Without this
    // an object that moved into a group with a held occluded verdict was missing for the 1 to 4 frames the verdict of its new box took. LL_VK_HIZ_ENTRY_OFF=1 = the old behaviour.
    void hizEntryMoved(const LLVector4a* entry_extents);    bool hizNeedsMainThread() const { return mHizStale && isHizAwaiting(); }   // <FS> checkOcclusion would clear the occlusion state of the subtree (the parallel stateSort leaves such a sort to the main thread)
    virtual void hizResult(bool occluded);
    void hizRowSet(bool occluded);                          // stage 8d: the worker's verdict of the row, applied to this group alone (the agreement of two verdicts is made by LLVisSetHooks::hizRowsApply)
    bool isOcclusionState(U32 state) const { return row().occlusion[LLViewerCamera::sCurCameraID] & state; }
    U32  getOcclusionState() const { return row().occlusion[LLViewerCamera::sCurCameraID];}

    bool needsUpdate();

    //virtual
    void handleChildAddition(const OctreeNode* parent, OctreeNode* child);

    //virtual
    bool isRecentlyVisible() const;
    bool isAnyRecentlyVisible() const;

private:
    bool earlyFail(LLCamera* camera, const LLVector4a* bounds);

protected:
    LLViewerOctreePartition* mSpatialPartition;
    bool                     mHizStale = false;
};//LL_ALIGN_POSTFIX(16);

class LLViewerOctreePartition
{
public:
    LLViewerOctreePartition();
    virtual ~LLViewerOctreePartition();

    // Cull on arbitrary frustum
    virtual S32 cull(LLCamera &camera, bool do_occlusion) = 0;
    bool isOcclusionEnabled();

protected:
    // MUST call from destructor of any derived classes (SL-17276)
    void cleanup();

public:
    U32              mPartitionType;
    U32              mDrawableType;
    OctreeNode*      mOctree;
    LLViewerRegion*  mRegionp; // the region this partition belongs to.
    bool             mOcclusionEnabled; // if true, occlusion culling is performed
    U32              mLODSeed;
    U32              mLODPeriod;    //number of frames between LOD updates for a given spatial group (staggered by mLODSeed)
    // <FS> C4.3: a number that is never used by another partition (a retained result is keyed on it: the address of a destroyed partition can be reused) and the partition's change epoch (see
    // LLViewerOctreeGroup::bumpPartitionEpoch)
    U32              mId = 0;
    std::atomic<U32> mEpoch{ 0 };
    std::atomic<bool> mVisTracked{ false };      // <FS> C4.5 S1b: the retained set follows this partition (the hooks of groups of other partitions return at once)
};

class LLViewerOctreeCull : public OctreeTraveler
{
public:
    LLViewerOctreeCull(LLCamera* camera)
        : mCamera(camera), mRes(0) { }

    virtual void traverse(const OctreeNode* n);

    // <FS> stage 3a (PLAN-statesort-offmain.md): traverse() with its top `levels` levels done on the calling thread and the subtrees below them handed out as tasks, in the order the serial walk reaches them
    // (on_task is called when one is emitted, so what the top levels did after it comes after it). A task is traverseTask()ed on any thread. mRes at the node is part of it: a sibling's walk starts with
    // the value the one before left, which the top walk reproduces (0 after a node that was checked, unchanged after one that was wholly inside; a subtree whose end state cannot be told is walked at once).
    struct SplitTask { const OctreeNode* mNode; S32 mRes; };
    // `levels` is the deepest the top may go; a node at a level above it is walked here only when descend_more(node) says so (its subtree cost too much the last time), else it is a task
    void traverseTop(const OctreeNode* n, S32 levels, const std::function<bool(const OctreeNode*)>& descend_more, const std::function<void(const SplitTask&)>& on_task);
    void traverseTask(const SplitTask& task);

protected:
    virtual bool earlyFail(LLViewerOctreeGroup* group);

    //agent space group cull
    S32 AABBInFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group);
    S32 AABBSphereIntersectGroupExtents(const LLViewerOctreeGroup* group);
    S32 AABBInFrustumGroupBounds(const LLViewerOctreeGroup* group);

    //agent space object set cull
    S32 AABBInFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group);
    S32 AABBSphereIntersectObjectExtents(const LLViewerOctreeGroup* group);
    S32 AABBInFrustumObjectBounds(const LLViewerOctreeGroup* group);

    //local region space group cull
    S32 AABBInRegionFrustumNoFarClipGroupBounds(const LLViewerOctreeGroup* group);
    S32 AABBInRegionFrustumGroupBounds(const LLViewerOctreeGroup* group);
    S32 AABBRegionSphereIntersectGroupExtents(const LLViewerOctreeGroup* group, const LLVector3& shift);

    //local region space object set cull
    S32 AABBInRegionFrustumNoFarClipObjectBounds(const LLViewerOctreeGroup* group);
    S32 AABBInRegionFrustumObjectBounds(const LLViewerOctreeGroup* group);
    S32 AABBRegionSphereIntersectObjectExtents(const LLViewerOctreeGroup* group, const LLVector3& shift);

    virtual S32 frustumCheck(const LLViewerOctreeGroup* group) = 0;
    virtual S32 frustumCheckObjects(const LLViewerOctreeGroup* group) = 0;

    bool checkProjectionArea(const LLVector4a& center, const LLVector4a& size, const LLVector3& shift, F32 pixel_threshold, F32 near_radius);
    virtual bool checkObjects(const OctreeNode* branch, const LLViewerOctreeGroup* group);
    virtual void preprocess(LLViewerOctreeGroup* group);
    virtual void processGroup(LLViewerOctreeGroup* group);
    virtual void visit(const OctreeNode* branch);

protected:
    LLCamera *mCamera;
    S32 mRes;
};

//scan the octree, output the info of each node for debug use.
class LLViewerOctreeDebug : public OctreeTraveler
{
public:
    virtual void processGroup(LLViewerOctreeGroup* group);
    virtual void visit(const OctreeNode* branch);

public:
    static bool sInDebug;
};

#endif
