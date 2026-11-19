/**
 * @file llvoavatar.h
 * @brief Declaration of LLVOAvatar class which is a derivation of
 * LLViewerObject
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

#ifndef LL_VOAVATAR_H
#define LL_VOAVATAR_H

#include "rkhandletable.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <deque>
#include <string>
#include <vector>

#include <boost/signals2/trackable.hpp>

#include "llavatarappearance.h"
#include "llchat.h"
#include "lldrawpoolalpha.h"
#include "llviewerobject.h"
#include "llcharacter.h"
#include "llcontrol.h"
#include "llviewerjointmesh.h"
#include "llviewerjointattachment.h"
#include "llrendertarget.h"
#include "rkvkbridge.h"          // <FS> ImpostorQuadRow (fillImpostorQuad)
#include "llavatarappearancedefines.h"
#include "lltexglobalcolor.h"
#include "lldriverparam.h"
#include "llviewertexlayer.h"
#include "material_codes.h"     // LL_MCODE_END
#include "llrigginginfo.h"
#include "llviewerstats.h"
#include "llvovolume.h"
#include "llavatarrendernotifier.h"
#include "llmodel.h"

extern const LLUUID ANIM_AGENT_BODY_NOISE;
extern const LLUUID ANIM_AGENT_BREATHE_ROT;
extern const LLUUID ANIM_AGENT_PHYSICS_MOTION;
extern const LLUUID ANIM_AGENT_EDITING;
extern const LLUUID ANIM_AGENT_EYE;
extern const LLUUID ANIM_AGENT_FLY_ADJUST;
extern const LLUUID ANIM_AGENT_HAND_MOTION;
extern const LLUUID ANIM_AGENT_HEAD_ROT;
extern const LLUUID ANIM_AGENT_PELVIS_FIX;
extern const LLUUID ANIM_AGENT_TARGET;
extern const LLUUID ANIM_AGENT_WALK_ADJUST;

class LLViewerWearable;
class LLSpatialBridge;              // <FS> the attachment rows
class LLViewerJointAttachment;
class LLVoiceVisualizer;
class LLHUDNameTag;
class LLHUDEffectSpiral;
class LLTexGlobalColor;

struct LLAppearanceMessageContents;
class LLViewerJointMesh;

const F32 MAX_AVATAR_LOD_FACTOR = 1.0f;

extern U32 gFrameCount;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LLVOAvatar
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLVOAvatar;
struct RKFlatSkeleton;      // <FS> stage B0 (llvoavatar.cpp): the avatar's world matrices as a flat array by joint number
using RKAvatarHandles = RKHandleTable<LLVOAvatar>;       // <FS> C2.4d: the handles of avatars (rkhandletable.h)

class LLVOAvatar :
    public LLAvatarAppearance,
    public LLViewerObject,
    public boost::signals2::trackable
{
    LL_ALIGN_NEW;
    LOG_CLASS(LLVOAvatar);

public:
    friend class LLVOAvatarSelf;
    friend class LLAvatarCheckImpostorMode;
    friend class LLVisualParamHint;

/********************************************************************************
 **                                                                            **
 **                    INITIALIZATION
 **/

public:
    LLVOAvatar(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);
    virtual void        markDead();
    static void         initClass(); // Initialize data that's only init'd once per class.
    static void         cleanupClass(); // Cleanup data that's only init'd once per class.
    static void initCloud();
    // <FS:TP> [FIRE-36987] Selectable avatar loading/bakefail cloud color
    // Applies the FSCloudColorStart/FSCloudColorEnd settings to sCloud's
    // particle colors. Safe to call anytime after initCloud() - takes
    // effect on the very next idle tick for any avatar currently showing
    // as a cloud, no restart required.
    static void applyCloudColor();
    // </FS:TP>
    virtual void        initInstance(); // Called after construction to initialize the class.

protected:
public:
    // <FS> C2.4d (doc/vulkan/PLAN-clean-core.md section 7): a handle for render-side data (RKAvatarRef, rkavatarref.h) and the retire list (llcommon/rkretirelist.h, kind 3): the last owner letting go
    // parks the avatar for a few frames, so a draw info that still reads it through its handle finds it intact
    U32 getRenderHandle();
    /*virtual*/ bool retireOnLastRef() const override;
    /*virtual*/ U32 retireKind() const override { return 3; }
    struct RenderHandle
    {
        std::atomic<U32> v{0};
        RenderHandle() = default;
        RenderHandle(const RenderHandle&) {}
        RenderHandle& operator=(const RenderHandle&) { return *this; }
    };
    RenderHandle mRenderHandle;

protected:
    virtual             ~LLVOAvatar();

/**                    Initialization
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    INHERITED
 **/

    //--------------------------------------------------------------------
    // LLViewerObject interface and related
    //--------------------------------------------------------------------
public:
    /*virtual*/ void            updateResources();
    /*virtual*/ LLVOAvatar*     asAvatar();

    virtual U32                 processUpdateMessage(LLMessageSystem *mesgsys,
                                                     void **user_data,
                                                     U32 block_num,
                                                     const EObjectUpdateType update_type,
                                                     LLDataPacker *dp);
    virtual void                idleUpdate(LLAgent &agent, const F64 &time);
    /*virtual*/ bool            updateLOD();
    bool                        updateJointLODs();
    void                        updateLODRiggedAttachments(void);
    void                        setCorrectedPixelArea(F32 area);
    /*virtual*/ bool            isActive() const; // Whether this object needs to do an idleUpdate.
    S32Bytes                    totalTextureMemForUUIDS(std::set<LLUUID>& ids);
    bool                        allTexturesCompletelyDownloaded(std::set<LLUUID>& ids) const;
    bool                        allLocalTexturesCompletelyDownloaded() const;
    bool                        allBakedTexturesCompletelyDownloaded() const;
    void                        bakedTextureOriginCounts(S32 &sb_count, S32 &host_count,
                                                         S32 &both_count, S32 &neither_count);
    std::string                 bakedTextureOriginInfo();
    void                        collectLocalTextureUUIDs(std::set<LLUUID>& ids) const;
    void                        collectBakedTextureUUIDs(std::set<LLUUID>& ids) const;
    void                        collectTextureUUIDs(std::set<LLUUID>& ids);
    void                        releaseOldTextures();
    /*virtual*/ void            updateTextures();
    LLViewerFetchedTexture*     getBakedTextureImage(const U8 te, const LLUUID& uuid);
    /*virtual*/ S32             setTETexture(const U8 te, const LLUUID& uuid); // If setting a baked texture, need to request it from a non-local sim.
    /*virtual*/ void            onShift(const LLVector4a& shift_vector);
    /*virtual*/ U32             getPartitionType() const;
    /*virtual*/ const           LLVector3 getRenderPosition() const;
    /*virtual*/ void            updateDrawable(bool force_damped);
    /*virtual*/ LLDrawable*     createDrawable(LLPipeline *pipeline);
    /*virtual*/ bool            updateGeometry(LLDrawable *drawable);
    /*virtual*/ void            setPixelAreaAndAngle(LLAgent &agent);
    /*virtual*/ void            updateRegion(LLViewerRegion *regionp);
    /*virtual*/ void            updateSpatialExtents(LLVector4a& newMin, LLVector4a &newMax);
    void                        calculateSpatialExtents(LLVector4a& newMin, LLVector4a& newMax);
    /*virtual*/ bool            lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end,
                                                 S32 face = -1,                    // which face to check, -1 = ALL_SIDES
                                                 bool pick_transparent = false,
                                                 bool pick_rigged = false,
                                                 bool pick_unselectable = true,
                                                 S32* face_hit = NULL,             // which face was hit
                                                 LLVector4a* intersection = NULL,   // return the intersection point
                                                 LLVector2* tex_coord = NULL,      // return the texture coordinates of the intersection point
                                                 LLVector4a* normal = NULL,         // return the surface normal at the intersection point
                                                 LLVector4a* tangent = NULL);     // return the surface tangent at the intersection point
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

    //--------------------------------------------------------------------
    // LLCharacter interface and related
    //--------------------------------------------------------------------
