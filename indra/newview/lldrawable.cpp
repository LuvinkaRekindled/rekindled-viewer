/**
 * @file lldrawable.cpp
 * @brief LLDrawable class implementation
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

#include "llviewerprecompiledheaders.h"
#include "rkgpuzonetimer.h"

#include "lldrawable.h"
#if !LL_WINDOWS
#include <sys/uio.h>
#include <unistd.h>
#endif
#include "rkattcensus.h"
#include "rkviewertexturerecs.h"
#include <mutex>
#include <atomic>

// library includes
#include "material_codes.h"

// viewer includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "llface.h"
#include "lllightconstants.h"
#include "llmatrix4a.h"
#include "llsky.h"
#include "llsurfacepatch.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llvolume.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "llvosurfacepatch.h" // for debugging
#include "llworld.h"
#include "pipeline.h"
#include "llspatialpartition.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvocache.h"
#include "llcontrolavatar.h"
#include "lldrawpoolavatar.h"

// <FS:ND> Tentatively ignoring mismatched new/delete from the MemTrackableNonVirtual. I think according to the docs
// they are properly matched
#if LL_LINUX
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
// </FS:ND>

const F32 MIN_INTERPOLATE_DISTANCE_SQUARED = 0.001f * 0.001f;
const F32 MAX_INTERPOLATE_DISTANCE_SQUARED = 10.f * 10.f;
const F32 OBJECT_DAMPING_TIME_CONSTANT = 0.06f;

extern bool gShiftFrame;


////////////////////////
//
// Inline implementations.
//
//



//////////////////////////////
//
// Drawable code
//
//

// static
U32 LLDrawable::sNumZombieDrawables = 0;
F32 LLDrawable::sCurPixelAngle = 0;
std::vector<LLPointer<LLDrawable> > LLDrawable::sDeadList;

#define FORCE_INVISIBLE_AREA 16.f

// static
void LLDrawable::incrementVisible()
{
    LLViewerOctreeEntryData::incrementVisible();
    sCurPixelAngle = (F32) gViewerWindow->getWindowHeightRaw()/LLViewerCamera::getInstance()->getView();
}

LLDrawable::LLDrawable(LLViewerObject *vobj, bool new_entry)
:   LLViewerOctreeEntryData(LLViewerOctreeEntry::LLDRAWABLE),
    mVObjp(vobj)
{
    init(new_entry);
}

void LLDrawable::init(bool new_entry)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    // mXform
    mParent = NULL;
    // <FS> C1a: the row (flags 0, radius 0, render type 0, scale 1,1,1) comes cleared from the table
    mInstance = RKInstances::acquire(this);
    if (!mInstance)
    {
        LL_ERRS() << "the instance table is full (" << RKInstances::kCapacity << " drawables)" << LL_ENDL;
    }
    mDistanceWRTCamera = 0.0f;

    // mFaces
    mGeneration = -1;
    mSpatialBridge = NULL;

    LLViewerOctreeEntry* entry = NULL;
    LLVOCacheEntry* vo_entry = NULL;
    if(!new_entry && mVObjp && getRegion() != NULL)
    {
        vo_entry = getRegion()->getCacheEntryForOctree(mVObjp->getLocalID());
        if(vo_entry)
        {
            entry = vo_entry->getEntry();
        }
    }
    setOctreeEntry(entry);
    if(vo_entry)
    {
        if(!entry)
        {
            vo_entry->setOctreeEntry(mEntry);
        }

        getRegion()->addActiveCacheEntry(vo_entry);

        if(vo_entry->getNumOfChildren() > 0)
        {
            getRegion()->addVisibleChildCacheEntry(vo_entry, NULL); //to load all children.
        }

        llassert(!vo_entry->getGroup()); //not in the object cache octree.
    }

    llassert(!vo_entry || vo_entry->getEntry() == mEntry);

    initVisible(sCurVisible - 2);//invisible for the current frame and the last frame.
}

void LLDrawable::unload()
{
    LLVOVolume *pVVol = getVOVolume();
    pVVol->setNoLOD();
    pVVol->markForUpdate();
}

// static
void LLDrawable::initClass()
{
}


std::string rk_stack_here(int skip, int max_frames);
std::string rk_describe_frame(const void* pc);

// <FS> the ring of destroyed drawables (see lldrawable.h)
namespace
{
    struct DestroyRec
    {
        const void* mAddr = nullptr;
        U32 mThread = 0;
        U32 mFrame = 0;
        U32 mSeq = 0;
        USHORT mDepth = 0;
        void* mFrames[18] = {};
    };
    constexpr U32 kDestroyRing = 8192;
    DestroyRec sDestroyRing[kDestroyRing];
    std::atomic<U32> sDestroyNext{0};

    bool ring_on()
    {
        static const bool on = getenv("RK_VK_DRAWABLE_RING") ? atoi(getenv("RK_VK_DRAWABLE_RING")) != 0 : true;
        return on;
    }

    void ring_note(const void* p)
    {
        const U32 seq = sDestroyNext.fetch_add(1);
        DestroyRec& r = sDestroyRing[seq % kDestroyRing];
        r.mAddr = nullptr;                      // (a reader that meets it half written sees no address)
        r.mThread = GetCurrentThreadId();
        r.mFrame = LLViewerOctreeEntryData::getCurrentFrame();
        r.mSeq = seq;
        r.mDepth = CaptureStackBackTrace(2, 18, r.mFrames, nullptr);
        r.mAddr = p;
    }

    // the first word of the object, read under a handler (a freed block can be unmapped)
    bool read_first_word(const void* p, const void*& word)
    {
#if LL_WINDOWS
        __try
        {
            word = *(const void* const*)p;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        // the kernel copies the word and reports a bad address instead of faulting
        iovec local = { &word, sizeof(word) };
        iovec remote = { const_cast<void*>(p), sizeof(word) };
        return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(word);
#endif
    }
}

bool rk_drawable_looks_live(const void* p)
{
#if LL_WINDOWS
    static const char* image_base = (const char*)GetModuleHandle(nullptr);
    static const size_t image_size = []()
    {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)image_base;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(image_base + dos->e_lfanew);
        return (size_t)nt->OptionalHeader.SizeOfImage;
    }();
#else
    // the executable's own mapping: the linker's start and end symbols
    extern char __executable_start;
    extern char end;
    static const char* image_base = &__executable_start;
    static const size_t image_size = (size_t)(&end - &__executable_start);
#endif
    const void* word = nullptr;
    if (!p || !read_first_word(p, word))
    {
        return false;
    }
    const char* w = (const char*)word;
    return w >= image_base && w < image_base + image_size;
}

std::string rk_drawable_destroy_info(const void* p)
{
    if (!ring_on())
    {
        return "(the ring is off: RK_VK_DRAWABLE_RING=0)";
    }
    const U32 newest = sDestroyNext.load();
    const U32 count = std::min(newest, kDestroyRing);
    for (U32 k = 0; k < count; ++k)
    {
        const DestroyRec& r = sDestroyRing[(newest - 1 - k) % kDestroyRing];
        if (r.mAddr == p)
        {
            std::string text = llformat("destroyed %u destructions ago, in frame %u (now frame %u), on thread %lu (main %lu):\n", k, r.mFrame, LLViewerOctreeEntryData::getCurrentFrame(), (unsigned long)r.mThread, (unsigned long)GetCurrentThreadId());
            for (USHORT i = 0; i < r.mDepth; ++i)
            {
                text += llformat("  %s\n", rk_describe_frame(r.mFrames[i]).c_str());
            }
            return text;
        }
    }
    return llformat("not among the last %u destroyed drawables (%u destroyed so far): not freed by ~LLDrawable, or longer ago", count, newest);
}

void LLDrawable::destroy()
{
    if (ring_on())
    {
        ring_note(this);
    }
    // <FS> RK_VK_DRAWABLE_TRACE (default on while the moved list crash of 2026-11-01 is open, 0 = off): a drawable freed while the moved list still holds it. The list holds a counted pointer, so this is a reference
    // released one time too many; the stack is the code that released the last one. (lap rp4-scchk1: updateMovedListSerial read entry 0 of 89 after it was freed)
    static const bool trace = getenv("RK_VK_DRAWABLE_TRACE") ? atoi(getenv("RK_VK_DRAWABLE_TRACE")) != 0 : true;
    if (trace && isState(ON_MOVE_LIST) && gPipeline.mInitialized && gPipeline.inMovedList(this))
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8)
        {
            LL_WARNS("DrawableTrace") << "a drawable is freed while the moved list holds it (state " << getState() << ", dead " << isDead() << "):\n" << rk_stack_here(0, 30) << LL_ENDL;
        }
    }
    if (gDebugRender)
    {
        gPipeline.checkReferences(this);
    }

    if (isDead())
    {
        sNumZombieDrawables--;
    }

    // Attempt to catch violations of this in debug,
    // knowing that some false alarms may result
    //
    llassert(!LLSpatialGroup::sNoDelete);

    /* cannot be guaranteed and causes crashes on false alarms
    if (LLSpatialGroup::sNoDelete)
    {
        LL_ERRS() << "Illegal deletion of LLDrawable!" << LL_ENDL;
    }*/

    std::for_each(mFaces.begin(), mFaces.end(), DeletePointer());
    mFaces.clear();


    /*if (!(sNumZombieDrawables % 10))
    {
        LL_INFOS() << "- Zombie drawables: " << sNumZombieDrawables << LL_ENDL;
    }*/

}

