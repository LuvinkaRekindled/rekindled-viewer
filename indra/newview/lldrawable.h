/**
 * @file lldrawable.h
 * @brief LLDrawable class definition
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

#ifndef LL_DRAWABLE_H
#define LL_DRAWABLE_H

#include <vector>
#include <map>

#include "v2math.h"
#include "v3math.h"
#include "v4math.h"
#include "m4math.h"
#include "v4coloru.h"
#include "llvector4a.h"
#include "llquaternion.h"
#include "xform.h"
#include "llviewerobject.h"
#include "llrect.h"
#include "llappviewer.h" // for gFrameTimeSeconds
#include "llvieweroctree.h"
#include "llinstancetable.h"   // <FS> C1a: the drawable's flag word, radius, scale and render type are columns of the instance table
#include <unordered_set>

class LLCamera;
class LLDrawPool;
class LLDrawable;
class LLFace;
class LLFacePool;
class LLSpatialGroup;
class LLSpatialBridge;
class LLSpatialPartition;
class LLVOVolume;
class LLViewerTexture;

// Can have multiple silhouettes for each object
const U32 SILHOUETTE_HIGHLIGHT = 0;

// All data for new renderer goes into this class.
LL_ALIGN_PREFIX(16)
class LLDrawable
    : public LLViewerOctreeEntryData
{
    LL_ALIGN_NEW;
public:
    typedef std::vector<LLFace*> face_list_t;

    LLDrawable(const LLDrawable& rhs) = delete;
    const LLDrawable& operator=(const LLDrawable& rhs) = delete;

    static void initClass();

    LLDrawable(LLViewerObject *vobj, bool new_entry = false);

    void markDead();            // Mark this drawable as dead
    bool isDead() const         { return isState(DEAD); }
    bool isNew() const          { return !isState(BUILT); }
    bool isUnload() const       { return isState(FOR_UNLOAD); }

    bool isLight() const;

    virtual void setVisible(LLCamera& camera_in, std::vector<LLDrawable*>* results = NULL, bool for_select = false);

    LLSpatialGroup* getSpatialGroup()const          {return (LLSpatialGroup*)getGroup();}
    LLViewerRegion* getRegion()               const { return mVObjp->getRegion(); }
    const LLTextureEntry* getTextureEntry(U8 which) const { return mVObjp->getTE(which); }
    LLPointer<LLViewerObject>& getVObj()                              { return mVObjp; }
    const LLViewerObject *getVObj() const                         { return mVObjp; }
    LLVOVolume* getVOVolume() const; // cast mVObjp tp LLVOVolume if OK

    const LLMatrix4&      getWorldMatrix() const        { return xform().getWorldMatrix(); }
    const LLMatrix4&      getRenderMatrix() const;
    void                  setPosition(LLVector3 v) const { }
    const LLVector3&      getPosition() const           { return xform().getPosition(); }
    const LLVector3&      getWorldPosition() const      { return xform().getPositionW(); }
    const LLVector3       getPositionAgent() const;
    const LLVector3&      getScale() const              { return LLInstances::row(mInstance).scale; }
    void                  setScale(const LLVector3& scale) { LLInstances::row(mInstance).scale = scale; }
    const LLQuaternion&   getWorldRotation() const      { return xform().getWorldRotation(); }
    const LLQuaternion&   getRotation() const           { return xform().getRotation(); }
    F32                   getIntensity() const          { return llmin(xform().getScale().mV[0], 4.f); }
    S32                   getLOD() const                { return mVObjp ? mVObjp->getLOD() : 1; }

    void  getMinMax(LLVector3& min,LLVector3& max) const { xform().getMinMax(min,max); }
    // <FS> C1a.2: the transform is the second part of the drawable's row in the instance table
    LLXformMatrix&      xform()       { return LLInstances::xform(mInstance); }
    const LLXformMatrix& xform() const { return LLInstances::xform(mInstance); }
    LLXformMatrix*      getXform() { return &LLInstances::xform(mInstance); }

    // <FS> C1a: the state word is a column of the instance table (row mInstance)
    U32                 getState()           const { return LLInstances::row(mInstance).flags; }
    bool                isState   (U32 bits) const { return ((LLInstances::row(mInstance).flags & bits) != 0); }
    void                setState  (U32 bits)       { LLInstances::row(mInstance).flags |= bits; }
    void                clearState(U32 bits)       { LLInstances::row(mInstance).flags &= ~bits; }
    U32                 getInstance()        const { return mInstance; }
    bool                ownsInstanceRow()    const { return LLInstances::ownedBy(mInstance, this); }   // false for a pointer to a destroyed drawable

    bool                isAvatar()  const           { return mVObjp.notNull() && mVObjp->isAvatar(); }
    bool                isRoot() const              { return !mParent || mParent->isAvatar(); }
    LLDrawable*         getRoot();
    bool                isSpatialRoot() const       { return !mParent || mParent->isAvatar(); }
    virtual bool        isSpatialBridge() const     { return false; }
    virtual LLSpatialPartition* asPartition()       { return NULL; }
    LLDrawable*         getParent() const           { return mParent; }

    // must set parent through LLViewerObject::     ()
    //bool                setParent(LLDrawable *parent);

    inline LLFace*      getFace(const S32 i) const;
    inline S32          getNumFaces()        const;
    face_list_t& getFaces() { return mFaces; }
    const face_list_t& getFaces() const { return mFaces; }

    //void                removeFace(const S32 i); // SJB: Avoid using this, it's slow
    LLFace*             addFace(LLFacePool *poolp, LLViewerTexture *texturep);
    LLFace*             addFace(const LLTextureEntry *te, LLViewerTexture *texturep);
    LLFace*             addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp);
    LLFace*             addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp, LLViewerTexture *specularp);
    void                deleteFaces(S32 offset, S32 count);
    void                setNumFaces(const S32 numFaces, LLFacePool *poolp, LLViewerTexture *texturep);
    void                setNumFacesFast(const S32 numFaces, LLFacePool *poolp, LLViewerTexture *texturep);
    void                mergeFaces(LLDrawable* src);

    void init(bool new_entry);
    void unload();
    void destroy();

    void update();
    F32 updateXform(bool undamped);

    virtual void makeActive();
    void makeActiveShared();        // <FS> view rework A2: what makeActive does once the drawable is marked ACTIVE (the shared half: texture records, the hierarchy, the rebuild and the octree)
    /*virtual*/ void makeStatic(bool warning_enabled = true);

    bool isActive() const                           { return isState(ACTIVE); }
    bool isStatic() const                           { return !isActive(); }
    bool isAnimating() const;

    virtual bool updateMove();
    virtual void movePartition();

    void updateTexture();
    void updateMaterial();
    virtual void updateDistance(LLCamera& camera, bool force_update);
    bool updateGeometry();
    void updateFaceSize(S32 idx);

    void updateSpecialHoverCursor(bool enabled);

    virtual void shiftPos(const LLVector4a &shift_vector);

    S32 getGeneration() const                   { return mGeneration; }

    bool getLit() const                         { return !isState(UNLIT); }
    void setLit(bool lit)                       { lit ? clearState(UNLIT) : setState(UNLIT); }

    bool isVisible() const;
    bool isRecentlyVisible() const;

    virtual void cleanupReferences();

    void setGroup(LLViewerOctreeGroup* group);
    void setRadius(const F32 radius);
    F32 getRadius() const                       { return LLInstances::row(mInstance).radius; }
    F32 getVisibilityRadius() const;

    void updateUVMinMax();  // Updates the cache of sun space bounding box.

    const LLVector3& getBounds(LLVector3& min, LLVector3& max) const;
    virtual void updateSpatialExtents();
    virtual void updateBinRadius();

    void setRenderType(S32 type)                { LLInstances::row(mInstance).renderType = type; }
    bool isRenderType(S32 type)                 { return LLInstances::row(mInstance).renderType == type; }
    S32  getRenderType()                        { return LLInstances::row(mInstance).renderType; }

    // Debugging methods
    S32 findReferences(LLDrawable *drawablep); // Not const because of @#$! iterators...

    LLSpatialPartition* getSpatialPartition();

    void removeFromOctree();

    void setSpatialBridge(LLSpatialBridge* bridge) { mSpatialBridge = (LLDrawable*) bridge; }
    LLSpatialBridge* getSpatialBridge() { return (LLSpatialBridge*) (LLDrawable*) mSpatialBridge; }

    // Statics
    static void incrementVisible();
    static void cleanupDeadDrawables();