public:
    /*virtual*/ LLVector3       getCharacterPosition();
    /*virtual*/ LLQuaternion    getCharacterRotation();
    /*virtual*/ LLVector3       getCharacterVelocity();
    /*virtual*/ LLVector3       getCharacterAngularVelocity();

    /*virtual*/ LLUUID          remapMotionID(const LLUUID& id);
    /*virtual*/ bool            startMotion(const LLUUID& id, F32 time_offset = 0.f);
    /*virtual*/ bool            stopMotion(const LLUUID& id, bool stop_immediate = false);
    virtual bool            hasMotionFromSource(const LLUUID& source_id);
    virtual void            stopMotionFromSource(const LLUUID& source_id);
    virtual void            requestStopMotion(LLMotion* motion);
    LLMotion*               findMotion(const LLUUID& id) const;
    void                    startDefaultMotions();
    void                    dumpAnimationState();

    virtual LLJoint*        getJoint(std::string_view name);
    LLJoint*                getJoint(S32 num);
    void                    initAllJoints();

    //if you KNOW joint_num is a valid animated joint index, use getSkeletonJoint for efficiency
    inline LLJoint* getSkeletonJoint(S32 joint_num) { return mSkeleton[joint_num]; }
    inline size_t getSkeletonJointCount() const { return mSkeleton.size(); }

    void                    notifyAttachmentMeshLoaded();
    void                    addAttachmentOverridesForObject(LLViewerObject *vo, std::set<LLUUID>* meshes_seen = NULL, bool recursive = true);
    void                    removeAttachmentOverridesForObject(const LLUUID& mesh_id);
    void                    removeAttachmentOverridesForObject(LLViewerObject *vo);
    bool                    jointIsRiggedTo(const LLJoint *joint) const;
    void                    clearAttachmentOverrides();
    void                    rebuildAttachmentOverrides();
    void                    updateAttachmentOverrides();
    void                    showAttachmentOverrides(bool verbose = false) const;
    void                    getAttachmentOverrideNames(std::set<std::string>& pos_names,
                                                       std::set<std::string>& scale_names) const;

    void                    getAssociatedVolumes(std::vector<LLVOVolume*>& volumes);

    // virtual
    void                    updateRiggingInfo();
    // This encodes mesh id and LOD, so we can see whether display is up-to-date.
    size_t    mLastRiggingInfoKey;

    std::set<LLUUID>        mActiveOverrideMeshes;
    virtual void            onActiveOverrideMeshesChanged();

    /*virtual*/ const LLUUID&   getID() const;
    /*virtual*/ std::string     getDebugName() const;
    /*virtual*/ void            addDebugText(const std::string& text);
    /*virtual*/ F32             getTimeDilation();
    /*virtual*/ void            getGround(const LLVector3 &inPos, LLVector3 &outPos, LLVector3 &outNorm);
    /*virtual*/ F32             getPixelArea() const;
    /*virtual*/ LLVector3d      getPosGlobalFromAgent(const LLVector3 &position);
    /*virtual*/ LLVector3       getPosAgentFromGlobal(const LLVector3d &position);
    virtual void                updateVisualParams();

/**                    Inherited
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    STATE
 **/

public:
    virtual bool    isSelf() const { return false; } // True if this avatar is for this viewer's agent

    virtual bool    isControlAvatar() const { return mIsControlAvatar; } // True if this avatar is a control av (no associated user)
    virtual bool    isUIAvatar() const { return mIsUIAvatar; } // True if this avatar is a supplemental av used in some UI views (no associated user)
    virtual bool    isBuddy() const;

    // If this is an attachment, return the avatar it is attached to. Otherwise NULL.
    virtual const LLVOAvatar *getAttachedAvatar() const { return NULL; }
    virtual LLVOAvatar *getAttachedAvatar() { return NULL; }


private: //aligned members
    LL_ALIGN_16(LLVector4a  mImpostorExtents[2]);

    //--------------------------------------------------------------------
    // Updates
    //--------------------------------------------------------------------
public:
    void            updateAppearanceMessageDebugText();
    void            updateAnimationDebugText();
    virtual void    updateDebugText();
    virtual bool    computeNeedsUpdate();
    virtual bool    updateCharacter(LLAgent &agent);
    // <FS> PLAN-avatars.md stage A: the avatar table's decision for this avatar this frame (rkavatartable.h): the gates of idleUpdate / updateCharacter in their order; returns a RKAvatarTable::Mode
    unsigned char   decideFrameMode(bool& visible, bool& needs_update);
    bool            nameTagBusy() const { return mTyping || !mChats.empty(); }      // chat bubbles or the typing indicator show: the tag is updated whatever the camera sees
    std::shared_ptr<RKFlatSkeleton> mFlatSkel;          // <FS> stage B0
    bool            flatPaletteRel(LLMatrix4a* mat, S32 count, const LLMeshSkinInfo* skin, const F32* anchor);       // <FS> avatar relative skinning: the same palette from world matrices made relative to the anchor (false: not possible, the plain one stands)
    void            flatPalette(LLMatrix4a* mat, S32 count, const LLMeshSkinInfo* skin);       // the skin's palette from the flat array (the old per-joint build only where the array lacks a joint)
    mutable S32     mAvTableSlot = -1;                  // <FS> the avatar's row in RKAvatarTable's array this frame (valid when the row at the slot names this avatar)
    bool            computeTooSlowLive() const;         // isTooSlow() derived from the avatar's state (what the table caches at endFrame)
    bool            computeVisuallyMutedLive();         // isVisuallyMuted() derived likewise
    bool            computeImpostorLive();      // isImpostor() derived from the avatar's state (what the table caches at endFrame)
    bool            reducedHiddenSkip();        // idleUpdate's reduced update of an avatar nobody sees (every 2nd or 4th frame)
    void            updateCharacterPostMotion(bool was_sit_ground_constrained, bool visible, U32 clean_serial = ~0u);
    void            applyGroundSitHoverInJob(bool try_flat);      // <FS> PLAN-avatars-100.md D2c: the ground sit hover offset, added at the end of the pool job (RK_VK_HOVERJOB=0: in the completion as before)
    void            idleUpdatePostCharacter(bool detailed_update);
    bool            canDeferMotionUpdate() const;
    // Runs the deferred pose blend phase. Safe to call from a worker thread
    // while no other thread touches avatars or the world.
    void            runDeferredMotionBlend();
    // <FS> texture priority jobs (plan 8.26): every joint a rigged mesh can be rigged to has its world matrix up to date, so that LLJoint::getWorldMatrix only reads: the skeleton below the root, the collision
    // volumes and the attachment points (which do not update with the tree). Main thread.
    void            cleanJointMatrices();
    // the same, once a frame, from any thread: the first caller cleans, the others wait for it (the texture priority jobs: only the avatars that own a rigged face the jobs look at are cleaned)
    void            cleanJointMatricesOnce(U32 frame);
    std::atomic<U32> mJointCleanState{ 0 };     // the frame of the last clean, with 0x80000000 set while it is being done
    // <FS> stage d (view prep rework): the blend of the own avatar runs in the batch too. The motion controller asks the avatar to stop a motion (requestStopMotion), which for the own avatar is a message to the
    // server and a change of the agent's state: while the blend runs as a job the ids are kept (sBlendJobAvatar names the avatar a job is blending) and the requests are made when the job is done
    static thread_local LLVOAvatar* sBlendJobAvatar;
    virtual void    flushDeferredStopRequests() {}
    // Runs the rest of idleUpdate() on the main thread once the blend is done.
    void            completeDeferredMotionUpdate();
    void            updateFootstepSounds();
    void            computeUpdatePeriod();
    void            updateOrientation(LLAgent &agent, F32 speed, F32 delta_time);
    void            updateTimeStep();
    void            updateRootPositionAndRotation(LLAgent &agent, F32 speed, bool was_sit_ground_constrained);

    void            idleUpdateVoiceVisualizer(bool voice_enabled, const LLVector3 &position);
    void            idleUpdateMisc(bool detailed_update);
    // <FS> view rework A2: the attachment loop of idleUpdateMisc as a function of its own; runAttachmentJob runs it with the avatar's writes recorded (a pool job) and
    // idleUpdateMisc replays them in its turn (replayAttachmentJob) instead of running the loop
    void            idleUpdateAttachments(bool visible);
    // <FS> PLAN-avatars.md stage B3a (RK_VK_PALJOB: 1 default, 0 the lazy palettes only, 2 job and compare): after the completions of the frame, a pool job per avatar rebuilds the palettes the avatar had asked for in the last
    // rendered frame, for the frame about to be rendered (the skeleton is final by then), so the requests of the render find them ready and no thread builds them under the global palette lock
    void            buildPalettesInJob();
    // <FS> PLAN-avatars.md stage C: the flat world array (RKFlatSkeleton) is the OUTPUT of the avatar's pose job: filled at its end, from the world
    // walk's result; the attachment rows and the palette job read it, and the palette job fills it again only when a joint was touched after (skeletonDirty)
    void            ensureJointLocals();        // the joints' locals into the flat array
    void            releaseJointViews();        // the joints stop pointing into this avatar's arrays
    void            walkWorldAfterPass(bool rerun_pass);   // the xform chain walked outside the pose job (a root move, a lazy clean): the flat array of this frame no longer holds, so it is made stale, or the pass runs again
    bool            flatWorldPass();            // the flat world pass over the joints in array order; false: the walk does it
    bool            skeletonDirty() const;
    bool            flatJoint(S32 joint_num, const LLVector3*& pos, const LLQuaternion*& rot, const LLVector3*& scale, bool& scale_child_offset) const;      // the joint's world position, rotation, own scale and scale-child-offset flag from the array the pass made this frame
    const F32*      flatWorld(S32 joint_num) const;     // the joint's world matrix (16 floats) from the array when it was filled this frame, else null
    // <FS> PLAN-avatars.md stage C: the plain rigid attachments of the avatar as a list of rows, built when the
    // attachment set changes (LLViewerJointAttachment::sRevision) and swept every 32 frames, not found by walking the attachment points and testing each object every frame
    struct AttRow
    {
        LLViewerObject* mObject = nullptr;
        LLDrawable* mDrawable = nullptr;
        LLSpatialBridge* mBridge = nullptr;
        LLViewerJointAttachment* mPoint = nullptr;
        bool mRigged = false;       // stage C4: a rigged attachment (drawn with the avatar's joints, held at the avatar's box), moved by the rows with the rigged rule
    };
    std::vector<AttRow> mAttRows;
    U32 mAttRowsRevision = ~0u;
    void            rebuildAttRows();
    static void     sPalJobDone();                  // (the palette jobs of a batch are done: remember the joint touch serial; a joint touched after it makes the job's palettes suspect)
    static void     sPalJobWallNsAdd(U64 ns);       // (RK_VK_PALTIME: the wall time of the palette jobs of a batch)
    bool            canRunAttachmentJob();
    static LLVOAvatar* sAttachmentSelectedAvatar;   // with sAttachmentSelectedForJobs == 1: the avatar the selected attachments belong to (null: not known, every avatar counts as selected)
    static void     askAttachmentSelection(S32& selected, LLVOAvatar*& avatar);     // main thread: is an attachment (or a HUD object) selected, and whose
    static S32      sAttachmentSelectedForJobs;     // -1: ask the selection manager; 0/1: what the main thread found before the jobs started (getObjectCount() cleans the selection's node list: not for several threads)
    void            runAttachmentJob();
    void            replayAttachmentJob();
    bool            mAttJobDone = false;
    std::shared_ptr<void> mAttLedger;       // the writes of the attachment job (AvJobLedger in llvoavatar.cpp), made on first use
    virtual void    idleUpdateAppearanceAnimation();
    void            idleUpdateLipSync(bool voice_enabled);
    void            idleUpdateLoadingEffect();
    void            idleUpdateWindEffect();
    void            idleUpdateNameTag(const LLVector3& root_pos_last);
    void            idleUpdateNameTagText(bool new_name);
    void            idleUpdateNameTagAlpha(bool new_name, F32 alpha);
    // <FS:CR> Colorize tags
    //LLColor4      getNameTagColor(bool is_friend);
    LLColor4        getNameTagColor();
    // </FS:CR>
    void            clearNameTag();
    static void     invalidateNameTag(const LLUUID& agent_id);
    // force all name tags to rebuild, useful when display names turned on/off
    static void     invalidateNameTags();
    // <FS:Ansariel> Fix nametag not properly updating when display name arrives
    //void          addNameTagLine(const std::string& line, const LLColor4& color, S32 style, const LLFontGL* font, const bool use_ellipses = false);
    void            addNameTagLine(const std::string& line, const LLColor4& color, S32 style, const LLFontGL* font, const bool use_ellipses = false, bool is_name = false);
    // </FS:Ansariel>
    void            idleUpdateRenderComplexity();
    void            idleUpdateDebugInfo();
    void            accountRenderComplexityForObject(LLViewerObject *attached_object,
                                                     const F32 max_attachment_complexity,
                                                     LLVOVolume::texture_cost_t& textures,
                                                     U32& cost,
                                                     U32& visible_triangle_count,
                                                     F32& est_triangle_count,
                                                     F32& surface_area,
                                                     LLHUDComplexity& hud_object_complexity,
                                                     LLObjectComplexity& object_complexity);
    void            calculateUpdateRenderComplexity();
    static const U32 VISUAL_COMPLEXITY_UNKNOWN;
    void            updateVisualComplexity();
    // Mark that an attachment needs complexity recalculation
    void markAttachmentComplexityDirty(const LLUUID& object_id, bool force_reset_attachment = false);
    void markBodyPartsComplexityDirty();

    void placeProfileQuery();
    void readProfileQuery(S32 retries);

    // get the GPU time in ms of rendering this avatar including all attachments
    // returns 0.f if this avatar has not been profiled using gPipeline.profileAvatar
    // or the avatar is visually muted
    F32             getGPURenderTime();

    // get the total GPU render time in ms of all avatars that have been benched
    static F32      getTotalGPURenderTime();

    // get the max GPU render time in ms of all avatars that have been benched
    static F32      getMaxGPURenderTime();

    // get the average GPU render time in ms of all avatars that have been benched
    static F32      getAverageGPURenderTime();

    // get the CPU time in ms of rendering this avatar including all attachments
    // return 0.f if this avatar has not been profiled using gPipeline.mProfileAvatar
    F32             getCPURenderTime() { return mCPURenderTime; }


    // avatar render cost
    U32             getVisualComplexity()           { return mVisualComplexity;             };

    // surface area calculation
    F32             getAttachmentSurfaceArea()      { return mAttachmentSurfaceArea;        };

    U32             getReportedVisualComplexity()                   { return mReportedVisualComplexity;             };  // Numbers as reported by the SL server
    void            setReportedVisualComplexity(U32 value)          { mReportedVisualComplexity = value;            };

    S32             getUpdatePeriod()               { return mUpdatePeriod;         };
    const LLColor4 &  getMutedAVColor()             { return mMutedAVColor;         };
    static void     updateImpostorRendering(U32 newMaxNonImpostorsValue);

    void            idleUpdateBelowWater();

    static void updateNearbyAvatarCount();

    LLVector3 idleCalcNameTagPosition(const LLVector3 &root_pos_last);

    //--------------------------------------------------------------------
    // Static preferences (controlled by user settings/menus)
    //--------------------------------------------------------------------