void LLDrawable::markDead()
{
    if (isDead())
    {
        LL_WARNS() << "Warning!  Marking dead multiple times!" << LL_ENDL;
        return;
    }
    setState(DEAD);

    if (mSpatialBridge)
    {
        mSpatialBridge->markDead();
        mSpatialBridge = NULL;
    }

    sNumZombieDrawables++;

    // We're dead.  Free up all of our references to other objects
    cleanupReferences();
//  sDeadList.push_back(this);
}

LLVOVolume* LLDrawable::getVOVolume() const
{
    LLViewerObject* objectp = mVObjp;
    if ( !isDead() && objectp && (objectp->getPCode() == LL_PCODE_VOLUME))
    {
        return ((LLVOVolume*)objectp);
    }
    else
    {
        return NULL;
    }
}

const LLMatrix4& LLDrawable::getRenderMatrix() const
{
    return isRoot() ? getWorldMatrix() : getParent()->getWorldMatrix();
}

bool LLDrawable::isLight() const
{
    LLViewerObject* objectp = mVObjp;
    if ( objectp && (objectp->getPCode() == LL_PCODE_VOLUME) && !isDead())
    {
        return ((LLVOVolume*)objectp)->getIsLight();
    }
    else
    {
        return false;
    }
}

void LLDrawable::cleanupReferences()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    std::for_each(mFaces.begin(), mFaces.end(), DeletePointer());
    mFaces.clear();

    if (gPipeline.mInitialized)
    {
        gPipeline.unlinkDrawable(this);
    }
    else if (getSpatialGroup())
    {
        // Not supposed to happen?
        getSpatialGroup()->getSpatialPartition()->remove(this, getSpatialGroup());
    }

    removeFromOctree();

    // Cleanup references to other objects
    mVObjp = NULL;
    mParent = NULL;
}

void LLDrawable::removeFromOctree()
{
    if(!mEntry)
    {
        return;
    }

    mEntry->removeData(this);
    if(mEntry->hasVOCacheEntry())
    {
        getRegion()->removeActiveCacheEntry((LLVOCacheEntry*)mEntry->getVOCacheEntry(), this);
    }
    mEntry = NULL;
}

void LLDrawable::cleanupDeadDrawables()
{
    /*
    S32 i;
    for (i = 0; i < sDeadList.size(); i++)
    {
        if (sDeadList[i]->getNumRefs() > 1)
        {
            LL_WARNS() << "Dead drawable has " << sDeadList[i]->getNumRefs() << " remaining refs" << LL_ENDL;
            gPipeline.findReferences(sDeadList[i]);
        }
    }
    */
    sDeadList.clear();
}

S32 LLDrawable::findReferences(LLDrawable *drawablep)
{
    S32 count = 0;
    if (mParent == drawablep)
    {
        LL_INFOS() << this << ": parent reference" << LL_ENDL;
        count++;
    }
    return count;
}

LLFace* LLDrawable::addFace(LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    {
        face = new LLFace(this, mVObjp);
    }

    if (!face) LL_ERRS() << "Allocating new Face: " << mFaces.size() << LL_ENDL;

    if (face)
    {
        mFaces.push_back(face);

        if (poolp)
        {
            face->setPool(poolp, texturep);
        }

        if (isState(UNLIT))
        {
            face->setState(LLFace::FULLBRIGHT);
        }
    }
    return face;
}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;

    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setNormalMap(normalp);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

LLFace* LLDrawable::addFace(const LLTextureEntry *te, LLViewerTexture *texturep, LLViewerTexture *normalp, LLViewerTexture *specularp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLFace *face;
    face = new LLFace(this, mVObjp);

    face->setTEOffset(static_cast<S32>(mFaces.size()));
    face->setTexture(texturep);
    face->setNormalMap(normalp);
    face->setSpecularMap(specularp);
    face->setPoolType(gPipeline.getPoolTypeFromTE(te, texturep));

    mFaces.push_back(face);

    if (isState(UNLIT))
    {
        face->setState(LLFace::FULLBRIGHT);
    }

    return face;

}

void LLDrawable::setNumFaces(const S32 newFaces, LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (newFaces == (S32)mFaces.size())
    {
        return;
    }
    else if (newFaces < (S32)mFaces.size())
    {
        std::for_each(mFaces.begin() + newFaces, mFaces.end(), DeletePointer());
        mFaces.erase(mFaces.begin() + newFaces, mFaces.end());
    }
    else // (newFaces > mFaces.size())
    {
        mFaces.reserve(newFaces);
        for (auto i = mFaces.size(); i<newFaces; i++)
        {
            addFace(poolp, texturep);
        }
    }

    llassert_always(mFaces.size() == newFaces);
}

void LLDrawable::setNumFacesFast(const S32 newFaces, LLFacePool *poolp, LLViewerTexture *texturep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (newFaces <= (S32)mFaces.size() && newFaces >= (S32)mFaces.size()/2)
    {
        return;
    }
    else if (newFaces < (S32)mFaces.size())
    {
        std::for_each(mFaces.begin() + newFaces, mFaces.end(), DeletePointer());
        mFaces.erase(mFaces.begin() + newFaces, mFaces.end());
    }
    else // (newFaces > mFaces.size())
    {
        mFaces.reserve(newFaces);
        for (auto i = mFaces.size(); i<newFaces; i++)
        {
            addFace(poolp, texturep);
        }
    }

    llassert_always(mFaces.size() == newFaces) ;
}

void LLDrawable::mergeFaces(LLDrawable* src)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    auto face_count = mFaces.size() + src->mFaces.size();

    mFaces.reserve(face_count);
    for (size_t i = 0; i < src->mFaces.size(); i++)
    {
        LLFace* facep = src->mFaces[i];
        facep->setDrawable(this);
        mFaces.push_back(facep);
    }
    src->mFaces.clear();
}

void LLDrawable::deleteFaces(S32 offset, S32 count)
{
    face_list_t::iterator face_begin = mFaces.begin() + offset;
    face_list_t::iterator face_end = face_begin + count;

    std::for_each(face_begin, face_end, DeletePointer());
    mFaces.erase(face_begin, face_end);
}

void LLDrawable::update()
{
    LL_ERRS() << "Shouldn't be called!" << LL_ENDL;
}


void LLDrawable::updateMaterial()
{
}

void LLDrawable::makeActive()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