protected:
    ~LLDrawable() { destroy(); LLInstances::release(mInstance, DEAD); }   // <FS> C1a: the row goes back after destroy() (which still reads the state)
    void moveUpdatePipeline(bool moved);
    void updatePartition();
    bool updateMoveDamped();
    bool updateMoveUndamped();

public:
    friend class LLPipeline;
    friend class LLDrawPool;
    friend class LLSpatialBridge;

    typedef std::unordered_set<LLPointer<LLDrawable> > drawable_set_t;
    typedef std::set<LLPointer<LLDrawable> > ordered_drawable_set_t;
    typedef std::vector<LLPointer<LLDrawable> > drawable_vector_t;
    typedef std::list<LLPointer<LLDrawable> > drawable_list_t;
    typedef std::queue<LLPointer<LLDrawable> > drawable_queue_t;

    struct CompareDistanceGreater
    {
        bool operator()(const LLDrawable* const& lhs, const LLDrawable* const& rhs)
        {
            return lhs->mDistanceWRTCamera < rhs->mDistanceWRTCamera; // farthest = last
        }
    };

    struct CompareDistanceGreaterVisibleFirst
    {
        bool operator()(const LLDrawable* const& lhs, const LLDrawable* const& rhs)
        {
            if (lhs->isVisible() && !rhs->isVisible())
            {
                return true; //visible things come first
            }
            else if (!lhs->isVisible() && rhs->isVisible())
            {
                return false; //rhs is visible, comes first
            }

            return lhs->mDistanceWRTCamera < rhs->mDistanceWRTCamera; // farthest = last
        }
    };

    typedef enum e_drawable_flags
    {
        IN_REBUILD_Q    = 0x00000001,
        EARLY_MOVE      = 0x00000004,
        MOVE_UNDAMPED   = 0x00000008,
        ON_MOVE_LIST    = 0x00000010,
        UV              = 0x00000020,
        UNLIT           = 0x00000040,
        LIGHT           = 0x00000080,
        REBUILD_VOLUME  = 0x00000100,   //volume changed LOD or parameters, or vertex buffer changed
        REBUILD_TCOORD  = 0x00000200,   //texture coordinates changed
        REBUILD_COLOR   = 0x00000400,   //color changed
        REBUILD_POSITION= 0x00000800,   //vertex positions/normals changed
        REBUILD_GEOMETRY= REBUILD_POSITION|REBUILD_TCOORD|REBUILD_COLOR,
        REBUILD_MATERIAL= REBUILD_TCOORD|REBUILD_COLOR,
        REBUILD_ALL     = REBUILD_GEOMETRY|REBUILD_VOLUME,
        REBUILD_RIGGED  = 0x00001000,
        ON_SHIFT_LIST   = 0x00002000,
        ACTIVE          = 0x00004000,
        DEAD            = 0x00008000,
        INVISIBLE       = 0x00010000, // stay invisible until flag is cleared
        NEARBY_LIGHT    = 0x00020000, // In gPipeline.mNearbyLightSet
        BUILT           = 0x00040000,
        FORCE_INVISIBLE = 0x00080000, // stay invis until CLEAR_INVISIBLE is set (set of orphaned)
        HAS_ALPHA       = 0x00100000,
        RIGGED          = 0x00200000, //has a rigged face
        RIGGED_CHILD    = 0x00400000, //has a child with a rigged face
        PARTITION_MOVE  = 0x00800000,
        ANIMATED_CHILD  = 0x01000000,
        ACTIVE_CHILD    = 0x02000000,
        FOR_UNLOAD      = 0x04000000, //should be unload from memory
        MIRROR          = 0x08000000, // Used as a mirror, needs a hero probe position to be calculated.
    } EDrawableFlags;