public:
    static S32      sRenderName;
    static S32      sRenderGroupTitles;
    static const U32 NON_IMPOSTORS_MAX_SLIDER; /* Must equal the maximum allowed the RenderAvatarMaxNonImpostors
                                                * slider in panel_preferences_graphics1.xml */
    static U32      sMaxNonImpostors; // affected by control "RenderAvatarMaxNonImpostors"
    static bool     sLimitNonImpostors; // use impostors for far away avatars
    static F32      sRenderDistance; // distance at which avatars will render.
    static bool     sShowAnimationDebug; // show animation debug info
    static bool     sShowCollisionVolumes;  // show skeletal collision volumes
    static bool     sVisibleInFirstPerson;
    static S32      sNumLODChangesThisFrame;
    // While non-null, eligible avatars defer the pose blend phase of their
    // motion update into this batch instead of running it inline. The owner
    // runs the batch (see LLViewerObjectList::update) and then calls
    // completeDeferredMotionUpdate() on each entry.
    static std::vector<LLPointer<LLVOAvatar> >* sMotionUpdateBatch;
    static S32      sNumVisibleChatBubbles;
    static bool     sDebugInvisible;
    static bool     sShowAttachmentPoints;
    static F32      sLODFactor; // user-settable LOD factor
    static F32      sPhysicsLODFactor; // user-settable physics LOD factor
    static bool     sJointDebug; // output total number of joints being touched for each avatar

    static LLPartSysData sCloud;

    // <FS:TP> [FIRE-36987] track the signal connections so initCloud() can
    // disconnect and reconnect cleanly if it runs more than once (there is
    // a menu option to reload the particle cloud)
    static boost::signals2::connection sCloudColorStartConnection;
    static boost::signals2::connection sCloudColorEndConnection;
    // </FS:TP>

    static LLPointer<LLViewerTexture>  sCloudTexture;

    static std::vector<LLUUID> sAVsIgnoringARTLimit;
    static S32 sAvatarsNearby;

    //--------------------------------------------------------------------
    // Region state
    //--------------------------------------------------------------------
public:
    LLHost          getObjectHost() const;

    //--------------------------------------------------------------------
    // Loading state
    //--------------------------------------------------------------------
public:
    bool            isFullyLoaded() const;
    bool            hasFirstFullAttachmentData() const;
    F32             getFirstDecloudTime() const {return mFirstDecloudTime;}

    // check and return current state relative to limits
    // default will test only the geometry (combined=false).
    // this allows us to disable shadows separately on complex avatars.

    inline bool     isTooSlowWithoutShadows() const {return mTooSlowWithoutShadows;};
    bool    isTooSlow() const;

    void            updateTooSlow();

    virtual bool    isTooComplex() const; // <FS:Ansariel> FIRE-29012: Standalone animesh avatars get affected by complexity limit; changed to virtual
    bool            visualParamWeightsAreDefault();
    virtual bool    getHasMissingParts() const;
    bool            isFullyTextured() const;
    bool            hasGray() const;
    S32             getRezzedStatus() const; // 0 = cloud, 1 = gray, 2 = textured, 3 = waiting for attachments, 4 = full.
    void            updateRezzedStatusTimers(S32 status);
    S32             getNumBakes() const;//<FS:Beq/> BOM bake limits
    // U8               getNumTEs() const override;//<FS:Beq/> BOM bake limits

    S32             mLastRezzedStatus;

    void            startPhase(const std::string& phase_name);
    void            stopPhase(const std::string& phase_name, bool err_check = true);
    void            clearPhases();
    void            logPendingPhases();
    static void     logPendingPhasesAllAvatars();
    void            logMetricsTimerRecord(const std::string& phase_name, F32 elapsed, bool completed);

    void            calcMutedAVColor();

protected:
    LLViewerStats::PhaseMap& getPhases() { return mPhases; }
    bool            updateIsFullyLoaded();
    bool            processFullyLoadedChange(bool loading);
    void            updateRuthTimer(bool loading);
    F32             calcMorphAmount() const;