#if !LL_RELEASE_FOR_DOWNLOAD
    if (mVObjp.notNull())
    {
        U32 pcode = mVObjp->getPCode();
        if (pcode == LLViewerObject::LL_VO_WATER ||
            pcode == LLViewerObject::LL_VO_VOID_WATER ||
            pcode == LLViewerObject::LL_VO_SURFACE_PATCH ||
            pcode == LLViewerObject::LL_VO_PART_GROUP ||
            pcode == LLViewerObject::LL_VO_HUD_PART_GROUP ||
            pcode == LLViewerObject::LL_VO_SKY)
        {
            LL_ERRS() << "Static viewer object has active drawable!" << LL_ENDL;
        }
    }
#endif

    if (!isState(ACTIVE)) // && mGeneration > 0)
    {
        setState(ACTIVE);
        if (LLPipeline::sAvSink)
        {
            // <FS> view rework A2: the bit is set at once (the rest of the loop sees an active drawable), the shared half is made when the avatar's writes are replayed
            LLPipeline::sAvSink->push_back({ LLPipeline::AvOp::MAKE_ACTIVE_SHARED, this });
            return;
        }
        makeActiveShared();
    }
    else if (!isRoot() && !mParent->isActive()) //this should not happen, but occasionally it does...
    {
        mParent->makeActive();
        //NOTE: linked set will now NEVER become static
        mParent->setState(LLDrawable::ACTIVE_CHILD);
    }

    llassert(isAvatar() || isRoot() || mParent->isActive());
}

void LLDrawable::makeActiveShared()
{
    {
        for (S32 fi = 0; fi < getNumFaces(); ++fi) RKTexRecs::faceChanged(getFace(fi));        // <FS> H6: its faces' texture records are volatile from now on

        //parent must be made active first
        if (!isRoot() && !mParent->isActive())
        {
            mParent->makeActive();
            //NOTE: linked set will now NEVER become static
            mParent->setState(LLDrawable::ACTIVE_CHILD);
        }

        //all child objects must also be active
        llassert_always(mVObjp);

        LLViewerObject::const_child_list_t& child_list = mVObjp->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); iter++)
        {
            LLViewerObject* child = *iter;
            LLDrawable* drawable = child->mDrawable;
            if (drawable)
            {
                drawable->makeActive();
            }
        }

        if (mVObjp->getPCode() == LL_PCODE_VOLUME)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_VOLUME);
        }
        updatePartition();
    }
}


void LLDrawable::makeStatic(bool warning_enabled)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (isState(ACTIVE) &&
        !isState(ACTIVE_CHILD) &&
        !mVObjp->isAttachment() &&
        !mVObjp->isFlexible() &&
        !mVObjp->isAnimatedObject())
    {
        clearState(ACTIVE | ANIMATED_CHILD);
        for (S32 fi = 0; fi < getNumFaces(); ++fi) RKTexRecs::faceChanged(getFace(fi));        // <FS> H6

        //drawable became static with active parent, not acceptable
        llassert(mParent.isNull() || !mParent->isActive() || !warning_enabled);

        LLViewerObject::const_child_list_t& child_list = mVObjp->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); iter++)
        {
            LLViewerObject* child = *iter;
            LLDrawable* child_drawable = child->mDrawable;
            if (child_drawable)
            {
                if (child_drawable->getParent() != this)
                {
                    LL_WARNS() << "Child drawable has unknown parent." << LL_ENDL;
                }
                child_drawable->makeStatic(warning_enabled);
            }
        }

        if (mVObjp->getPCode() == LL_PCODE_VOLUME)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_VOLUME);
        }

        if (mSpatialBridge)
        {
            mSpatialBridge->markDead();
            setSpatialBridge(NULL);
        }
        updatePartition();
    }

    llassert(isAvatar() || isRoot() || mParent->isStatic());
}

// Returns "distance" between target destination and resulting xfrom
F32 LLDrawable::updateXform(bool undamped)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    bool damped = !undamped;

    // Position
    const LLVector3 old_pos(xform().getPosition());
    LLVector3 target_pos;
    if (xform().isRoot())
    {
        // get root position in your agent's region
        target_pos = mVObjp->getPositionAgent();
    }
    else
    {
        // parent-relative position
        target_pos = mVObjp->getPosition();
    }

    // Rotation
    const LLQuaternion old_rot(xform().getRotation());
    LLQuaternion target_rot = mVObjp->getRotation();
    //scaling
    LLVector3 target_scale = mVObjp->getScale();
    LLVector3 old_scale = getScale();

    // Damping
    F32 dist_squared = 0.f;
    F32 camdist2 = (mDistanceWRTCamera * mDistanceWRTCamera);

    if (damped && isVisible())
    {
        F32 lerp_amt = llclamp(LLSmoothInterpolation::getInterpolant(OBJECT_DAMPING_TIME_CONSTANT), 0.f, 1.f);
        LLVector3 new_pos = lerp(old_pos, target_pos, lerp_amt);
        dist_squared = dist_vec_squared(new_pos, target_pos);

        LLQuaternion new_rot = nlerp(lerp_amt, old_rot, target_rot);
        // FIXME: This can be negative! It is be possible for some rots to 'cancel out' pos or size changes.
        dist_squared += (1.f - dot(new_rot, target_rot)) * 10.f;

        LLVector3 new_scale = lerp(old_scale, target_scale, lerp_amt);
        dist_squared += dist_vec_squared(new_scale, target_scale);

        if ((dist_squared >= MIN_INTERPOLATE_DISTANCE_SQUARED * camdist2) &&
            (dist_squared <= MAX_INTERPOLATE_DISTANCE_SQUARED))
        {
            // interpolate
            target_pos = new_pos;
            target_rot = new_rot;
            target_scale = new_scale;
        }
        else if (mVObjp->getAngularVelocity().isExactlyZero())
        {
            // snap to final position (only if no target omega is applied)
            dist_squared = 0.0f;
            //set target scale here, because of dist_squared = 0.0f remove object from move list
            setScale(target_scale);

            if (getVOVolume() && !isRoot())
            { //child prim snapping to some position, needs a rebuild
                gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
            }
        }
    }
    else
    {
        // The following fixes MAINT-1742 but breaks vehicles similar to MAINT-2275
        // dist_squared = dist_vec_squared(old_pos, target_pos);

        // The following fixes MAINT-2247 but causes MAINT-2275
        //dist_squared += (1.f - dot(old_rot, target_rot)) * 10.f;
        //dist_squared += dist_vec_squared(old_scale, target_scale);
    }

    const LLVector3 vec = getScale()-target_scale;

    //It's a very important on each cycle on Drawable::update form(), when object remained in move
    //, list update the CurrentScale member, because if do not do that, it remained in this list forever
    //or when the delta time between two frames a become a sufficiently large (due to interpolation)
    //for overcome the MIN_INTERPOLATE_DISTANCE_SQUARED.
    setScale(target_scale);

    if (vec*vec > MIN_INTERPOLATE_DISTANCE_SQUARED)
    { //scale change requires immediate rebuild
        gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
    }
    else if (!isRoot() &&
         (!mVObjp->getAngularVelocity().isExactlyZero() ||
            dist_squared > 0.f))
    { //child prim moving relative to parent, tag as needing to be rendered atomically and rebuild
        dist_squared = 1.f; //keep this object on the move list
        if (!isState(LLDrawable::ANIMATED_CHILD))
        {
            setState(LLDrawable::ANIMATED_CHILD);
            gPipeline.markRebuild(this, LLDrawable::REBUILD_ALL);
            mVObjp->dirtySpatialGroup();
        }
    }
    else if (!isRoot() &&
            ((dist_vec_squared(old_pos, target_pos) > 0.f)
            || (1.f - dot(old_rot, target_rot)) > 0.f))
    { //fix for BUG-840, MAINT-2275, MAINT-1742, MAINT-2247
        mVObjp->shrinkWrap();
        gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
    }
    else if (!getVOVolume() && !isAvatar())
    {
        movePartition();
    }

    // Update
    xform().setPosition(target_pos);
    xform().setRotation(target_rot);
    xform().setScale(LLVector3(1,1,1)); //no scale in drawable transforms (IT'S A RULE!)
    xform().updateMatrix();
    if (isRoot() && mVObjp->isAnimatedObject())
    {
        LLControlAvatar* cav = mVObjp->getControlAvatar();
        if (cav)
        {
            // <FS> stage C5d: an animated object worn by an avatar is placed in its own pool job, from the host's array (not here, from another avatar's thread)
            if (!cav->hostJobOwned())
            {
                cav->matchVolumeTransform();
            }
        }
    }

    if (mSpatialBridge && !mBridgeFrozen)
    {
        gPipeline.markMoved(mSpatialBridge, false);     // (mBridgeFrozen: the avatar's loop holds this attachment's bridge at the avatar's box, PLAN-avatars.md stage C)
    }
    return dist_squared;
}