public:
    // vis data
    LLPointer<LLDrawable> mParent;

    F32             mDistanceWRTCamera;

    // <FS> a rigid attachment: the world matrix of its attachment point when its move chain last ran (LLVOAvatar::idleUpdateMisc skips the chain while the joint stays within a millimetre / 0.06 degrees of it)
    F32             mAttJointApplied[15];
    bool            mAttJointValid = false;

    static F32 sCurPixelAngle; //current pixels per radian

private:
    U32             mInstance;      // <FS> C1a: this drawable's row in the instance table (flags, radius, render type, scale)
    LLPointer<LLViewerObject> mVObjp;
    face_list_t     mFaces;
    LLPointer<LLDrawable> mSpatialBridge;

    S32             mGeneration;

    static U32 sNumZombieDrawables;
    static std::vector<LLPointer<LLDrawable> > sDeadList;
} LL_ALIGN_POSTFIX(16);


inline LLFace* LLDrawable::getFace(const S32 i) const
{
    //switch these asserts to LL_ERRS() -- davep
    //llassert((U32)i < mFaces.size());
    //llassert(mFaces[i]);

    if ((U32) i >= mFaces.size())
    {
        LLUUID objectID=getVObj()->getID();

        // if our face list is empty, we have no real choice. -Zi
        if(mFaces.empty())
        {
            LL_WARNS() << objectID << ": Empty face list." << LL_ENDL;
            return NULL;
        }

        // otherwise try to return a valid face to avoid crashing. -Zi
        LL_WARNS() << objectID << ": Invalid face index " << (U32) i << ". Max faces is: " << mFaces.size() << ". Returning face index 0." << LL_ENDL;
        return mFaces[0];
    }

    if (!mFaces[i])
    {
        LLUUID objectID=getVObj()->getID();

        LL_WARNS() << objectID << ": Null face found at index " << (U32) i << ". Max faces is: " << mFaces.size() << "." << LL_ENDL;
        if(i==0)
        {
            S32 max=getNumFaces();

            // try to return a valid face to avoid crashing. If we only have one face, return NULL as last resort. -Zi
            if(max>1)
                return mFaces[max-1];
            else
                return NULL;
        }
    }

    return mFaces[i];
}


inline S32 LLDrawable::getNumFaces()const
{
    return (S32)mFaces.size();
}

// <FS> LL_VK_DRAWABLE_RING (default on, 0 = off): the last few thousand drawables destroyed, each with the stack of its destruction (the code that released the last reference), and a test of whether a pointer
// still points at a live drawable (its first word is a vtable inside the executable). For the moved list crash of 2026-11-01: an entry already freed when the pass started.
bool fs_drawable_looks_live(const void* p);
std::string fs_drawable_destroy_info(const void* p);      // when and where the drawable at that address was last destroyed (or why not known), stack included

#endif