private:
    bool            mFirstFullyVisible;
    bool            mWaitingForMeshes;
    F32             mFirstDecloudTime;
    LLFrameTimer    mFirstAppearanceMessageTimer;

    bool            mFullyLoaded;
    bool            mPreviousFullyLoaded;
    bool            mFullyLoadedInitialized;
    S32             mFullyLoadedFrameCounter;
    LLColor4        mMutedAVColor;
    LLFrameTimer    mFullyLoadedTimer;
    LLFrameTimer    mRuthTimer;

    // variables to hold "slowness" status
    bool            mTooSlow{false};
    bool            mTooSlowWithoutShadows{false};

    bool            mTuned{false};

private:
    LLViewerStats::PhaseMap mPhases;

protected:
    LLFrameTimer    mInvisibleTimer;

/**                    State
 **                                                                            **
 *******************************************************************************/
/********************************************************************************
 **                                                                            **
 **                    SKELETON
 **/

protected:
    /*virtual*/ LLAvatarJoint*  createAvatarJoint(); // Returns LLViewerJoint
    /*virtual*/ LLAvatarJoint*  createAvatarJoint(S32 joint_num); // Returns LLViewerJoint
    /*virtual*/ LLAvatarJointMesh*  createAvatarJointMesh(); // Returns LLViewerJointMesh
public:
    void                updateHeadOffset();
    void                debugBodySize() const;
    void                postPelvisSetRecalc( void );

    /*virtual*/ bool    loadSkeletonNode();
    void                initAttachmentPoints(bool ignore_hud_joints = false);
    /*virtual*/ void    buildCharacter();
    void                resetVisualParams();
    void                applyDefaultParams();
    void                resetSkeleton(bool reset_animations);

    LLVector3           mCurRootToHeadOffset;
    LLVector3           mTargetRootToHeadOffset;

    S32                 mLastSkeletonSerialNum;


/**                    Skeleton
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    RENDERING
 **/

public:
    // <FS> 2026-11-10: the impostor's quad is a row of the pass's one impostor record (RKVkBridge kind 41), not an immediate mode draw: fillImpostorQuad makes the row (false: no picture yet),
    // renderImpostorDebug draws the outline of the quad (the RENDER_DEBUG_IMPOSTORS view)
    bool        fillImpostorQuad(RKVkBridge::ImpostorQuadRow& row) const;
    void        renderImpostorDebug(S32 diffuse_channel);
    bool        isVisuallyMuted();
    bool        isInMuteList() const;
// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
    bool        isRlvSilhouette() const;
// [/RLVa:KB]

    // states for RenderAvatarComplexityMode
    enum ERenderComplexityMode
    {
        AV_RENDER_LIMIT_BY_COMPLEXITY = 0,
        AV_RENDER_ALWAYS_SHOW_FRIENDS = 1,
        AV_RENDER_ONLY_SHOW_FRIENDS   = 2
    };

    // Visual Mute Setting is an input. Does not necessarily determine
    // what the avatar looks like, because it interacts with other
    // settings like muting, complexity threshold. Should be private or protected.
    enum VisualMuteSettings
    {
        AV_RENDER_NORMALLY = 0,
        AV_DO_NOT_RENDER   = 1,
        AV_ALWAYS_RENDER   = 2
    };
    void        setVisualMuteSettings(VisualMuteSettings set);
protected:
    // If you think you need to access this outside LLVOAvatar, you probably want getOverallAppearance()
    VisualMuteSettings  getVisualMuteSettings()                        { return mVisuallyMuteSetting;    };

public:

    // Overall Appearance is an output. Depending on whether the
    // avatar is blocked/muted, whether it exceeds the complexity
    // threshold, etc, avatar will want to be displayed in one of
    // these ways. Rendering code that wants to know how to display an
    // avatar should be looking at this value, NOT the visual mute
    // settings
    enum AvatarOverallAppearance
    {
        AOA_NORMAL,
        AOA_JELLYDOLL,
        AOA_INVISIBLE
    };

    AvatarOverallAppearance getOverallAppearance() const;
    AvatarOverallAppearance computeOverallAppearanceLive() const;       // <FS> getOverallAppearance() derived from the avatar's state (what the table caches at endFrame)
    void setOverallAppearanceNormal();
    void setOverallAppearanceJellyDoll();
    void setOverallAppearanceInvisible();

    void updateOverallAppearance();
    void updateOverallAppearanceAnimations();

    std::set<LLUUID> mJellyAnims;

    U32         renderRigid();
    U32         renderSkinned();
    F32         getLastSkinTime() { return mLastSkinTime; }
    U32         renderTransparent(bool first_pass);
    void        renderCollisionVolumes();
    void        renderBones(const std::string &selected_joint = std::string());
    virtual void renderJoints();
    void        renderOnlySelectedBones(const std::vector<std::string> &selected_joints);
    void        renderBoxAroundJointAttachments(LLJoint * joint);

    static void deleteCachedImages(bool clearAll=true);
    static void destroyResources();
    static void restoreResources();
    S32         mSpecialRenderMode; // special lighting

private:
    friend class LLPipeline;
    AvatarOverallAppearance mOverallAppearance;
    F32         mAttachmentSurfaceArea; //estimated surface area of attachments
    U32         mAttachmentVisibleTriangleCount;
    F32         mAttachmentEstTriangleCount;
    bool        shouldAlphaMask();

    bool        mNeedsSkin; // avatar has been animated and verts have not been updated
    F32         mLastSkinTime; //value of gFrameTimeSeconds at last skin update

    // state carried across a deferred motion update, see sMotionUpdateBatch
    bool        mMotionUpdateDeferred = false;
    bool        mBelowWaterDone = false;        // <FS> B3d: the pool job already worked out mBelowWater this frame
    U32         mWindFrame = 0xFFFFFFFFu;       // <FS> B3d: the frame the cloth wind state was last worked out (at draw)
    bool        mRootInJob = false;             // <FS> stage B3d: the root position and rotation are worked out at the start of the pool job (updateCharacter set it with the batch entry)
    F32         mDeferredSpeed = 0.f;           // (the speed updateCharacter worked out, for the root update in the job)
    bool        mDeferredMatricesOnly = false; // <FS> hidden avatar: only its skeleton's world matrices are worked out on the worker (no pose blend)
    bool        mDeferredSitGroundConstrained = false;
    bool        mHoverDone = false;             // <FS> PLAN-avatars-100.md D2c: the pool job already added the ground sit hover offset (applyGroundSitHoverInJob); the completion writes nothing
    F32         mHoverZ = 0.f;                  // (the offset it added, for the completion's check)
    bool        mDeferredVisible = false;
public:
    bool        mFarTierHeavy = false;          // the avatar's rigged attachments are over the budget of its far tier: it is drawn as an impostor (shouldImpostor), never cut apart
    U8          mFarTierMain = 0;               // the far avatar tier of the main view (pipeline.cpp far_avatar_tier): 0 full, 1 beyond RenderFarAvatarDistance, 2 beyond RenderFarAvatarDistance2 (hysteresis)
    U32         farUpdatePeriod();              // 1 near, or 2 / 3 for a visible avatar beyond RKAvatarFarUpdateDistance (see llvoavatar.cpp)
    static bool isLiveInstance(const LLVOAvatar* avatar);     // a pointer a spatial group kept (LLSpatialGroup::mAvatarp) may name an avatar that is gone: true when it is one of the characters alive now (a set made once a frame)
    std::vector<F32> mRtPosePrev;           // census (RK_VK_ITEMCENSUS): the palette of its biggest skin when the main view last named it to the ray scene
    U64         mRtNameLast = 0;                // the frame the main view last named this avatar's rigged draws to the ray scene (pipeline.cpp rt_name_rigged_main_view)
    U64         mRtNameChecked = ~0ull;         // the frame mRtNameDue was decided in
    bool        mRtNameDue = true;
    bool        mRtWasLoaded = false;           // <FS> this avatar has been fully loaded at least once: the ray scene's loading gate (rt_avatar_kind, kind 64) is for the first load only. isFullyLoaded()
                                                // flips back to false for a second whenever a texture or attachment arrives later (updateIsFullyLoaded resets the timer), and every such flip stopped the
                                                // naming: the ray scene kept the avatar's LAST skinned pose for up to 8 frames while the drawn body moved on, then jumped (2026-11-05, Adam: a new
                                                // avatar "blinking their last frames", lit and shadowed copies out of step)
private:
    bool        mDeferredDetailedUpdate = false;

    S32         mUpdatePeriod;
    S32         mNumInitFaces; //number of faces generated when creating the avatar drawable, does not inculde splitted faces due to long vertex buffer.

    // profile handle
    U32 mGPUTimerQuery = 0;

    // profile results

    // GPU render time in ms
    F32 mGPURenderTime = 0.f;
    bool mGPUProfilePending = false;

    // CPU render time in ms
    F32 mCPURenderTime = 0.f;

    mutable bool        mCachedInMuteList;
    mutable F64         mCachedMuteListUpdateTime;
    mutable bool        mCachedInBuddyList = false;
    mutable F64         mCachedBuddyListUpdateTime = 0.0;