void LLDrawable::setRadius(F32 radius)
{
    RKInstances::row(mInstance).radius = radius;
}

void LLDrawable::moveUpdatePipeline(bool moved)
{
    if (moved)
    {
        makeActive();
    }

    // Update the face centers.
    for (S32 i = 0; i < getNumFaces(); i++)
    {
        LLFace* face = getFace(i);
        if (face)
        {
            face->updateCenterAgent();
        }
    }
}

void LLDrawable::movePartition()
{
    RK_CPU_ZONE("octree: drawable movePartition");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (isSpatialBridge())
    {
        return;     // stage 9 (3.3): a bridge is not in a region partition
    }
    if (isAvatar())
    {
        // stage 9 (avatars): an avatar's drawable is not an entry of a region partition any more (the views list it by the registry, the pick tests it by the registry); what put and updateInGroup did for it
        // was its extents
        if (LLPipeline::sAvSink)
        {
            LLPipeline::sAvSink->push_back({ LLPipeline::AvOp::MOVE_PARTITION, this });
            return;
        }
        updateSpatialExtents();
        return;
    }

    if (LLPipeline::sAvSink)
    {
        LLPipeline::sAvSink->push_back({ LLPipeline::AvOp::MOVE_PARTITION, this });      // <FS> view rework A1: made when the avatar's loop is over
        return;
    }

    LLSpatialPartition* part = getSpatialPartition();
    if (part)
    {
        part->move(this, getSpatialGroup());
    }
}

bool LLDrawable::updateMove()
{
    RK_CPU_ZONE("drawable: updateMove");
    RKAttCensus::Scope att_census(RKAttCensus::sDrawableMove, this);
    if (isDead())
    {
        LL_WARNS() << "Update move on dead drawable!" << LL_ENDL;
        return true;
    }

    if (mVObjp.isNull())
    {
        return false;
    }

    makeActive();

    // #3256 force undampened movement for attached objects in mouselook
    // to prevent animation bork for linkset with animated parts
    if (!isRoot() && gAgentCamera.cameraMouselook() &&
        !mVObjp->isRiggedMesh() && mVObjp->getAvatar() && mVObjp->getAvatar()->isSelf())
    {
        return updateMoveUndamped();
    }

    return isState(MOVE_UNDAMPED) ? updateMoveUndamped() : updateMoveDamped();
}

// <FS> PLAN-avatars.md stage C: the move of an attached drawable whose own transform has not changed, which is all that follows its joint needs: updateXform of such a drawable finds the object's local
// position, rotation and scale equal to what the xform holds (nothing to damp, no rebuild), sets the same values again and recomputes the world matrix from the parent's (the attachment point's); with the bridge
// held at the avatar's box nothing is queued either. So this is that matrix and the faces' centres, for the loop of the avatar. False when anything differs: the full move is made.
// (C5e) an animated object whose control avatar is placed by the avatar it is worn by: the drawable's update does not place the skeleton's root, so the fast move is the full move
static bool animesh_fast(LLViewerObject* o)
{
    LLControlAvatar* cav = o->getControlAvatar();
    return cav && cav->hostJobOwned();
}

bool LLDrawable::updateMoveFast(const LLVector3* parent_pos, const LLQuaternion* parent_rot, const LLVector3* parent_scale, bool parent_sco)
{
    if (mVObjp.isNull() || isDead() || !isActive() || isState(LLDrawable::INVISIBLE | LLDrawable::ANIMATED_CHILD) || mVObjp->isChanged(LLXform::MOVED) || mVObjp->isFlexible() || (mVObjp->isAnimatedObject() && !animesh_fast(mVObjp.get())))
    {
        return false;
    }
    const LLXformMatrix& xf = xform();
    const LLVector3 target_pos = xf.isRoot() ? mVObjp->getPositionAgent() : mVObjp->getPosition();
    if (!(xf.getPosition() == target_pos) || !(xf.getRotation() == mVObjp->getRotation()) || !(getScale() == mVObjp->getScale()))
    {
        return false;
    }
    xform().setPosition(target_pos);            // (the same values as the full move sets, and its changed flags; no scale in drawable transforms)
    xform().setRotation(mVObjp->getRotation());
    xform().setScale(LLVector3(1, 1, 1));
    if (parent_pos && parent_rot && parent_scale)
    {
        xform().updateMatrixFromParent(*parent_pos, *parent_rot, *parent_scale, parent_sco);      // (the attachment point's data from the avatar's flat world array: the joint's xform is not read)
    }
    else
    {
        xform().updateMatrix();
    }
    mGeneration++;
    for (S32 i = 0; i < getNumFaces(); i++)
    {
        if (LLFace* face = getFace(i))
        {
            face->updateCenterAgent();
        }
    }
    return true;
}

bool LLDrawable::updateMoveUndamped()
{
    F32 dist_squared = updateXform(true);

    mGeneration++;

    if (!isState(LLDrawable::INVISIBLE))
    {
        bool moved = (dist_squared > 0.001f && dist_squared < 255.99f);
        moveUpdatePipeline(moved);
        mVObjp->updateText();
    }

    mVObjp->clearChanged(LLXform::MOVED);
    return true;
}

void LLDrawable::updatePartition()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (!getVOVolume())
    {
        movePartition();
    }
    else if (mSpatialBridge)
    {
        gPipeline.markMoved(mSpatialBridge, false);
    }
    else
    {
        //a child prim moved and needs its verts regenerated
        gPipeline.markRebuild(this, LLDrawable::REBUILD_POSITION);
    }
}

bool LLDrawable::updateMoveDamped()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    F32 dist_squared = updateXform(false);

    mGeneration++;

    if (!isState(LLDrawable::INVISIBLE))
    {
        bool moved = (dist_squared > 0.001f && dist_squared < 128.0f);
        moveUpdatePipeline(moved);
        mVObjp->updateText();
    }

    bool done_moving = dist_squared == 0.0f;

    if (done_moving)
    {
        mVObjp->clearChanged(LLXform::MOVED);
    }

    return done_moving;
}

