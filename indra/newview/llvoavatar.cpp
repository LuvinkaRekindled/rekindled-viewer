/**
 * @File llvoavatar.cpp
 * @brief Implementation of LLVOAvatar class which is a derivation of LLViewerObject
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
#include <mutex>
#include <atomic>
#include "llthread.h"            // <FS> on_main_thread() (the palette catcher)
#include <sstream>
#include "llvkplatform.h"
#include "llgpuzonetimer.h"
#include <chrono>

#include "llvoavatar.h"
#include "llavatartable.h"
#include "llretirelist.h"
#include "llavatarref.h"
#include <map>      // <FS> LL_VK_PALSTATS

#include <stdio.h>
#include <ctype.h>
#include <sstream>

#include "llaudioengine.h"
#include "noise.h"
#include "sound_ids.h"
#include "raytrace.h"

#include "aoengine.h"           // <FS:Zi> Animation Overrider
#include "llagent.h" //  Get state values from here
#include "llagentbenefits.h"
#include "llagentcamera.h"
#include "llagentwearables.h"
#include "llanimationstates.h"
#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llavatarrendernotifier.h"
#include "llcontrolavatar.h"
#include "llexperiencecache.h"
#include "llphysicsmotion.h"
#include "llviewercontrol.h"
#include "llcallingcard.h"      // IDEVO for LLAvatarTracker
#include "lldrawpoolavatar.h"
#include "lldriverparam.h"
#include "llpolyskeletaldistortion.h"
#include "lleditingmotion.h"
#include "llemote.h"
#include "llfloatertools.h"
#include "llheadrotmotion.h"
#include "llhudeffecttrail.h"
#include "llhudmanager.h"
#include "llhudnametag.h"
#include "llhudtext.h"              // for mText/mDebugText
#include "llimview.h"
#include "llinitparam.h"
#include "llkeyframefallmotion.h"
#include "llkeyframestandmotion.h"
#include "llkeyframewalkmotion.h"
#include "llmanipscale.h"  // for get_default_max_prim_scale()
#include "llmeshrepository.h"
#include "llmutelist.h"
#include "llmoveview.h"
#include "llnotificationsutil.h"
#include "llphysicsshapebuilderutil.h"
#include "llquantize.h"
#include "llrand.h"
#include "llregionhandle.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llsprite.h"
#include "lltargetingmotion.h"
#include "lltoolmgr.h"
#include "lltoolmorph.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llviewertexlayer.h"
#include "llviewertexturelist.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewerstats.h"
#include "llviewerwearable.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"
#include "llattcensus.h"
#include "llviewershadermgr.h"
#include "llsky.h"
#include "llanimstatelabels.h"
#include "lltrans.h"
#include "llappearancemgr.h"
// [RLVa:KB] - Checked: RLVa-2.0.1
#include "rlvactions.h"
#include "rlvhandler.h"
#include "rlvmodifiers.h"
// [/RLVa:KB]

#include "llgesturemgr.h" //needed to trigger the voice gesticulations
#include "llvoiceclient.h"
#include "llvoicevisualizer.h" // Ventrella

#include "lldebugmessagebox.h"
#include "llsdutil.h"
#include "llscenemonitor.h"
#include "llsdserialize.h"
#include "llrendersphere.h"
#include "llskinningutil.h"

#include "llperfstats.h"

#include <boost/lexical_cast.hpp>

#include "fsavatarrenderpersistence.h"
#include "fscommon.h"
#include "fsdata.h"
#include "fsdiscordconnect.h" // <FS:LO> tapping a place that happens on landing in world to start up discord
#include "fslslbridge.h" // <FS:PP> Movelock position refresh
#include "lfsimfeaturehandler.h"    // <FS:CR> Opensim
#include "lggcontactsets.h"
#include "llcontrol.h"
#include "llfilepicker.h"   // <FS:CR> FIRE-8893 - Dump archetype xml to user defined location
#include "llhudeffectresetskeleton.h"
#include "llnetmap.h"
#include "llsidepanelappearance.h"
#include "llviewermenufile.h"
#include "llviewernetwork.h"    // [FS:CR] isInSecondlife()


extern F32 SPEED_ADJUST_MAX;
extern F32 SPEED_ADJUST_MAX_SEC;
extern F32 ANIM_SPEED_MAX;
extern F32 ANIM_SPEED_MIN;
extern U32 JOINT_COUNT_REQUIRED_FOR_FULLRIG;

const F32 MAX_HOVER_Z = 3.0;
const F32 MIN_HOVER_Z = -3.0;

const F32 MIN_ATTACHMENT_COMPLEXITY = 0.f;
const F32 DEFAULT_MAX_ATTACHMENT_COMPLEXITY = 1.0e6f;

// Unlike with 'self' avatar, server doesn't inform viewer about
// expected attachments so viewer has to wait to see if anything
// else will arrive
const F32 FIRST_APPEARANCE_CLOUD_MIN_DELAY = 3.f; // seconds
const F32 FIRST_APPEARANCE_CLOUD_MAX_DELAY = 15.f;
const F32 FIRST_APPEARANCE_CLOUD_IMPOSTOR_MODIFIER = 1.25f;

using namespace LLAvatarAppearanceDefines;

//-----------------------------------------------------------------------------
// Global constants
//-----------------------------------------------------------------------------
const LLUUID ANIM_AGENT_BODY_NOISE = LLUUID("9aa8b0a6-0c6f-9518-c7c3-4f41f2c001ad"); //"body_noise"
const LLUUID ANIM_AGENT_BREATHE_ROT = LLUUID("4c5a103e-b830-2f1c-16bc-224aa0ad5bc8");  //"breathe_rot"
const LLUUID ANIM_AGENT_EDITING = LLUUID("2a8eba1d-a7f8-5596-d44a-b4977bf8c8bb");  //"editing"
const LLUUID ANIM_AGENT_EYE = LLUUID("5c780ea8-1cd1-c463-a128-48c023f6fbea");  //"eye"
const LLUUID ANIM_AGENT_FLY_ADJUST = LLUUID("db95561f-f1b0-9f9a-7224-b12f71af126e");  //"fly_adjust"
const LLUUID ANIM_AGENT_HAND_MOTION = LLUUID("ce986325-0ba7-6e6e-cc24-b17c4b795578");  //"hand_motion"
const LLUUID ANIM_AGENT_HEAD_ROT = LLUUID("e6e8d1dd-e643-fff7-b238-c6b4b056a68d");  //"head_rot"
const LLUUID ANIM_AGENT_PELVIS_FIX = LLUUID("0c5dd2a2-514d-8893-d44d-05beffad208b");  //"pelvis_fix"
const LLUUID ANIM_AGENT_TARGET = LLUUID("0e4896cb-fba4-926c-f355-8720189d5b55");  //"target"
const LLUUID ANIM_AGENT_WALK_ADJUST = LLUUID("829bc85b-02fc-ec41-be2e-74cc6dd7215d");  //"walk_adjust"
const LLUUID ANIM_AGENT_PHYSICS_MOTION = LLUUID("7360e029-3cb8-ebc4-863e-212df440d987");  //"physics_motion"


//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
const F32 DELTA_TIME_MIN = 0.01f;   // we clamp measured delta_time to this
const F32 DELTA_TIME_MAX = 0.2f;    // range to insure stability of computations.

const F32 PELVIS_LAG_FLYING     = 0.22f;// pelvis follow half life while flying
const F32 PELVIS_LAG_WALKING    = 0.4f; // ...while walking
const F32 PELVIS_LAG_MOUSELOOK = 0.15f;
const F32 MOUSELOOK_PELVIS_FOLLOW_FACTOR = 0.5f;
const F32 TORSO_NOISE_AMOUNT = 1.0f;    // Amount of deviation from up-axis, in degrees
const F32 TORSO_NOISE_SPEED = 0.2f; // Time scale factor on torso noise.

const F32 BREATHE_ROT_MOTION_STRENGTH = 0.05f;

const S32 MIN_REQUIRED_PIXEL_AREA_BODY_NOISE = 10000;
const S32 MIN_REQUIRED_PIXEL_AREA_BREATHE = 10000;
const S32 MIN_REQUIRED_PIXEL_AREA_PELVIS_FIX = 40;

const S32 TEX_IMAGE_SIZE_OTHER = 512 / 4;  // The size of local textures for other (!isSelf()) avatars

const F32 HEAD_MOVEMENT_AVG_TIME = 0.9f;

const S32 MORPH_MASK_REQUESTED_DISCARD = 0;

const F32 MAX_STANDOFF_FROM_ORIGIN = 3;
const F32 MAX_STANDOFF_DISTANCE_CHANGE = 32;

// Discard level at which to switch to baked textures
// Should probably be 4 or 3, but didn't want to change it while change other logic - SJB
const S32 SWITCH_TO_BAKED_DISCARD = 5;

const F32 HOVER_EFFECT_MAX_SPEED = 3.f;
const F32 HOVER_EFFECT_STRENGTH = 0.f;
const F32 UNDERWATER_EFFECT_STRENGTH = 0.1f;
const F32 UNDERWATER_FREQUENCY_DAMP = 0.33f;
const F32 APPEARANCE_MORPH_TIME = 0.65f;
const F32 TIME_BEFORE_MESH_CLEANUP = 5.f; // seconds
const S32 AVATAR_RELEASE_THRESHOLD = 10; // number of avatar instances before releasing memory
const F32 FOOT_GROUND_COLLISION_TOLERANCE = 0.25f;
const F32 AVATAR_LOD_TWEAK_RANGE = 0.7f;
const S32 MAX_BUBBLE_CHAT_LENGTH = DB_CHAT_MSG_STR_LEN;
const S32 MAX_BUBBLE_CHAT_UTTERANCES = 12;
const F32 CHAT_FADE_TIME = 8.0;
const F32 BUBBLE_CHAT_TIME = CHAT_FADE_TIME * 3.f;
const F32 NAMETAG_UPDATE_THRESHOLD = 0.3f;
const F32 NAMETAG_VERTICAL_SCREEN_OFFSET = 25.f;
const F32 NAMETAG_VERT_OFFSET_WEIGHT = 0.17f;

const U32 LLVOAvatar::VISUAL_COMPLEXITY_UNKNOWN = 0;
const F64 HUD_OVERSIZED_TEXTURE_DATA_SIZE = 1024 * 1024;

const F32 MAX_TEXTURE_WAIT_TIME_SEC = 60.f;
const F32 MAX_ATTACHMENT_WAIT_TIME_SEC = 60;

const S32 MIN_NONTUNED_AVS = 5;

enum ERenderName
{
    RENDER_NAME_NEVER,
    RENDER_NAME_ALWAYS,
    RENDER_NAME_FADE
};

enum ERenderGroupTitle
{
    RENDER_GROUP_TITLE_NEVER,
    RENDER_GROUP_TITLE_SELF,
    RENDER_GROUP_TITLE_ALWAYS
};

// <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
// Different settings based on FSImpostorAvatarExclude
enum EImpostorAvatarExclude
{
    NONE, // Default, no avatar excluded
    USER, // Check for mIsAnimesh only, exclude user Avatar's which are Animesh or have Animesh attachments
    CONTROL, // Check for mIsControlAvatar only, exclude control avatars (avtars which don't have a user UUID assigned)
    BOTH // Check both mIsAnimesh or mIsControlAvatar, exclude both User Amimesh and Control avatars.
};
// </FS:minerjr> [FIRE-35735]

#define JELLYDOLLS_SHOULD_IMPOSTOR

//-----------------------------------------------------------------------------
// Callback data
//-----------------------------------------------------------------------------

struct LLTextureMaskData
{
    LLTextureMaskData( const LLUUID& id ) :
        mAvatarID(id),
        mLastDiscardLevel(S32_MAX)
    {}
    LLUUID              mAvatarID;
    S32                 mLastDiscardLevel;
};

/*********************************************************************************
 **                                                                             **
 ** Begin private LLVOAvatar Support classes
 **
 **/


struct LLAppearanceMessageContents: public LLRefCount
{
    LLAppearanceMessageContents():
        mAppearanceVersion(-1),
        mParamAppearanceVersion(-1),
        mCOFVersion(LLViewerInventoryCategory::VERSION_UNKNOWN)
    {
    }
    LLTEContents mTEContents;
    S32 mAppearanceVersion;
    S32 mParamAppearanceVersion;
    S32 mCOFVersion;
    // For future use:
    //U32 appearance_flags = 0;
    std::vector<F32> mParamWeights;
    std::vector<LLVisualParam*> mParams;
    LLVector3 mHoverOffset;
    bool mHoverOffsetWasSet;
};


//-----------------------------------------------------------------------------
// class LLBodyNoiseMotion
//-----------------------------------------------------------------------------
class LLBodyNoiseMotion :
    public LLMotion
{
public:
    // Constructor
    LLBodyNoiseMotion(const LLUUID &id)
        : LLMotion(id)
    {
        mName = "body_noise";
        mTorsoState = new LLJointState;
    }

    // Destructor
    virtual ~LLBodyNoiseMotion() { }

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID &id) { return new LLBodyNoiseMotion(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.0; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.0; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::HIGH_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return ADDITIVE_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_BODY_NOISE; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        if( !mTorsoState->setJoint( character->getJoint("mTorso") ))
        {
            return STATUS_FAILURE;
        }

        mTorsoState->setUsage(LLJointState::ROT);

        addJointState( mTorsoState );
        return STATUS_SUCCESS;
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        F32 nx[2];
        nx[0]=time*TORSO_NOISE_SPEED;
        nx[1]=0.0f;
        F32 ny[2];
        ny[0]=0.0f;
        ny[1]=time*TORSO_NOISE_SPEED;
        F32 noiseX = noise2(nx);
        F32 noiseY = noise2(ny);

        F32 rx = TORSO_NOISE_AMOUNT * DEG_TO_RAD * noiseX / 0.42f;
        F32 ry = TORSO_NOISE_AMOUNT * DEG_TO_RAD * noiseY / 0.42f;
        LLQuaternion tQn;
        tQn.setQuat( rx, ry, 0.0f );
        mTorsoState->setRotation( tQn );

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mTorsoState;
};

//-----------------------------------------------------------------------------
// class LLBreatheMotionRot
//-----------------------------------------------------------------------------
class LLBreatheMotionRot :
    public LLMotion
{
public:
    // Constructor
    LLBreatheMotionRot(const LLUUID &id) :
        LLMotion(id),
        mBreatheRate(1.f),
        mCharacter(NULL)
    {
        mName = "breathe_rot";
        mChestState = new LLJointState;
    }

    // Destructor
    virtual ~LLBreatheMotionRot() {}

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID &id) { return new LLBreatheMotionRot(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.0; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.0; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::MEDIUM_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return NORMAL_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_BREATHE; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        mCharacter = character;
        bool success = true;

        if ( !mChestState->setJoint( character->getJoint( "mChest" ) ) )
        {
            success = false;
        }

        if ( success )
        {
            mChestState->setUsage(LLJointState::ROT);
            addJointState( mChestState );
        }

        if ( success )
        {
            return STATUS_SUCCESS;
        }
        else
        {
            return STATUS_FAILURE;
        }
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        mBreatheRate = 1.f;

        F32 breathe_amt = (sinf(mBreatheRate * time) * BREATHE_ROT_MOTION_STRENGTH);

        mChestState->setRotation(LLQuaternion(breathe_amt, LLVector3(0.f, 1.f, 0.f)));

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mChestState;
    F32                 mBreatheRate;
    LLCharacter*        mCharacter;
};

//-----------------------------------------------------------------------------
// class LLPelvisFixMotion
//-----------------------------------------------------------------------------
class LLPelvisFixMotion :
    public LLMotion
{
public:
    // Constructor
    LLPelvisFixMotion(const LLUUID &id)
        : LLMotion(id), mCharacter(NULL)
    {
        mName = "pelvis_fix";

        mPelvisState = new LLJointState;
    }

    // Destructor
    virtual ~LLPelvisFixMotion() { }

public:
    //-------------------------------------------------------------------------
    // functions to support MotionController and MotionRegistry
    //-------------------------------------------------------------------------
    // static constructor
    // all subclasses must implement such a function and register it
    static LLMotion *create(const LLUUID& id) { return new LLPelvisFixMotion(id); }

public:
    //-------------------------------------------------------------------------
    // animation callbacks to be implemented by subclasses
    //-------------------------------------------------------------------------

    // motions must specify whether or not they loop
    virtual bool getLoop() { return true; }

    // motions must report their total duration
    virtual F32 getDuration() { return 0.0; }

    // motions must report their "ease in" duration
    virtual F32 getEaseInDuration() { return 0.5f; }

    // motions must report their "ease out" duration.
    virtual F32 getEaseOutDuration() { return 0.5f; }

    // motions must report their priority
    virtual LLJoint::JointPriority getPriority() { return LLJoint::LOW_PRIORITY; }

    virtual LLMotionBlendType getBlendType() { return NORMAL_BLEND; }

    // called to determine when a motion should be activated/deactivated based on avatar pixel coverage
    virtual F32 getMinPixelArea() { return MIN_REQUIRED_PIXEL_AREA_PELVIS_FIX; }

    // run-time (post constructor) initialization,
    // called after parameters have been set
    // must return true to indicate success and be available for activation
    virtual LLMotionInitStatus onInitialize(LLCharacter *character)
    {
        mCharacter = character;

        if (!mPelvisState->setJoint( character->getJoint("mPelvis")))
        {
            return STATUS_FAILURE;
        }

        mPelvisState->setUsage(LLJointState::POS);

        addJointState( mPelvisState );
        return STATUS_SUCCESS;
    }

    // called when a motion is activated
    // must return true to indicate success, or else
    // it will be deactivated
    virtual bool onActivate() { return true; }

    // called per time step
    // must return true while it is active, and
    // must return false when the motion is completed.
    virtual bool onUpdate(F32 time, U8* joint_mask)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        mPelvisState->setPosition(LLVector3::zero);

        return true;
    }

    // called when a motion is deactivated
    virtual void onDeactivate() {}

private:
    //-------------------------------------------------------------------------
    // joint states to be animated
    //-------------------------------------------------------------------------
    LLPointer<LLJointState> mPelvisState;
    LLCharacter* mCharacter;
};

/**
 **
 ** End LLVOAvatar Support classes
 **                                                                             **
 *********************************************************************************/


//-----------------------------------------------------------------------------
// Static Data
//-----------------------------------------------------------------------------
U32 LLVOAvatar::sMaxNonImpostors = 12; // Set from RenderAvatarMaxNonImpostors
bool LLVOAvatar::sLimitNonImpostors = false; // True unless RenderAvatarMaxNonImpostors is 0 (unlimited)
F32 LLVOAvatar::sRenderDistance = 256.f;
S32 LLVOAvatar::sNumVisibleAvatars = 0;
S32 LLVOAvatar::sNumLODChangesThisFrame = 0;
std::vector<LLPointer<LLVOAvatar> >* LLVOAvatar::sMotionUpdateBatch = nullptr;

// const LLUUID LLVOAvatar::sStepSoundOnLand("e8af4a28-aa83-4310-a7c4-c047e15ea0df"); - <FS:PP> Commented out for FIRE-3169: Option to change the default footsteps sound
const LLUUID LLVOAvatar::sStepSounds[LL_MCODE_END] =
{
    SND_STONE_RUBBER,
    SND_METAL_RUBBER,
    SND_GLASS_RUBBER,
    SND_WOOD_RUBBER,
    SND_FLESH_RUBBER,
    SND_RUBBER_PLASTIC,
    SND_RUBBER_RUBBER
};

S32 LLVOAvatar::sRenderName = RENDER_NAME_ALWAYS;
S32 LLVOAvatar::sRenderGroupTitles = RENDER_GROUP_TITLE_ALWAYS;
S32 LLVOAvatar::sNumVisibleChatBubbles = 0;
bool LLVOAvatar::sDebugInvisible = false;
bool LLVOAvatar::sShowAttachmentPoints = false;
bool LLVOAvatar::sShowAnimationDebug = false;
bool LLVOAvatar::sVisibleInFirstPerson = false;
F32 LLVOAvatar::sLODFactor = 1.f;
F32 LLVOAvatar::sPhysicsLODFactor = 1.f;
bool LLVOAvatar::sJointDebug            = false;
F32 LLVOAvatar::sUnbakedTime = 0.f;
F32 LLVOAvatar::sUnbakedUpdateTime = 0.f;
F32 LLVOAvatar::sGreyTime = 0.f;
F32 LLVOAvatar::sGreyUpdateTime = 0.f;
LLPointer<LLViewerTexture> LLVOAvatar::sCloudTexture = NULL;
std::vector<LLUUID> LLVOAvatar::sAVsIgnoringARTLimit;
S32 LLVOAvatar::sAvatarsNearby = 0;

//-----------------------------------------------------------------------------
// Helper functions
//-----------------------------------------------------------------------------
static F32 calc_bouncy_animation(F32 x);

//-----------------------------------------------------------------------------
// LLVOAvatar()
//-----------------------------------------------------------------------------
LLVOAvatar::LLVOAvatar(const LLUUID& id,
                       const LLPCode pcode,
                       LLViewerRegion* regionp) :
    LLAvatarAppearance(&gAgentWearables),
    LLViewerObject(id, pcode, regionp),
    mSpecialRenderMode(0),
    mAttachmentSurfaceArea(0.f),
    mAttachmentVisibleTriangleCount(0),
    mAttachmentEstTriangleCount(0.f),
    mReportedVisualComplexity(VISUAL_COMPLEXITY_UNKNOWN),
    mTurning(false),
    mLastSkeletonSerialNum( 0 ),
    mIsSitting(false),
    mTimeVisible(),
    mTyping(false),
    mMeshValid(false),
    mVisible(false),
    mLastImpostorUpdateFrameTime(0.f),
    mLastImpostorUpdateReason(0),
    mWindFreq(0.f),
    mRipplePhase( 0.f ),
    mBelowWater(false),
    mLastAppearanceBlendTime(0.f),
    mAppearanceAnimating(false),
    mNameIsSet(false),
    // <FS:Ansariel> FIRE-13414: Avatar name isn't updated when the simulator sends a new name
    mNameFirstname(),
    mNameLastname(),
    // </FS:Ansariel>
    mTitle(),
    // <FS:Ansariel> Show Arc in nametag (for Jelly Dolls)
    mNameArc(0),
    mNameArcColor(LLColor4::white),
    // </FS:Ansariel>
    mNameAway(false),
    mNameDoNotDisturb(false),
    mNameAutoResponse(false), // <FS:Ansariel> Show auto-response in nametag,
    mNameIsTyping(false), // <FS:Ansariel> FIRE-3475: Show typing in nametag
    mNameMute(false),
    mNameAppearance(false),
    mNameFriend(false),
    mNameAlpha(0.f),
    mRenderGroupTitles(sRenderGroupTitles),
    mNameCloud(false),
    mFirstTEMessageReceived( false ),
    mFirstAppearanceMessageReceived( false ),
    mCulled( false ),
    mVisibilityRank(0),
    mNeedsSkin(false),
    mLastSkinTime(0.f),
    mUpdatePeriod(1),
    mOverallAppearance(AOA_INVISIBLE),
    mVisualComplexityStale(true),
    mVisuallyMuteSetting(AV_RENDER_NORMALLY),
    mMutedAVColor(LLColor4::white /* used for "uninitialize" */),
    mFirstFullyVisible(true),
    mWaitingForMeshes(false),
    mFirstDecloudTime(-1.f),
    mFullyLoaded(false),
    mPreviousFullyLoaded(false),
    mFullyLoadedInitialized(false),
    mLastCloudAttachmentCount(-1),
    mFullyLoadedFrameCounter(0),
    mVisualComplexity(VISUAL_COMPLEXITY_UNKNOWN),
    mLoadedCallbacksPaused(false),
    mLoadedCallbackTextures(0),
    mRenderUnloadedAvatar(LLCachedControl<bool>(gSavedSettings, "RenderUnloadedAvatar", false)),
    mLastRezzedStatus(-1),
    mIsEditingAppearance(false),
    mUseLocalAppearance(false),
    // <FS:Ansariel> [Legacy Bake]
    mUseServerBakes(false),
    // </FS:Ansariel> [Legacy Bake]
    mLastUpdateRequestCOFVersion(-1),
    mLastUpdateReceivedCOFVersion(-1),
    mCachedMuteListUpdateTime(0),
    mCachedInMuteList(false),
    mIsControlAvatar(false),
    mIsUIAvatar(false),
    mEnableDefaultMotions(true)
{
    LL_DEBUGS("AvatarRender") << "LLVOAvatar Constructor (0x" << this << ") id:" << mID << LL_ENDL;

    //VTResume();  // VTune
    setHoverOffset(LLVector3(0.0, 0.0, 0.0));

    // mVoiceVisualizer is created by the hud effects manager and uses the HUD Effects pipeline
    const bool needsSendToSim = false; // currently, this HUD effect doesn't need to pack and unpack data to do its job
    mVoiceVisualizer = ( LLVoiceVisualizer *)LLHUDManager::getInstance()->createViewerEffect( LLHUDObject::LL_HUD_EFFECT_VOICE_VISUALIZER, needsSendToSim );

    LL_DEBUGS("Avatar","Message") << "LLVOAvatar Constructor (0x" << this << ") id:" << mID << LL_ENDL;
    mPelvisp = NULL;

    mDirtyMesh = 2; // Dirty geometry, need to regenerate.
    mMeshTexturesDirty = false;
    mHeadp = NULL;


    // set up animation variables
    mSpeed = 0.f;
    setAnimationData("Speed", &mSpeed);

    mNeedsImpostorUpdate = true;
    mLastImpostorUpdateReason = 0;
    mNeedsAnimUpdate = true;

    mNeedsExtentUpdate = true;

    mImpostorDistance = 0;
    mImpostorPixelArea = 0;

    setNumTEs(TEX_NUM_INDICES);

    mbCanSelect = true;

    mSignaledAnimations.clear();
    mPlayingAnimations.clear();

    mWasOnGroundLeft = false;
    mWasOnGroundRight = false;

    mTimeLast = 0.0f;
    mSpeedAccum = 0.0f;

    mRippleTimeLast = 0.f;

    mInAir = false;

    mStepOnLand = true;
    mStepMaterial = 0;

    mLipSyncActive = false;
    mOohMorph      = NULL;
    mAahMorph      = NULL;

    mCurrentGesticulationLevel = 0;

    mFirstAppearanceMessageTimer.reset();
    mRuthTimer.reset();
    mRuthDebugTimer.reset();
    mDebugExistenceTimer.reset();
    mLastAppearanceMessageTimer.reset();

    if(LLSceneMonitor::getInstance()->isEnabled())
    {
        LLSceneMonitor::getInstance()->freezeAvatar((LLCharacter*)this);
    }

    // <FS:Ansariel> [FS Persisted Avatar Render Settings]
    //mVisuallyMuteSetting = LLVOAvatar::VisualMuteSettings(LLRenderMuteList::getInstance()->getSavedVisualMuteSetting(getID()));
    mVisuallyMuteSetting = FSAvatarRenderPersistence::instance().getAvatarRenderSettings(id);

    sInstances.push_back(this);
}

//<FS:Beq> BOM constrain number of bake requests when BOM not supported
S32 LLVOAvatar::getNumBakes() const
{
#ifdef OPENSIM
    // BAKED_LEFT_ARM is equal to the pre-BOM BAKED_NUM_INDICES
    if(getRegion())
    {
        // LL_INFOS("BOMOS")
        //              << getFullname()
        //              << "Using avatar region settings [" << getRegion()->getName() << "]"
        //              << " bakesOnMesh = " << static_cast<const char *>(getRegion()->bakesOnMeshEnabled()?"True":"False")
        //              << LL_ENDL;
        return getRegion()->getRegionMaxBakes();
    }
    // LL_INFOS("BOMOS")
    //              << " Using fallback settings"
    //              << " bakesOnMesh = " << static_cast<const char *>(LLGridManager::instance().isInSecondLife()?"True":"False")
    //              << LL_ENDL;
    // fallback, in SL assume BOM, elsewhere assume not.
    return LLGridManager::instance().isInSecondLife()?BAKED_NUM_INDICES:BAKED_LEFT_ARM;
#else
    return BAKED_NUM_INDICES;
#endif
}

// U8 LLVOAvatar::getNumTEs() const
// {
// #ifdef OPENSIM
//  // TEX_HEAD_UNIVERSAL_TATTOO is equal to the pre-BOM TEX_NUM_INDICES
//  if(!mTEImages){return 0;}
//  if(getRegion())
//  {
//      return getRegion()->getRegionMaxTEs();
//  }
//  // fallback, in SL assume BOM, elsewhere assume not.
//  return LLGridManager::instance().isInSecondLife()?TEX_NUM_INDICES:TEX_HEAD_UNIVERSAL_TATTOO;
// #else
//  return TEX_NUM_INDICES;
// #endif
// }
//</FS:Beq>
std::string LLVOAvatar::avString() const
{
    if (isControlAvatar())
    {
        return " " + getFullname() + " ";
    }

    std::string status = LLVOAvatar::rezStatusToString(getRezzedStatus());
    return " Avatar '" + getDebugName() + "' " + status + " ";
}

void LLVOAvatar::debugAvatarRezTime(std::string notification_name, std::string comment)
{
    if (gDisconnected)
    {
        // If we disconected, these values are likely to be invalid and
        // avString() might crash due to a dead sAvatarDictionary
        return;
    }

    LL_INFOS("Avatar") << "REZTIME: [ " << (U32)mDebugExistenceTimer.getElapsedTimeF32()
                       << "sec ]"
                       << avString()
                       << "RuthTimer " << (U32)mRuthDebugTimer.getElapsedTimeF32()
                       << " Notification " << notification_name
                       << " : " << comment
                       << LL_ENDL;

    if (gSavedSettings.getBOOL("DebugAvatarRezTime"))
    {
        LLSD args;
        args["EXISTENCE"] = llformat("%d", (U32)mDebugExistenceTimer.getElapsedTimeF32());
        args["TIME"] = llformat("%d", (U32)mRuthDebugTimer.getElapsedTimeF32());
        args["NAME"] = getFullname();
        LLNotificationsUtil::add(notification_name, args);
    }
}

//------------------------------------------------------------------------
// LLVOAvatar::~LLVOAvatar()
//------------------------------------------------------------------------
LLVOAvatar::~LLVOAvatar()
{
    LLAvatarHandles::release(mRenderHandle.v.load(std::memory_order_acquire));       // <FS> C2.4d: handles of this avatar stop resolving
    sInstances.remove(this);

    if (!mFullyLoaded)
    {
        debugAvatarRezTime("AvatarRezLeftCloudNotification", "left after ruth seconds as cloud");
    }
    else
    {
        debugAvatarRezTime("AvatarRezLeftNotification", "left sometime after declouding");
    }

    if (mTuned)
    {
        LLPerfStats::tunedAvatars--;
        mTuned = false;
    }
    sAVsIgnoringARTLimit.erase(std::remove(sAVsIgnoringARTLimit.begin(), sAVsIgnoringARTLimit.end(), mID), sAVsIgnoringARTLimit.end());

    // <FS:ND> only call logPendingPhases if we're still alive. Otherwise this can lead to shutdown crashes
    // logPendingPhases();
    if (isAgentAvatarValid())
        logPendingPhases();
    // </FS:ND>
    LL_DEBUGS("Avatar") << "LLVOAvatar Destructor (0x" << this << ") id:" << mID << LL_ENDL;

    std::for_each(mAttachmentPoints.begin(), mAttachmentPoints.end(), DeletePairedPointer());
    mAttachmentPoints.clear();

    mDead = true;

    mAnimationSources.clear();
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackTextureList) ;

    getPhases().clearPhases();

    LL_DEBUGS() << "LLVOAvatar Destructor end" << LL_ENDL;
}

void LLVOAvatar::markDead()
{
    if (mNameText)
    {
        mNameText->markDead();
        mNameText = NULL;
        sNumVisibleChatBubbles--;
    }
    mVoiceVisualizer->markDead();
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackTextureList) ;
    LLViewerObject::markDead();
}


bool LLVOAvatar::isFullyBaked()
{
    if (mIsDummy) return true;
    if (getNumTEs() == 0) return false;
    // <FS:Beq> OS BOM limit the tests to avoid "invalid face error"
    // for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    for (S32 i = 0; i < getNumBakes(); i++)
    {
        if (!isTextureDefined(mBakedTextureDatas[i].mTextureIndex)
            && ((i != BAKED_SKIRT) || isWearingWearableType(LLWearableType::WT_SKIRT))
            && (i != BAKED_LEFT_ARM) && (i != BAKED_LEFT_LEG) && (i != BAKED_AUX1) && (i != BAKED_AUX2) && (i != BAKED_AUX3))
        {
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::isFullyTextured() const
{
    for (S32 i = 0; i < mMeshLOD.size(); i++)
    {
        LLAvatarJoint* joint = mMeshLOD[i];
        if (i==MESH_ID_SKIRT && !isWearingWearableType(LLWearableType::WT_SKIRT))
        {
            continue; // don't care about skirt textures if we're not wearing one.
        }
        if (!joint)
        {
            continue; // nonexistent LOD OK.
        }
        avatar_joint_mesh_list_t::iterator meshIter = joint->mMeshParts.begin();
        if (meshIter != joint->mMeshParts.end())
        {
            LLAvatarJointMesh *mesh = (*meshIter);
            if (!mesh)
            {
                continue; // nonexistent mesh OK
            }
            if (mesh->hasImageTexture())
            {
                continue; // Mesh exists and has a baked texture.
            }
            if (mesh->hasComposite())
            {
                continue; // Mesh exists and has a composite texture.
            }
            // Fail
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::hasGray() const
{
    return !getHasMissingParts() && !isFullyTextured();
}

S32 LLVOAvatar::getRezzedStatus() const
{
    if (getHasMissingParts()) return 0;
    bool textured = isFullyTextured();
    bool all_baked_loaded = allBakedTexturesCompletelyDownloaded();
    if (textured && all_baked_loaded && getAttachmentCount() == mSimAttachments.size()) return 4;
    if (textured && all_baked_loaded) return 3;
    if (textured) return 2;
    llassert(hasGray());
    return 1; // gray
}

void LLVOAvatar::deleteLayerSetCaches(bool clearAll)
{
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        if (mBakedTextureDatas[i].mTexLayerSet)
        {
            // ! BACKWARDS COMPATIBILITY !
            // Can be removed after hair baking is mandatory on the grid
            if ((i != BAKED_HAIR || isSelf()) && !clearAll)
            {
                mBakedTextureDatas[i].mTexLayerSet->deleteCaches();
            }
        }
        if (mBakedTextureDatas[i].mMaskTexName)
        {
            LLImageTex::deleteTextures(1, (U32*)&(mBakedTextureDatas[i].mMaskTexName));
            mBakedTextureDatas[i].mMaskTexName = 0 ;
        }
    }
}

// static
bool LLVOAvatar::areAllNearbyInstancesBaked(S32& grey_avatars)
{
    grey_avatars = 0;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        if (!inst->isDead() && inst->mHasGrey && !inst->isFullyBaked())
        {
            ++grey_avatars;
        }
    }
    return grey_avatars == 0;
}

// static
void LLVOAvatar::getNearbyRezzedStats(std::vector<S32>& counts, F32& avg_cloud_time, S32& cloud_avatars, S32& pending_meshes, S32& control_avatars)
{
    counts.clear();
    counts.resize(5);
    avg_cloud_time = 0;
    cloud_avatars = 0;
    pending_meshes = 0;
    control_avatars = 0;
    S32 count_avg = 0;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        if (inst && !inst->isUIAvatar() && !inst->isSelf())
        {
            if (inst->isControlAvatar())
            {
                control_avatars++;
            }
            else
            {
                S32 rez_status = inst->getRezzedStatus();
                counts[rez_status]++;
                F32 time = inst->getFirstDecloudTime();
                if (time >= 0)
                {
                    avg_cloud_time += time;
                    count_avg++;
                }
                if (!inst->isFullyLoaded() || time < 0)
                {
                    // still renders as cloud
                    cloud_avatars++;
                    if (rez_status >= 4
                        && inst->mWaitingForMeshes)
                    {
                        pending_meshes++;
                    }
                }
            }
        }
    }

    if (count_avg > 0)
    {
        avg_cloud_time /= count_avg;
    }
}

// static
std::string LLVOAvatar::rezStatusToString(S32 rez_status)
{
    switch (rez_status)
    {
    case 0:
        return "missing parts";
    case 1:
        return "gray";
    case 2:
        return "downloading baked";
    case 3:
        return "loading attachments";
    case 4:
        return "full";
    }
    return "unknown";
}

// static
void LLVOAvatar::dumpBakedStatus()
{
    LLVector3d camera_pos_global = gAgentCamera.getCameraPositionGlobal();

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        LL_INFOS() << "Avatar ";

        LLNameValue* firstname = inst->getNVPair("FirstName");
        LLNameValue* lastname = inst->getNVPair("LastName");

        if( firstname )
        {
            LL_CONT << firstname->getString();
        }
        if( lastname )
        {
            LL_CONT << " " << lastname->getString();
        }

        LL_CONT << " " << inst->mID;

        if( inst->isDead() )
        {
            LL_CONT << " DEAD ("<< inst->getNumRefs() << " refs)";
        }

        if( inst->isSelf() )
        {
            LL_CONT << " (self)";
        }


        F64 dist_to_camera = (inst->getPositionGlobal() - camera_pos_global).length();
        LL_CONT << " " << dist_to_camera << "m ";

        LL_CONT << " " << inst->mPixelArea << " pixels";

        if( inst->isVisible() )
        {
            LL_CONT << " (visible)";
        }
        else
        {
            LL_CONT << " (not visible)";
        }

        if( inst->isFullyBaked() )
        {
            LL_CONT << " Baked";
        }
        else
        {
            LL_CONT << " Unbaked (";

            for (LLAvatarAppearanceDictionary::BakedTextures::const_iterator iter = LLAvatarAppearance::getDictionary()->getBakedTextures().begin();
                 iter != LLAvatarAppearance::getDictionary()->getBakedTextures().end();
                 ++iter)
            {
                const LLAvatarAppearanceDictionary::BakedEntry *baked_dict = iter->second;
                const ETextureIndex index = baked_dict->mTextureIndex;
                if (!inst->isTextureDefined(index))
                {
                    LL_CONT << " " << (LLAvatarAppearance::getDictionary()->getTexture(index) ? LLAvatarAppearance::getDictionary()->getTexture(index)->mName : "");
                }
            }
            LL_CONT << " ) " << inst->getUnbakedPixelAreaRank();
            if( inst->isCulled() )
            {
                LL_CONT << " culled";
            }
        }
        LL_CONT << LL_ENDL;
    }
}

//static
void LLVOAvatar::restoreResources()
{
    if (!isAgentAvatarValid()) return;

    gAgentAvatarp->setCompositeUpdatesEnabled(true);
    for (U32 i = 0; i < gAgentAvatarp->mBakedTextureDatas.size(); i++)
    {
        // <FS:Ansariel> [Legacy Bake]
        //gAgentAvatarp->invalidateComposite(gAgentAvatarp->getTexLayerSet(i));
        gAgentAvatarp->invalidateComposite(gAgentAvatarp->getTexLayerSet(i), false);
    }
    gAgentAvatarp->updateMeshTextures();
}

//static
void LLVOAvatar::destroyResources()
{
    deleteCachedImages();

    resetImpostors();
}

//static
void LLVOAvatar::resetImpostors()
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        avatar->mImpostor.release();
        avatar->mNeedsImpostorUpdate = true;
        avatar->mLastImpostorUpdateReason = 1;
    }
}

// static
void LLVOAvatar::deleteCachedImages(bool clearAll)
{
    if (LLViewerTexLayerSet::sHasCaches)
    {
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            ((LLVOAvatar*)character)->deleteLayerSetCaches(clearAll);
        }
        LLViewerTexLayerSet::sHasCaches = false;
    }
    LLVOAvatarSelf::deleteScratchTextures();
    LLTexLayerStaticImageList::getInstance()->deleteCachedImages();
}


//------------------------------------------------------------------------
// static
// LLVOAvatar::initClass()
//------------------------------------------------------------------------
void LLVOAvatar::initClass()
{
    gAnimLibrary.animStateSetString(ANIM_AGENT_BODY_NOISE,"body_noise");
    gAnimLibrary.animStateSetString(ANIM_AGENT_BREATHE_ROT,"breathe_rot");
    gAnimLibrary.animStateSetString(ANIM_AGENT_PHYSICS_MOTION,"physics_motion");
    gAnimLibrary.animStateSetString(ANIM_AGENT_EDITING,"editing");
    gAnimLibrary.animStateSetString(ANIM_AGENT_EYE,"eye");
    gAnimLibrary.animStateSetString(ANIM_AGENT_FLY_ADJUST,"fly_adjust");
    gAnimLibrary.animStateSetString(ANIM_AGENT_HAND_MOTION,"hand_motion");
    gAnimLibrary.animStateSetString(ANIM_AGENT_HEAD_ROT,"head_rot");
    gAnimLibrary.animStateSetString(ANIM_AGENT_PELVIS_FIX,"pelvis_fix");
    gAnimLibrary.animStateSetString(ANIM_AGENT_TARGET,"target");
    gAnimLibrary.animStateSetString(ANIM_AGENT_WALK_ADJUST,"walk_adjust");

    // Where should this be set initially?
    LLJoint::setDebugJointNames(gSavedSettings.getString("DebugAvatarJoints"));

    LLControlAvatar::sRegionChangedSlot = gAgent.addRegionChangedCallback(&LLControlAvatar::onRegionChanged);

    sCloudTexture = LLViewerTextureManager::getFetchedTextureFromFile("cloud-particle.j2c");

    initCloud();
}


void LLVOAvatar::cleanupClass()
{
}

LLPartSysData LLVOAvatar::sCloud;

// <FS:TP> [FIRE-36987] track the signal connections so initCloud() can
// disconnect and reconnect cleanly if it runs more than once (there is
// a menu option to reload the particle cloud)
boost::signals2::connection LLVOAvatar::sCloudColorStartConnection;
boost::signals2::connection LLVOAvatar::sCloudColorEndConnection;
// </FS:TP>

void LLVOAvatar::initCloud()
{
    // fancy particle cloud designed by Brent
    std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "cloud.xml");
    if(!gDirUtilp->fileExists(filename))
    {
        filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "cloud.xml");
    }
    if(!gDirUtilp->fileExists(filename))
    {
        filename = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "cloud.xml");
    }
    LLSD cloud;
    // <FS:ND> On Linux ifstream only has a const char* constructor
    // llifstream in_file(filename);
    llifstream in_file(filename.c_str());
    // </FS:ND>
    LLSDSerialize::fromXMLDocument(cloud, in_file);
    sCloud.fromLLSD(cloud);
    sCloud.mPartImageID = sCloudTexture->getID();

    //Todo: have own image, de-copy-pasta
    LLSD cloud_muted;
    filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "cloud_muted.xml");
    if(!gDirUtilp->fileExists(filename))
    {
        filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "cloud_muted.xml");
    }
    if(!gDirUtilp->fileExists(filename))
    {
        filename = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "cloud_muted.xml");
    }
    // <FS:ND> On Linux ifstream only has a const char* constructor
    // llifstream in_file_muted(filename);
    llifstream in_file_muted(filename.c_str());
    // </FS:ND>

    // <FS:TP> [FIRE-36987] Selectable avatar loading/bakefail cloud color
    // Apply whatever start/end colors are currently saved, on top of
    // whatever cloud.xml just loaded above, then keep re-applying live if
    // the user changes either preference later (no restart needed - see
    // applyCloudColor()).
    applyCloudColor();

    // disconnect any previous connection first, since initCloud() can run
    // more than once (menu option to reload the particle cloud), then bind
    // straight to applyCloudColor() - no wrapper needed since the signal's
    // new_value/old_value aren't used
    if (sCloudColorStartConnection.connected())
    {
        sCloudColorStartConnection.disconnect();
    }
    sCloudColorStartConnection = gSavedSettings.getControl("FSCloudColorStart")->getSignal()->connect(
        boost::bind(&LLVOAvatar::applyCloudColor));

    if (sCloudColorEndConnection.connected())
    {
        sCloudColorEndConnection.disconnect();
    }
    sCloudColorEndConnection = gSavedSettings.getControl("FSCloudColorEnd")->getSignal()->connect(
        boost::bind(&LLVOAvatar::applyCloudColor));
    // </FS:TP>
}

void LLVOAvatar::applyCloudColor()
{
    // Same alpha envelope (0.1 -> 0.9) and burst/scale/pattern as the
    // stock Firestorm cloud.xml - only the start/end hues are user
    // configurable, so changing colors doesn't change the shape/behavior
    // of the effect. Alpha is intentionally not exposed to the color
    // pickers and is forced back to the stock values here.
    LLColor4 start_color = gSavedSettings.getColor4("FSCloudColorStart");
    LLColor4 end_color   = gSavedSettings.getColor4("FSCloudColorEnd");
    start_color.mV[VALPHA] = 0.1f;
    end_color.mV[VALPHA]   = 0.9f;

    sCloud.mPartData.mStartColor = start_color;
    sCloud.mPartData.mEndColor   = end_color;
}
// </FS:TP>

// virtual
void LLVOAvatar::initInstance()
{
    //-------------------------------------------------------------------------
    // register motions
    //-------------------------------------------------------------------------
    if (LLCharacter::sInstances.size() == 1)
    {
        registerMotion( ANIM_AGENT_DO_NOT_DISTURB,          LLNullMotion::create );
        registerMotion( ANIM_AGENT_CROUCH,                  LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_CROUCHWALK,              LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_EXPRESS_AFRAID,          LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_ANGER,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_BORED,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_CRY,             LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_DISDAIN,         LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_EMBARRASSED,     LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_FROWN,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_KISS,            LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_LAUGH,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_OPEN_MOUTH,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_REPULSED,        LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SAD,             LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SHRUG,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SMILE,           LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_SURPRISE,        LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_TONGUE_OUT,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_TOOTHSMILE,      LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_WINK,            LLEmote::create );
        registerMotion( ANIM_AGENT_EXPRESS_WORRY,           LLEmote::create );
        registerMotion( ANIM_AGENT_FEMALE_RUN_NEW,          LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK,             LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK_NEW,         LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_RUN,                     LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_RUN_NEW,                 LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_STAND,                   LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_1,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_2,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_3,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STAND_4,                 LLKeyframeStandMotion::create );
        registerMotion( ANIM_AGENT_STANDUP,                 LLKeyframeFallMotion::create );
        registerMotion( ANIM_AGENT_TURNLEFT,                LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_TURNRIGHT,               LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_WALK,                    LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_WALK_NEW,                LLKeyframeWalkMotion::create );

        // motions without a start/stop bit
        registerMotion( ANIM_AGENT_BODY_NOISE,              LLBodyNoiseMotion::create );
        registerMotion( ANIM_AGENT_BREATHE_ROT,             LLBreatheMotionRot::create );
        registerMotion( ANIM_AGENT_PHYSICS_MOTION,          LLPhysicsMotionController::create );
        registerMotion( ANIM_AGENT_EDITING,                 LLEditingMotion::create );
        registerMotion( ANIM_AGENT_EYE,                     LLEyeMotion::create );
        registerMotion( ANIM_AGENT_FEMALE_WALK,             LLKeyframeWalkMotion::create );
        registerMotion( ANIM_AGENT_FLY_ADJUST,              LLFlyAdjustMotion::create );
        registerMotion( ANIM_AGENT_HAND_MOTION,             LLHandMotion::create );
        registerMotion( ANIM_AGENT_HEAD_ROT,                LLHeadRotMotion::create );
        registerMotion( ANIM_AGENT_PELVIS_FIX,              LLPelvisFixMotion::create );
        registerMotion( ANIM_AGENT_SIT_FEMALE,              LLKeyframeMotion::create );
        registerMotion( ANIM_AGENT_TARGET,                  LLTargetingMotion::create );
        registerMotion( ANIM_AGENT_WALK_ADJUST,             LLWalkAdjustMotion::create );
    }

    LLAvatarAppearance::initInstance();

    // preload specific motions here
    createMotion( ANIM_AGENT_CUSTOMIZE);
    createMotion( ANIM_AGENT_CUSTOMIZE_DONE);

    //VTPause();  // VTune

    mVoiceVisualizer->setVoiceEnabled( LLVoiceClient::getInstance()->getVoiceEnabled( mID ) );

    mInitFlags |= 1<<1;
}

// virtual
LLAvatarJoint* LLVOAvatar::createAvatarJoint()
{
    return new LLViewerJoint();
}

// virtual
LLAvatarJoint* LLVOAvatar::createAvatarJoint(S32 joint_num)
{
    return new LLViewerJoint(joint_num);
}

// virtual
LLAvatarJointMesh* LLVOAvatar::createAvatarJointMesh()
{
    return new LLViewerJointMesh();
}

// virtual
LLTexLayerSet* LLVOAvatar::createTexLayerSet()
{
    return new LLViewerTexLayerSet(this);
}

const LLVector3 LLVOAvatar::getRenderPosition() const
{

    if (mDrawable.isNull() || mDrawable->getGeneration() < 0)
    {
        return getPositionAgent();
    }
    else if (isRoot())
    {
        F32 fixup;
        if ( hasPelvisFixup( fixup) )
        {
            //Apply a pelvis fixup (as defined by the avs skin)
            LLVector3 pos = mDrawable->getPositionAgent();
            pos[VZ] += fixup;
            return pos;
        }
        else
        {
            return mDrawable->getPositionAgent();
        }
    }
    else
    {
        return getPosition() * mDrawable->getParent()->getRenderMatrix();
    }
}

void LLVOAvatar::updateDrawable(bool force_damped)
{
    clearChanged(SHIFTED);
}

void LLVOAvatar::onShift(const LLVector4a& shift_vector)
{
    const LLVector3& shift = reinterpret_cast<const LLVector3&>(shift_vector);
    mLastAnimExtents[0] += shift;
    mLastAnimExtents[1] += shift;
}

void LLVOAvatar::updateSpatialExtents(LLVector4a& newMin, LLVector4a &newMax)
{
    if (mDrawable.isNull())
    {
        return;
    }

    if (mNeedsExtentUpdate)
    {
        calculateSpatialExtents(newMin,newMax);
        mLastAnimExtents[0].set(newMin.getF32ptr());
        mLastAnimExtents[1].set(newMax.getF32ptr());
        mLastAnimBasePos = mPelvisp->getWorldPosition();
        mNeedsExtentUpdate = false;
    }
    else
    {
        LLVector3 new_base_pos = mPelvisp->getWorldPosition();
        LLVector3 shift = new_base_pos-mLastAnimBasePos;
        mLastAnimExtents[0] += shift;
        mLastAnimExtents[1] += shift;
        mLastAnimBasePos = new_base_pos;

    }

    if (isImpostor() && !needsImpostorUpdate())
    {
        LLVector3 delta = getRenderPosition() -
            ((LLVector3(mDrawable->getPositionGroup().getF32ptr())-mImpostorOffset));

        newMin.load3( (mLastAnimExtents[0] + delta).mV);
        newMax.load3( (mLastAnimExtents[1] + delta).mV);
    }
    else
    {
        newMin.load3(mLastAnimExtents[0].mV);
        newMax.load3(mLastAnimExtents[1].mV);
        LLVector4a pos_group;
        pos_group.setAdd(newMin,newMax);
        pos_group.mul(0.5f);
        mImpostorOffset = LLVector3(pos_group.getF32ptr())-getRenderPosition();
        mDrawable->setPositionGroup(pos_group);
    }
}


void LLVOAvatar::calculateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    const S32 BOX_DETAIL_DEFAULT = 3;
    S32 box_detail = BOX_DETAIL_DEFAULT;
    if (getOverallAppearance() != AOA_NORMAL)
    {
        if (isControlAvatar())
        {
            // Animated objects don't show system avatar but do need to include rigged meshes in their bounding box.
            box_detail = 3;
        }
        else
        {
            // Jellydolled avatars ignore attachments, etc, use only system avatar.
            box_detail = 1;
        }
    }

    // FIXME the update_min_max function used below assumes there is a
    // known starting point, but in general there isn't. Ideally the
    // box update logic should be modified to handle the no-point-yet
    // case. For most models, starting with the pelvis is safe though.
    LLVector3 zero_pos;
    LLVector4a pos;
    if (dist_vec(zero_pos, mPelvisp->getWorldPosition())<0.001)
    {
        // Don't use pelvis until av initialized
        pos.load3(getRenderPosition().mV);
    }
    else
    {
        pos.load3(mPelvisp->getWorldPosition().mV);
    }
    newMin = pos;
    newMax = pos;

    if (box_detail>=1 && !isControlAvatar())
    {
        //stretch bounding box by joint positions. Doing this for
        //control avs, where the polymeshes aren't maintained or
        //displayed, can give inaccurate boxes due to joints stuck at (0,0,0).
        for (polymesh_map_t::iterator i = mPolyMeshes.begin(); i != mPolyMeshes.end(); ++i)
        {
            LLPolyMesh* mesh = i->second;
            for (S32 joint_num = 0; joint_num < mesh->mJointRenderData.size(); joint_num++)
            {
                LLVector4a trans;
                trans.load3( mesh->mJointRenderData[joint_num]->mWorldMatrix->getTranslation().mV);
                update_min_max(newMin, newMax, trans);
            }
        }
    }

    // Pad bounding box for starting joint, plus polymesh if
    // applicable. Subsequent calcs should be accurate enough to not
    // need padding.
    LLVector4a padding(0.25);
    newMin.sub(padding);
    newMax.add(padding);


    //stretch bounding box by static attachments
    if (box_detail >= 2)
    {
        float max_attachment_span = get_default_max_prim_scale() * 5.0f;

        for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
             iter != mAttachmentPoints.end();
             ++iter)
        {
            LLViewerJointAttachment* attachment = iter->second;

            // <FS:Ansariel> Possible crash fix
            //if (attachment->getValid())
            if (attachment && attachment->getValid())
            // </FS:Ansariel>
            {
                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                     attachment_iter != attachment->mAttachedObjects.end();
                     ++attachment_iter)
                {
                    // Don't we need to look at children of attached_object as well?
                    const LLViewerObject* attached_object = attachment_iter->get();
                    if (attached_object && !attached_object->isHUDAttachment())
                    {
                        const LLVOVolume *vol = dynamic_cast<const LLVOVolume*>(attached_object);
                        if (vol && vol->isAnimatedObject())
                        {
                            // Animated objects already have a bounding box in their control av, use that.
                            // Could lag by a frame if there's no guarantee on order of processing for avatars.
                            LLControlAvatar *cav = vol->getControlAvatar();
                            if (cav)
                            {
                                LLVector4a cav_min;
                                cav_min.load3(cav->mLastAnimExtents[0].mV);
                                LLVector4a cav_max;
                                cav_max.load3(cav->mLastAnimExtents[1].mV);
                                update_min_max(newMin,newMax,cav_min);
                                update_min_max(newMin,newMax,cav_max);
                                continue;
                            }
                        }
                        if (vol && vol->isRiggedMeshFast())
                        {
                            continue;
                        }
                        LLDrawable* drawable = attached_object->mDrawable;
                        if (drawable && !drawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD)) // <-- don't extend bounding box if any rigged objects are present
                        {
                            LLSpatialBridge* bridge = drawable->getSpatialBridge();
                            if (bridge)
                            {
                                const LLVector4a* ext = bridge->getSpatialExtents();
                                LLVector4a distance;
                                distance.setSub(ext[1], ext[0]);
                                LLVector4a max_span(max_attachment_span);

                                S32 lt = distance.lessThan(max_span).getGatheredBits() & 0x7;

                                // Only add the prim to spatial extents calculations if it isn't a megaprim.
                                // max_attachment_span calculated at the start of the function
                                // (currently 5 times our max prim size)
                                if (lt == 0x7)
                                {
                                    update_min_max(newMin,newMax,ext[0]);
                                    update_min_max(newMin,newMax,ext[1]);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Stretch bounding box by rigged mesh joint boxes
    if (box_detail>=3)
    {
        updateRiggingInfo();
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJoint *joint = getJoint(joint_num);
            LLJointRiggingInfo *rig_info = NULL;
            if (joint_num < mJointRiggingInfoTab.size())
            {
                rig_info = &mJointRiggingInfoTab[joint_num];
            }

            if (joint && rig_info && rig_info->isRiggedTo())
            {
                LLViewerJointAttachment *as_joint_attach = dynamic_cast<LLViewerJointAttachment*>(joint);
                if (as_joint_attach && as_joint_attach->getIsHUDAttachment())
                {
                    // Ignore bounding box of HUD joints
                    continue;
                }
                LLMatrix4a mat;
                LLVector4a new_extents[2];
                mat.loadu(joint->getWorldMatrix());
                matMulBoundBox(mat, rig_info->getRiggedExtents(), new_extents);
                update_min_max(newMin, newMax, new_extents[0]);
                update_min_max(newMin, newMax, new_extents[1]);
                //if (isSelf())
                //{
                //    LL_INFOS() << joint->getName() << " extents " << new_extents[0] << "," << new_extents[1] << LL_ENDL;
                //    LL_INFOS() << joint->getName() << " av box is " << newMin << "," << newMax << LL_ENDL;
                //}
            }
        }
    }

    // Update pixel area
    LLVector4a center, size;
    center.setAdd(newMin, newMax);
    center.mul(0.5f);

    size.setSub(newMax,newMin);
    size.mul(0.5f);

    F32 pixel_area = LLPipeline::calcPixelArea(center, size, *LLViewerCamera::getInstance());
    setCorrectedPixelArea(pixel_area);
}

void render_sphere_and_line(const LLVector3& begin_pos, const LLVector3& end_pos, F32 sphere_scale, const LLVector3& occ_color, const LLVector3& visible_color)
{
    // Unoccluded bone portions
    LLRenderState::DepthScope normal_depth(true);

    // Draw line segment for unoccluded joint
    gRender.diffuseColor3f(visible_color[0], visible_color[1], visible_color[2]);

    gRender.begin(LLRender::LINES);
    gRender.vertex3fv(begin_pos.mV);
    gRender.vertex3fv(end_pos.mV);
    gRender.end();


    // Draw sphere representing joint pos
    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
    LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, sphere_scale, sphere_scale, sphere_scale);
    gSphere.renderGGL();
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);

    LLRenderState::DepthScope depth_under(true, false, LLRenderState::CompareOp::Greater);

    // Occluded bone portions
    gRender.diffuseColor3f(occ_color[0], occ_color[1], occ_color[2]);

    gRender.begin(LLRender::LINES);
    gRender.vertex3fv(begin_pos.mV);
    gRender.vertex3fv(end_pos.mV);
    gRender.end();

    // Draw sphere representing joint pos
    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
    LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, sphere_scale, sphere_scale, sphere_scale);
    gSphere.renderGGL();
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
}

//-----------------------------------------------------------------------------
// renderCollisionVolumes()
//-----------------------------------------------------------------------------
void LLVOAvatar::renderCollisionVolumes()
{
    std::ostringstream ostr;

    for (S32 i = 0; i < mNumCollisionVolumes; i++)
    {
        ostr << mCollisionVolumes[i].getName() << ", ";

        LLAvatarJointCollisionVolume& collision_volume = mCollisionVolumes[i];

        collision_volume.updateWorldMatrix();

        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &collision_volume.getXform()->getWorldMatrix().mMatrix[0][0]);

        LLVector3 begin_pos(0,0,0);
        LLVector3 end_pos(collision_volume.getEnd());
        static F32 sphere_scale = 1.0f;
        static F32 center_dot_scale = 0.05f;

        static LLVector3 BLUE(0.0f, 0.0f, 1.0f);
        static LLVector3 PASTEL_BLUE(0.5f, 0.5f, 1.0f);
        static LLVector3 RED(1.0f, 0.0f, 0.0f);
        static LLVector3 PASTEL_RED(1.0f, 0.5f, 0.5f);
        static LLVector3 WHITE(1.0f, 1.0f, 1.0f);


        LLVector3 cv_color_occluded;
        LLVector3 cv_color_visible;
        LLVector3 dot_color_occluded(WHITE);
        LLVector3 dot_color_visible(WHITE);
        if (isControlAvatar())
        {
            cv_color_occluded = RED;
            cv_color_visible = PASTEL_RED;
        }
        else
        {
            cv_color_occluded = BLUE;
            cv_color_visible = PASTEL_BLUE;
        }
        render_sphere_and_line(begin_pos, end_pos, sphere_scale, cv_color_occluded, cv_color_visible);
        render_sphere_and_line(begin_pos, end_pos, center_dot_scale, dot_color_occluded, dot_color_visible);

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }


    if (mNameText.notNull())
    {
        LLVector4a unused;

        mNameText->lineSegmentIntersect(unused, unused, unused, true);
    }
}

// defined in llspatialpartition.cpp -- draw a box outline in the current GL context from given center and half-size
void drawBoxOutline(const LLVector4a& pos, const LLVector4a& size);

void LLVOAvatar::renderBones(const std::string &selected_joint)
{
    LLRenderState::BlendScope blend(true);

    avatar_joint_list_t::iterator iter = mSkeleton.begin();
    avatar_joint_list_t::iterator end = mSkeleton.end();

    // For selected joints
    static LLVector3 SELECTED_COLOR_OCCLUDED(1.0f, 1.0f, 0.0f);
    static LLVector3 SELECTED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones with position overrides defined
    static LLVector3 OVERRIDE_COLOR_OCCLUDED(1.0f, 0.0f, 0.0f);
    static LLVector3 OVERRIDE_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones which are rigged to by at least one attachment
    static LLVector3 RIGGED_COLOR_OCCLUDED(0.0f, 1.0f, 1.0f);
    static LLVector3 RIGGED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones not otherwise colored
    static LLVector3 OTHER_COLOR_OCCLUDED(0.0f, 1.0f, 0.0f);
    static LLVector3 OTHER_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);

    static F32 SPHERE_SCALEF = 0.001f;

    for (; iter != end; ++iter)
    {
        LLJoint* jointp = *iter;
        if (!jointp)
        {
            continue;
        }

        jointp->updateWorldMatrix();

        LLVector3 occ_color, visible_color;

        LLVector3 pos;
        LLUUID mesh_id;
        F32 sphere_scale = SPHERE_SCALEF;

        // We are in render, so it is preferable to implement selection
        // in a different way, but since this is for debug/preview, this
        // is low priority
        if (jointp->getName() == selected_joint)
        {
            sphere_scale *= 16.f;
            occ_color = SELECTED_COLOR_OCCLUDED;
            visible_color = SELECTED_COLOR_VISIBLE;
        }
        else if (jointp->hasAttachmentPosOverride(pos,mesh_id))
        {
            occ_color = OVERRIDE_COLOR_OCCLUDED;
            visible_color = OVERRIDE_COLOR_VISIBLE;
        }
        else
        {
            if (jointIsRiggedTo(jointp))
            {
                occ_color = RIGGED_COLOR_OCCLUDED;
                visible_color = RIGGED_COLOR_VISIBLE;
            }
            else
            {
                occ_color = OTHER_COLOR_OCCLUDED;
                visible_color = OTHER_COLOR_VISIBLE;
            }
        }
        LLVector3 begin_pos(0,0,0);
        LLVector3 end_pos(jointp->getEnd());


        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &jointp->getXform()->getWorldMatrix().mMatrix[0][0]);

        render_sphere_and_line(begin_pos, end_pos, sphere_scale, occ_color, visible_color);

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }


    // draw joint space bounding boxes of rigged attachments in yellow
    gRender.color3f(1.f, 1.f, 0.f);
    for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
    {
        LLJoint* joint = getJoint(joint_num);
        LLJointRiggingInfo* rig_info = NULL;
        if (joint_num < mJointRiggingInfoTab.size())
        {
            rig_info = &mJointRiggingInfoTab[joint_num];
        }

        if (joint && rig_info && rig_info->isRiggedTo())
        {
            LLViewerJointAttachment* as_joint_attach = dynamic_cast<LLViewerJointAttachment*>(joint);
            if (as_joint_attach && as_joint_attach->getIsHUDAttachment())
            {
                // Ignore bounding box of HUD joints
                continue;
            }
            LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
            LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &joint->getXform()->getWorldMatrix().mMatrix[0][0]);

            LLVector4a pos;
            LLVector4a size;

            const LLVector4a* extents = rig_info->getRiggedExtents();

            pos.setAdd(extents[0], extents[1]);
            pos.mul(0.5f);
            size.setSub(extents[1], extents[0]);
            size.mul(0.5f);

            drawBoxOutline(pos, size);

            LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
        }
    }

    // draw world space attachment rigged bounding boxes in cyan
    gRender.color3f(0.f, 1.f, 1.f);
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        if (attachment->getValid())
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object && !attached_object->isHUDAttachment())
                {
                    LLDrawable* drawable = attached_object->mDrawable;
                    if (drawable && drawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD))
                    {
                        // get face rigged extents
                        for (S32 i = 0; i < drawable->getNumFaces(); ++i)
                        {
                            LLFace* facep = drawable->getFace(i);
                            if (facep && facep->isState(LLFace::RIGGED))
                            {
                                LLVector4a center, size;

                                LLVector4a* extents = facep->row().riggedExtents;

                                center.setAdd(extents[0], extents[1]);
                                center.mul(0.5f);
                                size.setSub(extents[1], extents[0]);
                                size.mul(0.5f);
                                drawBoxOutline(center, size);
                            }
                        }
                    }
                }
            }
        }
    }
}

void LLVOAvatar::renderOnlySelectedBones(const std::vector<std::string> &selected_joints)
{
    LLRenderState::BlendScope blend(true);

    avatar_joint_list_t::iterator iter = mSkeleton.begin();
    avatar_joint_list_t::iterator end = mSkeleton.end();

    // For selected joints
    static LLVector3 SELECTED_COLOR_OCCLUDED(1.0f, 1.0f, 0.0f);
    static LLVector3 SELECTED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones with position overrides defined
    static LLVector3 OVERRIDE_COLOR_OCCLUDED(1.0f, 0.0f, 0.0f);
    static LLVector3 OVERRIDE_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones which are rigged to by at least one attachment
    static LLVector3 RIGGED_COLOR_OCCLUDED(0.0f, 1.0f, 1.0f);
    static LLVector3 RIGGED_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);
    // For bones not otherwise colored
    static LLVector3 OTHER_COLOR_OCCLUDED(0.0f, 1.0f, 0.0f);
    static LLVector3 OTHER_COLOR_VISIBLE(0.5f, 0.5f, 0.5f);

    static F32 SPHERE_SCALEF = 0.001f;

    for (; iter != end; ++iter)
    {
        LLJoint* jointp = *iter;
        if (!jointp)
        {
            continue;
        }
        if (std::find(selected_joints.begin(), selected_joints.end(), jointp->getName()) == selected_joints.end())
        {
            continue;
        }
        jointp->updateWorldMatrix();

        LLVector3 occ_color, visible_color;

        occ_color = SELECTED_COLOR_OCCLUDED;
        visible_color = SELECTED_COLOR_VISIBLE;

        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &jointp->getXform()->getWorldMatrix().mMatrix[0][0]);

        gRender.diffuseColor3f( 1.f, 0.f, 1.f );

        gRender.begin(LLRender::LINES);

        LLVector3 v[] =
        {
            LLVector3(1,0,0),
            LLVector3(-1,0,0),
            LLVector3(0,1,0),
            LLVector3(0,-1,0),

            LLVector3(0,0,-1),
            LLVector3(0,0,1),
        };

        //sides
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[2].mV);

        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[3].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[2].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[3].mV);


        //top
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[2].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[3].mV);
        gRender.vertex3fv(v[4].mV);


        //bottom
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[2].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[3].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.end();

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);

        
        // renderBoxAroundJointAttachments( jointp );

    }


    // // draw joint space bounding boxes of rigged attachments in yellow
    // gRender.color3f(1.f, 1.f, 0.f);
    // for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
    // {
    //     LLJoint* joint = getJoint(joint_num);
    //     LLJointRiggingInfo* rig_info = NULL;
    //     if (joint_num < mJointRiggingInfoTab.size())
    //     {
    //         rig_info = &mJointRiggingInfoTab[joint_num];
    //     }

    //     if (joint && rig_info && rig_info->isRiggedTo())
    //     {
    //         LLViewerJointAttachment* as_joint_attach = dynamic_cast<LLViewerJointAttachment*>(joint);
    //         if (as_joint_attach && as_joint_attach->getIsHUDAttachment())
    //         {
    //             // Ignore bounding box of HUD joints
    //             continue;
    //         }
    //         gRender.pushMatrix();
    //         gRender.multMatrix(&joint->getXform()->getWorldMatrix().mMatrix[0][0]);

    //         LLVector4a pos;
    //         LLVector4a size;

    //         const LLVector4a* extents = rig_info->getRiggedExtents();

    //         pos.setAdd(extents[0], extents[1]);
    //         pos.mul(0.5f);
    //         size.setSub(extents[1], extents[0]);
    //         size.mul(0.5f);

    //         drawBoxOutline(pos, size);

    //         gRender.popMatrix();
    //     }
    // }

    // draw world space attachment rigged bounding boxes in cyan
    // gRender.color3f(0.f, 1.f, 1.f);
    // for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
    //      iter != mAttachmentPoints.end();
    //      ++iter)
    // {
    //     LLViewerJointAttachment* attachment = iter->second;

    //     if (attachment->getValid())
    //     {
    //         for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
    //              attachment_iter != attachment->mAttachedObjects.end();
    //              ++attachment_iter)
    //         {
    //             LLViewerObject* attached_object = attachment_iter->get();
    //             if (attached_object && !attached_object->isHUDAttachment())
    //             {
    //                 LLDrawable* drawable = attached_object->mDrawable;
    //                 if (drawable && drawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD))
    //                 {
    //                     // get face rigged extents
    //                     for (S32 i = 0; i < drawable->getNumFaces(); ++i)
    //                     {
    //                         LLFace* facep = drawable->getFace(i);
    //                         if (facep && facep->isState(LLFace::RIGGED))
    //                         {
    //                             LLVector4a center, size;

    //                             LLVector4a* extents = facep->mRiggedExtents;

    //                             center.setAdd(extents[0], extents[1]);
    //                             center.mul(0.5f);
    //                             size.setSub(extents[1], extents[0]);
    //                             size.mul(0.5f);
    //                             drawBoxOutline(center, size);
    //                         }
    //                     }
    //                 }
    //             }
    //         }
    //     }
    // }
}
void LLVOAvatar::renderBoxAroundJointAttachments(LLJoint * joint)
{
    LLJointRiggingInfo* rig_info = NULL;
    
    if (joint->getJointNum() < mJointRiggingInfoTab.size())
    {
        rig_info = &mJointRiggingInfoTab[joint->getJointNum()];
    }

    if (joint && rig_info && rig_info->isRiggedTo())
    {
        LLViewerJointAttachment* as_joint_attach = dynamic_cast<LLViewerJointAttachment*>(joint);
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &joint->getXform()->getWorldMatrix().mMatrix[0][0]);

        LLVector4a pos;
        LLVector4a size;

        const LLVector4a* extents = rig_info->getRiggedExtents();

        pos.setAdd(extents[0], extents[1]);
        pos.mul(0.5f);
        size.setSub(extents[1], extents[0]);
        size.mul(0.5f);

        drawBoxOutline(pos, size);

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }
}


void LLVOAvatar::renderJoints()
{
    std::ostringstream ostr;
    std::ostringstream nullstr;

    for (joint_map_t::iterator iter = mJointMap.begin(); iter != mJointMap.end(); ++iter)
    {
        LLJoint* jointp = iter->second;
        if (!jointp)
        {
            nullstr << iter->first << " is NULL" << std::endl;
            continue;
        }

        ostr << jointp->getName() << ", ";

        jointp->updateWorldMatrix();

        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::multiply(LLRenderMatrix::Kind::Modelview, &jointp->getXform()->getWorldMatrix().mMatrix[0][0]);

        gRender.diffuseColor3f( 1.f, 0.f, 1.f );

        gRender.begin(LLRender::LINES);

        LLVector3 v[] =
        {
            LLVector3(1,0,0),
            LLVector3(-1,0,0),
            LLVector3(0,1,0),
            LLVector3(0,-1,0),

            LLVector3(0,0,-1),
            LLVector3(0,0,1),
        };

        //sides
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[2].mV);

        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[3].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[2].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[3].mV);


        //top
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[2].mV);
        gRender.vertex3fv(v[4].mV);

        gRender.vertex3fv(v[3].mV);
        gRender.vertex3fv(v[4].mV);


        //bottom
        gRender.vertex3fv(v[0].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[1].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[2].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.vertex3fv(v[3].mV);
        gRender.vertex3fv(v[5].mV);

        gRender.end();

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }

    mDebugText.clear();
    addDebugText(ostr.str());
    addDebugText(nullstr.str());
}

bool LLVOAvatar::lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                      S32 face,
                                      bool pick_transparent,
                                      bool pick_rigged,
                                      bool pick_unselectable,
                                      S32* face_hit,
                                      LLVector4a* intersection,
                                      LLVector2* tex_coord,
                                      LLVector4a* normal,
                                      LLVector4a* tangent)
{
    if ((isSelf() && !gAgent.needsRenderAvatar()) || !LLPipeline::sPickAvatar)
    {
        return false;
    }

    if (isControlAvatar())
    {
        return false;
    }

    if (lineSegmentBoundingBox(start, end))
    {
        for (S32 i = 0; i < mNumCollisionVolumes; ++i)
        {
            mCollisionVolumes[i].updateWorldMatrix();

            glm::mat4 mat(glm::make_mat4((F32*) mCollisionVolumes[i].getXform()->getWorldMatrix().mMatrix));
            glm::mat4 inverse = glm::inverse(mat);
            glm::mat4 norm_mat = glm::transpose(inverse);

            glm::vec3 p1(start);
            glm::vec3 p2(end);

            p1 = mul_mat4_vec3(inverse, p1);
            p2 = mul_mat4_vec3(inverse, p2);

            LLVector3 position;
            LLVector3 norm;

            if (linesegment_sphere(LLVector3(p1), LLVector3(p2), LLVector3(0,0,0), 1.f, position, norm))
            {
                glm::vec3 res_pos(position);
                res_pos = mul_mat4_vec3(mat, res_pos);

                 glm::vec3 res_norm(norm);
                res_norm = glm::normalize(res_norm);
                res_norm = glm::mat3(norm_mat) * res_norm;

                if (intersection)
                {
                    intersection->load3(glm::value_ptr(res_pos));
                }

                if (normal)
                {
                    normal->load3(glm::value_ptr(res_norm));
                }

                return true;
            }
        }

        if (isSelf())
        {
            for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
             iter != mAttachmentPoints.end();
             ++iter)
            {
                LLViewerJointAttachment* attachment = iter->second;

                // <FS:Ansariel> Possible crash fix
                if (!attachment)
                {
                    continue;
                }
                // </FS:Ansariel>

                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                     attachment_iter != attachment->mAttachedObjects.end();
                     ++attachment_iter)
                {
                    LLViewerObject* attached_object = attachment_iter->get();

                    if (attached_object && !attached_object->isDead() && attachment->getValid())
                    {
                        LLDrawable* drawable = attached_object->mDrawable;
                        if (drawable->isState(LLDrawable::RIGGED))
                        { //regenerate octree for rigged attachment
                            gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_RIGGED);
                        }
                    }
                }
            }
        }
    }



    LLVector4a position;
    if (mNameText.notNull() && mNameText->lineSegmentIntersect(start, end, position))
    {
        if (intersection)
        {
            *intersection = position;
        }

        return true;
    }

    return false;
}

// virtual
LLViewerObject* LLVOAvatar::lineSegmentIntersectRiggedAttachments(const LLVector4a& start, const LLVector4a& end,
                                      S32 face,
                                      bool pick_transparent,
                                      bool pick_rigged,
                                      bool pick_unselectable,
                                      S32* face_hit,
                                      LLVector4a* intersection,
                                      LLVector2* tex_coord,
                                      LLVector4a* normal,
                                      LLVector4a* tangent)
{
    if (isSelf() && !gAgent.needsRenderAvatar())
    {
        return NULL;
    }

    LLViewerObject* hit = NULL;

    if (lineSegmentBoundingBox(start, end))
    {
        LLVector4a local_end = end;
        LLVector4a local_intersection;

        for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
            iter != mAttachmentPoints.end();
            ++iter)
        {
            LLViewerJointAttachment* attachment = iter->second;

            // <FS:Ansariel> Possible crash fix
            if (!attachment)
            {
                continue;
            }
            // </FS:Ansariel>

            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                    attachment_iter != attachment->mAttachedObjects.end();
                    ++attachment_iter)
            {
                LLViewerObject* attached_object = attachment_iter->get();

                if (attached_object->lineSegmentIntersect(start, local_end, face, pick_transparent, pick_rigged, pick_unselectable, face_hit, &local_intersection, tex_coord, normal, tangent))
                {
                    local_end = local_intersection;
                    if (intersection)
                    {
                        *intersection = local_intersection;
                    }

                    hit = attached_object;
                }
            }
        }
    }

    return hit;
}


LLVOAvatar* LLVOAvatar::asAvatar()
{
    return this;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::startDefaultMotions()
//-----------------------------------------------------------------------------
void LLVOAvatar::startDefaultMotions()
{
    //-------------------------------------------------------------------------
    // start default motions
    //-------------------------------------------------------------------------
    startMotion( ANIM_AGENT_HEAD_ROT );
    startMotion( ANIM_AGENT_EYE );
    startMotion( ANIM_AGENT_BODY_NOISE );
    startMotion( ANIM_AGENT_BREATHE_ROT );
    startMotion( ANIM_AGENT_PHYSICS_MOTION );
    startMotion( ANIM_AGENT_HAND_MOTION );
    startMotion( ANIM_AGENT_PELVIS_FIX );

    //-------------------------------------------------------------------------
    // restart any currently active motions
    //-------------------------------------------------------------------------
    processAnimationStateChanges();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::buildCharacter()
// Deferred initialization and rebuild of the avatar.
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::buildCharacter()
{
    LLAvatarAppearance::buildCharacter();

    // Not done building yet; more to do.
    mIsBuilt = false;

    //-------------------------------------------------------------------------
    // set head offset from pelvis
    //-------------------------------------------------------------------------
    updateHeadOffset();

    //-------------------------------------------------------------------------
    // initialize lip sync morph pointers
    //-------------------------------------------------------------------------
    mOohMorph     = getVisualParam( "Lipsync_Ooh" );
    mAahMorph     = getVisualParam( "Lipsync_Aah" );

    // If we don't have the Ooh morph, use the Kiss morph
    if (!mOohMorph)
    {
        LL_WARNS() << "Missing 'Ooh' morph for lipsync, using fallback." << LL_ENDL;
        mOohMorph = getVisualParam( "Express_Kiss" );
    }

    // If we don't have the Aah morph, use the Open Mouth morph
    if (!mAahMorph)
    {
        LL_WARNS() << "Missing 'Aah' morph for lipsync, using fallback." << LL_ENDL;
        mAahMorph = getVisualParam( "Express_Open_Mouth" );
    }

    // Currently disabled for control avatars (animated objects), enabled for all others.
    if (mEnableDefaultMotions)
    {
    startDefaultMotions();
    }

    //-------------------------------------------------------------------------
    // restart any currently active motions
    //-------------------------------------------------------------------------
    processAnimationStateChanges();

    mIsBuilt = true;

    mMeshValid = true;
}

//-----------------------------------------------------------------------------
// resetVisualParams()
//-----------------------------------------------------------------------------
void LLVOAvatar::resetVisualParams()
{
    // Skeletal params
    {
        LLAvatarXmlInfo::skeletal_distortion_info_list_t::iterator iter;
        for (iter = sAvatarXmlInfo->mSkeletalDistortionInfoList.begin();
             iter != sAvatarXmlInfo->mSkeletalDistortionInfoList.end();
             ++iter)
        {
            LLPolySkeletalDistortionInfo *info = (LLPolySkeletalDistortionInfo*)*iter;
            LLPolySkeletalDistortion *param = dynamic_cast<LLPolySkeletalDistortion*>(getVisualParam(info->getID()));
            *param = LLPolySkeletalDistortion(this);
            llassert(param);
            if (!param->setInfo(info))
            {
                llassert(false);
            }
        }
    }

    // Driver parameters
    for (LLAvatarXmlInfo::driver_info_list_t::iterator iter = sAvatarXmlInfo->mDriverInfoList.begin();
         iter != sAvatarXmlInfo->mDriverInfoList.end();
         ++iter)
    {
        LLDriverParamInfo *info = *iter;
        LLDriverParam *param = dynamic_cast<LLDriverParam*>(getVisualParam(info->getID()));
        LLDriverParam::entry_list_t driven_list = param->getDrivenList();
        *param = LLDriverParam(this);
        llassert(param);
        if (!param->setInfo(info))
        {
            llassert(false);
        }
        param->setDrivenList(driven_list);
    }
}

void LLVOAvatar::applyDefaultParams()
{
    // These are params from avs with newly created copies of shape,
    // skin, hair, eyes, plus gender set as noted. Run arche_tool.py
    // to get params from some other xml appearance dump.
    std::map<S32, U8> male_params = {
        {1,33}, {2,61}, {4,85}, {5,23}, {6,58}, {7,127}, {8,63}, {10,85}, {11,63}, {12,42}, {13,0}, {14,85}, {15,63}, {16,36}, {17,85}, {18,95}, {19,153}, {20,63}, {21,34}, {22,0}, {23,63}, {24,109}, {25,88}, {27,132}, {31,63}, {33,136}, {34,81}, {35,85}, {36,103}, {37,136}, {38,127}, {80,255}, {93,203}, {98,0}, {99,0}, {105,127}, {108,0}, {110,0}, {111,127}, {112,0}, {113,0}, {114,127}, {115,0}, {116,0}, {117,0}, {119,127}, {130,114}, {131,127}, {132,99}, {133,63}, {134,127}, {135,140}, {136,127}, {137,127}, {140,0}, {141,0}, {142,0}, {143,191}, {150,0}, {155,104}, {157,0}, {162,0}, {163,0}, {165,0}, {166,0}, {167,0}, {168,0}, {169,0}, {177,0}, {181,145}, {182,216}, {183,133}, {184,0}, {185,127}, {192,0}, {193,127}, {196,170}, {198,0}, {503,0}, {505,127}, {506,127}, {507,109}, {508,85}, {513,127}, {514,127}, {515,63}, {517,85}, {518,42}, {603,100}, {604,216}, {605,214}, {606,204}, {607,204}, {608,204}, {609,51}, {616,25}, {617,89}, {619,76}, {624,204}, {625,0}, {629,127}, {637,0}, {638,0}, {646,144}, {647,85}, {649,127}, {650,132}, {652,127}, {653,85}, {654,0}, {656,127}, {659,127}, {662,127}, {663,127}, {664,127}, {665,127}, {674,59}, {675,127}, {676,85}, {678,127}, {682,127}, {683,106}, {684,47}, {685,79}, {690,127}, {692,127}, {693,204}, {700,63}, {701,0}, {702,0}, {703,0}, {704,0}, {705,127}, {706,127}, {707,0}, {708,0}, {709,0}, {710,0}, {711,127}, {712,0}, {713,159}, {714,0}, {715,0}, {750,178}, {752,127}, {753,36}, {754,85}, {755,131}, {756,127}, {757,127}, {758,127}, {759,153}, {760,95}, {762,0}, {763,140}, {764,74}, {765,27}, {769,127}, {773,127}, {775,0}, {779,214}, {780,204}, {781,198}, {785,0}, {789,0}, {795,63}, {796,30}, {799,127}, {800,226}, {801,255}, {802,198}, {803,255}, {804,255}, {805,255}, {806,255}, {807,255}, {808,255}, {812,255}, {813,255}, {814,255}, {815,204}, {816,0}, {817,255}, {818,255}, {819,255}, {820,255}, {821,255}, {822,255}, {823,255}, {824,255}, {825,255}, {826,255}, {827,255}, {828,0}, {829,255}, {830,255}, {834,255}, {835,255}, {836,255}, {840,0}, {841,127}, {842,127}, {844,255}, {848,25}, {858,100}, {859,255}, {860,255}, {861,255}, {862,255}, {863,84}, {868,0}, {869,0}, {877,0}, {879,51}, {880,132}, {921,255}, {922,255}, {923,255}, {10000,0}, {10001,0}, {10002,25}, {10003,0}, {10004,25}, {10005,23}, {10006,51}, {10007,0}, {10008,25}, {10009,23}, {10010,51}, {10011,0}, {10012,0}, {10013,25}, {10014,0}, {10015,25}, {10016,23}, {10017,51}, {10018,0}, {10019,0}, {10020,25}, {10021,0}, {10022,25}, {10023,23}, {10024,51}, {10025,0}, {10026,25}, {10027,23}, {10028,51}, {10029,0}, {10030,25}, {10031,23}, {10032,51}, {11000,1}, {11001,127}
    };
    std::map<S32, U8> female_params = {
        {1,33}, {2,61}, {4,85}, {5,23}, {6,58}, {7,127}, {8,63}, {10,85}, {11,63}, {12,42}, {13,0}, {14,85}, {15,63}, {16,36}, {17,85}, {18,95}, {19,153}, {20,63}, {21,34}, {22,0}, {23,63}, {24,109}, {25,88}, {27,132}, {31,63}, {33,136}, {34,81}, {35,85}, {36,103}, {37,136}, {38,127}, {80,0}, {93,203}, {98,0}, {99,0}, {105,127}, {108,0}, {110,0}, {111,127}, {112,0}, {113,0}, {114,127}, {115,0}, {116,0}, {117,0}, {119,127}, {130,114}, {131,127}, {132,99}, {133,63}, {134,127}, {135,140}, {136,127}, {137,127}, {140,0}, {141,0}, {142,0}, {143,191}, {150,0}, {155,104}, {157,0}, {162,0}, {163,0}, {165,0}, {166,0}, {167,0}, {168,0}, {169,0}, {177,0}, {181,145}, {182,216}, {183,133}, {184,0}, {185,127}, {192,0}, {193,127}, {196,170}, {198,0}, {503,0}, {505,127}, {506,127}, {507,109}, {508,85}, {513,127}, {514,127}, {515,63}, {517,85}, {518,42}, {603,100}, {604,216}, {605,214}, {606,204}, {607,204}, {608,204}, {609,51}, {616,25}, {617,89}, {619,76}, {624,204}, {625,0}, {629,127}, {637,0}, {638,0}, {646,144}, {647,85}, {649,127}, {650,132}, {652,127}, {653,85}, {654,0}, {656,127}, {659,127}, {662,127}, {663,127}, {664,127}, {665,127}, {674,59}, {675,127}, {676,85}, {678,127}, {682,127}, {683,106}, {684,47}, {685,79}, {690,127}, {692,127}, {693,204}, {700,63}, {701,0}, {702,0}, {703,0}, {704,0}, {705,127}, {706,127}, {707,0}, {708,0}, {709,0}, {710,0}, {711,127}, {712,0}, {713,159}, {714,0}, {715,0}, {750,178}, {752,127}, {753,36}, {754,85}, {755,131}, {756,127}, {757,127}, {758,127}, {759,153}, {760,95}, {762,0}, {763,140}, {764,74}, {765,27}, {769,127}, {773,127}, {775,0}, {779,214}, {780,204}, {781,198}, {785,0}, {789,0}, {795,63}, {796,30}, {799,127}, {800,226}, {801,255}, {802,198}, {803,255}, {804,255}, {805,255}, {806,255}, {807,255}, {808,255}, {812,255}, {813,255}, {814,255}, {815,204}, {816,0}, {817,255}, {818,255}, {819,255}, {820,255}, {821,255}, {822,255}, {823,255}, {824,255}, {825,255}, {826,255}, {827,255}, {828,0}, {829,255}, {830,255}, {834,255}, {835,255}, {836,255}, {840,0}, {841,127}, {842,127}, {844,255}, {848,25}, {858,100}, {859,255}, {860,255}, {861,255}, {862,255}, {863,84}, {868,0}, {869,0}, {877,0}, {879,51}, {880,132}, {921,255}, {922,255}, {923,255}, {10000,0}, {10001,0}, {10002,25}, {10003,0}, {10004,25}, {10005,23}, {10006,51}, {10007,0}, {10008,25}, {10009,23}, {10010,51}, {10011,0}, {10012,0}, {10013,25}, {10014,0}, {10015,25}, {10016,23}, {10017,51}, {10018,0}, {10019,0}, {10020,25}, {10021,0}, {10022,25}, {10023,23}, {10024,51}, {10025,0}, {10026,25}, {10027,23}, {10028,51}, {10029,0}, {10030,25}, {10031,23}, {10032,51}, {11000,1}, {11001,127}
    };
    std::map<S32, U8> *params = NULL;
    if (getSex() == SEX_MALE)
        params = &male_params;
    else
        params = &female_params;

    for( auto it = params->begin(); it != params->end(); ++it)
    {
        LLVisualParam* param = getVisualParam(it->first);
        if( !param )
        {
            // invalid id
            break;
        }

        U8 value = it->second;
        F32 newWeight = U8_to_F32(value, param->getMinWeight(), param->getMaxWeight());
        // <FS:Ansariel> [Legacy Bake]
        //param->setWeight(newWeight);
        param->setWeight(newWeight, false); // Most likely false is correct here because it's used in resetSkeleton, which is a local operation
    }
}

//-----------------------------------------------------------------------------
// resetSkeleton()
//-----------------------------------------------------------------------------
void LLVOAvatar::resetSkeleton(bool reset_animations)
{
    LL_DEBUGS("Avatar") << avString() << " reset starts" << LL_ENDL;
    if (!isControlAvatar() && !mLastProcessedAppearance)
    {
        LL_WARNS() << "Can't reset avatar " << getID() << "; no appearance message has been received yet." << LL_ENDL;
        return;
    }

    // Save mPelvis state
    //LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    //LLQuaternion pelvis_rot = getJoint("mPelvis")->getRotation();

    // Clear all attachment pos and scale overrides
    clearAttachmentOverrides();

    // Note that we call buildSkeleton twice in this function. The first time is
    // just to get the right scale for the collision volumes, because
    // this will be used in setting the mJointScales for the
    // LLPolySkeletalDistortions of which the CVs are children.
    if( !buildSkeleton(sAvatarSkeletonInfo) )
    {
        LL_ERRS() << "Error resetting skeleton" << LL_ENDL;
    }

    // Reset some params to default state, without propagating changes downstream.
    resetVisualParams();

    // Now we have to reset the skeleton again, because its state
    // got clobbered by the resetVisualParams() calls
    // above.
    if( !buildSkeleton(sAvatarSkeletonInfo) )
    {
        LL_ERRS() << "Error resetting skeleton" << LL_ENDL;
    }

    // Reset attachment points
    // BuildSkeleton only does bones and CVs but we still need to reinit huds
    // since huds can be animated.
    bool ignore_hud_joints = !isSelf();
    initAttachmentPoints(ignore_hud_joints);

    // Fix up collision volumes
    for (LLVisualParam *param = getFirstVisualParam();
         param;
         param = getNextVisualParam())
    {
        LLPolyMorphTarget *poly_morph = dynamic_cast<LLPolyMorphTarget*>(param);
        if (poly_morph)
        {
            // This is a kludgy way to correct for the fact that the
            // collision volumes have been reset out from under the
            // poly morph sliders.
            F32 delta_weight = poly_morph->getLastWeight() - poly_morph->getDefaultWeight();
            poly_morph->applyVolumeChanges(delta_weight);
        }
    }

    // Reset tweakable params to preserved state
    if (getOverallAppearance() == AOA_NORMAL)
    {
        if (mLastProcessedAppearance)
        {
            bool slam_params = true;
            applyParsedAppearanceMessage(*mLastProcessedAppearance, slam_params);
        }
    }
    else
    {
        // Stripped down approximation of
        // applyParsedAppearanceMessage, but with alternative default
        // (jellydoll) params
        setCompositeUpdatesEnabled( false );
        gPipeline.markRenderRebuild(this);
        applyDefaultParams();
        setCompositeUpdatesEnabled( true );
        updateMeshTextures();
        updateMeshVisibility();
    }
    updateVisualParams();

    // Restore attachment pos overrides
    updateAttachmentOverrides();

    // Animations
    if (reset_animations)
    {
        if (isSelf())
        {
            // This is equivalent to "Stop Animating Me". Will reset
            // all animations and propagate the changes to other
            // viewers.
            gAgent.stopCurrentAnimations();
        }
        else
        {
            // Local viewer-side reset for non-self avatars.
            resetAnimations();
        }

        // <FS:Ansariel> FIRE-22135: Try to re-register LLPhysicsMotionController to see if that unfreezes stuck physics
        removeMotion(ANIM_AGENT_PHYSICS_MOTION);
        registerMotion(ANIM_AGENT_PHYSICS_MOTION, LLPhysicsMotionController::create);
        startMotion(ANIM_AGENT_PHYSICS_MOTION);
        // </FS:Ansariel>
    }

    LL_DEBUGS("Avatar") << avString() << " reset ends" << LL_ENDL;
}

//-----------------------------------------------------------------------------
// releaseMeshData()
//-----------------------------------------------------------------------------
void LLVOAvatar::releaseMeshData()
{
    if (sInstances.size() < AVATAR_RELEASE_THRESHOLD || isUIAvatar())
    {
        return;
    }

    // cleanup mesh data
    for (avatar_joint_list_t::iterator iter = mMeshLOD.begin();
         iter != mMeshLOD.end();
         ++iter)
    {
        LLAvatarJoint* joint = (*iter);
        joint->setValid(false, true);
    }

    //cleanup data
    if (mDrawable.notNull())
    {
        LLFace* facep = mDrawable->getFace(0);
        if (facep)
        {
        facep->setSize(0, 0);
        for(S32 i = mNumInitFaces ; i < mDrawable->getNumFaces(); i++)
        {
            facep = mDrawable->getFace(i);
                if (facep)
                {
            facep->setSize(0, 0);
        }
    }
        }
    }

    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        // <FS:Ansariel> Possible crash fix
        //if (!attachment->getIsHUDAttachment())
        if (attachment && !attachment->getIsHUDAttachment())
        // </FS:Ansariel>
        {
            attachment->setAttachmentVisibility(false);
        }
    }
    mMeshValid = false;
}

//-----------------------------------------------------------------------------
// restoreMeshData()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::restoreMeshData()
{
    llassert(!isSelf());
    if (mDrawable.isNull())
    {
        return;
    }

    //LL_INFOS() << "Restoring" << LL_ENDL;
    mMeshValid = true;
    updateJointLODs();

    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (!attachment->getIsHUDAttachment())
        {
            attachment->setAttachmentVisibility(true);
        }
    }

    // force mesh update as LOD might not have changed to trigger this
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_GEOMETRY);
}

//-----------------------------------------------------------------------------
// updateMeshData()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateMeshData()
{
    if (mDrawable.notNull())
    {
        S32 f_num = 0 ;
        const U32 VERTEX_NUMBER_THRESHOLD = 128 ;//small number of this means each part of an avatar has its own vertex buffer.
        const auto num_parts = mMeshLOD.size();

        // this order is determined by number of LODS
        // if a mesh earlier in this list changed LODs while a later mesh doesn't,
        // the later mesh's index offset will be inaccurate
        for(S32 part_index = 0 ; part_index < num_parts ;)
        {
            S32 j = part_index ;
            U32 last_v_num = 0, num_vertices = 0 ;
            U32 last_i_num = 0, num_indices = 0 ;

            while(part_index < num_parts && num_vertices < VERTEX_NUMBER_THRESHOLD)
            {
                last_v_num = num_vertices ;
                last_i_num = num_indices ;

                LLViewerJoint* part_mesh = getViewerJoint(part_index++);
                if (part_mesh)
                {
                    part_mesh->updateFaceSizes(num_vertices, num_indices, mAdjustedPixelArea);
                }
            }
            if(num_vertices < 1)//skip empty meshes
            {
                continue ;
            }
            if(last_v_num > 0)//put the last inserted part into next vertex buffer.
            {
                num_vertices = last_v_num ;
                num_indices = last_i_num ;
                part_index-- ;
            }

            LLFace* facep = NULL;
            if(f_num < mDrawable->getNumFaces())
            {
                facep = mDrawable->getFace(f_num);
            }
            else
            {
                facep = mDrawable->getFace(0);
                if (facep)
                {
                    facep = mDrawable->addFace(facep->getPool(), facep->getTexture()) ;
                }
            }
            if (!facep) continue;

            // resize immediately
            facep->setSize(num_vertices, num_indices);

            bool terse_update = false;

            facep->setGeomIndex(0);
            facep->setIndicesIndex(0);

            LLVertexBuffer* buff = facep->getVertexBuffer();
            if(!facep->getVertexBuffer())
            {
                buff = new LLVertexBuffer(LLDrawPoolAvatar::VERTEX_DATA_MASK);
                if (!buff->allocateBuffer(num_vertices, num_indices))
                {
                    LL_WARNS() << "Failed to allocate Vertex Buffer for Mesh to "
                        << num_vertices << " vertices and "
                        << num_indices << " indices" << LL_ENDL;
                    // Attempt to create a dummy triangle (one vertex, 3 indices, all 0)
                    facep->setSize(1, 3);
                    buff->allocateBuffer(1, 3);
                    memset((U8*) buff->getMappedData(), 0, buff->getSize());
                    memset((U8*) buff->getMappedIndices(), 0, buff->getIndicesSize());
                }
                else
                {
                    // <FS> a new buffer holds whatever the heap held: the slots no part of the mesh writes (the vertices between the parts when a part changes its level of detail and the later parts' offsets lag, see the
                    // comment above) came out as garbage positions (1.5e25, 3e32: the VertexCheck and the ray scene's bad range warnings, 2-9 vertices a buffer): zero, which makes a degenerate triangle at the origin
                    memset((U8*) buff->getMappedData(), 0, buff->getSize());
                }
                facep->setVertexBuffer(buff);
            }
            else
            {
                if (buff->getNumIndices() == num_indices &&
                    buff->getNumVerts() == num_vertices)
                {
                    terse_update = true;
                }
                else
                {
                    buff = new LLVertexBuffer(buff->getTypeMask());
                    if (!buff->allocateBuffer(num_vertices, num_indices))
                    {
                        LL_WARNS() << "Failed to allocate vertex buffer for Mesh, Substituting" << LL_ENDL;
                        // Attempt to create a dummy triangle (one vertex, 3 indices, all 0)
                        facep->setSize(1, 3);
                        buff->allocateBuffer(1, 3);
                        memset((U8*) buff->getMappedData(), 0, buff->getSize());
                        memset((U8*) buff->getMappedIndices(), 0, buff->getIndicesSize());
                    }
                    else
                    {
                        memset((U8*) buff->getMappedData(), 0, buff->getSize());        // <FS> as above
                    }
                }
            }


            // This is a hack! Avatars have their own pool, so we are detecting
            //   the case of more than one avatar in the pool (thus > 0 instead of >= 0)
            if (facep->getGeomIndex() > 0)
            {
                LL_ERRS() << "non-zero geom index: " << facep->getGeomIndex() << " in LLVOAvatar::restoreMeshData" << LL_ENDL;
            }

            if (num_vertices == buff->getNumVerts() && num_indices == buff->getNumIndices())
            {
                for(S32 k = j ; k < part_index ; k++)
                {
                    bool rigid = false;
                    if (k == MESH_ID_EYEBALL_LEFT ||
                        k == MESH_ID_EYEBALL_RIGHT)
                    {
                        //eyeballs can't have terse updates since they're never rendered with
                        //the hardware skinning shader
                        rigid = true;
                    }

                    LLViewerJoint* mesh = getViewerJoint(k);
                    if (mesh)
                    {
                        mesh->updateFaceData(facep, mAdjustedPixelArea, k == MESH_ID_HAIR, terse_update && !rigid);
                    }
                }
            }

            buff->unmapBuffer();

            if(!f_num)
            {
                f_num += mNumInitFaces ;
            }
            else
            {
                f_num++ ;
            }
        }

        mDirtyMesh = 0;
        mNeedsSkin = true;
        mDrawable->clearState(LLDrawable::REBUILD_GEOMETRY);
    }
}

//------------------------------------------------------------------------

//------------------------------------------------------------------------
// LLVOAvatar::processUpdateMessage()
//------------------------------------------------------------------------
U32 LLVOAvatar::processUpdateMessage(LLMessageSystem *mesgsys,
                                     void **user_data,
                                     U32 block_num, const EObjectUpdateType update_type,
                                     LLDataPacker *dp)
{
    const bool had_no_name = !getNVPair("FirstName");

    // Do base class updates...
    U32 retval = LLViewerObject::processUpdateMessage(mesgsys, user_data, block_num, update_type, dp);

    // Print out arrival information once we have name of avatar.
    const bool has_name = getNVPair("FirstName");
    if (had_no_name && has_name)
    {
        mDebugExistenceTimer.reset();
        debugAvatarRezTime("AvatarRezArrivedNotification", "avatar arrived");
    }

    if (retval & LLViewerObject::INVALID_UPDATE)
    {
        if (isSelf())
        {
            //tell sim to cancel this update
            gAgent.teleportViaLocation(gAgent.getPositionGlobal());
        }
    }

    return retval;
}

LLViewerFetchedTexture *LLVOAvatar::getBakedTextureImage(const U8 te, const LLUUID& uuid)
{
    LLViewerFetchedTexture *result = NULL;

    if (uuid == IMG_DEFAULT_AVATAR ||
        uuid == IMG_DEFAULT ||
        uuid == IMG_INVISIBLE)
    {
        // Should already exist, don't need to find it on sim or baked-texture host.
        result = gTextureList.findImage(uuid, TEX_LIST_STANDARD);
    }
    if (!result)
    {
        const std::string url = getImageURL(te,uuid);

        if (url.empty())
        {
            // <FS:Ansariel> [Legacy Bake]
            //LL_WARNS() << "unable to determine URL for te " << te << " uuid " << uuid << LL_ENDL;
            //return NULL;
            LL_DEBUGS("Avatar") << avString() << "get old-bake image from host " << uuid << LL_ENDL;
            LLHost host = getObjectHost();
            result = LLViewerTextureManager::getFetchedTexture(
                uuid, FTT_HOST_BAKE, true, LLBaseTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE, TexelFormat::None, PixelFormat::None, host);
            // </FS:Ansariel> [Legacy Bake]
        }
        LL_DEBUGS("Avatar") << avString() << "get server-bake image from URL " << url << LL_ENDL;
        result = LLViewerTextureManager::getFetchedTextureFromUrl(
            url, FTT_SERVER_BAKE, true, LLBaseTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE, TexelFormat::None, PixelFormat::None, uuid);
        if (result->isMissingAsset())
        {
            result->setIsMissingAsset(false);
        }

    }
    return result;
}

// virtual
S32 LLVOAvatar::setTETexture(const U8 te, const LLUUID& uuid)
{
    if (!isIndexBakedTexture((ETextureIndex)te))
    {
        // Sim still sends some uuids for non-baked slots sometimes - ignore.
        return LLViewerObject::setTETexture(te, LLUUID::null);
    }

    LLViewerFetchedTexture *image = getBakedTextureImage(te,uuid);
    llassert(image);
    return setTETextureCore(te, image);
}

//------------------------------------------------------------------------
// LLVOAvatar::dumpAnimationState()
//------------------------------------------------------------------------
void LLVOAvatar::dumpAnimationState()
{
    LL_INFOS() << "==============================================" << LL_ENDL;
    for (LLVOAvatar::AnimIterator it = mSignaledAnimations.begin(); it != mSignaledAnimations.end(); ++it)
    {
        LLUUID id = it->first;
        std::string playtag = "";
        if (mPlayingAnimations.find(id) != mPlayingAnimations.end())
        {
            playtag = "*";
        }
        LL_INFOS() << gAnimLibrary.animationName(id) << playtag << LL_ENDL;
    }
    for (LLVOAvatar::AnimIterator it = mPlayingAnimations.begin(); it != mPlayingAnimations.end(); ++it)
    {
        LLUUID id = it->first;
        bool is_signaled = mSignaledAnimations.find(id) != mSignaledAnimations.end();
        if (!is_signaled)
        {
            LL_INFOS() << gAnimLibrary.animationName(id) << "!S" << LL_ENDL;
        }
    }
}

// <FS> Where the main thread's avatar update time goes, per avatar and path (logged from LLViewerObjectList::update every 300 frames; reset there).
U64 gFSAvStatNs[32];
U32 gFSAvStatCount[32];
bool gFSAvTimersOn = getenv("FS_AVATAR_TIMERS") != nullptr; // the timers cost a clock read pair per scope: opt-in (log tag AvatarUpdate)
namespace
{
enum
{
    AV_IDLE_TOTAL,      // idleUpdate, the serial part (a deferred avatar's completion is counted apart)
    AV_IDLE_PRE,        // idleUpdate before updateCharacter
    AV_CHAR_TOTAL,      // updateCharacter
    AV_CHAR_EARLY,      // updateMotions(HIDDEN) of an avatar that needs no update
    AV_CHAR_ROOT,       // updateRootPositionAndRotation
    AV_CHAR_HIDDEN,     // updateMotions(HIDDEN) of an invisible avatar
    AV_CHAR_BEGIN,      // beginNormalMotionUpdate (deferred)
    AV_CHAR_INLINE,     // updateMotions(NORMAL) inline: not deferred (self, animesh, ...)
    AV_CHAR_POST,       // updateCharacterPostMotion inline
    AV_IDLE_POSTCHAR,   // idleUpdatePostCharacter inline
    AV_DEFERRED_DONE,   // completeDeferredMotionUpdate
    AV_INL_SELF,        // counts only: why an avatar's normal update ran inline
    AV_INL_CONTROL,
    AV_INL_OTHER,
    AV_PC_VOICEVIS,     // idleUpdatePostCharacter pieces
    AV_PC_NAMEPOS,
    AV_PC_MISC,
    AV_PC_APPEARANCE,
    AV_PC_DETAILED,     // lipsync, loading effect, below water, wind
    AV_PC_NAMETAG,
    AV_PC_COMPLEXITY,
    AV_PC_DEBUGINFO,
    AV_PM_HEADOFFSET,   // updateCharacterPostMotion pieces
    AV_PM_FOOTSTEP,
    AV_PM_MATRIX,
    AV_ROOT_ORIENT,     // updateOrientation inside updateRootPositionAndRotation
    AV_ROOT_HEIGHT,     // resolveHeightGlobal (the ground under the pelvis) inside updateRootPositionAndRotation
    AV_ORIENT_MATRIX,   // the root's world matrix and its euler angles inside updateOrientation
    AV_COUNT
};

struct AvScope
{
    int mSlot;
    std::chrono::steady_clock::time_point mStart;
    explicit AvScope(int slot) : mSlot(slot)
    {
        if (gFSAvTimersOn)
        {
            mStart = std::chrono::steady_clock::now();
        }
    }
    ~AvScope()
    {
        if (gFSAvTimersOn)
        {
            gFSAvStatNs[mSlot] += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mStart).count();
            gFSAvStatCount[mSlot]++;
        }
    }
};

// What the attachment loop of idleUpdateMisc does per attachment kind (FS_AVATAR_TIMERS=1; logged as AttLoop every 600 frames): doc/vulkan/PLAN-texture-handles.md section 17.13
struct AttStats
{
    U64 avatars = 0, skipFrames = 0, attachments = 0, rigged = 0, rigid = 0, noBridge = 0, skipped = 0, ovApplied = 0, ovSkipped = 0, riggedSelected = 0;
    U64 clearRigid = 0, clearRigged = 0, clearFaces = 0, noFaces = 0;      // census: attachments (root and children) whose faces all have a colour alpha of 0, and how many faces that is; attachments with no face at all
    U64 riggedByDist[5] = {}, riggedRanByDist[5] = {}, riggedNsByDist[5] = {}, riggedRanNsByDist[5] = {};      // census: rigged attachments by the avatar's distance (under 8, 8-16, 16-32, 32-64, over 64 m): visited, run in full (not skipped), their time, the time of the ones run in full
    U64 rigidSkipped = 0;      // rigid attachments whose drawable and bridge move were left out (FSSkipStaticRigidAttachmentUpdates)
    U64 rigidFirst = 0, rigidSame = 0, rigidMovedTiny = 0, rigidMoved1mm = 0;      // rigid attachments: the attachment point's world matrix against the one when the loop last saw it (unchanged / more than 0.1 mm / more than 1 mm)
    U64 nsPre = 0, nsLoop = 0, nsRigged = 0, nsRigid = 0, nsExtents = 0, nsOverride = 0, nsMove = 0, nsBridge = 0, nsText = 0;
    U32 firstFrame = 0;
};
AttStats gAttStats;
// an attachment that only follows its joint: nothing selected, flexible, animated, lighted, sounding or emitting particles in it or its children, not a HUD
inline bool att_plain(const LLViewerObject* root)
{
    auto plain = [](const LLViewerObject* o)
    {
        if (o->isSelected() || o->isFlexible() || o->isAnimatedObject() || o->isAudioSource() || o->isParticleSource())
        {
            return false;
        }
        return !(o->getPCode() == LL_PCODE_VOLUME && static_cast<const LLVOVolume*>(o)->getIsLight());
    };
    if (root->isHUDAttachment() || !plain(root))
    {
        return false;
    }
    if (root->numChildren() > 0)
    {
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            if (child.notNull() && !plain(child.get()))
            {
                return false;
            }
        }
    }
    return true;
}
// census: 1 = the attachment has faces and every one has a texture entry colour alpha of 0 (nothing of it is drawn), 0 = something is visible, -1 = no face at all
inline int att_all_clear(const LLViewerObject* root, U64& faces)
{
    int seen = 0;
    auto scan = [&](const LLViewerObject* o) -> bool
    {
        const LLDrawable* d = o->mDrawable;
        if (!d)
        {
            return true;
        }
        for (S32 i = 0; i < d->getNumFaces(); ++i)
        {
            const LLFace* f = d->getFace(i);
            if (f)
            {
                ++seen;
                const LLTextureEntry* te = f->getTextureEntry();
                // visible: any alpha, a glow (it shows through an alpha of 0), or a glTF material (its own base colour alpha decides)
                if (!te || te->getColor().mV[3] > 0.f || te->getGlow() > 0.f || te->getGLTFRenderMaterial())
                {
                    return false;
                }
            }
        }
        return true;
    };
    bool clear = scan(root);
    if (clear && root->numChildren() > 0)
    {
        for (const LLPointer<LLViewerObject>& child : root->getChildren())
        {
            if (child.notNull() && !scan(child.get()))
            {
                clear = false;
                break;
            }
        }
    }
    if (!seen)
    {
        return -1;
    }
    faces += clear ? (U64)seen : 0;
    return clear ? 1 : 0;
}
inline std::chrono::steady_clock::time_point att_now() { return gFSAvTimersOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point(); }
inline U64 att_ns(std::chrono::steady_clock::time_point since) { return gFSAvTimersOn ? (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count() : 0; }
}

// <FS> PLAN-avatars.md stage 0 (LL_VK_AVIMP=1): the time of three regions of the avatar update by importance class and kind (defined with the class below): 0 idleUpdate (the serial part), 1 the batched pose
// blend of one avatar (a pool thread), 2 completeDeferredMotionUpdate (the serial completion)
static void fs_av_imp_time(int region, const LLVOAvatar* av, U64 ns);
static bool fs_av_imp_on();
static void fs_av_piece_time(int slot, const LLVOAvatar* av, U64 ns);
static void fs_av_imp_motions(const LLVOAvatar* av, size_t motions);
namespace
{
struct AvSelfScope      // (a piece of the avatar update by importance class: the name is from the first cut, which was the own avatar alone)
{
    int mSlot;
    const LLVOAvatar* mAv;
    bool mOn;
    std::chrono::steady_clock::time_point mStart;
    AvSelfScope(int slot, const LLVOAvatar* av) : mSlot(slot), mAv(av), mOn(fs_av_imp_on())
    {
        if (mOn) { mStart = std::chrono::steady_clock::now(); }
    }
    ~AvSelfScope()
    {
        if (mOn)
        {
            fs_av_piece_time(mSlot, mAv, (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mStart).count());
        }
    }
};
struct AvImpTimer
{
    int mRegion;
    const LLVOAvatar* mAv;
    std::chrono::steady_clock::time_point mStart;
    AvImpTimer(int region, const LLVOAvatar* av) : mRegion(region), mAv(av)
    {
        if (fs_av_imp_on())
        {
            mStart = std::chrono::steady_clock::now();
        }
    }
    ~AvImpTimer()
    {
        if (fs_av_imp_on())
        {
            fs_av_imp_time(mRegion, mAv, (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mStart).count());
        }
    }
};
}

//------------------------------------------------------------------------
// idleUpdate()
//------------------------------------------------------------------------
void LLVOAvatar::idleUpdate(LLAgent &agent, const F64 &time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    AvScope idle_total_scope(AV_IDLE_TOTAL);
    AvImpTimer imp_idle_timer(0, this);

    if (LLApp::isExiting())
        return;

    if (isDead())
    {
        LL_INFOS() << "Warning!  Idle on dead avatar" << LL_ENDL;
        return;
    }

    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    if (friends_only()
        && !isUIAvatar()
        && !isControlAvatar()
        && !isSelf()
        && !isBuddy())
    {
        if (mNameText)
        {
            mNameIsSet = false;
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        deleteParticleSource();
        mVoiceVisualizer->setVoiceEnabled(false);

        return;
    }

    // record time and refresh "tooSlow" status (<FS> stage A mode 1: the table's pre-pass has done it)
    const LLAvatarTable::Row* table_row = LLAvatarTable::switchMode() == 1 ? LLAvatarTable::row(this) : nullptr;
    if (!table_row)
    {
        updateTooSlow();
    }

    static LLCachedControl<bool> disable_all_render_types(gSavedSettings, "DisableAllRenderTypes");
    if (!(gPipeline.hasRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR))
        && !disable_all_render_types && !isSelf())
    {
        if (!mIsControlAvatar)
        {
            idleUpdateNameTag(idleCalcNameTagPosition(mLastRootPos));
        }
        return;
    }

    // <FS> An avatar nobody can see does not need the whole per-frame update: its motions, matrices, name tag and the rest are all driven by elapsed time (the impostor
    // path already runs them every 32-64 frames), and it is picked up in full the frame after it comes into view. Others' avatars only, and not when something is waiting
    // for the next full update (a first update, a special render mode): every second frame, every fourth when the avatar is far away, staggered by avatar id.
    if (table_row ? table_row->mMode == LLAvatarTable::MODE_NONE : reducedHiddenSkip())
    {
        LLAvatarTable::noteActual(this, LLAvatarTable::MODE_NONE);
        return;
    }

    // Update should be happening max once per frame.
    static LLCachedControl<S32> refreshPeriod(gSavedSettings, "AvatarExtentRefreshPeriodBatch");
    static LLCachedControl<S32> refreshMaxPerPeriod(gSavedSettings, "AvatarExtentRefreshMaxPerBatch");
    static S32 upd_freq = refreshPeriod; // initialise to a reasonable default of 1 batch
    static S32 lastRecalibrationFrame{ 0 };

    const S32 thisFrame = LLDrawable::getCurrentFrame();
    if (thisFrame - lastRecalibrationFrame >= upd_freq)
    {
        // Only update at the start of a cycle. .
        // update frequency = ((Num_Avatars -1 / NumberPerPeriod) + 1 ) * Periodicity
        // Given NumberPerPeriod = 5 and Periodicity = 4
        // | NumAvatars  | frequency |
        // +-------------+-----------+
        // |      1      |     4     |
        // |      2      |     4     |
        // |      5      |     4     |
        // |     10      |     8     |
        // |     25      |     20    |

        upd_freq = (((gObjectList.getAvatarCount() - 1) / refreshMaxPerPeriod) + 1)*refreshPeriod;
        lastRecalibrationFrame = thisFrame;
    }

    if ((mLastAnimExtents[0]==LLVector3())||
        (mLastAnimExtents[1])==LLVector3())
    {
        mNeedsExtentUpdate = true;
    }
    else
    {
        // Update extent if necessary.
        // if the frame counnter + the first byte of the UUID % upd_freq = 0 then update the extent.
        mNeedsExtentUpdate = ((thisFrame + mID.mData[0]) % upd_freq == 0);
    }

    checkTextureLoading() ;

    // force immediate pixel area update on avatars using last frames data (before drawable or camera updates)
    setPixelAreaAndAngle(gAgent);

    // force asynchronous drawable update
    if(mDrawable.notNull())
    {
        if (isSitting() && getParent())
        {
            LLViewerObject *root_object = (LLViewerObject*)getRoot();
            LLDrawable* drawablep = root_object->mDrawable;
            // if this object hasn't already been updated by another avatar...
            if (drawablep) // && !drawablep->isState(LLDrawable::EARLY_MOVE))
            {
                if (root_object->isSelected())
                {
                    gPipeline.updateMoveNormalAsync(drawablep);
                }
                else
                {
                    gPipeline.updateMoveDampedAsync(drawablep);
                }
            }
        }
        else
        {
            gPipeline.updateMoveDampedAsync(mDrawable);
        }
    }

    //--------------------------------------------------------------------
    // set alpha flag depending on state
    //--------------------------------------------------------------------

    if (isSelf())
    {
        LLViewerObject::idleUpdate(agent, time);

        // trigger fidget anims
        if (isAnyAnimationSignaled(AGENT_STAND_ANIMS, NUM_AGENT_STAND_ANIMS))
        {
            agent.fidget();
        }
    }
    else
    {
        // Should override the idleUpdate stuff and leave out the angular update part.
        LLQuaternion rotation = getRotation();
        LLViewerObject::idleUpdate(agent, time);
        setRotation(rotation);
    }

    // attach objects that were waiting for a drawable
    if (!mPendingAttachment.empty())
    {
        lazyAttach();
    }

    // animate the character
    // store off last frame's root position to be consistent with camera position
    mLastRootPos = mRoot->getWorldPosition();
    if (gFSAvTimersOn)
    {
        gFSAvStatNs[AV_IDLE_PRE] += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idle_total_scope.mStart).count();
        gFSAvStatCount[AV_IDLE_PRE]++;
    }
    bool detailed_update = updateCharacter(agent);
    detailed_update = LLAvatarTable::attachmentLoop(this, detailed_update);     // <FS> stage A step 3: the gate of the attachment loop and the detailed presentation is the row's bit

    if (mMotionUpdateDeferred)
    {
        // the rest of this function runs in completeDeferredMotionUpdate()
        mDeferredDetailedUpdate = detailed_update;
        return;
    }

    {
        AvScope post_scope(AV_IDLE_POSTCHAR);
        idleUpdatePostCharacter(detailed_update);
    }
}

void LLVOAvatar::idleUpdatePostCharacter(bool detailed_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    static LLUICachedControl<bool> visualizers_in_calls("ShowVoiceVisualizersInCalls", false);
    bool voice_enabled = (visualizers_in_calls || LLVoiceClient::getInstance()->inProximalChannel()) &&
                         LLVoiceClient::getInstance()->getVoiceEnabled(mID);

    LLVector3 hud_name_pos;
    {
        AvScope s(AV_PC_NAMEPOS);
        AvSelfScope self_scope(3, this);
        LL_CPU_ZONE("av post: name tag position");
        hud_name_pos = idleCalcNameTagPosition(mLastRootPos);
    }

    {
        AvScope s(AV_PC_VOICEVIS);
        AvSelfScope self_scope(4, this);
        LL_CPU_ZONE("av post: voice visualizer");
        idleUpdateVoiceVisualizer(voice_enabled, hud_name_pos);
    }
    {
        AvScope s(AV_PC_MISC);
        AvSelfScope self_scope(5, this);
        LL_CPU_ZONE("av post: idleUpdateMisc (attachment replay, matrices)");
        idleUpdateMisc( detailed_update );
    }
    {
        AvScope s(AV_PC_APPEARANCE);
        AvSelfScope self_scope(9, this);
        LL_CPU_ZONE("av post: appearance animation");
        idleUpdateAppearanceAnimation();
    }
    if (detailed_update)
    {
        AvScope s(AV_PC_DETAILED);
        AvSelfScope self_scope(10, this);
        LL_CPU_ZONE("av post: lipsync, loading, water, wind");
        idleUpdateLipSync( voice_enabled );
        idleUpdateLoadingEffect();
        idleUpdateBelowWater(); // wind effect uses this
        idleUpdateWindEffect();
    }

    {
        AvScope s(AV_PC_NAMETAG);
        AvSelfScope self_scope(11, this);
        LL_CPU_ZONE("av post: name tag");
        if (LLAvatarTable::nameTagOpen(this))
        {
            idleUpdateNameTag(hud_name_pos);
        }
    }

    // Complexity has stale mechanics, but updates still can be very rapid
    // so spread avatar complexity calculations over frames to lesen load from
    // rapid updates and to make sure all avatars are not calculated at once.
    S32 compl_upd_freq = 20;
    if (isControlAvatar())
    {
        // animeshes do not (or won't) have impostors nor change outfis,
        // no need for high frequency
        compl_upd_freq = 100;
    }
    else if (mLastRezzedStatus <= 0) //cloud or  init
    {
        compl_upd_freq = 60;
    }
    else if (isSelf())
    {
        compl_upd_freq = 5;
    }
    else if (mLastRezzedStatus == 1) //'grey', not fully loaded
    {
        compl_upd_freq = 40;
    }
    else if (isInMuteList()) //cheap, buffers value from search
    {
        compl_upd_freq = 100;
    }

    if ((LLFrameTimer::getFrameCount() + mID.mData[0]) % compl_upd_freq == 0)
    {
        // DEPRECATED
        // replace with LLPipeline::profileAvatar?
        // Avatar profile takes ~ 0.5ms while idleUpdateRenderComplexity takes ~5ms
        // (both are unacceptably costly)
        AvScope s(AV_PC_COMPLEXITY);
        AvSelfScope self_scope(12, this);
        idleUpdateRenderComplexity();
    }
    {
        AvScope s(AV_PC_DEBUGINFO);
        AvSelfScope self_scope(13, this);
        idleUpdateDebugInfo();
    }
}

void LLVOAvatar::idleUpdateVoiceVisualizer(bool voice_enabled, const LLVector3 &position)
{
    bool render_visualizer = voice_enabled;

    // Don't render the user's own voice visualizer when in mouselook, or when opening the mic is disabled.
    if(isSelf())
    {
        static LLCachedControl<bool> voice_disable_mic(gSavedSettings, "VoiceDisableMic");
        static LLCachedControl<bool> fsShowMyOwnVoiceVisualizer(gSavedSettings, "FSShowMyOwnVoiceVisualizer"); // <FS:PP> FIRE-21210: Don't show my voice visualizer
        if (gAgentCamera.cameraMouselook() || voice_disable_mic || !fsShowMyOwnVoiceVisualizer)
        {
            render_visualizer = false;
        }
    }

    // <FS:Ansariel> FIRE-1916: Hide voice dots over avatars
    static LLCachedControl<bool> voiceVisualizerEnabled(gSavedSettings, "VoiceVisualizerEnabled");
    if (!voiceVisualizerEnabled)
    {
        render_visualizer = false;
    }
    // </FS:Ansariel>

    mVoiceVisualizer->setVoiceEnabled(render_visualizer);

    if ( voice_enabled )
    {
        //----------------------------------------------------------------
        // Only do gesture triggering for your own avatar, and only when you're in a proximal channel.
        //----------------------------------------------------------------
        if( isSelf() )
        {
            //----------------------------------------------------------------------------------------
            // The following takes the voice signal and uses that to trigger gesticulations.
            //----------------------------------------------------------------------------------------
            int lastGesticulationLevel = mCurrentGesticulationLevel;
            mCurrentGesticulationLevel = mVoiceVisualizer->getCurrentGesticulationLevel();

            //---------------------------------------------------------------------------------------------------
            // If "current gesticulation level" changes, we catch this, and trigger the new gesture
            //---------------------------------------------------------------------------------------------------
            if ( lastGesticulationLevel != mCurrentGesticulationLevel )
            {
                if ( mCurrentGesticulationLevel != VOICE_GESTICULATION_LEVEL_OFF )
                {
                    std::string gestureString = "unInitialized";
                    if ( mCurrentGesticulationLevel == 0 )  { gestureString = "/voicelevel1";   }
                    else    if ( mCurrentGesticulationLevel == 1 )  { gestureString = "/voicelevel2";   }
                    else    if ( mCurrentGesticulationLevel == 2 )  { gestureString = "/voicelevel3";   }
                    else    { LL_INFOS() << "oops - CurrentGesticulationLevel can be only 0, 1, or 2"  << LL_ENDL; }

                    // this is the call that Karl S. created for triggering gestures from within the code.
                    LLGestureMgr::instance().triggerAndReviseString( gestureString );
                }
            }

        } //if( isSelf() )

        //-----------------------------------------------------------------------------------------------------------------
        // If the avatar is speaking, then the voice amplitude signal is passed to the voice visualizer.
        // Also, here we trigger voice visualizer start and stop speaking, so it can animate the voice symbol.
        //
        // Notice the calls to "gAwayTimer.reset()". This resets the timer that determines how long the avatar has been
        // "away", so that the avatar doesn't lapse into away-mode (and slump over) while the user is still talking.
        //-----------------------------------------------------------------------------------------------------------------
        if (LLVoiceClient::getInstance()->getIsSpeaking( mID ) && (!isInMuteList() || isSelf()))
        {
            if (!mVoiceVisualizer->getCurrentlySpeaking())
            {
                mVoiceVisualizer->setStartSpeaking();

                //printf( "gAwayTimer.reset();\n" );
            }

            mVoiceVisualizer->setSpeakingAmplitude( LLVoiceClient::getInstance()->getCurrentPower( mID ) );

            if( isSelf() )
            {
                gAgent.clearAFK();
            }
        }
        else
        {
            if ( mVoiceVisualizer->getCurrentlySpeaking() )
            {
                mVoiceVisualizer->setStopSpeaking();

                if ( mLipSyncActive )
                {
                    // <FS:Ansariel> [Legacy Bake]
                    //if( mOohMorph ) mOohMorph->setWeight(mOohMorph->getMinWeight());
                    //if( mAahMorph ) mAahMorph->setWeight(mAahMorph->getMinWeight());
                    if( mOohMorph ) mOohMorph->setWeight(mOohMorph->getMinWeight(), false);
                    if( mAahMorph ) mAahMorph->setWeight(mAahMorph->getMinWeight(), false);
                    // </FS:Ansariel> [Legacy Bake]

                    mLipSyncActive = false;
                    LLCharacter::updateVisualParams();
                    dirtyMesh();
                }
            }
        }
        mVoiceVisualizer->setPositionAgent(position);
    }//if ( voiceEnabled )
}

// <FS> debug switch (live): skip override_bbox() when the box has not changed
static bool FSSkipUnchangedBBoxOverride()
{
    static LLCachedControl<bool> skip(gSavedSettings, "FSSkipUnchangedBBoxOverride", true);
    return skip;
}

static void override_bbox(LLDrawable* drawable, LLVector4a* extents)
{
    LL_CPU_ZONE("octree: avatar override_bbox");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_SPATIAL;

    // <FS> A rigged attachment keeps the box it was given (LLVOVolume::updateSpatialExtents does nothing), so giving it the same box again only makes
    // movePartition() run updateInGroup(): the bin radius is worked out again, the group is marked dirty, for nothing. Skip it when the box has not
    // moved (within a centimetre) and the drawable is still in its octree node.
    if (drawable->getSpatialGroup() && FSSkipUnchangedBBoxOverride())
    {
        const F32* cur = drawable->getSpatialExtents()[0].getF32ptr();
        const F32* cur_max = drawable->getSpatialExtents()[1].getF32ptr();
        const F32* pos = drawable->getPositionGroup().getF32ptr();
        const F32* new_min = extents[0].getF32ptr();
        const F32* new_max = extents[1].getF32ptr();
        constexpr F32 tolerance = 0.01f;
        if (pos[0] == 0.f && pos[1] == 0.f && pos[2] == 0.f
            && fabsf(cur[0] - new_min[0]) < tolerance && fabsf(cur[1] - new_min[1]) < tolerance && fabsf(cur[2] - new_min[2]) < tolerance
            && fabsf(cur_max[0] - new_max[0]) < tolerance && fabsf(cur_max[1] - new_max[1]) < tolerance && fabsf(cur_max[2] - new_max[2]) < tolerance)
        {
            ++gAttStats.ovSkipped;
            return;
        }
    }

    ++gAttStats.ovApplied;
    LL_CPU_ZONE("octree: avatar override_bbox applied");
    drawable->setSpatialExtents(extents[0], extents[1]);
    drawable->setPositionGroup(LLVector4a(0.f, 0.f, 0.f));
    drawable->movePartition();
}

bool LLVOAvatar::isLiveInstance(const LLVOAvatar* avatar)
{
    static U32 frame = ~0u;
    static std::unordered_set<const LLVOAvatar*> live;
    const U32 now = (U32)LLFrameTimer::getFrameCount();
    if (frame != now)
    {
        frame = now;
        live.clear();
        for (const LLCharacter* c : LLCharacter::sInstances)
        {
            if (const LLVOAvatar* a = dynamic_cast<const LLVOAvatar*>(c))
            {
                live.insert(a);
            }
        }
    }
    return avatar && live.count(avatar) != 0;
}

// <FS> view rework A1: the writes of an avatar's attachment loop to shared state are recorded and replayed in order when the loop is over (LL_VK_AV_LEDGER=1; the viewer's own avatar keeps the direct path)
namespace
{
struct AvLedgerScope
{
    std::vector<LLPipeline::AvOp>* mOps = nullptr;
    explicit AvLedgerScope(bool on)
    {
        if (on)
        {
            static thread_local std::vector<LLPipeline::AvOp> ops;
            ops.clear();
            mOps = &ops;
            LLPipeline::sAvSink = mOps;
        }
    }
    void finish()
    {
        if (mOps)
        {
            gPipeline.applyAvOps(*mOps);
            mOps = nullptr;
        }
    }
    ~AvLedgerScope() { finish(); }
};
}

S32 LLVOAvatar::sAttachmentSelectedForJobs = -1;

namespace
{
struct AvJobLedger
{
    std::vector<LLPipeline::AvOp> mOps;
    std::vector<LLPipeline::SortDelta> mSort;
};
}

void LLVOAvatar::idleUpdateAttachments(bool visible)
{
        LL_CPU_ZONE("att: attachment loop (per avatar)");
        static const bool av_ledger = !getenv("LL_VK_AV_LEDGER") || getenv("LL_VK_AV_LEDGER")[0] != '0';       // (on unless LL_VK_AV_LEDGER=0)
        AvLedgerScope ledger_scope(av_ledger && !isSelf() && !LLPipeline::sAvSink);
        const auto att_loop_t0 = att_now();
        ++gAttStats.avatars;
        LLAttCensus::sAvatarsLooped += LLAttCensus::sOn;
        U32 draw_order = 0;
        bool attachment_selected = sAttachmentSelectedForJobs >= 0 ? sAttachmentSelectedForJobs != 0
            : (LLSelectMgr::getInstance()->getSelection()->getObjectCount() > 0 && LLSelectMgr::getInstance()->getSelection()->isAttachment());

        // <FS> A rigged attachment is culled with the avatar's own world box (override_bbox) and drawn with the avatar's joints, so while that box has not
        // changed there is nothing for its per-frame update to change: the box it would get, its drawable move and its bridge move all come out the same.
        // When the avatar's box and its attachments have been the same for a couple of frames, the rigged attachments are only updated every 8th frame
        // (staggered per avatar); anything that moves the avatar, changes what it wears, or selects an attachment brings the full update back at once.
        static LLCachedControl<bool> skip_static_rigged(gSavedSettings, "FSSkipStaticRiggedAttachmentUpdates", true);
        // <FS> the same for the rigid ones, one by one: while the attachment point's world matrix is within 1 mm / 0.06 degrees of the one the attachment's last move was made with, its drawable move,
        // bridge move and everything downstream of them (moved list, partition moves) would come out the same. Not for the viewer's own avatar, a selected attachment, one that is not plain (light,
        // flexible, particles, sound, animesh, HUD, linksets with those) or one whose object changed (MOVED); every 8th frame (staggered per attachment) runs in full anyway.
        static LLCachedControl<bool> skip_static_rigid(gSavedSettings, "FSSkipStaticRigidAttachmentUpdates", true);
        const U32 rigid_frame = (U32)LLFrameTimer::getFrameCount();
        // the tolerance is a quarter of a pixel on the screen at the avatar's distance (1 mm up close, up to 2 cm far away)
        F32 rigid_tolerance = 0.001f;
        F32 att_distance = 0.f;
        if ((skip_static_rigid && !isSelf()) || gFSAvTimersOn)
        {
            LLViewerCamera* view_camera = LLViewerCamera::getInstance();
            const F32 height_px = (F32)llmax(gViewerWindow->getWorldViewHeightRaw(), 1);
            att_distance = dist_vec(getPositionAgent(), view_camera->getOrigin());
            rigid_tolerance = llclamp(att_distance * 0.25f * 2.f * tanf(0.5f * view_camera->getView()) / height_px, 0.001f, 0.02f);
        }
        const int att_bucket = att_distance < 8.f ? 0 : (att_distance < 16.f ? 1 : (att_distance < 32.f ? 2 : (att_distance < 64.f ? 3 : 4)));
        bool skip_rigged_this_frame = false;
        if (skip_static_rigged && !isSelf() && !attachment_selected && mDrawable.notNull())
        {
            const U32 attachment_count = LLViewerJointAttachment::sRevision;      // (was: the sum of the sizes of the lists of all 55 points, every frame)
            const LLVector4a* box = mDrawable->getSpatialExtents();
            const F32* box_min = box[0].getF32ptr();
            const F32* box_max = box[1].getF32ptr();
            constexpr F32 tolerance = 0.01f;
            bool same = attachment_count == mAttCountLast;
            F32 box_move = 0.f;     // how far the box moved since the last frame it changed (the avatar's speed, in metres a frame)
            for (S32 axis = 0; axis < 3; ++axis)
            {
                box_move = llmax(box_move, fabsf(box_min[axis] - mAttBBoxLast[0].mV[axis]), fabsf(box_max[axis] - mAttBBoxLast[1].mV[axis]));
            }
            for (S32 axis = 0; axis < 3 && same; ++axis)
            {
                same = fabsf(box_min[axis] - mAttBBoxLast[0].mV[axis]) < tolerance && fabsf(box_max[axis] - mAttBBoxLast[1].mV[axis]) < tolerance;
            }
            if (same)
            {
                ++mAttBBoxStable;
            }
            else
            {
                mAttBBoxStable = 0;
                mAttBBoxLast[0].set(box_min);
                mAttBBoxLast[1].set(box_max);
                mAttCountLast = attachment_count;
            }
            skip_rigged_this_frame = mAttBBoxStable >= 2 && ((LLFrameTimer::getFrameCount() + mID.mData[0]) & 7) != 0;
            // <FS> the same for an avatar that moves, when it is far: its rigged attachments are drawn with the avatar's joints (the skinning does not need the attachment's move), so what
            // they would update here is the octree box and the bridge, a cull that lags by a frame or three at 16-64 m by less than the avatar's step. 2 frames beyond 16 m, 4 beyond 32 m;
            // an avatar that moves more than 10 cm a frame (a vehicle, a run) and one that is selected are updated in full.
            static LLCachedControl<bool> stagger_far(gSavedSettings, "FSStaggerFarRiggedAttachments", true);
            if (stagger_far && !skip_rigged_this_frame && box_move < 0.1f)
            {
                const U32 period = att_distance >= 32.f ? 4 : (att_distance >= 16.f ? 2 : 1);
                skip_rigged_this_frame = period > 1 && ((LLFrameTimer::getFrameCount() + mID.mData[0]) % period) != 0;
            }
        }
        else
        {
            mAttBBoxStable = 0;
        }
        gAttStats.nsPre += att_ns(att_loop_t0);
        gAttStats.skipFrames += skip_rigged_this_frame;
        for (const auto& [attachment_point_id, attachment] : mAttachmentPoints)
        {
            // <FS:Ansariel> Possible crash fix
            if (!attachment)
            {
                continue;
            }
            // </FS:Ansariel>

            for (auto& attached_object : attachment->mAttachedObjects)
            {
                if (attached_object.isNull()
                    || attached_object->isDead()
                    || !attachment->getValid()
                    || attached_object->mDrawable.isNull())
                {
                    continue;
                }

                LLSpatialBridge* bridge = attached_object->mDrawable->getSpatialBridge();
                const auto att_t0 = att_now();
                bool att_rigged_kind = false;
                ++gAttStats.attachments;
                gAttStats.noBridge += !bridge;

                bool att_run_full = false;
                if (visible || !(bridge && bridge->getRadius() < 2.0f))
                {
                    //override rigged attachments' octree spatial extents with this avatar's bounding box
                    bool rigged = false;
                    if (bridge)
                    {
                        //transform avatar bounding box into attachment's coordinate frame
                        // (only for a rigged attachment that will take it: the rigid ones threw it away, one 4x4 inverse each)
                        LLVector4a extents[2];
                        if (attached_object->mDrawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD) && !skip_rigged_this_frame)
                        { LL_CPU_ZONE("att: transformExtents"); const auto t = att_now(); bridge->transformExtents(mDrawable->getSpatialExtents(), extents); gAttStats.nsExtents += att_ns(t); }

                        if (attached_object->mDrawable->isState(LLDrawable::RIGGED | LLDrawable::RIGGED_CHILD))
                        {
                            rigged = true;
                            if (!skip_rigged_this_frame)
                            {
                                const auto t = att_now();
                                override_bbox(attached_object->mDrawable, extents);
                                gAttStats.nsOverride += att_ns(t);
                            }
                        }
                    }
                    const bool skip_update = rigged && skip_rigged_this_frame;
                    att_run_full = !skip_update;
                    att_rigged_kind = rigged;
                    gAttStats.skipped += skip_update;
                    if (LLAttCensus::sOn && !rigged && bridge)
                    {
                        LLAttCensus::classify(attached_object.get(), attached_object->mDrawable);
                    }
                    if (gFSAvTimersOn && !rigged && bridge)
                    {
                        // measurement: does the joint the attachment follows move from one visit to the next?
                        static std::unordered_map<const LLViewerObject*, LLMatrix4a> sPrevWorld;
                        const LLMatrix4a& m = attachment->getWorldMatrix4a();
                        auto ins = sPrevWorld.try_emplace(attached_object.get(), m);
                        if (ins.second)
                        {
                            ++gAttStats.rigidFirst;
                        }
                        else
                        {
                            const F32* a = ins.first->second.getF32ptr();
                            const F32* b = m.getF32ptr();
                            F32 diff = 0.f;
                            for (int k = 0; k < 16; ++k)
                            {
                                diff = std::max(diff, fabsf(a[k] - b[k]));
                            }
                            if (diff > 1e-3f) { ++gAttStats.rigidMoved1mm; }
                            if (diff > 1e-4f) { ++gAttStats.rigidMovedTiny; }
                            else { ++gAttStats.rigidSame; }
                            ins.first->second = m;
                        }
                    }

                    if (gFSAvTimersOn && bridge)
                    {
                        // census: attachments of which nothing is drawn (every face alpha 0, no glow, no glTF material)
                        const int clear_verdict = att_all_clear(attached_object.get(), gAttStats.clearFaces);
                        if (clear_verdict > 0) { if (rigged) ++gAttStats.clearRigged; else ++gAttStats.clearRigid; }
                        else if (clear_verdict < 0) { ++gAttStats.noFaces; }
                    }
                    bool skip_rigid = false;
                    if (skip_static_rigid && !rigged && bridge && !attachment_selected && !isSelf())
                    {
                        LLDrawable* att_drawable = attached_object->mDrawable;
                        const F32* joint = attachment->getWorldMatrix4a().getF32ptr();
                        if (att_drawable->mAttJointValid && ((rigid_frame + attached_object->mID.mData[1]) & 7) != 0 && !attached_object->isChanged(LLXform::MOVED))
                        {
                            const F32* last = att_drawable->mAttJointApplied;
                            const F32 tolerance = rigid_tolerance;      // metres in the translation, the same number in the rotation elements (a lever of a metre)
                            bool same = true;
                            for (int k = 0; k < 15 && same; ++k)
                            {
                                same = fabsf(joint[k] - last[k]) <= tolerance;
                            }
                            skip_rigid = same && att_plain(attached_object.get());
                        }
                        if (!skip_rigid)
                        {
                            memcpy(att_drawable->mAttJointApplied, joint, sizeof(att_drawable->mAttJointApplied));
                            att_drawable->mAttJointValid = true;
                        }
                        gAttStats.rigidSkipped += skip_rigid;
                    }

                    // if selecting any attachments, update all of them as non-damped
                    if (!skip_update && !skip_rigid)
                    {
                        LL_CPU_ZONE("att: drawable move");
                        const auto t = att_now();
                        if (attachment_selected)
                        {
                            gPipeline.updateMoveNormalAsync(attached_object->mDrawable);
                        }
                        else
                        {
                            // Note: SL-17415; While most objects follow joints,
                            // some objects get position updates from server
                            gPipeline.updateMoveDampedAsync(attached_object->mDrawable);
                        }
                        gAttStats.nsMove += att_ns(t);
                    }

                    // override_bbox calls movePartition() and getSpatialPartition(),
                    // so bridge might no longer be valid, get it again.
                    // ex: animesh stops being an animesh
                    bridge = attached_object->mDrawable->getSpatialBridge();
                    if (bridge)
                    {
                        if (!rigged)
                        {
                            if (!skip_rigid)
                            {
                                LL_CPU_ZONE("att: bridge move (not rigged)");
                                const auto t = att_now();
                                gPipeline.updateMoveNormalAsync(bridge);
                                gAttStats.nsBridge += att_ns(t);
                            }
                        }
                        else
                        {
                            LL_CPU_ZONE("att: bridge move (rigged)");
                            //specialized impl of updateMoveNormalAsync just for rigged attachment SpatialBridge
                            if (!skip_update)
                            {
                                const auto t = att_now();
                                bridge->setState(LLDrawable::MOVE_UNDAMPED);
                                bridge->updateMove();
                                bridge->setState(LLDrawable::EARLY_MOVE);
                                gAttStats.nsBridge += att_ns(t);
                            }

                            LLSpatialGroup* group = attached_object->mDrawable->getSpatialGroup();
                            if (group)
                            { //set draw order of group
                                if (LLPipeline::sAvSink)
                                {
                                    LLPipeline::sAvSink->push_back({ LLPipeline::AvOp::RENDER_ORDER, attached_object->mDrawable, this, group, draw_order++ });
                                }
                                else
                                {
                                    group->mAvatarp = this;
                                    group->mRenderOrder = draw_order++;
                                }
                            }
                        }
                    }

                    { LL_CPU_ZONE("att: updateText"); const auto t = att_now(); attached_object->updateText(); gAttStats.nsText += att_ns(t); }
                }
                if (gFSAvTimersOn)
                {
                    const U64 ns = att_ns(att_t0);
                    if (att_rigged_kind)
                    {
                        ++gAttStats.rigged;
                        gAttStats.nsRigged += ns;
                        ++gAttStats.riggedByDist[att_bucket];
                        gAttStats.riggedNsByDist[att_bucket] += ns;
                        if (att_run_full) { ++gAttStats.riggedRanByDist[att_bucket]; gAttStats.riggedRanNsByDist[att_bucket] += ns; }
                    }
                    else { ++gAttStats.rigid; gAttStats.nsRigid += ns; }
                }
            }
        }
        gAttStats.nsLoop += att_ns(att_loop_t0);
        if (gFSAvTimersOn)
        {
            const U32 frame = (U32)LLFrameTimer::getFrameCount();
            if (!gAttStats.firstFrame)
            {
                gAttStats.firstFrame = frame;
            }
            if (frame - gAttStats.firstFrame >= 600)
            {
                const AttStats& s = gAttStats;
                const double F = (double)(frame - s.firstFrame);
                const double R = (double)std::max<U64>(s.rigged, 1);
                const double G = (double)std::max<U64>(s.rigid, 1);
                LL_INFOS("AttLoop") << llformat("per frame over %.0f frames: avatars with the loop %.1f (rigged update skipped this frame %.1f), attachments %.1f (rigged %.1f, rigid %.1f, no bridge %.1f), rigged updates skipped %.1f, "
                    "override_bbox applied %.1f / unchanged %.1f | ms per frame: loop %.3f (stable check %.3f), rigged attachments %.3f (%.2f us each), rigid %.3f (%.2f us each); pieces: transformExtents %.3f, override_bbox %.3f, drawable move %.3f, bridge move %.3f, updateText %.3f",
                    F, (double)s.avatars / F, (double)s.skipFrames / F, (double)s.attachments / F, (double)s.rigged / F, (double)s.rigid / F, (double)s.noBridge / F, (double)s.skipped / F, (double)s.ovApplied / F, (double)s.ovSkipped / F,
                    (double)s.nsLoop / 1e6 / F, (double)s.nsPre / 1e6 / F, (double)s.nsRigged / 1e6 / F, (double)s.nsRigged / 1e3 / R, (double)s.nsRigid / 1e6 / F, (double)s.nsRigid / 1e3 / G,
                    (double)s.nsExtents / 1e6 / F, (double)s.nsOverride / 1e6 / F, (double)s.nsMove / 1e6 / F, (double)s.nsBridge / 1e6 / F, (double)s.nsText / 1e6 / F) << LL_ENDL;
                LL_INFOS("AttLoop") << llformat("attachments with every face fully transparent (alpha 0, no glow, no glTF), per frame: rigid %.1f, rigged %.1f (%.1f faces in them), attachments with no face at all %.1f", (double)s.clearRigid / F, (double)s.clearRigged / F, (double)s.clearFaces / F, (double)s.noFaces / F) << LL_ENDL;
                {
                    static const char* const names[5] = { "<8 m", "8-16 m", "16-32 m", "32-64 m", ">64 m" };
                    std::string text;
                    for (int b = 0; b < 5; ++b)
                    {
                        text += llformat(" | %s: %.1f visited, %.1f run in full, %.3f ms (%.3f ms of the ones run in full)", names[b], (double)s.riggedByDist[b] / F, (double)s.riggedRanByDist[b] / F, (double)s.riggedNsByDist[b] / 1e6 / F, (double)s.riggedRanNsByDist[b] / 1e6 / F);
                    }
                    LL_INFOS("AttLoop") << "rigged attachments by the distance of their avatar, per frame:" << text << LL_ENDL;
                }
                LL_INFOS("AttLoop") << llformat("rigid attachments whose drawable and bridge move were skipped (joint within 1 mm / 0.06 degrees of the last applied), per frame: %.1f", (double)s.rigidSkipped / F) << LL_ENDL;
                LL_INFOS("AttLoop") << llformat("rigid attachments with a bridge, per frame: first seen %.1f, joint matrix unchanged since the last visit (within 0.1 mm) %.1f, moved more than 0.1 mm %.1f (more than 1 mm %.1f)",
                    (double)s.rigidFirst / F, (double)s.rigidSame / F, (double)s.rigidMovedTiny / F, (double)s.rigidMoved1mm / F) << LL_ENDL;
                gAttStats = AttStats();
            }
        }
}

// <FS> view rework A2: an avatar of the motion batch whose attachment loop can run as a pool job: not the viewer's own, its detailed update this frame, and not sitting on the ground (updateCharacterPostMotion moves
// its root then, and the loop reads the skeleton)
bool LLVOAvatar::canRunAttachmentJob()
{
    return mMotionUpdateDeferred && mDeferredDetailedUpdate && !mDeferredMatricesOnly && !isSelf() && !isControlAvatar() && !isDead() && mDrawable.notNull()
        && !(!getParent() && (isSitting() || mDeferredSitGroundConstrained));
}

void LLVOAvatar::runAttachmentJob()
{
    if (!mAttLedger) { mAttLedger = std::make_shared<AvJobLedger>(); }
    AvJobLedger& led = *static_cast<AvJobLedger*>(mAttLedger.get());
    std::vector<LLPipeline::AvOp>* saved_av = LLPipeline::sAvSink;
    std::vector<LLPipeline::SortDelta>* saved_sort = LLPipeline::sSortSink;
    led.mOps.clear();
    led.mSort.clear();
    LLPipeline::sAvSink = &led.mOps;
    LLPipeline::sSortSink = &led.mSort;
    idleUpdateAttachments(isVisible() || mNeedsAnimUpdate);
    LLPipeline::sAvSink = saved_av;
    LLPipeline::sSortSink = saved_sort;
    mAttJobDone = true;
}

void LLVOAvatar::replayAttachmentJob()
{
    mAttJobDone = false;
    AvJobLedger& led = *static_cast<AvJobLedger*>(mAttLedger.get());
    gPipeline.applyAvOps(led.mOps);
    gPipeline.applySortDeltas(led.mSort);
    led.mSort.clear();
}

void LLVOAvatar::idleUpdateMisc(bool detailed_update)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (LLVOAvatar::sJointDebug)
    {
        LL_INFOS() << getDebugName() << ": joint touches: " << LLJoint::sNumTouches << " updates: " << LLJoint::sNumUpdates << LL_ENDL;
    }

    LLJoint::sNumUpdates = 0;
    LLJoint::sNumTouches = 0;

    bool visible = isVisible() || mNeedsAnimUpdate;

    // update attachments positions
    if (detailed_update)
    {
        AvSelfScope self_scope(6, this);
        if (mAttJobDone)
        {
            replayAttachmentJob();      // <FS> A2: the loop ran as a pool job, its writes are made now
        }
        else
        {
            idleUpdateAttachments(visible);
        }
    }
    else if (LLAttCensus::sOn)
    {
        // census: an avatar whose attachment loop did not run this frame, and what it wears
        ++LLAttCensus::sAvatarsHidden;
        for (const auto& [attachment_point_id, attachment] : mAttachmentPoints)
        {
            if (attachment)
            {
                LLAttCensus::sAttachmentsOnHidden += (U64)attachment->mAttachedObjects.size();
            }
        }
    }

    mNeedsAnimUpdate = false;

    if (isImpostor() && !mNeedsImpostorUpdate)
    {
        AvSelfScope self_scope(7, this);
        LL_ALIGN_16(LLVector4a ext[2]);
        F32 distance;
        LLVector3 angle;

        getImpostorValues(ext, angle, distance);

        for (U32 i = 0; i < 3 && !mNeedsImpostorUpdate; i++)
        {
            F32 cur_angle = angle.mV[i];
            F32 old_angle = mImpostorAngle.mV[i];
            F32 angle_diff = fabsf(cur_angle-old_angle);

            if (angle_diff > F_PI/512.f*distance*mUpdatePeriod)
            {
                mNeedsImpostorUpdate = true;
                mLastImpostorUpdateReason = 2;
            }
        }

        if (detailed_update && !mNeedsImpostorUpdate)
        {   //update impostor if view angle, distance, or bounding box change
            //significantly

            F32 dist_diff = fabsf(distance-mImpostorDistance);
            if (dist_diff/mImpostorDistance > 0.1f)
            {
                mNeedsImpostorUpdate = true;
                mLastImpostorUpdateReason = 3;
            }
            else
            {
                ext[0].load3(mLastAnimExtents[0].mV);
                ext[1].load3(mLastAnimExtents[1].mV);
                // Expensive. Just call this once per frame, in updateSpatialExtents();
                //calculateSpatialExtents(ext[0], ext[1]);
                LLVector4a diff;
                diff.setSub(ext[1], mImpostorExtents[1]);
                if (diff.getLength3().getF32() > 0.05f)
                {
                    mNeedsImpostorUpdate = true;
                    mLastImpostorUpdateReason = 4;
                }
                else
                {
                    diff.setSub(ext[0], mImpostorExtents[0]);
                    if (diff.getLength3().getF32() > 0.05f)
                    {
                        mNeedsImpostorUpdate = true;
                        mLastImpostorUpdateReason = 5;
                    }
                }
            }
        }
    }

    if (mDrawable.notNull())
    {
        AvSelfScope self_scope(8, this);
        mDrawable->movePartition();

        //force a move if sitting on an active object
        if (getParent() && ((LLViewerObject*) getParent())->mDrawable->isActive())
        {
            gPipeline.markMoved(mDrawable, true);
        }
    }
}

void LLVOAvatar::idleUpdateAppearanceAnimation()
{
    // update morphing params
    if (mAppearanceAnimating)
    {
        ESex avatar_sex = getSex();
        F32 appearance_anim_time = mAppearanceMorphTimer.getElapsedTimeF32();
        if (appearance_anim_time >= APPEARANCE_MORPH_TIME)
        {
            mAppearanceAnimating = false;
            for (LLVisualParam *param = getFirstVisualParam();
                 param;
                 param = getNextVisualParam())
            {
                if (param->isTweakable())
                {
                    // <FS:Ansariel> [Legacy Bake]
                    //param->stopAnimating();
                    param->stopAnimating(false);
                }
            }
            updateVisualParams();
            // <FS:Ansariel> [Legacy Bake]
            if (isSelf())
            {
                gAgent.sendAgentSetAppearance();
            }
            // </FS:Ansariel> [Legacy Bake]
        }
        else
        {
            if (!isSelf())
            {
                F32 morph_amt = calcMorphAmount();
                // animate only top level params for non-self avatars
                // <FS:Ansariel> [Legacy Bake]
                //animateTweakableVisualParams(morph_amt);
                animateTweakableVisualParams(morph_amt, false);
            }

            // apply all params
            applyAllVisualParams(avatar_sex);

            mLastAppearanceBlendTime = appearance_anim_time;
        }
        dirtyMesh();
    }
}

F32 LLVOAvatar::calcMorphAmount() const
{
    F32 appearance_anim_time = mAppearanceMorphTimer.getElapsedTimeF32();
    F32 blend_frac = calc_bouncy_animation(appearance_anim_time / APPEARANCE_MORPH_TIME);
    F32 last_blend_frac = calc_bouncy_animation(mLastAppearanceBlendTime / APPEARANCE_MORPH_TIME);

    F32 morph_amt;
    if (last_blend_frac == 1.f)
    {
        morph_amt = 1.f;
    }
    else
    {
        morph_amt = (blend_frac - last_blend_frac) / (1.f - last_blend_frac);
    }

    return morph_amt;
}

void LLVOAvatar::idleUpdateLipSync(bool voice_enabled)
{
    // Use the Lipsync_Ooh and Lipsync_Aah morphs for lip sync
    if (voice_enabled
        && mLastRezzedStatus > 0 // no point updating lip-sync for clouds
        && LLVoiceVisualizer::getLipSyncEnabled()
        && LLVoiceClient::getInstance()->getIsSpeaking(mID))
    {
        F32 ooh_morph_amount = 0.0f;
        F32 aah_morph_amount = 0.0f;

        mVoiceVisualizer->lipSyncOohAah( ooh_morph_amount, aah_morph_amount );

        if (mOohMorph)
        {
            F32 ooh_weight = mOohMorph->getMinWeight()
                + ooh_morph_amount * (mOohMorph->getMaxWeight() - mOohMorph->getMinWeight());

            // <FS:Ansariel> [Legacy Bake]
            //mOohMorph->setWeight( ooh_weight);
            mOohMorph->setWeight( ooh_weight, false);
        }

        if (mAahMorph)
        {
            F32 aah_weight = mAahMorph->getMinWeight()
                + aah_morph_amount * (mAahMorph->getMaxWeight() - mAahMorph->getMinWeight());

            // <FS:Ansariel> [Legacy Bake]
            //mAahMorph->setWeight( aah_weight);
            mAahMorph->setWeight( aah_weight, false);
        }

        mLipSyncActive = true;
        LLCharacter::updateVisualParams();
        dirtyMesh();
    }
}

void LLVOAvatar::idleUpdateLoadingEffect()
{
    // update visibility when avatar is partially loaded
    if (!mFullyLoaded && updateIsFullyLoaded()) // Avoid repeat calculations by checking if mFullyLoaded is true first.
    {
        if (isFullyLoaded())
        {
            if (mFirstFullyVisible)
            {
                mFirstFullyVisible = false;
                mLastCloudAttachmentCount = (S32)mSimAttachments.size();
                mFirstDecloudTime = mFirstAppearanceMessageTimer.getElapsedTimeF32();
                if (isSelf())
                {
                    LL_INFOS("Avatar") << avString() << "self isFullyLoaded, mFirstFullyVisible after " << mFirstDecloudTime << LL_ENDL;
                    LLAppearanceMgr::instance().onFirstFullyVisible();

                    // <FS:Zi> Animation Overrider
                    AOEngine::instance().onLoginComplete();

                    // <FS:LO> tapping a place that happens on landing in world to start up discord
                    FSDiscordConnect::instance().checkConnectionToDiscord(gSavedPerAccountSettings.getBOOL("FSEnableDiscordIntegration"));
                }
                else
                {
                    LL_INFOS("Avatar") << avString() << "other isFullyLoaded, mFirstFullyVisible after " << mFirstDecloudTime << LL_ENDL;
                }
            }

            deleteParticleSource();
            updateLOD();
        }
        else
        {
// <FS> Custom avatar particle cloud
//          LLPartSysData particle_parameters;
//
//          // fancy particle cloud designed by Brent
//          particle_parameters.mPartData.mMaxAge            = 4.f;
//          particle_parameters.mPartData.mStartScale.mV[VX] = 0.8f;
//          particle_parameters.mPartData.mStartScale.mV[VX] = 0.8f;
//          particle_parameters.mPartData.mStartScale.mV[VY] = 1.0f;
//          particle_parameters.mPartData.mEndScale.mV[VX]   = 0.02f;
//          particle_parameters.mPartData.mEndScale.mV[VY]   = 0.02f;
//          particle_parameters.mPartData.mStartColor        = LLColor4(1, 1, 1, 0.5f);
//          particle_parameters.mPartData.mEndColor          = LLColor4(1, 1, 1, 0.0f);
//          particle_parameters.mPartData.mStartScale.mV[VX] = 0.8f;
//          particle_parameters.mPartImageID                 = sCloudTexture->getID();
//          particle_parameters.mMaxAge                      = 0.f;
//          particle_parameters.mPattern                     = LLPartSysData::LL_PART_SRC_PATTERN_ANGLE_CONE;
//          particle_parameters.mInnerAngle                  = F_PI;
//          particle_parameters.mOuterAngle                  = 0.f;
//          particle_parameters.mBurstRate                   = 0.02f;
//          particle_parameters.mBurstRadius                 = 0.0f;
//          particle_parameters.mBurstPartCount              = 1;
//          particle_parameters.mBurstSpeedMin               = 0.1f;
//          particle_parameters.mBurstSpeedMax               = 1.f;
//          particle_parameters.mPartData.mFlags             = ( LLPartData::LL_PART_INTERP_COLOR_MASK | LLPartData::LL_PART_INTERP_SCALE_MASK |
//                                                               LLPartData::LL_PART_EMISSIVE_MASK | // LLPartData::LL_PART_FOLLOW_SRC_MASK |
//                                                               LLPartData::LL_PART_TARGET_POS_MASK );
//
//          // do not generate particles for dummy or overly-complex avatars
//          if (!mIsDummy && !isTooComplex())
//          {
//              setParticleSource(particle_parameters, getID());
//          }

            // Firestorm Clouds
            // do not generate particles for dummy or overly-complex avatars
            if (!mIsDummy && !isTooComplex() && !isTooSlow())
            {
                setParticleSource(sCloud, getID());
            }
        }
// </FS>
    }
}

void LLVOAvatar::idleUpdateWindEffect()
{
    // update wind effect
    if (LLPipeline::RenderAvatarCloth)
    {
        F32 hover_strength = 0.f;
        F32 time_delta = mRippleTimer.getElapsedTimeF32() - mRippleTimeLast;
        mRippleTimeLast = mRippleTimer.getElapsedTimeF32();
        LLVector3 velocity = getVelocity();
        F32 speed = velocity.length();
        //RN: velocity varies too much frame to frame for this to work
        mRippleAccel.clearVec();//lerp(mRippleAccel, (velocity - mLastVel) * time_delta, LLSmoothInterpolation::getInterpolant(0.02f));
        mLastVel = velocity;
        LLVector4 wind;
        wind.setVec(getRegion()->mWind.getVelocityNoisy(getPositionAgent(), 4.f) - velocity);

        if (mInAir)
        {
            hover_strength = HOVER_EFFECT_STRENGTH * llmax(0.f, HOVER_EFFECT_MAX_SPEED - speed);
        }

        if (mBelowWater)
        {
            // TODO: make cloth flow more gracefully when underwater
            hover_strength += UNDERWATER_EFFECT_STRENGTH;
        }

        wind.mV[VZ] += hover_strength;
        wind.normalize();

        wind.mV[VW] = llmin(0.025f + (speed * 0.015f) + hover_strength, 0.5f);
        F32 interp;
        if (wind.mV[VW] > mWindVec.mV[VW])
        {
            interp = LLSmoothInterpolation::getInterpolant(0.2f);
        }
        else
        {
            interp = LLSmoothInterpolation::getInterpolant(0.4f);
        }
        mWindVec = lerp(mWindVec, wind, interp);

        F32 wind_freq = hover_strength + llclamp(8.f + (speed * 0.7f) + (noise1(mRipplePhase) * 4.f), 8.f, 25.f);
        mWindFreq = lerp(mWindFreq, wind_freq, interp);

        if (mBelowWater)
        {
            mWindFreq *= UNDERWATER_FREQUENCY_DAMP;
        }

        mRipplePhase += (time_delta * mWindFreq);
        if (mRipplePhase > F_TWO_PI)
        {
            mRipplePhase = fmodf(mRipplePhase, F_TWO_PI);
        }
    }
}

void LLVOAvatar::idleUpdateNameTag(const LLVector3& root_pos_last)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    // update chat bubble
    //--------------------------------------------------------------------
    // draw text label over character's head
    //--------------------------------------------------------------------
    if (mChatTimer.getElapsedTimeF32() > BUBBLE_CHAT_TIME)
    {
        mChats.clear();
    }

    const F32 time_visible = mTimeVisible.getElapsedTimeF32();
    static LLCachedControl<F32> NAME_SHOW_TIME(gSavedSettings, "RenderNameShowTime"); // seconds
    static LLCachedControl<F32> FADE_DURATION(gSavedSettings, "RenderNameFadeDuration"); // seconds
    static LLCachedControl<bool> use_chat_bubbles(gSavedSettings, "UseChatBubbles");
    static LLCachedControl<bool> use_typing_bubbles(gSavedSettings, "UseTypingBubbles");

// [RLVa:KB] - Checked: RLVa-2.0.1
    bool fRlvShowAvTag = true, fRlvShowAvName = true;
    if (RlvActions::isRlvEnabled())
    {
        fRlvShowAvTag = RlvActions::canShowNameTag(this);
        fRlvShowAvName = (fRlvShowAvTag) && (RlvActions::canShowName(RlvActions::SNC_DEFAULT, getID()));
    }
// [/RLVa:KB]
    bool visible_chat = use_chat_bubbles && (mChats.size() || mTyping);
    bool visible_typing = use_typing_bubbles && mTyping;
    bool render_name =  visible_chat ||
                visible_typing ||
// [RLVa:KB] - Checked: RLVa-2.0.1
                        ((fRlvShowAvTag) &&
// [/RLVa:KB]
                        ((sRenderName == RENDER_NAME_ALWAYS) ||
                         (sRenderName == RENDER_NAME_FADE && time_visible < NAME_SHOW_TIME)));
    // If it's your own avatar, don't draw in mouselook, and don't
    // draw if we're specifically hiding our own name.
    if (isSelf())
    {
        static LLCachedControl<bool> render_name_show_self(gSavedSettings, "RenderNameShowSelf");
        static LLCachedControl<S32> name_tag_mode(gSavedSettings, "AvatarNameTagMode");
        render_name = render_name
            && !gAgentCamera.cameraMouselook()
            && (visible_chat || (render_name_show_self && name_tag_mode));
    }

    if ( !render_name )
    {
        if (mNameText)
        {
            // ...clean up old name tag
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        return;
    }

    bool new_name = false;
    if (visible_chat != mVisibleChat)
    {
        mVisibleChat = visible_chat;
        new_name = true;
    }
        if (visible_typing != mVisibleTyping)
        {
            mVisibleTyping = visible_typing;
            new_name = true;
        }

// [RLVa:KB] - Checked: RLVa-0.2.0
    if (!fRlvShowAvName)
    {
        if (mRenderGroupTitles)
        {
            mRenderGroupTitles = false;
            new_name = true;
        }
    }
    else if (sRenderGroupTitles != mRenderGroupTitles)
// [/RLVa]
//  if (sRenderGroupTitles != mRenderGroupTitles)
    {
        mRenderGroupTitles = sRenderGroupTitles;
        new_name = true;
    }

    // First Calculate Alpha
    // If alpha > 0, create mNameText if necessary, otherwise delete it
    F32 alpha = 0.f;
    if (mAppAngle > 5.f)
    {
        const F32 START_FADE_TIME = NAME_SHOW_TIME - FADE_DURATION;
        if (!visible_chat && !visible_typing && sRenderName == RENDER_NAME_FADE && time_visible > START_FADE_TIME)
        {
            alpha = 1.f - (time_visible - START_FADE_TIME) / FADE_DURATION;
        }
        else
        {
            // ...not fading, full alpha
            alpha = 1.f;
        }
    }
    else if (mAppAngle > 2.f)
    {
        // far away is faded out also
        alpha = (mAppAngle-2.f)/3.f;
    }

    if (alpha <= 0.f)
    {
        if (mNameText)
        {
            mNameText->markDead();
            mNameText = NULL;
            sNumVisibleChatBubbles--;
        }
        return;
    }

    if (!mNameText)
    {
        mNameText = static_cast<LLHUDNameTag*>( LLHUDObject::addHUDObject(
            LLHUDObject::LL_HUD_NAME_TAG) );
        //mNameText->setMass(10.f);
        mNameText->setSourceObject(this);
        mNameText->setVertAlignment(LLHUDNameTag::ALIGN_VERT_TOP);
        mNameText->setVisibleOffScreen(true);
        mNameText->setMaxLines(11);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS, 5.f);
        sNumVisibleChatBubbles++;
        new_name = true;
    }

    mNameText->setPositionAgent(root_pos_last);

    idleUpdateNameTagText(new_name);
    // Wolfspirit: Following thing is already handled in LLHUDNameTag::lineSegmentIntersect
    // Fixing bubblechat alpha flashing with commenting this out.
    // idleUpdateNameTagAlpha(new_name, alpha);
}

// <FS> PLAN-avatars.md stage B3d, name tag: when the tag's INPUTS are looked at (LL_VK_NAMETAGPOLL: 1 default, 0 every frame as before, 2 every frame and counts what the throttle would have delayed). The inputs (away /
// do not disturb / appearance animations, mute, friend, missing parts, colour, distance text, complexity, title and names) were gathered and compared with the last build of the tag every frame for every avatar whose tag
// can be seen (about 1.7 us each); the viewer already marks the tag for rebuilding where it knows (clearNameTag: settings, display names, RLV, contact sets) and typing / chat force it here, the rest is looked at
// at most 0.1 s apart (a label that says "away" appears a tenth of a second late; staggered by avatar so that they do not all look in the same frame).
static int fs_nametagpoll_mode()
{
    static const int mode = getenv("LL_VK_NAMETAGPOLL") ? atoi(getenv("LL_VK_NAMETAGPOLL")) : 1;
    return mode;
}

namespace
{
std::atomic<U32> sNameTagGen{ 1 };           // bumped by the mute list and the friend list
struct NameTagMuteObserver : public LLMuteListObserver
{
    void onChange() override { ++sNameTagGen; }
};
struct NameTagFriendObserver : public LLFriendObserver
{
    void changed(U32) override { ++sNameTagGen; }
};
}

void LLVOAvatar::bumpNameTagGen()
{
    ++sNameTagGen;
}

void LLVOAvatar::idleUpdateNameTagText(bool new_name)
{
    bool poll = true;
    const int poll_mode = fs_nametagpoll_mode();
    const F64 poll_now = LLFrameTimer::getElapsedSeconds();
    bool would_skip = false;
    if (poll_mode != 0)
    {
        static bool observers_added = false;
        if (!observers_added)
        {
            observers_added = true;
            static NameTagMuteObserver mute_observer;
            static NameTagFriendObserver friend_observer;
            LLMuteList::getInstance()->addObserver(&mute_observer);
            LLAvatarTracker::instance().addObserver(&friend_observer);
        }
        // The tag's inputs are looked at when an event said they may have changed (the hooks of invalidateNameTagInputs, the mute and friend lists, clearNameTag), and otherwise on a slow sweep that
        // catches what has no event (the colour from map marks and client tags, the missing parts, anything this list does not know): 2 s. Continuous inputs, the distance text and colour when their settings
        // are on, and the viewer's own tag (the auto-response text comes from account settings), are looked at at most 0.1 s apart.
        static LLCachedControl<bool> distance_color(gSavedSettings, "FSTagShowDistanceColors");
        static LLCachedControl<bool> distance_text(gSavedSettings, "FSTagShowDistance");
        const F64 period = (isSelf() || distance_color || distance_text) ? 0.1 : 2.0;
        if (mNameTagLastPoll < 0.0)
        {
            mNameTagLastPoll = poll_now - 0.01 * (F64)(getID().mData[0] % 10);       // (first look: spread the avatars over the interval)
        }
        const U32 gen = sNameTagGen.load(std::memory_order_relaxed);
        would_skip = mNameIsSet && !new_name && !mNameTagForce && mNameTagGen == gen && (poll_now - mNameTagLastPoll) < period;
        poll = poll_mode == 2 || !would_skip;
        if (poll)
        {
            mNameTagGen = gen;
        }
    }
    if (poll)
    {
        mNameTagLastPoll = poll_now;
        mNameTagForce = false;
    }
    static std::atomic<U64> polled{ 0 }, skipped{ 0 }, found_by_sweep{ 0 };
    ++(poll ? polled : skipped);
    if (poll_mode != 0 && ((polled + skipped) % 400000) == 0)
    {
        LL_INFOS("NameTagPoll") << "name tag inputs looked at " << polled.load() << " times, left for later " << skipped.load() << "; rebuilds found only by the sweep (mode 2) " << found_by_sweep.load() << LL_ENDL;
    }
    if (poll)
    {
    LLNameValue *title = getNVPair("Title");
    LLNameValue* firstname = getNVPair("FirstName");
    LLNameValue* lastname = getNVPair("LastName");

    // Avatars must have a first and last name
    if (!firstname || !lastname) return;

    // <FS:Ansariel> OpenSim chat distance compatibility
    static const F32 chat_range_whisper_squared = (F32)(LFSimFeatureHandler::getInstance()->whisperRange() * LFSimFeatureHandler::getInstance()->whisperRange());
    static const F32 chat_range_say_squared = (F32)(LFSimFeatureHandler::getInstance()->sayRange() * LFSimFeatureHandler::getInstance()->sayRange());
    static const F32 chat_range_shout_squared = (F32)(LFSimFeatureHandler::getInstance()->shoutRange() * LFSimFeatureHandler::getInstance()->shoutRange());
    // </FS:Ansariel>

// [RLVa:KB] - Checked: RLVa-2.0.1
    bool fRlvShowAvName = RlvActions::canShowName(RlvActions::SNC_DEFAULT, getID());
// [/RLVa:KB]
    // <FS:Ansariel> Show auto-response in nametag
    static LLCachedControl<bool> fsAutorespondMode(gSavedPerAccountSettings, "FSAutorespondMode");
    static LLCachedControl<bool> fsAutorespondNonFriendsMode(gSavedPerAccountSettings, "FSAutorespondNonFriendsMode");
    static LLCachedControl<bool> fsShowAutorespondInNametag(gSavedSettings, "FSShowAutorespondInNametag");
    bool is_autoresponse = isSelf() && fsShowAutorespondInNametag && (fsAutorespondMode || fsAutorespondNonFriendsMode);
    // </FS:Ansariel>
    // <FS:Ansariel> FIRE-3475: Show typing in nametag
    static LLCachedControl<bool> fsShowTypingStateInNameTag(gSavedSettings, "FSShowTypingStateInNameTag");
    bool is_typing = !isSelf() && mTyping && fsShowTypingStateInNameTag;
    // </FS:Ansariel>
    bool is_away = mSignaledAnimations.find(ANIM_AGENT_AWAY)  != mSignaledAnimations.end();
    bool is_do_not_disturb = mSignaledAnimations.find(ANIM_AGENT_DO_NOT_DISTURB) != mSignaledAnimations.end();
    bool is_appearance = mSignaledAnimations.find(ANIM_AGENT_CUSTOMIZE) != mSignaledAnimations.end();
    bool is_muted;
    if (isSelf())
    {
        is_muted = false;
    }
    else
    {
        is_muted = isInMuteList();
    }
//  bool is_friend = isBuddy();
// [RLVa:KB] - Checked: RLVa-1.2.2
    bool is_friend = fRlvShowAvName && isBuddy();
// [/RLVa:KB]
    bool is_cloud = getHasMissingParts();

    if (is_appearance != mNameAppearance)
    {
        if (is_appearance)
        {
            debugAvatarRezTime("AvatarRezEnteredAppearanceNotification","entered appearance mode");
        }
        else
        {
            debugAvatarRezTime("AvatarRezLeftAppearanceNotification","left appearance mode");
        }
    }
    // <FS:CR> Colorize name tags
    //LLColor4 name_tag_color = getNameTagColor(is_friend);
    LLColor4 name_tag_color = getNameTagColor();
    // </FS:CR>
    LLColor4 distance_color = name_tag_color;
    std::string distance_string;

    // Wolfspirit: If we don't need to display a friend,
    // if we aren't self, if we use colored Clienttags and if we have a color
    // then use that color as name_tag_color
    static LLUICachedControl<bool> show_friends("NameTagShowFriends");
    static LLUICachedControl<U32> color_client_tags("FSColorClienttags");
    bool special_color_override = (show_friends && (is_friend || LGGContactSets::getInstance()->hasFriendColorThatShouldShow(getID(), ContactSetType::TAG))) ||
                                    LLNetMap::hasAvatarMarkColor(getID());
    if (mClientTagData.has("color")
        && !special_color_override
        && color_client_tags && !this->isSelf())
    {
        name_tag_color = mClientTagData["color"];
    }

    // <FS:Ansariel> Color name tags based on distance
    static LLCachedControl<bool> show_distance_color_tag(gSavedSettings, "FSTagShowDistanceColors");
    static LLCachedControl<bool> show_distance_in_tag(gSavedSettings, "FSTagShowDistance");
    // <FS:CR> FIRE-6664: Add whisper range to color tags
    static LLUIColor tag_whisper_color = LLUIColorTable::instance().getColor("NameTagWhisperDistanceColor", LLColor4::green);
    // </FS:CR> FIRE-6664: Add whisper range to color tags
    static LLUIColor tag_chat_color = LLUIColorTable::instance().getColor("NameTagChatDistanceColor", LLColor4::green);
    static LLUIColor tag_shout_color = LLUIColorTable::instance().getColor("NameTagShoutDistanceColor", LLColor4::yellow);
    static LLUIColor tag_beyond_shout_color = LLUIColorTable::instance().getColor("NameTagBeyondShoutDistanceColor", LLColor4::red);

    if (!isSelf() && (show_distance_color_tag || show_distance_in_tag))
    {
        F64 distance_squared = dist_vec_squared(getPositionGlobal(), gAgent.getPositionGlobal());
        // <FS:CR> FIRE-6664: Add whisper range color tag
        if (distance_squared <= chat_range_whisper_squared)
        {
            distance_color = tag_whisper_color;
        }
        else if (distance_squared <= chat_range_say_squared)
        // </FS:CR> FIRE-6664: Add whisper range color tag
        {
            distance_color = tag_chat_color;
        }
        else if (distance_squared <= chat_range_shout_squared)
        {
            distance_color = tag_shout_color;
        }
        else
        {
            distance_color = tag_beyond_shout_color;
        }

        if (show_distance_in_tag)
        {
            distance_string = llformat("%.02f m", sqrt(distance_squared));
        }

        // Override nametag color only if friend color is disabled
        // or avatar is not a friend nor has a contact set color
        if (show_distance_color_tag && !special_color_override)
        {
            name_tag_color = distance_color;
        }
    }
    // </FS:Ansariel>

    // <FS:Ansariel> Show ARW in nametag options (for Jelly Dolls)
    static LLCachedControl<bool> show_arw_tag(gSavedSettings, "FSTagShowARW");
    static LLCachedControl<bool> show_too_complex_only_arw_tag(gSavedSettings, "FSTagShowTooComplexOnlyARW");
    static LLCachedControl<bool> show_own_arw_tag(gSavedSettings, "FSTagShowOwnARW");
    U32 complexity(0);
    LLColor4 complexity_color(LLColor4::grey1); // default if we're not limiting the complexity

    if (show_arw_tag &&
       ((isSelf() && show_own_arw_tag) ||
       (!isSelf() && (!show_too_complex_only_arw_tag || isTooComplex()))))
    {
        complexity = mVisualComplexity;

        // Show complexity color if we're limiting and not showing our own ARW...
        static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0);
        if (max_render_cost != 0 && !isSelf())
        {
            // This calculation is copied from idleUpdateRenderComplexity()
            F32 green_level = 1.f - llclamp(((F32)complexity - (F32)max_render_cost) / (F32)max_render_cost, 0.f, 1.f);
            F32 red_level = llmin((F32)complexity / (F32)max_render_cost, 1.f);
            complexity_color.set(red_level, green_level, 0.f, 1.f);
        }
    }
    // </FS:Ansariel>

    // Rebuild name tag if state change detected
    if (!mNameIsSet
        || new_name
        // <FS:Ansariel> FIRE-13414: Avatar name isn't updated when the simulator sends a new name
        || (!LLGridManager::instance().isInSecondLife() && (firstname->getString() != mNameFirstname || lastname->getString() != mNameLastname))
        // </FS:Ansariel>
        || (!title && !mTitle.empty())
        || (title && mTitle != title->getString())
        || is_away != mNameAway
        || is_do_not_disturb != mNameDoNotDisturb
        || is_autoresponse != mNameAutoResponse
        || is_muted != mNameMute
        || is_appearance != mNameAppearance
        || is_friend != mNameFriend
        || is_cloud != mNameCloud
        || name_tag_color != mNameColor
        || is_typing != mNameIsTyping
        || distance_string != mDistanceString
        // <FS:Ansariel> Show Arc in nametag (for Jelly Dolls)
        || complexity != mNameArc
        || complexity_color != mNameArcColor)
    {
        if (poll_mode == 2 && would_skip)
        {
            // (mode 2) a change no event flagged: which input moved (an input can be counted with others; "other" is a rebuild none of the listed inputs explains)
            ++found_by_sweep;
            static std::atomic<U64> by_input[12];
            const bool title_changed = (!title && !mTitle.empty()) || (title && mTitle != title->getString());
            const bool names_changed = !LLGridManager::instance().isInSecondLife() && (firstname->getString() != mNameFirstname || lastname->getString() != mNameLastname);
            bool any = false;
            auto count = [&](int k, bool moved) { if (moved) { ++by_input[k]; any = true; } };
            count(0, is_away != mNameAway || is_do_not_disturb != mNameDoNotDisturb || is_autoresponse != mNameAutoResponse);
            count(1, is_appearance != mNameAppearance);
            count(2, is_muted != mNameMute);
            count(3, is_friend != mNameFriend);
            count(4, is_cloud != mNameCloud);
            count(5, name_tag_color != mNameColor);
            count(6, is_typing != mNameIsTyping);
            count(7, distance_string != mDistanceString);
            count(8, complexity != mNameArc || complexity_color != mNameArcColor);
            count(9, title_changed);
            count(10, names_changed);
            if (!any)
            {
                ++by_input[11];
            }
            if (found_by_sweep % 200 == 1)
            {
                LL_INFOS("NameTagPoll") << "rebuilds no event flagged, so far " << found_by_sweep.load() << " by input: away/dnd/auto " << by_input[0].load() << ", appearance " << by_input[1].load() << ", mute " << by_input[2].load()
                    << ", friend " << by_input[3].load() << ", missing parts " << by_input[4].load() << ", colour " << by_input[5].load() << ", typing " << by_input[6].load() << ", distance text " << by_input[7].load()
                    << ", complexity " << by_input[8].load() << ", title " << by_input[9].load() << ", names " << by_input[10].load() << ", none of these " << by_input[11].load() << LL_ENDL;
            }
        }

        //WS: If we got a uuid and if we know if it's id_based or not, ask FSDATA for the other tagdata, before we display it.
        if (mClientTagData.has("uuid") && mClientTagData.has("id_based"))
        {
            LLColor4 color;
            if (mClientTagData.has("tex_color"))
            {
                color.setValue(mClientTagData["tex_color"]);
            }
            else
            {
                color = LLColor4::black;
            }
            mClientTagData = FSData::getInstance()->resolveClientTag(LLUUID(mClientTagData["uuid"].asString()),
                                                                     mClientTagData["id_based"].asBoolean(),
                                                                     color);
        }

        clearNameTag();

        // <FS:Ansariel> Show auto-response in nametag
        //if (is_away || is_muted || is_do_not_disturb || is_appearance)
        if (is_away || is_muted || is_do_not_disturb || is_autoresponse || is_appearance || is_typing)
        // </FS:Ansariel>
        {
            std::string line;
            if (is_away)
            {
                line += LLTrans::getString("AvatarAway");
                line += ", ";
            }
            if (is_do_not_disturb)
            {
                line += LLTrans::getString("AvatarDoNotDisturb");
                line += ", ";
            }
            // <FS:Ansariel> Show auto-response in nametag
            if (is_autoresponse)
            {
                line += LLTrans::getString("AvatarAutoResponse");
                line += ", ";
            }
            // </FS:Ansariel>
            if (is_muted)
            {
                line += LLTrans::getString("AvatarMuted");
                line += ", ";
            }
            if (is_appearance)
            {
                line += LLTrans::getString("AvatarEditingAppearance");
                line += ", ";
            }
            if (is_cloud && !is_muted)
            {
                line += LLTrans::getString("LoadingData");
                line += ", ";
            }
            // <FS:Ansariel> FIRE-3475: Show typing in nametag
            if (is_typing)
            {
                line += LLTrans::getString("AvatarTyping");
                line += ", ";
            }
            // </FS:Ansariel>
            // trim last ", "
            line.resize( line.length() - 2 );
            addNameTagLine(line, name_tag_color, LLFontGL::NORMAL,
                LLFontGL::getFontSansSerifSmall());
        }
        bool render_title = (sRenderGroupTitles == RENDER_GROUP_TITLE_ALWAYS) ||
                            (isSelf() && (sRenderGroupTitles == RENDER_GROUP_TITLE_SELF));

//      if (render_title && title && title->getString() && title->getString()[0] != '\0')
// [RLVa:KB] - Checked: RLVa-1.2.2
        if (render_title && fRlvShowAvName && title && title->getString() && title->getString()[0] != '\0')
// [/RLVa:KB]
        {
            std::string title_str = title->getString();
            LLStringFn::replace_ascii_controlchars(title_str,LL_UNKNOWN_CHAR);
            addNameTagLine(title_str, name_tag_color, LLFontGL::NORMAL,
                LLFontGL::getFontSansSerifSmall(), true);
        }

        static LLUICachedControl<bool> show_display_names("NameTagShowDisplayNames", true);
        static LLUICachedControl<bool> show_usernames("NameTagShowUsernames", true);
        static LLUICachedControl<bool> show_rez_status("NameTagDebugAVRezState", false);
        static LLUICachedControl<bool> colorize_username("FSColorUsername");    // <FS:CR> FIRE-1061
        static LLUICachedControl<bool> show_legacynames("FSNameTagShowLegacyUsernames");

        if (LLAvatarName::useDisplayNames())
        {
            LLAvatarName av_name;
            if (!LLAvatarNameCache::get(getID(), &av_name))
            {
                // Force a rebuild at next idle
                // Note: do not connect a callback on idle().
                clearNameTag();
            }

// [RLVa:KB] - Checked: RLVa-1.2.2
            if ( (fRlvShowAvName) || (isSelf()) )
            {
// [/RLVa:KB]
                // Might be blank if name not available yet, that's OK
                if (show_display_names)
                {

                    if (mClientTagData.has("name") && !mClientTagData["name"].asString().empty())
                    {
                        addNameTagLine((av_name.isDisplayNameDefault() ? av_name.getUserNameForDisplay() : av_name.getDisplayName()) +" (" + mClientTagData["name"].asString() + ")",name_tag_color,LLFontGL::NORMAL, LLFontGL::getFontSansSerif(), true, (!av_name.getDisplayName().empty()) );
                    }
                    else
                    {
                        addNameTagLine((av_name.isDisplayNameDefault() ? av_name.getUserNameForDisplay() : av_name.getDisplayName()), name_tag_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerif(), true, true);
                    }
                }
                // Suppress SLID display if display name matches exactly (ugh)
                if (show_usernames && !av_name.isDisplayNameDefault())
                {
                    // *HACK: Desaturate the color
                    // <FS:CR> FIRE-1061
                    LLColor4 username_color;
                    if (colorize_username)
                    {
                        username_color = LLUIColorTable::instance().getColor("NameTagUsername", LLColor4::white);
                    }
                    else
                    {
                        username_color = name_tag_color * 0.83f;
                    }
                    // </FS:CR>

                    // <FS:CR> Show user name as legacy name if selected
                    std::string username( show_legacynames ? av_name.getUserNameForDisplay() : av_name.getAccountName() );

                    addNameTagLine(username, username_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall(), true);
                }
// [RLVa:KB] - Checked: RLVa-1.2.2
            }
            else
            {
                addNameTagLine(RlvStrings::getAnonym(av_name), name_tag_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerif(), true, (!av_name.getDisplayName().empty()) );
            }
// [/RLVa:KB]
        }
        else  // DISPLAY NAMES OFF
        {
            const LLFontGL* font = LLFontGL::getFontSansSerif();
            std::string full_name = LLCacheName::buildFullName( firstname->getString(), lastname->getString() );
// [RLVa:KB] - Checked: RLVa-1.2.2
            if ( (!fRlvShowAvName) && (!isSelf()) )
            {
                full_name = RlvStrings::getAnonym(full_name);
                addNameTagLine(full_name, name_tag_color, LLFontGL::NORMAL, font, true, true);
            }
// [/RLVa:KB]
            else // Only check for client tags when not RLV anon -AO
            {
                if (mClientTagData.has("name") && !mClientTagData["name"].asString().empty())
                {
                    addNameTagLine(full_name + " (" + mClientTagData["name"].asString() + ")", name_tag_color, LLFontGL::NORMAL, font, true, true);
                }
                else
                {
                    addNameTagLine(full_name, name_tag_color, LLFontGL::NORMAL, font, true, true);
                }
            }
        }

        // <FS:Ansariel> Show distance in tag
        if (show_distance_in_tag)
        {
            addNameTagLine(distance_string, distance_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall());
        }
        // <FS:Ansariel> Show distance in tag

        // <FS:Ansariel> Show ARW in nametag options (for Jelly Dolls)
        static const std::string complexity_label = LLTrans::getString("Nametag_Complexity_Label");
        static const std::string texture_area_label = LLTrans::getString("Nametag_Texture_Area_Label");
        if (show_arw_tag &&
           ((isSelf() && show_own_arw_tag) ||
           (!isSelf() && (!show_too_complex_only_arw_tag || isTooComplex()))))
        {
            std::string complexity_string;
            LLLocale locale("");

            // always show complexity, even if the reason for a jelly baby is the texture area
            // this is technically not 100% correct but the decision logic with all of the
            // exceptions would be way too complex to justify the result - Zi
            LLResMgr::getInstance()->getIntegerString(complexity_string, complexity);
            LLStringUtil::format_map_t label_args;
            label_args["COMPLEXITY"] = complexity_string;

            addNameTagLine(FSCommon::format_string(complexity_label, label_args), complexity_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall());

            // only show texture area if this is the reason for jelly baby rendering
            static LLCachedControl<F32> max_attachment_area(gSavedSettings, "RenderAutoMuteSurfaceAreaLimit", 1000.0f);
            if (max_attachment_area > 0.f && mAttachmentSurfaceArea > max_attachment_area)
            {
                LLResMgr::getInstance()->getIntegerString(complexity_string, (S32)mAttachmentSurfaceArea);
                label_args["TEXTURE_AREA"] = complexity_string;

                addNameTagLine(FSCommon::format_string(texture_area_label, label_args), LLColor4::red, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall());
            }
        }
        // </FS:Ansariel>

        if (show_rez_status)
        {
            std::string av_string = LLVOAvatar::rezStatusToString(mLastRezzedStatus);
            addNameTagLine(av_string, name_tag_color, LLFontGL::NORMAL, LLFontGL::getFontSansSerifSmall(), true);
        }

        mNameAway = is_away;
        mNameDoNotDisturb = is_do_not_disturb;
        mNameAutoResponse = is_autoresponse; // <FS:Ansariel> Show auto-response in nametag
        mNameMute = is_muted;
        mNameAppearance = is_appearance;
        mNameFriend = is_friend;
        mNameCloud = is_cloud;
        mNameColor=name_tag_color;
        mDistanceString = distance_string;
        mTitle = title ? title->getString() : "";
        mNameIsTyping = is_typing;
        // <FS:Ansariel> FIRE-13414: Avatar name isn't updated when the simulator sends a new name
        mNameFirstname = firstname->getString();
        mNameLastname = lastname->getString();
        // </FS:Ansariel>
        // <FS:Ansariel> Show Arc in nametag (for Jelly Dolls)
        mNameArc = complexity;
        mNameArcColor = complexity_color;
        // </FS:Ansariel>
        LLStringFn::replace_ascii_controlchars(mTitle,LL_UNKNOWN_CHAR);
        new_name = true;
    }
    }       // (if (poll))

    if (mVisibleChat || mVisibleTyping)
    {
        mNameText->setFont(LLFontGL::getFontSansSerif());
                mNameText->setTextAlignment(LLHUDNameTag::ALIGN_TEXT_LEFT);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS * 2.f, 5.f);

        std::deque<LLChat>::iterator chat_iter = mChats.begin();
        mNameText->clearString();

        static const LLUIColor user_chat_color = LLUIColorTable::instance().getColor("UserChatColor");
        static const LLUIColor agent_chat_color = LLUIColorTable::instance().getColor("AgentChatColor");
        // <FS:CR> Colorize tags
        //const LLColor4& new_chat = isSelf() ? user_chat_color : agent_chat_color;
        LLColor4 new_chat = isSelf() ? user_chat_color : agent_chat_color;

        // <FS:CR> Colorize tags
        new_chat = LGGContactSets::getInstance()->colorize(getID(), new_chat, ContactSetType::CHAT);

        //color based on contact sets prefs
        LGGContactSets::getInstance()->hasFriendColorThatShouldShow(getID(), ContactSetType::CHAT, new_chat);
        // </FS:CR>

        if (mVisibleChat)
        {
        LLColor4 normal_chat = lerp(new_chat, LLColor4(0.8f, 0.8f, 0.8f, 1.f), 0.7f);
        LLColor4 old_chat = lerp(normal_chat, LLColor4(0.6f, 0.6f, 0.6f, 1.f), 0.7f);
        if (mTyping && mChats.size() >= MAX_BUBBLE_CHAT_UTTERANCES)
        {
            ++chat_iter;
        }

        for(; chat_iter != mChats.end(); ++chat_iter)
        {
            F32 chat_fade_amt = llclamp((F32)((LLFrameTimer::getElapsedSeconds() - chat_iter->mTime) / CHAT_FADE_TIME), 0.f, 4.f);
            LLFontGL::StyleFlags style;
            switch(chat_iter->mChatType)
            {
            case CHAT_TYPE_WHISPER:
                style = LLFontGL::ITALIC;
                break;
            case CHAT_TYPE_SHOUT:
                style = LLFontGL::BOLD;
                break;
            default:
                style = LLFontGL::NORMAL;
                break;
            }
            if (chat_fade_amt < 1.f)
            {
                F32 u = clamp_rescale(chat_fade_amt, 0.9f, 1.f, 0.f, 1.f);
                mNameText->addLine(chat_iter->mText, lerp(new_chat, normal_chat, u), style);
            }
            else if (chat_fade_amt < 2.f)
            {
                F32 u = clamp_rescale(chat_fade_amt, 1.9f, 2.f, 0.f, 1.f);
                mNameText->addLine(chat_iter->mText, lerp(normal_chat, old_chat, u), style);
            }
            else if (chat_fade_amt < 3.f)
            {
                // *NOTE: only remove lines down to minimum number
                mNameText->addLine(chat_iter->mText, old_chat, style);
            }
        }
        }
        mNameText->setVisibleOffScreen(true);

        if (mVisibleTyping && mTyping)
        {
            S32 dot_count = (llfloor(mTypingTimer.getElapsedTimeF32() * 3.f) + 2) % 3 + 1;
            switch(dot_count)
            {
            case 1:
                mNameText->addLine(".", new_chat);
                break;
            case 2:
                mNameText->addLine("..", new_chat);
                break;
            case 3:
                mNameText->addLine("...", new_chat);
                break;
            }

        }
    }
    else
    {
        // ...not using chat bubbles, just names
        mNameText->setTextAlignment(LLHUDNameTag::ALIGN_TEXT_CENTER);
        mNameText->setFadeDistance(CHAT_NORMAL_RADIUS, 5.f);
        mNameText->setVisibleOffScreen(false);
    }
}

// <FS:Ansariel> Fix nametag not properly updating when display name arrives
//void LLVOAvatar::addNameTagLine(const std::string& line, const LLColor4& color, S32 style, const LLFontGL* font, const bool use_ellipses)
void LLVOAvatar::addNameTagLine(const std::string& line, const LLColor4& color, S32 style, const LLFontGL* font, const bool use_ellipses, bool is_name /* = false */)
// </FS:Ansariel>
{
    // extra width (NAMETAG_MAX_WIDTH) is for names only, not for chat
    llassert(mNameText);
    if (mVisibleChat || mVisibleTyping)
    {
        mNameText->addLabel(line, LLHUDNameTag::NAMETAG_MAX_WIDTH);
    }
    else
    {
        mNameText->addLine(line, color, (LLFontGL::StyleFlags)style, font, use_ellipses, LLHUDNameTag::NAMETAG_MAX_WIDTH);
    }
    // <FS:Ansariel> Fix nametag not properly updating when display name arrives
    //mNameIsSet |= !line.empty();
    if (is_name)
    {
        mNameIsSet |= !line.empty();
    }
    // </FS:Ansariel>
}

void LLVOAvatar::clearNameTag()
{
    mNameIsSet = false;
    if (mNameText)
    {
        mNameText->setLabel("");
        mNameText->setString("");
    }
    mTimeVisible.reset();
}

// static
void LLVOAvatar::invalidateNameTag(const LLUUID& agent_id)
{
    if (LLViewerObject* obj = gObjectList.findObject(agent_id))
    {
        if (LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(obj))
        {
            avatar->clearNameTag();
        }
    }
}

// static
void LLVOAvatar::invalidateNameTags()
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead())
        {
            avatar->clearNameTag();
        }
    }
}

// Compute name tag position during idle update
LLVector3 LLVOAvatar::idleCalcNameTagPosition(const LLVector3 &root_pos_last)
{
    LLQuaternion root_rot = mRoot->getWorldRotation();
    LLQuaternion inv_root_rot = ~root_rot;
    LLVector3 pixel_right_vec;
    LLVector3 pixel_up_vec;
    LLViewerCamera::getInstance()->getPixelVectors(root_pos_last, pixel_up_vec, pixel_right_vec);
    LLVector3 camera_to_av = root_pos_last - LLViewerCamera::getInstance()->getOrigin();
    camera_to_av.normalize();
    LLVector3 local_camera_at = camera_to_av * inv_root_rot;
    LLVector3 local_camera_up = camera_to_av % LLViewerCamera::getInstance()->getLeftAxis();
    local_camera_up.normalize();
    local_camera_up = local_camera_up * inv_root_rot;

    // <FS:Ansariel> Optional legacy nametag position
    LLVector3 name_position;
    static LLCachedControl<bool> fsLegacyNametagPosition(gSavedSettings, "FSLegacyNametagPosition");
    if (fsLegacyNametagPosition)
    {
        local_camera_up.scaleVec((mBodySize + mAvatarOffset) * 0.5f);
        local_camera_at.scaleVec((mBodySize + mAvatarOffset) * 0.5f);

        name_position = mRoot->getWorldPosition();
        name_position[VZ] -= mPelvisToFoot;
        name_position[VZ] += ((mBodySize[VZ] - mAvatarOffset[VZ] * 0.9f) * 0.55f);
        name_position += (local_camera_up * root_rot) - (projected_vec(local_camera_at * root_rot, camera_to_av));
        name_position += pixel_up_vec * 15.f;
    }
    else
    {
    // </FS:Ansariel>
    // position is based on head position, does not require mAvatarOffset here. - Nyx
    LLVector3 avatar_ellipsoid(mBodySize.mV[VX] * 0.4f,
                                mBodySize.mV[VY] * 0.4f,
                                mBodySize.mV[VZ] * NAMETAG_VERT_OFFSET_WEIGHT);

    local_camera_up.scaleVec(avatar_ellipsoid);
    local_camera_at.scaleVec(avatar_ellipsoid);

    LLVector3 head_offset = (mHeadp->getLastWorldPosition() - mRoot->getLastWorldPosition()) * inv_root_rot;

    if (dist_vec(head_offset, mTargetRootToHeadOffset) > NAMETAG_UPDATE_THRESHOLD)
    {
        mTargetRootToHeadOffset = head_offset;
    }

    mCurRootToHeadOffset = lerp(mCurRootToHeadOffset, mTargetRootToHeadOffset, LLSmoothInterpolation::getInterpolant(0.2f));

    // <FS:Ansariel> Optional legacy nametag position
    //LLVector3 name_position = mRoot->getLastWorldPosition() + (mCurRootToHeadOffset * root_rot);
    name_position = mRoot->getLastWorldPosition() + (mCurRootToHeadOffset * root_rot);
    name_position += (local_camera_up * root_rot) - (projected_vec(local_camera_at * root_rot, camera_to_av));
    name_position += pixel_up_vec * NAMETAG_VERTICAL_SCREEN_OFFSET;
    // <FS:Ansariel> Optional legacy nametag position
    }
    // </FS:Ansariel>

    // <FS:Ansariel> Optional Z-offset correction for name tags
    static LLCachedControl<S32> fsNameTagOffset(gSavedSettings, "FSNameTagZOffsetCorrection");
    name_position[VZ] += fsNameTagOffset / 10.f;
    // </FS:Ansariel>

    // Avoid of crossing the name tag by the water surface
    if (mNameText)
    {
        F32 water_height = getRegion()->getWaterHeight();
        static const F32 WATER_HEIGHT_ABOVE_DELTA = 0.25;
        if (name_position[VZ] < water_height + WATER_HEIGHT_ABOVE_DELTA)
        {
            F32 camera_height = LLViewerCamera::getInstance()->getOrigin()[VZ];
            if (camera_height >= water_height)
            {
                F32 name_world_height = mNameText->getWorldHeight();
                static const F32 WATER_HEIGHT_BELOW_DELTA = 0.5;
                if (name_position[VZ] + name_world_height > water_height - WATER_HEIGHT_BELOW_DELTA)
                {
                    name_position[VZ] = water_height + WATER_HEIGHT_ABOVE_DELTA;
                }
            }
        }
    }

    return name_position;
}

void LLVOAvatar::idleUpdateNameTagAlpha(bool new_name, F32 alpha)
{
    llassert(mNameText);

    if (new_name
        || alpha != mNameAlpha)
    {
        mNameText->setAlpha(alpha);
        mNameAlpha = alpha;
    }
}

// <FS:CR> Colorize tags
//LLColor4 LLVOAvatar::getNameTagColor(bool is_friend)
LLColor4 LLVOAvatar::getNameTagColor()
// </FS:CR>
{
    // ...not using display names
    LLColor4 color = LLUIColorTable::getInstance()->getColor("NameTagLegacy");
    if (LLAvatarName::useDisplayNames())
    {
        // ...color based on whether username "matches" a computed display name
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(getID(), &av_name) && av_name.isDisplayNameDefault())
        {
            color = LLUIColorTable::getInstance()->getColor("NameTagMatch");
        }
        else
        {
            color = LLUIColorTable::getInstance()->getColor("NameTagMismatch");
        }
    }

    // <FS:CR> FIRE-1061 - Color friends, lindens, muted, etc
    color = LGGContactSets::getInstance()->colorize(getID(), color, ContactSetType::TAG);
    // </FS:CR>

    LGGContactSets::getInstance()->hasFriendColorThatShouldShow(getID(), ContactSetType::TAG, color);

    LLNetMap::getAvatarMarkColor(getID(), color);

    return color;
}

void LLVOAvatar::idleUpdateBelowWater()
{
    F32 avatar_height = (F32)(getPositionGlobal().mdV[VZ]);

    F32 water_height;
    water_height = getRegion()->getWaterHeight();

    // <FS:Zi> Animation Overrider
    bool wasBelowWater = mBelowWater;
    mBelowWater =  avatar_height < water_height;
    // <FS:Zi> Animation Overrider
    if (isSelf() && wasBelowWater != mBelowWater)
    {
        AOEngine::instance().checkBelowWater(mBelowWater);
    }
    // </FS:Zi> Animation Overrider
}

void LLVOAvatar::slamPosition()
{
    gAgent.setPositionAgent(getPositionAgent());
    // SL-315
    mRoot->setWorldPosition(getPositionAgent()); // teleport
    setChanged(TRANSLATED);
    if (mDrawable.notNull())
    {
        gPipeline.updateMoveNormalAsync(mDrawable);
    }
    mRoot->updateWorldMatrixChildren();
}

bool LLVOAvatar::isVisuallyMuted()
{
    // <FS> PLAN-avatars.md stage A step 4b: after the avatars' update the table holds the answer
    if (const LLAvatarTable::Row* row = LLAvatarTable::finalRow(this))
    {
        static const bool verify = getenv("LL_VK_AVTABLE_VERIFY") != nullptr;
        if (verify)
        {
            LLAvatarTable::noteCheck(2, row->mMuted == computeVisuallyMutedLive());
        }
        return row->mMuted;
    }
    return computeVisuallyMutedLive();
}

bool LLVOAvatar::computeVisuallyMutedLive()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR; // <FS:Beq/> Tracy accounting for imposter testing.
    bool muted = false;
    // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
    static LLCachedControl<U32> impostor_avatar_exclude(gSavedSettings,"FSImpostorAvatarExclude", 0);
    // </FS:minerjr> [FIRE-35735]

    // <FS:Ansariel> FIRE-11783: Always visually mute avatars that are muted
    if (!isSelf() && isInMuteList())
    {
        return true;
    }
    // </FS:Ansariel>

    // Priority order (highest priority first)
    // * own avatar is never visually muted
    // * if on the "always draw normally" list, draw them normally
    // * if on the "always visually mute" list, mute them
    // * check against the render cost and attachment limits
    if (!isSelf())
    {
        if (mVisuallyMuteSetting == AV_ALWAYS_RENDER)
        {
            muted = false;
        }
        else if (mVisuallyMuteSetting == AV_DO_NOT_RENDER)
        {
#ifdef JELLYDOLLS_SHOULD_IMPOSTOR
            muted = true;
            // Always want to see this AV as an impostor
#else
            muted = false;
#endif
            // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
            // If the avatar is set to be excluded, set the muted flag to false
            if ((mIsControlAvatar && impostor_avatar_exclude >= EImpostorAvatarExclude::CONTROL) || (mIsAnimesh && (impostor_avatar_exclude & EImpostorAvatarExclude::USER)))
            {
                muted = false;
            }
            // </FS:minerjr> [FIRE-35735]
        }
        // <FS:Ansariel> FIRE-11783: Always visually mute avatars that are muted
        //else if (isInMuteList())
        //{
        //    muted = true;
        //}
        // </FS:Ansariel>
        else if (mIsControlAvatar)
        {
            // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
            // If the avatar is set to control or both, set the the meted flag to false
            if (impostor_avatar_exclude >= EImpostorAvatarExclude::CONTROL)
            {
                muted = false;
            }
            else
            // </FS:minerjr> [FIRE-35735]
            muted = isTooSlow();
        }
        else
        {
            // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
            // If the avatar is an animesh and the FSImpostorAvatarExclude is either a USER or BOTH (Can use & as both 1 and 3 have 1 set)
            if ((impostor_avatar_exclude & EImpostorAvatarExclude::USER) && mIsAnimesh)
            {
                muted = false;
            }
            else
            // </FS:minerjr> [FIRE-35735]
            muted = isTooComplex(); // <FS:Beq/> this should not trigger based on perfstats
        }
    }

    return muted;
}

bool LLVOAvatar::isInMuteList() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR; // <FS:Beq/> Tracy accounting for imposter testing.
    bool muted = false;
    F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mCachedMuteListUpdateTime)
    {
        muted = mCachedInMuteList;
    }
    else
    {
        muted = LLMuteList::getInstance()->isMuted(getID());

        const F64 SECONDS_BETWEEN_MUTE_UPDATES = 1;
        mCachedMuteListUpdateTime = now + SECONDS_BETWEEN_MUTE_UPDATES;
        if (muted != mCachedInMuteList)
        {
            const_cast<LLVOAvatar*>(this)->invalidateNameTagInputs();       // <FS> B3d: the cached answer flipped (the mute list observer fires before this cache catches up): the tag shows "muted"
        }
        mCachedInMuteList = muted;
    }
    return muted;
}

// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
bool LLVOAvatar::isRlvSilhouette() const
{
    if (!RlvActions::hasBehaviour(RLV_BHVR_SETCAM_AVDIST))
        return false;

    // <FS> FIRE-34340-2 Your own avatar and attached animesh shouldn't be a silhouette
    if (isSelf())
        return false;

    if (const LLVOAvatar* wearer = getAttachedAvatar())
    {
        if (wearer->isSelf())
            return false;
    }
    // </FS>

    static RlvCachedBehaviourModifier<float> s_nSetCamAvDist(RLV_MODIFIER_SETCAM_AVDIST);

    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now >= mCachedRlvSilhouetteUpdateTime)
    {
        const F64 SECONDS_BETWEEN_SILHOUETTE_UPDATES = .5f;
        bool fIsRlvSilhouette = dist_vec_squared(gAgent.getPositionGlobal(), getPositionGlobal()) > s_nSetCamAvDist() * s_nSetCamAvDist();
        if (fIsRlvSilhouette != mCachedIsRlvSilhouette)
        {
            mCachedIsRlvSilhouette = fIsRlvSilhouette;
            mNeedsImpostorUpdate = true;
        }
        mCachedRlvSilhouetteUpdateTime = now + SECONDS_BETWEEN_SILHOUETTE_UPDATES;
    }
    return mCachedIsRlvSilhouette;
}
// [/RLVa:KB]

void LLVOAvatar::updateAppearanceMessageDebugText()
{
        S32 central_bake_version = -1;
        if (getRegion())
        {
            central_bake_version = getRegion()->getCentralBakeVersion();
        }
        bool all_baked_downloaded = allBakedTexturesCompletelyDownloaded();
        bool all_local_downloaded = allLocalTexturesCompletelyDownloaded();
        std::string debug_line = llformat("%s%s - mLocal: %d, mEdit: %d, mUSB: %d, CBV: %d",
                                          isSelf() ? (all_local_downloaded ? "L" : "l") : "-",
                                          all_baked_downloaded ? "B" : "b",
                                          mUseLocalAppearance, mIsEditingAppearance,
                                          // <FS:Ansariel> [Legacy Bake]
                                          //1, central_bake_version);
                                          mUseServerBakes, central_bake_version);
                                          // </FS:Ansariel> [Legacy Bake]
        std::string origin_string = bakedTextureOriginInfo();
        debug_line += " [" + origin_string + "]";
        S32 curr_cof_version = LLAppearanceMgr::instance().getCOFVersion();
        S32 last_request_cof_version = mLastUpdateRequestCOFVersion;
        S32 last_received_cof_version = mLastUpdateReceivedCOFVersion;
        if (isSelf())
        {
            debug_line += llformat(" - cof: %d req: %d rcv:%d",
                                   curr_cof_version, last_request_cof_version, last_received_cof_version);
            static LLCachedControl<bool> debug_force_failure(gSavedSettings, "DebugForceAppearanceRequestFailure");
            if (debug_force_failure)
            {
                debug_line += " FORCING ERRS";
            }
        }
        else
        {
            debug_line += llformat(" - cof rcv:%d", last_received_cof_version);
        }
        debug_line += llformat(" bsz-z: %.3f", mBodySize[2]);
        if (mAvatarOffset[2] != 0.0f)
        {
            debug_line += llformat("avofs-z: %.3f", mAvatarOffset[2]);
        }
        bool hover_enabled = getRegion() && getRegion()->avatarHoverHeightEnabled();
        debug_line += hover_enabled ? " H" : " h";
        const LLVector3& hover_offset = getHoverOffset();
        if (hover_offset[2] != 0.0)
        {
            debug_line += llformat(" hov_z: %.3f", hover_offset[2]);
            debug_line += llformat(" %s", (isSitting() ? "S" : "T"));
            debug_line += llformat("%s", (isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED) ? "G" : "-"));
        }
        if (mInAir)
        {
            debug_line += " A";

        }

        LLVector3 ankle_right_pos_agent = mFootRightp->getWorldPosition();
        LLVector3 normal;
        LLVector3 ankle_right_ground_agent = ankle_right_pos_agent;
        resolveHeightAgent(ankle_right_pos_agent, ankle_right_ground_agent, normal);
        F32 rightElev = llmax(-0.2f, ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ]);
        debug_line += llformat(" relev %.3f", rightElev);

        LLVector3 root_pos = mRoot->getPosition();
        LLVector3 pelvis_pos = mPelvisp->getPosition();
        debug_line += llformat(" rp %.3f pp %.3f", root_pos[2], pelvis_pos[2]);

        const LLVector3& scale = getScale();
        debug_line += llformat(" scale-z %.3f", scale[2]);
        S32 is_visible = (S32) isVisible();
        S32 is_m_visible = (S32) mVisible;
        debug_line += llformat(" v %d/%d", is_visible, is_m_visible);

        AvatarOverallAppearance aoa = getOverallAppearance();
        if (aoa == AOA_NORMAL)
        {
            debug_line += " N";
        }
        else if (aoa == AOA_JELLYDOLL)
        {
            debug_line += " J";
        }
        else
        {
            debug_line += " I";
        }

        if (mMeshValid)
        {
            debug_line += "m";
        }
        else
        {
            debug_line += "-";
        }
        if (isImpostor())
        {
            debug_line += " Imp" + llformat("%d[%d]:%.1f", mUpdatePeriod, mLastImpostorUpdateReason, ((F32)(gFrameTimeSeconds-mLastImpostorUpdateFrameTime)));
        }

        addDebugText(debug_line);
}

LLViewerInventoryItem* getObjectInventoryItem(LLViewerObject *vobj, LLUUID asset_id)
{
    LLViewerInventoryItem *item = NULL;

    if (vobj)
    {
        if (vobj->getInventorySerial()<=0)
        {
            vobj->requestInventory();
    }
        item = vobj->getInventoryItemByAsset(asset_id);
    }
    return item;
}

LLViewerInventoryItem* recursiveGetObjectInventoryItem(LLViewerObject *vobj, LLUUID asset_id)
{
    LLViewerInventoryItem *item = getObjectInventoryItem(vobj, asset_id);
    if (!item)
    {
        LLViewerObject::const_child_list_t& children = vobj->getChildren();
        for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
             it != children.end(); ++it)
        {
            LLViewerObject *childp = *it;
            item = getObjectInventoryItem(childp, asset_id);
            if (item)
    {
                break;
            }
        }
    }
    return item;
}

void LLVOAvatar::updateAnimationDebugText()
{
    for (LLMotionController::motion_list_t::iterator iter = mMotionController.getActiveMotions().begin();
         iter != mMotionController.getActiveMotions().end(); ++iter)
    {
        LLMotion* motionp = *iter;
        if (motionp->getMinPixelArea() < getPixelArea())
        {
            std::string output;
            std::string motion_name = motionp->getName();
            if (motion_name.empty())
            {
                if (isControlAvatar())
                {
                    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
                    // Try to get name from inventory of associated object
                    LLVOVolume *volp = control_av->mRootVolp;
                    LLViewerInventoryItem *item = recursiveGetObjectInventoryItem(volp,motionp->getID());
                    if (item)
                    {
                        motion_name = item->getName();
                    }
                }
                else
                {
                    if (motionp->getID() == ANIM_AGENT_BENTO_IDLE)
                    {
                        motion_name = "bento_idle";
                    }
                }
            }
            if (motion_name.empty())
            {
                std::string name;
                if (gAgent.isGodlikeWithoutAdminMenuFakery() || isSelf())
                {
                    name = motionp->getID().asString();
                    LLVOAvatar::AnimSourceIterator anim_it = mAnimationSources.begin();
                    for (; anim_it != mAnimationSources.end(); ++anim_it)
                    {
                        if (anim_it->second == motionp->getID())
                        {
                            LLViewerObject* object = gObjectList.findObject(anim_it->first);
                            if (!object)
                            {
                                break;
                            }
                            if (object->isAvatar())
                            {
                                if (mMotionController.mIsSelf)
                                {
                                    // Searching inventory by asset id is really long
                                    // so just mark as inventory
                                    // Also item is likely to be named by LLPreviewAnim
                                    name += "(inventory)";
                                }
                            }
                            else
                            {
                                LLViewerInventoryItem* item = NULL;
                                if (!object->isInventoryDirty())
                                {
                                    item = object->getInventoryItemByAsset(motionp->getID());
                                }
                                if (item)
                                {
                                    name = item->getName();
                                }
                                else if (object->isAttachment())
                                {
                                    name += "(att:" + getAttachmentItemName() + ")";
                                }
                                else
                                {
                                    // in-world object, name or content unknown
                                    name += "(in-world)";
                                }
                            }
                            break;
                        }
                    }
                }
                else
                {
                    name = LLUUID::null.asString();
                }
                motion_name = name;
            }
            std::string motion_tag = "";
            if (mPlayingAnimations.find(motionp->getID()) != mPlayingAnimations.end())
            {
                motion_tag = "*";
            }
            output = llformat("%s%s - %d",
                              motion_name.c_str(),
                              motion_tag.c_str(),
                              (U32)motionp->getPriority());
            addDebugText(output);
        }
    }
}

void LLVOAvatar::updateDebugText()
{
    // Leave mDebugText uncleared here, in case a derived class has added some state first

    static LLCachedControl<bool> debug_av_appearance_message(gSavedSettings, "DebugAvatarAppearanceMessage");
    if (debug_av_appearance_message)
    {
        updateAppearanceMessageDebugText();
    }

    static LLCachedControl<bool> debug_av_composite_baked(gSavedSettings, "DebugAvatarCompositeBaked");
    if (debug_av_composite_baked)
    {
        if (!mBakedTextureDebugText.empty())
            addDebugText(mBakedTextureDebugText);
    }

    // Develop -> Avatar -> Animation Info
    if (LLVOAvatar::sShowAnimationDebug)
    {
        updateAnimationDebugText();
    }

    if (!mDebugText.size() && mText.notNull())
    {
        mText->markDead();
        mText = NULL;
    }
    else if (mDebugText.size())
    {
        setDebugText(mDebugText);
    }
    mDebugText.clear();
}

//------------------------------------------------------------------------
// updateFootstepSounds
// Factored out from updateCharacter()
// Generate footstep sounds when feet hit the ground
//------------------------------------------------------------------------
void LLVOAvatar::updateFootstepSounds()
{
    if (mIsDummy)
    {
        return;
    }

    // <FS> the two ground height lookups below were done for every avatar every frame and only used when a footstep animation is signaled
    {
        const LLUUID FOOTSTEP_ANIMS[] = { ANIM_AGENT_WALK, ANIM_AGENT_RUN, ANIM_AGENT_LAND };
        if (!gAudiop || !isAnyAnimationSignaled(FOOTSTEP_ANIMS, LL_ARRAY_SIZE(FOOTSTEP_ANIMS)))
        {
            return;
        }
    }

    //-------------------------------------------------------------------------
    // Find the ground under each foot, these are used for a variety
    // of things that follow
    //-------------------------------------------------------------------------
    LLVector3 ankle_left_pos_agent = mFootLeftp->getWorldPosition();
    LLVector3 ankle_right_pos_agent = mFootRightp->getWorldPosition();

    LLVector3 ankle_left_ground_agent = ankle_left_pos_agent;
    LLVector3 ankle_right_ground_agent = ankle_right_pos_agent;
    LLVector3 normal;
    resolveHeightAgent(ankle_left_pos_agent, ankle_left_ground_agent, normal);
    resolveHeightAgent(ankle_right_pos_agent, ankle_right_ground_agent, normal);

    F32 leftElev = llmax(-0.2f, ankle_left_pos_agent.mV[VZ] - ankle_left_ground_agent.mV[VZ]);
    F32 rightElev = llmax(-0.2f, ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ]);

    if (!isSitting())
    {
        //-------------------------------------------------------------------------
        // Figure out which foot is on ground
        //-------------------------------------------------------------------------
        if (!mInAir)
        {
            if ((leftElev < 0.0f) || (rightElev < 0.0f))
    {
                ankle_left_pos_agent = mFootLeftp->getWorldPosition();
                ankle_right_pos_agent = mFootRightp->getWorldPosition();
                leftElev = ankle_left_pos_agent.mV[VZ] - ankle_left_ground_agent.mV[VZ];
                rightElev = ankle_right_pos_agent.mV[VZ] - ankle_right_ground_agent.mV[VZ];
            }
        }
    }

    const LLUUID AGENT_FOOTSTEP_ANIMS[] = {ANIM_AGENT_WALK, ANIM_AGENT_RUN, ANIM_AGENT_LAND};
    const S32 NUM_AGENT_FOOTSTEP_ANIMS = LL_ARRAY_SIZE(AGENT_FOOTSTEP_ANIMS);

    if ( gAudiop && isAnyAnimationSignaled(AGENT_FOOTSTEP_ANIMS, NUM_AGENT_FOOTSTEP_ANIMS) )
    {
        bool playSound = false;
        LLVector3 foot_pos_agent;

        bool onGroundLeft = (leftElev <= 0.05f);
        bool onGroundRight = (rightElev <= 0.05f);

        // did left foot hit the ground?
        if ( onGroundLeft && !mWasOnGroundLeft )
        {
            foot_pos_agent = ankle_left_pos_agent;
            playSound = true;
        }

        // did right foot hit the ground?
        if ( onGroundRight && !mWasOnGroundRight )
    {
            foot_pos_agent = ankle_right_pos_agent;
            playSound = true;
    }

        mWasOnGroundLeft = onGroundLeft;
        mWasOnGroundRight = onGroundRight;

        // <FS:PP> FIRE-3169: Option to change the default footsteps sound
        // if ( playSound )
        static LLCachedControl<bool> PlayModeUISndFootsteps(gSavedSettings, "PlayModeUISndFootsteps");
        if ( playSound && PlayModeUISndFootsteps )
        // </FS:PP>
        {
            const F32 STEP_VOLUME = 0.1f;
            const LLUUID& step_sound_id = getStepSound();

            LLVector3d foot_pos_global = gAgent.getPosGlobalFromAgent(foot_pos_agent);

            if (LLViewerParcelMgr::getInstance()->canHearSound(foot_pos_global)
                && !LLMuteList::getInstance()->isMuted(getID(), LLMute::flagObjectSounds))
            {
                gAudiop->triggerSound(step_sound_id, getID(), STEP_VOLUME, LLAudioEngine::AUDIO_TYPE_AMBIENT, foot_pos_global);
            }
        }
    }
}

//------------------------------------------------------------------------
// computeUpdatePeriod()
// Factored out from updateCharacter()
// Set new value for mUpdatePeriod based on distance and various other factors.
//
// Note 10-2020: it turns out that none of these update period
// calculations have been having any effect, because
// mNeedsImpostorUpdate was not being set in updateCharacter(). So
// it's really open to question whether we want to enable time based updates, and if
// so, at what rate. Leaving the rates as given would lead to
// drastically more frequent impostor updates than we've been doing all these years.
// ------------------------------------------------------------------------
void LLVOAvatar::computeUpdatePeriod()
{
    bool visually_muted = isVisuallyMuted();
    bool slow = isTooSlowWithoutShadows();// <FS:Beq/> the geometry alone is forcing this to be slow so we must imposter
    if (mDrawable.notNull()
        && isVisible()
        && (!isSelf() || visually_muted)
        && !isUIAvatar()
        && (sLimitNonImpostors || visually_muted || slow) // <FS:Beq/> imposter slow avatars irrespective of nonimposter setting.
        && !mNeedsAnimUpdate)
    {
        const LLVector4a* ext = mDrawable->getSpatialExtents();
        LLVector4a size;
        size.setSub(ext[1], ext[0]);
        F32 mag = size.getLength3().getF32() * 0.5f;

        const S32 UPDATE_RATE_SLOW = 64;
        const S32 UPDATE_RATE_MED = 48;
        const S32 UPDATE_RATE_FAST = 32;
        if(slow)
        {
            mUpdatePeriod = UPDATE_RATE_FAST;
        }
        else if (visually_muted)
        {   // visually muted avatars update at lowest rate
            mUpdatePeriod = UPDATE_RATE_SLOW;
        }
        else if (!shouldImpostor()
            || mDrawable->mDistanceWRTCamera < 1.f + mag)
        {   // first 25% of max visible avatars are not impostored
            // also, don't impostor avatars whose bounding box may be penetrating the
            // impostor camera near clip plane
            mUpdatePeriod = 1;
        }
        else if (shouldImpostor(4.0))
        { //background avatars are REALLY slow updating impostors
            mUpdatePeriod = UPDATE_RATE_SLOW;
        }
        else if (mLastRezzedStatus <= 0)
        {
            // Don't update cloud avatars too often
            mUpdatePeriod = UPDATE_RATE_SLOW;
        }
        else if (shouldImpostor(3.0))
        { //back 25% of max visible avatars are slow updating impostors
            mUpdatePeriod = UPDATE_RATE_MED;
        }
        else
        {
            //nearby avatars, update the impostors more frequently.
            mUpdatePeriod = UPDATE_RATE_FAST;
        }
    }
    else
    {
        mUpdatePeriod = 1;
    }
}

//------------------------------------------------------------------------
// updateOrientation()
// Factored out from updateCharacter()
// This is used by updateCharacter() to update the avatar's orientation:
// - updates mTurning state
// - updates rotation of the mRoot joint in the skeleton
// - for self, calls setControlFlags() to notify the simulator about any turns
//------------------------------------------------------------------------
void LLVOAvatar::updateOrientation(LLAgent& agent, F32 speed, F32 delta_time)
{
            LLQuaternion iQ;
            LLVector3 upDir( 0.0f, 0.0f, 1.0f );

            // Compute a forward direction vector derived from the primitive rotation
            // and the velocity vector.  When walking or jumping, don't let body deviate
            // more than 90 from the view, if necessary, flip the velocity vector.

            LLVector3 primDir;
            if (isSelf())
            {
                primDir = agent.getAtAxis() - projected_vec(agent.getAtAxis(), agent.getReferenceUpVector());
                primDir.normalize();
            }
            else
            {
                primDir = getRotation().getMatrix3().getFwdRow();
            }
            LLVector3 velDir = getVelocity();
            velDir.normalize();
            // <FS> Disable avatar turning towards camera when walking backwards
            //if ( mSignaledAnimations.find(ANIM_AGENT_WALK) != mSignaledAnimations.end())
            static LLCachedControl<bool> walk_backwards(gSavedSettings, "FSDisableTurningAroundWhenWalkingBackwards");
            if (walk_backwards && mSignaledAnimations.find(ANIM_AGENT_WALK) != mSignaledAnimations.end())
            // </FS>
            {
                F32 vpD = velDir * primDir;
                if (vpD < -0.5f)
                {
                    velDir *= -1.0f;
                }
            }
            LLVector3 fwdDir = lerp(primDir, velDir, clamp_rescale(speed, 0.5f, 2.0f, 0.0f, 1.0f));
            if (isSelf() && gAgentCamera.cameraMouselook())
            {
                // make sure fwdDir stays in same general direction as primdir
                if (gAgent.getFlying())
                {
                    fwdDir = LLViewerCamera::getInstance()->getAtAxis();
                }
                else
                {
                    LLVector3 at_axis = LLViewerCamera::getInstance()->getAtAxis();
                    LLVector3 up_vector = gAgent.getReferenceUpVector();
                    at_axis -= up_vector * (at_axis * up_vector);
                    at_axis.normalize();

                    F32 dot = fwdDir * at_axis;
                    if (dot < 0.f)
                    {
                        fwdDir -= 2.f * at_axis * dot;
                        fwdDir.normalize();
                    }
                }
            }

            F32 root_roll, root_pitch, root_yaw;
            {
                AvScope matrix_scope(AV_ORIENT_MATRIX);
                LLQuaternion root_rotation = mRoot->getWorldMatrix().quaternion();
                root_rotation.getEulerAngles(&root_roll, &root_pitch, &root_yaw);
            }

            // When moving very slow, the pelvis is allowed to deviate from the
    // forward direction to allow it to hold its position while the torso
            // and head turn.  Once in motion, it must conform however.
            bool self_in_mouselook = isSelf() && gAgentCamera.cameraMouselook();

            LLVector3 pelvisDir( mRoot->getWorldMatrix().getFwdRow4().mV );

            static LLCachedControl<F32> s_pelvis_rot_threshold_slow(gSavedSettings, "AvatarRotateThresholdSlow", 60.0);
            static LLCachedControl<F32> s_pelvis_rot_threshold_fast(gSavedSettings, "AvatarRotateThresholdFast", 2.0);

            F32 pelvis_rot_threshold = clamp_rescale(speed, 0.1f, 1.0f, s_pelvis_rot_threshold_slow, s_pelvis_rot_threshold_fast);

            if (self_in_mouselook)
            {
                pelvis_rot_threshold *= MOUSELOOK_PELVIS_FOLLOW_FACTOR;
            }
            pelvis_rot_threshold *= DEG_TO_RAD;

            F32 angle = angle_between( pelvisDir, fwdDir );

            // The avatar's root is allowed to have a yaw that deviates widely
            // from the forward direction, but if roll or pitch are off even
            // a little bit we need to correct the rotation.
            if(root_roll < 1.f * DEG_TO_RAD
               && root_pitch < 5.f * DEG_TO_RAD)
            {
                // smaller correction vector means pelvis follows prim direction more closely
                if (!mTurning && angle > pelvis_rot_threshold*0.75f)
                {
                    mTurning = true;
                }

                // use tighter threshold when turning
                if (mTurning)
                {
                    pelvis_rot_threshold *= 0.4f;
                    // account for fps, assume that above value is for ~60fps
                    constexpr F32 default_frame_sec = 0.016f;
                    F32 prev_frame_sec = LLFrameTimer::getFrameDeltaTimeF32();
                    if (default_frame_sec > prev_frame_sec)
                    {
                        // reduce threshold since turn rate per second is constant,
                        // shorter frame means shorter turn.
                        pelvis_rot_threshold *= prev_frame_sec/default_frame_sec;
                    }
                }

                // am I done turning?
                if (angle < pelvis_rot_threshold)
                {
                    mTurning = false;
                }

                LLVector3 correction_vector = (pelvisDir - fwdDir) * clamp_rescale(angle, pelvis_rot_threshold*0.75f, pelvis_rot_threshold, 1.0f, 0.0f);
                fwdDir += correction_vector;
            }
            else
            {
                mTurning = false;
            }

            // Now compute the full world space rotation for the whole body (wQv)
            LLVector3 leftDir = upDir % fwdDir;
            leftDir.normalize();
            fwdDir = leftDir % upDir;
            LLQuaternion wQv( fwdDir, leftDir, upDir );

            if (isSelf() && mTurning)
            {
                if ((fwdDir % pelvisDir) * upDir > 0.f)
                {
                    gAgent.setControlFlags(AGENT_CONTROL_TURN_RIGHT);
                }
                else
                {
                    gAgent.setControlFlags(AGENT_CONTROL_TURN_LEFT);
                }
            }

            // Set the root rotation, but do so incrementally so that it
            // lags in time by some fixed amount.
            //F32 u = LLSmoothInterpolation::getInterpolant(PELVIS_LAG);
            F32 pelvis_lag_time = 0.f;
            if (self_in_mouselook)
            {
                pelvis_lag_time = PELVIS_LAG_MOUSELOOK;
            }
            else if (mInAir)
            {
                pelvis_lag_time = PELVIS_LAG_FLYING;
                // increase pelvis lag time when moving slowly
                pelvis_lag_time *= clamp_rescale(mSpeedAccum, 0.f, 15.f, 3.f, 1.f);
            }
            else
            {
                // <FS:Beq> FIRE-29581 remove stones from wet sack for Willow
                // pelvis_lag_time = PELVIS_LAG_WALKING;
                static constexpr F32 turn_rate_delta{0.0019f}; // linear scale
                static LLCachedControl<F32> turn_speed(gSavedSettings, "FSAvatarTurnSpeed", 0.0f); // 0 is default. We can't go slower.
                pelvis_lag_time = llmax(PELVIS_LAG_WALKING - (llclamp(turn_speed(), 0.f, 100.f) * turn_rate_delta), F_ALMOST_ZERO);
                // </FS:Beq>
            }

    F32 u = llclamp((delta_time / pelvis_lag_time), 0.0f, 1.0f);

            {
                // <FS> an avatar that stands still comes out of the slerp with the rotation it had, give or take the last bit: do not touch the skeleton for that
                const LLQuaternion current_rotation = mRoot->getWorldRotation();
                const LLQuaternion new_rotation = slerp(u, current_rotation, wQv);
                if (dot(current_rotation, new_rotation) < 0.9999999f)
                {
                    mRoot->setWorldRotation(new_rotation);
                }
            }
}

//------------------------------------------------------------------------
// updateTimeStep()
// Factored out from updateCharacter().
//
// Updates the time step used by the motion controller, based on area
// and avatar count criteria.  This will also stop the
// ANIM_AGENT_WALK_ADJUST animation under some circumstances.
// ------------------------------------------------------------------------
void LLVOAvatar::updateTimeStep()
{
    // <FS:Zi> Optionally disable the usage of timesteps, testing if this affects performance or
    //         creates animation issues - FIRE-3657
    //if (!isSelf() && !isUIAvatar()) // ie, non-self avatars, and animated objects will be affected.
    static LLCachedControl<bool> use_timesteps(gSavedSettings, "UseAnimationTimeSteps");
    if (!isSelf() && !isUIAvatar() && use_timesteps)
    // </FS:Zi>
    {
        // Note that sInstances counts animated objects and
        // standard avatars in the same bucket. Is this desirable?
        F32 time_quantum = clamp_rescale((F32)sInstances.size(), 10.f, 35.f, 0.f, 0.25f);
        F32 pixel_area_scale = clamp_rescale(mPixelArea, 100, 5000, 1.f, 0.f);
        F32 time_step = time_quantum * pixel_area_scale;
        // Extrema:
        //   If number of avs is 10 or less, time_step is unmodified (flagged with 0.0).
        //   If area of av is 5000 or greater, time_step is unmodified (flagged with 0.0).
        //   If number of avs is 35 or greater, and area of av is 100 or less,
        //   time_step takes the maximum possible value of 0.25.
        //   Other situations will give values within the (0, 0.25) range.
        if (time_step != 0.f)
        {
            // disable walk motion servo controller as it doesn't work with motion timesteps
            stopMotion(ANIM_AGENT_WALK_ADJUST);
            removeAnimationData("Walk Speed");
        }
        // See SL-763 - playback with altered time step does not
        // appear to work correctly, odd behavior for distant avatars.
        // As of 11-2017, LLMotionController::updateMotions() will
        // ignore the value here. Need to re-enable if it's every
        // fixed.
        mMotionController.setTimeStep(time_step);
    }
    // <FS:Zi> Optionally disable the usage of timesteps, testing if this affects performance or
    //         creates animation issues - FIRE-3657
    else
    {
        mMotionController.setTimeStep(0.0f);
    }
    // </FS:Zi>
}

void LLVOAvatar::updateRootPositionAndRotation(LLAgent& agent, F32 speed, bool was_sit_ground_constrained)
{
    if (!(isSitting() && getParent()))
    {
        // This case includes all configurations except sitting on an
        // object, so does include ground sit.

        //--------------------------------------------------------------------
        // get timing info
        // handle initial condition case
        //--------------------------------------------------------------------
        F32 animation_time = mAnimTimer.getElapsedTimeF32();
        if (mTimeLast == 0.0f)
        {
            mTimeLast = animation_time;

            // Initially put the pelvis at slaved position/mRotation
            // SL-315
            mRoot->setWorldPosition( getPositionAgent() ); // first frame
            mRoot->setWorldRotation( getRotation() );
        }

        //--------------------------------------------------------------------
        // dont' let dT get larger than 1/5th of a second
        //--------------------------------------------------------------------
        F32 delta_time = animation_time - mTimeLast;

        delta_time = llclamp( delta_time, DELTA_TIME_MIN, DELTA_TIME_MAX );
        mTimeLast = animation_time;

        mSpeedAccum = (mSpeedAccum * 0.95f) + (speed * 0.05f);

        //--------------------------------------------------------------------
        // compute the position of the avatar's root
        //--------------------------------------------------------------------
        LLVector3d root_pos;
        LLVector3d ground_under_pelvis;

        if (isSelf())
        {
            gAgent.setPositionAgent(getRenderPosition());
        }

        root_pos = gAgent.getPosGlobalFromAgent(getRenderPosition());
        root_pos.mdV[VZ] += getVisualParamWeight(AVATAR_HOVER);

        LLVector3 normal;
        {
            AvScope height_scope(AV_ROOT_HEIGHT);
            resolveHeightGlobal(root_pos, ground_under_pelvis, normal);
        }
        F32 foot_to_ground = (F32) (root_pos.mdV[VZ] - mPelvisToFoot - ground_under_pelvis.mdV[VZ]);
        bool in_air = ((!LLWorld::getInstance()->getRegionFromPosGlobal(ground_under_pelvis)) ||
                        foot_to_ground > FOOT_GROUND_COLLISION_TOLERANCE);

        if (in_air && !mInAir)
        {
            mTimeInAir.reset();
        }
        mInAir = in_air;

        // SL-402: with the ability to animate the position of joints
        // that affect the body size calculation, computed body size
        // can get stale much more easily. Simplest fix is to update
        // it frequently.
        // SL-427: this appears to be too frequent, moving to only do on animation state change.
        //computeBodySize();

        // correct for the fact that the pelvis is not necessarily the center
        // of the agent's physical representation
        root_pos.mdV[VZ] -= (0.5f * mBodySize.mV[VZ]) - mPelvisToFoot;
        if (!isSitting() && !was_sit_ground_constrained)
        {
            root_pos += LLVector3d(getHoverOffset());
            if (getOverallAppearance() == AOA_JELLYDOLL)
            {
                F32 offz = -0.5f * (getScale()[VZ] - mBodySize.mV[VZ]);
                root_pos[2] += offz;
                // if (!isSelf() && !isControlAvatar())
                // {
                //  LL_DEBUGS("Avatar") << "av " << getFullname()
                //                      << " frame " << LLFrameTimer::getFrameCount()
                //                      << " root adjust offz " << offz
                //                      << " scalez " << getScale()[VZ]
                //                      << " bsz " << mBodySize.mV[VZ]
                //                      << LL_ENDL;
                // }
            }
        }
        // if (!isSelf() && !isControlAvatar())
        // {
        //  LL_DEBUGS("Avatar") << "av " << getFullname() << " aoa " << (S32) getOverallAppearance()
        //                      << " frame " << LLFrameTimer::getFrameCount()
        //                      << " scalez " << getScale()[VZ]
        //                      << " bsz " << mBodySize.mV[VZ]
        //                      << " root pos " << root_pos[2]
        //                      << " curr rootz " << mRoot->getPosition()[2]
        //                      << " pp-z " << mPelvisp->getPosition()[2]
        //                      << " renderpos " << getRenderPosition()
        //                      << LL_ENDL;
        // }

        LLControlAvatar *cav = dynamic_cast<LLControlAvatar*>(this);
        if (cav)
        {
            // SL-1350: Moved to LLDrawable::updateXform()
            cav->matchVolumeTransform();
        }
        else
        {
            LLVector3 newPosition = gAgent.getPosAgentFromGlobal(root_pos);
            // if (!isSelf() && !isControlAvatar())
            // {
            //  LL_DEBUGS("Avatar") << "av " << getFullname()
            //                      << " frame " << LLFrameTimer::getFrameCount()
            //                      << " newPosition " << newPosition
            //                      << " renderpos " << getRenderPosition()
            //                      << LL_ENDL;
            // }
            if (newPosition != mRoot->getXform()->getWorldPosition())
            {
                mRoot->touch();
                // SL-315
                mRoot->setWorldPosition( newPosition ); // regular update
            }
        }

        //--------------------------------------------------------------------
        // Propagate viewer object rotation to root of avatar
        //--------------------------------------------------------------------
        if (!isControlAvatar() && !isAnyAnimationSignaled(AGENT_NO_ROTATE_ANIMS, NUM_AGENT_NO_ROTATE_ANIMS))
        {
            // Rotation fixups for avatars in motion.
            // Skip for animated objects.
            AvScope orient_scope(AV_ROOT_ORIENT);
            updateOrientation(agent, speed, delta_time);
        }
    }
    else if (mDrawable.notNull())
    {
        // Sitting on an object - mRoot is slaved to mDrawable orientation.
        LLVector3 pos = mDrawable->getPosition();
        pos += getHoverOffset() * mDrawable->getRotation();
        // SL-315
        mRoot->setPosition(pos);
        mRoot->setRotation(mDrawable->getRotation());
    }
}

//------------------------------------------------------------------------
// LLVOAvatar::computeNeedsUpdate()
//
// Most of the logic here is to figure out when to periodically update impostors.
// Non-impostors have mUpdatePeriod == 1 and will need update every frame.
//------------------------------------------------------------------------

// <FS> PLAN-avatars.md stage 0 (LL_VK_AVIMP=1): the importance class of every avatar the update loop visits, as a number next to the tests the code uses today, and what the loop then did with it.
// Class: 0 the viewer's own, 1 seen in the main view (isVisible()), 2 not in the main view but inside a sun cascade's frustum (the previous frame's cameras), 3 seen by nothing. Kind: 0 a resident's
// avatar, 1 a dummy / control avatar (animesh) / UI avatar. Ran: 0 skipped by computeNeedsUpdate (impostor period, far period), 1 the hidden update (not visible: motions only, matrices in the batch),
// 2 the full update. Counters only: nothing here changes what is done.
namespace
{
struct AvImportanceStats
{
    U64 mRan[2][4][3] = {};         // [kind][class][ran]
    U64 mFlag[4][8] = {};           // resident avatars by class: impostor, culled, too slow, visually muted, update period > 1, sitting, far from the camera over 40 m, fully loaded
    U64 mCalls = 0;
    U32 mLast = 0;
};
AvImportanceStats sAvImp;
std::atomic<U64> sAvImpNs[4][2][4], sAvImpCount[4][2][4];          // [region][kind][class]: time and visits
std::unordered_map<const LLVOAvatar*, int> sAvImpMap;              // avatar -> kind * 4 + class, set in updateCharacter (the main thread), read after it by the blend jobs and the completion
std::atomic<U64> sAvPieceNs[8][16], sAvPieceCount[8][16];          // [kind * 4 + class][piece]
std::atomic<U64> sAvMotionSum[8], sAvMotionCount[8];               // active motions of the controller at the blend job
}

static void fs_av_piece_time(int slot, const LLVOAvatar* av, U64 ns)
{
    auto found = sAvImpMap.find(av);
    if (found == sAvImpMap.end())
    {
        return;
    }
    sAvPieceNs[found->second][slot].fetch_add(ns, std::memory_order_relaxed);
    sAvPieceCount[found->second][slot].fetch_add(1, std::memory_order_relaxed);
}

static void fs_av_imp_motions(const LLVOAvatar* av, size_t motions)
{
    auto found = sAvImpMap.find(av);
    if (found == sAvImpMap.end())
    {
        return;
    }
    sAvMotionSum[found->second].fetch_add(motions, std::memory_order_relaxed);
    sAvMotionCount[found->second].fetch_add(1, std::memory_order_relaxed);
}

static bool fs_av_imp_on()
{
    static const bool on = getenv("LL_VK_AVIMP") != nullptr;
    return on;
}

static void fs_av_imp_time(int region, const LLVOAvatar* av, U64 ns)
{
    auto found = sAvImpMap.find(av);
    if (found == sAvImpMap.end())
    {
        return;
    }
    const int kind = found->second / 4, cls = found->second % 4;
    sAvImpNs[region][kind][cls].fetch_add(ns, std::memory_order_relaxed);
    sAvImpCount[region][kind][cls].fetch_add(1, std::memory_order_relaxed);
}

static int fs_av_importance_class(LLVOAvatar* av, bool visible)
{
    if (av->isSelf())
    {
        return 0;
    }
    if (visible)
    {
        return 1;
    }
    LLDrawable* drawable = av->mDrawable;
    if (drawable && LLPipeline::RenderShadowDetail > 0)
    {
        const LLVector4a* ext = drawable->getSpatialExtents();
        LLVector4a center, half;
        center.setAdd(ext[0], ext[1]);
        center.mul(0.5f);
        half.setSub(ext[1], ext[0]);
        half.mul(0.5f);
        for (int c = 0; c < 4; ++c)
        {
            if (gPipeline.mShadowCamera[c].AABBInFrustum(center, half) != 0)
            {
                return 2;
            }
        }
    }
    return 3;
}

static int sAvImpKind = 0, sAvImpClass = 0;

static void fs_av_importance_begin(LLVOAvatar* av, bool visible)
{
    static const bool on = getenv("LL_VK_AVIMP") != nullptr;
    if (!on)
    {
        sAvImpClass = -1;
        return;
    }
    sAvImpKind = (av->isControlAvatar() || av->isUIAvatar() || av->mDrawable.isNull()) ? 1 : 0;
    sAvImpClass = fs_av_importance_class(av, visible);
    sAvImpMap[av] = sAvImpKind * 4 + sAvImpClass;
    if (sAvImpKind == 0)
    {
        U64* f = sAvImp.mFlag[sAvImpClass];
        f[0] += av->isImpostor() ? 1 : 0;
        f[1] += av->isCulled() ? 1 : 0;
        f[2] += av->isTooSlow() ? 1 : 0;
        f[3] += av->isVisuallyMuted() ? 1 : 0;
        f[4] += av->getUpdatePeriod() > 1 ? 1 : 0;
        f[5] += av->getParent() ? 1 : 0;
        f[6] += (av->mDrawable.notNull() && av->mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale() > 40.f) ? 1 : 0;
        f[7] += av->isFullyLoaded() ? 1 : 0;
    }
}

static void fs_av_importance_ran(int ran)
{
    if (sAvImpClass < 0)
    {
        return;
    }
    ++sAvImp.mRan[sAvImpKind][sAvImpClass][ran];
    if (gFrameCount >= sAvImp.mLast + 600)
    {
        const double frames = (double)std::max<U32>(1, gFrameCount - sAvImp.mLast);
        sAvImp.mLast = gFrameCount;
        static const char* const cls[4] = { "own", "main view", "cascade only", "nothing" };
        std::string text;
        for (int k = 0; k < 2; ++k)
        {
            for (int c = 0; c < 4; ++c)
            {
                const U64* r = sAvImp.mRan[k][c];
                if (r[0] + r[1] + r[2])
                {
                    text += llformat(" | %s %s: %.2f (skipped %.2f, hidden update %.2f, full update %.2f)", k ? "dummy/control/UI" : "avatars", cls[c], (r[0] + r[1] + r[2]) / frames, r[0] / frames, r[1] / frames, r[2] / frames);
                }
            }
        }
        std::string flags;
        static const char* const names[8] = { "impostor", "culled", "too slow", "visually muted", "period>1", "sitting", "over 40 m", "fully loaded" };
        for (int c = 0; c < 4; ++c)
        {
            const U64* f = sAvImp.mFlag[c];
            flags += llformat(" | %s:", cls[c]);
            for (int n = 0; n < 8; ++n)
            {
                flags += llformat(" %s %.2f", names[n], f[n] / frames);
            }
        }
        LL_INFOS("AvImportance") << "avatar importance class, per frame over " << (U32)frames << " frames (visits of the update loop)" << text << LL_ENDL;
        LL_INFOS("AvImportance") << "today's tests by class (avatars a frame)" << flags << LL_ENDL;
        {
            static const char* const region_names[4] = { "idleUpdate serial", "pose blend job", "completion", "of the job: world walk" };
            std::string cost;
            for (int r = 0; r < 4; ++r)
            {
                cost += llformat(" | %s:", region_names[r]);
                for (int k = 0; k < 2; ++k)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        const U64 n = sAvImpCount[r][k][c].exchange(0), ns = sAvImpNs[r][k][c].exchange(0);
                        if (n)
                        {
                            cost += llformat(" %s %s %.3f ms (%.1f us x %.1f)", k ? "ctl" : "av", cls[c], (double)ns / frames / 1.0e6, (double)ns / n / 1000.0, n / frames);
                        }
                    }
                }
            }
            LL_INFOS("AvImportance") << "cost by class, ms a frame (us an avatar x avatars a frame)" << cost << LL_ENDL;
            static const char* const self_names[14] = { "flush stop requests", "updateCharacterPostMotion", "idleUpdatePostCharacter (all)", "name tag position", "voice visualizer", "idleUpdateMisc (all)",
                                                        "attachments (replay / loop)", "impostor checks", "movePartition", "appearance animation", "lipsync / loading / water / wind", "name tag", "render complexity", "debug info" };
            for (int kc = 0; kc < 8; ++kc)
            {
                std::string own;
                for (int n = 0; n < 14; ++n)
                {
                    const U64 c = sAvPieceCount[kc][n].exchange(0), ns = sAvPieceNs[kc][n].exchange(0);
                    if (c)
                    {
                        own += llformat(" | %s %.3f ms (%.1f us x %.2f)", self_names[n], (double)ns / frames / 1.0e6, (double)ns / c / 1000.0, c / frames);
                    }
                }
                const U64 mc = sAvMotionCount[kc].exchange(0), ms = sAvMotionSum[kc].exchange(0);
                if (!own.empty() || mc)
                {
                    LL_INFOS("AvImportance") << "pieces of the update, " << (kc / 4 ? "animesh / control / UI " : "avatars ") << cls[kc % 4] << " (ms a frame, us each, visits a frame; the pieces nest as the names say)" << own
                        << " | active motions at the blend " << (mc ? (double)ms / mc : 0.0) << " (" << mc / frames << " blends a frame)" << LL_ENDL;
                }
            }
        }
        memset(&sAvImp.mRan, 0, sizeof(sAvImp.mRan));
        memset(&sAvImp.mFlag, 0, sizeof(sAvImp.mFlag));
    }
}


//------------------------------------------------------------------------
// <FS> PLAN-avatars.md stage A: the avatar table (llavatartable.h)
//------------------------------------------------------------------------
bool LLVOAvatar::reducedHiddenSkip()
{
    // <FS> An avatar nobody can see does not need the whole per-frame update: its motions, matrices, name tag and the rest are all driven by elapsed time (the impostor
    // path already runs them every 32-64 frames), and it is picked up in full the frame after it comes into view. Others' avatars only, and not when something is waiting
    // for the next full update (a first update, a special render mode): every second frame, every fourth when the avatar is far away, staggered by avatar id.
    static LLCachedControl<bool> reduce_hidden(gSavedSettings, "FSReduceHiddenAvatarUpdates", true);
    if (reduce_hidden && !isSelf() && !mIsControlAvatar && !isUIAvatar() && mDrawable.notNull() && mSpecialRenderMode == 0
        && !mNeedsAnimUpdate && !isVisible())
    {
        const F32 distance = (getPositionAgent() - LLViewerCamera::getInstance()->getOrigin()).length();
        const S32 period = distance > 50.f ? 4 : 2;
        if (((LLFrameTimer::getFrameCount() + mID.mData[0]) % period) != 0)
        {
            return true;
        }
    }
    return false;
}

// the gates of idleUpdate and updateCharacter, in their order (idleUpdate: dead / exiting, friends only, tooSlow, render type off, the reduced update; updateCharacter: not built, visible, computeNeedsUpdate)
unsigned char LLVOAvatar::decideFrameMode(bool& visible, bool& needs_update)
{
    visible = false;
    needs_update = false;
    if (LLApp::isExiting() || isDead())
    {
        return LLAvatarTable::MODE_OTHER;
    }
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    if (friends_only() && !isUIAvatar() && !isControlAvatar() && !isSelf() && !isBuddy())
    {
        return LLAvatarTable::MODE_OTHER;
    }
    if (LLAvatarTable::switchMode() == 1)
    {
        updateTooSlow();        // (mode 1: idleUpdate does not; mode 2 leaves it to idleUpdate and the answers differ where the state it makes matters)
    }
    static LLCachedControl<bool> disable_all_render_types(gSavedSettings, "DisableAllRenderTypes");
    if (!(gPipeline.hasRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR)) && !disable_all_render_types && !isSelf())
    {
        return LLAvatarTable::MODE_OTHER;
    }
    if (reducedHiddenSkip())
    {
        return LLAvatarTable::MODE_NONE;
    }
    if (!mIsBuilt)
    {
        return LLAvatarTable::MODE_OTHER;
    }
    visible = isVisible();
    needs_update = computeNeedsUpdate();
    if (!needs_update && !isSelf())
    {
        return LLAvatarTable::MODE_SKIP;
    }
    if (!visible && !isSelf())
    {
        return LLAvatarTable::MODE_MATRICES;
    }
    return LLAvatarTable::MODE_POSE;
}

namespace
{
std::vector<std::pair<const LLVOAvatar*, LLAvatarTable::Row>> sAvatarRows;      // the rows of the frame; an avatar's index is in LLVOAvatar::mAvTableSlot
U64 sAvTableAgree = 0, sAvTableMatrix[LLAvatarTable::MODE_COUNT][LLAvatarTable::MODE_COUNT] = {}, sAvTableNoRow = 0;
U32 sAvTableLast = 0;
}

int LLAvatarTable::switchMode()
{
    static const int mode = getenv("LL_VK_AVTABLE") ? atoi(getenv("LL_VK_AVTABLE")) : 1;       // 1 (default since 2026-11-07: Adam looked at the avt2 lap, nothing differed) the loop reads the row, 0 the old gates, 2 both and count
    return mode;
}

const LLAvatarTable::Row* LLAvatarTable::row(const LLVOAvatar* av)
{
    const S32 slot = av->mAvTableSlot;
    return (slot >= 0 && (size_t)slot < sAvatarRows.size() && sAvatarRows[slot].first == av) ? &sAvatarRows[slot].second : nullptr;
}

const LLAvatarTable::Row* LLAvatarTable::finalRow(const LLVOAvatar* av)
{
    if (switchMode() != 1)
    {
        return nullptr;
    }
    const S32 slot = av->mAvTableSlot;
    if (slot < 0 || (size_t)slot >= sAvatarRows.size() || sAvatarRows[slot].first != av || !sAvatarRows[slot].second.mFinal)
    {
        return nullptr;
    }
    return &sAvatarRows[slot].second;
}

void LLAvatarTable::endFrame()
{
    if (switchMode() != 1)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    for (auto& entry : sAvatarRows)
    {
        // (the avatar may have died in the loop: its row is dropped at the next beginFrame, and nothing asks a dead avatar)
        LLVOAvatar* av = const_cast<LLVOAvatar*>(entry.first);
        if (av->isDead())
        {
            continue;
        }
        entry.second.mTooSlow = av->isTooSlow();        // (not final yet: the live answers)
        entry.second.mMuted = av->isVisuallyMuted();
        entry.second.mAppearance = (U8)av->getOverallAppearance();
        entry.second.mImpostor = av->isImpostor();      // (the virtual one: a control avatar follows the avatar it is attached to; after the two above: they are inputs)
    }
    for (auto& entry : sAvatarRows)
    {
        entry.second.mFinal = true;
    }
}

void LLAvatarTable::noteCheck(int which, bool same)
{
    static U64 reads[4] = {}, different[4] = {};
    static U32 last = 0;
    ++reads[which];
    different[which] += same ? 0 : 1;
    if (LLFrameTimer::getFrameCount() >= last + 600)
    {
        const double frames = (double)std::max<U32>(1, LLFrameTimer::getFrameCount() - last);
        last = LLFrameTimer::getFrameCount();
        LL_INFOS("AvTable") << "reads from the row per frame over " << (U32)frames << " frames (different from the live answer, in all this window): impostor " << reads[0] / frames << " (" << different[0] << "), too slow "
            << reads[1] / frames << " (" << different[1] << "), visually muted " << reads[2] / frames << " (" << different[2] << "), overall appearance " << reads[3] / frames << " (" << different[3] << ")" << LL_ENDL;
        for (int k = 0; k < 4; ++k) { reads[k] = different[k] = 0; }
    }
}

void LLAvatarTable::beginFrame(const std::vector<LLViewerObject*>& list, size_t count)
{
    if (!switchMode())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    sAvatarRows.clear();
    const U32 frame = LLFrameTimer::getFrameCount();
    for (size_t i = 0; i < count; ++i)
    {
        LLViewerObject* object = list[i];
        if (!object || !object->isAvatar())
        {
            continue;
        }
        LLVOAvatar* av = static_cast<LLVOAvatar*>(object);
        Row r;
        r.mMode = av->decideFrameMode(r.mVisible, r.mNeedsUpdate);
        r.mClass = av->isSelf() ? CLASS_OWN : (r.mVisible ? CLASS_SEEN : CLASS_UNSEEN);
        r.mFrame = frame;
        if (av->mDrawable.notNull() && !av->isSelf())
        {
            // the main camera's frustum (the previous frame's planes: the update runs before the cull), the extents grown by the room a name tag takes (above the head, sideways in screen space)
            const LLVector4a* ext = av->mDrawable->getSpatialExtents();
            LLVector4a center, half;
            center.setAdd(ext[0], ext[1]);
            center.mul(0.5f);
            half.setSub(ext[1], ext[0]);
            half.mul(0.5f);
            const F32 grow = 2.5f + 0.3f * av->mDrawable->mDistanceWRTCamera;
            half.add(LLVector4a(grow));
            r.mInFrustum = LLViewerCamera::getInstance()->AABBInFrustum(center, half) != 0;
        }
        r.mBits = (av->isSelf() || r.mInFrustum || av->nameTagBusy()) ? BIT_NAME_TAG : 0;
        r.mBits |= (r.mMode == MODE_POSE && r.mVisible) ? BIT_ATTACHMENT_LOOP : 0;
        av->mAvTableSlot = (S32)sAvatarRows.size();
        sAvatarRows.emplace_back(av, r);
    }
}

bool LLAvatarTable::attachmentLoop(const LLVOAvatar* av, bool from_update)
{
    if (switchMode() != 1)
    {
        return from_update;
    }
    const Row* r = row(av);
    if (!r)
    {
        return from_update;
    }
    const bool bit = (r->mBits & BIT_ATTACHMENT_LOOP) != 0;
    static U64 reads = 0, different = 0;
    static U32 last = 0;
    ++reads;
    different += bit != from_update ? 1 : 0;
    if (LLFrameTimer::getFrameCount() >= last + 600)
    {
        const double frames = (double)std::max<U32>(1, LLFrameTimer::getFrameCount() - last);
        last = LLFrameTimer::getFrameCount();
        LL_INFOS("AvTable") << "attachment loop gate from the row per frame over " << (U32)frames << " frames: " << reads / frames << " avatars, different from updateCharacter's result " << different / frames << " (" << different << " in this window)" << LL_ENDL;
        reads = different = 0;
    }
    return bit;
}

bool LLAvatarTable::nameTagOpen(const LLVOAvatar* av)
{
    if (switchMode() != 1)
    {
        return true;
    }
    static U64 ran = 0, skipped = 0;
    static U32 last = 0;
    const Row* r = row(av);
    const bool open = !r || (r->mBits & BIT_NAME_TAG);
    ++(open ? ran : skipped);
    if (LLFrameTimer::getFrameCount() >= last + 600)
    {
        const double frames = (double)std::max<U32>(1, LLFrameTimer::getFrameCount() - last);
        last = LLFrameTimer::getFrameCount();
        LL_INFOS("AvTable") << "name tags per frame over " << (U32)frames << " frames: updated " << ran / frames << ", left out (not in the camera's frustum, no chat) " << skipped / frames << LL_ENDL;
        ran = skipped = 0;
    }
    return open;
}

void LLAvatarTable::noteActual(const LLVOAvatar* av, U8 actual)
{
    if (switchMode() != 2)
    {
        return;
    }
    const Row* found = row(av);
    if (!found)
    {
        ++sAvTableNoRow;
    }
    else
    {
        ++sAvTableMatrix[found->mMode][actual];
        sAvTableAgree += found->mMode == actual ? 1 : 0;
    }
    if (LLFrameTimer::getFrameCount() >= sAvTableLast + 600)
    {
        const double frames = (double)std::max<U32>(1, LLFrameTimer::getFrameCount() - sAvTableLast);
        sAvTableLast = LLFrameTimer::getFrameCount();
        static const char* const names[MODE_COUNT] = { "other", "none", "skip", "matrices", "pose" };
        std::string text;
        U64 total = 0;
        for (int p = 0; p < MODE_COUNT; ++p)
        {
            for (int a = 0; a < MODE_COUNT; ++a)
            {
                if (sAvTableMatrix[p][a])
                {
                    total += sAvTableMatrix[p][a];
                    text += llformat(" | row %s, old gates %s: %.2f%s", names[p], names[a], sAvTableMatrix[p][a] / frames, p == a ? "" : "  <- DISAGREE");
                }
            }
        }
        LL_INFOS("AvTable") << "avatar table check, per frame over " << (U32)frames << " frames: " << total / frames << " avatars compared, agree " << sAvTableAgree / frames << ", no row " << sAvTableNoRow / frames << text << LL_ENDL;
        sAvTableAgree = sAvTableNoRow = 0;
        memset(sAvTableMatrix, 0, sizeof(sAvTableMatrix));
    }
}

// <FS> FSAvatarFarUpdateDistance: a visible avatar (not an impostor) beyond this distance updates its pose, root and attachments every 2nd frame, beyond 1.6 times that every 3rd; the pose is
// held in between (the animation time still runs: the next update catches up). Not the viewer's own avatar, a sitting one (it follows its seat), a fast one (over 3 m/s), an animated
// object, a UI avatar.
static F32 far_update_distance()
{
    static LLCachedControl<F32> setting(gSavedSettings, "FSAvatarFarUpdateDistance", 0.f);
    return setting;
}

U32 LLVOAvatar::farUpdatePeriod()
{
    // census (LL_VK_SCENECENSUS): why the avatars the rule looked at did or did not get a longer period, per frame over 600 frames
    static const bool census_on = getenv("LL_VK_SCENECENSUS") != nullptr;
    static U64 reasons[9] = {}, calls = 0;
    auto done = [&](int reason, U32 period) -> U32
    {
        if (census_on)
        {
            ++reasons[reason];
            if (++calls % (600 * 40) == 0)
            {
                LL_INFOS("FarUpdate") << "far avatar update rule, avatar visits a frame (about 40 avatars): off " << reasons[0] / 600.0 << ", self " << reasons[1] / 600.0 << ", dummy/control/animesh/UI " << reasons[2] / 600.0
                                      << ", sitting " << reasons[3] / 600.0 << ", fast " << reasons[4] / 600.0 << ", no drawable or not visible " << reasons[5] / 600.0 << ", near " << reasons[6] / 600.0
                                      << ", period 2 " << reasons[7] / 600.0 << ", period 3 " << reasons[8] / 600.0 << LL_ENDL;
                memset(reasons, 0, sizeof(reasons));
            }
        }
        return period;
    };
    const F32 distance_setting = far_update_distance();
    if (distance_setting <= 0.f) return done(0, 1);
    if (isSelf()) return done(1, 1);
    if (mIsDummy || mIsControlAvatar || mIsAnimesh || isUIAvatar()) return done(2, 1);
    if (getParent()) return done(3, 1);
    if (mSpeed > 3.f) return done(4, 1);
    if (mDrawable.isNull() || !isVisible()) return done(5, 1);
    const F32 distance = mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale();
    if (distance < distance_setting)
    {
        return done(6, 1);
    }
    return distance >= distance_setting * 1.6f ? done(8, 3) : done(7, 2);
}

bool LLVOAvatar::computeNeedsUpdate()
{
    const F32 MAX_IMPOSTOR_INTERVAL = 4.0f;
    computeUpdatePeriod();

    bool needs_update_by_frame_count = ((LLDrawable::getCurrentFrame()+mID.mData[0])%mUpdatePeriod == 0);

    bool needs_update_by_max_time = ((gFrameTimeSeconds-mLastImpostorUpdateFrameTime)> MAX_IMPOSTOR_INTERVAL);
    bool needs_update = needs_update_by_frame_count || needs_update_by_max_time;
    if (needs_update && mUpdatePeriod == 1)
    {
        // a full avatar far away: every 2nd or 3rd frame (impostors have their own, longer period). (needs_update_by_max_time is always true for a full avatar: its impostor time never
        // resets, so it must not decide here)
        const U32 far_period = farUpdatePeriod();
        if (far_period > 1 && ((LLDrawable::getCurrentFrame() + mID.mData[0]) % far_period) != 0)
        {
            needs_update = false;
        }
    }

    if (needs_update && !isSelf())
    {
        if (needs_update_by_max_time)
        {
            mNeedsImpostorUpdate = true;
            mLastImpostorUpdateReason = 11;
        }
        else
        {
            //mNeedsImpostorUpdate = true;
            //mLastImpostorUpdateReason = 10;
        }
    }
    return needs_update;
}

// updateCharacter()
//
// This is called for all avatars, so there are 4 possible situations:
//
// 1) Avatar is your own. In this case the class is LLVOAvatarSelf,
// isSelf() is true, and agent specifies the corresponding agent
// information for you. In all the other cases, agent is irrelevant
// and it would be less confusing if it were null or something.
//
// 2) Avatar is controlled by another resident. Class is LLVOAvatar,
// and isSelf() is false.
//
// 3) Avatar is the controller for an animated object. Class is
// LLControlAvatar and mIsDummy is true. Avatar is a purely
// viewer-side entity with no representation on the simulator.
//
// 4) Avatar is a UI avatar used in some areas of the UI, such as when
// previewing uploaded animations. Class is LLUIAvatar, and mIsDummy
// is true. Avatar is purely viewer-side with no representation on the
// simulator.
//
//------------------------------------------------------------------------
bool LLVOAvatar::updateCharacter(LLAgent &agent)
{
    AvScope char_total_scope(AV_CHAR_TOTAL);
    updateDebugText();

    if (!mIsBuilt)
    {
        return false;
    }

    const LLAvatarTable::Row* table_row = LLAvatarTable::switchMode() == 1 ? LLAvatarTable::row(this) : nullptr;       // <FS> stage A mode 1
    bool visible = table_row ? table_row->mVisible : isVisible();

    // For fading out the names above heads, only let the timer
    // run if we're visible.
    if (mDrawable.notNull() && !visible)
    {
        mTimeVisible.reset();
    }
    fs_av_importance_begin(this, visible);

    //--------------------------------------------------------------------
    // The rest should only be done occasionally for far away avatars.
    // Set mUpdatePeriod and visible based on distance and other criteria,
    // and flag for impostor update if needed.
    //--------------------------------------------------------------------
    bool needs_update = table_row ? table_row->mNeedsUpdate : computeNeedsUpdate();

    //--------------------------------------------------------------------
    // Early out if does not need update and not self
    // don't early out for your own avatar, as we rely on your animations playing reliably
    // for example, the "turn around" animation when entering customize avatar needs to trigger
    // even when your avatar is offscreen
    //--------------------------------------------------------------------
    if (!needs_update && !isSelf())
    {
        LLAvatarTable::noteActual(this, LLAvatarTable::MODE_SKIP);
        fs_av_importance_ran(0);
        AvScope early_scope(AV_CHAR_EARLY);
        updateMotions(LLCharacter::HIDDEN_UPDATE);
        return false;
    }
    fs_av_importance_ran((!visible && !isSelf()) ? 1 : 2);
    LLAvatarTable::noteActual(this, (!visible && !isSelf()) ? LLAvatarTable::MODE_MATRICES : LLAvatarTable::MODE_POSE);

    //--------------------------------------------------------------------
    // Handle transitions between regular rendering, jellydoll, or invisible.
    // Can trigger skeleton reset or animation changes
    //--------------------------------------------------------------------
    updateOverallAppearance();

    //--------------------------------------------------------------------
    // change animation time quanta based on avatar render load
    //--------------------------------------------------------------------
    // SL-763 the time step quantization does not currently work.
    //updateTimeStep();

    //--------------------------------------------------------------------
    // Update sitting state based on parent and active animation info.
    //--------------------------------------------------------------------
    if (getParent() && !isSitting())
    {
        sitOnObject((LLViewerObject*)getParent());
    }
    else if (!getParent() && isSitting() && !isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED))
    {
        // If we are starting up, motion might be loading
        LLMotion *motionp = mMotionController.findMotion(ANIM_AGENT_SIT_GROUND_CONSTRAINED);
        if (!motionp || !mMotionController.isMotionLoading(motionp))
        {
            getOffObject();
        }
    }

    //--------------------------------------------------------------------
    // create local variables in world coords for region position values
    //--------------------------------------------------------------------
    LLVector3 xyVel = getVelocity();
    xyVel.mV[VZ] = 0.0f;
    F32 speed = xyVel.length();
    // remembering the value here prevents a display glitch if the
    // animation gets toggled during this update.
    bool was_sit_ground_constrained = isMotionActive(ANIM_AGENT_SIT_GROUND_CONSTRAINED);

    //--------------------------------------------------------------------
    // This does a bunch of state updating, including figuring out
    // whether av is in the air, setting mRoot position and rotation
    // In some cases, calls updateOrientation() for a lot of the
    // work
    // --------------------------------------------------------------------
    // <FS> PLAN-avatars.md stage B3d (LL_VK_ROOTJOB: 1 default, 0 inline as before): an avatar that goes into the motion batch has its root position and rotation worked out at the start of its pool job, not here on the main
    // thread: the root only feeds the pose job's world pass (nothing between here and the batch reads it: begin and the hidden motion update do not), and it reads the terrain and writes the avatar's own fields.
    // The viewer's own avatar, animesh, UI avatars, special render modes and an avatar sitting on an object keep the inline form.
    int can_defer_known = -1;
    bool root_in_job = false;
    {
        static const bool root_job_on = !getenv("LL_VK_ROOTJOB") || atoi(getenv("LL_VK_ROOTJOB")) != 0;
        if (root_job_on && !gFSAvTimersOn && sMotionUpdateBatch && !isSelf() && !isControlAvatar() && !isUIAvatar() && mSpecialRenderMode == 0 && mRoot && !(isSitting() && getParent()))
        {
            if (!visible)
            {
                root_in_job = true;         // (the hidden branch below puts it in the batch under the same conditions)
            }
            else
            {
                can_defer_known = canDeferMotionUpdate() ? 1 : 0;
                root_in_job = can_defer_known == 1 && !mMotionController.isPaused();
            }
        }
    }
    if (!root_in_job)
    {
        AvScope root_scope(AV_CHAR_ROOT);
        updateRootPositionAndRotation(agent, speed, was_sit_ground_constrained);
    }

    //-------------------------------------------------------------------------
    // Update character motions
    //-------------------------------------------------------------------------
    // store data relevant to motions
    mSpeed = speed;

    // update animations
    if (!visible && !isSelf()) // NOTE: never do a "hidden update" for self avatar as it interrupts controller processing
    {
        AvScope hidden_scope(AV_CHAR_HIDDEN);
        updateMotions(LLCharacter::HIDDEN_UPDATE);
        if (sMotionUpdateBatch && mSpecialRenderMode == 0 && !isUIAvatar() && mRoot)
        {
            // <FS> nothing to blend, but the skeleton's world matrices still follow the root: worked out in the parallel batch, then the rest of the update completes
            mMotionUpdateDeferred = true;
            mDeferredMatricesOnly = true;
            mDeferredSitGroundConstrained = was_sit_ground_constrained;
            mDeferredVisible = visible;
            mRootInJob = root_in_job;
            mDeferredSpeed = speed;
            sMotionUpdateBatch->push_back(this);
            return visible;
        }
    }
    else if (mSpecialRenderMode == 1) // Animation Preview
    {
        updateMotions(LLCharacter::FORCE_UPDATE);
    }
    else if (can_defer_known >= 0 ? can_defer_known == 1 : canDeferMotionUpdate())
    {
        // Serial part now; the pose blend runs later, possibly on a worker
        // thread, and then completeDeferredMotionUpdate() finishes the frame.
        bool began;
        {
            AvScope begin_scope(AV_CHAR_BEGIN);
            began = beginNormalMotionUpdate();
        }
        if (began)
        {
            mMotionUpdateDeferred = true;
            mDeferredSitGroundConstrained = was_sit_ground_constrained;
            mDeferredVisible = visible;
            mRootInJob = root_in_job;
            mDeferredSpeed = speed;
            sMotionUpdateBatch->push_back(this);
            return visible;
        }
    }
    else
    {
        // Might be better to do HIDDEN_UPDATE if cloud
        if (gFSAvTimersOn)
        {
            gFSAvStatCount[isSelf() ? AV_INL_SELF : (isControlAvatar() ? AV_INL_CONTROL : AV_INL_OTHER)]++;
        }
        AvScope inline_scope(AV_CHAR_INLINE);
        updateMotions(LLCharacter::NORMAL_UPDATE);
    }

    if (root_in_job)
    {
        // (not expected: the avatar was going to join the batch and did not; the root is worked out now, late)
        static std::atomic<U32> late{ 0 };
        if (++late <= 5)
        {
            LL_WARNS("RootJob") << "an avatar planned for the root job did not join the motion batch; root worked out inline, late" << LL_ENDL;
        }
        updateRootPositionAndRotation(agent, speed, was_sit_ground_constrained);
    }
    {
        AvScope post_scope(AV_CHAR_POST);
        updateCharacterPostMotion(was_sit_ground_constrained, visible);
    }

    return visible;
}

//-----------------------------------------------------------------------------
// canDeferMotionUpdate()
// Whether this avatar's pose blend can be run later (and in parallel with other
// avatars') instead of inline in updateCharacter().
//-----------------------------------------------------------------------------
bool LLVOAvatar::canDeferMotionUpdate() const
{
    static const bool self_deferred = !getenv("LL_VK_AV_SELF") || getenv("LL_VK_AV_SELF")[0] != '0';        // <FS> stage d: the own avatar in the batch (LL_VK_AV_SELF=0: inline, as before)
    if (!sMotionUpdateBatch
        || (isSelf() && !self_deferred)     // own avatar: motion stop requests go to the network (kept while the blend runs as a job, made afterwards: flushDeferredStopRequests)
        || isUIAvatar()
        || mSpecialRenderMode != 0
        || !canDeferNormalMotionUpdate())
    {
        return false;
    }

    // A sex change makes updateVisualParams() start/stop motions, which loads
    // assets and must stay on the main thread.
    LLVOAvatar* self = const_cast<LLVOAvatar*>(this);
    const ESex avatar_sex = (self->getVisualParamWeight("male") > 0.5f) ? SEX_MALE : SEX_FEMALE;
    return getSex() == avatar_sex;
}

//-----------------------------------------------------------------------------
// completeDeferredMotionUpdate()
//-----------------------------------------------------------------------------
// <FS> The deferred pose blend, run on a worker thread (see run_avatar_motion_batch). The skeleton's world matrices follow the blend: each joint is worked out from
// its own local transform and its parent's (read only), and every avatar has its own joints, so this is done here, in parallel, instead of serially afterwards in
// updateCharacterPostMotion() (which then finds the matrices clean). The counters of LLJoint are statistics only.
thread_local LLVOAvatar* LLVOAvatar::sBlendJobAvatar = nullptr;

void LLVOAvatar::cleanJointMatricesOnce(U32 frame)
{
    const U32 done = frame & 0x7FFFFFFFu;
    const U32 busy = done | 0x80000000u;
    U32 state = mJointCleanState.load(std::memory_order_acquire);
    while (state != done)
    {
        if (state != busy && mJointCleanState.compare_exchange_weak(state, busy, std::memory_order_acq_rel))
        {
            cleanJointMatrices();
            mJointCleanState.store(done, std::memory_order_release);
            return;
        }
        if (state == busy)
        {
            _mm_pause();
            state = mJointCleanState.load(std::memory_order_acquire);
        }
    }
}

void LLVOAvatar::cleanJointMatrices()
{
    if (mRoot)
    {
        mRoot->updateWorldMatrixChildren();
    }
    for (S32 i = 0; i < mNumCollisionVolumes; ++i)
    {
        if (mCollisionVolumes[i].mDirtyFlags & LLJoint::MATRIX_DIRTY)
        {
            mCollisionVolumes[i].getWorldMatrix();
        }
    }
    for (auto& attachment : mAttachmentPoints)
    {
        if (attachment.second && (attachment.second->mDirtyFlags & LLJoint::MATRIX_DIRTY))
        {
            attachment.second->getWorldMatrix();
        }
    }
}

void LLVOAvatar::runDeferredMotionBlend()
{
    AvImpTimer imp_blend_timer(1, this);
    if (mRootInJob)
    {
        // <FS> stage B3d: the root position and rotation, worked out here (see updateCharacter); the job's own thread, the avatar's own fields and joints
        mRootInJob = false;
        updateRootPositionAndRotation(gAgent, mDeferredSpeed, mDeferredSitGroundConstrained);
        static std::atomic<U64> in_job{ 0 };
        if (++in_job % 200000 == 0)
        {
            LL_INFOS("RootJob") << "root position and rotation worked out in the avatar's pool job: " << in_job.load() << " so far" << LL_ENDL;
        }
    }
    if (fs_av_imp_on())
    {
        fs_av_imp_motions(this, mMotionController.getActiveMotions().size());
    }
    if (mDeferredMatricesOnly)
    {
        if (mRoot)
        {
            mRoot->updateWorldMatrixChildren();
        }
        return;
    }
    LLVOAvatar* saved_job_avatar = sBlendJobAvatar;
    sBlendJobAvatar = this;
    finishNormalMotionUpdate();
    sBlendJobAvatar = saved_job_avatar;
    if (mRoot)
    {
        AvImpTimer imp_world_timer(3, this);        // (region 3: the world matrix walk at the end of the job)
        mRoot->updateWorldMatrixChildren();
    }
    // <FS> PLAN-avatars.md stage B2c, LL_VK_SCALEGUARD=2: with the scale setter not dirtying an unchanged scale, a joint left clean must hold the world matrix a full recompute gives: save the matrices, dirty the whole
    // skeleton, walk again, compare bit for bit
    static const bool verify_clean = getenv("LL_VK_SCALEGUARD") && atoi(getenv("LL_VK_SCALEGUARD")) == 2;
    if (verify_clean && mRoot)
    {
        static std::atomic<U64> checked{ 0 }, stale{ 0 }, jobs{ 0 };
        std::vector<LLJoint*> joints;
        for (LLJoint* bone : mSkeleton)
        {
            if (bone && bone->mUpdateXform)
            {
                joints.push_back(bone);
            }
        }
        for (auto& attachment : mAttachmentPoints)
        {
            if (attachment.second && attachment.second->mUpdateXform)
            {
                joints.push_back(attachment.second);
            }
        }
        std::vector<LLMatrix4a> before(joints.size());
        for (size_t k = 0; k < joints.size(); ++k)
        {
            before[k] = joints[k]->getWorldMatrix4a();
        }
        mRoot->touch(LLJoint::ALL_DIRTY);
        mRoot->updateWorldMatrixChildren();
        for (size_t k = 0; k < joints.size(); ++k)
        {
            ++checked;
            if (memcmp(&before[k], &joints[k]->getWorldMatrix4a(), sizeof(LLMatrix4a)) != 0)
            {
                if (++stale <= 6)
                {
                    LL_WARNS("ScaleGuard") << "joint '" << joints[k]->getName() << "' of " << getFullname() << " was left clean with a stale world matrix" << LL_ENDL;
                }
            }
        }
        if (++jobs % 20000 == 0)
        {
            LL_INFOS("ScaleGuard") << "clean joints checked against a full recompute over " << jobs.load() << " jobs: joints " << checked.load() << ", stale " << stale.load() << LL_ENDL;
        }
    }
}

void LLVOAvatar::completeDeferredMotionUpdate()
{
    if (!mMotionUpdateDeferred)
    {
        return;
    }
    mMotionUpdateDeferred = false;
    mDeferredMatricesOnly = false;

    if (isDead())
    {
        mAttJobDone = false;
        mAttLedger.reset();
        return;
    }

    AvScope done_scope(AV_DEFERRED_DONE);
    AvImpTimer imp_done_timer(2, this);
    { AvSelfScope self_scope(0, this); flushDeferredStopRequests(); }        // <FS> stage d: the motions the own avatar's blend stopped
    // (the blend job cleaned the skeleton's world matrices: the walk of updateCharacterPostMotion only runs when something dirtied a joint since)
    { AvSelfScope self_scope(1, this); LL_CPU_ZONE("av post: updateCharacterPostMotion"); updateCharacterPostMotion(mDeferredSitGroundConstrained, mDeferredVisible, LLJoint::sTouchSerial); }
    { AvSelfScope self_scope(2, this); idleUpdatePostCharacter(mDeferredDetailedUpdate); }
}

//-----------------------------------------------------------------------------
// updateCharacterPostMotion()
// Everything updateCharacter() does after the motions have been updated.
//-----------------------------------------------------------------------------
void LLVOAvatar::updateCharacterPostMotion(bool was_sit_ground_constrained, bool visible, U32 clean_serial)
{
    // Special handling for sitting on ground.
    if (!getParent() && (isSitting() || was_sit_ground_constrained))
    {

        F32 off_z = (F32)LLVector3d(getHoverOffset()).mdV[VZ];
        if (off_z != 0.0f)
        {
            LLVector3 pos = mRoot->getWorldPosition();
            pos.mV[VZ] += off_z;
            mRoot->touch();
            // SL-315
            mRoot->setWorldPosition(pos);
        }
    }

    // update head position
    {
        AvScope s(AV_PM_HEADOFFSET);
        updateHeadOffset();
    }

    // Generate footstep sounds when feet hit the ground
    {
        AvScope s(AV_PM_FOOTSTEP);
        updateFootstepSounds();
    }

    // Update child joints as needed.
    if (clean_serial != LLJoint::sTouchSerial)
    {
        AvScope s(AV_PM_MATRIX);
        mRoot->updateWorldMatrixChildren();
    }

    if (visible)
    {
        // System avatar mesh vertices need to be reskinned.
        mNeedsSkin = true;
    }
}

//-----------------------------------------------------------------------------
// updateHeadOffset()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateHeadOffset()
{
    // since we only care about Z, just grab one of the eyes
    LLVector3 midEyePt = mEyeLeftp->getWorldPosition();
    midEyePt -= mDrawable.notNull() ? mDrawable->getWorldPosition() : mRoot->getWorldPosition();
    midEyePt.mV[VZ] = llmax(-mPelvisToFoot + LLViewerCamera::getInstance()->getNear(), midEyePt.mV[VZ]);

    if (mDrawable.notNull())
    {
        midEyePt = midEyePt * ~mDrawable->getWorldRotation();
    }
    if (isSitting())
    {
        mHeadOffset = midEyePt;
    }
    else
    {
        F32 u = llmax(0.f, HEAD_MOVEMENT_AVG_TIME - (1.f / gFPSClamped));
        mHeadOffset = lerp(midEyePt, mHeadOffset,  u);
    }
}

void LLVOAvatar::debugBodySize() const
{
    LLVector3 pelvis_scale = mPelvisp->getScale();

    // some of the joints have not been cached
    LLVector3 skull = mSkullp->getPosition();
    LL_DEBUGS("Avatar") << "skull pos " << skull << LL_ENDL;
    //LLVector3 skull_scale = mSkullp->getScale();

    LLVector3 neck = mNeckp->getPosition();
    LLVector3 neck_scale = mNeckp->getScale();
    LL_DEBUGS("Avatar") << "neck pos " << neck << " neck_scale " << neck_scale << LL_ENDL;

    LLVector3 chest = mChestp->getPosition();
    LLVector3 chest_scale = mChestp->getScale();
    LL_DEBUGS("Avatar") << "chest pos " << chest << " chest_scale " << chest_scale << LL_ENDL;

    // the rest of the joints have been cached
    LLVector3 head = mHeadp->getPosition();
    LLVector3 head_scale = mHeadp->getScale();
    LL_DEBUGS("Avatar") << "head pos " << head << " head_scale " << head_scale << LL_ENDL;

    LLVector3 torso = mTorsop->getPosition();
    LLVector3 torso_scale = mTorsop->getScale();
    LL_DEBUGS("Avatar") << "torso pos " << torso << " torso_scale " << torso_scale << LL_ENDL;

    LLVector3 hip = mHipLeftp->getPosition();
    LLVector3 hip_scale = mHipLeftp->getScale();
    LL_DEBUGS("Avatar") << "hip pos " << hip << " hip_scale " << hip_scale << LL_ENDL;

    LLVector3 knee = mKneeLeftp->getPosition();
    LLVector3 knee_scale = mKneeLeftp->getScale();
    LL_DEBUGS("Avatar") << "knee pos " << knee << " knee_scale " << knee_scale << LL_ENDL;

    LLVector3 ankle = mAnkleLeftp->getPosition();
    LLVector3 ankle_scale = mAnkleLeftp->getScale();
    LL_DEBUGS("Avatar") << "ankle pos " << ankle << " ankle_scale " << ankle_scale << LL_ENDL;

    LLVector3 foot  = mFootLeftp->getPosition();
    LL_DEBUGS("Avatar") << "foot pos " << foot << LL_ENDL;

    F32 new_offset = (const_cast<LLVOAvatar*>(this))->getVisualParamWeight(AVATAR_HOVER);
    LL_DEBUGS("Avatar") << "new_offset " << new_offset << LL_ENDL;

    F32 new_pelvis_to_foot = hip.mV[VZ] * pelvis_scale.mV[VZ] -
        knee.mV[VZ] * hip_scale.mV[VZ] -
        ankle.mV[VZ] * knee_scale.mV[VZ] -
        foot.mV[VZ] * ankle_scale.mV[VZ];
    LL_DEBUGS("Avatar") << "new_pelvis_to_foot " << new_pelvis_to_foot << LL_ENDL;

    LLVector3 new_body_size;
    new_body_size.mV[VZ] = new_pelvis_to_foot +
                       // the sqrt(2) correction below is an approximate
                       // correction to get to the top of the head
                       F_SQRT2 * (skull.mV[VZ] * head_scale.mV[VZ]) +
                       head.mV[VZ] * neck_scale.mV[VZ] +
                       neck.mV[VZ] * chest_scale.mV[VZ] +
                       chest.mV[VZ] * torso_scale.mV[VZ] +
                       torso.mV[VZ] * pelvis_scale.mV[VZ];

    // TODO -- measure the real depth and width
    new_body_size.mV[VX] = DEFAULT_AGENT_DEPTH;
    new_body_size.mV[VY] = DEFAULT_AGENT_WIDTH;

    LL_DEBUGS("Avatar") << "new_body_size " << new_body_size << LL_ENDL;
}

//------------------------------------------------------------------------
// postPelvisSetRecalc
//------------------------------------------------------------------------
void LLVOAvatar::postPelvisSetRecalc()
{
    mRoot->updateWorldMatrixChildren();
    computeBodySize();
    dirtyMesh(2);
}
//------------------------------------------------------------------------
// updateVisibility()
//------------------------------------------------------------------------
void LLVOAvatar::updateVisibility()
{
    bool visible = false;

    if (mIsDummy)
    {
        visible = false;
    }
    else if (mDrawable.isNull())
    {
        visible = false;
    }
    else
    {
        if (!mDrawable->getSpatialGroup() || mDrawable->getSpatialGroup()->isVisible())
        {
            visible = true;
        }

        if (isSelf())
        {
            if (!gAgentWearables.areWearablesLoaded())
            {
                visible = false;
            }
        }
        else if (!mFirstAppearanceMessageReceived)
        {
            visible = false;
        }

        if (sDebugInvisible)
        {
            if (getNVPair("FirstName"))// <FS:Beq/> Fix more Mac whining.
            {
                LL_DEBUGS("Avatar") << avString() << " updating visibility" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Avatar " << this << " updating visiblity" << LL_ENDL;
            }

            if (visible)
            {
                LL_INFOS() << "Visible" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Not visible" << LL_ENDL;
            }

            /*if (avatar_in_frustum)
            {
                LL_INFOS() << "Avatar in frustum" << LL_ENDL;
            }
            else
            {
                LL_INFOS() << "Avatar not in frustum" << LL_ENDL;
            }*/

            /*if (LLViewerCamera::getInstance()->sphereInFrustum(sel_pos_agent, 2.0f))
            {
                LL_INFOS() << "Sel pos visible" << LL_ENDL;
            }
            if (LLViewerCamera::getInstance()->sphereInFrustum(wrist_right_pos_agent, 0.2f))
            {
                LL_INFOS() << "Wrist pos visible" << LL_ENDL;
            }
            if (LLViewerCamera::getInstance()->sphereInFrustum(getPositionAgent(), getMaxScale()*2.f))
            {
                LL_INFOS() << "Agent visible" << LL_ENDL;
            }*/
            LL_INFOS() << "PA: " << getPositionAgent() << LL_ENDL;
            /*LL_INFOS() << "SPA: " << sel_pos_agent << LL_ENDL;
            LL_INFOS() << "WPA: " << wrist_right_pos_agent << LL_ENDL;*/
            for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
                 iter != mAttachmentPoints.end();
                 ++iter)
            {
                LLViewerJointAttachment* attachment = iter->second;

                // <FS:Ansariel> Possible crash fix
                if (!attachment)
                {
                    continue;
                }
                // </FS:Ansariel>

                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                     attachment_iter != attachment->mAttachedObjects.end();
                     ++attachment_iter)
                {
                    if (LLViewerObject *attached_object = attachment_iter->get())
                    {
                        if(attached_object->mDrawable->isVisible())
                        {
                            LL_INFOS() << attachment->getName() << " visible" << LL_ENDL;
                        }
                        else
                        {
                            LL_INFOS() << attachment->getName() << " not visible at " << mDrawable->getWorldPosition() << " and radius " << mDrawable->getRadius() << LL_ENDL;
                        }
                    }
                }
            }
        }
    }

    if (!visible && mVisible)
    {
        mMeshInvisibleTime.reset();
    }

    if (visible)
    {
        if (!mMeshValid)
        {
            restoreMeshData();
        }
    }
    else
    {
        if (mMeshValid &&
            (isControlAvatar() || mMeshInvisibleTime.getElapsedTimeF32() > TIME_BEFORE_MESH_CLEANUP))
        {
            releaseMeshData();
        }
    }

    if (visible != mVisible)
    {
        LL_DEBUGS("AvatarRender") << "visible was " << mVisible << " now " << visible << LL_ENDL;
    }

    mVisible = visible;

    // <FS> the visibility rank (RenderAvatarMaxNonImpostors picks the first of it as full avatars, the rest are impostors updating every 32 to 64 frames) is by screen size, so in a crowd a standing
    // avatar at 18 m lost to dancers at 30 m and played as a slideshow until the camera zoomed in (Adam, 2026-11-05). Within FSImpostorNearDistance the rank is by distance ahead of everyone else:
    // nobody that near is an impostor unless more than the cap are that near, and then the farthest of them are. Beyond it the screen size decides as before.
    static LLCachedControl<F32> near_distance(gSavedSettings, "FSImpostorNearDistance", 25.f);
    F32 preference = visible ? getPixelArea() : 0.f;
    if (visible && !isSelf() && near_distance > 0.f && mDrawable.notNull())
    {
        const F32 distance = mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale();
        if (distance < near_distance)
        {
            preference = 1.0e12f + (near_distance - distance) * 1.0e6f;      // above any pixel area, nearest first
        }
    }
    mVisibilityPreference = preference;
}

// private
bool LLVOAvatar::shouldAlphaMask()
{
    const bool should_alpha_mask = !LLDrawPoolAlpha::sShowDebugAlpha // Don't alpha mask if "Highlight Transparent" checked
                            && !LLDrawPoolAvatar::sSkipTransparent;

    return should_alpha_mask;

}

//-----------------------------------------------------------------------------
// renderSkinned()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::renderSkinned()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    U32 num_indices = 0;

    if (!mIsBuilt)
    {
        return num_indices;
    }

    if (mDrawable.isNull())
    {
        return num_indices;
    }

    LLFace* face = mDrawable->getFace(0);

    bool needs_rebuild = !face || !face->getVertexBuffer() || mDrawable->isState(LLDrawable::REBUILD_GEOMETRY);

    if (needs_rebuild || mDirtyMesh)
    {   //LOD changed or new mesh created, allocate new vertex buffer if needed
        if (needs_rebuild || mDirtyMesh >= 2 || mVisibilityRank <= 4)
        {
            updateMeshData();
        }
    }

    if (LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_AVATAR) <= 0)
    {
        if (mNeedsSkin)
        {
            //generate animated mesh
            LLViewerJoint* lower_mesh = getViewerJoint(MESH_ID_LOWER_BODY);
            LLViewerJoint* upper_mesh = getViewerJoint(MESH_ID_UPPER_BODY);
            LLViewerJoint* skirt_mesh = getViewerJoint(MESH_ID_SKIRT);
            LLViewerJoint* eyelash_mesh = getViewerJoint(MESH_ID_EYELASH);
            LLViewerJoint* head_mesh = getViewerJoint(MESH_ID_HEAD);
            LLViewerJoint* hair_mesh = getViewerJoint(MESH_ID_HAIR);

            if(upper_mesh)
            {
                upper_mesh->updateJointGeometry();
            }
            if (lower_mesh)
            {
                lower_mesh->updateJointGeometry();
            }

            if( isWearingWearableType( LLWearableType::WT_SKIRT ) )
            {
                if(skirt_mesh)
                {
                    skirt_mesh->updateJointGeometry();
                }
            }

            if (!isSelf() || gAgent.needsRenderHead() || LLPipeline::sShadowRender)
            {
                if(eyelash_mesh)
                {
                    eyelash_mesh->updateJointGeometry();
                }
                if(head_mesh)
                {
                    head_mesh->updateJointGeometry();
                }
                if(hair_mesh)
                {
                    hair_mesh->updateJointGeometry();
                }
            }
            mNeedsSkin = false;
            mLastSkinTime = gFrameTimeSeconds;

            LLFace * face = mDrawable->getFace(0);
            if (face)
            {
                LLVertexBuffer* vb = face->getVertexBuffer();
                if (vb)
                {
                    vb->unmapBuffer();
                }
            }
        }
    }
    else
    {
        mNeedsSkin = false;
    }

    if (sDebugInvisible)
    {
        LLNameValue* firstname = getNVPair("FirstName");
        if (firstname)
        {
            LL_DEBUGS("Avatar") << avString() << " in render" << LL_ENDL;
        }
        else
        {
            LL_INFOS() << "Avatar " << this << " in render" << LL_ENDL;
        }
        if (!mIsBuilt)
        {
            LL_INFOS() << "Not built!" << LL_ENDL;
        }
        else if (!gAgent.needsRenderAvatar())
        {
            LL_INFOS() << "Doesn't need avatar render!" << LL_ENDL;
        }
        else
        {
            LL_INFOS() << "Rendering!" << LL_ENDL;
        }
    }

    if (!mIsBuilt)
    {
        return num_indices;
    }

    if (isSelf() && !gAgent.needsRenderAvatar())
    {
        return num_indices;
    }

    //--------------------------------------------------------------------
    // render all geometry attached to the skeleton
    //--------------------------------------------------------------------

        bool first_pass = true;
        if (!LLDrawPoolAvatar::sSkipOpaque)
        {
            if (isUIAvatar() && mIsDummy)
            {
                LLViewerJoint* hair_mesh = getViewerJoint(MESH_ID_HAIR);
                if (hair_mesh)
                {
                    num_indices += hair_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
                }
                first_pass = false;
            }
            if (!isSelf() || gAgent.needsRenderHead() || LLPipeline::sShadowRender)
            {

                if (isTextureVisible(TEX_HEAD_BAKED) || (getOverallAppearance() == AOA_JELLYDOLL && !isControlAvatar()) || isUIAvatar())
                {
                    LLViewerJoint* head_mesh = getViewerJoint(MESH_ID_HEAD);
                    if (head_mesh)
                    {
                        num_indices += head_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
                    }
                    first_pass = false;
                }
            }
            if (isTextureVisible(TEX_UPPER_BAKED) || (getOverallAppearance() == AOA_JELLYDOLL && !isControlAvatar()) || isUIAvatar())
            {
                LLViewerJoint* upper_mesh = getViewerJoint(MESH_ID_UPPER_BODY);
                if (upper_mesh)
                {
                    num_indices += upper_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
                }
                first_pass = false;
            }

            if (isTextureVisible(TEX_LOWER_BAKED) || (getOverallAppearance() == AOA_JELLYDOLL && !isControlAvatar()) || isUIAvatar())
            {
                LLViewerJoint* lower_mesh = getViewerJoint(MESH_ID_LOWER_BODY);
                if (lower_mesh)
                {
                    num_indices += lower_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
                }
                first_pass = false;
            }
        }

        if (!LLDrawPoolAvatar::sSkipTransparent || LLPipeline::sImpostorRender)
        {
            LLRenderState::BlendScope blend(!mIsDummy);
            num_indices += renderTransparent(first_pass);
        }

    return num_indices;
}

U32 LLVOAvatar::renderTransparent(bool first_pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR; // <FS:Beq/> Tracy accounting for render tracking
    U32 num_indices = 0;
    if( isWearingWearableType( LLWearableType::WT_SKIRT ) && (isUIAvatar() || isTextureVisible(TEX_SKIRT_BAKED)) )
    {
        gRender.flush();
        LLViewerJoint* skirt_mesh = getViewerJoint(MESH_ID_SKIRT);
        if (skirt_mesh)
        {
            num_indices += skirt_mesh->render(mAdjustedPixelArea, false);
        }
        first_pass = false;
        gRender.flush();
    }

    if (!isSelf() || gAgent.needsRenderHead() || LLPipeline::sShadowRender)
    {
        if (LLPipeline::sImpostorRender)
        {
            gRender.flush();
        }

        if (isTextureVisible(TEX_HEAD_BAKED))
        {
            LLViewerJoint* eyelash_mesh = getViewerJoint(MESH_ID_EYELASH);
            if (eyelash_mesh)
            {
                num_indices += eyelash_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
            }
            first_pass = false;
        }
        if (isTextureVisible(TEX_HAIR_BAKED) && (getOverallAppearance() != AOA_JELLYDOLL))
        {
            LLViewerJoint* hair_mesh = getViewerJoint(MESH_ID_HAIR);
            if (hair_mesh)
            {
                num_indices += hair_mesh->render(mAdjustedPixelArea, first_pass, mIsDummy);
            }
            first_pass = false;
        }
        if (LLPipeline::sImpostorRender)
        {
            gRender.flush();
        }
    }

    return num_indices;
}

//-----------------------------------------------------------------------------
// renderRigid()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::renderRigid()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR; // <FS:Beq/> Tracy accounting for render tracking
    U32 num_indices = 0;

    if (!mIsBuilt)
    {
        return 0;
    }

    if (isSelf() && (!gAgent.needsRenderAvatar() || !gAgent.needsRenderHead()))
    {
        return 0;
    }

    if (isTextureVisible(TEX_EYES_BAKED) || (getOverallAppearance() == AOA_JELLYDOLL && !isControlAvatar()) || isUIAvatar())
    {
        LLViewerJoint* eyeball_left = getViewerJoint(MESH_ID_EYEBALL_LEFT);
        LLViewerJoint* eyeball_right = getViewerJoint(MESH_ID_EYEBALL_RIGHT);
        if (eyeball_left)
        {
            num_indices += eyeball_left->render(mAdjustedPixelArea, true, mIsDummy);
        }
        if(eyeball_right)
        {
            num_indices += eyeball_right->render(mAdjustedPixelArea, true, mIsDummy);
        }
    }

    return num_indices;
}

U32 LLVOAvatar::renderImpostor(LLColor4U color, S32 diffuse_channel)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR; // <FS:Beq/> Tracy accounting for render tracking
    if (!mImpostor.isComplete())
    {
        return 0;
    }

    LLVector3 pos(getRenderPosition()+mImpostorOffset);
    LLVector3 at = (pos - LLViewerCamera::getInstance()->getOrigin());
    at.normalize();
    LLVector3 left = LLViewerCamera::getInstance()->getUpAxis() % at;
    LLVector3 up = at%left;

    left *= mImpostorDim.mV[0];
    up *= mImpostorDim.mV[1];

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_IMPOSTORS))
    {
        LLRenderState::BlendScope blend(true);
        gRender.setSceneBlendType(LLRender::BT_ADD);
        gRender.getTexUnit(diffuse_channel)->bindWhite();

        gRender.begin(LLRender::LINES);
        gRender.color4f(1.f,1.f,1.f,1.f);
        F32 thickness = llmax(F32(5.0f-5.0f*(gFrameTimeSeconds-mLastImpostorUpdateFrameTime)),1.0f);
        gRender.setLineWidth(thickness); // <FS> Line width OGL core profile fix by Rye Mutt
        gRender.vertex3fv((pos+left-up).mV);
        gRender.vertex3fv((pos-left-up).mV);
        gRender.vertex3fv((pos-left-up).mV);
        gRender.vertex3fv((pos-left+up).mV);
        gRender.vertex3fv((pos-left+up).mV);
        gRender.vertex3fv((pos+left+up).mV);
        gRender.vertex3fv((pos+left+up).mV);
        gRender.vertex3fv((pos+left-up).mV);
        gRender.end();
        gRender.flush();
    }
    {
    gRender.flush();

    gRender.color4ubv(color.mV);
    gRender.getTexUnit(diffuse_channel)->bind(&mImpostor);
    gRender.begin(LLRender::TRIANGLES);
    {
        gRender.texCoord2f(0.f, 0.f);
        gRender.vertex3fv((pos + left - up).mV);
        gRender.texCoord2f(1.f, 0.f);
        gRender.vertex3fv((pos - left - up).mV);
        gRender.texCoord2f(1.f, 1.f);
        gRender.vertex3fv((pos - left + up).mV);

        gRender.texCoord2f(0.f, 0.f);
        gRender.vertex3fv((pos + left - up).mV);
        gRender.texCoord2f(1.f, 1.f);
        gRender.vertex3fv((pos - left + up).mV);
        gRender.texCoord2f(0.f, 1.f);
        gRender.vertex3fv((pos + left + up).mV);
    }
    gRender.end();
    gRender.flush();
    }

    return 6;
}

bool LLVOAvatar::allTexturesCompletelyDownloaded(std::set<LLUUID>& ids) const
{
    for (std::set<LLUUID>::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
        LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
        if (imagep && imagep->getDiscardLevel()!=0)
        {
            return false;
        }
    }
    return true;
}

bool LLVOAvatar::allLocalTexturesCompletelyDownloaded() const
{
    std::set<LLUUID> local_ids;
    collectLocalTextureUUIDs(local_ids);
    return allTexturesCompletelyDownloaded(local_ids);
}

bool LLVOAvatar::allBakedTexturesCompletelyDownloaded() const
{
    std::set<LLUUID> baked_ids;
    collectBakedTextureUUIDs(baked_ids);
    return allTexturesCompletelyDownloaded(baked_ids);
}

std::string LLVOAvatar::bakedTextureOriginInfo()
{
    std::string result;

    std::set<LLUUID> baked_ids;
    collectBakedTextureUUIDs(baked_ids);
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        ETextureIndex texture_index = mBakedTextureDatas[i].mTextureIndex;
        LLViewerFetchedTexture *imagep =
            LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
        if (!imagep ||
            imagep->getID() == IMG_DEFAULT ||
            imagep->getID() == IMG_DEFAULT_AVATAR)

        {
            result += "-";
        }
        else
        {
            bool has_url = false, has_host = false;
            if (!imagep->getUrl().empty())
            {
                has_url = true;
            }
            if (imagep->getTargetHost().isOk())
            {
                has_host = true;
            }
            S32 discard = imagep->getDiscardLevel();
            if (has_url && !has_host) result += discard ? "u" : "U"; // server-bake texture with url
            else if (has_host && !has_url) result += discard ? "h" : "H"; // old-style texture on sim
            else if (has_host && has_url) result += discard ? "x" : "X"; // both origins?
            else if (!has_host && !has_url) result += discard ? "n" : "N"; // no origin?
            if (discard != 0)
            {
                result += llformat("(%d/%d)",discard,imagep->getDesiredDiscardLevel());
            }
        }

    }
    return result;
}

S32Bytes LLVOAvatar::totalTextureMemForUUIDS(std::set<LLUUID>& ids)
{
    S32Bytes result(0);
    for (std::set<LLUUID>::const_iterator it = ids.begin(); it != ids.end(); ++it)
    {
        LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
        if (imagep)
        {
            result += imagep->getTextureMemory();
        }
    }
    return result;
}

void LLVOAvatar::collectLocalTextureUUIDs(std::set<LLUUID>& ids) const
{
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLWearableType::EType wearable_type = LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)texture_index);
        U32 num_wearables = gAgentWearables.getWearableCount(wearable_type);

        LLViewerFetchedTexture *imagep = NULL;
        for (U32 wearable_index = 0; wearable_index < num_wearables; wearable_index++)
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index, wearable_index), true);
            if (imagep)
            {
                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)texture_index);
                if (texture_dict && texture_dict->mIsLocalTexture)
                {
                    ids.insert(imagep->getID());
                }
            }
        }
    }
    ids.erase(IMG_DEFAULT);
    ids.erase(IMG_DEFAULT_AVATAR);
    ids.erase(IMG_INVISIBLE);
}

void LLVOAvatar::collectBakedTextureUUIDs(std::set<LLUUID>& ids) const
{
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLViewerFetchedTexture *imagep = NULL;
        if (isIndexBakedTexture((ETextureIndex) texture_index))
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
            if (imagep)
            {
                ids.insert(imagep->getID());
            }
        }
    }
    ids.erase(IMG_DEFAULT);
    ids.erase(IMG_DEFAULT_AVATAR);
    ids.erase(IMG_INVISIBLE);
}

void LLVOAvatar::collectTextureUUIDs(std::set<LLUUID>& ids)
{
    collectLocalTextureUUIDs(ids);
    collectBakedTextureUUIDs(ids);
}

void LLVOAvatar::releaseOldTextures()
{
    S32Bytes current_texture_mem;

    // Any textures that we used to be using but are no longer using should no longer be flagged as "NO_DELETE"
    std::set<LLUUID> baked_texture_ids;
    collectBakedTextureUUIDs(baked_texture_ids);
    S32Bytes new_baked_mem = totalTextureMemForUUIDS(baked_texture_ids);

    std::set<LLUUID> local_texture_ids;
    collectLocalTextureUUIDs(local_texture_ids);
    //S32 new_local_mem = totalTextureMemForUUIDS(local_texture_ids);

    std::set<LLUUID> new_texture_ids;
    new_texture_ids.insert(baked_texture_ids.begin(),baked_texture_ids.end());
    new_texture_ids.insert(local_texture_ids.begin(),local_texture_ids.end());
    S32Bytes new_total_mem = totalTextureMemForUUIDS(new_texture_ids);

    //S32 old_total_mem = totalTextureMemForUUIDS(mTextureIDs);
    //LL_DEBUGS("Avatar") << getFullname() << " old_total_mem: " << old_total_mem << " new_total_mem (L/B): " << new_total_mem << " (" << new_local_mem <<", " << new_baked_mem << ")" << LL_ENDL;
    if (!isSelf() && new_total_mem > new_baked_mem)
    {
            LL_WARNS() << "extra local textures stored for non-self av" << LL_ENDL;
    }
    for (std::set<LLUUID>::iterator it = mTextureIDs.begin(); it != mTextureIDs.end(); ++it)
    {
        if (new_texture_ids.find(*it) == new_texture_ids.end())
        {
            LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
            if (imagep)
            {
                current_texture_mem += imagep->getTextureMemory();
                if (imagep->getTextureState() == LLBaseTexture::NO_DELETE)
                {
                    // This will allow the texture to be deleted if not in use.
                    imagep->forceActive();

                    // This resets the clock to texture being flagged
                    // as unused, preventing the texture from being
                    // deleted immediately. If other avatars or
                    // objects are using it, it can still be flagged
                    // no-delete by them.
                    imagep->forceUpdateBindStats();
                }
            }
        }
    }
    mTextureIDs = new_texture_ids;
}

void LLVOAvatar::updateTextures()
{
    releaseOldTextures();

    bool render_avatar = true;

    if (mIsDummy)
    {
        return;
    }

    if( isSelf() )
    {
        render_avatar = true;
    }
    else
    {
        if(!isVisible())
        {
            return ;//do not update for invisible avatar.
        }

        render_avatar = !mCulled; //visible and not culled.
    }

    std::vector<bool> layer_baked;
    // GL NOT ACTIVE HERE - *TODO
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        layer_baked.push_back(isTextureDefined(mBakedTextureDatas[i].mTextureIndex));
        // bind the texture so that they'll be decoded slightly
        // inefficient, we can short-circuit this if we have to
        if (render_avatar && !gDeviceCaps.mIsDisabled)
        {
            if (layer_baked[i] && !mBakedTextureDatas[i].mIsLoaded)
            {
                gRender.getTexUnit(0)->bind(getImage( mBakedTextureDatas[i].mTextureIndex, 0 ));
            }
        }
    }

    mMaxPixelArea = 0.f;
    mMinPixelArea = 99999999.f;
    mHasGrey = false; // debug
    for (U32 texture_index = 0; texture_index < getNumTEs(); texture_index++)
    {
        LLWearableType::EType wearable_type = LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)texture_index);
        U32 num_wearables = gAgentWearables.getWearableCount(wearable_type);
        const LLTextureEntry *te = getTE(texture_index);

        // getTE can return 0.
        // Not sure yet why it does, but of course it crashes when te->mScale? gets used.
        // Put safeguard in place so this corner case get better handling and does not result in a crash.
        F32 texel_area_ratio = 1.0f;
        if( te )
        {
            texel_area_ratio = fabs(te->mScaleS * te->mScaleT);
        }
        else
        {
            LL_WARNS() << "getTE( " << texture_index << " ) returned 0" <<LL_ENDL;
        }

        LLViewerFetchedTexture *imagep = NULL;
        for (U32 wearable_index = 0; wearable_index < num_wearables; wearable_index++)
        {
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index, wearable_index), true);
            if (imagep)
            {
                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)texture_index);
                const EBakedTextureIndex baked_index = texture_dict ? texture_dict->mBakedTextureIndex : EBakedTextureIndex::BAKED_NUM_INDICES;
                if (texture_dict && texture_dict->mIsLocalTexture)
                {
                    addLocalTextureStats((ETextureIndex)texture_index, imagep, texel_area_ratio, render_avatar, mBakedTextureDatas[baked_index].mIsUsed);
                }
            }
        }
        if (isIndexBakedTexture((ETextureIndex) texture_index) && render_avatar)
        {
            const S32 boost_level = getAvatarBakedBoostLevel();
            imagep = LLViewerTextureManager::staticCastToFetchedTexture(getImage(texture_index,0), true);
            addBakedTextureStats( imagep, mPixelArea, texel_area_ratio, boost_level );
            // <FS:Ansariel> [Legacy Bake]
            // Spam if this is a baked texture, not set to default image, without valid host info
            if (isIndexBakedTexture((ETextureIndex)texture_index)
                && imagep->getID() != IMG_DEFAULT_AVATAR
                && imagep->getID() != IMG_INVISIBLE
                && !isUsingServerBakes()
                && !imagep->getTargetHost().isOk())
            {
                LL_WARNS_ONCE("Texture") << "LLVOAvatar::updateTextures No host for texture "
                                         << imagep->getID() << " for avatar "
                                         << (isSelf() ? "<myself>" : getID().asString())
                                         << " on host " << getRegion()->getHost() << LL_ENDL;
            }
            // </FS:Ansariel> [Legacy Bake]
        }
    }

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_TEXTURE_AREA))
    {
        setDebugText(llformat("%4.0f:%4.0f", (F32) sqrt(mMinPixelArea),(F32) sqrt(mMaxPixelArea)));
    }
}


void LLVOAvatar::addLocalTextureStats( ETextureIndex idx, LLViewerFetchedTexture* imagep,
                                       F32 texel_area_ratio, bool render_avatar, bool covered_by_baked)
{
    // No local texture stats for non-self avatars
    return;
}

const S32 MAX_TEXTURE_UPDATE_INTERVAL = 64 ; //need to call updateTextures() at least every 32 frames.
const S32 MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL = S32_MAX ; //frames
void LLVOAvatar::checkTextureLoading()
{
    static const F32 MAX_INVISIBLE_WAITING_TIME = 15.f ; //seconds

    bool pause = !isVisible() ;
    if(!pause)
    {
        mInvisibleTimer.reset() ;
    }
    if(mLoadedCallbacksPaused == pause)
    {
        if (!pause && mFirstFullyVisible && mLoadedCallbackTextures < mCallbackTextureList.size())
        {
            // We still need to update 'loaded' textures count to decide on 'cloud' visibility
            // Alternatively this can be done on TextureLoaded callbacks, but is harder to properly track
            mLoadedCallbackTextures = 0;
            for (LLLoadedCallbackEntry::source_callback_list_t::iterator iter = mCallbackTextureList.begin();
                iter != mCallbackTextureList.end(); ++iter)
            {
                LLViewerFetchedTexture* tex = gTextureList.findImage(*iter);
                if (tex && (tex->getDiscardLevel() >= 0 || tex->isMissingAsset()))
                {
                    mLoadedCallbackTextures++;
                }
            }
        }
        return ;
    }

    if(mCallbackTextureList.empty()) //when is self or no callbacks. Note: this list for self is always empty.
    {
        mLoadedCallbacksPaused = pause ;
        mLoadedCallbackTextures = 0;
        return ; //nothing to check.
    }

    if(pause && mInvisibleTimer.getElapsedTimeF32() < MAX_INVISIBLE_WAITING_TIME)
    {
        return ; //have not been invisible for enough time.
    }

    mLoadedCallbackTextures = pause ? static_cast<S32>(mCallbackTextureList.size()) : 0;

    for(LLLoadedCallbackEntry::source_callback_list_t::iterator iter = mCallbackTextureList.begin();
        iter != mCallbackTextureList.end(); ++iter)
    {
        LLViewerFetchedTexture* tex = gTextureList.findImage(*iter) ;
        if(tex)
        {
            if(pause)//pause texture fetching.
            {
                tex->pauseLoadedCallbacks(&mCallbackTextureList) ;

                //set to terminate texture fetching after MAX_TEXTURE_UPDATE_INTERVAL frames.
                tex->setMaxVirtualSizeResetInterval(MAX_TEXTURE_UPDATE_INTERVAL);
                tex->resetMaxVirtualSizeResetCounter() ;
            }
            else//unpause
            {
                static const F32 START_AREA = 100.f ;

                tex->unpauseLoadedCallbacks(&mCallbackTextureList) ;
                tex->addTextureStats(START_AREA); //jump start the fetching again

                // technically shouldn't need to account for missing, but callback might not have happened yet
                if (tex->getDiscardLevel() >= 0 || tex->isMissingAsset())
                {
                    mLoadedCallbackTextures++; // consider it loaded (we have at least some data)
                }
            }
        }
    }

    if(!pause)
    {
        updateTextures() ; //refresh texture stats.
    }
    mLoadedCallbacksPaused = pause ;
    return ;
}

const F32  SELF_ADDITIONAL_PRI = 0.75f ;
void LLVOAvatar::addBakedTextureStats( LLViewerFetchedTexture* imagep, F32 pixel_area, F32 texel_area_ratio, S32 boost_level)
{
    //Note:
    //if this function is not called for the last MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL frames,
    //the texture pipeline will stop fetching this texture.

    imagep->resetTextureStats();
    imagep->setMaxVirtualSizeResetInterval(MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL);
    imagep->resetMaxVirtualSizeResetCounter() ;

    mMaxPixelArea = llmax(pixel_area, mMaxPixelArea);
    mMinPixelArea = llmin(pixel_area, mMinPixelArea);
    imagep->addTextureStats(pixel_area / texel_area_ratio);
    imagep->setBoostLevel(boost_level);
}

//virtual
void LLVOAvatar::setImage(const U8 te, LLViewerTexture *imagep, const U32 index)
{
    setTEImage(te, imagep);
}

//virtual
LLViewerTexture* LLVOAvatar::getImage(const U8 te, const U32 index) const
{
    return getTEImage(te);
}
//virtual
const LLTextureEntry* LLVOAvatar::getTexEntry(const U8 te_num) const
{
    return getTE(te_num);
}

//virtual
void LLVOAvatar::setTexEntry(const U8 index, const LLTextureEntry &te)
{
    setTE(index, te);
}

const std::string LLVOAvatar::getImageURL(const U8 te, const LLUUID &uuid)
{
    llassert(isIndexBakedTexture(ETextureIndex(te)));
    std::string url = "";
    // <FS:Ansariel> [Legacy Bake]
    if (isUsingServerBakes())
    {
    // </FS:Ansariel> [Legacy Bake]
    const std::string& appearance_service_url = LLAppearanceMgr::instance().getAppearanceServiceURL();
    if (appearance_service_url.empty())
    {
        // Probably a server-side issue if we get here:
        LL_WARNS() << "AgentAppearanceServiceURL not set - Baked texture requests will fail" << LL_ENDL;
        return url;
    }

    const LLAvatarAppearanceDictionary::TextureEntry* texture_entry = LLAvatarAppearance::getDictionary()->getTexture((ETextureIndex)te);
    if (texture_entry != NULL)
    {
        url = appearance_service_url + "texture/" + getID().asString() + "/" + texture_entry->mDefaultImageName + "/" + uuid.asString();
        //LL_INFOS() << "baked texture url: " << url << LL_ENDL;
    }
    // <FS:Ansariel> [Legacy Bake]
    }
    // </FS:Ansariel> [Legacy Bake]
    return url;
}

//-----------------------------------------------------------------------------
// resolveHeight()
//-----------------------------------------------------------------------------

void LLVOAvatar::resolveHeightAgent(const LLVector3 &in_pos_agent, LLVector3 &out_pos_agent, LLVector3 &out_norm)
{
    LLVector3d in_pos_global, out_pos_global;

    in_pos_global = gAgent.getPosGlobalFromAgent(in_pos_agent);
    resolveHeightGlobal(in_pos_global, out_pos_global, out_norm);
    out_pos_agent = gAgent.getPosAgentFromGlobal(out_pos_global);
}


void LLVOAvatar::resolveRayCollisionAgent(const LLVector3d start_pt, const LLVector3d end_pt, LLVector3d &out_pos, LLVector3 &out_norm)
{
    LLViewerObject *obj;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, start_pt, end_pt, out_pos, out_norm, &obj);
}

void LLVOAvatar::resolveHeightGlobal(const LLVector3d &inPos, LLVector3d &outPos, LLVector3 &outNorm)
{
    LLVector3d zVec(0.0f, 0.0f, 0.5f);
    LLVector3d p0 = inPos + zVec;
    LLVector3d p1 = inPos - zVec;
    LLViewerObject *obj;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, p0, p1, outPos, outNorm, &obj);
    if (!obj)
    {
        mStepOnLand = true;
        mStepMaterial = 0;
        mStepObjectVelocity.setVec(0.0f, 0.0f, 0.0f);
    }
    else
    {
        mStepOnLand = false;
        mStepMaterial = obj->getMaterial();

        // We want the primitive velocity, not our velocity... (which actually subtracts the
        // step object velocity)
        LLVector3 angularVelocity = obj->getAngularVelocity();
        LLVector3 relativePos = gAgent.getPosAgentFromGlobal(outPos) - obj->getPositionAgent();

        LLVector3 linearComponent = angularVelocity % relativePos;
//      LL_INFOS() << "Linear Component of Rotation Velocity " << linearComponent << LL_ENDL;
        mStepObjectVelocity = obj->getVelocity() + linearComponent;
    }
}


//-----------------------------------------------------------------------------
// getStepSound()
//-----------------------------------------------------------------------------
const LLUUID& LLVOAvatar::getStepSound() const
{
    if ( mStepOnLand )
    {
        // <FS:PP> FIRE-3169: Option to change the default footsteps sound
        // return sStepSoundOnLand;
        static LLCachedControl<std::string> UISndFootsteps(gSavedSettings, "UISndFootsteps");
        static const LLUUID sFootstepsSnd = LLUUID(UISndFootsteps);
        return sFootstepsSnd;
        // </FS:PP>
    }

    return sStepSounds[mStepMaterial];
}


//-----------------------------------------------------------------------------
// processAnimationStateChanges()
//-----------------------------------------------------------------------------
void LLVOAvatar::processAnimationStateChanges()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if ( isAnyAnimationSignaled(AGENT_WALK_ANIMS, NUM_AGENT_WALK_ANIMS) )
    {
        startMotion(ANIM_AGENT_WALK_ADJUST);
        stopMotion(ANIM_AGENT_FLY_ADJUST);
    }
    else if (mInAir && !isSitting())
    {
        stopMotion(ANIM_AGENT_WALK_ADJUST);
        if (mEnableDefaultMotions)
        {
        startMotion(ANIM_AGENT_FLY_ADJUST);
    }
    }
    else
    {
        stopMotion(ANIM_AGENT_WALK_ADJUST);
        stopMotion(ANIM_AGENT_FLY_ADJUST);
    }

    if ( isAnyAnimationSignaled(AGENT_GUN_AIM_ANIMS, NUM_AGENT_GUN_AIM_ANIMS) )
    {
        if (mEnableDefaultMotions)
        {
        startMotion(ANIM_AGENT_TARGET);
        }
        stopMotion(ANIM_AGENT_BODY_NOISE);
    }
    else
    {
        stopMotion(ANIM_AGENT_TARGET);
        if (mEnableDefaultMotions)
        {
            startMotion(ANIM_AGENT_BODY_NOISE);
        }
    }

    // clear all current animations
    AnimIterator anim_it;
    for (anim_it = mPlayingAnimations.begin(); anim_it != mPlayingAnimations.end();)
    {
        AnimIterator found_anim = mSignaledAnimations.find(anim_it->first);

        // playing, but not signaled, so stop
        if (found_anim == mSignaledAnimations.end())
        {
            processSingleAnimationStateChange(anim_it->first, false);
            mPlayingAnimations.erase(anim_it++);
            continue;
        }

        ++anim_it;
    }

    // if jellydolled, shelve all playing animations
    if (getOverallAppearance() != AOA_NORMAL)
    {
        mPlayingAnimations.clear();
    }

    // start up all new anims
    if (getOverallAppearance() == AOA_NORMAL)
    {
        for (anim_it = mSignaledAnimations.begin(); anim_it != mSignaledAnimations.end();)
        {
            AnimIterator found_anim = mPlayingAnimations.find(anim_it->first);

            // signaled but not playing, or different sequence id, start motion
            if (found_anim == mPlayingAnimations.end() || found_anim->second != anim_it->second)
            {
                if (processSingleAnimationStateChange(anim_it->first, true))
                {
                    mPlayingAnimations[anim_it->first] = anim_it->second;
                    ++anim_it;
                    continue;
                }
            }

            ++anim_it;
        }
    }

    // clear source information for animations which have been stopped
    if (isSelf())
    {
        AnimSourceIterator source_it = mAnimationSources.begin();

        for (source_it = mAnimationSources.begin(); source_it != mAnimationSources.end();)
        {
            if (mSignaledAnimations.find(source_it->second) == mSignaledAnimations.end())
            {
                mAnimationSources.erase(source_it++);
            }
            else
            {
                ++source_it;
            }
        }
    }

    // <FS:PP> keep AO "Always" animations alive
    if (isSelf() && AOEngine::instanceExists())
    {
        AOEngine::instance().reassertAlwaysAnimations(true);
    }
    // </FS:PP>

}


//-----------------------------------------------------------------------------
// processSingleAnimationStateChange();
//-----------------------------------------------------------------------------
bool LLVOAvatar::processSingleAnimationStateChange( const LLUUID& anim_id, bool start )
{
    // SL-402, SL-427 - we need to update body size often enough to
    // keep appearances in sync, but not so often that animations
    // cause constant jiggling of the body or camera. Possible
    // compromise is to do it on animation changes:
    computeBodySize();

    bool result = false;

    if ( start ) // start animation
    {
        if (anim_id == ANIM_AGENT_TYPE)
        {
            if (gAudiop)
            {
                LLVector3d char_pos_global = gAgent.getPosGlobalFromAgent(getCharacterPosition());
                if (LLViewerParcelMgr::getInstance()->canHearSound(char_pos_global)
                    && !LLMuteList::getInstance()->isMuted(getID(), LLMute::flagObjectSounds))
                {
                    // RN: uncomment this to play on typing sound at fixed volume once sound engine is fixed
                    // to support both spatialized and non-spatialized instances of the same sound
                    //if (isSelf())
                    //{
                    //  gAudiop->triggerSound(LLUUID(gSavedSettings.getString("UISndTyping")), 1.0f, LLAudioEngine::AUDIO_TYPE_UI);
                    //}
                    //else

                    // <FS:PP> FIRE-8190: Preview function for "UI Sounds" Panel
                    // static LLCachedControl<bool> FSPlayTypingSound(gSavedSettings, "FSPlayTypingSound");
                    // if (FSPlayTypingSound)
                    static LLCachedControl<bool> PlayModeUISndTyping(gSavedSettings, "PlayModeUISndTyping");
                    if (PlayModeUISndTyping)
                    // </FS:PP> FIRE-8190: Preview function for "UI Sounds" Panel
                    {
                        static LLCachedControl<std::string> ui_snd_string(gSavedSettings, "UISndTyping");
                        LLUUID sound_id = LLUUID(ui_snd_string);
                        gAudiop->triggerSound(sound_id, getID(), 1.0f, LLAudioEngine::AUDIO_TYPE_SFX, char_pos_global);
                    }
                }
            }
        }
        else if (anim_id == ANIM_AGENT_SIT_GROUND_CONSTRAINED)
        {
            sitDown(true);
        }


        if (startMotion(anim_id))
        {
            result = true;
        }
        else
        {
            LL_WARNS("Motion") << "Failed to start motion!" << LL_ENDL;
        }
    }
    else //stop animation
    {
        if (anim_id == ANIM_AGENT_SIT_GROUND_CONSTRAINED)
        {
            sitDown(false);
        }
        if ((anim_id == ANIM_AGENT_DO_NOT_DISTURB) && gAgent.isDoNotDisturb())
        {
            // re-assert DND tag animation
            gAgent.sendAnimationRequest(ANIM_AGENT_DO_NOT_DISTURB, ANIM_REQUEST_START);
            return result;
        }
        stopMotion(anim_id);
        result = true;
    }

    return result;
}

//-----------------------------------------------------------------------------
// isAnyAnimationSignaled()
//-----------------------------------------------------------------------------
bool LLVOAvatar::isAnyAnimationSignaled(const LLUUID *anim_array, const S32 num_anims) const
{
    for (S32 i = 0; i < num_anims; i++)
    {
        if(mSignaledAnimations.find(anim_array[i]) != mSignaledAnimations.end())
        {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// resetAnimations()
//-----------------------------------------------------------------------------
void LLVOAvatar::resetAnimations()
{
    LLKeyframeMotion::flushKeyframeCache();
    flushAllMotions();
}

//-----------------------------------------------------------------------------
// remapMotionID()
// Override selectively based on avatar sex and whether we're using new animations.
//-----------------------------------------------------------------------------
// virtual
LLUUID LLVOAvatar::remapMotionID(const LLUUID& id)
{
    static LLCachedControl<bool> use_new_walk_run(gSavedSettings, "UseNewWalkRun");
    LLUUID result = id;

    // start special case female walk for female avatars
    if (getSex() == SEX_FEMALE)
    {
        if (id == ANIM_AGENT_WALK)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_FEMALE_WALK_NEW;
            else
                result = ANIM_AGENT_FEMALE_WALK;
        }
        else if (id == ANIM_AGENT_RUN)
        {
            // There is no old female run animation, so only override
            // in one case.
            if (use_new_walk_run)
                result = ANIM_AGENT_FEMALE_RUN_NEW;
        }
        else if (id == ANIM_AGENT_SIT)
        {
            result = ANIM_AGENT_SIT_FEMALE;
        }
    }
    else
    {
        // Male avatar.
        if (id == ANIM_AGENT_WALK)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_WALK_NEW;
        }
        else if (id == ANIM_AGENT_RUN)
        {
            if (use_new_walk_run)
                result = ANIM_AGENT_RUN_NEW;
        }
        // keeps in sync with setSex() related code (viewer controls sit's sex)
        else if (id == ANIM_AGENT_SIT_FEMALE)
        {
            result = ANIM_AGENT_SIT;
        }

    }

    return result;
}

//-----------------------------------------------------------------------------
// startMotion()
// id is the asset if of the animation to start
// time_offset is the offset into the animation at which to start playing
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::startMotion(const LLUUID& id, F32 time_offset)
{
    LL_DEBUGS("Motion") << "motion requested " << id.asString() << " " << gAnimLibrary.animationName(id) << LL_ENDL;

    // <FS:Zi> Animation Overrider
    //LLUUID remap_id = remapMotionID(id, getSex());
    LLUUID remap_id;
    if (isSelf())
    {
        remap_id = AOEngine::getInstance()->override(id, true);
        if (remap_id.isNull())
        {
            remap_id = remapMotionID(id);
        }
        else
        {
            gAgent.sendAnimationRequest(remap_id, ANIM_REQUEST_START);

            // since we did an override, there is no need to do anything else,
            // specifically not the startMotion() part at the bottom of this function
            // See FIRE-29020
            return true;
        }
    }
    else
    {
        remap_id = remapMotionID(id);
    }
    // </FS:Zi> Animation Overrider

    if (remap_id != id)
    {
        LL_DEBUGS("Motion") << "motion resultant " << remap_id.asString() << " " << gAnimLibrary.animationName(remap_id) << LL_ENDL;
    }

    if (isSelf() && remap_id == ANIM_AGENT_AWAY)
    {
        gAgent.setAFK();
    }

    return LLCharacter::startMotion(remap_id, time_offset);
}

//-----------------------------------------------------------------------------
// stopMotion()
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::stopMotion(const LLUUID& id, bool stop_immediate)
{
    LL_DEBUGS("Motion") << "Motion requested " << id.asString() << " " << gAnimLibrary.animationName(id) << LL_ENDL;

    // <FS:Zi> Animation Overrider
    //LLUUID remap_id = remapMotionID(id);
    LLUUID remap_id;
    if (isSelf())
    {
        remap_id = AOEngine::getInstance()->override(id, false);
        if (remap_id.isNull())
        {
            remap_id = remapMotionID(id);
        }
        else
        {
            gAgent.sendAnimationRequest(remap_id, ANIM_REQUEST_STOP);

            // since we did an override, there is no need to do anything else,
            // specifically not the stopMotion() part at the bottom of this function
            // See FIRE-29020
            return true;
        }
    }
    else
    {
        remap_id = remapMotionID(id);
    }
    // </FS:Zi> Animation Overrider

    if (remap_id != id)
    {
        LL_DEBUGS("Motion") << "motion resultant " << remap_id.asString() << " " << gAnimLibrary.animationName(remap_id) << LL_ENDL;
    }

    if (isSelf())
    {
        gAgent.onAnimStop(remap_id);
    }

    return LLCharacter::stopMotion(remap_id, stop_immediate);
}

//-----------------------------------------------------------------------------
// hasMotionFromSource()
//-----------------------------------------------------------------------------
// virtual
bool LLVOAvatar::hasMotionFromSource(const LLUUID& source_id)
{
    return false;
}

//-----------------------------------------------------------------------------
// stopMotionFromSource()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::stopMotionFromSource(const LLUUID& source_id)
{
}

//-----------------------------------------------------------------------------
// addDebugText()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::addDebugText(const std::string& text)
{
    mDebugText.append(1, '\n');
    mDebugText.append(text);
}

//-----------------------------------------------------------------------------
// getDebugName()
//-----------------------------------------------------------------------------
// virtual
std::string LLVOAvatar::getDebugName() const
{
#if LL_RELEASE_WITH_DEBUG_INFO
    return getFullname();
#else
    return getID().asString();
#endif // LL_RELEASE_WITH_DEBUG_INFO
}

//-----------------------------------------------------------------------------
// getID()
//-----------------------------------------------------------------------------
// virtual
const LLUUID& LLVOAvatar::getID() const
{
    return mID;
}

//-----------------------------------------------------------------------------
// getJoint()
//-----------------------------------------------------------------------------
// RN: avatar joints are multi-rooted to include screen-based attachments
// virtual
LLJoint* LLVOAvatar::getJoint(std::string_view name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    //<FS:Ansariel> Joint-lookup improvements
    //joint_map_t::iterator iter = mJointMap.find( name );
    joint_map_t::iterator iter = mJointMap.find(name.data());

    LLJoint* jointp = nullptr;

    if (iter == mJointMap.end() || iter->second == nullptr)
    {   //search for joint and cache found joint in lookup table
        if (mJointAliasMap.empty())
        {
            getJointAliases();
        }
        joint_alias_map_t::const_iterator alias_iter = mJointAliasMap.find(name);
        std::string canonical_name;
        if (alias_iter != mJointAliasMap.end())
        {
            canonical_name = alias_iter->second;
        }
        else
        {
            canonical_name = name;
        }
        jointp = mRoot->findJoint(canonical_name);
        mJointMap[std::string(name)] = jointp;
    }
    else
    { //return cached pointer
      jointp = iter->second;
    }

#ifndef LL_RELEASE_FOR_DOWNLOAD
    if (jointp && jointp->getName()!="mScreen" && jointp->getName()!="mRoot")
    {
        llassert(getJoint(jointp->getJointNum())==jointp);
    }
#endif
    return jointp;
}

LLJoint *LLVOAvatar::getJoint( S32 joint_num )
{
    LLJoint *pJoint = NULL;
    if (joint_num >= 0)
    {
        if (joint_num < mNumBones)
        {
            pJoint = mSkeleton[joint_num];
        }
        else if (joint_num < mNumBones + mNumCollisionVolumes)
        {
            S32 collision_id = joint_num - mNumBones;
            pJoint = &mCollisionVolumes[collision_id];
        }
        else
        {
            // Attachment IDs start at 1
            S32 attachment_id = joint_num - (mNumBones + mNumCollisionVolumes) + 1;
            attachment_map_t::iterator iter = mAttachmentPoints.find(attachment_id);
            if (iter != mAttachmentPoints.end())
            {
                pJoint = iter->second;
            }
        }
    }

    llassert(!pJoint || pJoint->getJointNum() == joint_num);
    return pJoint;
}

void LLVOAvatar::initAllJoints()
{
    getJointAliases();
    for (auto& alias : mJointAliasMap)
    {
        mJointMap[alias.first] = mRoot->findJoint(alias.second);
    }
    // ignore mScreen and mRoot
}

//-----------------------------------------------------------------------------
// getRiggedMeshID
//
// If viewer object is a rigged mesh, set the mesh id and return true.
// Otherwise, null out the id and return false.
//-----------------------------------------------------------------------------
// static
bool LLVOAvatar::getRiggedMeshID(LLViewerObject* pVO, LLUUID& mesh_id)
{
    mesh_id.setNull();

    //If a VO has a skin that we'll reset the joint positions to their default
    if ( pVO && pVO->mDrawable )
    {
        LLVOVolume* pVObj = pVO->mDrawable->getVOVolume();
        if ( pVObj )
        {
            const LLMeshSkinInfo* pSkinData = pVObj->getSkinInfo();
            if (pSkinData
                && pSkinData->mJointNames.size() > JOINT_COUNT_REQUIRED_FOR_FULLRIG // full rig
                && pSkinData->mAlternateBindMatrix.size() > 0 )
                    {
                        mesh_id = pSkinData->mMeshID;
                        return true;
                    }
        }
    }
    return false;
}

bool LLVOAvatar::jointIsRiggedTo(const LLJoint *joint) const
{
    if (joint)
    {
        const LLJointRiggingInfoTab& tab = mJointRiggingInfoTab;
        S32 joint_num = joint->getJointNum();
        if (joint_num < tab.size() && tab[joint_num].isRiggedTo())
            {
                return true;
            }
        }
    return false;
}

void LLVOAvatar::clearAttachmentOverrides()
{

    for (S32 i=0; i<LL_CHARACTER_MAX_ANIMATED_JOINTS; i++)
    {
        LLJoint *pJoint = getJoint(i);
        if (pJoint)
        {
            pJoint->clearAttachmentPosOverrides();
            pJoint->clearAttachmentScaleOverrides();
        }
    }

    if (mPelvisFixups.count()>0)
    {
        mPelvisFixups.clear();
        LLJoint* pJointPelvis = getJoint("mPelvis");
        if (pJointPelvis)
    {
            pJointPelvis->setPosition( LLVector3( 0.0f, 0.0f, 0.0f) );
        }
        postPelvisSetRecalc();
    }

    mActiveOverrideMeshes.clear();
    onActiveOverrideMeshesChanged();
}

//-----------------------------------------------------------------------------
// rebuildAttachmentOverrides
//-----------------------------------------------------------------------------
void LLVOAvatar::rebuildAttachmentOverrides()
{

    LL_DEBUGS("AnimatedObjects") << "rebuilding" << LL_ENDL;

    clearAttachmentOverrides();

    // Handle the case that we're resetting the skeleton of an animated object.
    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            LL_DEBUGS("Avatar") << volp->getID() << " adding attachment overrides for root vol, prim count "
                                << (S32) (1+volp->numChildren()) << LL_ENDL;
            addAttachmentOverridesForObject(volp);
        }
    }

    // Attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator at_it = attachment_pt->mAttachedObjects.begin();
                 at_it != attachment_pt->mAttachedObjects.end(); ++at_it)
            {
                LLViewerObject *vo = at_it->get();
                // Attached animated objects affect joints in their control
                // avs, not the avs to which they are attached.
                if (vo && !vo->isAnimatedObject())
                {
                    addAttachmentOverridesForObject(vo);
                }
            }
        }
    }
}

//-----------------------------------------------------------------------------
// updateAttachmentOverrides
//
// This is intended to give the same results as
// rebuildAttachmentOverrides(), while avoiding redundant work.
// -----------------------------------------------------------------------------
void LLVOAvatar::updateAttachmentOverrides()
{

    LL_DEBUGS("AnimatedObjects") << "updating" << LL_ENDL;

    std::set<LLUUID> meshes_seen;

    // Handle the case that we're updating the skeleton of an animated object.
    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            LL_DEBUGS("Avatar") << volp->getID() << " adding attachment overrides for root vol, prim count "
                                << (S32) (1+volp->numChildren()) << LL_ENDL;
            addAttachmentOverridesForObject(volp, &meshes_seen);
        }
    }

    // Attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator at_it = attachment_pt->mAttachedObjects.begin();
                 at_it != attachment_pt->mAttachedObjects.end(); ++at_it)
            {
                LLViewerObject *vo = at_it->get();
                // Attached animated objects affect joints in their control
                // avs, not the avs to which they are attached.
                if (vo && !vo->isAnimatedObject())
                {
                    addAttachmentOverridesForObject(vo, &meshes_seen);
                }
            }
        }
    }
    // Remove meshes that are no longer present on the skeleton

    // have to work with a copy because removeAttachmentOverrides() will change mActiveOverrideMeshes.
    std::set<LLUUID> active_override_meshes = mActiveOverrideMeshes;
    for (std::set<LLUUID>::iterator it = active_override_meshes.begin(); it != active_override_meshes.end(); ++it)
    {
        if (meshes_seen.find(*it) == meshes_seen.end())
        {
            removeAttachmentOverridesForObject(*it);
        }
    }


#ifdef ATTACHMENT_OVERRIDE_VALIDATION
    {
        std::vector<LLVector3OverrideMap> pos_overrides_by_joint;
        std::vector<LLVector3OverrideMap> scale_overrides_by_joint;
        LLVector3OverrideMap pelvis_fixups;

        // Capture snapshot of override state after update
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLVector3OverrideMap pos_overrides;
            LLJoint *joint = getJoint(joint_num);
            if (joint)
            {
                pos_overrides_by_joint.push_back(joint->m_attachmentPosOverrides);
                scale_overrides_by_joint.push_back(joint->m_attachmentScaleOverrides);
            }
            else
            {
                // No joint, use default constructed empty maps
                pos_overrides_by_joint.push_back(LLVector3OverrideMap());
                scale_overrides_by_joint.push_back(LLVector3OverrideMap());
            }
        }
        pelvis_fixups = mPelvisFixups;
        //dumpArchetypeXML(getDebugName() + "_paranoid_updated");

        // Rebuild and compare
        rebuildAttachmentOverrides();
        //dumpArchetypeXML(getDebugName() + "_paranoid_rebuilt");
        bool mismatched = false;
        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJoint *joint = getJoint(joint_num);
            if (joint)
            {
                if (pos_overrides_by_joint[joint_num] != joint->m_attachmentPosOverrides)
                {
                    mismatched = true;
                }
                if (scale_overrides_by_joint[joint_num] != joint->m_attachmentScaleOverrides)
                {
                    mismatched = true;
            }
        }
    }
        if (pelvis_fixups != mPelvisFixups)
        {
            mismatched = true;
        }
        if (mismatched)
        {
            LL_WARNS() << "MISMATCHED ATTACHMENT OVERRIDES" << LL_ENDL;
        }
    }
#endif
    // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
    // If either the main body of the avatar is animated, or there are any animated attachedments, then flag it as an Animesh.
    mIsAnimesh = getNumAnimatedObjectAttachments() > 0 || isAnimatedObject();
    // </FS:minerjr> [FIRE-35735]
}

void LLVOAvatar::notifyAttachmentMeshLoaded()
{
    if (!isFullyLoaded())
    {
        // We just received mesh or skin info
        // Reset timer to wait for more potential meshes or changes
        mFullyLoadedTimer.reset();
    }
}

//-----------------------------------------------------------------------------
// addAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::addAttachmentOverridesForObject(LLViewerObject *vo, std::set<LLUUID>* meshes_seen, bool recursive)
{
    if (vo->getAvatar() != this && vo->getAvatarAncestor() != this)
    {
        LL_WARNS("Avatar") << "called with invalid avatar" << LL_ENDL;
        return;
    }

    if (getOverallAppearance() != AOA_NORMAL)
    {
        return;
    }

    LL_DEBUGS("AnimatedObjects") << "adding" << LL_ENDL;

    // Process all children
    if (recursive)
    {
    LLViewerObject::const_child_list_t& children = vo->getChildren();
    for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
         it != children.end(); ++it)
    {
        LLViewerObject *childp = *it;
            addAttachmentOverridesForObject(childp, meshes_seen, true);
        }
    }

    LLVOVolume *vobj = dynamic_cast<LLVOVolume*>(vo);
    bool pelvisGotSet = false;

    if (!vobj)
    {
        return;
    }

    LLViewerObject *root_object = (LLViewerObject*)vobj->getRoot();
    LL_DEBUGS("AnimatedObjects") << "trying to add attachment overrides for root object " << root_object->getID() << " prim is " << vobj << LL_ENDL;
    if (vobj->isMesh() &&
        ((vobj->getVolume() && !vobj->getVolume()->isMeshAssetLoaded()) || !gMeshRepo.meshRezEnabled()))
    {
        LL_DEBUGS("AnimatedObjects") << "failed to add attachment overrides for root object " << root_object->getID() << " mesh asset not loaded" << LL_ENDL;
        return;
    }
    const LLMeshSkinInfo*  pSkinData = vobj->getSkinInfo();

    if ( vobj && vobj->isMesh() && pSkinData )
    {
        const unsigned int bindCnt = static_cast<unsigned int>(pSkinData->mAlternateBindMatrix.size());
        const unsigned int jointCnt = static_cast<unsigned int>(pSkinData->mJointNames.size());
        if ((bindCnt > 0) && (bindCnt != jointCnt))
        {
            LL_WARNS_ONCE() << "invalid mesh, bindCnt " << bindCnt << "!= jointCnt " << jointCnt << ", joint overrides will be ignored." << LL_ENDL;
        }
        if ((bindCnt > 0) && (bindCnt == jointCnt))
        {
            const F32 pelvisZOffset = pSkinData->mPelvisOffset;
            const LLUUID& mesh_id = pSkinData->mMeshID;

            if (meshes_seen)
            {
                meshes_seen->insert(mesh_id);
            }
            bool mesh_overrides_loaded = (mActiveOverrideMeshes.find(mesh_id) != mActiveOverrideMeshes.end());
            if (mesh_overrides_loaded)
            {
                LL_DEBUGS("AnimatedObjects") << "skipping add attachment overrides for " << mesh_id
                                             << " to root object " << root_object->getID()
                                             << ", already loaded"
                                             << LL_ENDL;
            }
            else
            {
                LL_DEBUGS("AnimatedObjects") << "adding attachment overrides for " << mesh_id
                                             << " to root object " << root_object->getID() << LL_ENDL;
            }
            bool fullRig = jointCnt >= JOINT_COUNT_REQUIRED_FOR_FULLRIG;
            if ( fullRig && !mesh_overrides_loaded )
            {
                for (unsigned int i = 0; i < jointCnt; ++i)
                {
                    std::string lookingForJoint = pSkinData->mJointNames[i].c_str();
                    LLJoint* pJoint = getJoint( lookingForJoint );
                    if (pJoint)
                    {
                        const LLVector3& jointPos = LLVector3(pSkinData->mAlternateBindMatrix[i].getTranslation());
                        if (pJoint->aboveJointPosThreshold(jointPos))
                        {
                            bool override_changed;
                            pJoint->addAttachmentPosOverride( jointPos, mesh_id, avString(), override_changed );

                            if (override_changed)
                            {
                                //If joint is a pelvis then handle old/new pelvis to foot values
                                if( lookingForJoint == "mPelvis" )
                                {
                                    pelvisGotSet = true;
                                }
                            }
                            if (pSkinData->mLockScaleIfJointPosition)
                            {
                                // Note that unlike positions, there's no threshold check here,
                                // just a lock at the default value.
                                pJoint->addAttachmentScaleOverride(pJoint->getDefaultScale(), mesh_id, avString());
                            }
                        }
                    }
                }

                if (pelvisZOffset != 0.0F)
                {
                    F32 pelvis_fixup_before;
                    bool has_fixup_before =  hasPelvisFixup(pelvis_fixup_before);
                    addPelvisFixup( pelvisZOffset, mesh_id );
                    F32 pelvis_fixup_after;
                    hasPelvisFixup(pelvis_fixup_after); // Don't have to check bool here because we just added it...
                    if (!has_fixup_before || (pelvis_fixup_before != pelvis_fixup_after))
                    {
                        pelvisGotSet = true;
                    }

                }
                mActiveOverrideMeshes.insert(mesh_id);
                onActiveOverrideMeshesChanged();
            }
        }
    }
    else
    {
        LL_DEBUGS("AnimatedObjects") << "failed to add attachment overrides for root object " << root_object->getID() << " not mesh or no pSkinData" << LL_ENDL;
    }

    //Rebuild body data if we altered joints/pelvis
    if ( pelvisGotSet )
    {
        postPelvisSetRecalc();
    }
}

//-----------------------------------------------------------------------------
// getAttachmentOverrideNames
//-----------------------------------------------------------------------------
void LLVOAvatar::getAttachmentOverrideNames(std::set<std::string>& pos_names, std::set<std::string>& scale_names) const
{
    LLVector3 pos;
    LLVector3 scale;
    LLUUID mesh_id;

    // Bones
    for (avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
         iter != mSkeleton.end(); ++iter)
    {
        const LLJoint* pJoint = (*iter);
        if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
        {
            pos_names.insert(pJoint->getName());
        }
        if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
        {
            scale_names.insert(pJoint->getName());
        }
    }

    // Attachment points
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt && attachment_pt->hasAttachmentPosOverride(pos,mesh_id))
        {
            pos_names.insert(attachment_pt->getName());
        }
        // Attachment points don't have scales.
    }
}

//-----------------------------------------------------------------------------
// showAttachmentOverrides
//-----------------------------------------------------------------------------
void LLVOAvatar::showAttachmentOverrides(bool verbose) const
{
    std::set<std::string> pos_names, scale_names;
    getAttachmentOverrideNames(pos_names, scale_names);
    if (pos_names.size())
    {
        std::stringstream ss;
        std::copy(pos_names.begin(), pos_names.end(), std::ostream_iterator<std::string>(ss, ","));
        LL_INFOS() << avString() << " attachment positions defined for joints: " << ss.str() << "\n" << LL_ENDL;
    }
    else
    {
        LL_DEBUGS("Avatar") << avString() << " no attachment positions defined for any joints" << "\n" << LL_ENDL;
    }

    if (scale_names.size())
    {
        std::stringstream ss;
        std::copy(scale_names.begin(), scale_names.end(), std::ostream_iterator<std::string>(ss, ","));
        LL_INFOS() << getDebugName() << " attachment scales defined for joints: " << ss.str() << "\n" << LL_ENDL;
    }
    else
    {
        LL_INFOS() << getDebugName() << " no attachment scales defined for any joints" << "\n" << LL_ENDL;
    }

    if (!verbose)
    {
        return;
    }

    LLVector3 pos, scale;
    LLUUID mesh_id;
    S32 count = 0;

    // Bones
    for (avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
         iter != mSkeleton.end(); ++iter)
    {
        const LLJoint* pJoint = (*iter);
        if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
        {
            pJoint->showAttachmentPosOverrides(getFullname());
            count++;
        }
        if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
        {
            pJoint->showAttachmentScaleOverrides(getFullname());
            count++;
        }
    }

    // Attachment points
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        if (attachment_pt && attachment_pt->hasAttachmentPosOverride(pos,mesh_id))
        {
            attachment_pt->showAttachmentPosOverrides(getFullname());
            count++;
        }
    }

    if (count)
    {
        LL_DEBUGS("Avatar") << avString() << " end of pos, scale overrides" << LL_ENDL;
        LL_DEBUGS("Avatar") << "=================================" << LL_ENDL;
    }
}

//-----------------------------------------------------------------------------
// removeAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::removeAttachmentOverridesForObject(LLViewerObject *vo)
{
    if (vo->getAvatar() != this && vo->getAvatarAncestor() != this)
    {
        LL_WARNS("Avatar") << "called with invalid avatar" << LL_ENDL;
        return;
    }

    // Process all children
    LLViewerObject::const_child_list_t& children = vo->getChildren();
    for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
         it != children.end(); ++it)
    {
        LLViewerObject *childp = *it;
        removeAttachmentOverridesForObject(childp);
    }

    // Process self.
    LLUUID mesh_id;
    if (getRiggedMeshID(vo,mesh_id))
    {
        removeAttachmentOverridesForObject(mesh_id);
    }
}

//-----------------------------------------------------------------------------
// removeAttachmentOverridesForObject
//-----------------------------------------------------------------------------
void LLVOAvatar::removeAttachmentOverridesForObject(const LLUUID& mesh_id)
{
    LLJoint* pJointPelvis = getJoint( "mPelvis" );

    const std::string av_string = avString();
    for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
    {
        LLJoint *pJoint = getJoint(joint_num);
        if (pJoint)
        {
            bool dummy; // unused
            pJoint->removeAttachmentPosOverride(mesh_id, av_string, dummy);
            pJoint->removeAttachmentScaleOverride(mesh_id, av_string);
        }
        if (pJoint && pJoint == pJointPelvis)
        {
            removePelvisFixup(mesh_id);
            // SL-315
            pJoint->setPosition(LLVector3( 0.0f, 0.0f, 0.0f));
        }
    }

    postPelvisSetRecalc();

    mActiveOverrideMeshes.erase(mesh_id);
    onActiveOverrideMeshesChanged();
}

//-----------------------------------------------------------------------------
// getCharacterPosition()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterPosition()
{
    if (mDrawable.notNull())
    {
        return mDrawable->getPositionAgent();
    }
    else
    {
        return getPositionAgent();
    }
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterRotation()
//-----------------------------------------------------------------------------
LLQuaternion LLVOAvatar::getCharacterRotation()
{
    return getRotation();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterVelocity()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterVelocity()
{
    return getVelocity() - mStepObjectVelocity;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getCharacterAngularVelocity()
//-----------------------------------------------------------------------------
LLVector3 LLVOAvatar::getCharacterAngularVelocity()
{
    return getAngularVelocity();
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getGround()
//-----------------------------------------------------------------------------
void LLVOAvatar::getGround(const LLVector3 &in_pos_agent, LLVector3 &out_pos_agent, LLVector3 &outNorm)
{
    LLVector3d z_vec(0.0f, 0.0f, 1.0f);
    LLVector3d p0_global, p1_global;

    if (isUIAvatar())
    {
        outNorm.setVec(z_vec);
        out_pos_agent = in_pos_agent;
        return;
    }

    p0_global = gAgent.getPosGlobalFromAgent(in_pos_agent) + z_vec;
    p1_global = gAgent.getPosGlobalFromAgent(in_pos_agent) - z_vec;
    LLViewerObject *obj;
    LLVector3d out_pos_global;
    LLWorld::getInstance()->resolveStepHeightGlobal(this, p0_global, p1_global, out_pos_global, outNorm, &obj);
    out_pos_agent = gAgent.getPosAgentFromGlobal(out_pos_global);
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getTimeDilation()
//-----------------------------------------------------------------------------
F32 LLVOAvatar::getTimeDilation()
{
    return mRegionp ? mRegionp->getTimeDilation() : 1.f;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getPixelArea()
//-----------------------------------------------------------------------------
F32 LLVOAvatar::getPixelArea() const
{
    if (isUIAvatar())
    {
        return 100000.f;
    }
    return mPixelArea;
}

//-----------------------------------------------------------------------------
// LLVOAvatar::getPosGlobalFromAgent()
//-----------------------------------------------------------------------------
LLVector3d  LLVOAvatar::getPosGlobalFromAgent(const LLVector3 &position)
{
    return gAgent.getPosGlobalFromAgent(position);
}

//-----------------------------------------------------------------------------
// getPosAgentFromGlobal()
//-----------------------------------------------------------------------------
LLVector3   LLVOAvatar::getPosAgentFromGlobal(const LLVector3d &position)
{
    return gAgent.getPosAgentFromGlobal(position);
}

//-----------------------------------------------------------------------------
// requestStopMotion()
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::requestStopMotion( LLMotion* motion )
{
    // Only agent avatars should handle the stop motion notifications.
}

//-----------------------------------------------------------------------------
// loadSkeletonNode(): loads <skeleton> node from XML tree
//-----------------------------------------------------------------------------
//virtual
bool LLVOAvatar::loadSkeletonNode ()
{
    if (!LLAvatarAppearance::loadSkeletonNode())
    {
        return false;
    }

    bool ignore_hud_joints = false;
    initAttachmentPoints(ignore_hud_joints);

    return true;
}

//-----------------------------------------------------------------------------
// initAttachmentPoints(): creates attachment points if needed, sets state based on avatar_lad.xml.
//-----------------------------------------------------------------------------
void LLVOAvatar::initAttachmentPoints(bool ignore_hud_joints)
{
    LLAvatarXmlInfo::attachment_info_list_t::iterator iter;
    for (iter = sAvatarXmlInfo->mAttachmentInfoList.begin();
         iter != sAvatarXmlInfo->mAttachmentInfoList.end();
         ++iter)
    {
        LLAvatarXmlInfo::LLAvatarAttachmentInfo *info = *iter;
        if (info->mIsHUDAttachment && (!isSelf() || ignore_hud_joints))
        {
            //don't process hud joint for other avatars.
            continue;
        }

        S32 attachmentID = info->mAttachmentID;
        if (attachmentID < 1 || attachmentID > 255)
        {
            LL_WARNS() << "Attachment point out of range [1-255]: " << attachmentID << " on attachment point " << info->mName << LL_ENDL;
            continue;
        }

        LLViewerJointAttachment* attachment = NULL;
        bool newly_created = false;
        if (mAttachmentPoints.find(attachmentID) == mAttachmentPoints.end())
        {
            attachment = new LLViewerJointAttachment();
            newly_created = true;
        }
        else
        {
            attachment = mAttachmentPoints[attachmentID];
        }

        attachment->setName(info->mName);

        LLJoint *parent_joint = getJoint(info->mJointName);

        if (!parent_joint)
        {
            // If the intended parent for attachment point is unavailable, avatar_lad.xml is corrupt.
            LL_WARNS() << "No parent joint by name " << info->mJointName << " found for attachment point " << info->mName << LL_ENDL;
            LL_ERRS() << "Invalid avatar_lad.xml file" << LL_ENDL;
        }

        if (info->mHasPosition)
        {
            attachment->setOriginalPosition(info->mPosition);
            attachment->setDefaultPosition(info->mPosition);
        }

        if (info->mHasRotation)
        {
            LLQuaternion rotation;
            rotation.setQuat(info->mRotationEuler.mV[VX] * DEG_TO_RAD,
                             info->mRotationEuler.mV[VY] * DEG_TO_RAD,
                             info->mRotationEuler.mV[VZ] * DEG_TO_RAD);
            attachment->setRotation(rotation);
        }

        int group = info->mGroup;
        if (group >= 0)
        {
            if (group < 0 || group > 9)
            {
                LL_WARNS() << "Invalid group number (" << group << ") for attachment point " << info->mName << LL_ENDL;
            }
            else
            {
                attachment->setGroup(group);
            }
        }

        attachment->setPieSlice(info->mPieMenuSlice);
        attachment->setVisibleInFirstPerson(info->mVisibleFirstPerson);
        attachment->setIsHUDAttachment(info->mIsHUDAttachment);
        // attachment can potentially be animated, needs a number.
        attachment->setJointNum(mNumBones + mNumCollisionVolumes + attachmentID - 1);

        if (newly_created)
        {
            mAttachmentPoints[attachmentID] = attachment;

            // now add attachment joint
            parent_joint->addChild(attachment);
        }
    }
}

//-----------------------------------------------------------------------------
// updateVisualParams()
//-----------------------------------------------------------------------------
void LLVOAvatar::updateVisualParams()
{
    ESex avatar_sex = (getVisualParamWeight("male") > 0.5f) ? SEX_MALE : SEX_FEMALE;
    if (getSex() != avatar_sex)
    {
        if (mIsSitting && findMotion(avatar_sex == SEX_MALE ? ANIM_AGENT_SIT_FEMALE : ANIM_AGENT_SIT) != NULL)
        {
            // In some cases of gender change server changes sit motion with motion message,
            // but in case of some avatars (legacy?) there is no update from server side,
            // likely because server doesn't know about difference between motions
            // (female and male sit ids are same server side, so it is likely unaware that it
            // need to send update)
            // Make sure motion is up to date
            stopMotion(ANIM_AGENT_SIT);
            setSex(avatar_sex);
            startMotion(ANIM_AGENT_SIT);
        }
        else
        {
            setSex(avatar_sex);
        }
    }

    LLCharacter::updateVisualParams();

    if (mLastSkeletonSerialNum != mSkeletonSerialNum)
    {
        computeBodySize();
        mLastSkeletonSerialNum = mSkeletonSerialNum;
        mRoot->updateWorldMatrixChildren();
    }

    dirtyMesh();
    updateHeadOffset();
}

void LLVOAvatar::setCorrectedPixelArea(F32 area)
{
    // We always want to look good to ourselves
    if (isSelf())
    {
        area = llmax(area, F32(getTexImageSize() / 16));
    }

    setPixelArea(area);
}

//-----------------------------------------------------------------------------
// isActive()
//-----------------------------------------------------------------------------
bool LLVOAvatar::isActive() const
{
    return true;
}

//-----------------------------------------------------------------------------
// setPixelAreaAndAngle()
//-----------------------------------------------------------------------------
void LLVOAvatar::setPixelAreaAndAngle(LLAgent &agent)
{
    if (mDrawable.isNull())
    {
        return;
    }

    const LLVector4a* ext = mDrawable->getSpatialExtents();
    LLVector4a center;
    center.setAdd(ext[1], ext[0]);
    center.mul(0.5f);
    LLVector4a size;
    size.setSub(ext[1], ext[0]);
    size.mul(0.5f);

    mImpostorPixelArea = LLPipeline::calcPixelArea(center, size, *LLViewerCamera::getInstance());
    setCorrectedPixelArea(mImpostorPixelArea);

    F32 range = mDrawable->mDistanceWRTCamera;

    if (range < 0.001f)     // range == zero
    {
        mAppAngle = 180.f;
    }
    else
    {
        F32 radius = size.getLength3().getF32();
        mAppAngle = (F32) atan2( radius, range) * RAD_TO_DEG;
    }
}

//-----------------------------------------------------------------------------
// updateJointLODs()
//-----------------------------------------------------------------------------
bool LLVOAvatar::updateJointLODs()
{
    const F32 MAX_PIXEL_AREA = 100000000.f;
    F32 lod_factor = (sLODFactor * AVATAR_LOD_TWEAK_RANGE + (1.f - AVATAR_LOD_TWEAK_RANGE));
    F32 avatar_num_min_factor = clamp_rescale(sLODFactor, 0.f, 1.f, 0.25f, 0.6f);
    F32 avatar_num_factor = clamp_rescale((F32)sNumVisibleAvatars, 8, 25, 1.f, avatar_num_min_factor);
    F32 area_scale = 0.16f;

        if (isSelf())
        {
            if(gAgentCamera.cameraCustomizeAvatar() || gAgentCamera.cameraMouselook())
            {
                mAdjustedPixelArea = MAX_PIXEL_AREA;
            }
            else
            {
                mAdjustedPixelArea = mPixelArea*area_scale;
            }
        }
        else if (mIsDummy)
        {
            mAdjustedPixelArea = MAX_PIXEL_AREA;
        }
        else
        {
            // reported avatar pixel area is dependent on avatar render load, based on number of visible avatars
            mAdjustedPixelArea = (F32)mPixelArea * area_scale * lod_factor * lod_factor * avatar_num_factor * avatar_num_factor;
        }

        // now select meshes to render based on adjusted pixel area
        LLViewerJoint* root = dynamic_cast<LLViewerJoint*>(mRoot);
        bool res = false;
        if (root)
        {
            res = root->updateLOD(mAdjustedPixelArea, true);
        }
        if (res)
        {
            sNumLODChangesThisFrame++;
            dirtyMesh(2);
            return true;
        }

    return false;
}

//-----------------------------------------------------------------------------
// createDrawable()
//-----------------------------------------------------------------------------
LLDrawable *LLVOAvatar::createDrawable(LLPipeline *pipeline)
{
    pipeline->allocDrawable(this);
    mDrawable->setLit(false);

    LLDrawPoolAvatar *poolp = (LLDrawPoolAvatar*)gPipeline.getPool(mIsControlAvatar ? LLDrawPool::POOL_CONTROL_AV : LLDrawPool::POOL_AVATAR);

    // Only a single face (one per avatar)
    //this face will be splitted into several if its vertex buffer is too long.
    mDrawable->setState(LLDrawable::ACTIVE);
    mDrawable->addFace(poolp, NULL);
    mDrawable->setRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR);

    mNumInitFaces = mDrawable->getNumFaces() ;

    dirtyMesh(2);
    return mDrawable;
}


void LLVOAvatar::updateResources()
{
    if (mMeshTexturesDirty)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
        updateMeshTextures();
        mMeshTexturesDirty = false;
    }
}

//-----------------------------------------------------------------------------
// updateGeometry()
//-----------------------------------------------------------------------------
bool LLVOAvatar::updateGeometry(LLDrawable *drawable)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (!(gPipeline.hasRenderType(mIsControlAvatar ? LLPipeline::RENDER_TYPE_CONTROL_AV : LLPipeline::RENDER_TYPE_AVATAR)))
    {
        return true;
    }

    if (!mMeshValid)
    {
        return true;
    }

    if (!drawable)
    {
        LL_ERRS() << "LLVOAvatar::updateGeometry() called with NULL drawable" << LL_ENDL;
    }

    return true;
}

//-----------------------------------------------------------------------------
// updateSexDependentLayerSets()
//-----------------------------------------------------------------------------
// <FS:Ansariel> [Legacy Bake]
//void LLVOAvatar::updateSexDependentLayerSets()
//{
//  invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
//  invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet);
//  invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet);
//}
void LLVOAvatar::updateSexDependentLayerSets(bool upload_bake)
{
    invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet, upload_bake);
    invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet, upload_bake);
    invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet, upload_bake);
}
// </FS:Ansariel> [Legacy Bake]

//-----------------------------------------------------------------------------
// dirtyMesh()
//-----------------------------------------------------------------------------
void LLVOAvatar::dirtyMesh()
{
    dirtyMesh(1);
}
void LLVOAvatar::dirtyMesh(S32 priority)
{
    mDirtyMesh = llmax(mDirtyMesh, priority);
}

//-----------------------------------------------------------------------------
// getViewerJoint()
//-----------------------------------------------------------------------------
LLViewerJoint*  LLVOAvatar::getViewerJoint(S32 idx)
{
    return dynamic_cast<LLViewerJoint*>(mMeshLOD[idx]);
}

//-----------------------------------------------------------------------------
// hideHair()
//-----------------------------------------------------------------------------
void LLVOAvatar::hideHair()
{
    mMeshLOD[MESH_ID_HAIR]->setVisible(false, true);
}

//-----------------------------------------------------------------------------
// hideSkirt()
//-----------------------------------------------------------------------------
void LLVOAvatar::hideSkirt()
{
    mMeshLOD[MESH_ID_SKIRT]->setVisible(false, true);
}

bool LLVOAvatar::setParent(LLViewerObject* parent)
{
    bool ret ;
    if (parent == NULL)
    {
        getOffObject();
        ret = LLViewerObject::setParent(parent);
        if (isSelf())
        {
            gAgentCamera.resetCamera();
        }
    }
    else
    {
        ret = LLViewerObject::setParent(parent);
        if(ret)
        {
            sitOnObject(parent);
        }
    }
    return ret ;
}

void LLVOAvatar::addChild(LLViewerObject *childp)
{
    childp->extractAttachmentItemID(); // find the inventory item this object is associated with.
    if (isSelf())
    {
        const LLUUID& item_id = childp->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_DEBUGS("Avatar") << "ATT attachment child added " << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;

    }

    LLViewerObject::addChild(childp);
    if (childp->mDrawable)
    {
        if (!attachObject(childp))
        {
            LL_WARNS() << "ATT addChild() failed for "
                    << childp->getID()
                    << " item " << childp->getAttachmentItemID()
                    << LL_ENDL;
            // MAINT-3312 backout
            // mPendingAttachment.push_back(childp);
        }
    }
    else
    {
        mPendingAttachment.push_back(childp);
    }
}

void LLVOAvatar::removeChild(LLViewerObject *childp)
{
    LLViewerObject::removeChild(childp);
    if (!detachObject(childp))
    {
        LL_WARNS() << "Calling detach on non-attached object " << LL_ENDL;
    }
}

LLViewerJointAttachment* LLVOAvatar::getTargetAttachmentPoint(LLViewerObject* viewer_object)
{
    S32 attachmentID = ATTACHMENT_ID_FROM_STATE(viewer_object->getAttachmentState());

    // This should never happen unless the server didn't process the attachment point
    // correctly, but putting this check in here to be safe.
    if (attachmentID & ATTACHMENT_ADD)
    {
        LL_WARNS() << "Got an attachment with ATTACHMENT_ADD mask, removing ( attach pt:" << attachmentID << " )" << LL_ENDL;
        attachmentID &= ~ATTACHMENT_ADD;
    }

    LLViewerJointAttachment* attachment = get_if_there(mAttachmentPoints, attachmentID, (LLViewerJointAttachment*)NULL);

    if (!attachment)
    {
        if(attachmentID != 127)
        {
        LL_WARNS() << "Object attachment point invalid: " << attachmentID
            << " trying to use 1 (chest)"
            << LL_ENDL;
        }
        attachment = get_if_there(mAttachmentPoints, 1, (LLViewerJointAttachment*)NULL); // Arbitrary using 1 (chest)
        if (attachment)
        {
            LL_WARNS() << "Object attachment point invalid: " << attachmentID
                << " on object " << viewer_object->getID()
                << " attachment item " << viewer_object->getAttachmentItemID()
                << " falling back to 1 (chest)"
                << LL_ENDL;
        }
        else
        {
            LL_WARNS() << "Object attachment point invalid: " << attachmentID
                << " on object " << viewer_object->getID()
                << " attachment item " << viewer_object->getAttachmentItemID()
                << "Unable to use fallback attachment point 1 (chest)"
                << LL_ENDL;
        }
    }

    return attachment;
}

//-----------------------------------------------------------------------------
// attachObject()
//-----------------------------------------------------------------------------
const LLViewerJointAttachment *LLVOAvatar::attachObject(LLViewerObject *viewer_object)
{
    if (isSelf())
    {
        const LLUUID& item_id = viewer_object->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_DEBUGS("Avatar") << "ATT attaching object "
                            << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
    }
    LLViewerJointAttachment* attachment = getTargetAttachmentPoint(viewer_object);

    if (!attachment || !attachment->addObject(viewer_object))
    {
        const LLUUID& item_id = viewer_object->getAttachmentItemID();
        LLViewerInventoryItem *item = gInventory.getItem(item_id);
        LL_WARNS("Avatar") << "ATT attach failed "
                           << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
        return 0;
    }

    if (!viewer_object->isAnimatedObject())
    {
        updateAttachmentOverrides();
    }

    // Inform complexity logic to do partial update.
    markAttachmentComplexityDirty(viewer_object->getID());

    if (viewer_object->isSelected())
    {
        LLSelectMgr::getInstance()->updateSelectionCenter();
        LLSelectMgr::getInstance()->updatePointAt();
    }

    viewer_object->refreshBakeTexture();


    LLViewerObject::const_child_list_t& child_list = viewer_object->getChildren();
    for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
        iter != child_list.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;
        if (objectp)
        {
            objectp->refreshBakeTexture();
        }
    }

    updateMeshVisibility();

    // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
    // If either the main body of the avatar is animated, or there are any animated attachedments, then flag it as an Animesh.
    mIsAnimesh = getNumAnimatedObjectAttachments() > 0 || isAnimatedObject();
    // </FS:minerjr> [FIRE-35735]
    return attachment;
}

//-----------------------------------------------------------------------------
// getNumAttachments()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::getNumAttachments() const
{
    U32 num_attachments = 0;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        // <FS:Ansariel> Possible crash fix
        if (!attachment_pt)
        {
            continue;
        }
        // </FS:Ansariel>
        num_attachments += attachment_pt->getNumObjects();
    }
    return num_attachments;
}

//-----------------------------------------------------------------------------
// getMaxAttachments()
//-----------------------------------------------------------------------------
S32 LLVOAvatar::getMaxAttachments() const
{
    return LLAgentBenefitsMgr::current().getAttachmentLimit();
}

//-----------------------------------------------------------------------------
// canAttachMoreObjects()
// Returns true if we can attach <n> more objects.
//-----------------------------------------------------------------------------
bool LLVOAvatar::canAttachMoreObjects(U32 n) const
{
    return (getNumAttachments() + n) <= (U32)getMaxAttachments();
}

//-----------------------------------------------------------------------------
// getNumAnimatedObjectAttachments()
//-----------------------------------------------------------------------------
U32 LLVOAvatar::getNumAnimatedObjectAttachments() const
{
    U32 num_attachments = 0;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        const LLViewerJointAttachment *attachment_pt = (*iter).second;
        num_attachments += attachment_pt->getNumAnimatedObjects();
    }
    return num_attachments;
}

//-----------------------------------------------------------------------------
// getMaxAnimatedObjectAttachments()
// Gets from simulator feature if available, otherwise 0.
//-----------------------------------------------------------------------------
S32 LLVOAvatar::getMaxAnimatedObjectAttachments() const
{
    return LLAgentBenefitsMgr::current().getAnimatedObjectLimit();
}

//-----------------------------------------------------------------------------
// canAttachMoreAnimatedObjects()
// Returns true if we can attach <n> more animated objects.
//-----------------------------------------------------------------------------
bool LLVOAvatar::canAttachMoreAnimatedObjects(U32 n) const
{
    return (getNumAnimatedObjectAttachments() + n) <= (U32)getMaxAnimatedObjectAttachments();
}

//-----------------------------------------------------------------------------
// lazyAttach()
//-----------------------------------------------------------------------------
void LLVOAvatar::lazyAttach()
{
    std::vector<LLPointer<LLViewerObject> > still_pending;

    for (U32 i = 0; i < mPendingAttachment.size(); i++)
    {
        LLPointer<LLViewerObject> cur_attachment = mPendingAttachment[i];
        // Object might have died while we were waiting for drawable
        if (!cur_attachment->isDead())
        {
            if (cur_attachment->mDrawable)
            {
                if (isSelf())
                {
                    const LLUUID& item_id = cur_attachment->getAttachmentItemID();
                    LLViewerInventoryItem *item = gInventory.getItem(item_id);
                    LL_DEBUGS("Avatar") << "ATT attaching object "
                        << (item ? item->getName() : "UNKNOWN") << " id " << item_id << LL_ENDL;
                }
                if (!attachObject(cur_attachment))
                {   // Drop it
                    LL_WARNS() << "attachObject() failed for "
                        << cur_attachment->getID()
                        << " item " << cur_attachment->getAttachmentItemID()
                        << LL_ENDL;
                    // MAINT-3312 backout
                    //still_pending.push_back(cur_attachment);
                }
            }
            else
            {
                still_pending.push_back(cur_attachment);
            }
        }
    }

    mPendingAttachment = still_pending;
}

void LLVOAvatar::resetHUDAttachments()
{

    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        // <FS:Ansariel> Fix possible crash
        //if (attachment->getIsHUDAttachment())
        if (attachment && attachment->getIsHUDAttachment())
        // </FS:Ansariel>
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                const LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object && attached_object->mDrawable.notNull())
                {
                    gPipeline.markMoved(attached_object->mDrawable);
                }
            }
        }
    }
}

void LLVOAvatar::rebuildRiggedAttachments( void )
{
    for ( attachment_map_t::iterator iter = mAttachmentPoints.begin(); iter != mAttachmentPoints.end(); ++iter )
    {
        LLViewerJointAttachment* pAttachment = iter->second;

        // <FS:Ansariel> Possible crash fix
        if (!pAttachment)
        {
            continue;
        }
        // </FS:Ansariel>

        LLViewerJointAttachment::attachedobjs_vec_t::iterator attachmentIterEnd = pAttachment->mAttachedObjects.end();

        for ( LLViewerJointAttachment::attachedobjs_vec_t::iterator attachmentIter = pAttachment->mAttachedObjects.begin();
             attachmentIter != attachmentIterEnd; ++attachmentIter)
        {
            const LLViewerObject* pAttachedObject =  *attachmentIter;
            if ( pAttachment && pAttachedObject->mDrawable.notNull() )
            {
                gPipeline.markRebuild(pAttachedObject->mDrawable);
            }
        }
    }
}
//-----------------------------------------------------------------------------
// cleanupAttachedMesh()
//-----------------------------------------------------------------------------
void LLVOAvatar::cleanupAttachedMesh( LLViewerObject* pVO )
{
    LLUUID mesh_id;
    if (getRiggedMeshID(pVO, mesh_id))
    {
        // FIXME this seems like an odd place for this code.
        if ( gAgentCamera.cameraCustomizeAvatar() )
        {
            gAgent.unpauseAnimation();
            //Still want to refocus on head bone
            gAgentCamera.changeCameraToCustomizeAvatar();
        }
    }
}

bool check_object_for_mesh_loading(LLViewerObject* objectp)
{
    if (!objectp || !objectp->getVolume())
    {
        return false;
    }
    LLVolume* volp = objectp->getVolume();
    const LLUUID& mesh_id = volp->getParams().getSculptID();
    if (mesh_id.isNull())
    {
        // No mesh nor skin info needed
        return false;
    }

    if (volp->isMeshAssetUnavaliable())
    {
        // Mesh failed to load, do not expect it
        return false;
    }

    if (!objectp->mDrawable)
    {
        return false;
    }

    LLVOVolume* pvobj = objectp->mDrawable->getVOVolume();
    if (pvobj)
    {
        if (!pvobj->isMesh())
        {
            // Not a mesh
            return false;
        }

        if (!volp->isMeshAssetLoaded())
        {
            // Waiting for mesh
            return true;
        }

        const LLMeshSkinInfo* skin_data = pvobj->getSkinInfo();
        if (skin_data)
        {
            // Skin info present, done
            return false;
        }

        if (pvobj->isSkinInfoUnavaliable())
        {
            // Load failed or info not present, don't expect it
            return false;
        }
    }

    // object is not ready
    return true;
}

bool LLVOAvatar::hasPendingAttachedMeshes()
{
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment)
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                LLViewerObject* objectp = attachment_iter->get();
                if (objectp && !objectp->isDead())
                {
                    if (check_object_for_mesh_loading(objectp))
                    {
                        return true;
                    }
                    LLViewerObject::const_child_list_t& child_list = objectp->getChildren();
                    for (LLViewerObject::child_list_t::const_iterator iter1 = child_list.begin();
                         iter1 != child_list.end(); ++iter1)
                    {
                        LLViewerObject* objectchild = *iter1;
                        if (check_object_for_mesh_loading(objectchild))
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// detachObject()
//-----------------------------------------------------------------------------
bool LLVOAvatar::detachObject(LLViewerObject *viewer_object)
{
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        // <FS:Ansariel> Possible crash fix
        //if (attachment->isObjectAttached(viewer_object))
        if (attachment && attachment->isObjectAttached(viewer_object))
        // </FS:Ansariel>
        {
            markAttachmentComplexityDirty(viewer_object->getID(), true);
            bool is_animated_object = viewer_object->isAnimatedObject();
            cleanupAttachedMesh(viewer_object);

            attachment->removeObject(viewer_object);
            if (!is_animated_object)
            {
                updateAttachmentOverrides();
            }
            viewer_object->refreshBakeTexture();

            LLViewerObject::const_child_list_t& child_list = viewer_object->getChildren();
            for (LLViewerObject::child_list_t::const_iterator iter1 = child_list.begin();
                iter1 != child_list.end(); ++iter1)
            {
                LLViewerObject* objectp = *iter1;
                if (objectp)
            {
                    objectp->refreshBakeTexture();
                }
            }

            updateMeshVisibility();
            // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
            // If either the main body of the avatar is animated, or there are any animated attachedments, then flag it as an Animesh.
            mIsAnimesh = getNumAnimatedObjectAttachments() > 0 || isAnimatedObject();
            // </FS:minerjr> [FIRE-35735]

            LL_DEBUGS() << "Detaching object " << viewer_object->mID << " from " << attachment->getName() << LL_ENDL;
            return true;
        }
    }

    std::vector<LLPointer<LLViewerObject> >::iterator iter = std::find(mPendingAttachment.begin(), mPendingAttachment.end(), viewer_object);
    if (iter != mPendingAttachment.end())
    {
        mPendingAttachment.erase(iter);
        // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
        // If either the main body of the avatar is animated, or there are any animated attachedments, then flag it as an Animesh.
        mIsAnimesh = getNumAnimatedObjectAttachments() > 0 || isAnimatedObject();
        // </FS:minerjr> [FIRE-35735]
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// sitDown()
//-----------------------------------------------------------------------------
void LLVOAvatar::sitDown(bool bSitting)
{
    mIsSitting = bSitting;
    if (isSelf())
    {
        // Update Movement Controls according to own Sitting mode
        LLFloaterMove::setSittingMode(bSitting);

// [RLVa:KB] - Checked: 2010-08-29 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
        if (rlv_handler_t::isEnabled())
        {
            gRlvHandler.onSitOrStand(bSitting);
        }
// [/RLVa:KB]

        // <FS:PP> Refresh movelock position after sitting down to prevent pulling avatar back to previous one after standing up
        if (bSitting && gSavedPerAccountSettings.getBOOL("UseMoveLock") && gSavedPerAccountSettings.getBOOL("RelockMoveLockAfterMovement"))
        {
            FSLSLBridge::instance().viewerToLSL("UseMoveLock|1|noreport");
        }
        // </FS:PP>
    }
}

//-----------------------------------------------------------------------------
// sitOnObject()
//-----------------------------------------------------------------------------
void LLVOAvatar::sitOnObject(LLViewerObject *sit_object)
{
    if (isSelf())
    {
        // Might be first sit
        //LLFirstUse::useSit();

        gAgent.setFlying(false);
        gAgentCamera.setThirdPersonHeadOffset(LLVector3::zero);
        //interpolate to new camera position
        gAgentCamera.startCameraAnimation();
        // make sure we are not trying to autopilot
        gAgent.stopAutoPilot();
        gAgentCamera.setupSitCamera();
        if (gAgentCamera.getForceMouselook())
        {
            gAgentCamera.changeCameraToMouselook();
        }

        if (gAgentCamera.getFocusOnAvatar() && LLToolMgr::getInstance()->inEdit())
        {
            LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode();
            if (node && node->mValid)
            {
                LLViewerObject* root_object = node->getObject();
                if (root_object == sit_object)
                {
                    LLFloaterTools::sPreviousFocusOnAvatar = true;
                }
            }
        }

        // <FS:KC> revoke perms on sit
        U32 revoke_on = gSavedSettings.getU32("FSRevokePerms");
        if ((revoke_on == 1 || revoke_on == 3) && !sit_object->permYouOwner())
        {
            revokePermissionsOnObject(sit_object);
        }
        // </FS:KC>
    }

    if (mDrawable.isNull())
    {
        return;
    }
    LLQuaternion inv_obj_rot = ~sit_object->getRenderRotation();
    LLVector3 obj_pos = sit_object->getRenderPosition();

    LLVector3 rel_pos = getRenderPosition() - obj_pos;
    rel_pos.rotVec(inv_obj_rot);

    mDrawable->xform().setPosition(rel_pos);
    mDrawable->xform().setRotation(mDrawable->getWorldRotation() * inv_obj_rot);

    gPipeline.markMoved(mDrawable, true);
    // Notice that removing sitDown() from here causes avatars sitting on
    // objects to be not rendered for new arrivals. See EXT-6835 and EXT-1655.
    sitDown(true);
    mRoot->getXform()->setParent(&sit_object->mDrawable->xform()); // LLVOAvatar::sitOnObject
    // SL-315
    mRoot->setPosition(getPosition());
    mRoot->updateWorldMatrixChildren();

    stopMotion(ANIM_AGENT_BODY_NOISE);

    gAgentCamera.setInitSitRot(gAgent.getFrameAgent().getQuaternion());
}

//-----------------------------------------------------------------------------
// getOffObject()
//-----------------------------------------------------------------------------
void LLVOAvatar::getOffObject()
{
    if (mDrawable.isNull())
    {
        return;
    }

    LLViewerObject* sit_object = (LLViewerObject*)getParent();

    if (sit_object)
    {
        stopMotionFromSource(sit_object->getID());
        LLFollowCamMgr::getInstance()->setCameraActive(sit_object->getID(), false);

        LLViewerObject::const_child_list_t& child_list = sit_object->getChildren();
        for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
             iter != child_list.end(); ++iter)
        {
            LLViewerObject* child_objectp = *iter;

            stopMotionFromSource(child_objectp->getID());
            LLFollowCamMgr::getInstance()->setCameraActive(child_objectp->getID(), false);
        }
    }

    // assumes that transform will not be updated with drawable still having a parent
    // or that drawable had no parent from the start
    LLVector3 cur_position_world = mDrawable->getWorldPosition();
    LLQuaternion cur_rotation_world = mDrawable->getWorldRotation();

    if (mLastRootPos.length() >= MAX_STANDOFF_FROM_ORIGIN
        && (cur_position_world.length() < MAX_STANDOFF_FROM_ORIGIN
            || dist_vec(cur_position_world, mLastRootPos) > MAX_STANDOFF_DISTANCE_CHANGE))
    {
        // Most likely drawable got updated too early or some updates were missed - we got relative position to non-existing parent
        // restore coordinates from cache
        cur_position_world = mLastRootPos;
    }

    // set *local* position based on last *world* position, since we're unparenting the avatar
    mDrawable->xform().setPosition(cur_position_world);
    mDrawable->xform().setRotation(cur_rotation_world);

    gPipeline.markMoved(mDrawable, true);

    sitDown(false);

    mRoot->getXform()->setParent(NULL); // LLVOAvatar::getOffObject
    // SL-315
    mRoot->setPosition(cur_position_world);
    mRoot->setRotation(cur_rotation_world);
    mRoot->getXform()->update();

    if (mEnableDefaultMotions)
    {
    startMotion(ANIM_AGENT_BODY_NOISE);
    }

    if (isSelf())
    {
        LLQuaternion av_rot = gAgent.getFrameAgent().getQuaternion();
        LLQuaternion obj_rot = sit_object ? sit_object->getRenderRotation() : LLQuaternion::DEFAULT;
        av_rot = av_rot * obj_rot;
        LLVector3 at_axis = LLVector3::x_axis;
        at_axis = at_axis * av_rot;
        at_axis.mV[VZ] = 0.f;
        at_axis.normalize();
        gAgent.resetAxes(at_axis);
        gAgentCamera.setThirdPersonHeadOffset(LLVector3(0.f, 0.f, 1.f));
        gAgentCamera.setSitCamera(LLUUID::null);

        //KC: revoke perms on sit
        U32 revoke_on = gSavedSettings.getU32("FSRevokePerms");
        if ((revoke_on == 2 || revoke_on == 3) && (sit_object && !sit_object->permYouOwner()))
        {
            revokePermissionsOnObject(sit_object);
        }

        // <FS:Ansariel> Reset skeleton on stand up option
        if (gSavedSettings.getBOOL("FSResetSkeletonOnStandUp"))
        {
            LLHUDEffectResetSkeleton* effectp = (LLHUDEffectResetSkeleton*)LLHUDManager::getInstance()->createViewerEffect(LLHUDObject::LL_HUD_EFFECT_RESET_SKELETON, true);
            effectp->setSourceObject(gAgentAvatarp);
            effectp->setTargetObject(gAgentAvatarp);
            effectp->setResetAnimations(false);
        }
        // </FS:Ansariel>
    }
}

//-----------------------------------------------------------------------------
// revokePermissionsOnObject()
//-----------------------------------------------------------------------------
void LLVOAvatar::revokePermissionsOnObject(LLViewerObject *sit_object)
{
    if (sit_object)
    {
        gMessageSystem->newMessageFast(_PREHASH_RevokePermissions);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gMessageSystem->nextBlockFast(_PREHASH_Data);
        gMessageSystem->addUUIDFast(_PREHASH_ObjectID, sit_object->getID());
        gMessageSystem->addU32Fast(_PREHASH_ObjectPermissions, 0xFFFFFFFF);
        gAgent.sendReliableMessage();
    }
}

//-----------------------------------------------------------------------------
// findAvatarFromAttachment()
//-----------------------------------------------------------------------------
// static
LLVOAvatar* LLVOAvatar::findAvatarFromAttachment( LLViewerObject* obj )
{
    if( obj->isAttachment() )
    {
        do
        {
            obj = (LLViewerObject*) obj->getParent();
        }
        while( obj && !obj->isAvatar() );

        if( obj && !obj->isDead() )
        {
            return (LLVOAvatar*)obj;
        }
    }
    return NULL;
}

S32 LLVOAvatar::getAttachmentCount() const
{
    size_t count = 0;

    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin(); iter != mAttachmentPoints.end(); ++iter)
    {
        LLViewerJointAttachment* pAttachment = iter->second;
        count += pAttachment->mAttachedObjects.size();
    }

    return static_cast<S32>(count);
}

bool LLVOAvatar::isWearingWearableType(LLWearableType::EType type) const
{
    if (mIsDummy) return true;

    if (isSelf())
    {
        return LLAvatarAppearance::isWearingWearableType(type);
    }

    switch(type)
    {
        case LLWearableType::WT_SHAPE:
        case LLWearableType::WT_SKIN:
        case LLWearableType::WT_HAIR:
        case LLWearableType::WT_EYES:
            return true;  // everyone has all bodyparts
        default:
            break; // Do nothing
    }


    // <FS:ND> Gets called quite a lot from processObjectUpdates. Remove the frequent getInstance calls.

    // for (LLAvatarAppearanceDictionary::Textures::const_iterator tex_iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
    //   tex_iter != LLAvatarAppearance::getDictionary()->getTextures().end();
    //   ++tex_iter)

    LLAvatarAppearanceDictionary::Textures::const_iterator itrEnd = LLAvatarAppearance::getDictionary()->getTextures().end();
    for (LLAvatarAppearanceDictionary::Textures::const_iterator tex_iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         tex_iter != itrEnd;
         ++tex_iter)
    {
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = tex_iter->second;
        if (texture_dict->mWearableType == type)
        {
            // Thus, you must check to see if the corresponding baked texture is defined.
            // NOTE: this is a poor substitute if you actually want to know about individual pieces of clothing
            // this works for detecting a skirt (most important), but is ineffective at any piece of clothing that
            // gets baked into a texture that always exists (upper or lower).
            if (texture_dict->mIsUsedByBakedTexture)
            {
                const EBakedTextureIndex baked_index = texture_dict->mBakedTextureIndex;
                return isTextureDefined(LLAvatarAppearance::getDictionary()->getBakedTexture(baked_index)->mTextureIndex);
            }
            return false;
        }
    }
    return false;
}

LLViewerObject *    LLVOAvatar::findAttachmentByID( const LLUUID & target_id ) const
{
    for(attachment_map_t::const_iterator attachment_points_iter = mAttachmentPoints.begin();
        attachment_points_iter != gAgentAvatarp->mAttachmentPoints.end();
        ++attachment_points_iter)
    {
        LLViewerJointAttachment* attachment = attachment_points_iter->second;

        // <FS:Ansariel> Possible crash fix
        if (!attachment)
        {
            continue;
        }
        // </FS:Ansariel>

        for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
             attachment_iter != attachment->mAttachedObjects.end();
             ++attachment_iter)
        {
            LLViewerObject *attached_object = attachment_iter->get();
            if (attached_object &&
                attached_object->getID() == target_id)
            {
                return attached_object;
            }
        }
    }

    return NULL;
}

// virtual
// <FS:Ansariel> [Legacy Bake]
//void LLVOAvatar::invalidateComposite( LLTexLayerSet* layerset)
void LLVOAvatar::invalidateComposite( LLTexLayerSet* layerset, bool upload_result)
{
}

void LLVOAvatar::invalidateAll()
{
}

// virtual
// <FS:Ansariel> [Legacy Bake]
//void LLVOAvatar::onGlobalColorChanged(const LLTexGlobalColor* global_color)
void LLVOAvatar::onGlobalColorChanged(const LLTexGlobalColor* global_color, bool upload_bake)
{
    if (global_color == mTexSkinColor)
    {
        // <FS:Ansariel> [Legacy Bake]
        //invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
        //invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet);
        //invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet, upload_bake);
        invalidateComposite( mBakedTextureDatas[BAKED_UPPER].mTexLayerSet, upload_bake);
        invalidateComposite( mBakedTextureDatas[BAKED_LOWER].mTexLayerSet, upload_bake);
        // </FS:Ansariel> [Legacy Bake]
    }
    else if (global_color == mTexHairColor)
    {
        // <FS:Ansariel> [Legacy Bake]
        //invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet);
        //invalidateComposite( mBakedTextureDatas[BAKED_HAIR].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_HEAD].mTexLayerSet, upload_bake);
        invalidateComposite( mBakedTextureDatas[BAKED_HAIR].mTexLayerSet, upload_bake);
        // </FS:Ansariel> [Legacy Bake]

        // ! BACKWARDS COMPATIBILITY !
        // Fix for dealing with avatars from viewers that don't bake hair.
        if (!isTextureDefined(mBakedTextureDatas[BAKED_HAIR].mTextureIndex))
        {
            LLColor4 color = mTexHairColor->getColor();
            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
            {
                    mesh->setColor( color );
                }
            }
        }
    }
    else if (global_color == mTexEyeColor)
    {
        // LL_INFOS() << "invalidateComposite cause: onGlobalColorChanged( eyecolor )" << LL_ENDL;
        // <FS:Ansariel> [Legacy Bake]
        //invalidateComposite( mBakedTextureDatas[BAKED_EYES].mTexLayerSet);
        invalidateComposite( mBakedTextureDatas[BAKED_EYES].mTexLayerSet, upload_bake);
    }
    updateMeshTextures();
}

// virtual
// Do rigged mesh attachments display with this av?
bool LLVOAvatar::shouldRenderRigged() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    if (getOverallAppearance() == AOA_NORMAL)
    {
        return true;
    }
    // TBD - render for AOA_JELLYDOLL?
    return false;
}

// FIXME: We have an mVisible member, set in updateVisibility(), but this
// function doesn't return it! isVisible() and mVisible are used
// different places for different purposes. mVisible seems to be more
// related to whether the actual avatar mesh is shown, and isVisible()
// to whether anything about the avatar is displayed in the scene.
// Maybe better naming could make this clearer?
bool LLVOAvatar::isVisible() const
{
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    return mDrawable.notNull()
        && (!mOrphaned || isSelf())
        && (mDrawable->isVisible() || mIsDummy)
        && (!friends_only() || isUIAvatar() || isSelf() || isControlAvatar() || isBuddy());
}

// Determine if we have enough avatar data to render
bool LLVOAvatar::getHasMissingParts() const
{
    if (mIsDummy)
    {
        return false;
    }

    return (   ((const_cast<LLVOAvatar*>(this))->visualParamWeightsAreDefault())// Do we have a shape?
            || (   !isTextureDefined(TEX_LOWER_BAKED)
                || !isTextureDefined(TEX_UPPER_BAKED)
                || !isTextureDefined(TEX_HEAD_BAKED)
                )
            );
}

void LLVOAvatar::updateRezzedStatusTimers(S32 rez_status)
{
    // State machine for rezzed status. Statuses are -1 on startup, 0
    // Statuses are -1 on startup, 0 = cloud, 1 = gray, 2 = downloading, 3 = waiting for attachments, 4 = full.
    // Purpose is to collect time data for each it takes avatar to reach
    // various loading landmarks: gray, textured (partial), textured fully.

    if (rez_status != mLastRezzedStatus)
    {
        LL_DEBUGS("Avatar") << avString() << "rez state change: " << mLastRezzedStatus << " -> " << rez_status << LL_ENDL;

        if (mLastRezzedStatus == -1 && rez_status != -1)
        {
            // First time initialization, start all timers.
            for (S32 i = 1; i < 4; i++)
            {
                startPhase("load_" + LLVOAvatar::rezStatusToString(i));
                startPhase("first_load_" + LLVOAvatar::rezStatusToString(i));
            }
        }
        if (rez_status < mLastRezzedStatus)
        {
            // load level has decreased. start phase timers for higher load levels.
            for (S32 i = rez_status + 1; i <= mLastRezzedStatus; i++)
            {
                startPhase("load_" + LLVOAvatar::rezStatusToString(i));
            }
        }
        else if (rez_status > mLastRezzedStatus)
        {
            // load level has increased. stop phase timers for lower and equal load levels.
            for (S32 i = llmax(mLastRezzedStatus + 1, 1); i <= rez_status; i++)
            {
                stopPhase("load_" + LLVOAvatar::rezStatusToString(i));
                stopPhase("first_load_" + LLVOAvatar::rezStatusToString(i), false);
            }
            if (rez_status == 4)
            {
                // "fully loaded", mark any pending appearance change complete.
                selfStopPhase("update_appearance_from_cof");
                selfStopPhase("wear_inventory_category", false);
                selfStopPhase("process_initial_wearables_update", false);

                // Start a complexity update.
                updateVisualComplexity();
            }
        }
        mLastRezzedStatus = rez_status;
        invalidateNameTagInputs();      // <FS> B3d: "loading" shows on the name tag while the avatar is a cloud

        static LLUICachedControl<bool> show_rez_status("NameTagDebugAVRezState", false);
        if (show_rez_status)
        {
            mNameIsSet = false;
        }
    }
}

void LLVOAvatar::clearPhases()
{
    getPhases().clearPhases();
}

void LLVOAvatar::startPhase(const std::string& phase_name)
{
    F32 elapsed = 0.0;
    bool completed = false;
    bool found = getPhases().getPhaseValues(phase_name, elapsed, completed);
    //LL_DEBUGS("Avatar") << avString() << " phase state " << phase_name
    //                  << " found " << found << " elapsed " << elapsed << " completed " << completed << LL_ENDL;
    if (found)
    {
        if (!completed)
        {
            LL_DEBUGS("Avatar") << avString() << "no-op, start when started already for " << phase_name << LL_ENDL;
            return;
        }
    }
    LL_DEBUGS("Avatar") << "started phase " << phase_name << LL_ENDL;
    getPhases().startPhase(phase_name);
}

void LLVOAvatar::stopPhase(const std::string& phase_name, bool err_check)
{
    F32 elapsed = 0.0;
    bool completed = false;
    if (getPhases().getPhaseValues(phase_name, elapsed, completed))
    {
        if (!completed)
        {
            getPhases().stopPhase(phase_name);
            completed = true;
            logMetricsTimerRecord(phase_name, elapsed, completed);
            LL_DEBUGS("Avatar") << avString() << "stopped phase " << phase_name << " elapsed " << elapsed << LL_ENDL;
        }
        else
        {
            if (err_check)
            {
                LL_DEBUGS("Avatar") << "no-op, stop when stopped already for " << phase_name << LL_ENDL;
            }
        }
    }
    else
    {
        if (err_check)
        {
            LL_DEBUGS("Avatar") << "no-op, stop when not started for " << phase_name << LL_ENDL;
        }
    }
}

void LLVOAvatar::logPendingPhases()
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    for (LLViewerStats::phase_map_t::iterator it = getPhases().begin();
         it != getPhases().end();
         ++it)
    {
        const std::string& phase_name = it->first;
        F32 elapsed;
        bool completed;
        if (getPhases().getPhaseValues(phase_name, elapsed, completed))
        {
            if (!completed)
            {
                logMetricsTimerRecord(phase_name, elapsed, completed);
            }
        }
    }
}

//static
void LLVOAvatar::logPendingPhasesAllAvatars()
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead())
        {
            avatar->logPendingPhases();
        }
    }
}

void LLVOAvatar::logMetricsTimerRecord(const std::string& phase_name, F32 elapsed, bool completed)
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    LLSD record;
    record["timer_name"] = phase_name;
    record["avatar_id"] = getID();
    record["elapsed"] = elapsed;
    record["completed"] = completed;
    U32 grid_x(0), grid_y(0);
    if (getRegion() && LLWorld::instance().isRegionListed(getRegion()))
    {
        record["central_bake_version"] = LLSD::Integer(getRegion()->getCentralBakeVersion());
        grid_from_region_handle(getRegion()->getHandle(), &grid_x, &grid_y);
    }
    record["grid_x"] = LLSD::Integer(grid_x);
    record["grid_y"] = LLSD::Integer(grid_y);
    // <FS:Ansariel> [Legacy Bake]
    //record["is_using_server_bakes"] = true;
    record["is_using_server_bakes"] = isUsingServerBakes();
    record["is_self"] = isSelf();

    if (isAgentAvatarValid())
    {
        gAgentAvatarp->addMetricsTimerRecord(record);
    }
}

// call periodically to keep isFullyLoaded up to date.
// returns true if the value has changed.
bool LLVOAvatar::updateIsFullyLoaded()
{
    S32 rez_status = getRezzedStatus();
    bool loading = rez_status == 0;
    if (mFirstFullyVisible && !mIsControlAvatar)
    {
        loading = ((rez_status < 2)
                   // Wait at least 60s for unfinished textures to finish on first load,
                   // don't wait forever, it might fail. Even if it will eventually load by
                   // itself and update mLoadedCallbackTextures (or fail and clean the list),
                   // avatars are more time-sensitive than textures and can't wait that long.
                   || (mLoadedCallbackTextures < mCallbackTextureList.size() && mLastTexCallbackAddedTime.getElapsedTimeF32() < MAX_TEXTURE_WAIT_TIME_SEC)
                   || !mPendingAttachment.empty()
                   || (rez_status < 3 && !isFullyBaked())
                  );
        if (!loading)
        {
            mWaitingForMeshes = hasPendingAttachedMeshes();
            loading = mWaitingForMeshes;
        }

        // compare amount of attachments to one reported by simulator
        if (!isSelf() && mLastCloudAttachmentCount < mSimAttachments.size() && mSimAttachments.size() > 0)
        {
            S32 attachment_count = getAttachmentCount();
            if (mLastCloudAttachmentCount != attachment_count)
            {
                mLastCloudAttachmentCount = attachment_count;
                if (attachment_count != mSimAttachments.size())
                {
                    // attachment count changed, but still below desired, wait for more updates
                    mLastCloudAttachmentChangeTime.reset();
                    loading = true;
                }
            }
            else if (mLastCloudAttachmentChangeTime.getElapsedTimeF32() < MAX_ATTACHMENT_WAIT_TIME_SEC)
            {
                // waiting
                loading = true;
            }
            else if (!loading)
            {
                // for hasFirstFullAttachmentData
                mLastCloudAttachmentCount = (S32)mSimAttachments.size();
            }
        }
    }
    updateRezzedStatusTimers(rez_status);
    updateRuthTimer(loading);
    return processFullyLoadedChange(loading);
}

void LLVOAvatar::updateRuthTimer(bool loading)
{
    if (isSelf() || !loading)
    {
        return;
    }

    if (mPreviousFullyLoaded)
    {
        mRuthTimer.reset();
        debugAvatarRezTime("AvatarRezCloudNotification","became cloud");
    }

    const F32 LOADING_TIMEOUT__SECONDS = 120.f;
    if (mRuthTimer.getElapsedTimeF32() > LOADING_TIMEOUT__SECONDS)
    {
        LL_DEBUGS("Avatar") << avString()
                << "Ruth Timer timeout: Missing texture data for '" << getFullname() << "' "
                << "( Params loaded : " << !visualParamWeightsAreDefault() << " ) "
                << "( Lower : " << isTextureDefined(TEX_LOWER_BAKED) << " ) "
                << "( Upper : " << isTextureDefined(TEX_UPPER_BAKED) << " ) "
                << "( Head : " << isTextureDefined(TEX_HEAD_BAKED) << " )."
                << LL_ENDL;

        LLAvatarPropertiesProcessor::getInstance()->sendAvatarTexturesRequest(getID());
        mRuthTimer.reset();
    }
}

bool LLVOAvatar::processFullyLoadedChange(bool loading)
{
    // We wait a little bit before giving the 'all clear', to let things to
    // settle down: models to snap into place, textures to get first packets,
    // LODs to load.
    const F32 LOADED_DELAY = 1.f;

    if (loading)
    {
        mFullyLoadedTimer.reset();
    }

    if (mFirstFullyVisible)
    {
        F32 first_use_delay = FIRST_APPEARANCE_CLOUD_MIN_DELAY;
        if (!isSelf() && loading)
        {
            // Note that textures can causes 60s delay on thier own
            // so this delay might end up on top of textures' delay
            first_use_delay = llclamp(
                mFirstAppearanceMessageTimer.getElapsedTimeF32(),
                FIRST_APPEARANCE_CLOUD_MIN_DELAY,
                FIRST_APPEARANCE_CLOUD_MAX_DELAY);

            if (shouldImpostor())
            {
                // Impostors are less of a priority,
                // let them stay cloud longer
                first_use_delay *= FIRST_APPEARANCE_CLOUD_IMPOSTOR_MODIFIER;
            }
        }
        mFullyLoaded = (mFullyLoadedTimer.getElapsedTimeF32() > first_use_delay);
    }
    else
    {
        mFullyLoaded = (mFullyLoadedTimer.getElapsedTimeF32() > LOADED_DELAY);
    }

    if (!mPreviousFullyLoaded && !loading && mFullyLoaded)
    {
        debugAvatarRezTime("AvatarRezNotification", "fully loaded");
    }

    // did our loading state "change" from last call?
    // FIXME runway - why are we updating every 30 calls even if nothing has changed?
    // This causes updateLOD() to run every 30 frames, among other things.
    bool fully_loaded_changed = (mFullyLoaded != mPreviousFullyLoaded);
    const S32 UPDATE_RATE = 30;
    bool changed =
        ((mFullyLoaded != mPreviousFullyLoaded) ||         // if the value is different from the previous call
            (!mFullyLoadedInitialized) ||                     // if we've never been called before
            (mFullyLoadedFrameCounter % UPDATE_RATE == 0));   // every now and then issue a change

    mPreviousFullyLoaded = mFullyLoaded;
    mFullyLoadedInitialized = true;
    mFullyLoadedFrameCounter++;

    if (changed && isSelf())
    {
        // Agent's own avatar doesn't track bakes the same way as other avatars.
        // So just update here, on cloud removal.
        markBodyPartsComplexityDirty();

        // to know about outfit switching
        LLAvatarRenderNotifier::getInstance()->updateNotificationState();
    }

    if (fully_loaded_changed && !isSelf() && mFullyLoaded && isImpostor())
    {
        // Fix for jellydoll initially invisible
        mNeedsImpostorUpdate = true;
        mLastImpostorUpdateReason = 6;
    }

    return changed;
}

bool LLVOAvatar::isFullyLoaded() const
{
    return (mRenderUnloadedAvatar && !isSelf()) || mFullyLoaded;
}

bool LLVOAvatar::hasFirstFullAttachmentData() const
{
    return !mFirstFullyVisible // Avatar is fully visible, have all data
        || mLastCloudAttachmentCount >= (S32)mSimAttachments.size();
}

bool LLVOAvatar::isTooComplex() const
{
    // [RLVa] FIRE-35778 @camavdist:1=n RLVa command turns avatars invisible instead of a silhouette (fix from Ellie Sable)
    if (isRlvSilhouette())
    {
        return true;
    }
    // [/RLVa]
    bool too_complex;
    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    bool render_friend =  (isBuddy() && complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY);

    if (isSelf() || render_friend || mVisuallyMuteSetting == AV_ALWAYS_RENDER)
    {
        too_complex = false;
    }
    else if (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS && !mIsControlAvatar)
    {
        too_complex = true;
    }
    else
    {
        // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
        static LLCachedControl<U32> impostor_avatar_exclude(gSavedSettings,"FSImpostorAvatarExclude", 0);

        // If the avatar is set to be excluded, return that the avatar is not too complex
        if ((mIsControlAvatar && impostor_avatar_exclude >= EImpostorAvatarExclude::CONTROL) || (mIsAnimesh && (impostor_avatar_exclude & EImpostorAvatarExclude::USER)))
        {
            return false;
        }
        // </FS:minerjr> [FIRE-35735]
        // Determine if visually muted or not
        static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0U);
        static LLCachedControl<F32> max_attachment_area(gSavedSettings, "RenderAutoMuteSurfaceAreaLimit", 1000.0f);
        // If the user has chosen unlimited max complexity, we also disregard max attachment area
        // so that unlimited will completely disable the overly complex impostor rendering
        // yes, this leaves them vulnerable to griefing objects... their choice
        too_complex = (   max_render_cost > 0
                          && (mVisualComplexity > max_render_cost
                           || (max_attachment_area > 0.0f && mAttachmentSurfaceArea > max_attachment_area)
                           ));
    }

    return too_complex;
}

bool LLVOAvatar::isTooSlow() const
{
    // <FS> PLAN-avatars.md stage A step 4b: after the avatars' update the table holds the answer
    if (const LLAvatarTable::Row* row = LLAvatarTable::finalRow(this))
    {
        static const bool verify = getenv("LL_VK_AVTABLE_VERIFY") != nullptr;
        if (verify)
        {
            LLAvatarTable::noteCheck(1, row->mTooSlow == computeTooSlowLive());
        }
        return row->mTooSlow;
    }
    return computeTooSlowLive();
}

bool LLVOAvatar::computeTooSlowLive() const
{
    if (mIsControlAvatar)
    {
        return mTooSlow;
    }

    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    bool is_friend = isBuddy();
    bool render_friend = is_friend && complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY;

    if (render_friend || mVisuallyMuteSetting == AV_ALWAYS_RENDER)
    {
        return false;
    }
    else if (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS)
    {
        return true;
    }
    else if (!is_friend && friends_only())
    {
        return true;
    }

    return mTooSlow;
}

// Udpate Avatar state based on render time
void LLVOAvatar::updateTooSlow()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    static LLCachedControl<S32> complexity_render_mode(gSavedSettings, "RenderAvatarComplexityMode");
    static LLCachedControl<bool> allowSelfImpostor(gSavedSettings, "AllowSelfImpostor");
    const auto id = getID();
    bool changed_slow_state{false}; // <FS:Beq> Post LL merge, force dirty when slowness state changes

    // mTooSlow - Is the avatar flagged as being slow (includes shadow time)
    // mTooSlowWithoutShadows - Is the avatar flagged as being slow even with shadows removed.

    // get max render time in ms
    F32 max_art_ms = (F32) (LLPerfStats::renderAvatarMaxART_ns / 1000000.0);

    bool autotune = LLPerfStats::tunables.userAutoTuneEnabled && !mIsControlAvatar && !isSelf();

    bool ignore_tune = false;
    if (autotune && sAVsIgnoringARTLimit.size() > 0)
    {
        auto it = std::find(sAVsIgnoringARTLimit.begin(), sAVsIgnoringARTLimit.end(), mID);
        if (it != sAVsIgnoringARTLimit.end())
        {
            S32 index = (S32)(it - sAVsIgnoringARTLimit.begin());
            ignore_tune = (index < (MIN_NONTUNED_AVS - sAvatarsNearby + 1 + LLPerfStats::tunedAvatars));
        }
    }

    bool exceeds_max_ART =
        ((LLPerfStats::renderAvatarMaxART_ns > 0) &&
            (mGPURenderTime >= max_art_ms)); // NOTE: don't use getGPURenderTime accessor here to avoid "isTooSlow" feedback loop

    if (exceeds_max_ART && !ignore_tune)
    {
        mTooSlow = true;

        if(!mTooSlowWithoutShadows) // if we were not previously above the full impostor cap
        {
            bool always_render_friends = complexity_render_mode > AV_RENDER_LIMIT_BY_COMPLEXITY;
            bool render_friend_or_exception =   (always_render_friends && isBuddy()) ||
                ( getVisualMuteSettings() == LLVOAvatar::AV_ALWAYS_RENDER );
            if( (!isSelf() || allowSelfImpostor) && !render_friend_or_exception)
            {
                // Note: slow rendering Friends still get their shadows zapped.
                mTooSlowWithoutShadows = (getGPURenderTime()*2.f >= max_art_ms)  // NOTE: assumes shadow rendering doubles render time
                    || (complexity_render_mode == AV_RENDER_ONLY_SHOW_FRIENDS && !mIsControlAvatar);
            }
            if(mTooSlowWithoutShadows)
            {
                changed_slow_state = true;
                // </FS:Beq>
            }

        }
    }
    else
    {
        // <FS:Beq> better state change flagging
        if( mTooSlow || mTooSlowWithoutShadows )
        {
            changed_slow_state = true;
        }
        // </FS:Beq>
        mTooSlow = false;
        mTooSlowWithoutShadows = false;

        if (ignore_tune)
        {
            return;
        }
    }
    if(mTooSlow && !mTuned)
    {
        LLPerfStats::tunedAvatars++; // increment the number of avatars that have been tweaked.
        mTuned = true;
    }
    else if(!mTooSlow && mTuned)
    {
        LLPerfStats::tunedAvatars--;
        mTuned = false;
    }
    // <FS:Beq> better state change flagging
    if( changed_slow_state )
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_GEOMETRY);
    }
    // </FS:Beq>
}

//-----------------------------------------------------------------------------
// findMotion()
//-----------------------------------------------------------------------------
LLMotion* LLVOAvatar::findMotion(const LLUUID& id) const
{
    return mMotionController.findMotion(id);
}

// This is a semi-deprecated debugging tool - meshes will not show as
// colorized if using deferred rendering.
void LLVOAvatar::debugColorizeSubMeshes(U32 i, const LLColor4& color)
{
    static LLCachedControl<bool> debug_av_composite_baked(gSavedSettings, "DebugAvatarCompositeBaked");
    if (debug_av_composite_baked)
    {
        avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
        avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
        for (; iter != end; ++iter)
        {
            LLAvatarJointMesh* mesh = (*iter);
            if (mesh)
            {
                mesh->setColor(color);
            }
        }
    }
}


//-----------------------------------------------------------------------------
// updateMeshVisibility()
// Hide the mesh joints if attachments are using baked textures
//-----------------------------------------------------------------------------
void LLVOAvatar::updateMeshVisibility()
{
    bool bake_flag[BAKED_NUM_INDICES];
    memset(bake_flag, 0, BAKED_NUM_INDICES*sizeof(bool));

    if (getOverallAppearance() == AOA_NORMAL)
    {
        for (const auto& [attachment_point_id, attachment] : mAttachmentPoints)
        {
            if (!attachment)
                continue;

            for (const auto& objectp : attachment->mAttachedObjects)
            {
                if (objectp.isNull())
                    continue;

                for (int face_index = 0; face_index < objectp->getNumTEs(); face_index++)
                {
                    LLTextureEntry* tex_entry = objectp->getTE(face_index);
                    if (tex_entry)
                    {
                        const auto& tex_id = tex_entry->getID();
                        bake_flag[BAKED_HEAD] |= (tex_id == IMG_USE_BAKED_HEAD);
                        bake_flag[BAKED_EYES] |= (tex_id == IMG_USE_BAKED_EYES);
                        bake_flag[BAKED_HAIR] |= (tex_id == IMG_USE_BAKED_HAIR);
                        bake_flag[BAKED_LOWER] |= (tex_id == IMG_USE_BAKED_LOWER);
                        bake_flag[BAKED_UPPER] |= (tex_id == IMG_USE_BAKED_UPPER);
                        bake_flag[BAKED_SKIRT] |= (tex_id == IMG_USE_BAKED_SKIRT);
                        bake_flag[BAKED_LEFT_ARM] |= (tex_id == IMG_USE_BAKED_LEFTARM);
                        bake_flag[BAKED_LEFT_LEG] |= (tex_id == IMG_USE_BAKED_LEFTLEG);
                        bake_flag[BAKED_AUX1] |= (tex_id == IMG_USE_BAKED_AUX1);
                        bake_flag[BAKED_AUX2] |= (tex_id == IMG_USE_BAKED_AUX2);
                        bake_flag[BAKED_AUX3] |= (tex_id == IMG_USE_BAKED_AUX3);
                    }
                }

                for (const auto& objectchild : objectp->getChildren())
                {
                    if (objectchild.isNull())
                        continue;

                    for (int face_index = 0; face_index < objectchild->getNumTEs(); face_index++)
                    {
                        LLTextureEntry* tex_entry = objectchild->getTE(face_index);
                        if (tex_entry)
                        {
                            const auto& tex_id = tex_entry->getID();
                            bake_flag[BAKED_HEAD] |= (tex_id == IMG_USE_BAKED_HEAD);
                            bake_flag[BAKED_EYES] |= (tex_id == IMG_USE_BAKED_EYES);
                            bake_flag[BAKED_HAIR] |= (tex_id == IMG_USE_BAKED_HAIR);
                            bake_flag[BAKED_LOWER] |= (tex_id == IMG_USE_BAKED_LOWER);
                            bake_flag[BAKED_UPPER] |= (tex_id == IMG_USE_BAKED_UPPER);
                            bake_flag[BAKED_SKIRT] |= (tex_id == IMG_USE_BAKED_SKIRT);
                            bake_flag[BAKED_LEFT_ARM] |= (tex_id == IMG_USE_BAKED_LEFTARM);
                            bake_flag[BAKED_LEFT_LEG] |= (tex_id == IMG_USE_BAKED_LEFTLEG);
                            bake_flag[BAKED_AUX1] |= (tex_id == IMG_USE_BAKED_AUX1);
                            bake_flag[BAKED_AUX2] |= (tex_id == IMG_USE_BAKED_AUX2);
                            bake_flag[BAKED_AUX3] |= (tex_id == IMG_USE_BAKED_AUX3);
                        }
                    }
                }
            }
        }
    }

    //LL_INFOS() << "head " << bake_flag[BAKED_HEAD] << "eyes " << bake_flag[BAKED_EYES] << "hair " << bake_flag[BAKED_HAIR] << "lower " << bake_flag[BAKED_LOWER] << "upper " << bake_flag[BAKED_UPPER] << "skirt " << bake_flag[BAKED_SKIRT] << LL_ENDL;

    for (S32 i = 0; i < mMeshLOD.size(); i++)
    {
        LLAvatarJoint* joint = mMeshLOD[i];
        if (i == MESH_ID_HAIR)
        {
            joint->setVisible(!bake_flag[BAKED_HAIR], true);
        }
        else if (i == MESH_ID_HEAD)
        {
            joint->setVisible(!bake_flag[BAKED_HEAD], true);
        }
        else if (i == MESH_ID_SKIRT)
        {
            joint->setVisible(!bake_flag[BAKED_SKIRT], true);
        }
        else if (i == MESH_ID_UPPER_BODY)
        {
            joint->setVisible(!bake_flag[BAKED_UPPER], true);
        }
        else if (i == MESH_ID_LOWER_BODY)
        {
            joint->setVisible(!bake_flag[BAKED_LOWER], true);
        }
        else if (i == MESH_ID_EYEBALL_LEFT)
        {
            joint->setVisible(!bake_flag[BAKED_EYES], true);
        }
        else if (i == MESH_ID_EYEBALL_RIGHT)
        {
            joint->setVisible(!bake_flag[BAKED_EYES], true);
        }
        else if (i == MESH_ID_EYELASH)
        {
            joint->setVisible(!bake_flag[BAKED_HEAD], true);
        }
    }
}

//-----------------------------------------------------------------------------
// updateMeshTextures()
// Uses the current TE values to set the meshes' and layersets' textures.
//-----------------------------------------------------------------------------
// virtual
void LLVOAvatar::updateMeshTextures()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    static S32 update_counter = 0;
    mBakedTextureDebugText.clear();

    // if user has never specified a texture, assign the default
    for (U32 i=0; i < getNumTEs(); i++)
    {
        const LLViewerTexture* te_image = getImage(i, 0);
        if(!te_image || te_image->getID().isNull() || (te_image->getID() == IMG_DEFAULT))
        {
            // IMG_DEFAULT_AVATAR = a special texture that's never rendered.
            const LLUUID& image_id = (i == TEX_HAIR ? IMG_DEFAULT : IMG_DEFAULT_AVATAR);
            setImage(i, LLViewerTextureManager::getFetchedTexture(image_id), 0);
        }
    }

    const bool other_culled = !isSelf() && mCulled;
    LLLoadedCallbackEntry::source_callback_list_t* src_callback_list = NULL ;
    bool paused = false;
    if(!isSelf())
    {
        src_callback_list = &mCallbackTextureList ;
        paused = !isVisible();
    }

    std::vector<bool> is_layer_baked;
    is_layer_baked.resize(mBakedTextureDatas.size(), false);

    std::vector<bool> use_lkg_baked_layer; // lkg = "last known good"
    use_lkg_baked_layer.resize(mBakedTextureDatas.size(), false);

    mBakedTextureDebugText += llformat("%06d\n",update_counter++);
    mBakedTextureDebugText += "indx layerset linvld ltda ilb ulkg ltid\n";
    // <FS:Beq> BOM OS
    // for (U32 i=0; i < mBakedTextureDatas.size(); i++)
    for (S32 i=0; i < getNumBakes(); i++)
    {
        is_layer_baked[i] = isTextureDefined(mBakedTextureDatas[i].mTextureIndex);
        LLViewerTexLayerSet* layerset = NULL;
        bool layerset_invalid = false;
        if (!other_culled)
        {
            // When an avatar is changing clothes and not in Appearance mode,
            // use the last-known good baked texture until it finishes the first
            // render of the new layerset.
            layerset = getTexLayerSet(i);
            layerset_invalid = layerset && ( !layerset->getViewerComposite()->isInitialized()
                                             || !layerset->isLocalTextureDataAvailable() );
            use_lkg_baked_layer[i] = (!is_layer_baked[i]
                                      && (mBakedTextureDatas[i].mLastTextureID != IMG_DEFAULT_AVATAR)
                                      && layerset_invalid);
            if (use_lkg_baked_layer[i])
            {
                layerset->setUpdatesEnabled(true);
            }
        }
        else
        {
            use_lkg_baked_layer[i] = (!is_layer_baked[i]
                                      && mBakedTextureDatas[i].mLastTextureID != IMG_DEFAULT_AVATAR);
        }

        std::string last_id_string;
        if (mBakedTextureDatas[i].mLastTextureID == IMG_DEFAULT_AVATAR)
            last_id_string = "A";
        else if (mBakedTextureDatas[i].mLastTextureID == IMG_DEFAULT)
            last_id_string = "D";
        else if (mBakedTextureDatas[i].mLastTextureID == IMG_INVISIBLE)
            last_id_string = "I";
        else
            last_id_string = "*";
        bool is_ltda = layerset
            && layerset->getViewerComposite()->isInitialized()
            && layerset->isLocalTextureDataAvailable();
        mBakedTextureDebugText += llformat("%4d   %4s     %4d %4d %4d %4d %4s\n",
                                           i,
                                           (layerset?"*":"0"),
                                           layerset_invalid,
                                           is_ltda,
                                           // <FS:TJ> Fix heap-buffer-overflow with varargs and using bools from std::vector<bool>
                                           //is_layer_baked[i],
                                           //use_lkg_baked_layer[i],
                                           (int)is_layer_baked[i],
                                           (int)use_lkg_baked_layer[i],
                                           // </FS:TJ>
                                           last_id_string.c_str());
    }
    // <FS:Beq> BOM OS
    // for (U32 i=0; i < mBakedTextureDatas.size(); i++)
    for (S32 i=0; i < getNumBakes(); i++)
    // </FS:Beq>
    {
        debugColorizeSubMeshes(i, LLColor4::white);

        LLViewerTexLayerSet* layerset = getTexLayerSet(i);
        if (use_lkg_baked_layer[i] && !isUsingLocalAppearance() )
        {
            // use last known good layer (no new one)
            LLViewerFetchedTexture* baked_img = LLViewerTextureManager::getFetchedTexture(mBakedTextureDatas[i].mLastTextureID);
            mBakedTextureDatas[i].mIsUsed = true;

            debugColorizeSubMeshes(i,LLColor4::red);

            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
                {
                    mesh->setTexture( baked_img );
                }
            }
        }
        else if (!isUsingLocalAppearance() && is_layer_baked[i])
        {
            // use new layer
            LLViewerFetchedTexture* baked_img =
                LLViewerTextureManager::staticCastToFetchedTexture(
                    getImage( mBakedTextureDatas[i].mTextureIndex, 0 ), true) ;
            if( baked_img->getID() == mBakedTextureDatas[i].mLastTextureID )
            {
                // Even though the file may not be finished loading,
                // we'll consider it loaded and use it (rather than
                // doing compositing).
                useBakedTexture( baked_img->getID() );
                                mLoadedCallbacksPaused |= !isVisible();
                                checkTextureLoading();
            }
            else
            {
                mBakedTextureDatas[i].mIsLoaded = false;
                if ( (baked_img->getID() != IMG_INVISIBLE) &&
                     ((i == BAKED_HEAD) || (i == BAKED_UPPER) || (i == BAKED_LOWER)) )
                {
                    baked_img->setLoadedCallback(onBakedTextureMasksLoaded, MORPH_MASK_REQUESTED_DISCARD, true, true, new LLTextureMaskData( mID ),
                        src_callback_list, paused);
                }
                baked_img->setLoadedCallback(onBakedTextureLoaded, SWITCH_TO_BAKED_DISCARD, false, false, new LLUUID( mID ),
                    src_callback_list, paused );
                if (baked_img->getDiscardLevel() < 0 && !paused)
                {
                    // mLoadedCallbackTextures will be updated by checkTextureLoading() below
                    mLastTexCallbackAddedTime.reset();
                }

                // this could add paused texture callbacks
                mLoadedCallbacksPaused |= paused;
                checkTextureLoading();
            }
        }
        else if (layerset && isUsingLocalAppearance())
        {
            debugColorizeSubMeshes(i,LLColor4::yellow );

            layerset->createComposite();
            layerset->setUpdatesEnabled( true );
            mBakedTextureDatas[i].mIsUsed = false;

            avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
            avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
            for (; iter != end; ++iter)
            {
                LLAvatarJointMesh* mesh = (*iter);
                if (mesh)
                {
                    mesh->setLayerSet( layerset );
                }
            }
        }
        else
        {
            debugColorizeSubMeshes(i,LLColor4::blue);
        }
    }

    // set texture and color of hair manually if we are not using a baked image.
    // This can happen while loading hair for yourself, or for clients that did not
    // bake a hair texture. Still needed for yourself after 1.22 is depricated.
    if (!is_layer_baked[BAKED_HAIR])
    {
        const LLColor4 color = mTexHairColor ? mTexHairColor->getColor() : LLColor4(1,1,1,1);
        LLViewerTexture* hair_img = getImage( TEX_HAIR, 0 );
        avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.begin();
        avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[BAKED_HAIR].mJointMeshes.end();
        for (; iter != end; ++iter)
        {
            LLAvatarJointMesh* mesh = (*iter);
            if (mesh)
            {
                mesh->setColor( color );
                mesh->setTexture( hair_img );
            }
        }
    }


    for (LLAvatarAppearanceDictionary::BakedTextures::const_iterator baked_iter =
             LLAvatarAppearance::getDictionary()->getBakedTextures().begin();
         baked_iter != LLAvatarAppearance::getDictionary()->getBakedTextures().end();
         ++baked_iter)
    {
        const EBakedTextureIndex baked_index = baked_iter->first;
        const LLAvatarAppearanceDictionary::BakedEntry *baked_dict = baked_iter->second;

        for (texture_vec_t::const_iterator local_tex_iter = baked_dict->mLocalTextures.begin();
             local_tex_iter != baked_dict->mLocalTextures.end();
             ++local_tex_iter)
        {
            const ETextureIndex texture_index = *local_tex_iter;
            const bool is_baked_ready = (is_layer_baked[baked_index] && mBakedTextureDatas[baked_index].mIsLoaded) || other_culled;
            if (isSelf())
            {
                setBakedReady(texture_index, is_baked_ready);
            }
        }
    }

    // removeMissingBakedTextures() will call back into this rountine if something is removed, and can blow up the stack
    static bool call_remove_missing = true;
    if (call_remove_missing)
    {
        call_remove_missing = false;
        removeMissingBakedTextures();   // May call back into this function if anything is removed
        call_remove_missing = true;
    }

    //refresh bakes on any attached objects
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
        iter != mAttachmentPoints.end();
        ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
            attachment_iter != attachment->mAttachedObjects.end();
            ++attachment_iter)
        {
            LLViewerObject* attached_object = attachment_iter->get();
            if (attached_object && !attached_object->isDead())
            {
                attached_object->refreshBakeTexture();

                LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
                for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                    iter != child_list.end(); ++iter)
                {
                    LLViewerObject* objectp = *iter;
                    if (objectp && !objectp->isDead())
                    {
                        objectp->refreshBakeTexture();
                    }
                }
            }
        }
    }



}

// virtual
//-----------------------------------------------------------------------------
// setLocalTexture()
//-----------------------------------------------------------------------------
void LLVOAvatar::setLocalTexture( ETextureIndex type, LLViewerTexture* in_tex, bool baked_version_ready, U32 index )
{
    // invalid for anyone but self
    llassert(0);
}

//virtual
void LLVOAvatar::setBakedReady(LLAvatarAppearanceDefines::ETextureIndex type, bool baked_version_exists, U32 index)
{
    // invalid for anyone but self
    llassert(0);
}

void LLVOAvatar::addChat(const LLChat& chat)
{
    std::deque<LLChat>::iterator chat_iter;

    mChats.push_back(chat);

    size_t chat_length = 0;
    for( chat_iter = mChats.begin(); chat_iter != mChats.end(); ++chat_iter)
    {
        chat_length += chat_iter->mText.size();
    }

    // remove any excess chat
    chat_iter = mChats.begin();
    while ((chat_length > MAX_BUBBLE_CHAT_LENGTH || mChats.size() > MAX_BUBBLE_CHAT_UTTERANCES) && chat_iter != mChats.end())
    {
        chat_length -= chat_iter->mText.size();
        mChats.pop_front();
        chat_iter = mChats.begin();
    }

    mChatTimer.reset();
    mNameTagForce = true;
}

void LLVOAvatar::clearChat()
{
    mChats.clear();
}


void LLVOAvatar::applyMorphMask(const U8* tex_data, S32 width, S32 height, S32 num_components, LLAvatarAppearanceDefines::EBakedTextureIndex index)
{
    if (index >= BAKED_NUM_INDICES)
    {
        LL_WARNS() << "invalid baked texture index passed to applyMorphMask" << LL_ENDL;
        return;
    }

    for (morph_list_t::const_iterator iter = mBakedTextureDatas[index].mMaskedMorphs.begin();
         iter != mBakedTextureDatas[index].mMaskedMorphs.end(); ++iter)
    {
        const LLMaskedMorph* maskedMorph = (*iter);
        LLPolyMorphTarget* morph_target = dynamic_cast<LLPolyMorphTarget*>(maskedMorph->mMorphTarget);
        if (morph_target)
        {
            morph_target->applyMask(tex_data, width, height, num_components, maskedMorph->mInvert);
        }
    }
}

// returns true if morph masks are present and not valid for a given baked texture, false otherwise
bool LLVOAvatar::morphMaskNeedsUpdate(LLAvatarAppearanceDefines::EBakedTextureIndex index)
{
    if (index >= BAKED_NUM_INDICES)
    {
        return false;
    }

    if (!mBakedTextureDatas[index].mMaskedMorphs.empty())
    {
        if (isSelf())
        {
            LLViewerTexLayerSet *layer_set = getTexLayerSet(index);
            if (layer_set)
            {
                return !layer_set->isMorphValid();
            }
        }
        else
        {
            return false;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// releaseComponentTextures()
// release any component texture UUIDs for which we have a baked texture
// ! BACKWARDS COMPATIBILITY !
// This is only called for non-self avatars, it can be taken out once component
// textures aren't communicated by non-self avatars.
//-----------------------------------------------------------------------------
void LLVOAvatar::releaseComponentTextures()
{
    // ! BACKWARDS COMPATIBILITY !
    // Detect if the baked hair texture actually wasn't sent, and if so set to default
    if (isTextureDefined(TEX_HAIR_BAKED) && getImage(TEX_HAIR_BAKED,0)->getID() == getImage(TEX_SKIRT_BAKED,0)->getID())
    {
        if (getImage(TEX_HAIR_BAKED,0)->getID() != IMG_INVISIBLE)
        {
            // Regression case of messaging system. Expected 21 textures, received 20. last texture is not valid so set to default
            setTETexture(TEX_HAIR_BAKED, IMG_DEFAULT_AVATAR);
            markBodyPartsComplexityDirty();
        }
    }

    //<FS:Beq> BOM constrain number of bake requests when BOM not supported
    // for (U8 baked_index = 0; baked_index < BAKED_NUM_INDICES; baked_index++)
    for (U8 baked_index = 0; baked_index < getNumBakes(); baked_index++)
        //</FS:Beq>
    {
        const LLAvatarAppearanceDictionary::BakedEntry * bakedDicEntry = LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)baked_index);
        // skip if this is a skirt and av is not wearing one, or if we don't have a baked texture UUID
        if (!isTextureDefined(bakedDicEntry->mTextureIndex)
            && ( (baked_index != BAKED_SKIRT) || isWearingWearableType(LLWearableType::WT_SKIRT) ))
        {
            continue;
        }

        for (U8 texture = 0; texture < bakedDicEntry->mLocalTextures.size(); texture++)
        {
            const U8 te = (ETextureIndex)bakedDicEntry->mLocalTextures[texture];
            setTETexture(te, IMG_DEFAULT_AVATAR);
            markBodyPartsComplexityDirty();
        }
    }
}

void LLVOAvatar::dumpAvatarTEs( const std::string& context ) const
{
    LL_DEBUGS("Avatar") << avString() << (isSelf() ? "Self: " : "Other: ") << context << LL_ENDL;
    for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         iter != LLAvatarAppearance::getDictionary()->getTextures().end();
         ++iter)
    {
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
        // TODO: MULTI-WEARABLE: handle multiple textures for self
        const LLViewerTexture* te_image = getImage(iter->first,0);
        if( !te_image )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": null ptr" << LL_ENDL;
        }
        else if( te_image->getID().isNull() )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": null UUID" << LL_ENDL;
        }
        else if( te_image->getID() == IMG_DEFAULT )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": IMG_DEFAULT" << LL_ENDL;
        }
        else if( te_image->getID() == IMG_DEFAULT_AVATAR )
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": IMG_DEFAULT_AVATAR" << LL_ENDL;
        }
        else
        {
            LL_DEBUGS("Avatar") << avString() << "       " << texture_dict->mName << ": " << te_image->getID() << LL_ENDL;
        }
    }
}

//-----------------------------------------------------------------------------
// clampAttachmentPositions()
//-----------------------------------------------------------------------------
void LLVOAvatar::clampAttachmentPositions()
{
    if (isDead())
    {
        return;
    }
    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (attachment)
        {
            attachment->clampObjectPosition();
        }
    }
}

bool LLVOAvatar::hasHUDAttachment() const
{
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;

        // <FS:Ansariel> Possible crash fix
        if (!attachment)
        {
            continue;
        }
        // </FS:Ansariel>

        if (attachment->getIsHUDAttachment() && attachment->getNumObjects() > 0)
        {
            return true;
        }
    }
    return false;
}

LLBBox LLVOAvatar::getHUDBBox() const
{
    LLBBox bbox;
    for (attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
         iter != mAttachmentPoints.end();
         ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        // <FS:Ansariel> Possible crash fix
        //if (attachment->getIsHUDAttachment())
        if (attachment && attachment->getIsHUDAttachment())
        // </FS:Ansariel>
        {
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                 attachment_iter != attachment->mAttachedObjects.end();
                 ++attachment_iter)
            {
                const LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object == NULL)
                {
                    LL_WARNS() << "HUD attached object is NULL!" << LL_ENDL;
                    continue;
                }
                // initialize bounding box to contain identity orientation and center point for attached object
                bbox.addPointLocal(attached_object->getPosition());
                // add rotated bounding box for attached object
                bbox.addBBoxAgent(attached_object->getBoundingBoxAgent());
                LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
                for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                     iter != child_list.end();
                     ++iter)
                {
                    const LLViewerObject* child_objectp = *iter;
                    bbox.addBBoxAgent(child_objectp->getBoundingBoxAgent());
                }
            }
        }
    }

    return bbox;
}

//-----------------------------------------------------------------------------
// onFirstTEMessageReceived()
//-----------------------------------------------------------------------------
void LLVOAvatar::onFirstTEMessageReceived()
{
    LL_DEBUGS("Avatar") << avString() << LL_ENDL;
    if( !mFirstTEMessageReceived )
    {
        mFirstTEMessageReceived = true;

        LLLoadedCallbackEntry::source_callback_list_t* src_callback_list = NULL ;
        bool paused = false ;
        if(!isSelf())
        {
            src_callback_list = &mCallbackTextureList ;
            paused = !isVisible();
        }

        for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
        {
            const bool layer_baked = isTextureDefined(mBakedTextureDatas[i].mTextureIndex);

            // Use any baked textures that we have even if they haven't downloaded yet.
            // (That is, don't do a transition from unbaked to baked.)
            if (layer_baked)
            {
                LLViewerFetchedTexture* image = LLViewerTextureManager::staticCastToFetchedTexture(getImage( mBakedTextureDatas[i].mTextureIndex, 0 ), true) ;
                mBakedTextureDatas[i].mLastTextureID = image->getID();
                // If we have more than one texture for the other baked layers, we'll want to call this for them too.
                if ( (image->getID() != IMG_INVISIBLE) && ((i == BAKED_HEAD) || (i == BAKED_UPPER) || (i == BAKED_LOWER)) )
                {
                    image->setLoadedCallback( onBakedTextureMasksLoaded, MORPH_MASK_REQUESTED_DISCARD, true, true, new LLTextureMaskData( mID ),
                        src_callback_list, paused);
                }
                LL_DEBUGS("Avatar") << avString() << "layer_baked, setting onInitialBakedTextureLoaded as callback" << LL_ENDL;
                image->setLoadedCallback( onInitialBakedTextureLoaded, MAX_DISCARD_LEVEL, false, false, new LLUUID( mID ),
                    src_callback_list, paused );
                if (image->getDiscardLevel() < 0 && !paused)
                {
                    mLastTexCallbackAddedTime.reset();
                }
                               // this could add paused texture callbacks
                               mLoadedCallbacksPaused |= paused;
            }
        }

        mMeshTexturesDirty = true;
        gPipeline.markRenderRebuild(this);

        mFirstAppearanceMessageTimer.reset();
        mFullyLoadedTimer.reset();
    }
}

//-----------------------------------------------------------------------------
// bool visualParamWeightsAreDefault()
//-----------------------------------------------------------------------------
bool LLVOAvatar::visualParamWeightsAreDefault()
{
    bool rtn = true;

    bool is_wearing_skirt = isWearingWearableType(LLWearableType::WT_SKIRT);
    for (LLVisualParam *param = getFirstVisualParam();
         param;
         param = getNextVisualParam())
    {
        if (param->isTweakable())
        {
            LLViewerVisualParam* vparam = dynamic_cast<LLViewerVisualParam*>(param);
            llassert(vparam);
            bool is_skirt_param = vparam &&
                LLWearableType::WT_SKIRT == vparam->getWearableType();
            if (param->getWeight() != param->getDefaultWeight() &&
                // we have to not care whether skirt weights are default, if we're not actually wearing a skirt
                (is_wearing_skirt || !is_skirt_param))
            {
                //LL_INFOS() << "param '" << param->getName() << "'=" << param->getWeight() << " which differs from default=" << param->getDefaultWeight() << LL_ENDL;
                rtn = false;
                break;
            }
        }
    }

    //LL_INFOS() << "params are default ? " << int(rtn) << LL_ENDL;

    return rtn;
}

void dump_visual_param(apr_file_t* file, LLVisualParam* viewer_param, F32 value)
{
    std::string type_string = "unknown";
    if (dynamic_cast<LLTexLayerParamAlpha*>(viewer_param))
        type_string = "param_alpha";
    if (dynamic_cast<LLTexLayerParamColor*>(viewer_param))
        type_string = "param_color";
    if (dynamic_cast<LLDriverParam*>(viewer_param))
        type_string = "param_driver";
    if (dynamic_cast<LLPolyMorphTarget*>(viewer_param))
        type_string = "param_morph";
    if (dynamic_cast<LLPolySkeletalDistortion*>(viewer_param))
        type_string = "param_skeleton";
    S32 wtype = -1;
    LLViewerVisualParam *vparam = dynamic_cast<LLViewerVisualParam*>(viewer_param);
    if (vparam)
    {
        wtype = vparam->getWearableType();
    }
    S32 u8_value = F32_to_U8(value,viewer_param->getMinWeight(),viewer_param->getMaxWeight());
    apr_file_printf(file, "\t\t<param id=\"%d\" name=\"%s\" display=\"%s\" value=\"%.3f\" u8=\"%d\" type=\"%s\" wearable=\"%s\" group=\"%d\"/>\n",
                    viewer_param->getID(), viewer_param->getName().c_str(), viewer_param->getDisplayName().c_str(), value, u8_value, type_string.c_str(),
                    LLWearableType::getInstance()->getTypeName(LLWearableType::EType(wtype)).c_str(),
                    viewer_param->getGroup());
    }


void LLVOAvatar::dumpAppearanceMsgParams( const std::string& dump_prefix,
    const LLAppearanceMessageContents& contents)
{
    std::string outfilename = get_sequential_numbered_file_name(dump_prefix, ".xml");
    const std::vector<F32>& params_for_dump = contents.mParamWeights;
    const LLTEContents& tec = contents.mTEContents;

    LLAPRFile outfile;
    std::string fullpath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, outfilename);
    outfile.open(fullpath, LL_APR_WB );
    apr_file_t* file = outfile.getFileHandle();
    if (!file)
    {
        return;
    }
    else
    {
        LL_DEBUGS("Avatar") << "dumping appearance message to " << fullpath << LL_ENDL;
    }

    apr_file_printf(file, "<header>\n");
    apr_file_printf(file, "\t\t<cof_version %i />\n", contents.mCOFVersion);
    apr_file_printf(file, "\t\t<appearance_version %i />\n", contents.mAppearanceVersion);
    apr_file_printf(file, "</header>\n");

    apr_file_printf(file, "\n<params>\n");
    LLVisualParam* param = getFirstVisualParam();
    for (S32 i = 0; i < params_for_dump.size(); i++)
    {
        while( param && ((param->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) &&
                         (param->getGroup() != VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE)) ) // should not be any of group VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        {
            param = getNextVisualParam();
        }
        LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
        F32 value = params_for_dump[i];
        dump_visual_param(file, viewer_param, value);
        param = getNextVisualParam();
    }
    apr_file_printf(file, "</params>\n");

    apr_file_printf(file, "\n<textures>\n");
    for (U32 i = 0; i < tec.face_count; i++)
    {
        std::string uuid_str;
        ((LLUUID*)tec.image_data)[i].toString(uuid_str);
        apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", i, uuid_str.c_str());
    }
    apr_file_printf(file, "</textures>\n");
}

void LLVOAvatar::parseAppearanceMessage(LLMessageSystem* mesgsys, LLAppearanceMessageContents& contents)
{
    parseTEMessage(mesgsys, _PREHASH_ObjectData, -1, contents.mTEContents);

    // Parse the AppearanceData field, if any.
    if (mesgsys->has(_PREHASH_AppearanceData))
    {
        U8 av_u8;
        mesgsys->getU8Fast(_PREHASH_AppearanceData, _PREHASH_AppearanceVersion, av_u8, 0);
        contents.mAppearanceVersion = av_u8;
        //LL_DEBUGS("Avatar") << "appversion set by AppearanceData field: " << contents.mAppearanceVersion << LL_ENDL;
        mesgsys->getS32Fast(_PREHASH_AppearanceData, _PREHASH_CofVersion, contents.mCOFVersion, 0);
        // For future use:
        //mesgsys->getU32Fast(_PREHASH_AppearanceData, _PREHASH_Flags, appearance_flags, 0);
    }

    // Parse the AppearanceHover field, if any.
    contents.mHoverOffsetWasSet = false;
    if (mesgsys->has(_PREHASH_AppearanceHover))
    {
        LLVector3 hover;
        mesgsys->getVector3Fast(_PREHASH_AppearanceHover, _PREHASH_HoverHeight, hover);
        //LL_DEBUGS("Avatar") << avString() << " hover received " << hover.mV[ VX ] << "," << hover.mV[ VY ] << "," << hover.mV[ VZ ] << LL_ENDL;
        contents.mHoverOffset = hover;
        contents.mHoverOffsetWasSet = true;
    }

    // Get attachment info, if sent
    LLUUID attachment_id;
    U8     attach_point;
    S32    attach_count = mesgsys->getNumberOfBlocksFast(_PREHASH_AttachmentBlock);
    LL_DEBUGS("AVAppearanceAttachments") << "Agent " << getID() << " has "
                                         << attach_count << " attachments" << LL_ENDL;
    size_t old_size = mSimAttachments.size();
    mSimAttachments.clear();
    for (S32 attach_i = 0; attach_i < attach_count; attach_i++)
    {
        mesgsys->getUUIDFast(_PREHASH_AttachmentBlock, _PREHASH_ID, attachment_id, attach_i);
        mesgsys->getU8Fast(_PREHASH_AttachmentBlock, _PREHASH_AttachmentPoint, attach_point, attach_i);
        LL_DEBUGS("AVAppearanceAttachments") << "AV " << getID() << " has attachment " << attach_i << " "
            << (attachment_id.isNull() ? "pending" : attachment_id.asString())
            << " on point " << (S32)attach_point << LL_ENDL;

        if (attachment_id.notNull())
        {
            mSimAttachments[attachment_id] = attach_point;
        }
        else
        {
            // at the moment viewer is only interested in non-null attachments
            LL_DEBUGS("AVAppearanceAttachments") << "AV " << getID()
                << " has null attachment on point " << (S32)attach_point
                << ", discarding" << LL_ENDL;
        }
    }

    // todo? Doesn't detect if attachments were switched
    if (old_size != mSimAttachments.size())
    {
        mLastCloudAttachmentCount = 0;
        mLastCloudAttachmentChangeTime.reset();
        if (!isFullyLoaded())
        {
            mFullyLoadedTimer.reset();
        }
    }

    // Parse visual params, if any.
    S32 num_blocks = mesgsys->getNumberOfBlocksFast(_PREHASH_VisualParam);
    if( num_blocks > 1)
    {
        //LL_DEBUGS("Avatar") << avString() << " handle visual params, num_blocks " << num_blocks << LL_ENDL;

        LLVisualParam* param = getFirstVisualParam();
        llassert(param); // if this ever fires, we should do the same as when num_blocks<=1
        if (!param)
        {
            LL_WARNS() << "No visual params!" << LL_ENDL;
        }
        else
        {
            for( S32 i = 0; i < num_blocks; i++ )
            {
                while( param && ((param->getGroup() != VISUAL_PARAM_GROUP_TWEAKABLE) &&
                                 (param->getGroup() != VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE)) ) // should not be any of group VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
                {
                    param = getNextVisualParam();
                }

                if( !param )
                {
                    // more visual params supplied than expected - just process what we know about
                    break;
                }

                U8 value;
                mesgsys->getU8Fast(_PREHASH_VisualParam, _PREHASH_ParamValue, value, i);
                F32 newWeight = U8_to_F32(value, param->getMinWeight(), param->getMaxWeight());
                contents.mParamWeights.push_back(newWeight);
                contents.mParams.push_back(param);

                param = getNextVisualParam();
            }
        }

        const S32 expected_tweakable_count = getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TWEAKABLE) +
                                             getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE); // don't worry about VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        if (num_blocks != expected_tweakable_count)
        {
            LL_DEBUGS("Avatar") << "Number of params in AvatarAppearance msg (" << num_blocks << ") does not match number of tweakable params in avatar xml file (" << expected_tweakable_count << ").  Processing what we can.  object: " << getID() << LL_ENDL;
        }
    }
    else
    {
            LL_DEBUGS("Avatar") << "AvatarAppearance msg received without any parameters, object: " << getID() << LL_ENDL;
        }

    LLVisualParam* appearance_version_param = getVisualParam(11000);
    if (appearance_version_param)
    {
        std::vector<LLVisualParam*>::iterator it = std::find(contents.mParams.begin(), contents.mParams.end(),appearance_version_param);
        if (it != contents.mParams.end())
        {
            S32 index = (S32)(it - contents.mParams.begin());
            contents.mParamAppearanceVersion = ll_round(contents.mParamWeights[index]);
            //LL_DEBUGS("Avatar") << "appversion req by appearance_version param: " << contents.mParamAppearanceVersion << LL_ENDL;
        }
    }
}

bool resolve_appearance_version(const LLAppearanceMessageContents& contents, S32& appearance_version)
{
    appearance_version = -1;

    if ((contents.mAppearanceVersion) >= 0 &&
        (contents.mParamAppearanceVersion >= 0) &&
        (contents.mAppearanceVersion != contents.mParamAppearanceVersion))
    {
        LL_WARNS() << "inconsistent appearance_version settings - field: " <<
            contents.mAppearanceVersion << ", param: " <<  contents.mParamAppearanceVersion << LL_ENDL;
        return false;
    }
    // <FS:Ansariel> [Legacy Bake]
    //if (contents.mParamAppearanceVersion >= 0) // use visual param if available.
    //{
    //  appearance_version = contents.mParamAppearanceVersion;
    //}
    //else if (contents.mAppearanceVersion > 0)
    //{
    //  appearance_version = contents.mAppearanceVersion;
    //}
    //else // still not set, go with 1.
    //{
    //  appearance_version = 1;
    //}
    if (contents.mParamAppearanceVersion >= 0) // use visual param if available.
    {
        appearance_version = contents.mParamAppearanceVersion;
    }
    if (contents.mAppearanceVersion >= 0)
    {
        appearance_version = contents.mAppearanceVersion;
    }
    if (appearance_version < 0) // still not set, go with 0.
    {
        appearance_version = 0;
    }
    // </FS:Ansariel> [Legacy Bake]
    //LL_DEBUGS("Avatar") << "appearance version info - field " << contents.mAppearanceVersion
    //                  << " param: " << contents.mParamAppearanceVersion
    //                  << " final: " << appearance_version << LL_ENDL;
    return true;
}

//-----------------------------------------------------------------------------
// processAvatarAppearance()
//-----------------------------------------------------------------------------
void LLVOAvatar::processAvatarAppearance( LLMessageSystem* mesgsys )
{
    invalidateNameTagInputs();      // <FS> B3d: the shape and baked textures decide the "loading" text of the name tag
    static LLCachedControl<bool> enable_verbose_dumps(gSavedSettings, "DebugAvatarAppearanceMessage");
    static LLCachedControl<bool> block_avatar_appearance_messages(gSavedSettings, "BlockAvatarAppearanceMessages");

    std::string dump_prefix = getDebugName() + (isSelf() ? "_s_" : "_o_");
    if (block_avatar_appearance_messages)
    {
        LL_WARNS() << "Blocking AvatarAppearance message" << LL_ENDL;
        return;
    }

    mLastAppearanceMessageTimer.reset();

    LLPointer<LLAppearanceMessageContents> contents(new LLAppearanceMessageContents);
    parseAppearanceMessage(mesgsys, *contents);
    if (enable_verbose_dumps)
    {
        std::string dump_prefix = getFullname() + "_" + (isSelf()?"s":"o") + "_";
        dumpAppearanceMsgParams(dump_prefix + "appearance_msg", *contents);
    }

    S32 appearance_version;
    if (!resolve_appearance_version(*contents, appearance_version))
    {
        LL_WARNS() << "bad appearance version info, discarding" << LL_ENDL;
        return;
    }
    //llassert(appearance_version > 0);
    if (appearance_version > 1)
    {
        LL_WARNS() << "unsupported appearance version " << appearance_version << ", discarding appearance message" << LL_ENDL;
        return;
    }

    S32 thisAppearanceVersion(contents->mCOFVersion);
    if (isSelf())
    {   // In the past this was considered to be the canonical COF version,
        // that is no longer the case.  The canonical version is maintained
        // by the AIS code and should match the COF version there. Even so,
        // we must prevent rolling this one backwards backwards or processing
        // stale versions.

        S32 aisCOFVersion(LLAppearanceMgr::instance().getCOFVersion());

        LL_DEBUGS("Avatar") << "handling self appearance message #" << thisAppearanceVersion <<
            " (highest seen #" << mLastUpdateReceivedCOFVersion <<
            ") (AISCOF=#" << aisCOFVersion << ")" << LL_ENDL;

        // <FS:Ansariel> [Legacy Bake]
        if (mFirstTEMessageReceived && (appearance_version == 0))
        {
            return;
        }
        // </FS:Ansariel> [Legacy Bake]

// <FS:Beq> appearance fail fix from Rye
        // if (mLastUpdateReceivedCOFVersion >= thisAppearanceVersion)
        if (appearance_version > 0 && mLastUpdateReceivedCOFVersion >= thisAppearanceVersion)
// </FS:Beq>
        {
            LL_WARNS("Avatar") << "Stale appearance received #" << thisAppearanceVersion <<
                " attempt to roll back from #" << mLastUpdateReceivedCOFVersion <<
                "... dropping." << LL_ENDL;
            return;
        }
        if (isEditingAppearance())
        {
            LL_DEBUGS("Avatar") << "Editing appearance.  Dropping appearance update." << LL_ENDL;
            return;
        }

    }

    // SUNSHINE CLEANUP - is this case OK now?
    auto num_params = contents->mParamWeights.size();
    if (num_params <= 1)
    {
        // In this case, we have no reliable basis for knowing
        // appearance version, which may cause us to look for baked
        // textures in the wrong place and flag them as missing
        // assets.
        // <FS:Beq> Attempt to deal with empty appearance for self
        if(isSelf() && mLastUpdateReceivedCOFVersion != -1)
        {
            LL_INFOS("Avatar") << "Empty appearance for self. Forcing a refresh" << LL_ENDL;
            LLNotificationsUtil::add("AvatarRezSelfBakeForceUpdateNotification");
            LLAppearanceMgr::instance().syncCofVersionAndRefresh();
        }
        else
        // </FS:Beq>
        LL_DEBUGS("Avatar") << "ignoring appearance message due to lack of params" << LL_ENDL;
        return;
    }

    // No backsies zone - if we get here, the message should be valid and usable, will be processed.
    // Note:
    // RequestAgentUpdateAppearanceResponder::onRequestRequested()
    // assumes that cof version is only updated with server-bake
    // appearance messages.
    if (isSelf())
    {
        LL_INFOS("Avatar") << "Processing appearance message version " << thisAppearanceVersion << LL_ENDL;
    }
    else
    {
        LL_INFOS("Avatar") << "Processing appearance message for " << getID() << ", version " << thisAppearanceVersion << LL_ENDL;
    }

    // Note:
    // locally the COF is maintained via LLInventoryModel::accountForUpdate
    // which is called from various places.  This should match the simhost's
    // idea of what the COF version is.  AIS however maintains its own version
    // of the COF that should be considered canonical.
    mLastUpdateReceivedCOFVersion = thisAppearanceVersion;

    // <FS:Ansariel> [Legacy Bake]
    setIsUsingServerBakes(appearance_version > 0);
    mLastProcessedAppearance = contents;

    bool slam_params = false;
    applyParsedAppearanceMessage(*contents, slam_params);
    if (getOverallAppearance() != AOA_NORMAL)
    {
        resetSkeleton(false);
    }
}

void LLVOAvatar::applyParsedAppearanceMessage(LLAppearanceMessageContents& contents, bool slam_params)
{
    auto num_params = contents.mParamWeights.size();
    ESex old_sex = getSex();

    if (applyParsedTEMessage(contents.mTEContents) > 0 && isChanged(TEXTURE))
    {
        updateVisualComplexity();
    }

    // <FS:clientTags>
    if (!LLGridManager::getInstance()->isInSecondLife())
    {
        //Wolfspirit: Read the UUID, system and Texturecolor
        const LLTEContents& tec = contents.mTEContents;
        const LLUUID tag_uuid = tec.image_data[TEX_HEAD_BODYPAINT];
        bool new_system = (tec.glow[TEX_HEAD_BODYPAINT]);

        //WS: Write them into an LLSD map
        mClientTagData["uuid"] = tag_uuid.asString();
        mClientTagData["id_based"] = new_system;
        mClientTagData["tex_color"] = tec.colors[TEX_HEAD_BODYPAINT].getValue();

        //WS: Clear mNameString to force a rebuild
        mNameIsSet = false;
    }
    // </FS:clientTags>

    // prevent the overwriting of valid baked textures with invalid baked textures
    for (U8 baked_index = 0; baked_index < mBakedTextureDatas.size(); baked_index++)
    {
        // <FS:Beq> refactor a little to help debug
        // if (!isTextureDefined(mBakedTextureDatas[baked_index].mTextureIndex)
        auto isDefined = isTextureDefined(mBakedTextureDatas[baked_index].mTextureIndex);
        LL_DEBUGS("Avatar") << avString() << "sb " << (S32) isUsingServerBakes() << " baked_index " << (S32) baked_index << " textureDefined= " << isDefined << LL_ENDL;
        if (!isDefined
        // </FS:Beq>
            && mBakedTextureDatas[baked_index].mLastTextureID != IMG_DEFAULT
            && baked_index != BAKED_SKIRT && baked_index != BAKED_LEFT_ARM && baked_index != BAKED_LEFT_LEG && baked_index != BAKED_AUX1 && baked_index != BAKED_AUX2 && baked_index != BAKED_AUX3)
        {
            // <FS:Ansariel> [Legacy Bake]
            //LL_DEBUGS("Avatar") << avString() << " baked_index " << (S32) baked_index << " using mLastTextureID " << mBakedTextureDatas[baked_index].mLastTextureID << LL_ENDL;
            LL_DEBUGS("Avatar") << avString() << "sb " << (S32) isUsingServerBakes() << " baked_index " << (S32) baked_index << " using mLastTextureID " << mBakedTextureDatas[baked_index].mLastTextureID << LL_ENDL;
            setTEImage(mBakedTextureDatas[baked_index].mTextureIndex,
                LLViewerTextureManager::getFetchedTexture(mBakedTextureDatas[baked_index].mLastTextureID, FTT_DEFAULT, true, LLBaseTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE));
        }
        else
        {
            // <FS:Ansariel> [Legacy Bake]
            //LL_DEBUGS("Avatar") << avString() << " baked_index " << (S32) baked_index << " using texture id "
            LL_DEBUGS("Avatar") << avString() << "sb " << (S32) isUsingServerBakes() << " baked_index " << (S32) baked_index << " using texture id "
                                << getTEref(mBakedTextureDatas[baked_index].mTextureIndex).getID() << LL_ENDL;
        }
    }

    // runway - was
    // if (!is_first_appearance_message )
    // which means it would be called on second appearance message - probably wrong.
    bool is_first_appearance_message = !mFirstAppearanceMessageReceived;
    mFirstAppearanceMessageReceived = true;

    //LL_DEBUGS("Avatar") << avString() << "processAvatarAppearance start " << mID
    //                    << " first? " << is_first_appearance_message << " self? " << isSelf() << LL_ENDL;

    if (is_first_appearance_message )
    {
        onFirstTEMessageReceived();
    }

    setCompositeUpdatesEnabled( false );
    gPipeline.markRenderRebuild(this);

    // Apply visual params
    if( num_params > 1)
    {
        //LL_DEBUGS("Avatar") << avString() << " handle visual params, num_params " << num_params << LL_ENDL;
        bool params_changed = false;
        bool interp_params = false;
        S32 params_changed_count = 0;

        for( size_t i = 0; i < num_params; i++ )
        {
            LLVisualParam* param = contents.mParams[i];
            F32 newWeight = contents.mParamWeights[i];

            if (slam_params || is_first_appearance_message || (param->getWeight() != newWeight))
            {
                params_changed = true;
                params_changed_count++;

                if(is_first_appearance_message || slam_params)
                {
                    //LL_DEBUGS("Avatar") << "param slam " << i << " " << newWeight << LL_ENDL;
                    // <FS:Ansariel> [Legacy Bake]
                    //param->setWeight(newWeight);
                    param->setWeight(newWeight, false);
                }
                else
                {
                    interp_params = true;
                    // <FS:Ansariel> [Legacy Bake]
                    //param->setAnimationTarget(newWeight);
                    param->setAnimationTarget(newWeight, false);
                }
            }
        }
        const S32 expected_tweakable_count = getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TWEAKABLE) +
                                             getVisualParamCountInGroup(VISUAL_PARAM_GROUP_TRANSMIT_NOT_TWEAKABLE); // don't worry about VISUAL_PARAM_GROUP_TWEAKABLE_NO_TRANSMIT
        if (num_params != expected_tweakable_count)
        {
            LL_DEBUGS("Avatar") << "Number of params in AvatarAppearance msg (" << num_params << ") does not match number of tweakable params in avatar xml file (" << expected_tweakable_count << ").  Processing what we can.  object: " << getID() << LL_ENDL;
        }

        LL_DEBUGS("Avatar") << "Changed " << params_changed_count << " params" << LL_ENDL;
        if (params_changed)
        {
            if (interp_params)
            {
                startAppearanceAnimation();
            }
            updateVisualParams();

            ESex new_sex = getSex();
            if( old_sex != new_sex )
            {
                // <FS:Ansariel> [Legacy Bake]
                //updateSexDependentLayerSets();
                updateSexDependentLayerSets(false);
            }
        }

        llassert( getSex() == ((getVisualParamWeight( "male" ) > 0.5f) ? SEX_MALE : SEX_FEMALE) );
    }
    else
    {
        // AvatarAppearance message arrived without visual params
        LL_DEBUGS("Avatar") << avString() << "no visual params" << LL_ENDL;

        const F32 LOADING_TIMEOUT_SECONDS = 60.f;
        // this isn't really a problem if we already have a non-default shape
        if (visualParamWeightsAreDefault() && mRuthTimer.getElapsedTimeF32() > LOADING_TIMEOUT_SECONDS)
        {
            // re-request appearance, hoping that it comes back with a shape next time
            LL_INFOS() << "Re-requesting AvatarAppearance for object: "  << getID() << LL_ENDL;
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarTexturesRequest(getID());
            mRuthTimer.reset();
        }
        else
        {
            LL_INFOS() << "That's okay, we already have a non-default shape for object: "  << getID() << LL_ENDL;
            // we don't really care.
        }
    }

    if (contents.mHoverOffsetWasSet && !isSelf())
    {
        // Got an update for some other avatar
        // Ignore updates for self, because we have a more authoritative value in the preferences.
        setHoverOffset(contents.mHoverOffset);
        LL_DEBUGS("Avatar") << avString() << "setting hover to " << contents.mHoverOffset[2] << LL_ENDL;
    }

    if (!contents.mHoverOffsetWasSet && !isSelf())
    {
        // If we don't get a value at all, we are presumably in a
        // region that does not support hover height.
        LL_WARNS() << avString() << "zeroing hover because not defined in appearance message" << LL_ENDL;
        setHoverOffset(LLVector3(0.0, 0.0, 0.0));
    }

    setCompositeUpdatesEnabled( true );

    // If all of the avatars are completely baked, release the global image caches to conserve memory.
    cullAvatarsByPixelArea();

    if (isSelf())
    {
        mUseLocalAppearance = false;
    }

    updateMeshTextures();
    updateMeshVisibility();
}

LLViewerTexture* LLVOAvatar::getBakedTexture(const U8 te)
{
    //<FS:Beq> BOM constrain number of bake requests when BOM not supported
    // prior to BOM BAKES beyond BAKED_HAIR were not supported.
    // if (te < 0 || te >= BAKED_NUM_INDICES)
    if (te < 0 || te >= getNumBakes())
    //</FS:Beq>
    {
        return NULL;
    }

    bool is_layer_baked = isTextureDefined(mBakedTextureDatas[te].mTextureIndex);

    LLViewerTexLayerSet* layerset = NULL;
    layerset = getTexLayerSet(te);


    if (!isEditingAppearance() && is_layer_baked)
    {
        LLViewerFetchedTexture* baked_img = LLViewerTextureManager::staticCastToFetchedTexture(getImage(mBakedTextureDatas[te].mTextureIndex, 0), true);
        return baked_img;
    }
    else if (layerset && isEditingAppearance())
    {
        layerset->createComposite();
        layerset->setUpdatesEnabled(true);

        return layerset->getViewerComposite();
    }

    return NULL;


}


//------------------------------------------------------------------------
// <FS> PLAN-avatars.md stage B0: the avatar's world matrices as a flat array by joint number, once per avatar per frame (the palettes of all its skins read it), and the flat world pass checked against it
//------------------------------------------------------------------------
struct LLFlatSkeleton
{
    U32 mFrame = ~0u;
    std::vector<LLMatrix4a> mWorld;         // by joint number: the joint's world matrix (what getWorldMatrix4a() gives)
    std::vector<U8> mHas;                   // the joint exists
    std::vector<LLJoint*> mJoint;
    std::vector<S16> mParent;               // the parent's joint number (-1: none, or a parent that is not in the array)
    std::vector<S16> mOrder;                // the joints, parents before children
    U32 mJointCount = 0;
};

static int fs_skelflat_mode()
{
    static const int mode = getenv("LL_VK_SKELFLAT") ? atoi(getenv("LL_VK_SKELFLAT")) : 1;       // 1 (default since 2026-11-07: laps sf2 / sf3, palettes bit-equal, Adam: looks good) the palettes from the flat array, 0 the old build, 2 both compared
    return mode;
}

namespace
{
std::atomic<U64> sFlatPalettes{ 0 }, sFlatPalettesDiffer{ 0 }, sFlatJoints{ 0 }, sFlatJointsDiffer{ 0 }, sFlatFallbacks{ 0 }, sFlatFills{ 0 };
std::atomic<U32> sFlatLast{ 0 };
std::atomic<U32> sFlatWorstMilli{ 0 };
}

static void fs_flat_report()
{
    if (gFrameCount >= sFlatLast.load() + 600)
    {
        const double frames = (double)std::max<U32>(1, gFrameCount - sFlatLast.load());
        sFlatLast = gFrameCount;
        LL_INFOS("SkelFlat") << "flat skeleton per frame over " << (U32)frames << " frames: world arrays filled " << sFlatFills.exchange(0) / frames << ", palettes made from the array " << sFlatPalettes.load() / frames
            << " (not bit-equal to the old build: " << sFlatPalettesDiffer.load() << "), fell back to the old build " << sFlatFallbacks.exchange(0) / frames << "; flat world pass joints checked " << sFlatJoints.load() / frames
            << " (differ by more than 1e-5: " << sFlatJointsDiffer.load() << ", worst " << sFlatWorstMilli.exchange(0) / 1.0e6 << ")" << LL_ENDL;
        sFlatPalettes = sFlatPalettesDiffer = sFlatJoints = sFlatJointsDiffer = 0;
    }
}

static void fs_flat_fill(LLVOAvatar* av, LLFlatSkeleton& flat)
{
    const U32 N = LL_CHARACTER_MAX_ANIMATED_JOINTS;
    if (flat.mWorld.size() != N)
    {
        flat.mWorld.assign(N, LLMatrix4a());
        flat.mHas.assign(N, 0);
        flat.mJoint.assign(N, nullptr);
        flat.mParent.assign(N, -1);
        flat.mJointCount = 0;
    }
    bool structure_changed = false;
    for (U32 k = 0; k < N; ++k)
    {
        LLJoint* joint = av->getJoint((S32)k);
        if (joint != flat.mJoint[k])
        {
            flat.mJoint[k] = joint;
            structure_changed = true;
        }
        flat.mHas[k] = joint ? 1 : 0;
        if (joint)
        {
            flat.mWorld[k] = joint->getWorldMatrix4a();
        }
    }
    if (structure_changed)
    {
        // the parents by joint number, and the order (parents first)
        flat.mOrder.clear();
        for (U32 k = 0; k < N; ++k)
        {
            flat.mParent[k] = -1;
            if (flat.mJoint[k] && flat.mJoint[k]->getParent())
            {
                for (U32 p = 0; p < N; ++p)
                {
                    if (flat.mJoint[p] == flat.mJoint[k]->getParent())
                    {
                        flat.mParent[k] = (S16)p;
                        break;
                    }
                }
            }
        }
        std::vector<U8> done(N, 0);
        size_t placed = 0, before = ~(size_t)0;
        while (placed != before)
        {
            before = placed;
            for (U32 k = 0; k < N; ++k)
            {
                if (flat.mJoint[k] && !done[k] && (flat.mParent[k] < 0 || done[flat.mParent[k]]))
                {
                    done[k] = 1;
                    flat.mOrder.push_back((S16)k);
                    ++placed;
                }
            }
        }
        flat.mJointCount = (U32)placed;
    }
    ++sFlatFills;
}

// the world pass from the joints' LOCAL transforms, exactly what LLXformMatrix::update / updateMatrix do (the world position composes with the parent's scale only when the parent's scale-child-offset
// flag is set; the world matrix has the joint's own scale, the world rotation and the world position), compared with the world matrices the joints gave
static void fs_flat_pass_check(LLFlatSkeleton& flat)
{
    const U32 N = (U32)flat.mWorld.size();
    std::vector<LLVector3> wpos(N);
    std::vector<LLQuaternion> wrot(N);
    for (S16 k : flat.mOrder)
    {
        LLJoint* joint = flat.mJoint[k];
        LLXformMatrix* xf = joint->getXform();
        const S16 p = flat.mParent[k];
        if (p >= 0)
        {
            LLJoint* parent = flat.mJoint[p];
            LLVector3 pos = xf->getPosition();
            if (parent->getXform()->getScaleChildOffset())
            {
                pos.scaleVec(parent->getXform()->getScale());
            }
            pos *= wrot[p];
            pos += wpos[p];
            wpos[k] = pos;
            wrot[k] = xf->getRotation() * wrot[p];
        }
        else if (joint->getParent())
        {
            // a parent that is not in the array (the avatar's root joint above the skeleton): its world state is the start of the chain, taken from the joint object
            LLJoint* parent = joint->getParent();
            LLVector3 pos = xf->getPosition();
            if (parent->getXform()->getScaleChildOffset())
            {
                pos.scaleVec(parent->getXform()->getScale());
            }
            const LLQuaternion parent_rot = parent->getWorldRotation();
            pos *= parent_rot;
            pos += parent->getWorldPosition();
            wpos[k] = pos;
            wrot[k] = xf->getRotation() * parent_rot;
        }
        else
        {
            wpos[k] = xf->getPosition();
            wrot[k] = xf->getRotation();
        }
        LLMatrix4 m;
        m.initAll(xf->getScale(), wrot[k], wpos[k]);
        LLMatrix4a ma;
        ma.loadu(m);
        const F32* a = ma.getF32ptr();
        const F32* b = flat.mWorld[k].getF32ptr();
        F32 worst = 0.f;
        for (int i = 0; i < 16; ++i)
        {
            worst = std::max(worst, fabsf(a[i] - b[i]));
        }
        ++sFlatJoints;
        if (!(worst <= 1e-5f))
        {
            ++sFlatJointsDiffer;
            static std::atomic<U32> said{ 0 };
            if (said++ < 8)
            {
                LL_INFOS("SkelFlat") << "flat world pass differs: joint " << k << " '" << joint->getName() << "' parent number " << p << " (" << (joint->getParent() ? joint->getParent()->getName() : std::string("none")) << "), worst " << worst
                    << "; flat translation (" << a[12] << ", " << a[13] << ", " << a[14] << ") joint's (" << b[12] << ", " << b[13] << ", " << b[14] << "); local position " << xf->getPosition() << LL_ENDL;
            }
        }
        const U32 milli = (U32)std::min<F32>(worst * 1.0e6f, 4.0e9f);
        U32 seen = sFlatWorstMilli.load();
        while (milli > seen && !sFlatWorstMilli.compare_exchange_weak(seen, milli)) {}
    }
}

void LLVOAvatar::flatPalette(LLMatrix4a* mat, S32 count, const LLMeshSkinInfo* skin)
{
    const int mode = fs_skelflat_mode();
    if (mode == 0)
    {
        LLSkinningUtil::initSkinningMatrixPalette(mat, count, skin, this);
        return;
    }
    // (called under the palette mutex of updateSkinInfoMatrixPalette: one at a time)
    if (!mFlatSkel)
    {
        mFlatSkel = std::make_shared<LLFlatSkeleton>();
    }
    LLFlatSkeleton& flat = *mFlatSkel;
    if (flat.mFrame != gFrameCount)
    {
        fs_flat_fill(this, flat);
        flat.mFrame = gFrameCount;
        if (mode == 2)
        {
            fs_flat_pass_check(flat);
        }
    }
    LLSkinningUtil::initJointNums(const_cast<LLMeshSkinInfo*>(skin), this);
    const bool ok = LLSkinningUtil::initSkinningMatrixPaletteFlat(mat, count, skin, flat.mWorld.data(), flat.mHas.data(), (S32)flat.mWorld.size());
    if (!ok || mode == 2)
    {
        LLMeshSkinInfo::matrix_list_t reference(count);
        LLSkinningUtil::initSkinningMatrixPalette(reference.data(), count, skin, this);
        if (ok)
        {
            ++sFlatPalettes;
            if (memcmp(reference.data(), mat, sizeof(LLMatrix4a) * (size_t)count) != 0)
            {
                ++sFlatPalettesDiffer;
            }
        }
        else
        {
            ++sFlatFallbacks;
        }
        if (!ok || mode == 2)
        {
            memcpy(mat, reference.data(), sizeof(LLMatrix4a) * (size_t)count);      // (the old result stands in compare mode, and where the array lacks a joint)
        }
    }
    else
    {
        ++sFlatPalettes;
    }
    fs_flat_report();
}

// <FS> PLAN-avatars.md stage B3a: LL_VK_PALJOB 1 (default) the palettes are rebuilt by a pool job per avatar after the frame's completions, 0 only on request, 2 job and compared with a rebuild at the first request
static int fs_paljob_mode()
{
    static const int mode = getenv("LL_VK_PALJOB") ? atoi(getenv("LL_VK_PALJOB")) : 1;
    return mode;
}

namespace
{
// LL_VK_PALTIME=1: where the palette time goes. 0 built on request, 1 built by the job; per frame: builds, build time (all threads), time waiting for the global palette lock, hits
std::atomic<U64> sPalBuildNs[2], sPalBuilds[2], sPalWaitNs{ 0 }, sPalRequests2{ 0 }, sPalHits{ 0 }, sPalJobWallNs{ 0 }, sPalJobRuns{ 0 };
std::atomic<U64> sPalCompared{ 0 }, sPalDiffer{ 0 }, sPalSkippedUninit{ 0 }, sPalStaleByTouch{ 0 }, sPalDifferUntouched{ 0 };
std::atomic<U32> sPalJobTouchSerial{ 0 };       // LLJoint::sTouchSerial when the palette jobs of the last batch were done: a joint touched after that makes the job's palettes suspect for the frame
std::atomic<U32> sPalTimeLast{ 0 };
bool fs_paltime_on()
{
    static const bool on = getenv("LL_VK_PALTIME") != nullptr;
    return on;
}
inline U64 fs_pal_now()
{
    return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void fs_paltime_report()
{
    if (gFrameCount >= sPalTimeLast.load() + 600)
    {
        const double frames = (double)std::max<U32>(1, gFrameCount - sPalTimeLast.load());
        sPalTimeLast = gFrameCount;
        LL_INFOS("PalTime") << "palettes a frame over " << (U32)frames << " frames: requests " << sPalRequests2.exchange(0) / frames << " (hits " << sPalHits.exchange(0) / frames << "); built on request " << sPalBuilds[0].load() / frames
            << " in " << sPalBuildNs[0].exchange(0) / 1.0e6 / frames << " ms (all threads), built by the job " << sPalBuilds[1].load() / frames << " in " << sPalBuildNs[1].exchange(0) / 1.0e6 / frames
            << " ms (all threads), job wall " << sPalJobWallNs.exchange(0) / 1.0e6 / frames << " ms; waiting for the global palette lock " << sPalWaitNs.exchange(0) / 1.0e6 / frames
            << " ms (all threads); job palettes rebuilt at the request because a joint was touched after the job " << sPalStaleByTouch.exchange(0) / frames << " a frame; job palettes compared with a rebuild " << sPalCompared.load()
            << ", different " << sPalDiffer.load() << " (of them with no joint touched after the job: " << sPalDifferUntouched.load() << "), entries skipped (skin joint numbers not initialised) " << sPalSkippedUninit.load() << LL_ENDL;
        sPalBuilds[0] = sPalBuilds[1] = 0;
    }
}
}

const LLVOAvatar::MatrixPaletteCache& LLVOAvatar::updateSkinInfoMatrixPalette(const LLMeshSkinInfo* skin)
{
    // <FS> the packet builders of the shadow cascades run on pool threads and ask for palettes at the same time: the lazy cache (and the lazy joint matrices and joint numbers it fills in) is one at a time.
    // The entry returned is not written again in this frame, so the callers read it without the lock.
    static std::mutex palette_mutex;
    const bool timed = fs_paltime_on();
    // <FS> stage B3b: a request that finds the palette ready (the job built it, or an earlier request) reads the avatar's cache under a shared lock and never touches the global lock
    {
        const U64 shared_t0 = timed ? fs_pal_now() : 0;
        std::shared_lock<std::shared_mutex> shared_lock(mPaletteMutex);
        if (timed)
        {
            sPalWaitNs += fs_pal_now() - shared_t0;
            ++sPalRequests2;
        }
        auto found = mMatrixPaletteCache.find(skin->mHash);
        // (a palette the job built and nobody has asked for yet (mJobFor == this frame) goes the long way once: that first request decides, and from then on every consumer of the frame gets the same palette)
        if (found != mMatrixPaletteCache.end() && found->second.mFrame == gFrameCount && found->second.mJobFor != gFrameCount)
        {
            if (timed)
            {
                ++sPalHits;
                fs_paltime_report();
            }
            return found->second;
        }
    }
    const U64 wait_t0 = timed ? fs_pal_now() : 0;
    std::lock_guard<std::mutex> palette_lock(palette_mutex);
    std::unique_lock<std::shared_mutex> avatar_lock(mPaletteMutex);
    if (timed)
    {
        sPalWaitNs += fs_pal_now() - wait_t0;
    }

    U64 hash = skin->mHash;
    MatrixPaletteCache& entry = mMatrixPaletteCache[hash];

    if (entry.mFrame == gFrameCount && entry.mJobFor == gFrameCount && LLJoint::sTouchSerial != sPalJobTouchSerial.load(std::memory_order_relaxed))
    {
        // <FS> stage B3b: a joint was touched after the palette jobs (compare lap pl1 caught one avatar's joint changing between the job and the render, 12 times in 6.4 M): the job's palette may be a frame old, rebuild it
        // now from the skeleton as it is (the motion history was already moved on by the job)
        ++sPalStaleByTouch;
        entry.mJobFor = 0xFFFFFFFFu;
        entry.mSkin = skin;
        buildPaletteEntry(entry, skin, gFrameCount, true, false);
        return entry;
    }
    if (entry.mFrame == gFrameCount)
    {
        if (timed)
        {
            ++sPalHits;
            fs_paltime_report();
        }
        if (entry.mJobFor == gFrameCount && fs_paljob_mode() == 2 && !entry.mJobChecked && entry.mSkin.notNull())
        {
            // compare mode: the palette the job built against a rebuild now, bit for bit (matrices, as B0)
            entry.mJobChecked = true;
            const U32 count = LLSkinningUtil::getMeshJointCount(skin);
            LLMeshSkinInfo::matrix_list_t scratch(count);
            if (mFlatSkel)
            {
                mFlatSkel->mFrame = ~0u;
            }
            flatPalette(scratch.data(), (S32)count, skin);
            ++sPalCompared;
            if (scratch.size() != entry.mMatrixPalette.size() || memcmp(scratch.data(), entry.mMatrixPalette.data(), sizeof(LLMatrix4a) * (size_t)count) != 0)
            {
                ++sPalDifferUntouched;      // (the touch serial is unchanged since the job: nothing the serial sees explains it)
                if (++sPalDiffer <= 8)
                {
                    U32 first = 0;
                    for (; first < count && first < entry.mMatrixPalette.size() && memcmp(&scratch[first], &entry.mMatrixPalette[first], sizeof(LLMatrix4a)) == 0; ++first) {}
                    LL_WARNS("PalTime") << "job palette differs from a rebuild: avatar " << getFullname() << (isSelf() ? " (own)" : "") << (isControlAvatar() ? " (animated object)" : "") << (isSitting() ? " (sitting)" : "") << ", skin " << hash << ", " << count
                        << " joints, first different joint " << first << LL_ENDL;
                }
            }
        }
        // the first request of the frame has decided: the entry is frozen for the frame, every later consumer (the shadow cascades, the main view, the velocity pass) gets this very palette. (Walking shadow glitch of
        // 2026-11-07: a guard that rebuilt on a later request gave the cascades the job's palette and the main view a newer one, a frame apart)
        entry.mJobFor = 0xFFFFFFFFu;
    }
    else
    {
        entry.mSkin = skin;
        // <FS> motion vectors: last frame's palette becomes the previous one, but only when the entry was computed in the frame just before (an avatar that was not
        // asked for in between has no valid history: previous = current, no motion)
        const bool had_last_frame = entry.mComputed && entry.mFrame == gFrameCount - 1;
        const U64 build_t0 = timed ? fs_pal_now() : 0;
        buildPaletteEntry(entry, skin, gFrameCount, had_last_frame, had_last_frame);
        if (timed)
        {
            sPalBuildNs[0] += fs_pal_now() - build_t0;
            ++sPalBuilds[0];
            fs_paltime_report();
        }
    }
    return entry;
}

void LLVOAvatar::sPalJobDone()
{
    sPalJobTouchSerial.store(LLJoint::sTouchSerial, std::memory_order_relaxed);
}
void LLVOAvatar::sPalJobWallNsAdd(U64 ns)
{
    sPalJobWallNs += ns;
    ++sPalJobRuns;
}

// the pool job of one avatar (after the completions of the frame): rebuilds the palettes it had asked for in the last rendered frame, for the frame about to be rendered
void LLVOAvatar::buildPalettesInJob()
{
    const int mode = fs_paljob_mode();
    if (mode == 0 || isDead() || mMatrixPaletteCache.empty())
    {
        return;
    }
    const bool timed = fs_paltime_on();
    const U32 target = gFrameCount + 1;     // (gFrameCount is advanced at the start of display(), after this idle: the request of the render finds mFrame == its own frame)
    std::unique_lock<std::shared_mutex> avatar_lock(mPaletteMutex);
    if (mFlatSkel)
    {
        mFlatSkel->mFrame = ~0u;            // (the world array is filled again from the final skeleton of this frame)
    }
    for (auto& kv : mMatrixPaletteCache)
    {
        MatrixPaletteCache& entry = kv.second;
        // an entry asked for in the last rendered frame (mFrame == gFrameCount), or already rebuilt for the coming one by an earlier run of this job (display did not come in between)
        if (!entry.mComputed || entry.mSkin.isNull() || (entry.mFrame != gFrameCount && entry.mFrame != target))
        {
            continue;
        }
        const LLMeshSkinInfo* skin = entry.mSkin.get();
        if (!skin->mJointNumsInitialized)
        {
            ++sPalSkippedUninit;            // (initJointNums writes the skin, which avatars share: only the lazy path under the global lock does that)
            continue;
        }
        const bool again = entry.mFrame == target;
        const U64 t0 = timed ? fs_pal_now() : 0;
        buildPaletteEntry(entry, skin, target, true, !again);
        entry.mJobFor = target;
        entry.mJobChecked = false;
        if (timed)
        {
            sPalBuildNs[1] += fs_pal_now() - t0;
            ++sPalBuilds[1];
        }
    }
    if (mFlatSkel)
    {
        mFlatSkel->mFrame = ~0u;
    }
}

void LLVOAvatar::buildPaletteEntry(MatrixPaletteCache& entry, const LLMeshSkinInfo* skin, U32 stamp, bool had_last_frame, bool swap_prev)
{
    const U64 hash = skin->mHash;
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

        if (swap_prev)
        {
            entry.mPrevGLMp.swap(entry.mGLMp);
        }
        entry.mListSlot.store(~0ull, std::memory_order_relaxed);        // (a new palette: its place in the frame's shared list is not valid)
        entry.mFrame = stamp;
        entry.mComputed = true;
        if (skin->mBadBindCount > 0 && !entry.mBadBindNamed)
        {
            // <FS> once per avatar and mesh: who wears a mesh whose inverse bind matrices the parse refused (the identity stands in, so nothing else ever names the wearer; MeshSkin, 2026-11-05)
            entry.mBadBindNamed = true;
            LL_WARNS("MeshSkin") << "avatar " << getFullname() << " " << getID() << " wears mesh " << skin->mMeshID << " with " << skin->mBadBindCount << " of " << skin->mInvBindMatrix.size()
                                 << " inverse bind matrices refused at the parse (the identity stands in); joints " << skin->mJointNames.size() << LL_ENDL;
        }

        //build matrix palette
        U32 count = LLSkinningUtil::getMeshJointCount(skin);
        entry.mMatrixPalette.resize(count);
        flatPalette(&(entry.mMatrixPalette[0]), (S32)count, skin);

        const LLMatrix4a* mat = &(entry.mMatrixPalette[0]);

        entry.mGLMp.resize(count * 12);

        F32* mp = &(entry.mGLMp[0]);

        for (U32 i = 0; i < count; ++i)
        {
            F32* m = (F32*)mat[i].mMatrix[0].getF32ptr();

            U32 idx = i * 12;

            mp[idx + 0] = m[0];
            mp[idx + 1] = m[1];
            mp[idx + 2] = m[2];
            mp[idx + 3] = m[12];

            mp[idx + 4] = m[4];
            mp[idx + 5] = m[5];
            mp[idx + 6] = m[6];
            mp[idx + 7] = m[13];

            mp[idx + 8] = m[8];
            mp[idx + 9] = m[9];
            mp[idx + 10] = m[10];
            mp[idx + 11] = m[14];
        }

        // <FS> the palette catcher (2026-11-04: spike shadows from random avatars with ray traced shadows, and the jumping triangles of an animated attachment): the draw and the ray scene's
        // skinning both read this palette, so a joint matrix that is not finite, or whose translation lies farther than kPaletteReach from the avatar, is logged with what says where it came
        // from (the avatar, whether it is an animated object, whether it sits, the thread, the joint, the frame). Nothing is changed here: what the picture shows lines up with the line.
        // The ray scene has its own catcher over the skinned vertices (llvkrt.cpp, "skin catcher").
        static std::atomic<U32> palette_flagged{0};
        if ((gFrameCount & 3u) == 0u)       // every fourth frame: a persistent fault is caught all the same, and the scan of every joint of every palette stays off the frame's budget
        {
            static std::atomic<U32> palette_checked{0};
            ++palette_checked;
            constexpr F32 kPaletteReach = 48.f;
            const LLVector3 at = getRenderPosition();
            U32 bad_joint = ~0u;
            F32 bad_t[3] = { 0.f, 0.f, 0.f };
            F32 worst = 0.f;
            bool non_finite = false;
            for (U32 i = 0; i < count; ++i)
            {
                const F32* row = mp + i * 12;
                bool finite = true;
                for (U32 k = 0; k < 12; ++k)
                {
                    finite = finite && std::isfinite(row[k]);
                }
                const F32 tx = row[3], ty = row[7], tz = row[11];
                const F32 d = finite ? sqrtf((tx - at.mV[0]) * (tx - at.mV[0]) + (ty - at.mV[1]) * (ty - at.mV[1]) + (tz - at.mV[2]) * (tz - at.mV[2])) : 0.f;
                if (!finite || d > kPaletteReach)
                {
                    if (bad_joint == ~0u || !finite || d > worst)
                    {
                        bad_joint = i;
                        bad_t[0] = tx; bad_t[1] = ty; bad_t[2] = tz;
                        worst = d;
                        non_finite = non_finite || !finite;
                    }
                }
            }
            if (bad_joint != ~0u)
            {
                const U32 n = ++palette_flagged;
                if (n <= 60 || n % 200 == 0)
                {
                    // which joint of the skeleton, what it holds, where its chain goes, and whether anything in the skeleton has an absurd scale or position (2026-11-04: the bad palettes of one
                    // avatar were the same three skin joints on every frame, one all zeros and two enormous: a scale of zero or of millions somewhere in the chain, from the avatar's own data)
                    std::ostringstream detail;
                    const S32 joint_num = skin->mJointNums.size() > bad_joint ? skin->mJointNums[bad_joint] : -1;
                    LLJoint* joint = getJoint(joint_num);
                    detail << "; skin joint '" << (skin->mJointNames.size() > bad_joint ? skin->mJointNames[bad_joint] : std::string("?")) << "' num " << joint_num
                           << (joint_num < 0 ? " (not found)" : joint_num < mNumBones ? " (bone)" : joint_num < mNumBones + mNumCollisionVolumes ? " (collision volume)" : " (attachment point)");
                    if (skin->mInvBindMatrix.size() > bad_joint)
                    {
                        const LLVector4a& t = skin->mInvBindMatrix[bad_joint].getTranslation();
                        detail << "; inverse bind translation (" << t[0] << ", " << t[1] << ", " << t[2] << ")";
                    }
                    if (skin->mAlternateBindMatrix.size() > bad_joint)
                    {
                        const LLVector4a& t = skin->mAlternateBindMatrix[bad_joint].getTranslation();
                        detail << "; alternate bind translation (" << t[0] << ", " << t[1] << ", " << t[2] << ")";
                    }
                    {
                        // the skin info (2026-11-04: the joints were fine, the inverse bind matrices were garbage that drifted: constant data read from memory someone else owns?)
                        const LLMeshSkinInfo* live = gMeshRepo.getSkinInfo(skin->mMeshID);
                        detail << "; skin info " << (const void*)skin << " refs " << skin->getNumRefs() << " mesh " << skin->mMeshID << " names " << skin->mJointNames.size() << " inverse binds " << skin->mInvBindMatrix.size()
                               << " alternate binds " << skin->mAlternateBindMatrix.size() << "; the mesh repository's current skin info for that mesh: " << (live == skin ? "the same object" : live ? "a DIFFERENT object" : "none");
                        if (!skin->mInvBindMatrix.empty())
                        {
                            const LLVector4a& t0 = skin->mInvBindMatrix[0].getTranslation();
                            detail << "; inverse bind 0 translation (" << t0[0] << ", " << t0[1] << ", " << t0[2] << ")";
                        }
                    }
                    if (joint)
                    {
                        detail << "; joint '" << joint->getName() << "' position " << joint->getPosition() << " scale " << joint->getScale() << " world " << joint->getWorldPosition()
                               << " dirty 0x" << std::hex << joint->mDirtyFlags << std::dec << (joint->mUpdateXform ? "" : " (no xform update)");
                        LLVector3 ov;
                        LLUUID mesh;
                        if (joint->hasAttachmentPosOverride(ov, mesh))
                        {
                            detail << "; position override " << ov << " from mesh " << mesh;
                        }
                        if (joint->hasAttachmentScaleOverride(ov, mesh))
                        {
                            detail << "; scale override " << ov << " from mesh " << mesh;
                        }
                        detail << "; chain:";
                        int depth = 0;
                        for (LLJoint* pj = joint->getParent(); pj && depth < 12; pj = pj->getParent(), ++depth)
                        {
                            detail << " " << pj->getName() << " scale " << pj->getScale();
                        }
                    }
                    detail << "; skeleton joints off:";
                    U32 off = 0;
                    auto note_off = [&](LLJoint* j)
                    {
                        if (!j)
                        {
                            return;
                        }
                        const LLVector3& sc = j->getScale();
                        const LLVector3& po = j->getPosition();
                        bool odd = !sc.isFinite() || !po.isFinite();
                        for (int k = 0; k < 3 && !odd; ++k)
                        {
                            odd = sc.mV[k] < 1e-3f || sc.mV[k] > 1e3f || fabsf(po.mV[k]) > 100.f;
                        }
                        if (odd && off++ < 8)
                        {
                            detail << " " << j->getName() << " (pos " << po << " scale " << sc << ")";
                        }
                    };
                    for (LLAvatarJoint* j : mSkeleton)
                    {
                        note_off(j);
                    }
                    for (S32 i = 0; i < mNumCollisionVolumes; ++i)
                    {
                        note_off(&mCollisionVolumes[i]);
                    }
                    detail << (off ? "" : " none") << "; rez status " << getRezzedStatus() << (isFullyLoaded() ? ", fully loaded" : ", loading");
                    LL_WARNS("PaletteCatch") << "bad rigged palette: frame " << gFrameCount << ", avatar " << getFullname() << " " << getID() << (isControlAvatar() ? " (animated object)" : "")
                                             << (isSitting() ? " (sitting)" : "") << " at " << at << ", skin " << hash << " with " << count << " joints: joint " << bad_joint << (non_finite ? " NOT FINITE," : "")
                                             << " translation (" << bad_t[0] << ", " << bad_t[1] << ", " << bad_t[2] << ") " << worst << " m from the avatar; on the "
                                             << (on_main_thread() ? "main thread" : "pool thread") << "; " << n << " flagged of " << (U32)palette_checked << " palettes" << detail.str() << LL_ENDL;
                }
            }
        }
        // <FS> motion vectors: a history from last frame is only usable when the joint count is the same (the skin changed otherwise)
        entry.mHasPrev = had_last_frame && entry.mPrevGLMp.size() == entry.mGLMp.size();
        if (!entry.mHasPrev)
        {
            entry.mPrevGLMp = entry.mGLMp;
        }
        // LL_VK_PALSTATS=1: how much of the rigged palettes changes from one frame to the next (what a skinning cache could keep). Per palette (avatar and skin) and per joint matrix, exact and within 1e-4;
        // per avatar: the avatars whose palettes are all unchanged. Logged every 600 frames in which a palette was computed.
        {
            static const bool pal_stats = getenv("LL_VK_PALSTATS") != nullptr;
            if (pal_stats)
            {
                struct PalStats
                {
                    U32 mFrame = 0;
                    std::map<const LLVOAvatar*, std::pair<U32, U32>> mAvatars;      // this frame: palettes with history, of them unchanged (within 1e-4)
                    U64 mFrames = 0, mPalettes = 0, mNoHistory = 0, mSame = 0, mSameTol = 0, mJoints = 0, mJointsSame = 0, mJointsSameTol = 0, mAvatarSum = 0, mAvatarsSame = 0, mAvatarsWithHistory = 0;
                };
                static PalStats s;
                if (s.mFrame != gFrameCount)
                {
                    for (const auto& kv : s.mAvatars)
                    {
                        ++s.mAvatarSum;
                        if (kv.second.first)
                        {
                            ++s.mAvatarsWithHistory;
                            s.mAvatarsSame += kv.second.first == kv.second.second ? 1 : 0;
                        }
                    }
                    if (!s.mAvatars.empty() && ++s.mFrames >= 600)
                    {
                        const double f = (double)s.mFrames;
                        LL_INFOS("VkPalStats") << "rigged palettes a frame " << (double)s.mPalettes / f << " (" << (double)s.mNoHistory / f << " without history), avatars " << (double)s.mAvatarSum / f
                            << " (" << (double)s.mAvatarsWithHistory / f << " with history, " << (double)s.mAvatarsSame / f << " with every palette unchanged within 1e-4); palettes unchanged exactly "
                            << (double)s.mSame / f << ", within 1e-4 " << (double)s.mSameTol / f << "; joint matrices a frame " << (double)s.mJoints / f << ", unchanged exactly " << (double)s.mJointsSame / f
                            << ", within 1e-4 " << (double)s.mJointsSameTol / f << LL_ENDL;
                        s.mFrames = s.mPalettes = s.mNoHistory = s.mSame = s.mSameTol = s.mJoints = s.mJointsSame = s.mJointsSameTol = s.mAvatarSum = s.mAvatarsSame = s.mAvatarsWithHistory = 0;
                    }
                    s.mAvatars.clear();
                    s.mFrame = gFrameCount;
                }
                ++s.mPalettes;
                auto& av = s.mAvatars[this];
                if (!entry.mHasPrev)
                {
                    ++s.mNoHistory;
                }
                else
                {
                    bool all_same = true, all_tol = true;
                    for (U32 i = 0; i < count; ++i)
                    {
                        const F32* a = &entry.mGLMp[(size_t)i * 12];
                        const F32* b = &entry.mPrevGLMp[(size_t)i * 12];
                        bool same = true, tol = true;
                        for (U32 k = 0; k < 12; ++k)
                        {
                            same = same && a[k] == b[k];
                            tol = tol && fabsf(a[k] - b[k]) < 1e-4f;
                        }
                        ++s.mJoints;
                        s.mJointsSame += same ? 1 : 0;
                        s.mJointsSameTol += tol ? 1 : 0;
                        all_same = all_same && same;
                        all_tol = all_tol && tol;
                    }
                    s.mSame += all_same ? 1 : 0;
                    s.mSameTol += all_tol ? 1 : 0;
                    ++av.first;
                    av.second += all_tol ? 1 : 0;
                }
            }
        }
        {
            static std::atomic<U32> with_history{0}, without_history{0};
            ++(entry.mHasPrev ? with_history : without_history);
            const U32 total = with_history + without_history;
            if (total % 20000 == 0)
            {
                LL_INFOS("VkMotion") << "rigged palettes computed: " << with_history << " with a previous palette, " << without_history << " without (new, or not asked for in the last frame); flagged by the palette catcher " << (U32)palette_flagged << LL_ENDL;
            }
        }
    }
}

// static
void LLVOAvatar::getAnimLabels( std::vector<std::string>* labels )
{
    S32 i;
    labels->reserve(gUserAnimStatesCount);
    for( i = 0; i < gUserAnimStatesCount; i++ )
    {
        labels->push_back( LLAnimStateLabels::getStateLabel( gUserAnimStates[i].mName ) );
    }

    // Special case to trigger away (AFK) state
    labels->push_back( "Away From Keyboard" );
}

// static
void LLVOAvatar::getAnimNames( std::vector<std::string>* names )
{
    S32 i;

    names->reserve(gUserAnimStatesCount);
    for( i = 0; i < gUserAnimStatesCount; i++ )
    {
        names->push_back( std::string(gUserAnimStates[i].mName) );
    }

    // Special case to trigger away (AFK) state
    names->push_back( "enter_away_from_keyboard_state" );
}

// static
void LLVOAvatar::onBakedTextureMasksLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata )
{
    if (!userdata) return;

    //LL_INFOS() << "onBakedTextureMasksLoaded: " << src_vi->getID() << LL_ENDL;
    const LLUUID id = src_vi->getID();

    LLTextureMaskData* maskData = (LLTextureMaskData*) userdata;
    LLVOAvatar* self = (LLVOAvatar*) gObjectList.findObject( maskData->mAvatarID );

    // if discard level is 2 less than last discard level we processed, or we hit 0,
    // then generate morph masks
    if(self && success && (discard_level < maskData->mLastDiscardLevel - 2 || discard_level == 0))
    {
        if(aux_src && aux_src->getComponents() == 1)
        {
            LLImageDataSharedLock lock(aux_src);

            if (!aux_src->getData())
            {
                // <FS:Ansariel> FIRE-16122: Don't crash if we didn't receive any data
                //LL_ERRS() << "No auxiliary source (morph mask) data for image id " << id << LL_ENDL;
                LL_WARNS() << "No auxiliary source (morph mask) data for image id " << id << LL_ENDL;
                // </FS:Ansariel>
                return;
            }

            U32 gl_name;
            LLImageTex::generateTextures(1, &gl_name );

            gRender.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, gl_name);

            LLImageTex::setManualImage(
                gl_name, TextureTarget::Tex2D, 0, TexelFormat::Alpha8,
                aux_src->getWidth(), aux_src->getHeight(),
                PixelFormat::Alpha, ComponentType::U8, aux_src->getData());

            gRender.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);

            /* if( id == head_baked->getID() )
                 if (self->mBakedTextureDatas[BAKED_HEAD].mTexLayerSet)
                     //LL_INFOS() << "onBakedTextureMasksLoaded for head " << id << " discard = " << discard_level << LL_ENDL;
                     self->mBakedTextureDatas[BAKED_HEAD].mTexLayerSet->applyMorphMask(aux_src->getData(), aux_src->getWidth(), aux_src->getHeight(), 1);
                     maskData->mLastDiscardLevel = discard_level; */
            bool found_texture_id = false;
            for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
                 iter != LLAvatarAppearance::getDictionary()->getTextures().end();
                 ++iter)
            {

                const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
                if (texture_dict->mIsUsedByBakedTexture)
                {
                    const ETextureIndex texture_index = iter->first;
                    const LLViewerTexture *baked_img = self->getImage(texture_index, 0);
                    if (baked_img && id == baked_img->getID())
                    {
                        const EBakedTextureIndex baked_index = texture_dict->mBakedTextureIndex;
                        self->applyMorphMask(aux_src->getData(), aux_src->getWidth(), aux_src->getHeight(), 1, baked_index);
                        maskData->mLastDiscardLevel = discard_level;
                        if (self->mBakedTextureDatas[baked_index].mMaskTexName)
                        {
                            LLImageTex::deleteTextures(1, &(self->mBakedTextureDatas[baked_index].mMaskTexName));
                        }
                        self->mBakedTextureDatas[baked_index].mMaskTexName = gl_name;
                        found_texture_id = true;
                        break;
                    }
                }
            }
            if (!found_texture_id)
            {
                LL_INFOS() << "unexpected image id: " << id << LL_ENDL;
            }
            self->dirtyMesh();
            self->markBodyPartsComplexityDirty();
        }
        else
        {
            // this can happen when someone uses an old baked texture possibly provided by
            // viewer-side baked texture caching
            LL_WARNS() << "Masks loaded callback but NO aux source, id " << id << LL_ENDL;
        }
    }

    if (final || !success)
    {
        delete maskData;
    }
}

// static
void LLVOAvatar::onInitialBakedTextureLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata )
{
    LLUUID *avatar_idp = (LLUUID *)userdata;
    LLVOAvatar *selfp = (LLVOAvatar *)gObjectList.findObject(*avatar_idp);

    if (selfp)
    {
        //LL_DEBUGS("Avatar") << selfp->avString() << "discard_level " << discard_level << " success " << success << " final " << final << LL_ENDL;
    }

    if (!success && selfp)
    {
        selfp->removeMissingBakedTextures();
    }
    if (final || !success )
    {
        if (selfp)
        {
            selfp->markBodyPartsComplexityDirty();
        }
        delete avatar_idp;
    }
}

// Static
void LLVOAvatar::onBakedTextureLoaded(bool success,
                                      LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src,
                                      S32 discard_level, bool final, void* userdata)
{
    //LL_DEBUGS("Avatar") << "onBakedTextureLoaded: " << src_vi->getID() << LL_ENDL;

    LLUUID id = src_vi->getID();
    LLUUID *avatar_idp = (LLUUID *)userdata;
    LLVOAvatar *selfp = (LLVOAvatar *)gObjectList.findObject(*avatar_idp);
    if (selfp)
    {
        //LL_DEBUGS("Avatar") << selfp->avString() << "discard_level " << discard_level << " success " << success << " final " << final << " id " << src_vi->getID() << LL_ENDL;
    }

    if (selfp && !success)
    {
        selfp->removeMissingBakedTextures();
        selfp->markBodyPartsComplexityDirty();
    }

    if( final || !success )
    {
        delete avatar_idp;
    }

    if( selfp && success && final )
    {
        selfp->useBakedTexture( id );
        selfp->markBodyPartsComplexityDirty();
    }
}


// Called when baked texture is loaded and also when we start up with a baked texture
void LLVOAvatar::useBakedTexture( const LLUUID& id )
{
    for (U32 i = 0; i < mBakedTextureDatas.size(); i++)
    {
        LLViewerTexture* image_baked = getImage( mBakedTextureDatas[i].mTextureIndex, 0 );
        if (id == image_baked->getID())
        {
            //LL_DEBUGS("Avatar") << avString() << " i " << i << " id " << id << LL_ENDL;
            mBakedTextureDatas[i].mIsLoaded = true;
            mBakedTextureDatas[i].mLastTextureID = id;
            mBakedTextureDatas[i].mIsUsed = true;

            if (isUsingLocalAppearance())
            {
                LL_INFOS() << "not changing to baked texture while isUsingLocalAppearance" << LL_ENDL;
            }
            else
            {
                debugColorizeSubMeshes(i,LLColor4::green);

                avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
                avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
                for (; iter != end; ++iter)
                {
                    LLAvatarJointMesh* mesh = (*iter);
                    if (mesh)
                    {
                        mesh->setTexture( image_baked );
                    }
                }
            }

            const LLAvatarAppearanceDictionary::BakedEntry *baked_dict =
                LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)i);
            for (texture_vec_t::const_iterator local_tex_iter = baked_dict->mLocalTextures.begin();
                 local_tex_iter != baked_dict->mLocalTextures.end();
                 ++local_tex_iter)
            {
                if (isSelf()) setBakedReady(*local_tex_iter, true);
            }

            // ! BACKWARDS COMPATIBILITY !
            // Workaround for viewing avatars from old viewers that haven't baked hair textures.
            // This is paired with similar code in updateMeshTextures that sets hair mesh color.
            if (i == BAKED_HAIR)
            {
                avatar_joint_mesh_list_t::iterator iter = mBakedTextureDatas[i].mJointMeshes.begin();
                avatar_joint_mesh_list_t::iterator end  = mBakedTextureDatas[i].mJointMeshes.end();
                for (; iter != end; ++iter)
                {
                    LLAvatarJointMesh* mesh = (*iter);
                    if (mesh)
                    {
                        mesh->setColor( LLColor4::white );
                    }
                }
            }
        }
    }

    dirtyMesh();
}

std::string get_sequential_numbered_file_name(const std::string& prefix,
                                              const std::string& suffix)
{
    typedef std::map<std::string,S32> file_num_type;
    static  file_num_type file_nums;
    file_num_type::iterator it = file_nums.find(prefix);
    S32 num = 0;
    if (it != file_nums.end())
    {
        num = it->second;
    }
    file_nums[prefix] = num+1;
    std::string outfilename = prefix + " " + llformat("%04d",num) + ".xml";
    std::replace(outfilename.begin(),outfilename.end(),' ','_');
    return outfilename;
}

void dump_sequential_xml(const std::string outprefix, const LLSD& content)
{
    std::string outfilename = get_sequential_numbered_file_name(outprefix,".xml");
    std::string fullpath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,outfilename);
    llofstream ofs(fullpath.c_str(), std::ios_base::out);
    ofs << LLSDOStreamer<LLSDXMLFormatter>(content, LLSDFormatter::OPTIONS_PRETTY);
    LL_DEBUGS("Avatar") << "results saved to: " << fullpath << LL_ENDL;
}

void LLVOAvatar::getSortedJointNames(S32 joint_type, std::vector<std::string>& result) const
{
    result.clear();
    if (joint_type==0)
    {
        avatar_joint_list_t::const_iterator iter = mSkeleton.begin();
        avatar_joint_list_t::const_iterator end  = mSkeleton.end();
        for (; iter != end; ++iter)
        {
            LLJoint* pJoint = (*iter);
            result.push_back(pJoint->getName());
        }
    }
    else if (joint_type==1)
    {
        for (S32 i = 0; i < mNumCollisionVolumes; i++)
        {
            LLAvatarJointCollisionVolume* pJoint = &mCollisionVolumes[i];
            result.push_back(pJoint->getName());
        }
    }
    else if (joint_type==2)
    {
        for (LLVOAvatar::attachment_map_t::const_iterator iter = mAttachmentPoints.begin();
             iter != mAttachmentPoints.end(); ++iter)
        {
            LLViewerJointAttachment* pJoint = iter->second;
            if (!pJoint) continue;
            result.push_back(pJoint->getName());
        }
    }
    std::sort(result.begin(), result.end());
}

void LLVOAvatar::dumpArchetypeXML(const std::string& prefix, bool group_by_wearables )
{
    std::string outprefix(prefix);
    if (outprefix.empty())
    {
        outprefix = getDebugName() + (isSelf() ? "_s" : "_o");
    }
    std::string outfilename = get_sequential_numbered_file_name(outprefix, ".xml");

// <FS:CR> FIRE-8893  - Dump archetype xml to user defined location
    LLFilePickerReplyThread::startPicker(boost::bind(&LLVOAvatar::dumpArchetypeXMLCallback, this, _1, group_by_wearables),
        LLFilePicker::FFSAVE_XML, outfilename);
}

void LLVOAvatar::dumpArchetypeXMLCallback(const std::vector<std::string>& filenames, bool group_by_wearables)
{
// </FS:CR>
    LLAPRFile outfile;
    LLWearableType *wr_inst = LLWearableType::getInstance();
// <FS:CR> FIRE-8893 - Dump archetype xml to user defined location
    //std::string fullpath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, outfilename);
    std::string fullpath = filenames[0];
// </FS:CR>
    if (APR_SUCCESS == outfile.open(fullpath, LL_APR_WB ))
    {
        apr_file_t* file = outfile.getFileHandle();
        LL_INFOS() << "xmlfile write handle obtained : " << fullpath << LL_ENDL;

        apr_file_printf( file, "<?xml version=\"1.0\" encoding=\"US-ASCII\" standalone=\"yes\"?>\n" );
        apr_file_printf( file, "<linden_genepool version=\"1.0\">\n" );
        apr_file_printf( file, "\n\t<archetype name=\"???\">\n" );

        bool agent_is_godlike = gAgent.isGodlikeWithoutAdminMenuFakery();

        if (group_by_wearables)
        {
            for (S32 type = LLWearableType::WT_SHAPE; type < LLWearableType::WT_COUNT; type++)
            {
                const std::string& wearable_name = wr_inst->getTypeName((LLWearableType::EType)type);
                apr_file_printf( file, "\n\t\t<!-- wearable: %s -->\n", wearable_name.c_str() );

                for (LLVisualParam* param = getFirstVisualParam(); param; param = getNextVisualParam())
                {
                    LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
                    if( (viewer_param->getWearableType() == type) &&
                       (viewer_param->isTweakable() ) )
                    {
                        dump_visual_param(file, viewer_param, viewer_param->getWeight());
                    }
                }

                for (U8 te = 0; te < TEX_NUM_INDICES; te++)
                {
                    if (LLAvatarAppearance::getDictionary()->getTEWearableType((ETextureIndex)te) == type)
                    {
                        // MULTIPLE_WEARABLES: extend to multiple wearables?
                        LLViewerTexture* te_image = getImage((ETextureIndex)te, 0);
                        if( te_image )
                        {
                            std::string uuid_str = LLUUID().asString();
                            if (agent_is_godlike)
                            {
                                te_image->getID().toString(uuid_str);
                            }
                            apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", te, uuid_str.c_str());
                        }
                    }
                }
            }
        }
        else
        {
            // Just dump all params sequentially.
            for (LLVisualParam* param = getFirstVisualParam(); param; param = getNextVisualParam())
            {
                LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
                dump_visual_param(file, viewer_param, viewer_param->getWeight());
            }

            for (U8 te = 0; te < TEX_NUM_INDICES; te++)
            {
                // MULTIPLE_WEARABLES: extend to multiple wearables?
                LLViewerTexture* te_image = getImage((ETextureIndex)te, 0);
                if( te_image )
                {
                    std::string uuid_str = LLUUID().asString();
                    if (agent_is_godlike)
                    {
                        te_image->getID().toString(uuid_str);
                    }
                    apr_file_printf( file, "\t\t<texture te=\"%i\" uuid=\"%s\"/>\n", te, uuid_str.c_str());
                }
            }
        }

        // Root joint
        const LLVector3& pos = mRoot->getPosition();
        const LLVector3& scale = mRoot->getScale();
        apr_file_printf( file, "\t\t<root name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                         mRoot->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);

        // Bones
        std::vector<std::string> bone_names, cv_names, attach_names, all_names;
        getSortedJointNames(0, bone_names);
        getSortedJointNames(1, cv_names);
        getSortedJointNames(2, attach_names);
        all_names.insert(all_names.end(), bone_names.begin(), bone_names.end());
        all_names.insert(all_names.end(), cv_names.begin(), cv_names.end());
        all_names.insert(all_names.end(), attach_names.begin(), attach_names.end());

        for (std::vector<std::string>::iterator name_iter = bone_names.begin();
             name_iter != bone_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<bone name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Collision volumes
        for (std::vector<std::string>::iterator name_iter = cv_names.begin();
             name_iter != cv_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<collision_volume name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Attachment joints
        for (std::vector<std::string>::iterator name_iter = attach_names.begin();
             name_iter != attach_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);
            if (!pJoint) continue;
            const LLVector3& pos = pJoint->getPosition();
            const LLVector3& scale = pJoint->getScale();
            apr_file_printf( file, "\t\t<attachment_point name=\"%s\" position=\"%f %f %f\" scale=\"%f %f %f\"/>\n",
                             pJoint->getName().c_str(), pos[0], pos[1], pos[2], scale[0], scale[1], scale[2]);
        }

        // Joint pos overrides
        for (std::vector<std::string>::iterator name_iter = all_names.begin();
             name_iter != all_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);

            LLVector3 pos;
            LLUUID mesh_id;

            if (pJoint && pJoint->hasAttachmentPosOverride(pos,mesh_id))
            {
                S32 num_pos_overrides;
                std::set<LLVector3> distinct_pos_overrides;
                pJoint->getAllAttachmentPosOverrides(num_pos_overrides, distinct_pos_overrides);
                apr_file_printf( file, "\t\t<joint_offset name=\"%s\" position=\"%f %f %f\" mesh_id=\"%s\" count=\"%d\" distinct=\"%d\"/>\n",
                                 pJoint->getName().c_str(), pos[0], pos[1], pos[2], mesh_id.asString().c_str(),
                                 num_pos_overrides, (S32) distinct_pos_overrides.size());
            }
        }
        // Joint scale overrides
        for (std::vector<std::string>::iterator name_iter = all_names.begin();
             name_iter != all_names.end(); ++name_iter)
        {
            LLJoint *pJoint = getJoint(*name_iter);

            LLVector3 scale;
            LLUUID mesh_id;

            if (pJoint && pJoint->hasAttachmentScaleOverride(scale,mesh_id))
            {
                S32 num_scale_overrides;
                std::set<LLVector3> distinct_scale_overrides;
                pJoint->getAllAttachmentPosOverrides(num_scale_overrides, distinct_scale_overrides);
                apr_file_printf( file, "\t\t<joint_scale name=\"%s\" scale=\"%f %f %f\" mesh_id=\"%s\" count=\"%d\" distinct=\"%d\"/>\n",
                                 pJoint->getName().c_str(), scale[0], scale[1], scale[2], mesh_id.asString().c_str(),
                                 num_scale_overrides, (S32) distinct_scale_overrides.size());
            }
        }
        F32 pelvis_fixup;
        LLUUID mesh_id;
        if (hasPelvisFixup(pelvis_fixup, mesh_id))
        {
            apr_file_printf( file, "\t\t<pelvis_fixup z=\"%f\" mesh_id=\"%s\"/>\n",
                             pelvis_fixup, mesh_id.asString().c_str());
        }

        LLVector3 rp = getRootJoint()->getWorldPosition();
        LLVector4a rpv;
        rpv.load3(rp.mV);

        for (S32 joint_num = 0; joint_num < LL_CHARACTER_MAX_ANIMATED_JOINTS; joint_num++)
        {
            LLJoint *joint = getJoint(joint_num);
            if (joint_num < mJointRiggingInfoTab.size())
            {
                LLJointRiggingInfo& rig_info = mJointRiggingInfoTab[joint_num];
                if (rig_info.isRiggedTo())
                {
                    LLMatrix4a mat;
                    LLVector4a new_extents[2];
                    mat.loadu(joint->getWorldMatrix());
                    matMulBoundBox(mat, rig_info.getRiggedExtents(), new_extents);
                    LLVector4a rrp[2];
                    rrp[0].setSub(new_extents[0],rpv);
                    rrp[1].setSub(new_extents[1],rpv);
                    apr_file_printf( file, "\t\t<joint_rig_info num=\"%d\" name=\"%s\" min=\"%f %f %f\" max=\"%f %f %f\" tmin=\"%f %f %f\" tmax=\"%f %f %f\"/>\n",
                                     joint_num,
                                     joint->getName().c_str(),
                                     rig_info.getRiggedExtents()[0][0],
                                     rig_info.getRiggedExtents()[0][1],
                                     rig_info.getRiggedExtents()[0][2],
                                     rig_info.getRiggedExtents()[1][0],
                                     rig_info.getRiggedExtents()[1][1],
                                     rig_info.getRiggedExtents()[1][2],
                                     rrp[0][0],
                                     rrp[0][1],
                                     rrp[0][2],
                                     rrp[1][0],
                                     rrp[1][1],
                                     rrp[1][2] );
                }
            }
        }

        bool ultra_verbose = false;
        if (isSelf() && ultra_verbose)
        {
            // show the cloned params inside the wearables as well.
            gAgentAvatarp->dumpWearableInfo(outfile);
        }

        apr_file_printf( file, "\t</archetype>\n" );
        apr_file_printf( file, "\n</linden_genepool>\n" );

        LLSD args;
        args["PATH"] = fullpath;
        LLNotificationsUtil::add("AppearanceToXMLSaved", args);
    }
    else
    {
        LLNotificationsUtil::add("AppearanceToXMLFailed");
    }
    // File will close when handle goes out of scope
}


void LLVOAvatar::setVisibilityRank(U32 rank)
{
    if (mDrawable.notNull() && !mDrawable->isDead())
    {
        mVisibilityRank = rank;
    }
}

// Assumes LLVOAvatar::sInstances has already been sorted.
S32 LLVOAvatar::getUnbakedPixelAreaRank()
{
    S32 rank = 1;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (character == this)
        {
            return rank;
        }

        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead() && !avatar->isFullyBaked())
        {
            rank++;
        }
    }

    llassert(0);
    return 0;
}

// static
void LLVOAvatar::cullAvatarsByPixelArea()
{
    LLCharacter::sInstances.sort([](LLCharacter* lhs, LLCharacter* rhs)
        {
            return ((LLVOAvatar*)lhs)->mVisibilityPreference > ((LLVOAvatar*)rhs)->mVisibilityPreference;
        });

    // Update the avatars that have changed status
    U32 rank = 2; // Rank 1 is reserved for self.
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* inst = (LLVOAvatar*)character;
        bool culled = !inst->isSelf() && !inst->isFullyBaked();

        if (inst->mCulled != culled)
        {
            inst->mCulled = culled;
            LL_DEBUGS() << "avatar " << inst->getID() << (culled ? " start culled" : " start not culled" ) << LL_ENDL;
            inst->updateMeshTextures();
        }

        if (inst->isSelf())
        {
            inst->setVisibilityRank(1);
        }
        else if (inst->mDrawable.notNull() && inst->mDrawable->isVisible())
        {
            inst->setVisibilityRank(rank++);
        }
        else
        {
            inst->setVisibilityRank(sMaxNonImpostors * 5);
        }
    }

    // runway - this doesn't really detect gray/grey state.
    S32 grey_avatars = 0;
    if (!LLVOAvatar::areAllNearbyInstancesBaked(grey_avatars))
    {
        if (gFrameTimeSeconds != sUnbakedUpdateTime) // only update once per frame
        {
            sUnbakedUpdateTime = gFrameTimeSeconds;
            sUnbakedTime += gFrameIntervalSeconds.value();
        }
        if (grey_avatars > 0)
        {
            if (gFrameTimeSeconds != sGreyUpdateTime) // only update once per frame
            {
                sGreyUpdateTime = gFrameTimeSeconds;
                sGreyTime += gFrameIntervalSeconds.value();
            }
        }
    }
}

void LLVOAvatar::startAppearanceAnimation()
{
    if(!mAppearanceAnimating)
    {
        mAppearanceAnimating = true;
        mAppearanceMorphTimer.reset();
        mLastAppearanceBlendTime = 0.f;
    }
}

// virtual
void LLVOAvatar::removeMissingBakedTextures()
{
}

//virtual
void LLVOAvatar::updateRegion(LLViewerRegion *regionp)
{
    LLViewerObject::updateRegion(regionp);
}

// virtual
std::string LLVOAvatar::getFullname() const
{
    std::string name;

    LLNameValue* first = getNVPair("FirstName");
    LLNameValue* last  = getNVPair("LastName");
    if (first && last)
    {
        name = LLCacheName::buildFullName( first->getString(), last->getString() );
    }

    return name;
}

LLHost LLVOAvatar::getObjectHost() const
{
    LLViewerRegion* region = getRegion();
    if (region && !isDead())
    {
        return region->getHost();
    }
    else
    {
        return LLHost();
    }
}

bool LLVOAvatar::updateLOD()
{
    if (mDrawable.isNull())
    {
        return false;
    }

    if (!LLPipeline::sImpostorRender && isImpostor() && 0 != mDrawable->getNumFaces() && mDrawable->getFace(0)->hasGeometry())
    {
        return true;
    }

    bool res = updateJointLODs();

    LLFace* facep = mDrawable->getFace(0);
    if (!facep || !facep->getVertexBuffer())
    {
        dirtyMesh(2);
    }

    if (mDirtyMesh >= 2 || mDrawable->isState(LLDrawable::REBUILD_GEOMETRY))
    {   //LOD changed or new mesh created, allocate new vertex buffer if needed
        updateMeshData();
    }
    updateVisibility();

    return res;
}

void LLVOAvatar::updateLODRiggedAttachments()
{
    updateLOD();
    rebuildRiggedAttachments();
}

void showRigInfoTabExtents(LLVOAvatar *avatar, LLJointRiggingInfoTab& tab, S32& count_rigged, S32& count_box)
{
    count_rigged = count_box = 0;
    LLVector4a zero_vec;
    zero_vec.clear();
    for (S32 i=0; i<tab.size(); i++)
    {
        if (tab[i].isRiggedTo())
        {
            count_rigged++;
            LLJoint *joint = avatar->getJoint(i);
            LL_DEBUGS("RigSpam") << "joint " << i << " name " << joint->getName() << " box "
                                 << tab[i].getRiggedExtents()[0] << ", " << tab[i].getRiggedExtents()[1] << LL_ENDL;
            if ((!tab[i].getRiggedExtents()[0].equals3(zero_vec)) ||
                (!tab[i].getRiggedExtents()[1].equals3(zero_vec)))
            {
                count_box++;
            }
       }
    }
}

void LLVOAvatar::getAssociatedVolumes(std::vector<LLVOVolume*>& volumes)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    for (const auto& iter : mAttachmentPoints)
    {
        LLViewerJointAttachment* attachment = iter.second;
        // LLViewerJointAttachment::attachedobjs_vec_t::iterator attach_end = attachment->mAttachedObjects.end(); // <FS:Beq/> set but not used

        for (LLViewerObject* attached_object : attachment->mAttachedObjects)
        {
            if (attached_object->isDead())
                continue;

            if (attached_object->getPCode() == LL_PCODE_VOLUME)
            {
                LLVOVolume* volume = (LLVOVolume*)attached_object;
                volumes.push_back(volume);
                if (volume->isAnimatedObject())
                {
                    // For animated object attachment, don't need
                    // the children. Will just get bounding box
                    // from the control avatar.
                    continue;
                }
            }

            for (LLViewerObject* childp : attached_object->getChildren())
            {
                if (!childp->isDead() &&  childp->getPCode() == LL_PCODE_VOLUME)
                {
                    volumes.push_back((LLVOVolume*)childp);
                }
            }
        }
    }

    LLControlAvatar *control_av = dynamic_cast<LLControlAvatar*>(this);
    if (control_av)
    {
        LLVOVolume *volp = control_av->mRootVolp;
        if (volp)
        {
            volumes.push_back(volp);
            LLViewerObject::const_child_list_t& children = volp->getChildren();
            for (LLViewerObject::const_child_list_t::const_iterator it = children.begin();
                 it != children.end(); ++it)
            {
                LLViewerObject *childp = *it;
                LLVOVolume *volume = dynamic_cast<LLVOVolume*>(childp);
                if (volume)
                {
                    volumes.push_back(volume);
                }
            }
        }
    }
}

// virtual
void LLVOAvatar::updateRiggingInfo()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    //LL_DEBUGS("RigSpammish") << getDebugName() << " updating rig tab" << LL_ENDL; // <FS:Ansariel> Performance tweak

    // use a local static for scratch space to avoid reallocation here
    static std::vector<LLVOVolume*> volumes;
    volumes.resize(0);

    getAssociatedVolumes(volumes);

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_AVATAR("update rig info - get key");
        size_t hash = 0;
        // Get current rigging info key
        for (LLVOVolume* vol : volumes)
        {
            if (vol->isRiggedMesh())
            {
                const LLUUID& mesh_id = vol->getVolume()->getParams().getSculptID();
                S32 max_lod = llmax(vol->getLOD(), vol->mLastRiggingInfoLOD);

                boost::hash_combine(hash, mesh_id);
                boost::hash_combine(hash, max_lod);
            }
        }

        // Check for key change, which indicates some change in volume composition or LOD.
        if (hash == mLastRiggingInfoKey)
        {
            return;
        }


        // Something changed. Update.
        mLastRiggingInfoKey = hash;
    }

    mJointRiggingInfoTab.clear();
    for (LLVOVolume* vol : volumes)
    {
        vol->updateRiggingInfo();
        mJointRiggingInfoTab.merge(vol->mJointRiggingInfoTab);
    }

    //LL_INFOS() << "done update rig count is " << countRigInfoTab(mJointRiggingInfoTab) << LL_ENDL;
    // Remove debug only stuff on hot path
    // LL_DEBUGS("RigSpammish") << getFullname() << " after update rig tab:" << LL_ENDL;
    // S32 joint_count, box_count;
    // showRigInfoTabExtents(this, mJointRiggingInfoTab, joint_count, box_count);
    // LL_DEBUGS("RigSpammish") << "uses " << joint_count << " joints " << " nonzero boxes: " << box_count << LL_ENDL;
}

// virtual
void LLVOAvatar::onActiveOverrideMeshesChanged()
{
    mJointRiggingInfoTab.setNeedsUpdate(true);
}

U32 LLVOAvatar::getPartitionType() const
{
    // Avatars merely exist as drawables in the bridge partition
    return mIsControlAvatar ? LLViewerRegion::PARTITION_CONTROL_AV : LLViewerRegion::PARTITION_AVATAR;
}

//static
void LLVOAvatar::updateImpostors()
{
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isDead()
            && avatar->isVisible()
            && avatar->isImpostor()
            && avatar->needsImpostorUpdate())
        {
            avatar->calcMutedAVColor();
            gPipeline.generateImpostor(avatar);
        }
    }

    LLCharacter::sAllowInstancesChange = true;
}

// virtual
bool LLVOAvatar::isImpostor()
{
    // <FS> PLAN-avatars.md stage A step 4: after the avatars' update the table holds the answer (one owner for the decision; the 30 readers of the render side, the pools and the cull, read it)
    if (const LLAvatarTable::Row* row = LLAvatarTable::finalRow(this))
    {
        static const bool verify = getenv("LL_VK_AVTABLE_VERIFY") != nullptr;
        if (verify && !isControlAvatar())
        {
            LLAvatarTable::noteCheck(0, row->mImpostor == computeImpostorLive());
        }
        return row->mImpostor;
    }
    return computeImpostorLive();
}

bool LLVOAvatar::computeImpostorLive()
{
// <FS:Beq> render time handling using tooSlow()
//  return isVisuallyMuted() || (sLimitNonImpostors && (mUpdatePeriod > 1));
    // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
    static LLCachedControl<U32> impostor_avatar_exclude(gSavedSettings,"FSImpostorAvatarExclude", 0);

    // <FS> FIRE-34340-2 RLV silhouettes must be impostored
    if (isRlvSilhouette())
    {
        return true;
    }
    // </FS>

    // Store the result of is visually muted as used in possibly 2 places
    bool is_visual_muted = isVisuallyMuted();

    // If the avatar is set to be excluded, return that the avatar is not an Impostor
    if ((mIsControlAvatar && impostor_avatar_exclude >= EImpostorAvatarExclude::CONTROL) || (mIsAnimesh && (impostor_avatar_exclude & EImpostorAvatarExclude::USER)))
    {
        return false;
    }
    // </FS:minerjr> [FIRE-35735]
    return (
            // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
            // isVisuallyMuted() ||
            is_visual_muted || // Save from calling isVisuallyMuted a second time
            // </FS:minerjr> [FIRE-35735]
            isTooSlowWithoutShadows() ||
            (sLimitNonImpostors && (mUpdatePeriod > 1) )
    );
// </FS:Beq>
}

// <FS> FSImpostorMaxDistance: an avatar farther than that is an impostor whatever its visibility rank (0 = only the rank decides, as before).
static F32 impostor_max_distance()
{
    static LLCachedControl<F32> setting(gSavedSettings, "FSImpostorMaxDistance", 0.f);
    return setting;
}

bool LLVOAvatar::shouldImpostor(const F32 rank_factor)
{
    if (isSelf())
    {
        return false;
    }
    {
        const F32 max_distance = impostor_max_distance();
        if (max_distance > 0.f && mDrawable.notNull() && mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale() > max_distance)
        {
            return true;
        }
    }
    if (isVisuallyMuted())
    {
        return true;
    }
// <FS:Beq> render time handling using tooSlow()
    // return sLimitNonImpostors && (mVisibilityRank > sMaxNonImpostors * rank_factor);
    // static LLCachedControl<bool> render_jellys_As_imposters(gSavedSettings, "RenderJellyDollsAsImpostors");

    // <FS:minerjr> [FIRE-35735] Imposter/Impostor Avatar Exclusions
    static LLCachedControl<U32> impostor_avatar_exclude(gSavedSettings,"FSImpostorAvatarExclude", 0);

    // If the avatar is set to be excluded, return that the avatar should not be impostored
    if ((mIsControlAvatar && impostor_avatar_exclude >= EImpostorAvatarExclude::CONTROL) || (mIsAnimesh && (impostor_avatar_exclude & EImpostorAvatarExclude::USER)))
    {
        return false;
    }
    // </FS:minerjr> [FIRE-35735]
    if (mFarTierHeavy)
    {
        // <FS> far avatar tier (pipeline.cpp far_avatar_tier): over its triangle budget, a whole picture of it instead of the parts of it that fit. The verdict ends when the avatar is nearer than the tier again
        // (the tier's own pass does not see an avatar whose groups are not listed)
        static LLCachedControl<F32> tier_distance(gSavedSettings, "RenderFarAvatarDistance", 20.f);
        if (mDrawable.notNull() && mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale() < tier_distance * 0.875f)
        {
            mFarTierHeavy = false;
        }
        else
        {
            return true;
        }
    }
    if (isTooSlowWithoutShadows())
    {
        return true;
    }
// </FS:Beq>
    return sLimitNonImpostors && (mVisibilityRank > sMaxNonImpostors * rank_factor);
}

bool LLVOAvatar::needsImpostorUpdate() const
{
    return mNeedsImpostorUpdate;
}

const LLVector3& LLVOAvatar::getImpostorOffset() const
{
    return mImpostorOffset;
}

const LLVector2& LLVOAvatar::getImpostorDim() const
{
    return mImpostorDim;
}

void LLVOAvatar::setImpostorDim(const LLVector2& dim)
{
    mImpostorDim = dim;
}

void LLVOAvatar::cacheImpostorValues()
{
    getImpostorValues(mImpostorExtents, mImpostorAngle, mImpostorDistance);
}

void LLVOAvatar::getImpostorValues(LLVector4a* extents, LLVector3& angle, F32& distance) const
{
    const LLVector4a* ext = mDrawable->getSpatialExtents();
    extents[0] = ext[0];
    extents[1] = ext[1];

    LLVector3 at = LLViewerCamera::getInstance()->getOrigin()-(getRenderPosition()+mImpostorOffset);
    distance = at.normalize();
    F32 da = 1.f - (at*LLViewerCamera::getInstance()->getAtAxis());
    angle.mV[0] = LLViewerCamera::getInstance()->getYaw()*da;
    angle.mV[1] = LLViewerCamera::getInstance()->getPitch()*da;
    angle.mV[2] = da;
}

// static
const U32 LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER = 66; /* Must equal the maximum allowed the RenderAvatarMaxNonImpostors
                                           * slider in panel_preferences_graphics1.xml */

// static
void LLVOAvatar::updateImpostorRendering(U32 newMaxNonImpostorsValue)
{
    U32  oldmax = sMaxNonImpostors;
    bool oldflg = sLimitNonImpostors;

    if (NON_IMPOSTORS_MAX_SLIDER <= newMaxNonImpostorsValue)
    {
        sMaxNonImpostors = 0;
    }
    else
    {
        sMaxNonImpostors = newMaxNonImpostorsValue;
    }
    // the sLimitNonImpostors flag depends on whether or not sMaxNonImpostors is set to the no-limit value (0)
    sLimitNonImpostors = (0 != sMaxNonImpostors);
    if ( oldflg != sLimitNonImpostors )
    {
        LL_DEBUGS("AvatarRender")
            << "was " << (oldflg ? "use" : "don't use" ) << " impostors (max " << oldmax << "); "
            << "now " << (sLimitNonImpostors ? "use" : "don't use" ) << " impostors (max " << sMaxNonImpostors << "); "
            << LL_ENDL;
    }
}


void LLVOAvatar::idleUpdateRenderComplexity()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (isControlAvatar())
    {
        LLControlAvatar *cav = dynamic_cast<LLControlAvatar*>(this);
        bool is_attachment = cav && cav->mRootVolp && cav->mRootVolp->isAttachment(); // For attached animated objects
        if (is_attachment)
        {
            // ARC for animated object attachments is accounted with the avatar they're attached to.
            return;
        }
    }

    // Render Complexity
    calculateUpdateRenderComplexity(); // Update mVisualComplexity if needed

    bool autotune = LLPerfStats::tunables.userAutoTuneEnabled && !mIsControlAvatar && !isSelf();
    if (autotune && !isDead())
    {
        F32 radius = sRenderDistance * sRenderDistance;

        bool is_nearby = true;
        if ((dist_vec_squared(getPositionGlobal(), gAgent.getPositionGlobal()) > radius) &&
            (dist_vec_squared(getPositionGlobal(), gAgentCamera.getCameraPositionGlobal()) > radius))
        {
            is_nearby = false;
        }

        if (is_nearby && (sAVsIgnoringARTLimit.size() < MIN_NONTUNED_AVS))
        {
            if (std::count(sAVsIgnoringARTLimit.begin(), sAVsIgnoringARTLimit.end(), mID) == 0)
            {
                sAVsIgnoringARTLimit.push_back(mID);
            }
        }
        else if (!is_nearby)
        {
            sAVsIgnoringARTLimit.erase(std::remove(sAVsIgnoringARTLimit.begin(), sAVsIgnoringARTLimit.end(), mID),
                                       sAVsIgnoringARTLimit.end());
        }
        updateNearbyAvatarCount();
    }
}

void LLVOAvatar::updateNearbyAvatarCount()
{
    static LLFrameTimer agent_update_timer;

    if (agent_update_timer.getElapsedTimeF32() > 1.0f)
    {
        S32 avs_nearby = 0;
        F32 radius = sRenderDistance * sRenderDistance;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = (LLVOAvatar*)character;
            if (!avatar->isDead() && !avatar->isControlAvatar())
            {
                if ((dist_vec_squared(avatar->getPositionGlobal(), gAgent.getPositionGlobal()) <= radius) ||
                    (dist_vec_squared(avatar->getPositionGlobal(), gAgentCamera.getCameraPositionGlobal()) <= radius))
                {
                    avs_nearby++;
                }
            }
        }
        sAvatarsNearby = avs_nearby;
        agent_update_timer.reset();
    }
}

void LLVOAvatar::idleUpdateDebugInfo()
{
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_AVATAR_DRAW_INFO))
    {
        std::string info_line;
        F32 red_level;
        F32 green_level;
        LLColor4 info_color;
        LLFontGL::StyleFlags info_style;

        if ( !mText )
        {
            initHudText();
            mText->setFadeDistance(20.0, 5.0); // limit clutter in large crowds
        }
        else
        {
            mText->clearString(); // clear debug text
        }

        /*
         * NOTE: the logic for whether or not each of the values below
         * controls muting MUST match that in the isVisuallyMuted and isTooComplex methods.
         */

        static LLCachedControl<U32> max_render_cost(gSavedSettings, "RenderAvatarMaxComplexity", 0);
        info_line = llformat("%d Complexity", mVisualComplexity);

        if (max_render_cost != 0) // zero means don't care, so don't bother coloring based on this
        {
            green_level = 1.f-llclamp(((F32) mVisualComplexity-(F32)max_render_cost)/(F32)max_render_cost, 0.f, 1.f);
            red_level   = llmin((F32) mVisualComplexity/(F32)max_render_cost, 1.f);
            info_color.set(red_level, green_level, 0.0, 1.0);
            info_style = (  mVisualComplexity > max_render_cost
                          ? LLFontGL::BOLD : LLFontGL::NORMAL );
        }
        else
        {
            info_color.set(LLColor4::grey);
            info_style = LLFontGL::NORMAL;
        }
        mText->addLine(info_line, info_color, info_style);

        // Visual rank
        info_line = llformat("%d rank", mVisibilityRank);
        // Use grey for imposters, white for normal rendering or no impostors
        info_color.set(isImpostor() ? LLColor4::grey : (isControlAvatar() ? LLColor4::yellow : LLColor4::white));
        info_style = LLFontGL::NORMAL;
        mText->addLine(info_line, info_color, info_style);

        // Triangle count
        mText->addLine(std::string("VisTris ") + LLStringOps::getReadableNumber(mAttachmentVisibleTriangleCount),
                       info_color, info_style);
        mText->addLine(std::string("EstMaxTris ") + LLStringOps::getReadableNumber(mAttachmentEstTriangleCount),
                       info_color, info_style);

        // Attachment Surface Area
        static LLCachedControl<F32> max_attachment_area(gSavedSettings, "RenderAutoMuteSurfaceAreaLimit", 1000.0f);
        info_line = llformat("%.0f m^2", mAttachmentSurfaceArea);

        if (max_render_cost != 0 && max_attachment_area != 0) // zero means don't care, so don't bother coloring based on this
        {
            green_level = 1.f-llclamp((mAttachmentSurfaceArea-max_attachment_area)/max_attachment_area, 0.f, 1.f);
            red_level   = llmin(mAttachmentSurfaceArea/max_attachment_area, 1.f);
            info_color.set(red_level, green_level, 0.0, 1.0);
            info_style = (  mAttachmentSurfaceArea > max_attachment_area
                          ? LLFontGL::BOLD : LLFontGL::NORMAL );

        }
        else
        {
            info_color.set(LLColor4::grey);
            info_style = LLFontGL::NORMAL;
        }

        mText->addLine(info_line, info_color, info_style);

        updateText(); // corrects position
    }
}

void LLVOAvatar::updateVisualComplexity()
{
    LL_DEBUGS("AvatarRender") << "avatar " << getID() << " appearance changed" << LL_ENDL;
    // Trigger cache recalculation on next idle update.
    // Will recalculate stale, missing data and control avatar.
    mVisualComplexityStale = true;
}

void LLVOAvatar::calculateAttachmentComplexity(LLViewerObject* attached_object,
    const F32 max_attachment_complexity,
    ComplexityComponent& cache)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    cache.reset();

    if (!attached_object
        || attached_object->isDead())
    {
        return;
    }

    accountRenderComplexityForObject(
        attached_object,
        max_attachment_complexity,
        cache.textures,
        cache.render_cost,
        cache.triangle_count,
        cache.est_triangle_count,
        cache.surface_area,
        cache.hud_complexity,
        cache.object_complexity
    );

    cache.last_update_time = LLFrameTimer::getTotalSeconds();
    cache.needs_update = false;
}

void LLVOAvatar::calculateBodyPartsComplexity(ComplexityComponent& cache)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    cache.reset();

    // Body parts have a fixed cost
    // This represents the base avatar mesh (eyes, hair, shape, skin, etc.)
    cache.render_cost = calculateBodyPartsComplexity();

    // For more accurate body part complexity, could enumerate mesh LODs here
    // For now, using a constant cost as in the original implementation

    cache.last_update_time = LLFrameTimer::getTotalSeconds();
    cache.needs_update = false;
}

bool LLVOAvatar::shouldUpdateComplexityComponent(const ComplexityComponent& component) const
{
    if (component.needs_update)
    {
        return true;
    }

    constexpr F32 CACHE_LIFETIME_SECONDS = 30.0; // Todo: should be indefinite, until something actually changes
    F64 current_time = LLFrameTimer::getTotalSeconds();
    return (current_time - component.last_update_time) > CACHE_LIFETIME_SECONDS;
}

bool LLVOAvatar::calculateControlAvatarComplexity(ComplexityComponent& cache, const F32 max_attachment_complexity)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    cache.reset();

    if (!isControlAvatar())
    {
        return false;
    }

    LLControlAvatar* control_av = dynamic_cast<LLControlAvatar*>(this);
    if (!control_av)
    {
        return false;
    }

    LLVOVolume* volp = control_av->mRootVolp;
    if (!volp || volp->isAttachment())
    {
        return false;
    }

    accountRenderComplexityForObject(
        volp,
        max_attachment_complexity,
        cache.textures,
        cache.render_cost,
        cache.triangle_count,
        cache.est_triangle_count,
        cache.surface_area,
        cache.hud_complexity,
        cache.object_complexity
    );

    // todo: store 'expires' time instead or make it indefinite?
    cache.last_update_time = LLFrameTimer::getTotalSeconds();
    cache.needs_update = false;

    return true;
}

void LLVOAvatar::accumulateComplexityComponent(const ComplexityComponent& component,
    U32& total_cost,
    hud_complexity_list_t& hud_list,
    object_complexity_list_t& object_list)
{
    total_cost += component.render_cost;
    mAttachmentSurfaceArea += component.surface_area;
    mAttachmentVisibleTriangleCount += component.triangle_count;
    mAttachmentEstTriangleCount += component.est_triangle_count;

    // Add HUD/object complexity info if present
    if (component.hud_complexity.objectId.notNull())
    {
        hud_list.push_back(component.hud_complexity);
    }
    if (component.object_complexity.objectId.notNull())
    {
        object_list.push_back(component.object_complexity);
    }
}

void LLVOAvatar::markAttachmentComplexityDirty(const LLUUID& object_id, bool force_reset_attachment)
{
    // Mark the cache entry if it exists
    complexity_cache_map_t::iterator it = mComplexityCache.find(object_id);
    if (it != mComplexityCache.end())
    {
        if (force_reset_attachment)
        {
            // Object was detached.
            // Force reset it in case it lingers in gObjectList for some reason (ex: dropped to world).
            it->second.reset();
        }
        it->second.needs_update = true;
    }

    // Launch update process if not already scheduled
    // It will add any missing attachments.
    mVisualComplexityStale = true;
}

void LLVOAvatar::markBodyPartsComplexityDirty()
{
    mBodyPartsComplexity.needs_update = true;

    // Launch update process if not already scheduled
    mVisualComplexityStale = true;
}

void LLVOAvatar::performPartialComplexityUpdate(const F32 max_attachment_complexity)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    // Update any attachments marked as dirty
    // Todo: might want to limit time or count here and defer the rest
    // till next run. In such a case will need to make sure
    // mVisualComplexityStale remains true.

    for (attachment_map_t::iterator iter = mAttachmentPoints.begin();
        iter != mAttachmentPoints.end(); ++iter)
    {
        LLViewerJointAttachment* attachment = iter->second;
        if (!attachment || !attachment->getValid())
        {
            continue;
        }

        for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
            attachment_iter != attachment->mAttachedObjects.end(); ++attachment_iter)
        {
            LLViewerObject* attached_object = attachment_iter->get();
            if (attached_object && !attached_object->isDead())
            {
                LLUUID object_id = attached_object->getID();
                ComplexityComponent& cache = mComplexityCache[object_id];

                // Update if cache is stale or a new entry.
                if (shouldUpdateComplexityComponent(cache))
                {
                    calculateAttachmentComplexity(attached_object, max_attachment_complexity, cache);
                }
            }
        }
    }

    // Update body parts if stale
    if (shouldUpdateComplexityComponent(mBodyPartsComplexity))
    {
        calculateBodyPartsComplexity(mBodyPartsComplexity);
    }
}

// Calculations for mVisualComplexity value
// Call rate is flexible, can be once in 20, can be once in 200 frames,
// depends on priority and known cost of an avatar in question.
void LLVOAvatar::calculateUpdateRenderComplexity()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    // ****************************************************************
    // This calculation should not be modified by third party viewers,
    // since it is used to limit rendering and should be uniform for
    // everyone. If you have suggested improvements, submit them to
    // the official viewer for consideration.
    // ****************************************************************

    if (!mVisualComplexityStale)
    {
        return;
    }

    // Get the attachment complexity limit
    static LLCachedControl<F32> max_complexity_setting(gSavedSettings, "MaxAttachmentComplexity");
    F32 max_attachment_complexity = max_complexity_setting;
    max_attachment_complexity = llmax(max_attachment_complexity, DEFAULT_MAX_ATTACHMENT_COMPLEXITY);

    // Update complexity for any dirty attachments or body parts.
    //
    // Todo: Limit this by time or count and continue later as
    // doing everything in one go can be very expensive (multiple ms)
    // Note that calculateUpdateRenderComplexity() can be launched once
    // per 200 frames. Limiting it by time or count runs the risk of
    // already checked attachments getting stale on last_update_time,
    // thus function will keep running indefinetely.
    performPartialComplexityUpdate(max_attachment_complexity);

    // Reset per-run counters
    mAttachmentSurfaceArea = 0.f;
    mAttachmentVisibleTriangleCount = 0;
    mAttachmentEstTriangleCount = 0.f;

    U32 total_cost = 0;
    hud_complexity_list_t hud_complexity_list;
    object_complexity_list_t object_complexity_list;

    // <FS:Ansariel> Show per-item complexity in COF
    std::map<LLUUID, U32> item_complexity;
    std::map<LLUUID, U32> temp_item_complexity;
    U32 body_parts_complexity;
    // </FS:Ansariel>

    // Calculate and accumulate control avatar complexity if applicable
    // For now this is on each run.
    // Todo: See if mControlAvatarComplexity.needs_update is applicable here.
    if (calculateControlAvatarComplexity(mControlAvatarComplexity, max_attachment_complexity))
    {
        accumulateComplexityComponent(
            mControlAvatarComplexity,
            total_cost,
            hud_complexity_list,
            object_complexity_list);
    }

    // Accumulate body parts complexity
    accumulateComplexityComponent(mBodyPartsComplexity, total_cost, hud_complexity_list, object_complexity_list);

    // <FS:Ansariel> Show per-item complexity in COF
    body_parts_complexity = mBodyPartsComplexity.render_cost;
    // </FS:Ansariel>

    // Accumulate all attachment complexity from cache
    // Clean up cache entries for attachments that no longer exist
    std::vector<LLUUID> to_remove;

    for (complexity_cache_map_t::iterator cache_iter = mComplexityCache.begin();
        cache_iter != mComplexityCache.end(); ++cache_iter)
    {
        const LLUUID& object_id = cache_iter->first;

        // Verify object still exists
        LLViewerObject* obj = gObjectList.findObject(object_id);
        if (!obj
            || obj->isDead()
            || !obj->isAttachment())
        {
            to_remove.push_back(object_id);
            continue;
        }
        // <FS:Ansariel> Show per-item complexity in COF
        else if (isSelf() && obj && !obj->isDead())
        {
            if (!obj->isTempAttachment())
            {
                item_complexity.try_emplace(obj->getAttachmentItemID(), cache_iter->second.render_cost);
            }
            else
            {
                temp_item_complexity.try_emplace(obj->getID(), cache_iter->second.render_cost);
            }
        }
        // </FS:Ansariel>

        // Accumulate this attachment's complexity
        accumulateComplexityComponent(cache_iter->second, total_cost,
            hud_complexity_list, object_complexity_list);
    }

    // Remove stale cache entries
    for (std::vector<LLUUID>::iterator it = to_remove.begin(); it != to_remove.end(); ++it)
    {
        mComplexityCache.erase(*it);
    }

    if (total_cost != mVisualComplexity)
    {
        LL_DEBUGS("AvatarRender") << "Avatar " << getID()
            << " complexity updated was " << mVisualComplexity << " now " << total_cost
            << " reported " << mReportedVisualComplexity
            << LL_ENDL;
    }
    else
    {
        LL_DEBUGS("AvatarRender") << "Avatar " << getID()
            << " complexity updated no change " << mVisualComplexity
            << " reported " << mReportedVisualComplexity
            << LL_ENDL;
    }

    // Store results
    if (mVisualComplexity != total_cost)
    {
        invalidateNameTagInputs();      // <FS> B3d: the complexity shows on the name tag
    }
    mVisualComplexity = total_cost;

    // Call the reporting function with the aggregated lists
    // <FS:Ansariel> Show per-item complexity in COF
    //processComplexityCostChange(hud_complexity_list, object_complexity_list);
    processComplexityCostChange(hud_complexity_list, object_complexity_list, item_complexity, temp_item_complexity, body_parts_complexity);
    // </FS:Ansariel>

    // Stop processing until something changes
    mVisualComplexityStale = false;
}

U32 LLVOAvatar::calculateBodyPartsComplexity()
{
    constexpr U32 COMPLEXITY_BODY_PART_COST = 200;
    U32 cost = 0;
    //<FS:Beq> BOM constrain number of bake requests when BOM not supported
    //for (U8 baked_index = 0; baked_index < BAKED_NUM_INDICES; baked_index++)
    for (U8 baked_index = 0; baked_index < getNumBakes(); baked_index++)
    //</FS:Beq>
    {
        const LLAvatarAppearanceDictionary::BakedEntry* baked_dict
            = LLAvatarAppearance::getDictionary()->getBakedTexture((EBakedTextureIndex)baked_index);
        ETextureIndex tex_index = baked_dict->mTextureIndex;
        if ((tex_index != TEX_SKIRT_BAKED) || (isWearingWearableType(LLWearableType::WT_SKIRT)))
        {
            // Same as isTextureVisible(), but doesn't account for isSelf to ensure identical numbers for all avatars
            if (isIndexLocalTexture(tex_index))
            {
                if (isTextureDefined(tex_index, 0))
                {
                    cost += COMPLEXITY_BODY_PART_COST;
                }
            }
            else
            {
                // baked textures can use TE images directly
                if (isTextureDefined(tex_index)
                    && (getTEImage(tex_index)->getID() != IMG_INVISIBLE || LLDrawPoolAlpha::sShowDebugAlpha))
                {
                    cost += COMPLEXITY_BODY_PART_COST;
                }
            }
        }
    }
    LL_DEBUGS("ARCdetail") << "Avatar body parts complexity: " << cost << LL_ENDL;
    return cost;
}

// <FS:Ansariel> Show avatar complexity in appearance floater
//void LLVOAvatar::processComplexityCostChange(const hud_complexity_list_t &hud_complexity_list, const object_complexity_list_t &object_complexity_list)
void LLVOAvatar::processComplexityCostChange(const hud_complexity_list_t &hud_complexity_list, const object_complexity_list_t &object_complexity_list,
    const std::map<LLUUID, U32>& item_complexity, const std::map<LLUUID, U32>& temp_item_complexity, U32 body_parts_complexity)
// </FS:Ansariel>
{
    static LLCachedControl<U32> show_my_complexity_changes(gSavedSettings, "ShowMyComplexityChanges", 20);

    if (isSelf() && show_my_complexity_changes)
    {
        // Avatar complexity
        LLAvatarRenderNotifier::getInstance()->updateNotificationAgent(mVisualComplexity);
        LLAvatarRenderNotifier::getInstance()->setObjectComplexityList(object_complexity_list);
        // HUD complexity
        LLHUDRenderNotifier::getInstance()->updateNotificationHUD(hud_complexity_list);
    }

    // <FS:Ansariel> Show avatar complexity in appearance floater
    if (isSelf())
    {
        LLSidepanelAppearance::updateAvatarComplexity(mVisualComplexity, item_complexity, temp_item_complexity, body_parts_complexity);
    }
    // </FS:Ansariel>

    //schedule an update to ART next frame if needed
    if (LLPerfStats::tunables.userAutoTuneEnabled &&
        LLPerfStats::tunables.userFPSTuningStrategy != LLPerfStats::TUNE_SCENE_ONLY &&
        !isVisuallyMuted())
    {
        const LLUUID id = getID(); // <== use id to make sure this avatar didn't get deleted between frames
        LL::WorkQueue::getInstance("mainloop")->post([id]()
        {
            LLViewerObject* obj = gObjectList.findObject(id);
            if (obj
                && !obj->isDead()
                && obj->isAvatar()
                && obj->mDrawable)
            {
                LLVOAvatar* avatar = (LLVOAvatar*)obj;
                gPipeline.profileAvatar(avatar);
            }
        });
    }
}

// Account for the complexity of a single top-level object associated
// with an avatar. This will be either an attached object or an animated
// object.
void LLVOAvatar::accountRenderComplexityForObject(
    LLViewerObject *attached_object,
    const F32 max_attachment_complexity,
    LLVOVolume::texture_cost_t& textures,
    U32& cost,
    U32& visible_triangle_count,
    F32& est_triangle_count,
    F32& surface_area,
    LLHUDComplexity& hud_object_complexity,
    LLObjectComplexity& object_complexity)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (attached_object && !attached_object->isHUDAttachment())
    {
        visible_triangle_count += attached_object->recursiveGetTriangleCount();
        est_triangle_count += attached_object->recursiveGetEstTrianglesMax();
        surface_area += attached_object->recursiveGetScaledSurfaceArea();

        textures.clear();
        const LLDrawable* drawable = attached_object->mDrawable;
        if (drawable)
        {
            const LLVOVolume* volume = drawable->getVOVolume();
            if (volume)
            {
                F32 attachment_total_cost = 0;
                F32 attachment_volume_cost = 0;
                F32 attachment_texture_cost = 0;
                F32 attachment_children_cost = 0;
                            const F32 animated_object_attachment_surcharge = 1000;

                if (volume->isAnimatedObjectFast())
                {
                    attachment_volume_cost += animated_object_attachment_surcharge;
                }
                attachment_volume_cost += volume->getRenderCost(textures);

                const_child_list_t children = volume->getChildren();
                for (const_child_list_t::const_iterator child_iter = children.begin();
                    child_iter != children.end();
                    ++child_iter)
                {
                    LLViewerObject* child_obj = *child_iter;
                    LLVOVolume* child = dynamic_cast<LLVOVolume*>(child_obj);
                    if (child)
                    {
                        attachment_children_cost += child->getRenderCost(textures);
                    }
                }

                for (LLVOVolume::texture_cost_t::iterator volume_texture = textures.begin();
                    volume_texture != textures.end();
                    ++volume_texture)
                {
                    // add the cost of each individual texture in the linkset
                    attachment_texture_cost += LLVOVolume::getTextureCost(*volume_texture);
                }
                attachment_total_cost = attachment_volume_cost + attachment_texture_cost + attachment_children_cost;
                LL_DEBUGS("ARCdetail") << "Attachment costs " << attached_object->getAttachmentItemID()
                    << " total: " << attachment_total_cost
                    << ", volume: " << attachment_volume_cost
                    << ", " << textures.size()
                    << " textures: " << attachment_texture_cost
                    << ", " << volume->numChildren()
                    << " children: " << attachment_children_cost
                    << LL_ENDL;
                // Limit attachment complexity to avoid signed integer flipping of the wearer's ACI
                cost += (U32)llclamp(attachment_total_cost, MIN_ATTACHMENT_COMPLEXITY, max_attachment_complexity);

                if (isSelf())
                {
                    object_complexity.objectName = attached_object->getAttachmentItemName();
                    object_complexity.objectId = attached_object->getAttachmentItemID();
                    object_complexity.objectCost = (U32)attachment_total_cost;
                }
            }
        }
    }
    if (isSelf()
        && attached_object
        && attached_object->isHUDAttachment()
        && !attached_object->isTempAttachment()
        && attached_object->mDrawable)
    {
        textures.clear();
        surface_area += attached_object->recursiveGetScaledSurfaceArea();

        const LLVOVolume* volume = attached_object->mDrawable->getVOVolume();
        if (volume)
        {
            bool is_rigged_mesh = volume->isRiggedMeshFast();
            hud_object_complexity.objectName = attached_object->getAttachmentItemName();
            hud_object_complexity.objectId = attached_object->getAttachmentItemID();
            std::string joint_name;
            gAgentAvatarp->getAttachedPointName(attached_object->getAttachmentItemID(), joint_name);
            hud_object_complexity.jointName = joint_name;
            // get cost and individual textures
            hud_object_complexity.objectsCost += volume->getRenderCost(textures);
            hud_object_complexity.objectsCount++;

            LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
            for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                iter != child_list.end(); ++iter)
            {
                LLViewerObject* childp = *iter;
                const LLVOVolume* chld_volume = dynamic_cast<LLVOVolume*>(childp);
                if (chld_volume)
                {
                    is_rigged_mesh = is_rigged_mesh || chld_volume->isRiggedMeshFast();
                    // get cost and individual textures
                    hud_object_complexity.objectsCost += chld_volume->getRenderCost(textures);
                    hud_object_complexity.objectsCount++;
                }
            }
            if (is_rigged_mesh && !attached_object->mRiggedAttachedWarned)
            {
                LLSD args;
                LLViewerInventoryItem* itemp = gInventory.getItem(attached_object->getAttachmentItemID());
                args["NAME"] = itemp ? itemp->getName() : LLTrans::getString("Unknown");
                args["POINT"] = LLTrans::getString(getTargetAttachmentPoint(attached_object)->getName());
                LLNotificationsUtil::add("RiggedMeshAttachedToHUD", args);

                attached_object->mRiggedAttachedWarned = true;
            }

            hud_object_complexity.texturesCount += static_cast<U32>(textures.size());

            for (LLVOVolume::texture_cost_t::iterator volume_texture = textures.begin();
                volume_texture != textures.end();
                ++volume_texture)
            {
                // add the cost of each individual texture (ignores duplicates)
                hud_object_complexity.texturesCost += LLVOVolume::getTextureCost(*volume_texture);
                const LLViewerTexture* img = *volume_texture;
                if (img->getType() == LLViewerTexture::FETCHED_TEXTURE)
                {
                    LLViewerFetchedTexture* tex = (LLViewerFetchedTexture*)img;
                    // Note: Texture memory might be incorect since texture might be still loading.
                    hud_object_complexity.texturesMemoryTotal += tex->getTextureMemory();
                    if (tex->getOriginalHeight() * tex->getOriginalWidth() >= HUD_OVERSIZED_TEXTURE_DATA_SIZE)
                    {
                        hud_object_complexity.largeTexturesCount++;
                    }
                }
            }
        }
    }
}

void LLVOAvatar::setVisualMuteSettings(VisualMuteSettings set)
{
    mVisuallyMuteSetting = set;
    mNeedsImpostorUpdate = true;
    mLastImpostorUpdateReason = 7;

    // <FS:Ansariel> [FS Persisted Avatar Render Settings]
    //LLRenderMuteList::getInstance()->saveVisualMuteSetting(getID(), S32(set));
    FSAvatarRenderPersistence::instance().setAvatarRenderSettings(getID(), set);
}


void LLVOAvatar::setOverallAppearanceNormal()
{
    if (isControlAvatar())
        return;

    LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    if (isControlAvatar() || mLastProcessedAppearance)
    {
        resetSkeleton(false);
    }
    getJoint("mPelvis")->setPosition(pelvis_pos);

    for (auto it = mJellyAnims.begin(); it !=  mJellyAnims.end(); ++it)
    {
        bool is_playing = (mPlayingAnimations.find(*it) != mPlayingAnimations.end());
        LL_DEBUGS("Avatar") << "jelly anim " << *it << " " << is_playing << LL_ENDL;
        if (!is_playing)
        {
            // Anim was not requested for this av by sim, but may be playing locally
            stopMotion(*it);
        }
    }
    mJellyAnims.clear();

    processAnimationStateChanges();
}

void LLVOAvatar::setOverallAppearanceJellyDoll()
{
    if (isControlAvatar())
        return;

    // stop current animations
    {
        for ( LLVOAvatar::AnimIterator anim_it= mPlayingAnimations.begin();
              anim_it != mPlayingAnimations.end();
              ++anim_it)
        {
            {
                stopMotion(anim_it->first, true);
            }
        }
    }
    processAnimationStateChanges();

    // Start any needed anims for jellydoll
    updateOverallAppearanceAnimations();

    LLVector3 pelvis_pos = getJoint("mPelvis")->getPosition();
    resetSkeleton(false);
    getJoint("mPelvis")->setPosition(pelvis_pos);

}

void LLVOAvatar::setOverallAppearanceInvisible()
{
}

void LLVOAvatar::updateOverallAppearance()
{
    AvatarOverallAppearance new_overall = getOverallAppearance();
    if (new_overall != mOverallAppearance)
    {
        switch (new_overall)
        {
            case AOA_NORMAL:
                setOverallAppearanceNormal();
                break;
            case AOA_JELLYDOLL:
                setOverallAppearanceJellyDoll();
                break;
            case AOA_INVISIBLE:
                setOverallAppearanceInvisible();
                break;
        }
        mOverallAppearance = new_overall;
        if (!isSelf())
        {
            mNeedsImpostorUpdate = true;
            mLastImpostorUpdateReason = 8;
        }
        updateMeshVisibility();
    }

    // This needs to be done even if overall appearance has not
    // changed, since sit/stand status can be different.
    updateOverallAppearanceAnimations();
}

void LLVOAvatar::updateOverallAppearanceAnimations()
{
    if (isControlAvatar())
        return;

    if (getOverallAppearance() == AOA_JELLYDOLL)
    {
        LLUUID motion_id;
        if (isSitting() && getParent()) // sitting on object
        {
            motion_id = ANIM_AGENT_SIT_FEMALE;
        }
        else if (isSitting()) // sitting on ground
        {
            motion_id = ANIM_AGENT_SIT_GROUND_CONSTRAINED;
        }
        else // standing
        {
            motion_id = ANIM_AGENT_STAND;
        }
        if (mJellyAnims.find(motion_id) == mJellyAnims.end())
        {
            for (auto it = mJellyAnims.begin(); it !=  mJellyAnims.end(); ++it)
            {
                bool is_playing = (mPlayingAnimations.find(*it) != mPlayingAnimations.end());
                LL_DEBUGS("Avatar") << "jelly anim " << *it << " " << is_playing << LL_ENDL;
                if (!is_playing)
                {
                    // Anim was not requested for this av by sim, but may be playing locally
                    stopMotion(*it, true);
                }
            }
            mJellyAnims.clear();

            startMotion(motion_id);
            mJellyAnims.insert(motion_id);

            processAnimationStateChanges();
        }
    }
}

// Based on isVisuallyMuted(), but has 3 possible results.
LLVOAvatar::AvatarOverallAppearance LLVOAvatar::getOverallAppearance() const
{
    // <FS> PLAN-avatars.md stage A step 5: after the avatars' update the table holds the answer (jellydoll / invisible decisions of the avatar pool, the cull, the velocity pass, the ray scene)
    if (const LLAvatarTable::Row* row = LLAvatarTable::finalRow(this))
    {
        static const bool verify = getenv("LL_VK_AVTABLE_VERIFY") != nullptr;
        if (verify)
        {
            LLAvatarTable::noteCheck(3, row->mAppearance == (U8)computeOverallAppearanceLive());
        }
        return (AvatarOverallAppearance)row->mAppearance;
    }
    return computeOverallAppearanceLive();
}

LLVOAvatar::AvatarOverallAppearance LLVOAvatar::computeOverallAppearanceLive() const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    AvatarOverallAppearance result = AOA_NORMAL;

    // Priority order (highest priority first)
    // * own avatar is always drawn normally
    // * if on the "always draw normally" list, draw them normally
    // * if on the "always visually mute" list, show as jellydoll
    // * if explicitly muted (blocked), show as invisible
    // * check against the render cost and attachment limits - if too complex, show as jellydoll
    if (isSelf())
    {
        result = AOA_NORMAL;
    }
    else // !isSelf()
    {
        if (isInMuteList())
        {
            result = AOA_INVISIBLE;
        }
        // <FS> FIRE-34340-2 RLV silhouettes take priority over Always Render
        else if (isRlvSilhouette() && (!isControlAvatar() || getAttachedAvatar()))
        {
            result = AOA_JELLYDOLL;
        }
        // </FS>
        else if (mVisuallyMuteSetting == AV_ALWAYS_RENDER)
        {
            result = AOA_NORMAL;
        }
        else if (mVisuallyMuteSetting == AV_DO_NOT_RENDER)
        {   // Always want to see this AV as an impostor
            result = AOA_JELLYDOLL;
        }
        else if (isTooComplex() || isTooSlowWithoutShadows()) // <FS:Beq/> correct for misplaced check
        {
            result = AOA_JELLYDOLL;
        }
    }

    return result;
}

void LLVOAvatar::calcMutedAVColor()
{
    LLColor4 new_color(mMutedAVColor);
    std::string change_msg;
    LLUUID av_id(getID());

// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
    if (isRlvSilhouette())
    {
        new_color = LLColor4::silhouette;
        change_msg = " not rendered: color is silhouette";
    }
    else if (getVisualMuteSettings() == AV_DO_NOT_RENDER)
// [/RLVa:KB]
//    if (getVisualMuteSettings() == AV_DO_NOT_RENDER)
    {
        // explicitly not-rendered avatars are light grey
        new_color = LLColor4::grey4;
        change_msg = " not rendered: color is grey4";
    }
    else if (isInMuteList()) // the user blocked them
    {
        // blocked avatars are dark grey
        new_color = LLColor4::grey4;
        change_msg = " blocked: color is grey4";
    }
    // <FS:Beq> we don't want jelly dolls
    // else if (!isTooComplex() && !isTooSlow())
    else if (!isTooComplex())
    // </FS:Beq>
    {
        new_color = LLColor4::white;
        change_msg = " simple imposter ";
    }
#ifdef COLORIZE_JELLYDOLLS
    else if ( mMutedAVColor == LLColor4::white || mMutedAVColor == LLColor4::grey3 || mMutedAVColor == LLColor4::grey4 )
    {
        // select a color based on the first byte of the agents uuid so any muted agent is always the same color
        F32 color_value = (F32) (av_id.mData[0]);
        F32 spectrum = (color_value / 256.0);          // spectrum is between 0 and 1.f

        // Array of colors.  These are arranged so only one RGB color changes between each step,
        // and it loops back to red so there is an even distribution.  It is not a heat map
        const S32 NUM_SPECTRUM_COLORS = 7;
        static LLColor4 * spectrum_color[NUM_SPECTRUM_COLORS] = { &LLColor4::red, &LLColor4::magenta, &LLColor4::blue, &LLColor4::cyan, &LLColor4::green, &LLColor4::yellow, &LLColor4::red };

        spectrum = spectrum * (NUM_SPECTRUM_COLORS - 1);               // Scale to range of number of colors
        S32 spectrum_index_1  = floor(spectrum);                               // Desired color will be after this index
        S32 spectrum_index_2  = spectrum_index_1 + 1;                  //    and before this index (inclusive)
        F32 fractBetween = spectrum - (F32)(spectrum_index_1);  // distance between the two indexes (0-1)

        new_color = lerp(*spectrum_color[spectrum_index_1], *spectrum_color[spectrum_index_2], fractBetween);
        new_color.normalize();
        new_color *= 0.28f;            // Tone it down
    }
#endif
    else
    {
        new_color = LLColor4::grey4;
        change_msg = " over limit color ";
    }

    if (mMutedAVColor != new_color)
    {
        LL_DEBUGS("AvatarRender") << "avatar "<< av_id << change_msg << std::setprecision(3) << new_color << LL_ENDL;
        mMutedAVColor = new_color;
    }
}

// static
bool LLVOAvatar::isIndexLocalTexture(ETextureIndex index)
{
    return (index < 0 || index >= TEX_NUM_INDICES)
        ? false
        : LLAvatarAppearance::getDictionary()->getTexture(index)->mIsLocalTexture;
}

// static
bool LLVOAvatar::isIndexBakedTexture(ETextureIndex index)
{
    return (index < 0 || index >= TEX_NUM_INDICES)
        ? false
        : LLAvatarAppearance::getDictionary()->getTexture(index)->mIsBakedTexture;
}

const std::string LLVOAvatar::getBakedStatusForPrintout() const
{
    std::string line;

    for (LLAvatarAppearanceDictionary::Textures::const_iterator iter = LLAvatarAppearance::getDictionary()->getTextures().begin();
         iter != LLAvatarAppearance::getDictionary()->getTextures().end();
         ++iter)
    {
        const ETextureIndex index = iter->first;
        const LLAvatarAppearanceDictionary::TextureEntry *texture_dict = iter->second;
        if (texture_dict->mIsBakedTexture)
        {
            line += texture_dict->mName;
            if (isTextureDefined(index))
            {
                line += "_baked";
            }
            line += " ";
        }
    }
    return line;
}



//virtual
S32 LLVOAvatar::getTexImageSize() const
{
    return TEX_IMAGE_SIZE_OTHER;
}

//-----------------------------------------------------------------------------
// Utility functions
//-----------------------------------------------------------------------------

F32 calc_bouncy_animation(F32 x)
{
    return -(cosf(x * F_PI * 2.5f - F_PI_BY_TWO))*(0.4f + x * -0.1f) + x * 1.3f;
}

//virtual
bool LLVOAvatar::isTextureDefined(LLAvatarAppearanceDefines::ETextureIndex te, U32 index ) const
{
    if (isIndexLocalTexture(te))
    {
        return false;
    }

    LLViewerTexture* tex = getImage(te, index);
    if (!tex)
    {
        LL_WARNS() << "getImage( " << te << ", " << index << " ) returned 0" << LL_ENDL;
        return false;
    }

    return (tex->getID() != IMG_DEFAULT_AVATAR &&
            tex->getID() != IMG_DEFAULT);
}

//virtual
bool LLVOAvatar::isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, U32 index) const
{
    if (isIndexLocalTexture(type))
    {
        return isTextureDefined(type, index);
    }

    // baked textures can use TE images directly
    return ((isTextureDefined(type) || isSelf()) &&
            (getTEImage(type)->getID() != IMG_INVISIBLE || LLDrawPoolAlpha::sShowDebugAlpha));
}

//virtual
bool LLVOAvatar::isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, LLViewerWearable *wearable) const
{
    // non-self avatars don't have wearables
    return false;
}

void LLVOAvatar::placeProfileQuery()
{
    if (mGPUTimerQuery == 0)
    {
        gRender.genQueries(1, &mGPUTimerQuery);
    }

    gRender.beginQuery(LLRender::QUERY_TIME_ELAPSED, mGPUTimerQuery);
}

void LLVOAvatar::readProfileQuery(S32 retries)
{
    if (!mGPUProfilePending)
    {
        gRender.endQuery(LLRender::QUERY_TIME_ELAPSED);
        mGPUProfilePending = true;
    }

    if (gRender.queryReady(mGPUTimerQuery) || --retries <= 0)
    { // query available, readback result
        const U64 time_elapsed = gRender.queryResult(mGPUTimerQuery);
        mGPURenderTime = time_elapsed / 1000000.f;
        mGPUProfilePending = false;

        setDebugText(llformat("%d", (S32)(mGPURenderTime * 1000.f)));

    }
    else
    {
        // wait until next frame
        const LLUUID id = getID();

        LL::WorkQueue::getInstance("mainloop")->post([id, retries]
        {
            LLViewerObject* object = gObjectList.findObject(id);
            if (object
                && !object->isDead()
                && object->isAvatar()) // probably excessive, pcode isn't supposed to change
            {
                LLVOAvatar* avatar = (LLVOAvatar*)object;
                if (avatar)
                {
                    avatar->readProfileQuery(retries);
                }
            }
        });
    }
}


F32 LLVOAvatar::getGPURenderTime()
{
    return isVisuallyMuted() ? 0.f : mGPURenderTime;
}

// static
F32 LLVOAvatar::getTotalGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        ret += ((LLVOAvatar*)character)->getGPURenderTime();
    }

    return ret;
}

F32 LLVOAvatar::getMaxGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        ret = llmax(((LLVOAvatar*)character)->getGPURenderTime(), ret);
    }

    return ret;
}

F32 LLVOAvatar::getAverageGPURenderTime()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;

    F32 ret = 0.f;

    S32 count = 0;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (!avatar->isTooSlow())
        {
            ret += avatar->getGPURenderTime();
            ++count;
        }
    }

    if (count > 0)
    {
        ret /= count;
    }

    return ret;
}

bool LLVOAvatar::isBuddy() const
{
    bool is_friend = false;
    F64 now = LLFrameTimer::getTotalSeconds();
    if (now < mCachedBuddyListUpdateTime)
    {
        is_friend = mCachedInBuddyList;
    }
    else
    {
        is_friend = LLAvatarTracker::instance().isBuddy(getID());

        const F64 SECONDS_BETWEEN_BUDDY_UPDATES = 1;
        mCachedBuddyListUpdateTime = now + SECONDS_BETWEEN_BUDDY_UPDATES;
        mCachedInBuddyList = is_friend;
    }
    return is_friend;
}


// <FS:Ansariel> [Legacy Bake]
//-----------------------------------------------------------------------------
// Legacy baking
//-----------------------------------------------------------------------------
void LLVOAvatar::bakedTextureOriginCounts(S32 &sb_count, // server-bake, has origin URL.
                                          S32 &host_count, // host-based bake, has host.
                                          S32 &both_count, // error - both host and URL set.
                                          S32 &neither_count) // error - neither set.
{
    sb_count = host_count = both_count = neither_count = 0;

    std::set<LLUUID> baked_ids;
    collectBakedTextureUUIDs(baked_ids);
    for (std::set<LLUUID>::const_iterator it = baked_ids.begin(); it != baked_ids.end(); ++it)
    {
        LLViewerFetchedTexture *imagep = gTextureList.findImage(*it, TEX_LIST_STANDARD);
        bool has_url = false, has_host = false;
        if (!imagep->getUrl().empty())
        {
            has_url = true;
        }
        if (imagep->getTargetHost().isOk())
        {
            has_host = true;
        }
        if (has_url && !has_host) sb_count++;
        else if (has_host && !has_url) host_count++;
        else if (has_host && has_url) both_count++;
        else if (!has_host && !has_url) neither_count++;
    }
}

// virtual
void LLVOAvatar::bodySizeChanged()
{
    if (isSelf() && !LLAppearanceMgr::instance().isInUpdateAppearanceFromCOF())
    {   // notify simulator of change in size
        // but not if we are in the middle of updating appearance
        gAgent.sendAgentSetAppearance();
}
}

bool LLVOAvatar::isUsingServerBakes() const
{
#if 1
    // Sanity check - visual param for appearance version should match mUseServerBakes
    LLVisualParam* appearance_version_param = getVisualParam(11000);
    llassert(appearance_version_param);
    F32 wt = appearance_version_param->getWeight();
    F32 expect_wt = mUseServerBakes ? 1.0f : 0.0f;
    if (!is_approx_equal(wt,expect_wt))
    {
        LL_WARNS() << "wt " << wt << " differs from expected " << expect_wt << LL_ENDL;
    }
#endif

    return mUseServerBakes;
}

void LLVOAvatar::setIsUsingServerBakes(bool newval)
{
    mUseServerBakes = newval;
    LLVisualParam* appearance_version_param = getVisualParam(11000);
    llassert(appearance_version_param);
    appearance_version_param->setWeight(newval ? 1.0f : 0.0f, false);
}
// </FS:Ansariel> [Legacy Bake]


// ---- <FS> C2.4d: the handle and the retire hook of the avatar (llavatarref.h)
U32 LLVOAvatar::getRenderHandle()
{
    U32 handle = mRenderHandle.v.load(std::memory_order_acquire);
    if (handle)
    {
        return handle;
    }
    const U32 fresh = LLAvatarHandles::acquire(this);
    if (!fresh)
    {
        return 0;
    }
    U32 expected = 0;
    if (!mRenderHandle.v.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
    {
        LLAvatarHandles::release(fresh);       // another thread asked first
        return expected;
    }
    return fresh;
}

bool LLVOAvatar::retireOnLastRef() const
{
    return LLRetireList::park(const_cast<LLVOAvatar*>(this));
}

std::atomic<U64> LLAvatarRef::sDeadResolved{0};