// [RLVa:KB] - Checked: RLVa-2.2 (@setcam_avdist)
    mutable bool        mCachedIsRlvSilhouette = false;
    mutable F64         mCachedRlvSilhouetteUpdateTime = 0.f;
// [/RLVa:KB]

    VisualMuteSettings      mVisuallyMuteSetting;           // Always or never visually mute this AV

    //--------------------------------------------------------------------
    // Complexity calculation and caching
    //--------------------------------------------------------------------

private:
    // Structure to cache complexity metrics for individual attachments or components
    struct ComplexityComponent
    {
        U32 render_cost;
        U32 triangle_count;
        F32 surface_area;
        F32 est_triangle_count;
        LLVOVolume::texture_cost_t textures;
        F64 last_update_time;
        bool needs_update;

        // For tracking HUD and object lists
        LLHUDComplexity hud_complexity;
        LLObjectComplexity object_complexity;

        ComplexityComponent()
            : render_cost(0)
            , triangle_count(0)
            , surface_area(0.f)
            , est_triangle_count(0.f)
            , last_update_time(0.0)
            , needs_update(true)
        {
        }

        void reset()
        {
            render_cost = 0;
            triangle_count = 0;
            surface_area = 0.f;
            est_triangle_count = 0.f;
            textures.clear();
            hud_complexity.reset();
            object_complexity.reset();
            needs_update = true;
        }
    };

    void calculateAttachmentComplexity(LLViewerObject* attached_object,
        const F32 max_attachment_complexity,
        ComplexityComponent& cache);
    void calculateBodyPartsComplexity(ComplexityComponent& cache);
    U32 calculateBodyPartsComplexity();

    // return true, if a valid control avatar.
    bool calculateControlAvatarComplexity(ComplexityComponent& cache, const F32 max_attachment_complexity);

    void accumulateComplexityComponent(const ComplexityComponent& component,
        U32& total_cost,
        hud_complexity_list_t& hud_list,
        object_complexity_list_t& object_list);

    bool shouldUpdateComplexityComponent(const ComplexityComponent& component) const;
    void performPartialComplexityUpdate(const F32 max_attachment_complexity);

    // <FS:Ansariel> Show per-item complexity in COF
    //void processComplexityCostChange(const hud_complexity_list_t &hud_complexity_list, const object_complexity_list_t &object_complexity_list);
    void processComplexityCostChange(const hud_complexity_list_t& hud_complexity_list, const object_complexity_list_t& object_complexity_list,
        const std::map<LLUUID, U32>& item_complexity, const std::map<LLUUID, U32>& temp_item_complexity, U32 body_parts_complexity);
    // </FS:Ansariel>

    // Todo: probably safe to store by local instead of global id
    // since they should be unique to this avatar, but local id might be not known.
    typedef std::map<LLUUID, ComplexityComponent> complexity_cache_map_t;
    complexity_cache_map_t mComplexityCache; // Cache per-attachment complexity
    ComplexityComponent mBodyPartsComplexity; // Cache for body parts (mesh, eyes, hair, etc)
    ComplexityComponent mControlAvatarComplexity; // Cache for animated object control avatar

    // the isTooComplex method uses these mutable values to avoid recalculating too frequently
    // DEPRECATED -- obsolete avatar render cost values
    mutable U32  mVisualComplexity;
    mutable bool mVisualComplexityStale;
    U32          mReportedVisualComplexity; // from other viewers through the simulator

    //--------------------------------------------------------------------
    // animated object status
    //--------------------------------------------------------------------
public:
    bool mIsControlAvatar;
    bool mIsUIAvatar;
    bool mEnableDefaultMotions;

    //--------------------------------------------------------------------
    // Morph masks
    //--------------------------------------------------------------------
public:
    /*virtual*/ void    applyMorphMask(const U8* tex_data, S32 width, S32 height, S32 num_components, LLAvatarAppearanceDefines::EBakedTextureIndex index = LLAvatarAppearanceDefines::BAKED_NUM_INDICES);
    bool        morphMaskNeedsUpdate(LLAvatarAppearanceDefines::EBakedTextureIndex index = LLAvatarAppearanceDefines::BAKED_NUM_INDICES);


    //--------------------------------------------------------------------
    // Global colors
    //--------------------------------------------------------------------
public:
    // <FS:Ansariel> [Legacy Bake]
    ///*virtual*/void onGlobalColorChanged(const LLTexGlobalColor* global_color);
    /*virtual*/void onGlobalColorChanged(const LLTexGlobalColor* global_color, bool upload_bake);

    //--------------------------------------------------------------------
    // Visibility
    //--------------------------------------------------------------------
protected:
    void        updateVisibility();
private:
    F32         mVisibilityPreference;
    U32         mVisibilityRank;
    bool        mVisible;

    //--------------------------------------------------------------------
    // Shadowing
    //--------------------------------------------------------------------
public:
    LLDrawable* mShadow;
private:
    LLFace*     mShadow0Facep;
    LLFace*     mShadow1Facep;
    LLPointer<LLViewerTexture> mShadowImagep;

    //--------------------------------------------------------------------
    // Impostors
    //--------------------------------------------------------------------
public:
    virtual bool isImpostor();
    bool        shouldImpostor(const F32 rank_factor = 1.0);
    bool        needsImpostorUpdate() const;
    const LLVector3& getImpostorOffset() const;
    const LLVector2& getImpostorDim() const;
    void        getImpostorValues(LLVector4a* extents, LLVector3& angle, F32& distance) const;
    void        cacheImpostorValues();
    void        setImpostorDim(const LLVector2& dim);
    static void resetImpostors();
    static void updateImpostors();
    LLRenderTarget mImpostor;
// [RLVa:KB] - Checked: RLVa-2.4 (@setcam_avdist)
    mutable bool mNeedsImpostorUpdate;
// [/RLVa:KB]
//  bool        mNeedsImpostorUpdate;
    S32         mLastImpostorUpdateReason;
    bool        mIsAnimesh; // <FS:minerjr> FIRE-35735: Imposter/Impostor Avatar Exclusions (Flag to track if avatar or attachments have Animated Mesh flagged)
    F32SecondsImplicit mLastImpostorUpdateFrameTime;
    const LLVector3*  getLastAnimExtents() const { return mLastAnimExtents; }
    void        setNeedsExtentUpdate(bool val) { mNeedsExtentUpdate = val; }

private:
    LLVector3   mImpostorOffset;
    LLVector2   mImpostorDim;
    // This becomes true in the constructor and false after the first
    // idleUpdateMisc(). Not clear it serves any purpose.
    bool        mNeedsAnimUpdate;
    bool        mNeedsExtentUpdate;
    LLVector3   mImpostorAngle;
    F32         mImpostorDistance;
    F32         mImpostorPixelArea;
    LLVector3   mLastAnimExtents[2];
    // <FS> the avatar's world box and attachment count at the last frame its rigged attachments were updated, and for how many frames in a row
    // they have been the same (see RKSkipStaticRiggedAttachmentUpdates in idleUpdateMisc)
    LLVector3   mAttBBoxLast[2];
    U32         mAttBBoxStable = 0;
    U32         mAttCountLast = 0;
    LLVector3   mLastAnimBasePos;

    LLCachedControl<bool> mRenderUnloadedAvatar;

    //--------------------------------------------------------------------
    // Wind rippling in clothes
    //--------------------------------------------------------------------
public:
    LLVector4   mWindVec;
    F32         mRipplePhase;
    bool        mBelowWater;
private:
    F32         mWindFreq;
    LLFrameTimer mRippleTimer;
    F32         mRippleTimeLast;
    LLVector3   mRippleAccel;
    LLVector3   mLastVel;

    //--------------------------------------------------------------------
    // Culling
    //--------------------------------------------------------------------
public:
    static void cullAvatarsByPixelArea();
    bool        isCulled() const { return mCulled; }
    bool        isMatricesOnlyDeferred() const { return mDeferredMatricesOnly; }     // <FS> RK_VK_AVCLASS census: a hidden avatar whose batch entry only works out the skeleton matrices
private:
    bool        mCulled;

    //--------------------------------------------------------------------
    // Constants
    //--------------------------------------------------------------------
public:
    virtual LLViewerTexture::EBoostLevel    getAvatarBoostLevel() const { return RKBaseTexture::BOOST_AVATAR; }
    virtual LLViewerTexture::EBoostLevel    getAvatarBakedBoostLevel() const { return RKBaseTexture::BOOST_AVATAR_BAKED; }
    virtual S32                         getTexImageSize() const;
    /*virtual*/ S32                     getTexImageArea() const { return getTexImageSize()*getTexImageSize(); }

/**                    Rendering
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    TEXTURES
 **/

    //--------------------------------------------------------------------
    // Loading status
    //--------------------------------------------------------------------