void LLDrawable::updateDistance(LLCamera& camera, bool force_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        LL_WARNS() << "Attempted to update distance for non-world camera." << LL_ENDL;
        return;
    }

    if (gShiftFrame)
    {
        return;
    }

    //switch LOD with the spatial group to avoid artifacts
    //LLSpatialGroup* sg = getSpatialGroup();

    LLVector3 pos;

    //if (!sg || sg->changeLOD())
    {
        LLVOVolume* volume = getVOVolume();
        if (volume)
        {
            if (getGroup())
            {
                pos.set(getPositionGroup().getF32ptr());
            }
            else
            {
                pos = getPositionAgent();
            }

            if (isState(LLDrawable::HAS_ALPHA))
            {
                for (S32 i = 0; i < getNumFaces(); i++)
                {
                    LLFace* facep = getFace(i);
                    if (facep &&
                        (force_update || facep->isInAlphaPool()))
                    {
                        LLVector4a box;
                        box.setSub(facep->row().extents[1], facep->row().extents[0]);
                        box.mul(0.25f);
                        LLVector3 v = (facep->row().centerLocal-camera.getOrigin());
                        const LLVector3& at = camera.getAtAxis();
                        for (U32 j = 0; j < 3; j++)
                        {
                            v.mV[j] -= box[j] * at.mV[j];
                        }
                        facep->row().distance = v * camera.getAtAxis();
                    }
                }
            }


            // MAINT-7926 Handle volumes in an animated object as a special case
            // SL-937: add dynamic box handling for rigged mesh on regular avatars.
            //if (volume->getAvatar() && volume->getAvatar()->isControlAvatar())
            // <FS:Beq pp Rye> reduce recursive calls.
            // if (volume->getAvatar())
            // {
            //     const LLVector3* av_box = volume->getAvatar()->getLastAnimExtents();
            LLVOAvatar* avatarp = volume->getAvatar();
            if (avatarp)
            {
                const LLVector3* av_box = avatarp->getLastAnimExtents();
            // </FS:Beq pp Rye>
                LLVector3 cam_pos_from_agent = LLViewerCamera::getInstance()->getOrigin();
                LLVector3 cam_to_box_offset = point_to_box_offset(cam_pos_from_agent, av_box);
                mDistanceWRTCamera = llmax(0.01f, ll_round(cam_to_box_offset.magVec(), 0.01f));
                if (mVObjp)
                {
                    mVObjp->updateLOD();
                }
                return;
            }
        }
        else
        {
            // <FS> T6 of doc/vulkan/PLAN-visible-set-owner.md (the old T1 of PLAN-change-driven-sort.md, no longer behind RK_VK_SORT_CHANGE): an object nothing reads the distance of gets neither the distance nor the OBJECT_LOD effect nor
            // updateLOD: the classes that say so (usesDistance: the particle groups, the sky, the surface patches, the water, the WL sky). They were 247 of the 266 object LOD effects a sort (lap ef2-def1, 93 percent; the particle groups 214).
            if (mVObjp && !mVObjp->usesDistance())
            {
                return;
            }
            pos = LLVector3(getPositionGroup().getF32ptr());
        }

        pos -= camera.getOrigin();
        const F32 new_distance = ll_round(pos.magVec(), 0.01f);
        if (mVObjp && !getVOVolume() && (LLPipeline::sSortSink || LLPipeline::sSortTee))
        {
            // <FS> stateSort split: an avatar, grass, a terrain patch: the distance and the LOD update (joint LODs, mesh data, rebuilds) are made by the apply pass, in the order they were asked for
            const LLPipeline::SortDelta delta = { LLPipeline::SortDelta::OBJECT_LOD, 0, this, nullptr, new_distance };
            if (LLPipeline::sSortSink)
            {
                LLPipeline::sSortSink->push_back(delta);
                return;
            }
            LLPipeline::sSortTee->push_back(delta);
        }
        mDistanceWRTCamera = new_distance;
        if (mVObjp)
        {
            mVObjp->updateLOD();
        }
    }
}

void LLDrawable::updateTexture()
{
    if (isDead())
    {
        LL_WARNS() << "Dead drawable updating texture!" << LL_ENDL;
        return;
    }

    if (!mVObjp)
    {
        return;
    }

    if (getNumFaces() != mVObjp->getNumTEs())
    { //drawable is transitioning its face count
        return;
    }

    if (getVOVolume())
    {
        gPipeline.markRebuild(this, LLDrawable::REBUILD_MATERIAL);
    }
}

bool LLDrawable::updateGeometry()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    llassert(mVObjp.notNull());
    bool res = mVObjp && mVObjp->updateGeometry(this);
    return res;
}

void LLDrawable::shiftPos(const LLVector4a &shift_vector)
{
    if (isDead())
    {
        LL_WARNS() << "Shifting dead drawable" << LL_ENDL;
        return;
    }

    if (mParent)
    {
        xform().setPosition(mVObjp->getPosition());
    }
    else
    {
        xform().setPosition(mVObjp->getPositionAgent());
    }

    xform().updateMatrix();

    if (isStatic())
    {
        LLVOVolume* volume = getVOVolume();

        bool rebuild = (!volume &&
                        getRenderType() != LLPipeline::RENDER_TYPE_TREE &&
                        getRenderType() != LLPipeline::RENDER_TYPE_TERRAIN &&
                        getRenderType() != LLPipeline::RENDER_TYPE_SKY);

        if (rebuild)
        {
            gPipeline.markRebuild(this, LLDrawable::REBUILD_ALL);
        }

        for (S32 i = 0; i < getNumFaces(); i++)
        {
            LLFace *facep = getFace(i);
            if (facep)
            {
                facep->row().centerAgent += LLVector3(shift_vector.getF32ptr());
                facep->row().extents[0].add(shift_vector);
                facep->row().extents[1].add(shift_vector);
                facep->stampTextures();

                if (rebuild && facep->hasGeometry())
                {
                    facep->clearVertexBuffer();
                }
            }
        }

        shift(shift_vector);
    }
    else if (mSpatialBridge)
    {
        mSpatialBridge->shiftPos(shift_vector);
    }
    else if (isAvatar())
    {
        shift(shift_vector);
    }

    mVObjp->onShift(shift_vector);
}

const LLVector3& LLDrawable::getBounds(LLVector3& min, LLVector3& max) const
{
    xform().getMinMax(min,max);
    return xform().getPositionW();
}

void LLDrawable::updateSpatialExtents()
{
    RK_CPU_ZONE("drawable: updateSpatialExtents");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (mVObjp)
    {
        const LLVector4a* exts = getSpatialExtents();
        LLVector4a extents[2] = { exts[0], exts[1] };

        mVObjp->updateSpatialExtents(extents[0], extents[1]);
        setSpatialExtents(extents[0], extents[1]);
    }

    updateBinRadius();

    if (mSpatialBridge.notNull())
    {
        getGroupPosition().splat(0.f);
    }
}


void LLDrawable::updateBinRadius()
{
    if (mVObjp.notNull())
    {
        setBinRadius(llmin(mVObjp->getBinRadius(), 256.f));
    }
    else
    {
        setBinRadius(llmin(getRadius()*4.f, 256.f));
    }
}

void LLDrawable::updateSpecialHoverCursor(bool enabled)
{
    // TODO: maintain a list of objects that have special
    // hover cursors, then use that list for per-frame
    // hover cursor selection. JC
}

F32 LLDrawable::getVisibilityRadius() const
{
    if (isDead())
    {
        return 0.f;
    }

    if (isLight())
    {
        if (const LLVOVolume* vov = getVOVolume())
        {
            return llmax(getRadius(), vov->getLightRadius());
        }
        // LL_WARNS() ?
    }

    return getRadius();
}

void LLDrawable::updateUVMinMax()
{
}

//virtual
bool LLDrawable::isVisible() const
{
    if (LLViewerOctreeEntryData::isVisible())
    {
        return true;
    }

    LLViewerOctreeGroup* group = mEntry->getGroup();
    if (group && group->isVisible())
    {
        LLViewerOctreeEntryData::setVisible();
        return true;
    }

    return false;
}

//virtual
bool LLDrawable::isRecentlyVisible() const
{
    // Currently visible or visible in the previous frame.
    if (LLViewerOctreeEntryData::isRecentlyVisible())
    {
        return true;
    }

    const U32 MIN_VIS_FRAME_RANGE = 2 ; // Two frames: the current one and the last one.
    if (sCurVisible - getVisible() < MIN_VIS_FRAME_RANGE)
    {
        return true;
    }

    return false;
}

void LLDrawable::setGroup(LLViewerOctreeGroup *groupp)
{
    LLSpatialGroup* cur_groupp = (LLSpatialGroup*)getGroup();

    //precondition: mGroupp MUST be null or DEAD or mGroupp MUST NOT contain this
    //llassert(!cur_groupp || cur_groupp->isDead() || !cur_groupp->hasElement(this));

    //precondition: groupp MUST be null or groupp MUST contain this
    llassert(!groupp || groupp->hasElement(this));

    if (cur_groupp != groupp && getVOVolume())
    {
        //NULL out vertex buffer references for volumes on spatial group change to maintain
        //requirement that every face vertex buffer is either NULL or points to a vertex buffer
        //contained by its drawable's spatial group
        for (S32 i = 0; i < getNumFaces(); ++i)
        {
            if (LLFace* facep = getFace(i))
            {
                facep->clearVertexBuffer();
            }
        }
    }

    //postcondition: if next group is NULL, previous group must be dead OR NULL OR binIndex must be -1
    //postcondition: if next group is NOT NULL, binIndex must not be -1
    //llassert(groupp == NULL ? (cur_groupp == NULL || cur_groupp->isDead()) || (!getEntry() || getEntry()->getBinIndex() == -1) :
    //                      (getEntry() && getEntry()->getBinIndex() != -1));

    LLViewerOctreeEntryData::setGroup(groupp);
}

