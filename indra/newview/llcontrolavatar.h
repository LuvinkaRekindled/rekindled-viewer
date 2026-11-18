/**
 * @file llcontrolavatar.h
 * @brief Special dummy avatar used to drive rigged meshes.
 *
 * $LicenseInfo:firstyear=2017&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2017, Linden Research, Inc.
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

#ifndef LL_CONTROLAVATAR_H
#define LL_CONTROLAVATAR_H

#include "llvoavatar.h"
#include "llvovolume.h"

class LLControlAvatar:
    public LLVOAvatar
{
    LOG_CLASS(LLControlAvatar);

    using super = LLVOAvatar;

public:
    LLControlAvatar(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);
    virtual void initInstance(); // Called after construction to initialize the class.
    virtual void markDead();
    virtual ~LLControlAvatar();

    // If this is an attachment, return the avatar it is attached to. Otherwise NULL.
    virtual const LLVOAvatar *getAttachedAvatar() const;
    virtual LLVOAvatar *getAttachedAvatar();

    void getNewConstraintFixups(LLVector3& new_pos_constraint, F32& new_scale_constraint) const;
    void matchVolumeTransform();

    // <FS> PLAN-avatars.md stage C5d: an animated object worn by another avatar has the placement of its skeleton's root made ONCE per frame, in its own
    // pool job, from the HOST's flat world array, after the host's pose job (the motion batch runs such control avatars in a second phase). Before, the root was placed three times (the main thread at the start of the
    // frame from last frame's host joints, its own job at the same time as the host's, and the host's attachment job through the drawable's update, a write into this avatar's skeleton from another avatar's thread).
    bool hostJobOwned() const;
    void placeFromHost();           // main thread, after the host's completion: the root from the host's final pose (matchVolumeTransform)
    void updateVolumeGeom();

    void setGlobalScale(F32 scale);
    void recursiveScaleJoint(LLJoint *joint, F32 factor);
    static LLControlAvatar *createControlAvatar(LLVOVolume *obj);

    // Delayed kill so we don't make graphics pipeline unhappy calling
    // markDead() inside other graphics pipeline operations.
    void markForDeath();

    virtual void idleUpdate(LLAgent &agent, const F64 &time);
    virtual bool computeNeedsUpdate();
    virtual bool updateCharacter(LLAgent &agent);

    void getAnimatedVolumes(std::vector<LLVOVolume*>& volumes);
    void updateAnimations();

    virtual LLViewerObject* lineSegmentIntersectRiggedAttachments(
        const LLVector4a& start, const LLVector4a& end,
        S32 face = -1,                    // which face to check, -1 = ALL_SIDES
        bool pick_transparent = false,
        bool pick_rigged = false,
        bool pick_unselectable = true,
        S32* face_hit = NULL,             // which face was hit
        LLVector4a* intersection = NULL,   // return the intersection point
        LLVector2* tex_coord = NULL,      // return the texture coordinates of the intersection point
        LLVector4a* normal = NULL,         // return the surface normal at the intersection point
        LLVector4a* tangent = NULL);     // return the surface tangent at the intersection point

    virtual void    updateDebugText();

    virtual std::string getFullname() const;

    virtual bool shouldRenderRigged() const;

    virtual bool isImpostor();
    virtual bool isBuddy() const { return false; }

    bool isTooComplex() const; // <FS:Ansariel> FIRE-29012: Standalone animesh avatars get affected by complexity limit


    bool mPlaying;

    F32 mGlobalScale;

    LLVOVolume *mRootVolp;
    class LLControlAVBridge* mControlAVBridge;

    bool mMarkedForDeath;

    LLVector3 mPositionConstraintFixup;
    F32 mScaleConstraintFixup;

    static const F32 MAX_LEGAL_OFFSET;
    static const F32 MAX_LEGAL_SIZE;

    static void onRegionChanged();
    bool mRegionChanged;
    static boost::signals2::connection sRegionChangedSlot;
};

typedef std::map<LLUUID, S32> signaled_animation_map_t;
typedef std::map<LLUUID, signaled_animation_map_t> object_signaled_animation_map_t;

// Stores information about previously requested animations, by object id.
class LLObjectSignaledAnimationMap: public LLSingleton<LLObjectSignaledAnimationMap>
{
    LLSINGLETON_EMPTY_CTOR(LLObjectSignaledAnimationMap);

public:
    object_signaled_animation_map_t mMap;

    object_signaled_animation_map_t& getMap() { return mMap; }
};

#endif //LL_CONTROLAVATAR_H