public:
    virtual bool    isTextureDefined(LLAvatarAppearanceDefines::ETextureIndex type, U32 index = 0) const;
    virtual bool    isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, U32 index = 0) const;
    virtual bool    isTextureVisible(LLAvatarAppearanceDefines::ETextureIndex type, LLViewerWearable *wearable) const;

    bool            isFullyBaked();
    static bool     areAllNearbyInstancesBaked(S32& grey_avatars);
    static void     getNearbyRezzedStats(std::vector<S32>& counts, F32& avg_cloud_time, S32& cloud_avatars, S32& pending_meshes, S32& control_avatars);
    static std::string rezStatusToString(S32 status);

    //--------------------------------------------------------------------
    // Baked textures
    //--------------------------------------------------------------------
public:
    /*virtual*/ LLTexLayerSet*  createTexLayerSet(); // Return LLViewerTexLayerSet
    void            releaseComponentTextures(); // ! BACKWARDS COMPATIBILITY !

protected:
    static void     onBakedTextureMasksLoaded(bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata);
    static void     onInitialBakedTextureLoaded(bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata);
    static void     onBakedTextureLoaded(bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* aux_src, S32 discard_level, bool final, void* userdata);
    virtual void    removeMissingBakedTextures();
    void            useBakedTexture(const LLUUID& id);
    LLViewerTexLayerSet*  getTexLayerSet(const U32 index) const { return dynamic_cast<LLViewerTexLayerSet*>(mBakedTextureDatas[index].mTexLayerSet);    }


    LLLoadedCallbackEntry::source_callback_list_t mCallbackTextureList ;
    bool mLoadedCallbacksPaused;
    S32 mLoadedCallbackTextures; // count of 'loaded' baked textures, filled from mCallbackTextureList
    LLFrameTimer mLastTexCallbackAddedTime;
    std::set<LLUUID>    mTextureIDs;
    //--------------------------------------------------------------------
    // Local Textures
    //--------------------------------------------------------------------
protected:
    virtual void    setLocalTexture(LLAvatarAppearanceDefines::ETextureIndex type, LLViewerTexture* tex, bool baked_version_exits, U32 index = 0);
    virtual void    addLocalTextureStats(LLAvatarAppearanceDefines::ETextureIndex type, LLViewerFetchedTexture* imagep, F32 texel_area_ratio, bool rendered, bool covered_by_baked);
    // MULTI-WEARABLE: make self-only?
    virtual void    setBakedReady(LLAvatarAppearanceDefines::ETextureIndex type, bool baked_version_exists, U32 index = 0);

    //--------------------------------------------------------------------
    // Texture accessors
    //--------------------------------------------------------------------
private:
    virtual void                setImage(const U8 te, LLViewerTexture *imagep, const U32 index);
    virtual LLViewerTexture*    getImage(const U8 te, const U32 index) const;
    const std::string           getImageURL(const U8 te, const LLUUID &uuid);

    virtual const LLTextureEntry* getTexEntry(const U8 te_num) const;
    virtual void setTexEntry(const U8 index, const LLTextureEntry &te);

    void checkTextureLoading() ;
    //--------------------------------------------------------------------
    // Layers
    //--------------------------------------------------------------------
protected:
    void            deleteLayerSetCaches(bool clearAll = true);
    void            addBakedTextureStats(LLViewerFetchedTexture* imagep, F32 pixel_area, F32 texel_area_ratio, S32 boost_level);

    //--------------------------------------------------------------------
    // Composites
    //--------------------------------------------------------------------
public:
    // <FS:Ansariel> [Legacy Bake]
    //virtual void  invalidateComposite(LLTexLayerSet* layerset);
    virtual void    invalidateComposite(LLTexLayerSet* layerset, bool upload_result);
    virtual void    invalidateAll();
    virtual void    setCompositeUpdatesEnabled(bool b) {}
    virtual void    setCompositeUpdatesEnabled(U32 index, bool b) {}
    virtual bool    isCompositeUpdateEnabled(U32 index) { return false; }

    //--------------------------------------------------------------------
    // Static texture/mesh/baked dictionary
    //--------------------------------------------------------------------
public:
    static bool     isIndexLocalTexture(LLAvatarAppearanceDefines::ETextureIndex i);
    static bool     isIndexBakedTexture(LLAvatarAppearanceDefines::ETextureIndex i);

    //--------------------------------------------------------------------
    // Messaging
    //--------------------------------------------------------------------
public:
    void            onFirstTEMessageReceived();
private:
    bool            mFirstTEMessageReceived;
    bool            mFirstAppearanceMessageReceived;

/**                    Textures
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    MESHES
 **/

public:
    void            debugColorizeSubMeshes(U32 i, const LLColor4& color);
    virtual void    updateMeshTextures();
    // <FS:Ansariel> [Legacy Bake]
    //void          updateSexDependentLayerSets();
    void            updateSexDependentLayerSets(bool upload_bake);
    virtual void    dirtyMesh(); // Dirty the avatar mesh
    void            updateMeshData();
    void            updateMeshVisibility();
    LLViewerTexture*        getBakedTexture(const U8 te);

    // Matrix palette cache entry
    class alignas(16) MatrixPaletteCache
    {
    public:
        // Last frame this entry was updated
        U32 mFrame;

        // List of Matrix4a's for this entry
        LLMeshSkinInfo::matrix_list_t mMatrixPalette;

        // Float array ready to be sent to GL
        std::vector<F32> mGLMp;

        // <FS> motion vectors: the same palette as of the frame before (equal to mGLMp when there is no history: first time, not asked for in the last frame, joint count changed).
        // Same layout as mGLMp. mHasPrev says whether it is real.
        std::vector<F32> mPrevGLMp;
        // <FS> avatar relative skinning (see rk_rig_anchor): mGLMp and mPrevGLMp hold the palette the pull shaders read: 12 floats a joint and one more entry, the tail, whose first row is (anchor x, y, z, 1)
        // (all zeros: the palette is plain world matrices). While the palette is relative (mRebased) the plain world version, which the uniform palette programs need, is in mGLMpAbs / mPrevGLMpAbs (no tail).
        // mAnchor / mPrevAnchor: what mGLMp / mPrevGLMp are relative to; the previous palette is always moved to the current anchor, so one anchor serves both in the velocity pass.
        std::vector<F32> mGLMpAbs, mPrevGLMpAbs;
        F32 mAnchor[3] = { 0.f, 0.f, 0.f };
        F32 mPrevAnchor[3] = { 0.f, 0.f, 0.f };
        bool mRebased = false, mPrevRebased = false;
        const std::vector<F32>& absPalette() const { return mRebased ? mGLMpAbs : mGLMp; }
        const std::vector<F32>& absPrevPalette() const { return mRebased ? mPrevGLMpAbs : mPrevGLMp; }
        bool mHasPrev = false;
        bool mComputed = false;     // mFrame is only meaningful after the first computation (the constructor sets it to "last frame")
        bool mBadBindNamed = false; // <FS> the wearer of a mesh whose inverse bind matrices the parse refused was logged once (MeshSkin)

        // <FS> stage B3a: the skin this entry was built from (kept so the avatar's job can rebuild it without a request), and the frame the avatar's job built it for (0xFFFFFFFF: not by a job)
        RKSkinRef mSkin;                        // (a handle, not a counted pointer: the refcount of a skin info is not atomic; a gone skin info reads as null)
        // <FS> stage B3b: where this palette sits in the frame's shared palette list (rk_frame_palette): frame << 32 | float offset, ~0 none. A request that finds its frame needs no lock.
        mutable std::atomic<U64> mListSlot{ ~0ull };
        U32 mJobFor = 0xFFFFFFFFu;
        bool mJobChecked = false;               // (RK_VK_PALJOB=2) the job's palette was compared with a rebuild at the first request

        // <FS> P1 step 3: the palette's id (0 = none yet): what a record names the palette by instead of a float offset in a frame's list; made at the first write into a list, given back with the entry
        mutable std::atomic<U32> mPaletteId{ 0 };

        MatrixPaletteCache() :
            mFrame(gFrameCount - 1)
        {
        }
        ~MatrixPaletteCache();
    };

    // Accessor for Matrix Palette Cache
    // Will do a map lookup for the entry associated with the given MeshSkinInfo
    // Will update said entry if it hasn't been updated yet this frame
    const MatrixPaletteCache& updateSkinInfoMatrixPalette(const LLMeshSkinInfo* skinInfo);

    // Map of LLMeshSkinInfo::mHash to MatrixPaletteCache
    typedef std::unordered_map<U64, MatrixPaletteCache> matrix_palette_cache_t;
    matrix_palette_cache_t mMatrixPaletteCache;
    mutable std::shared_mutex mPaletteMutex;    // <FS> stage B3a/b: this avatar's palette entries: a request that hits reads under a shared lock; a build (the job, or the lazy path after the global palette lock) writes under an exclusive one
    void buildPaletteEntry(MatrixPaletteCache& entry, const LLMeshSkinInfo* skin, U32 stamp, bool had_last_frame, bool swap_prev);

protected:
    void            releaseMeshData();
    virtual void restoreMeshData();
private:
    virtual void    dirtyMesh(S32 priority); // Dirty the avatar mesh, with priority
    LLViewerJoint*  getViewerJoint(S32 idx);
    S32             mDirtyMesh; // 0 -- not dirty, 1 -- morphed, 2 -- LOD
    bool            mMeshTexturesDirty;

    //--------------------------------------------------------------------
    // Destroy invisible mesh
    //--------------------------------------------------------------------