/*
* Get the SpatialPartition this Drawable should use.
* Checks current SpatialPartition assignment and corrects if it is invalid.
*/
LLSpatialPartition* LLDrawable::getSpatialPartition()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLSpatialPartition* retval = NULL;

    if (!mVObjp ||
        !getVOVolume() ||
        isStatic())
    {
        retval = gPipeline.getSpatialPartition(mVObjp);
    }
    else if (isRoot())
    {
        // determine if the spatial bridge has changed
        if (mSpatialBridge)
        {
            U32 partition_type = mSpatialBridge->asPartition()->mPartitionType;
            bool is_hud = mVObjp->isHUDAttachment();
            bool is_animesh = mVObjp->isAnimatedObject() && mVObjp->getControlAvatar() != NULL;
            bool is_attachment = mVObjp->isAttachment() && !is_hud && !is_animesh;
            if ((partition_type == LLViewerRegion::PARTITION_HUD) != is_hud)
            {
                // Was/became HUD
                // remove obsolete bridge
                mSpatialBridge->markDead();
                setSpatialBridge(NULL);
            }
            else if ((partition_type == LLViewerRegion::PARTITION_CONTROL_AV) != is_animesh)
            {
                // Was/became part of animesh
                // remove obsolete bridge
                mSpatialBridge->markDead();
                setSpatialBridge(NULL);
            }
            else if ((partition_type == LLViewerRegion::PARTITION_AVATAR) != is_attachment)
            {
                // Was/became part of avatar
                // remove obsolete bridge
                mSpatialBridge->markDead();
                setSpatialBridge(NULL);
            }
        }
        //must be an active volume
        if (!mSpatialBridge)
        {
            if (mVObjp->isHUDAttachment())
            {
                setSpatialBridge(new LLHUDBridge(this, getRegion()));
            }
            else if (mVObjp->isAnimatedObject() && mVObjp->getControlAvatar())
            {
                setSpatialBridge(new LLControlAVBridge(this, getRegion()));
                mVObjp->getControlAvatar()->mControlAVBridge = (LLControlAVBridge*)getSpatialBridge();
            }
            // check HUD first, because HUD is also attachment
            else if (mVObjp->isAttachment())
            {
                // Attachment
                // Use AvatarBridge of root object in attachment linkset
                setSpatialBridge(new LLAvatarBridge(this, getRegion()));
            }
            else
            {
                // Moving linkset, use VolumeBridge of root object in linkset
                setSpatialBridge(new LLVolumeBridge(this, getRegion()));
            }
        }
        return mSpatialBridge->asPartition();
    }
    else
    {
        retval = getParent()->getSpatialPartition();
    }

    if (retval && mSpatialBridge.notNull())
    {
        mSpatialBridge->markDead();
        setSpatialBridge(NULL);
    }

    return retval;
}

//=======================================
// Spatial Partition Bridging Drawable
//=======================================

std::vector<LLSpatialBridge*> LLSpatialBridge::sAll;

LLSpatialBridge::LLSpatialBridge(LLDrawable* root, bool render_by_group, U32 data_mask, LLViewerRegion* regionp) :
    LLDrawable(root->getVObj(), true),
    LLSpatialPartition(data_mask, render_by_group, regionp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;
    mOcclusionEnabled = false;
    mBridge = this;
    sAll.push_back(this);
    mDrawable = root;
    root->setSpatialBridge(this);

    setRenderType(mDrawable->getRenderType());
    mDrawableType = mDrawable->getRenderType();

    mPartitionType = LLViewerRegion::PARTITION_VOLUME;

    mOctree->balance();

    llassert(mDrawable);
    llassert(mDrawable->getRegion());
    // stage 9 (3.3): a bridge is no longer an entry of its region's partition (mPartitionType only names the kind): the views list it by the registry (allBridges), the pick tests it by the registry, and it has no group; its extents are
    // made here and at every move (updateMove)
    updateSpatialExtents();
}

LLSpatialBridge::~LLSpatialBridge()
{
    sAll.erase(std::remove(sAll.begin(), sAll.end(), this), sAll.end());
    //delete octree here so listeners will still be able to access bridge specific state
    destroyTree();
}

void LLSpatialBridge::destroyTree()
{
    delete mOctree;
    mOctree = NULL;
}

void LLSpatialBridge::updateSpatialExtents()
{
    RK_CPU_ZONE("bridge: updateSpatialExtents");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLVector4a newMin;
    LLVector4a newMax;
    if (mConservative)
    {
        // <FS> PLAN-avatars.md stage C: a plain rigid attachment's bridge holds the avatar's box (world space) while the avatar's loop keeps it there: moves of the attachment do not move it in the octree
        newMin.load3(mConsMin);
        newMax.load3(mConsMax);
    }
    else
    {
        exactWorldBox(newMin, newMax);
    }
    setSpatialExtents(newMin, newMax);

    LLVector4a diagonal;
    diagonal.setSub(newMax, newMin);
    setRadius(diagonal.getLength3().getF32() * 0.5f);

    LLVector4a& pos = getGroupPosition();
    pos.setAdd(newMin,newMax);
    pos.mul(0.5f);
    updateBinRadius();
    // (2026-11-20: the rows of the bridge's groups no longer hold the bridge's world box: nothing read it, so a move of the bridge touches none of them)
}

bool LLSpatialBridge::sphereInsideBox(const F32* boxMin, const F32* boxMax, F32 tolerance)
{
    LLSpatialGroup* root = (LLSpatialGroup*) mOctree->getListener(0);
    root->rebound();        // (returns at once when nothing in the bridge changed since the last one)
    const LLVector4a* root_bounds = root->getBounds();      // (centre, half size) in the bridge's own space
    LLMatrix4a mat;
    mat.loadu(mDrawable->getXform()->getWorldMatrix());
    LLVector4a t;
    t.splat(0.f);
    LLVector4a origin;
    mat.affineTransform(t, origin);
    LLVector4a offset;
    mat.rotate(root_bounds[0], offset);
    LLVector4a center;
    center.setAdd(origin, offset);
    const F32 radius = root_bounds[1].getLength3().getF32();        // (half the diagonal: the rotation of the bridge cannot take a corner beyond it; the matrix has no scale)
    const F32* c = center.getF32ptr();
    for (S32 k = 0; k < 3; ++k)
    {
        if (c[k] - radius < boxMin[k] - tolerance || c[k] + radius > boxMax[k] + tolerance)
        {
            return false;
        }
    }
    return true;
}

// the box of the bridge's contents in world space, from the octree root's bounds and the bridge object's matrix (what updateSpatialExtents always did)
void LLSpatialBridge::exactWorldBox(LLVector4a& newMin, LLVector4a& newMax)
{
    LLSpatialGroup* root = (LLSpatialGroup*) mOctree->getListener(0);

    root->rebound();

    const LLVector4a* root_bounds = root->getBounds();
    LLVector4a offset;
    LLVector4a size = root_bounds[1];

    //VECTORIZE THIS
    LLMatrix4a mat;
    mat.loadu(mDrawable->getXform()->getWorldMatrix());

    LLVector4a t;
    t.splat(0.f);

    LLVector4a center;
    mat.affineTransform(t, center);

    mat.rotate(root_bounds[0], offset);
    center.add(offset);

    LLVector4a v[4];

    //get 4 corners of bounding box
    mat.rotate(size,v[0]);

    LLVector4a scale;

    scale.set(-1.f, -1.f, 1.f);
    scale.mul(size);
    mat.rotate(scale, v[1]);

    scale.set(1.f, -1.f, -1.f);
    scale.mul(size);
    mat.rotate(scale, v[2]);

    scale.set(-1.f, 1.f, -1.f);
    scale.mul(size);
    mat.rotate(scale, v[3]);

    newMin = newMax = center;
    for (U32 i = 0; i < 4; i++)
    {
        LLVector4a delta;
        delta.setAbs(v[i]);
        LLVector4a min;
        min.setSub(center, delta);
        LLVector4a max;
        max.setAdd(center, delta);

        newMin.setMin(newMin, min);
        newMax.setMax(newMax, max);
    }
}