protected:
    bool            mMeshValid;
    LLFrameTimer    mMeshInvisibleTime;

/**                    Meshes
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    APPEARANCE
 **/

    LLPointer<LLAppearanceMessageContents>  mLastProcessedAppearance;

public:
    void            parseAppearanceMessage(LLMessageSystem* mesgsys, LLAppearanceMessageContents& msg);
    void            processAvatarAppearance(LLMessageSystem* mesgsys);
    void            applyParsedAppearanceMessage(LLAppearanceMessageContents& contents, bool slam_params);
    void            hideHair();
    void            hideSkirt();
    void            startAppearanceAnimation();
    // <FS:Ansariel> [Legacy Bake]
    /*virtual*/ void bodySizeChanged();

    //--------------------------------------------------------------------
    // Appearance morphing
    //--------------------------------------------------------------------
public:
    bool            getIsAppearanceAnimating() const { return mAppearanceAnimating; }

    // True if we are computing our appearance via local compositing
    // instead of baked textures, as for example during wearable
    // editing or when waiting for a subsequent server rebake.
    /*virtual*/ bool    isUsingLocalAppearance() const { return mUseLocalAppearance; }

    // <FS:Ansariel> [Legacy Bake]
    // True if this avatar should fetch its baked textures via the new
    // appearance mechanism.
    bool                isUsingServerBakes() const;
    void                setIsUsingServerBakes(bool newval);
    // </FS:Ansariel> [Legacy Bake]

    // True if we are currently in appearance editing mode. Often but
    // not always the same as isUsingLocalAppearance().
    /*virtual*/ bool    isEditingAppearance() const { return mIsEditingAppearance; }
    void setIsEditingAppearance(bool editing) { mIsEditingAppearance = editing; }   // <FS:CR> for Built-in Posestand

    // FIXME review isUsingLocalAppearance uses, some should be isEditing instead.

private:
    bool            mAppearanceAnimating;
    LLFrameTimer    mAppearanceMorphTimer;
    F32             mLastAppearanceBlendTime;
    bool            mIsEditingAppearance; // flag for if we're actively in appearance editing mode
    bool            mUseLocalAppearance; // flag for if we're using a local composite
    // <FS:Ansariel> [Legacy Bake]
    bool            mUseServerBakes; // flag for if baked textures should be fetched from baking service (false if they're temporary uploads)

    //--------------------------------------------------------------------
    // Visibility
    //--------------------------------------------------------------------
public:
    bool            isVisible() const;
    // stage 9 (avatars): the owners' pass (ownersCollect) listed this avatar in the frame it ran or the one before: what the avatar's region group's visible stamp was for isVisible and updateVisibility. The frame counter of
    // LLFrameTimer (once a frame) is used, not the drawable's visible stamp, which counts twice a frame (the idle update and the display) and is one behind at the cull
    U32             mOwnerListedFrame = 0;
    bool            ownerListedRecently() const;
    virtual bool    shouldRenderRigged() const;
    void            setVisibilityRank(U32 rank);
    U32             getVisibilityRank() const { return mVisibilityRank; }
    static S32      sNumVisibleAvatars; // Number of instances of this class
/**                    Appearance
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    WEARABLES
 **/

    //--------------------------------------------------------------------
    // Attachments
    //--------------------------------------------------------------------
public:
    void                clampAttachmentPositions();
    virtual const LLViewerJointAttachment* attachObject(LLViewerObject *viewer_object);
    virtual bool        detachObject(LLViewerObject *viewer_object);
    static bool         getRiggedMeshID( LLViewerObject* pVO, LLUUID& mesh_id );
    void                cleanupAttachedMesh( LLViewerObject* pVO );
    bool                hasPendingAttachedMeshes();
    static LLVOAvatar*  findAvatarFromAttachment(LLViewerObject* obj);
    /*virtual*/ bool    isWearingWearableType(LLWearableType::EType type ) const;
    LLViewerObject *    findAttachmentByID( const LLUUID & target_id ) const;
    LLViewerJointAttachment* getTargetAttachmentPoint(LLViewerObject* viewer_object);

//-TT Patch: ReplaceWornItemsOnly
//-TT
protected:
    void                lazyAttach();
    void                rebuildRiggedAttachments( void );

    //--------------------------------------------------------------------
    // Map of attachment points, by ID
    //--------------------------------------------------------------------
public:
    S32                 getAttachmentCount() const; // Warning: order(N) not order(1)
    typedef std::map<S32, LLViewerJointAttachment*> attachment_map_t;
    attachment_map_t                                mAttachmentPoints;
    std::vector<LLPointer<LLViewerObject> >         mPendingAttachment;

    // List of attachments' ids with attach points from simulator.
    // we need this info to know when all attachments are present.
    std::map<LLUUID, S32>                           mSimAttachments;
    S32                                             mLastCloudAttachmentCount;
    LLFrameTimer                                    mLastCloudAttachmentChangeTime;

    //--------------------------------------------------------------------
    // HUD functions
    //--------------------------------------------------------------------
public:
    bool                hasHUDAttachment() const;
    LLBBox              getHUDBBox() const;
    void                resetHUDAttachments();
    void                resetChangedHUDAttachments();      // <FS> stage C5f: only the HUD attachments that are changing or flagged for an undamped move (see LLVOAvatarSelf::updateCharacter)
    S32                 getMaxAttachments() const;
    bool                canAttachMoreObjects(U32 n=1) const;
    S32                 getMaxAnimatedObjectAttachments() const;
    bool                canAttachMoreAnimatedObjects(U32 n=1) const;
protected:
    U32                 getNumAttachments() const; // O(N), not O(1)
    U32                 getNumAnimatedObjectAttachments() const; // O(N), not O(1)

/**                    Wearables
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    ACTIONS
 **/

    //--------------------------------------------------------------------
    // Animations
    //--------------------------------------------------------------------
public:
    bool            isAnyAnimationSignaled(const LLUUID *anim_array, const S32 num_anims) const;
    void            processAnimationStateChanges();
protected:
    bool            processSingleAnimationStateChange(const LLUUID &anim_id, bool start);
    void            resetAnimations();
private:
    LLTimer         mAnimTimer;
    F32             mTimeLast;

    //--------------------------------------------------------------------
    // Animation state data
    //--------------------------------------------------------------------
public:
    typedef std::map<LLUUID, S32>::iterator AnimIterator;
    std::map<LLUUID, S32>                   mSignaledAnimations; // requested state of Animation name/value
    std::map<LLUUID, S32>                   mPlayingAnimations; // current state of Animation name/value

    typedef std::multimap<LLUUID, LLUUID>   AnimationSourceMap;
    typedef AnimationSourceMap::iterator    AnimSourceIterator;
    AnimationSourceMap                      mAnimationSources; // object ids that triggered anim ids

    //--------------------------------------------------------------------
    // Chat
    //--------------------------------------------------------------------
public:
    void            addChat(const LLChat& chat);
    void            clearChat();
    void            startTyping() { mTyping = true; mTypingTimer.reset(); mNameTagForce = true; }
    void            stopTyping() { mTyping = false; mNameTagForce = true; }
    // <FS:Ansariel> Get typing status
    bool            isTyping() const { return mTyping; }
private:
    bool            mVisibleChat;
    bool            mVisibleTyping;

    //--------------------------------------------------------------------
    // Lip synch morphs
    //--------------------------------------------------------------------
private:
    bool            mLipSyncActive; // we're morphing for lip sync
    LLVisualParam*  mOohMorph; // cached pointers morphs for lip sync
    LLVisualParam*  mAahMorph; // cached pointers morphs for lip sync

    //--------------------------------------------------------------------
    // Flight
    //--------------------------------------------------------------------
public:
    bool            mInAir;
    LLFrameTimer    mTimeInAir;

/**                    Actions
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    PHYSICS
 **/

private:
    F32         mSpeedAccum; // measures speed (for diagnostics mostly).
    bool        mTurning; // controls hysteresis on avatar rotation
    F32         mSpeed; // misc. animation repeated state

    //--------------------------------------------------------------------
    // Dimensions
    //--------------------------------------------------------------------
public:
    void        resolveHeightGlobal(const LLVector3d &inPos, LLVector3d &outPos, LLVector3 &outNorm);
    bool        distanceToGround( const LLVector3d &startPoint, LLVector3d &collisionPoint, F32 distToIntersectionAlongRay );
    void        resolveHeightAgent(const LLVector3 &inPos, LLVector3 &outPos, LLVector3 &outNorm);
    void        resolveRayCollisionAgent(const LLVector3d start_pt, const LLVector3d end_pt, LLVector3d &out_pos, LLVector3 &out_norm);
    void        slamPosition(); // Slam position to transmitted position (for teleport);
protected:

    //--------------------------------------------------------------------
    // Material being stepped on
    //--------------------------------------------------------------------
private:
    bool        mStepOnLand;
    U8          mStepMaterial;
    LLVector3   mStepObjectVelocity;