void LLSpatialBridge::updateBinRadius()
{
    setBinRadius(llmin( mOctree->getSize()[0]*0.5f, 256.f));
}

LLCamera LLSpatialBridge::transformCamera(LLCamera& camera)
{
    LLCamera ret = camera;
    LLXformMatrix* mat = mDrawable->getXform();
    LLVector3 center = LLVector3(0,0,0) * mat->getWorldMatrix();

    LLVector3 delta = ret.getOrigin() - center;
    LLQuaternion rot = ~mat->getRotation();

    delta *= rot;
    LLVector3 lookAt = ret.getAtAxis();
    LLVector3 up_axis = ret.getUpAxis();
    LLVector3 left_axis = ret.getLeftAxis();

    lookAt *= rot;
    up_axis *= rot;
    left_axis *= rot;

    if (!delta.isFinite())
    {
        delta.clearVec();
    }

    ret.setOrigin(delta);
    ret.setAxes(lookAt, left_axis, up_axis);

    return ret;
}

void LLSpatialBridge::transformExtents(const LLVector4a* src, LLVector4a* dst)
{
    LLMatrix4 mat = mDrawable->getXform()->getWorldMatrix();
    mat.invert();

    LLMatrix4a world_to_bridge(mat);

    matMulBoundBox(world_to_bridge, src, dst);
}


void LLDrawable::setVisible(LLCamera& camera, std::vector<LLDrawable*>* results, bool for_select)
{
    LLViewerOctreeEntryData::setVisible();

#if 0 && !LL_RELEASE_FOR_DOWNLOAD
    //crazy paranoid rules checking
    if (getVOVolume())
    {
        if (!isRoot())
        {
            if (isActive() && !mParent->isActive())
            {
                LL_ERRS() << "Active drawable has static parent!" << LL_ENDL;
            }

            if (isStatic() && !mParent->isStatic())
            {
                LL_ERRS() << "Static drawable has active parent!" << LL_ENDL;
            }

            if (mSpatialBridge)
            {
                LL_ERRS() << "Child drawable has spatial bridge!" << LL_ENDL;
            }
        }
        else if (isActive() && !mSpatialBridge)
        {
            LL_ERRS() << "Active root drawable has no spatial bridge!" << LL_ENDL;
        }
        else if (isStatic() && mSpatialBridge.notNull())
        {
            LL_ERRS() << "Static drawable has spatial bridge!" << LL_ENDL;
        }
    }
#endif
}

class LLOctreeMarkNotCulled: public OctreeTraveler
{
public:
    LLCamera* mCamera;

    LLOctreeMarkNotCulled(LLCamera* camera_in) : mCamera(camera_in) { }

    virtual void traverse(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);
        group->setVisible();
        OctreeTraveler::traverse(node);
    }

    void visit(const OctreeNode* branch)
    {
        gPipeline.markNotCulled((LLSpatialGroup*) branch->getListener(0), *mCamera);
    }
};

bool LLSpatialBridge::avatarGateOpen() const
{
    LLViewerObject *vobj = mDrawable->getVObj();
    if (vobj && vobj->isAttachment() && !vobj->isHUDAttachment())
    {
        LLDrawable* av;
        LLDrawable* parent = mDrawable->getParent();

        if (parent)
        {
            LLViewerObject* objparent = parent->getVObj();
            av = objparent->mDrawable;
            (void)av;

            bool impostor = false;
            bool loaded = false;
            if (objparent->isAvatar())
            {
                LLVOAvatar* avatarp = (LLVOAvatar*) objparent;
                if (avatarp->isVisible())
                {
                    impostor = objparent->isAvatar() && !LLPipeline::sImpostorRender && ((LLVOAvatar*) objparent)->isImpostor();
                    loaded   = objparent->isAvatar() && ((LLVOAvatar*) objparent)->isFullyLoaded();
                }
                else
                {
                    return false;
                }
            }

            // (stage 9, avatars: the avatar's drawable has no region group any more, and its visible stamp counts twice a frame: the gate had asked for a group and for a stamp at most 1 old. The avatar's own listing by the owners'
            // pass is what LLVOAvatar::isVisible asked above)
            if (impostor ||
                !loaded)
            {
                return false;
            }
        }
    }
    return true;
}

void LLSpatialBridge::hizResult(bool occluded)
{
    if (occluded)
    {
        if (mHizOccludedOnce)
        {
            mHizOccluded = true;
        }
        mHizOccludedOnce = true;
        memcpy(mHizVerdictBox, mHizSentBox, sizeof(mHizVerdictBox));
    }
    else
    {
        mHizOccludedOnce = false;
        mHizOccluded = false;
    }
    mHizVerdictFrame = (U32)LLFrameTimer::getFrameCount();
}

bool LLSpatialBridge::hizHides() const
{
    if (!mHizOccluded || (U32)LLFrameTimer::getFrameCount() - mHizVerdictFrame > 8u)
    {
        return false;
    }
    const LLVector4a* exts = const_cast<LLSpatialBridge*>(this)->getSpatialExtents();
    for (int k = 0; k < 3; ++k)
    {
        const F32 c = 0.5f * (exts[0][k] + exts[1][k]), h = 0.5f * (exts[1][k] - exts[0][k]);
        if (fabsf(c - mHizVerdictBox[k]) + h > mHizVerdictBox[3 + k])
        {
            return false;       // the bridge has moved out of the box the verdict was made for
        }
    }
    return true;
}

bool LLSpatialBridge::inViewFor(LLCamera& camera_in)
{
    if (!gPipeline.hasRenderType(mDrawableType) || !avatarGateOpen())
    {
        return false;
    }
    const LLVector4a* exts = getSpatialExtents();
    LLVector4a center;
    center.setAdd(exts[0], exts[1]);
    center.mul(0.5f);
    LLVector4a size;
    size.setSub(exts[1], exts[0]);
    size.mul(0.5f);
    if ((LLPipeline::shadowRender() && camera_in.AABBInFrustum(center, size)) ||
        LLPipeline::sImpostorRender ||
        (camera_in.AABBInFrustumNoFarClip(center, size) &&
        AABBSphereIntersect(exts[0], exts[1], camera_in.getOrigin(), camera_in.mFrustumCornerDist)))
    {
        return LLPipeline::sImpostorRender || LLPipeline::shadowRender() || LLPipeline::calcPixelArea(center, size, camera_in) >= FORCE_INVISIBLE_AREA;
    }
    return false;
}