/**                    Physics
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    HIERARCHY
 **/

public:
    /*virtual*/ bool    setParent(LLViewerObject* parent);
    /*virtual*/ void    addChild(LLViewerObject *childp);
    /*virtual*/ void    removeChild(LLViewerObject *childp);

    //--------------------------------------------------------------------
    // Sitting
    //--------------------------------------------------------------------
public:
    void            sitDown(bool bSitting);
    bool            isSitting(){return mIsSitting;}
    static void     skinSharingCensus(const std::vector<LLPointer<LLVOAvatar> >& batch);        // <FS> PLAN-avatars-100.md D4 step 0: how many avatars wear the same rigged mesh (RK_VK_DIAG, one line every 600 batches)
    // <FS> true while this avatar's deferred motion update (the batched pose blend, completeDeferredMotionUpdate) has not made its post motion step yet, and that step is going to add the ground sit hover
    // offset to the root (updateCharacterPostMotion): until then the skeleton is one hover offset below where the frame ends
    bool            hasPendingGroundSitHover() const { return mMotionUpdateDeferred && !mHoverDone && !getParent() && (mIsSitting || mDeferredSitGroundConstrained); }
    // <FS> the avatar's pose job (blend + world pass) has finished for this frame: its joints hold this frame's pose WITHOUT the ground-sit hover (the post motion step adds it; with RK_VK_HOVERJOB the job adds it itself and hasPendingGroundSitHover is then false). Before that they still hold last frame's final
    // value, which has the hover in. An animated object worn by this avatar adds the hover only in the first case (LLControlAvatar::matchVolumeTransform).
    bool            poseJobDoneThisFrame() const { return mPoseJobFrame.load(std::memory_order_acquire) == (U32)LLFrameTimer::getFrameCount(); }
    std::atomic<U32> mPoseJobFrame{ ~0u };
    void            sitOnObject(LLViewerObject *sit_object);
    void            getOffObject();
    void            revokePermissionsOnObject(LLViewerObject *sit_object);
private:
    // set this property only with LLVOAvatar::sitDown method
    bool            mIsSitting;
    // position backup in case of missing data
    LLVector3       mLastRootPos;

/**                    Hierarchy
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    NAME
 **/

public:
    virtual std::string getFullname() const; // Returns "FirstName LastName"
    // <FS> PLAN-avatars.md stage B3d, name tag events (RK_VK_NAMETAGPOLL): something the tag shows may have changed (animations, names and title, complexity, rez state, appearance, typing, chat, marks): look at its
    // inputs at the next chance. bumpNameTagGen(): the same for every avatar (the mute list, the friend list).
    void            invalidateNameTagInputs() { mNameTagForce = true; }
    void            updateWindEffectForDraw();          // B3d: the cloth wind state, worked out when the avatar is drawn
    static void     bumpNameTagGen();
    std::string     avString() const; // Frequently used string in log messages "Avatar '<full name'"
protected:
    static void     getAnimLabels(std::vector<std::string>* labels);
    static void     getAnimNames(std::vector<std::string>* names);
private:
    bool            mNameIsSet;
    F64             mNameTagLastPoll = -1.0;        // <FS> stage B3d: when the tag's inputs were last looked at (RK_VK_NAMETAGPOLL); mNameTagForce: look next time
    bool            mNameTagForce = false;
    U32             mNameTagGen = 0;                // (the global generation this avatar's tag was last looked at under)
    LLSD            mClientTagData;
    bool            mHasClientTagColor;
    std::string     mTitle;
    // <FS:Ansariel> FIRE-13414: Avatar name isn't updated when the simulator sends a new name
    std::string     mNameFirstname;
    std::string     mNameLastname;
    // </FS:Ansariel>
    bool            mNameAway;
    bool            mNameDoNotDisturb;
    bool            mNameAutoResponse; // <FS:Ansariel> Show auto-response in nametag
    bool            mNameIsTyping; // <FS:Ansariel> FIRE-3475: Show typing in nametag
    bool            mNameMute;
    bool            mNameAppearance;
    bool            mNameFriend;
    bool            mNameCloud;
    F32             mNameAlpha;
    LLColor4        mNameColor;
    S32             mRenderGroupTitles;
    std::string     mDistanceString;
    // <FS:Ansariel> Show Arc in nametag (for Jelly Dolls)
    U32             mNameArc;
    LLColor4        mNameArcColor;
    // </FS:Ansariel>

    //--------------------------------------------------------------------
    // Display the name (then optionally fade it out)
    //--------------------------------------------------------------------
public:
    LLFrameTimer    mChatTimer;
    LLPointer<LLHUDNameTag> mNameText;
private:
    LLFrameTimer    mTimeVisible;
    std::deque<LLChat> mChats;
    bool            mTyping;
    LLFrameTimer    mTypingTimer;

/**                    Name
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    SOUNDS
 **/

    //--------------------------------------------------------------------
    // Voice visualizer
    //--------------------------------------------------------------------
public:
    // Responsible for detecting the user's voice signal (and when the
    // user speaks, it puts a voice symbol over the avatar's head) and gesticulations
    LLPointer<LLVoiceVisualizer>  mVoiceVisualizer;
    int                 mCurrentGesticulationLevel;

    //--------------------------------------------------------------------
    // Step sound
    //--------------------------------------------------------------------
protected:
    const LLUUID&       getStepSound() const;
private:
    // Global table of sound ids per material, and the ground
    const static LLUUID sStepSounds[LL_MCODE_END];
    // const static LLUUID  sStepSoundOnLand; - <FS:PP> Commented out for FIRE-3169: Option to change the default footsteps sound

    //--------------------------------------------------------------------
    // Foot step state (for generating sounds)
    //--------------------------------------------------------------------
public:
    void                setFootPlane(const LLVector4 &plane) { mFootPlane = plane; }
    LLVector4           mFootPlane;
private:
    bool                mWasOnGroundLeft;
    bool                mWasOnGroundRight;

/**                    Sounds
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    DIAGNOSTICS
 **/

    //--------------------------------------------------------------------
    // General
    //--------------------------------------------------------------------
public:
    void                getSortedJointNames(S32 joint_type, std::vector<std::string>& result) const;
    void                dumpArchetypeXML(const std::string& prefix, bool group_by_wearables = false);
    void                dumpArchetypeXMLCallback(const std::vector<std::string>& filenames, bool group_by_wearables); // <FS:Ansariel> Threaded filepickers
    void                dumpAppearanceMsgParams( const std::string& dump_prefix,
                                                 const LLAppearanceMessageContents& contents);
    static void         dumpBakedStatus();
    const std::string   getBakedStatusForPrintout() const;
    void                dumpAvatarTEs(const std::string& context) const;

    static F32          sUnbakedTime; // Total seconds with >=1 unbaked avatars
    static F32          sUnbakedUpdateTime; // Last time stats were updated (to prevent multiple updates per frame)
    static F32          sGreyTime; // Total seconds with >=1 grey avatars
    static F32          sGreyUpdateTime; // Last time stats were updated (to prevent multiple updates per frame)
protected:
    S32                 getUnbakedPixelAreaRank();
    bool                mHasGrey;
private:
    F32                 mMinPixelArea;
    F32                 mMaxPixelArea;
    F32                 mAdjustedPixelArea;
    std::string         mDebugText;
    std::string         mBakedTextureDebugText;


    //--------------------------------------------------------------------
    // Avatar Rez Metrics
    //--------------------------------------------------------------------
public:
    void            debugAvatarRezTime(std::string notification_name, std::string comment = "");
    F32             debugGetExistenceTimeElapsedF32() const { return mDebugExistenceTimer.getElapsedTimeF32(); }

protected:
    LLFrameTimer    mRuthDebugTimer; // For tracking how long it takes for av to rez
    LLFrameTimer    mDebugExistenceTimer; // Debugging for how long the avatar has been in memory.
    LLFrameTimer    mLastAppearanceMessageTimer; // Time since last appearance message received.

    //--------------------------------------------------------------------
    // COF monitoring
    //--------------------------------------------------------------------

public:
    // COF version of last viewer-initiated appearance update request. For non-self avs, this will remain at default.
    S32 mLastUpdateRequestCOFVersion;

    // COF version of last appearance message received for this av.
    S32 mLastUpdateReceivedCOFVersion;

/**                    Diagnostics
 **                                                                            **
 *******************************************************************************/

/********************************************************************************
 **                                                                            **
 **                    SUPPORT CLASSES
 **/

protected: // Shared with LLVOAvatarSelf


/**                    Support classes
 **                                                                            **
 *******************************************************************************/

}; // LLVOAvatar
extern const F32 SELF_ADDITIONAL_PRI;
extern const S32 MAX_TEXTURE_VIRTUAL_SIZE_RESET_INTERVAL;

extern const F32 MAX_HOVER_Z;
extern const F32 MIN_HOVER_Z;

std::string get_sequential_numbered_file_name(const std::string& prefix,
                                              const std::string& suffix);
void dump_sequential_xml(const std::string outprefix, const LLSD& content);
void dump_visual_param(apr_file_t* file, LLVisualParam* viewer_param, F32 value);

#endif // LL_VOAVATAR_H