void LLSpatialBridge::setVisible(LLCamera& camera_in, std::vector<LLDrawable*>* results, bool for_select)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (!gPipeline.hasRenderType(mDrawableType))
    {
        return;
    }


    //HACK don't draw attachments for avatars that haven't been visible in more than a frame
    if (!avatarGateOpen())
    {
        return;
    }


    // <FS> the parallel shadow cascade preparation culls the same bridge from several threads: rebound() writes and the walk
    // below reads, so one thread at a time per bridge. Control avatar bridges read each other's bounds in rebound(): they share one lock.
    std::unique_lock<std::mutex> bridge_lock;
    if (LLPipeline::sParallelPrep)
    {
        static std::mutex sBridgeLocks[16];
        const size_t stripe = (mDrawableType == LLPipeline::RENDER_TYPE_CONTROL_AV) ? 0 : 1 + ((reinterpret_cast<uintptr_t>(this) >> 6) % 15);
        bridge_lock = std::unique_lock<std::mutex>(sBridgeLocks[stripe]);
    }

    LLSpatialGroup* group = (LLSpatialGroup*) mOctree->getListener(0);
    group->rebound();

    LLVector4a center;
    const LLVector4a* exts = getSpatialExtents();
    center.setAdd(exts[0], exts[1]);
    center.mul(0.5f);
    LLVector4a size;
    size.setSub(exts[1], exts[0]);
    size.mul(0.5f);

    if ((LLPipeline::shadowRender() && camera_in.AABBInFrustum(center, size)) ||
        LLPipeline::sImpostorRender ||
        (camera_in.AABBInFrustumNoFarClip(center, size) &&
        AABBSphereIntersect(exts[0], exts[1], camera_in.getOrigin(), camera_in.mFrustumCornerDist)))
    {
        if (!LLPipeline::sImpostorRender &&
            !LLPipeline::shadowRender() &&
            LLPipeline::calcPixelArea(center, size, camera_in) < FORCE_INVISIBLE_AREA)
        {
            return;
        }

        LLDrawable::setVisible(camera_in);

        if (for_select)
        {
            results->push_back(mDrawable);
            if (mDrawable->getVObj())
            {
                LLViewerObject::const_child_list_t& child_list = mDrawable->getVObj()->getChildren();
                for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                     iter != child_list.end(); iter++)
                {
                    LLViewerObject* child = *iter;
                    LLDrawable* drawable = child->mDrawable;
                    results->push_back(drawable);
                }
            }
        }
        else
        {
            LLCamera trans_camera = transformCamera(camera_in);
            LLOctreeMarkNotCulled culler(&trans_camera);
            culler.traverse(mOctree);
        }
    }
}

void LLSpatialBridge::updateDistance(LLCamera& camera_in, bool force_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (mDrawable == NULL)
    {
        if (LLPipeline::sSortSink)
        {
            LLPipeline::sSortSink->push_back({ LLPipeline::SortDelta::BRIDGE_DEAD, 0, this, nullptr, 0.f });       // <FS> stateSort split
            return;
        }
        if (LLPipeline::sSortTee)
        {
            LLPipeline::sSortTee->push_back({ LLPipeline::SortDelta::BRIDGE_DEAD, 0, this, nullptr, 0.f });
        }
        markDead();
        return;
    }

    if (gShiftFrame)
    {
        return;
    }

    if (mDrawable->getVObj())
    {
        // Don't update if we are part of impostor, unles it's an impostor pass
        if (!LLPipeline::sImpostorRender && mDrawable->getVObj()->isAttachment())
        {
            LLDrawable* parent = mDrawable->getParent();
            if (parent && parent->getVObj())
            {
                LLVOAvatar* av = parent->getVObj()->asAvatar();
                if (av && av->isImpostor())
                {
                    return;
                }
            }
        }

        LLCamera camera = transformCamera(camera_in);

        mDrawable->updateDistance(camera, force_update);

        for (LLViewerObject* child : mDrawable->getVObj()->getChildren())
        {
            LLDrawable* drawable = child->mDrawable;
            if (drawable && !drawable->isDead() && !drawable->isAvatar())
            {
                drawable->updateDistance(camera, force_update);
            }
        }
    }
}

void LLSpatialBridge::makeActive()
{ //it is an error to make a spatial bridge active (it's already active)
    LL_ERRS() << "makeActive called on spatial bridge" << LL_ENDL;
}

void LLSpatialBridge::move(LLDrawable *drawablep, LLSpatialGroup *curp, bool immediate)
{
    LLSpatialPartition::move(drawablep, curp, immediate);
    gPipeline.markMoved(this, false);
}

bool LLSpatialBridge::updateMove()
{
    RK_CPU_ZONE("bridge: updateMove");
    RKAttCensus::Scope att_census(RKAttCensus::sBridgeMove, mDrawable);
    llassert_always(mDrawable);
    llassert_always(mDrawable->mVObjp);
    llassert_always(mDrawable->getRegion());
    mOctree->balance();
    // D2a-bridge (PLAN-avatars-100.md): made here, in the avatar's job as on the main thread (it used to be a BRIDGE_MOVE op replayed after the loop). A partition move into this bridge's octree is replayed
    // after the loop, and the replay makes the extents again (LLPipeline::applyAvOpsRange, MOVE_PARTITION). Compared over a crowd lap against the replayed op: 0 differences in 40,000 bridge moves.
    updateSpatialExtents();
    return true;
}


void LLSpatialBridge::shiftPos(const LLVector4a& vec)
{
    LLDrawable::shift(vec);
}

void LLSpatialBridge::cleanupReferences()
{
    LLDrawable::cleanupReferences();
    if (mDrawable)
    {
        mDrawable->setGroup(NULL);

        if (mDrawable->getVObj())
        {
            LLViewerObject::const_child_list_t& child_list = mDrawable->getVObj()->getChildren();
            for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                 iter != child_list.end(); iter++)
            {
                LLViewerObject* child = *iter;
                LLDrawable* drawable = child->mDrawable;
                if (drawable)
                {
                    drawable->setGroup(NULL);
                }
                }
            }

        // <FS:ND> setting the pointer to 0 can delete the drawable, referencing it with setSpatialBridge after that is illegal.

        // LLDrawable* drawablep = mDrawable;
        LLPointer< LLDrawable > drawablep = mDrawable;

        // </FS:ND>

        mDrawable = NULL;
        drawablep->setSpatialBridge(NULL);
    }
}

const LLVector3 LLDrawable::getPositionAgent() const
{
    if (getVOVolume())
    {
        if (isActive())
        {
            LLVector3 pos(0,0,0);
            if (!isRoot())
            {
                pos = mVObjp->getPosition();
            }
            return pos * getRenderMatrix();
        }
        else
        {
            return mVObjp->getPositionAgent();
        }
    }
    else
    {
        return getWorldPosition();
    }
}

bool LLDrawable::isAnimating() const
{
    if (!getVObj())
    {
        return true;
    }

    if (getScale() != mVObjp->getScale())
    {
        return true;
    }

    if (mVObjp->getPCode() == LLViewerObject::LL_VO_PART_GROUP)
    {
        return true;
    }
    if (mVObjp->getPCode() == LLViewerObject::LL_VO_HUD_PART_GROUP)
    {
        return true;
    }

    /*if (!isRoot() && !mVObjp->getAngularVelocity().isExactlyZero())
    { //target omega
        return true;
    }*/

    return false;
}

void LLDrawable::updateFaceSize(S32 idx)
{
    if (mVObjp.notNull())
    {
        mVObjp->updateFaceSize(idx);
    }
}

LLDrawable* LLDrawable::getRoot()
{
    LLDrawable* ret = this;
    while (!ret->isRoot())
    {
        ret = ret->getParent();
    }

    return ret;
}

LLBridgePartition::LLBridgePartition(LLViewerRegion* regionp)
: LLSpatialPartition(0, false, regionp)
{
    mDrawableType = LLPipeline::RENDER_TYPE_VOLUME;
    mPartitionType = LLViewerRegion::PARTITION_BRIDGE;
    mLODPeriod = 16;
    mSlopRatio = 0.25f;
}

LLAvatarPartition::LLAvatarPartition(LLViewerRegion* regionp)
    : LLBridgePartition(regionp)
{
    mDrawableType = LLPipeline::RENDER_TYPE_AVATAR;
    mPartitionType = LLViewerRegion::PARTITION_AVATAR;
}

LLControlAVPartition::LLControlAVPartition(LLViewerRegion* regionp)
    : LLBridgePartition(regionp)
{
    mDrawableType = LLPipeline::RENDER_TYPE_CONTROL_AV;
    mPartitionType = LLViewerRegion::PARTITION_CONTROL_AV;
}

LLHUDBridge::LLHUDBridge(LLDrawable* drawablep, LLViewerRegion* regionp)
: LLVolumeBridge(drawablep, regionp)
{
    mDrawableType = LLPipeline::RENDER_TYPE_HUD;
    mPartitionType = LLViewerRegion::PARTITION_HUD;
    mSlopRatio = 0.0f;
}



void LLHUDBridge::shiftPos(const LLVector4a& vec)
{
    //don't shift hud bridges on region crossing
}

