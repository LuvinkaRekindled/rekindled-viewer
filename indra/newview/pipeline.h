/**
 * @file pipeline.h
 * @brief Rendering pipeline definitions
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

#ifndef LL_PIPELINE_H
#define LL_PIPELINE_H

#include "llasyncpool.h"
#include "llcamera.h"
#include "llerror.h"
#include "lldrawpool.h"
#include "llspatialpartition.h"
#include "m4math.h"
#include "llpointer.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolmaterials.h"
#include "lldevicecaps.h"
#include "lldrawable.h"
#include "llrendertarget.h"
#include "llreflectionmapmanager.h"
#include "llheroprobemanager.h"

#include <atomic>
#include <map>
#include <memory>
#include <stack>

namespace LLVkBridge { struct SpotVolumeRow; }

class LLViewerTexture;
class LLFace;
class LLViewerObject;
class LLTextureEntry;
class LLCullResult;
class LLVOAvatar;
class LLVOVolume;
class LLVOPartGroup;
class LLShaderProgram;
class LLDrawPoolAlpha;
class LLSettingsSky;

typedef enum e_avatar_skinning_method
{
    SKIN_METHOD_SOFTWARE,
    SKIN_METHOD_VERTEX_PROGRAM
} EAvatarSkinningMethod;

bool compute_min_max(LLMatrix4& box, LLVector2& min, LLVector2& max); // Shouldn't be defined here!
bool LLRayAABB(const LLVector3 &center, const LLVector3 &size, const LLVector3& origin, const LLVector3& dir, LLVector3 &coord, F32 epsilon = 0);
bool setup_hud_matrices(); // use whole screen to render hud
bool setup_hud_matrices(const LLRect& screen_region); // specify portion of screen (in pixels) to render hud attachments from (for picking)


extern LLTrace::BlockTimerStatHandle FTM_RENDER_GEOMETRY;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_GRASS;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_INVISIBLE;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_SHINY;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_SIMPLE;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_TERRAIN;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_TREES;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_UI;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_WATER;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_WL_SKY;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_ALPHA;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_CHARACTERS;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_BUMP;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_MATERIALS;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_FULLBRIGHT;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_GLOW;
extern LLTrace::BlockTimerStatHandle FTM_STATESORT;
extern LLTrace::BlockTimerStatHandle FTM_PIPELINE;
extern LLTrace::BlockTimerStatHandle FTM_CLIENT_COPY;

extern LLTrace::BlockTimerStatHandle FTM_RENDER_UI_HUD;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_UI_3D;
extern LLTrace::BlockTimerStatHandle FTM_RENDER_UI_2D;

class LLPipeline
{
public:
    LLPipeline();
    ~LLPipeline();

    void destroyResources();
    void restoreResources();
    void requestResizeScreenTexture(); // set flag only, no work, safer for callbacks...
    void requestResizeShadowTexture(); // set flag only, no work, safer for callbacks...

    void resizeScreenTexture();
    void resizeShadowTexture();

    void releaseRenderBuffers();
    void releaseLUTBuffers();
    void releaseScreenBuffers();
    void releaseShadowBuffers();

    void createRenderBuffers();
    void createLUTBuffers();
    void generateBrdfLut(bool allocate);

    //allocate the largest screen buffer possible up to resX, resY
    //returns true if full size buffer allocated, false if some other size is allocated
    bool allocateScreenBuffer(U32 resX, U32 resY);

    typedef enum {
        FBO_SUCCESS_FULLRES = 0,
        FBO_SUCCESS_LOWRES,
        FBO_FAILURE
    } eFBOStatus;

private:
    //implementation of above, wrapped for easy error handling
    eFBOStatus doAllocateScreenBuffer(U32 resX, U32 resY);
public:

    //attempt to allocate screen buffers at resX, resY
    //returns true if allocation successful, false otherwise
    bool allocateScreenBufferInternal(U32 resX, U32 resY);
    bool allocateShadowBuffer(U32 resX, U32 resY);

    // rebuild all LLVOVolume render batches
    void rebuildDrawInfo();
    // Rebuild all terrain
    void rebuildTerrain();

    // Clear LLFace mVertexBuffer pointers
    void resetVertexBuffers(LLDrawable* drawable);

    // perform a profile of the given avatar
    // if profile_attachments is true, run a profile for each attachment
    void profileAvatar(LLVOAvatar* avatar, bool profile_attachments = false);

    // generate an impostor for the given avatar
    //  preview_avatar - if true, a preview window render is being performed
    //  for_profile - if true, a profile is being performed, do not update actual impostor
    //  specific_attachment - specific attachment to profile, or nullptr to profile entire avatar
    void generateImpostor(LLVOAvatar* avatar, bool preview_avatar = false, bool for_profile = false, LLViewerObject* specific_attachment = nullptr);

    void bindScreenToTexture();
    void renderFinalize();
    void copyScreenSpaceReflections(LLRenderTarget* src, LLRenderTarget* dst);
    void generateLuminance(LLRenderTarget* src, LLRenderTarget* dst);
    void generateExposure(LLRenderTarget* src, LLRenderTarget* dst, bool use_history = true);
    void tonemap(LLRenderTarget* src, LLRenderTarget* dst, bool gamma_correct);
    void gammaCorrect(LLRenderTarget* src, LLRenderTarget* dst);
    void generateGlow(LLRenderTarget* src);
    void applyCAS(LLRenderTarget* src, LLRenderTarget* dst);

    // DLSS proof of concept (FSVkDLSS): jitter the main scene's projection each frame, work out where every pixel was last frame, hand both to the upscaler
    void updateJitter();                                // call right before the main scene's setup3DRender
    void noteSceneMatrices();                           // call at the end of the main scene render
    void generateMotionVectors(LLRenderTarget* dst);    // camera motion from depth and the last two view-projection matrices, in render pixels
    // <FS> resolved_input: the source is ray reconstruction's one-to-one output (PLAN-dlss-nr.md section 8, "denoise first"): the super resolution call gets a jitter of zero
    bool applyUpscale(LLRenderTarget* src, LLRenderTarget* dst, bool ray_reconstruction = false, bool resolved_input = false);
    bool applyNeuralRender(LLRenderTarget* src, LLRenderTarget* dst);    // <FS> DLSS-NR (doc/vulkan/PLAN-dlss-nr.md): src (window size, finished) -> dst; false: nothing was done
    bool applyNeuralRenderRimLift(LLRenderTarget* nr, LLRenderTarget* src, LLRenderTarget* dst);
    // <FS> DLSS-NR: what the frame should do now: 0 nothing, 1 evaluate and show, 2 only while a snapshot renders, 3 evaluate but show the normal picture (the launcher's test flag)
    static S32 neuralRenderMode();
    S32 mNRReleaseFrames = 0;       // <FS> DLSS-NR: frames left to tell the worker to release the feature after the mode went off    // <FS> DLSS-NR: the rim lift (nrRimLiftF.glsl) of nr against its input src into dst; false: nothing was done    // upscales src (render size) into dst (window size) through the mirror; false: nothing was done
    static bool sDLSS;
    // <FS> DLSS ray reconstruction (FSVkRayReconstruction, needs DLSS): the denoiser / upscaler takes the noisy linear HDR picture (mRT->screen) before the tonemap, with guide buffers made from the
    // G-buffer, and gives back a window sized HDR picture; the tonemap, CAS, glow and the rest of the post chain then run at the window size on mRRPing / mRRPong.
    static bool sRR;
    static F32 farDistanceScale();      // the factor the far avatar rules (tiers, impostor distance, fringe pass) apply to a distance: 1 at the normal field of view, less when the camera zooms in (a zoomed in avatar is looked at closely whatever its distance)
    bool rayReconstructionActive();     // sRR and everything it needs exists (targets, program, device support, no zoomed snapshot)
    void generateRRGuides(LLRenderTarget* dst);
    void generateRRReflectionMotion(LLRenderTarget* dst);       // specular motion vectors from the hit distance: the reflected point projected with this frame's and last frame's camera
    LLRenderTarget          mRRReflMotion;  // render size RG16F
    bool                    mRRReflMotionValid = false;
    // <FS> the RT Reflection pass (rtReflectionF.glsl, doc/vulkan/PLAN-rt-reflection-pass.md): the traced glossy reflections of a view before its lighting pass. Output 0 RGBA16F: the reflection colour and how much of it
    // replaces the probe result (-1 traced and missed, 0 not traced); output 1 R16F: metres from a glossy pixel to what its ray hit (0: not traced), the specular hit distance guide of ray reconstruction.
    // mRtReflMain is the main view's (render size), the other views (cube faces, the planar mirror's view) have a target each, made at their size.
    LLRenderTarget          mRtReflMain;
    std::map<const LLRenderTarget*, std::unique_ptr<LLRenderTarget>> mRtReflSlots;
    LLRenderTarget*         rtReflectionTarget(const LLRenderTarget* screen);       // the pass's target for a view, made or remade at the view's size; null if it could not be made
    bool                    mRRHitDistValid = false;        // mRtReflMain's second output holds this frame's distances
    LLRenderTarget          mRRGuides;      // render size: 0 diffuse albedo, 1 specular albedo, 2 world normal + roughness (all RGBA16F), 3 the reflection motion (RG16F)
    LLRenderTarget          mRRHdr;         // window size RGBA16F: what ray reconstruction outputs
    LLRenderTarget          mRRDenoised;    // <FS> render size RGBA16F: what ray reconstruction outputs at a one-to-one ratio ("denoise first", PLAN-dlss-nr.md section 8); the frame then takes the plain path
    LLRenderTarget          mRRPing;        // window size RGBA16F: the post chain after the upscale
    LLRenderTarget          mRRPong;
    static F32 getDLSSScale();          // the render resolution as a fraction of the window: FSVkDLSSScale if set, else from FSVkDLSSQuality
    // <FS> avatar motion vectors (doc/vulkan/PLAN-ray-tracing.md, item 6): the velocity pass after the G-buffer. begin: before the main view's renderGeomDeferred (sets the body recording
    // when the pass is wanted); render: after the G-buffer is flushed, before the lighting. mAvatarVelocityMap: RGBA16F at render size, rg = total screen motion in pixels, a = 1 where an avatar wrote.
    void beginAvatarVelocity();
    void renderAvatarVelocity();
    bool avatarVelocityWanted();
    void visualizeMotion(LLRenderTarget* src, LLRenderTarget* dst, bool show_flag);
    void visualizeRtMaterials(LLRenderTarget* dst);
    LLRenderTarget          mAvatarVelocityMap;
    LLRenderTarget          mRtSunHard;                         // <FS> the centre ray's hard sun shadow alone, with a mip chain (R16F): the penumbra filter's taps read the level of their spacing
    LLRenderTarget          mRtShadowTemp;                      // <FS> PLAN-rt-shadow-penumbra.md step 2b: the penumbra filter's horizontal pass writes here, the vertical one reads it and writes the lightmap
    LLRenderTarget          mLightMapHalf2;                     // <FS> the second half resolution lightmap (the blur at half resolution ping-pongs between the two)
    LLRenderTarget          mLightMapHalf;                      // <FS> shaders from scratch (PLAN-shaders-rewrite.md): the sun lightmap at half resolution (native sun program, upsampled by the native upsample program), key lightmap_half
    LLRenderTarget          mRtBlocker;                         // <FS> PLAN-rt-shadow-penumbra.md step 1: the blocker distance of the sun lightmap's centre ray (R16F, copied from the lightmap's attachment 1 right after the sun pass)
    bool                    mAvatarVelocityActive = false;      // begin said yes for this frame
    bool                    mAvatarVelocityDrawn = false;       // the pass ran this frame: generateMotionVectors takes the target
    LLRenderTarget          mMotionMap;
    LLRenderTarget          mUpscaledMap;
    LLRenderTarget          mNROutput;      // <FS> DLSS-NR: window size RGBA16F, only with LL_VK_DLSS_NR >= 2 (the position after the upscale)
    LLRenderTarget          mNROutputRender;    // <FS> DLSS-NR: render size RGBA16F (the position before the upscale)
    LLRenderTarget          mNRLift;            // <FS> DLSS-NR: the rim lift's output, window size
    LLRenderTarget          mNRLiftRender;      // <FS> DLSS-NR: the rim lift's output, render size
    glm::mat4               mViewProjCur = glm::mat4(1.f);          // unjittered, this frame's
    glm::mat4               mViewProjPrev = glm::mat4(1.f);         // unjittered, last frame's
    bool                    mViewProjPrevValid = false;
    U32                     mJitterIndex = 0;
    U32                     mJitterFrame = ~0u;
    void applyFXAA(LLRenderTarget* src, LLRenderTarget* dst);
    void generateSMAABuffers(LLRenderTarget* src);
    void applySMAA(LLRenderTarget* src, LLRenderTarget* dst);
    void renderDoF(LLRenderTarget* src, LLRenderTarget* dst);
    void copyRenderTarget(LLRenderTarget* src, LLRenderTarget* dst);
    bool combineGlow(LLRenderTarget* src, LLRenderTarget* dst);       // false: nothing glows this frame, dst was not written (the caller keeps src)
    void visualizeBuffers(LLRenderTarget* src, LLRenderTarget* dst, U32 bufferIndex);

    void init();
    void cleanup();
    bool isInit() { return mInitialized; };

    /// @brief Get a draw pool from pool type (POOL_SIMPLE, POOL_MEDIA) and texture.
    /// @return Draw pool, or NULL if not found.
    LLDrawPool *findPool(const U32 pool_type, LLViewerTexture *tex0 = NULL);

    /// @brief Get a draw pool for faces of the appropriate type and texture.  Create if necessary.
    /// @return Always returns a draw pool.
    LLDrawPool *getPool(const U32 pool_type, LLViewerTexture *tex0 = NULL);

    /// @brief Figures out draw pool type from texture entry. Creates pool if necessary.
    static LLDrawPool* getPoolFromTE(const LLTextureEntry* te, LLViewerTexture* te_image);
    static U32 getPoolTypeFromTE(const LLTextureEntry* te, LLViewerTexture* imagep);

    void         addPool(LLDrawPool *poolp);    // Only to be used by LLDrawPool classes for splitting pools!
    void         removePool( LLDrawPool* poolp );

    void         allocDrawable(LLViewerObject *obj);

    void         unlinkDrawable(LLDrawable*);

    static void removeMutedAVsLights(LLVOAvatar*);

    // Object related methods
    void        markVisible(LLDrawable *drawablep, LLCamera& camera);
    void        markOccluder(LLSpatialGroup* group);

    void        doOcclusion(LLCamera& camera);
    void        markNotCulled(LLSpatialGroup* group, LLCamera &camera);
    // <FS> C4.5 R2: what markNotCulled did for a member of a retained set that the retained set does not already hold: the list it goes on and the occlusion test (a replay), and for a group that just joined the set its wake and its distance
    void        replayMember(LLSpatialGroup* group, U8 kind, bool occlude);
    void        replayAdopted(LLSpatialGroup* group, LLCamera& camera, bool world);
    void        replayMemberShadow(LLSpatialGroup* group, U8 kind);        // a sun cascade's cull: the lists (and the LOD list) markNotCulled fills, nothing else
    void        markMoved(LLDrawable *drawablep, bool damped_motion = false);
    void        markShift(LLDrawable *drawablep);
    void        markTextured(LLDrawable *drawablep);
    void        markRenderRebuild(LLRenderUpdate* glu);
    void        markRebuild(LLSpatialGroup* group);
    void        markRebuild(LLDrawable *drawablep, LLDrawable::EDrawableFlags flag = LLDrawable::REBUILD_ALL);
    void        markPartitionMove(LLDrawable* drawablep);
    void        markMeshDirty(LLSpatialGroup* group);

    //get the object between start and end that's closest to start.
    LLViewerObject* lineSegmentIntersectInWorld(const LLVector4a& start, const LLVector4a& end,
                                                bool pick_transparent,
                                                bool pick_rigged,
                                                bool pick_unselectable,
                                                bool pick_reflection_probe,
                                                S32* face_hit,                          // return the face hit
                                                S32* gltf_node_hit = nullptr,           // return the gltf node hit
                                                S32* gltf_primitive_hit = nullptr,      // return the gltf primitive hit
                                                LLVector4a* intersection = NULL,         // return the intersection point
                                                LLVector2* tex_coord = NULL,            // return the texture coordinates of the intersection point
                                                LLVector4a* normal = NULL,               // return the surface normal at the intersection point
                                                LLVector4a* tangent = NULL             // return the surface tangent at the intersection point
        );

    //get the closest particle to start between start and end, returns the LLVOPartGroup and particle index
    LLVOPartGroup* lineSegmentIntersectParticle(const LLVector4a& start, const LLVector4a& end, LLVector4a* intersection,
                                                        S32* face_hit);


    LLViewerObject* lineSegmentIntersectInHUD(const LLVector4a& start, const LLVector4a& end,
                                              bool pick_transparent,
                                              S32* face_hit,                          // return the face hit
                                              LLVector4a* intersection = NULL,         // return the intersection point
                                              LLVector2* tex_coord = NULL,            // return the texture coordinates of the intersection point
                                              LLVector4a* normal = NULL,               // return the surface normal at the intersection point
                                              LLVector4a* tangent = NULL             // return the surface tangent at the intersection point
        );

    // Something about these textures has changed.  Dirty them.

    void        resetDrawOrders();

    U32         addObject(LLViewerObject *obj);

    void        enableShadows(const bool enable_shadows);
    void        releaseSpotShadowTargets();
    void        releaseSunShadowTargets();
    void        releaseSunShadowTarget(U32 index);

    bool        shadersLoaded();
    bool        canUseWindLightShaders() const;
    bool        canUseAntiAliasing() const;

    // phases
    void resetFrameStats();

    void updateMoveDampedAsync(LLDrawable* drawablep);
    void updateMoveNormalAsync(LLDrawable* drawablep);
    void updateMovedList(LLDrawable::drawable_vector_t& move_list);
    void updateMove();
    bool visibleObjectsInFrustum(LLCamera& camera);
    bool getVisibleExtents(LLCamera& camera, LLVector3 &min, LLVector3& max);
    bool getVisiblePointCloud(LLCamera& camera, LLVector3 &min, LLVector3& max, std::vector<LLVector3>& fp, LLVector3 light_dir = LLVector3(0,0,0));
    bool pointCloudFromExtents(LLCamera& camera, const LLVector3& min, const LLVector3& max, std::vector<LLVector3>& fp);      // <FS> what getVisiblePointCloud makes of the visible extents (the walk is separate: LL_VK_PC_SPLIT)

    // Populate given LLCullResult with results of a frustum cull of the entire scene against the given LLCamera
    void updateCull(LLCamera& camera, LLCullResult& result, bool hud_attachments = false);
    void dropOcclusionMarks(LLCullResult& result);      // <FS> before a cull result is thrown away without doOcclusion() having run for it
    void createObjects(F32 max_dtime);
    void createObject(LLViewerObject* vobj);
    void processPartitionQ();
    void updateGeom(F32 max_dtime);
    void updateResources();
    void rebuildPriorityGroups();
    void rebuildGroups();
    void clearRebuildGroups();
    void clearRebuildDrawables();

    //calculate pixel area of given box from vantage point of given camera
    static F32 calcPixelArea(LLVector3 center, LLVector3 size, LLCamera& camera);
    static F32 calcPixelArea(const LLVector4a& center, const LLVector4a& size, LLCamera &camera);

    // <FS> defer_pools: the sort for the main view run BEFORE the shadow passes. It does everything except the pool face lists (which the shadow passes reset and fill for themselves):
    // the drawables that would be queued in a pool are remembered, and refillPools() queues them after the shadows, in the same order
    void stateSort(LLCamera& camera, LLCullResult& result, bool defer_pools = false);

    // <FS> view rework (doc/vulkan/PLAN-view-prep-rework.md, W1): what a view is, as data. Made once at the top of a stage from the state the pipeline has at that call (currentView); the stage and its
    // jobs read the view, not the globals and thread-locals it was made from. The jobs still install the camera id they work for (the walks below read it), from the view.
    struct View;                            // (defined after the render type enum: it holds a copy of the mask)
    static View currentView(LLCamera& camera, bool hud);

    // <FS> the stateSort split (doc/vulkan/PLAN-statesort-offmain.md, stage 1): the loops of stateSort make their writes to shared state (the rebuild and partition move queues, the LOD update of
    // avatars and other objects, the mesh rebuilds) through one list of deltas that is applied in the order it was recorded. LL_VK_SORT_SPLIT=1 sorts that way (compute everything, then apply);
    // =2 is the check: the loops run twice, deferred (nothing applied, the fields they write put back) and then as before with the same list recorded, and the two lists are compared.
    struct SortDelta
    {
        enum Kind : U8 { REBUILD_DRAWABLE = 0, PARTITION_MOVE, REBUILD_GROUP, OBJECT_LOD, REBUILD_MESH, BRIDGE_DEAD, WAKE_GROUP };
        Kind mKind;
        U32 mFlag;                  // REBUILD_DRAWABLE: the LLDrawable::EDrawableFlags
        LLDrawable* mDrawable;
        LLSpatialGroup* mGroup;
        F32 mDistance;              // OBJECT_LOD: the distance the drawable gets
    };
    static thread_local std::vector<SortDelta>* sSortSink;      // set: the effects are recorded, not made
    static thread_local std::vector<SortDelta>* sSortTee;       // set: recorded and made (the check's second run)
    // <FS> view rework A1 (doc/vulkan/PLAN-view-prep-rework.md section 8.3): the writes to shared state that the attachment loop of an avatar makes (the moved lists, the octree move of a drawable and of a bridge, the render
    // order written on a group) are recorded while sAvSink is set and replayed in the order they were made (applyAvOps) when the loop of the avatar is over. A drawable's own state bits (ON_MOVE_LIST, EARLY_MOVE, ACTIVE) are still set
    // at once, so the next step of the loop sees them. LL_VK_AV_LEDGER=1 turns it on.
    struct AvOp
    {
        enum Kind : U8 { MOVED_LIST = 0, MOVED_BRIDGE, MOVE_PARTITION, BRIDGE_MOVE, RENDER_ORDER, MAKE_ACTIVE_SHARED, UPDATE_TEXT, DIRTY_GROUP } mKind;
        LLDrawable* mDrawable;
        LLVOAvatar* mAvatar;            // RENDER_ORDER
        LLSpatialGroup* mSeenGroup;     // RENDER_ORDER: the group the loop saw when it made the request
        U32 mOrder;                     // RENDER_ORDER
        LLViewerObject* mObject = nullptr;      // UPDATE_TEXT
    };
    static thread_local std::vector<AvOp>* sAvSink;
    void applyAvOps(std::vector<AvOp>& ops);
    void applyAvOpsRange(const AvOp* begin, const AvOp* end);
    void applySortDeltasRange(const SortDelta* begin, const SortDelta* end);
    // <FS> view rework O1 (LL_VK_MOVED_PARALLEL): the entries of the moved list that are plain volume prims, grouped by linkset, run their updateMove as pool jobs that record their writes to shared state; the
    // writes are made, and the done handling of each entry, in list order afterwards. False when it did not run (too few entries, a census on, the setting off).
    bool updateMovedListParallel(LLDrawable::drawable_vector_t& moved_list);
    void updateMovedListSerial(LLDrawable::drawable_vector_t& moved_list, size_t first);
    static thread_local std::vector<LLDrawable*>* sSortPoolSink;     // set: the drawables the pools take by face are listed here, not in mDeferredPoolFill (a chunk of the parallel sort)
    static bool sSortJournalOn;                                  // set: the fields the loops write are saved first (the check's first run)
    static void sortJournalVolume(LLVOVolume* volume);
    void applySortDeltas(std::vector<SortDelta>& deltas);
    void sortLoops(LLCamera& camera, LLCullResult& read);
    void sortSplitCheck(LLCamera& camera, std::vector<SortDelta>& ledger_a, std::vector<SortDelta>& ledger_b);
    // stage 2: the compute on the pool (LL_VK_SORT_SPLIT=3; =4 checks it against the serial compute). False: the sort has to stay serial (no pool, a group whose occlusion state the main thread must change).
    bool sortLoopsParallel(const View& view, LLCullResult& read, std::vector<SortDelta>& ledger, void* journal);
    void sortDryRun(const View& view, bool parallel, void* result);
    void sortParallelCheck(const View& view);
    void refillPools(LLCullResult& result);
    void stateSort(LLSpatialGroup* group, LLCamera& camera);
    void stateSort(LLSpatialBridge* bridge, LLCamera& camera, bool fov_changed = false);
    void stateSort(LLDrawable* drawablep, LLCamera& camera);
    void postSort(LLCamera& camera);

    // <FS> Parallel preparation of the sun shadow cascades. The cull and the render map of each cascade are built on worker
    // threads (each with its own camera id and cull result); everything that touches shared or GL state (geometry rebuilds,
    // the draw pools) stays on the main thread. See generateSunShadow.
    struct ShadowStaticBuild;
    struct ShadowSkinBuild;
    struct ShadowAlphaBuild;
    // <FS> LL_VK_SHADOW_HULL: the part of the world that can put a shadow on the camera's view slice of one cascade: the slice (a truncated pyramid) pushed toward the sun without end.
    // A convex solid: the slice's faces that face away from the sun, and one plane through each silhouette edge along the sun direction. A group outside any plane cannot shadow the slice.
    struct ShadowSliceHull
    {
        bool mValid = false;
        S32 mCount = 0;
        F32 mPlane[24][4] = {};                 // outward normal and offset: a point p is inside a plane when n.p <= offset
        LLVector3 mCorners[8] = {};             // the slice's corners (near: left-up, right-up, right-down, left-down, then the far four): the verify rays start inside it
        LLVector3 mToSun;
    };
    struct ShadowCascadePrep
    {
        S32 mIndex = 0;                         // cascade number
        ShadowSliceHull mHull;                  // LL_VK_SHADOW_HULL
        LLCamera mCamera;                       // the cascade's shadow camera, frustum planes set
        LLCullResult* mResult = nullptr;
        glm::mat4 mView;                        // the cascade's matrices, and the ones it had the frame before
        glm::mat4 mProj;
        glm::mat4 mLastView;
        glm::mat4 mLastProj;
        const View* mJobView = nullptr;         // the view its jobs work for (view rework W3): set by generateSunShadow where the cascades are prepared, owned there
        bool mCullActive = false;               // FSShadowCascadeAvatarCull for this cascade
        LLVector3 mCullOrigin;
        LLVector3 mCullAt;
        F32 mCullSliceStart = 0.f;
        F32 mCullMargin = 0.f;
        const LLDrawable* mExclude = nullptr;   // J1 K3e: a spot light's own drawable, left out of its shadow pass's cull (what RenderSpotLight does for the unprepared pass; the jobs have no such global)
        std::vector<LLDrawable*> mDeferred;     // drawables of visible groups the workers left for the main thread (not volumes)
        std::shared_ptr<ShadowSkinBuild> mSkinBuild;        // the same for the rigged ones (their palettes included)
        // (the builders run on the async pool while the main thread draws the cascades before this one; renderShadow waits for them)
        LLAsyncPool::JobPtr mSkinJob;
        std::shared_ptr<ShadowAlphaBuild> mAlphaBuild[2];   // the casters with a texture, static and rigged
        LLAsyncPool::JobPtr mAlphaJob[2];
    };
    void shadowPrepCull(ShadowCascadePrep& prep, const std::vector<LLSpatialPartition*>& partitions);
    void shadowPrepHull(ShadowCascadePrep& prep);      // <FS> the second half of shadowPrepCull: the groups the slice cannot see a shadow from (LL_VK_SHADOW_HULL)
    // <FS> LL_VK_SC_SPLIT: the cull of all the pending cascades as pool jobs by partition and subtree (what a serial shadowPrepCull per cascade finds, in the same order)
    void shadowPrepCullSplit(std::vector<ShadowCascadePrep>& pending, const std::vector<LLSpatialPartition*>& partitions, class LLForkJoinPool* pool);
    // the marking half of the cull, for a range of the cascade's drawable groups (kind 0) or visible groups (kind 1): chunks of it run on the pool (MAINTHREAD-candidates.md, A); the drawables left to the main thread go into `deferred`
    void shadowPrepMark(ShadowCascadePrep& prep, int kind, U32 begin, U32 end, std::vector<LLDrawable*>& deferred, LLCullResult& local);
    void setShadowJobFlags(bool rt_raster = false);        // J1 K6 B: sViewJobsSkipStatic / sViewJobsSkipAlpha for the render map job of the shadow pass about to be prepared (the batch and the serial pass)
    void shadowPrepRenderMap(ShadowCascadePrep& prep, U32 begin, U32 end, LLCullResult& local);      // the visible groups [begin, end) of the cascade into `local` (a chunk of the render map job)
    void shadowPrepRebuild(std::vector<ShadowCascadePrep>& preps);
    void shadowPrepSubmitBuilds(ShadowCascadePrep& prep);
    void shadowPrepFinish(ShadowCascadePrep& prep);
    static void shadowPrepCompare(const ShadowCascadePrep& prep, LLCullResult& reference);
    static thread_local bool sParallelPrep; // true while a worker thread prepares a cascade: no shared statistics, bridges are locked
    static thread_local const LLDrawable* sPrepExclude; // J1 K3e: the drawable the pass being prepared on this thread leaves out (a spot light's own)

    void forAllVisibleDrawables(void (*func)(LLDrawable*));
    std::vector<LLDrawable*> mDeferredPoolFill;      // <FS> see stateSort(..., defer_pools)
    bool mDeferPoolFill = false;
    LLCullResult* getCull() const;        // <FS> the cull result this thread is working with (what beginRenderMap() reads)

    void renderFocusPoint(); // <FS:Beq/> FIRE-32023 Add focus point rendering

    void grabReferences(LLCullResult& result);
    void clearReferences();

    //check references will assert that there are no references in sCullResult to the provided data
    void checkReferences(LLFace* face);
    void checkReferences(LLDrawable* drawable);
    void checkReferences(LLDrawInfo* draw_info);
    void checkReferences(LLSpatialGroup* group);

    void renderGeomDeferred(LLCamera& camera, bool do_occlusion = false);
    void renderGeomPostDeferred(LLCamera& camera);
    void renderGeomShadow(LLCamera& camera);
    void bindLightFunc(LLShaderProgram& shader);

    // bind shadow maps
    // if setup is true, wil lset texture compare mode function and filtering options
    void bindShadowMaps(LLShaderProgram& shader);
    void bindDeferredShaderFast(LLShaderProgram& shader);
    void bindFastUnit(S32 k, S32 unit);                       // <FS> U1: one pass sampler of the Fast bind
    void bindDeferredFill(LLShaderProgram& shader);         // <FS> U1: the Fast bind in full
    void passFastState(void* out);                           // <FS> U1: the state the Fast bind depends on (PassFastState in pipeline.cpp)
    void bindDeferredShader(LLShaderProgram& shader, LLRenderTarget* light_target = nullptr, LLRenderTarget* depth_target = nullptr);
    void setupSpotLight(LLShaderProgram& shader, LLDrawable* drawablep);
    void spotLightRow(LLDrawable* drawablep, struct LLVkBridge::SpotVolumeRow& row, LLViewerTexture*& image);        // <FS> S6 LV2: setupSpotLight's values as a row of the projector record (and its side effects)

    void unbindDeferredShader(LLShaderProgram& shader);

    // set env_mat parameter in given shader
    void setEnvMat(LLShaderProgram& shader);

    void bindReflectionProbes(LLShaderProgram& shader);
    void stepPoissonOffset(LLShaderProgram& shader);
    void unbindReflectionProbes(LLShaderProgram& shader);

    void renderDeferredLighting();
    void rtSceneEarly(int where);            // <FS> LL_VK_RT_EARLY: the ray scene is built before the G-buffer (see renderDeferredLighting for the wait)
    bool mRtSceneEarly = false;
    F32 mRtEarlyView[16] = {};      // the view matrix and distance scale the early scene was built with (a comparison at the lighting, LL_VK_RT_EARLY)
    F32 mRtEarlyScale = 0.f;

    // apply atmospheric haze based on contents of color and depth buffer
    // should be called just before rendering water when camera is under water
    // and just before rendering alpha when camera is above water
    void doAtmospherics();

    // apply water haze based on contents of color and depth buffer
    // should be called just before rendering pre-water alpha objects
    void doWaterHaze();

    // Generate the water exclusion surface mask.
    void doWaterExclusionMask();

    void postDeferredGammaCorrect(LLRenderTarget* screen_target);

    void generateSunShadow(LLCamera& camera);
    LLRenderTarget* getSunShadowTarget(U32 i);
    LLRenderTarget* getSpotShadowTarget(U32 i);

    void renderHighlight(const LLViewerObject* obj, F32 fade);

    void renderShadow(const glm::mat4& view, const glm::mat4& proj, LLCamera& camera, LLCullResult& result, bool depth_clamp, ShadowCascadePrep* prepared = nullptr); // <FS> prepared: the cull and sort were done by the parallel preparation
    void renderSelectedFaces(const LLColor4& color);
    void renderHighlights();
    bool renderVignette(LLRenderTarget* src, LLRenderTarget* dst);
    bool renderSnapshotFrame(LLRenderTarget* src, LLRenderTarget* dst); // <FS:Beq/> Add snapshot frame rendering
    void renderSnapshotGuidesOverlay(); // <FS:Beq/> Add snapshot composition guide rendering
    void renderDebug();
    void renderPhysicsDisplay();

    void rebuildPools(); // Rebuild pools

    void findReferences(LLDrawable *drawablep); // Find the lists which have references to this object
    bool inMovedList(const LLDrawable* drawablep) const;    // <FS> LL_VK_DRAWABLE_TRACE: is the drawable an entry of the moved list (by pointer)
    static inline LLDrawable* sMovedCurrent = nullptr;      // <FS> the entry updateMovedListSerial is working on (main thread only)
    bool verify();                      // Verify that all data in the pipeline is "correct"

    S32  getLightCount() const { return static_cast<S32>(mLights.size()); }
    // <FS> LL_VK_EXPIRY (doc/vulkan/EXPIRY-design.md section 4, state Cold): 0 off, 1 counts what would be skipped, 2 skips. A group no camera has accepted for LL_VK_COLD_FRAMES (default 600, about 10 s) is Cold:
    // the moved list leaves the plain root objects in it alone while they stay inside the group, and animated textures of its objects are not stepped. The counters are logged with the scene census.
    static bool coldGroup(const LLSpatialGroup* group);
    // Released (LL_VK_EXPIRY=3): a VOLUME group no camera accepted for LL_VK_REL_FRAMES (default 3600, about 60 s) drops its draw infos and scene fragment (the vertex data stays) and is marked FS_RELEASED; when a camera
    // accepts it again it joins the wake queue, from which LL_VK_WAKE_PER_FRAME (default 48) groups a frame, nearest first, are rebuilt the way a dirty group is.
    void releaseScan();
    void wakeGroups();
    void wakeRequest(LLSpatialGroup* group);
    void wakeForget(LLSpatialGroup* group);
    void scheduleRelease(LLSpatialGroup* group);        // <FS> expiry (cold mode 3): the group's geometry was built; it is looked at again after LL_VK_REL_FRAMES frames (an age queue, no octree scan)
    void releaseForget(LLSpatialGroup* group);          // <FS> the group's draw map was cleared (a rebuild, a release, its death): it leaves the age queue
    std::vector<LLSpatialGroup*> mWakeQ;
    static U64 sRelReleased, sRelReleasedInfos, sRelWoken, sRelScans;
    static int coldMode();
    static U64 sColdMovedSeen, sColdMovedSkipped, sColdTexAnimSeen, sColdTexAnimSkipped, sColdMovedWoken;
    void pinMainThread();               // <FS> LL_VK_PIN_MAIN=<logical cpu>: pins the main thread (a test, doc/vulkan/PLAN-texture-handles.md section 17.22)
    void cpuProbe();                    // <FS> LL_VK_CPUPROBE=1: three fixed benchmarks on the main thread each frame, as CPU zones (does the thread itself get slower after a turn?)
    void sceneStateCensus();            // <FS> LL_VK_SCENECENSUS=1 (doc/vulkan/PLAN-texture-handles.md section 17.16): what the scene holds against what is in view, logged every 600 frames

    void calcNearbyLights(LLCamera& camera);
    void setupHWLights();
    void setupAvatarLights(bool for_edit = false);
    void enableLights(U32 mask);
    void enableLightsDynamic();
    void enableLightsAvatar();
    void enableLightsPreview();
    void enableLightsAvatarEdit(const LLColor4& color);
    void enableLightsFullbright();
    void disableLights();

    void shiftObjects(const LLVector3 &offset);

    void setLight(LLDrawable *drawablep, bool is_light);

    LLCullResult::sg_iterator beginAlphaGroups();
    LLCullResult::sg_iterator endAlphaGroups();
    LLCullResult::sg_iterator beginRiggedAlphaGroups();
    LLCullResult::sg_iterator endRiggedAlphaGroups();

    void addTrianglesDrawn(S32 index_count);
    void recordTrianglesDrawn();

    bool hasRenderDebugFeatureMask(const U32 mask) const    { return bool(mRenderDebugFeatureMask & mask); }
    bool hasRenderDebugMask(const U64 mask) const           { return bool(mRenderDebugMask & mask); }
    void setAllRenderDebugFeatures() { mRenderDebugFeatureMask = 0xffffffff; }
    void clearAllRenderDebugFeatures() { mRenderDebugFeatureMask = 0x0; }
    void setAllRenderDebugDisplays() { mRenderDebugMask = 0xffffffffffffffff; }
    void clearAllRenderDebugDisplays() { mRenderDebugMask = 0x0; }

    bool hasRenderType(const U32 type) const;
    bool hasAnyRenderType(const U32 type, ...) const;

    static bool isWaterClip();

    void setRenderTypeMask(U32 type, ...);
    // This is equivalent to 'setRenderTypeMask'
    //void orRenderTypeMask(U32 type, ...);
    void andRenderTypeMask(U32 type, ...);
    void clearRenderTypeMask(U32 type, ...);
    void setAllRenderTypes();
    void clearAllRenderTypes();

    void pushRenderTypeMask();
    void popRenderTypeMask();

    void pushRenderDebugFeatureMask();
    void popRenderDebugFeatureMask();

    static void toggleRenderType(U32 type);

    // For UI control of render features
    static bool hasRenderTypeControl(U32 data);
    static void toggleRenderDebug(U64 data);
    static void toggleRenderDebugFeature(U32 data);
    static void toggleRenderTypeControl(U32 data);
    static bool toggleRenderTypeControlNegated(S32 data);
    static bool toggleRenderDebugControl(U64 data);
    static bool toggleRenderDebugFeatureControl(U32 data);
    static void setRenderDebugFeatureControl(U32 bit, bool value);

    static void setRenderParticleBeacons(bool val);
    static void toggleRenderParticleBeacons();
    static bool getRenderParticleBeacons();

    static void setRenderSoundBeacons(bool val);
    static void toggleRenderSoundBeacons();
    static bool getRenderSoundBeacons();

    // <FS:PP> FIRE-33085 Region corner markers
    static void setRenderRegionCornerBeacons(bool val);
    static void toggleRenderRegionCornerBeacons();
    static bool getRenderRegionCornerBeacons();
    // </FS:PP>

    static void setRenderMOAPBeacons(bool val);
    static void toggleRenderMOAPBeacons();
    static bool getRenderMOAPBeacons();

    static void setRenderPhysicalBeacons(bool val);
    static void toggleRenderPhysicalBeacons();
    static bool getRenderPhysicalBeacons();

    static void setRenderScriptedBeacons(bool val);
    static void toggleRenderScriptedBeacons();
    static bool getRenderScriptedBeacons();

    static void setRenderScriptedTouchBeacons(bool val);
    static void toggleRenderScriptedTouchBeacons();
    static bool getRenderScriptedTouchBeacons();

    static void setRenderBeacons(bool val);
    static void toggleRenderBeacons();
    static bool getRenderBeacons();

    static void setRenderHighlights(bool val);
    static void toggleRenderHighlights();
    static bool getRenderHighlights();
    static void setRenderHighlightTextureChannel(LLRender::eTexIndex channel); // sets which UV setup to display in highlight overlay

    static void updateRenderTransparentWater();
    static void refreshCachedSettings();

    void addDebugBlip(const LLVector3& position, const LLColor4& color);

    void hidePermanentObjects( std::vector<U32>& restoreList );
    void restorePermanentObjects( const std::vector<U32>& restoreList );
    void skipRenderingOfTerrain( bool flag );
    void hideObject( const LLUUID& id );
    void restoreHiddenObject( const LLUUID& id );
    void handleShadowDetailChanged();

    LLReflectionMapManager mReflectionMapManager;
    LLHeroProbeManager mHeroProbeManager;

private:
    void unloadShaders();
    void addToQuickLookup( LLDrawPool* new_poolp );
    void removeFromQuickLookup( LLDrawPool* poolp );
    bool updateDrawableGeom(LLDrawable* drawable);
    void assertInitializedDoError();
    bool assertInitialized() { const bool is_init = isInit(); if (!is_init) assertInitializedDoError(); return is_init; };
    void connectRefreshCachedSettingsSafe(const std::string name);
    void hideDrawable( LLDrawable *pDrawable );
    void unhideDrawable( LLDrawable *pDrawable );
    void skipRenderingShadows();

    // <FS:Ansariel> Reset VB during TP
    void initDeferredVB();

public:
    enum {GPU_CLASS_MAX = 3 };

    enum LLRenderTypeMask
    {
        // Following are pool types (some are also object types)
        RENDER_TYPE_SKY                         = LLDrawPool::POOL_SKY,
        RENDER_TYPE_WL_SKY                      = LLDrawPool::POOL_WL_SKY,
        RENDER_TYPE_TERRAIN                     = LLDrawPool::POOL_TERRAIN,
        RENDER_TYPE_SIMPLE                      = LLDrawPool::POOL_SIMPLE,
        RENDER_TYPE_GRASS                       = LLDrawPool::POOL_GRASS,
        RENDER_TYPE_ALPHA_MASK                  = LLDrawPool::POOL_ALPHA_MASK,
        RENDER_TYPE_FULLBRIGHT_ALPHA_MASK       = LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK,
        RENDER_TYPE_FULLBRIGHT                  = LLDrawPool::POOL_FULLBRIGHT,
        RENDER_TYPE_BUMP                        = LLDrawPool::POOL_BUMP,
        RENDER_TYPE_MATERIALS                   = LLDrawPool::POOL_MATERIALS,
        RENDER_TYPE_AVATAR                      = LLDrawPool::POOL_AVATAR,
        RENDER_TYPE_CONTROL_AV                  = LLDrawPool::POOL_CONTROL_AV, // Animesh
        RENDER_TYPE_TREE                        = LLDrawPool::POOL_TREE,
        RENDER_TYPE_WATEREXCLUSION              = LLDrawPool::POOL_WATEREXCLUSION,
        RENDER_TYPE_VOIDWATER                   = LLDrawPool::POOL_VOIDWATER,
        RENDER_TYPE_WATER                       = LLDrawPool::POOL_WATER,
        RENDER_TYPE_GLTF_PBR                    = LLDrawPool::POOL_GLTF_PBR,
        RENDER_TYPE_GLTF_PBR_ALPHA_MASK         = LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK,
        RENDER_TYPE_ALPHA                       = LLDrawPool::POOL_ALPHA,
        RENDER_TYPE_ALPHA_PRE_WATER             = LLDrawPool::POOL_ALPHA_PRE_WATER,
        RENDER_TYPE_ALPHA_POST_WATER            = LLDrawPool::POOL_ALPHA_POST_WATER,
        RENDER_TYPE_GLOW                        = LLDrawPool::POOL_GLOW,
        RENDER_TYPE_PASS_SIMPLE                 = LLRenderPass::PASS_SIMPLE,
        RENDER_TYPE_PASS_SIMPLE_RIGGED = LLRenderPass::PASS_SIMPLE_RIGGED,
        RENDER_TYPE_PASS_GRASS                  = LLRenderPass::PASS_GRASS,
        RENDER_TYPE_PASS_FULLBRIGHT             = LLRenderPass::PASS_FULLBRIGHT,
        RENDER_TYPE_PASS_FULLBRIGHT_RIGGED = LLRenderPass::PASS_FULLBRIGHT_RIGGED,
        RENDER_TYPE_PASS_INVISIBLE              = LLRenderPass::PASS_INVISIBLE,
        RENDER_TYPE_PASS_INVISIBLE_RIGGED = LLRenderPass::PASS_INVISIBLE_RIGGED,
        RENDER_TYPE_PASS_INVISI_SHINY           = LLRenderPass::PASS_INVISI_SHINY,
        RENDER_TYPE_PASS_INVISI_SHINY_RIGGED = LLRenderPass::PASS_INVISI_SHINY_RIGGED,
        RENDER_TYPE_PASS_FULLBRIGHT_SHINY       = LLRenderPass::PASS_FULLBRIGHT_SHINY,
        RENDER_TYPE_PASS_FULLBRIGHT_SHINY_RIGGED = LLRenderPass::PASS_FULLBRIGHT_SHINY_RIGGED,
        RENDER_TYPE_PASS_SHINY                  = LLRenderPass::PASS_SHINY,
        RENDER_TYPE_PASS_SHINY_RIGGED = LLRenderPass::PASS_SHINY_RIGGED,
        RENDER_TYPE_PASS_BUMP                   = LLRenderPass::PASS_BUMP,
        RENDER_TYPE_PASS_BUMP_RIGGED = LLRenderPass::PASS_BUMP_RIGGED,
        RENDER_TYPE_PASS_POST_BUMP              = LLRenderPass::PASS_POST_BUMP,
        RENDER_TYPE_PASS_POST_BUMP_RIGGED = LLRenderPass::PASS_POST_BUMP_RIGGED,
        RENDER_TYPE_PASS_GLOW                   = LLRenderPass::PASS_GLOW,
        RENDER_TYPE_PASS_GLOW_RIGGED = LLRenderPass::PASS_GLOW_RIGGED,
        RENDER_TYPE_PASS_GLTF_GLOW = LLRenderPass::PASS_GLTF_GLOW,
        RENDER_TYPE_PASS_GLTF_GLOW_RIGGED = LLRenderPass::PASS_GLTF_GLOW_RIGGED,
        RENDER_TYPE_PASS_ALPHA                  = LLRenderPass::PASS_ALPHA,
        RENDER_TYPE_PASS_ALPHA_MASK             = LLRenderPass::PASS_ALPHA_MASK,
        RENDER_TYPE_PASS_ALPHA_MASK_RIGGED = LLRenderPass::PASS_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK  = LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK,
        RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK_RIGGED = LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_MATERIAL               = LLRenderPass::PASS_MATERIAL,
        RENDER_TYPE_PASS_MATERIAL_RIGGED = LLRenderPass::PASS_MATERIAL_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA         = LLRenderPass::PASS_MATERIAL_ALPHA,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK    = LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE= LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED = LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_SPECMAP                = LLRenderPass::PASS_SPECMAP,
        RENDER_TYPE_PASS_SPECMAP_RIGGED = LLRenderPass::PASS_SPECMAP_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_BLEND          = LLRenderPass::PASS_SPECMAP_BLEND,
        RENDER_TYPE_PASS_SPECMAP_BLEND_RIGGED = LLRenderPass::PASS_SPECMAP_BLEND_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_MASK           = LLRenderPass::PASS_SPECMAP_MASK,
        RENDER_TYPE_PASS_SPECMAP_MASK_RIGGED = LLRenderPass::PASS_SPECMAP_MASK_RIGGED,
        RENDER_TYPE_PASS_SPECMAP_EMISSIVE       = LLRenderPass::PASS_SPECMAP_EMISSIVE,
        RENDER_TYPE_PASS_SPECMAP_EMISSIVE_RIGGED = LLRenderPass::PASS_SPECMAP_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_NORMMAP                = LLRenderPass::PASS_NORMMAP,
        RENDER_TYPE_PASS_NORMMAP_RIGGED = LLRenderPass::PASS_NORMMAP_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_BLEND          = LLRenderPass::PASS_NORMMAP_BLEND,
        RENDER_TYPE_PASS_NORMMAP_BLEND_RIGGED = LLRenderPass::PASS_NORMMAP_BLEND_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_MASK           = LLRenderPass::PASS_NORMMAP_MASK,
        RENDER_TYPE_PASS_NORMMAP_MASK_RIGGED = LLRenderPass::PASS_NORMMAP_MASK_RIGGED,
        RENDER_TYPE_PASS_NORMMAP_EMISSIVE       = LLRenderPass::PASS_NORMMAP_EMISSIVE,
        RENDER_TYPE_PASS_NORMMAP_EMISSIVE_RIGGED = LLRenderPass::PASS_NORMMAP_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC               = LLRenderPass::PASS_NORMSPEC,
        RENDER_TYPE_PASS_NORMSPEC_RIGGED = LLRenderPass::PASS_NORMSPEC_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_BLEND         = LLRenderPass::PASS_NORMSPEC_BLEND,
        RENDER_TYPE_PASS_NORMSPEC_BLEND_RIGGED = LLRenderPass::PASS_NORMSPEC_BLEND_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_MASK          = LLRenderPass::PASS_NORMSPEC_MASK,
        RENDER_TYPE_PASS_NORMSPEC_MASK_RIGGED = LLRenderPass::PASS_NORMSPEC_MASK_RIGGED,
        RENDER_TYPE_PASS_NORMSPEC_EMISSIVE      = LLRenderPass::PASS_NORMSPEC_EMISSIVE,
        RENDER_TYPE_PASS_NORMSPEC_EMISSIVE_RIGGED = LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED,
        RENDER_TYPE_PASS_GLTF_PBR                 = LLRenderPass::PASS_GLTF_PBR,
        RENDER_TYPE_PASS_GLTF_PBR_RIGGED         = LLRenderPass::PASS_GLTF_PBR_RIGGED,
        RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK        = LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK,
        RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK_RIGGED = LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED,
        // Following are object types (only used in drawable mRenderType)
        RENDER_TYPE_HUD = LLRenderPass::NUM_RENDER_TYPES,
        RENDER_TYPE_VOLUME,
        RENDER_TYPE_PARTICLES,
        RENDER_TYPE_CLOUDS,
        RENDER_TYPE_HUD_PARTICLES,
        NUM_RENDER_TYPES,
        END_RENDER_TYPES = NUM_RENDER_TYPES
    };

    // <FS> view rework W1/W2 (doc/vulkan/PLAN-view-prep-rework.md): what a view is, as data. See currentView.
    struct View
    {
        LLCamera* mCamera = nullptr;
        S32 mCameraID = 0;                  // LLViewerCamera::eCameraID
        S32 mUseOcclusion = 0;
        bool mWorldCameraID = false;        // the camera id is CAMERA_WORLD
        bool mHud = false;                  // the HUD attachments' cull (updateCull's argument)
        bool mRenderingHUDs = false;        // the HUD pass (sRenderingHUDs)
        bool mShadowRender = false;
        bool mReflectionRender = false;
        bool mCubeSnapshot = false;
        bool mViewerCamera = false;         // the camera is the viewer's own
        bool mRenderTypes[NUM_RENDER_TYPES] = {};       // a copy of the render type mask: the jobs of a stage do not read the pipeline's
        // the world camera's view: not a HUD, a shadow cascade, a probe or a snapshot
        bool isWorld() const { return mWorldCameraID && !mHud && !mShadowRender && !mReflectionRender && !mCubeSnapshot; }
        // ... and made with the viewer's camera
        bool isMainWorld() const { return isWorld() && mViewerCamera; }
        // ... and not the HUD pass (what the sort and the render map of the main view ask)
        bool isMainWorldSort() const { return isMainWorld() && !mRenderingHUDs; }
        bool hasRenderType(U32 type) const { return type == 0 ? false : mRenderTypes[type]; }       // (as LLPipeline::hasRenderType)
    };

    // <FS> view rework W3: the view the job running on this thread works for (null: the thread works with the pipeline's own state, as the main thread does). The three flags below and the render type mask are read through
    // these, so the jobs of two views (the world and a shadow cascade) can have different values at the same time; a job installs its view with ViewScope (pipeline.cpp) and the pointer is put back afterwards.
    static thread_local const View* sJobView;
    static S32  useOcclusion()      { return sJobView ? sJobView->mUseOcclusion : sUseOcclusion; }
    static bool shadowRender()      { return sJobView ? sJobView->mShadowRender : sShadowRender; }
    static bool reflectionRender()  { return sJobView ? sJobView->mReflectionRender : sReflectionRender; }

    enum LLRenderDebugFeatureMask
    {
        RENDER_DEBUG_FEATURE_UI                 = 0x0001,
        RENDER_DEBUG_FEATURE_SELECTED           = 0x0002,
        RENDER_DEBUG_FEATURE_HIGHLIGHTED        = 0x0004,
        RENDER_DEBUG_FEATURE_DYNAMIC_TEXTURES   = 0x0008,
//      RENDER_DEBUG_FEATURE_HW_LIGHTING        = 0x0010,
        RENDER_DEBUG_FEATURE_FLEXIBLE           = 0x0010,
        RENDER_DEBUG_FEATURE_FOG                = 0x0020,
        RENDER_DEBUG_FEATURE_FR_INFO            = 0x0080,
        RENDER_DEBUG_FEATURE_FOOT_SHADOWS       = 0x0100,
    };

    enum LLRenderDebugMask: U64
    {
        RENDER_DEBUG_COMPOSITION        =  0x00000001,
        RENDER_DEBUG_VERIFY             =  0x00000002,
        RENDER_DEBUG_BBOXES             =  0x00000004,
        RENDER_DEBUG_OCTREE             =  0x00000008,
        RENDER_DEBUG_WIND_VECTORS       =  0x00000010,
        RENDER_DEBUG_OCCLUSION          =  0x00000020,
        RENDER_DEBUG_POINTS             =  0x00000040,
        RENDER_DEBUG_TEXTURE_PRIORITY   =  0x00000080,
        RENDER_DEBUG_TEXTURE_AREA       =  0x00000100,
        RENDER_DEBUG_FACE_AREA          =  0x00000200,
        RENDER_DEBUG_PARTICLES          =  0x00000400,
        RENDER_DEBUG_GLOW               =  0x00000800, // not used
        RENDER_DEBUG_TEXTURE_ANIM       =  0x00001000,
        RENDER_DEBUG_LIGHTS             =  0x00002000,
        RENDER_DEBUG_BATCH_SIZE         =  0x00004000,
        RENDER_DEBUG_ALPHA_BINS         =  0x00008000, // not used
        RENDER_DEBUG_RAYCAST            =  0x00010000,
        RENDER_DEBUG_AVATAR_DRAW_INFO   =  0x00020000,
        RENDER_DEBUG_SHADOW_FRUSTA      =  0x00040000,
        RENDER_DEBUG_SCULPTED           =  0x00080000,
        RENDER_DEBUG_AVATAR_VOLUME      =  0x00100000,
        RENDER_DEBUG_AVATAR_JOINTS      =  0x00200000,
        RENDER_DEBUG_AGENT_TARGET       =  0x00800000,
        RENDER_DEBUG_UPDATE_TYPE        =  0x01000000,
        RENDER_DEBUG_PHYSICS_SHAPES     =  0x02000000,
        RENDER_DEBUG_NORMALS            =  0x04000000,
        RENDER_DEBUG_LOD_INFO           =  0x08000000,
        RENDER_DEBUG_NODES              =  0x20000000,
        RENDER_DEBUG_TEXEL_DENSITY      =  0x40000000,
        RENDER_DEBUG_TRIANGLE_COUNT     =  0x80000000,
        RENDER_DEBUG_IMPOSTORS          = 0x100000000,
        RENDER_DEBUG_REFLECTION_PROBES  = 0x200000000,
        RENDER_DEBUG_PROBE_UPDATES      = 0x400000000,
        RENDER_DEBUG_TEXTURE_SIZE       = 0x800000000
    };

public:

    LLSpatialPartition* getSpatialPartition(LLViewerObject* vobj);

    void updateCamera(bool reset = false);

    LLVector3               mFlyCamPosition;
    LLQuaternion            mFlyCamRotation;

    bool                     mBackfaceCull;
    S32                      mMatrixOpCount;
    S32                      mTextureMatrixOps;
    S32                      mNumVisibleNodes;

    S32                      mDebugTextureUploadCost;
    S32                      mDebugSculptUploadCost;
    S32                      mDebugMeshUploadCost;

    std::atomic<S32>         mNumVisibleFaces;      // (<FS> the chunks of the parallel sort add to it)

    S32                     mPoissonOffset;

    static S32              sCompiles;

    static bool             sShowHUDAttachments;
    static bool             sForceOldBakedUpload; // If true will not use capabilities to upload baked textures.
    static S32              sUseOcclusion;  // 0 = no occlusion, 1 = read only, 2 = read/write
    static bool             sAutoMaskAlphaDeferred;
    static bool             sAutoMaskAlphaNonDeferred;
    static bool             sRenderTransparentWater;
    static bool             sBakeSunlight;
    static bool             sNoAlpha;
    static bool             sUseFarClip;
    static bool             sShadowRender;
    static S32              sRtProbePass;       // <FS> ray scene: the probe face being rendered (hero faces 7-12, reflection probe faces 13-18), -1 outside a probe face: its casters are named under that pass, not under the main view's

    static bool             sDynamicLOD;
    static bool             sPickAvatar;
    static bool             sReflectionRender;
    static bool             sDistortionRender;
    static bool             sImpostorRender;
    static bool             sImpostorRenderAlphaDepthPass;
    static bool             sShowJellyDollAsImpostor;
    static bool             sUnderWaterRender;
    static bool             sRenderGlow;
    static bool             sGlowDrawn;     // <FS> a glow second draw (the bloom's alpha) was made since the last renderFinalize: with none, the glow passes are skipped
    static bool             sTextureBindTest;
    static bool             sRenderAttachedLights;
    static bool             sRenderAttachedParticles;
    static bool             sRenderDeferred;
    static bool             sReflectionProbesEnabled;
    // <FS:Beq> [FIRE-35070] Address gradual slowdown issue
    static S32              sReflectionProbeLevel;
    // </FS:Beq>
    static S32              sVisibleLightCount;
    static bool             sRenderingHUDs;
    static F32              sDistortionWaterClipPlaneMargin;
    static F32              sVolumeSAFrame;

    static bool             sRenderParticles; // <FS:LO> flag to hold correct, user selected, status of particles
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    static bool             sRenderTextures;
// [/SL:KB]
    static LLVector3        sLastFocusPoint;// <FS:Beq/> FIRE-16728 focus point lock & free focus DoF 
    static bool             sDoFEnabled;// <FS:Beq/> FIRE-32023 focus point render 
    static LLTrace::EventStatHandle<S64> sStatBatchSize;

    class RenderTargetPack
    {
    public:
        U32                     width = 0;
        U32                     height = 0;

        //screen texture
        LLRenderTarget          screen;
        LLRenderTarget          deferredScreen;
        LLRenderTarget          deferredLight;

        //sun shadow map
        LLRenderTarget          shadow[4];
    };

    // main full resoltuion render target
    RenderTargetPack mMainRT;

    // auxillary 512x512 render target pack
    // used by reflection probes and dynamic texture bakes
    RenderTargetPack mAuxillaryRT;
    // <FS:Beq> Fix the build floater preview window
    // dedicated 2048x2048 render target for preview
    // used by preview window dynamic textures
    LLRenderTarget mPreviewScreen; 
    // </FS:Beq>

    // Auxillary render target pack scaled to the hero probe's per-face size.
    RenderTargetPack mHeroProbeRT;

    // <FS> the planar mirror (doc/vulkan/PLAN-planar-mirror.md): the reflected view, at the main view's render size (allocated with LL_VK_MIRROR_PLANAR=1)
    RenderTargetPack mMirrorViewRT;
    // <FS> the mirror view's quality setting (RenderHeroProbeResolution 256 / 512 / 1024 / 2048: 1/4, 1/2, full, full of the main view's size): below full size the view is rendered into the smaller
    // mMirrorViewRT and enlarged into this target, at the main view's size, which is what the glass reads (the glass shaders find a pixel by the size of the texture)
    LLRenderTarget mMirrorUpscaled;
    static U32 mirrorViewDivisor();
    void upscaleMirrorView();

    // currently used render target pack
    RenderTargetPack* mRT;

    LLRenderTarget          mSpotShadow[2];

    LLRenderTarget          mPbrBrdfLut;
    LLRenderTarget          mWaterExclusionMask;

    // copy of the color/depth buffer just before gamma correction
    // for use by SSR
    LLRenderTarget          mSceneMap;

    // exposure map for getting average color in scene
    LLRenderTarget          mLuminanceMap;
    LLRenderTarget          mExposureMap;
    LLRenderTarget          mLastExposure;

    // tonemapped and gamma corrected render ready for post
    LLRenderTarget          mPostPingMap;
    LLRenderTarget          mPostPongMap;

    // FXAA helper target
    LLRenderTarget          mFXAAMap;
    LLRenderTarget          mSMAABlendBuffer;

    // render ui to buffer target
    LLRenderTarget          mUIScreen;

    // downres scratch space for GPU downscaling of textures
    LLRenderTarget          mDownResMap;

    // 2k bom scratch target
    LLRenderTarget          mBakeMap;

    LLCullResult            mSky;
    LLCullResult            mReflectedObjects;
    LLCullResult            mRefractedObjects;

    //utility buffers for rendering post effects
    LLPointer<LLVertexBuffer> mDeferredVB;

    // a single triangle that covers the whole screen
    LLPointer<LLVertexBuffer> mScreenTriangleVB;

    //utility buffer for rendering cubes, 8 vertices are corners of a cube [-1, 1]
    LLPointer<LLVertexBuffer> mCubeVB;

    //list of currently bound reflection maps
    std::vector<LLReflectionMap*> mReflectionMaps;

    std::vector<LLVector3>  mShadowFrustPoints[4];
    LLVector4               mShadowError;
    LLVector4               mShadowFOV;
    LLVector3               mShadowFrustOrigin[4];
    LLCamera                mShadowCamera[8];
    LLVector3               mShadowExtents[4][2];
    // TODO : separate Sun Shadow and Spot Shadow matrices
    glm::mat4               mSunShadowMatrix[6];
    glm::mat4               mShadowModelview[6];
    glm::mat4               mShadowProjection[6];
    glm::mat4               mReflectionModelView;

    LLPointer<LLDrawable>   mShadowSpotLight[2];
    F32                     mSpotLightFade[2];
    LLPointer<LLDrawable>   mTargetShadowSpotLight[2];

    LLVector4               mSunClipPlanes;
    LLVector4               mSunOrthoClipPlanes;
    LLVector2               mScreenScale;

    //water distortion texture (refraction)
    LLRenderTarget              mWaterDis;

    static const U32 MAX_PREVIEW_WIDTH;
    static const U32 MAX_PREVIEW_HEIGHT; // <FS:Beq/> dedicated render target for previews

    //texture for making the glow
    LLRenderTarget              mGlow[3];
    bool                        mGlowHasContent = true;     // <FS> mGlow[1] may hold something other than zeros (a frame with glow, or a fresh target): cleared once when a frame without glow follows
    bool                        mGlowCombine = false;       // <FS> this frame's glow passes ran: the combine pass has something to add

    // texture for SH indirect sky contribution
    LLRenderTarget              mSkySH;

    //noise map
    U32                 mNoiseMap;
    U32                 mTrueNoiseMap;
    U32                 mLightFunc;

    //smaa
    U32                 mSMAAAreaMap = 0;
    U32                 mSMAASearchMap = 0;
    U32                 mSMAASampleMap = 0;

    LLColor4            mSunDiffuse;
    LLColor4            mMoonDiffuse;
    LLVector4           mSunDir;
    LLVector4           mMoonDir;
    bool                mNeedsShadowTargetClear;

    LLVector4           mTransformedSunDir;
    LLVector4           mTransformedMoonDir;

    bool                    mInitialized;
    bool                    mShadersLoaded;

    U32                     mTransformFeedbackPrimitives; //number of primitives expected to be generated by transform feedback
protected:
    bool                    mRenderTypeEnabled[NUM_RENDER_TYPES];
    std::stack<std::string> mRenderTypeEnableStack;

    U32                     mRenderDebugFeatureMask;
    U64                     mRenderDebugMask;
    U64                     mOldRenderDebugMask;
    std::stack<U32>         mRenderDebugFeatureStack;

    /////////////////////////////////////////////
    //
    //
    LLDrawable::drawable_vector_t   mMovedList;
    LLDrawable::drawable_vector_t mMovedBridge;
    LLDrawable::drawable_vector_t   mShiftList;

    /////////////////////////////////////////////
    //
    //
    struct Light
    {
        Light(LLDrawable* ptr, F32 d, F32 f = 0.0f)
            : drawable(ptr),
              dist(d),
              fade(f)
        {}
        LLPointer<LLDrawable> drawable;
        F32 dist;
        F32 fade;
        // <FS> the light's sphere (centre, and the radius the lighting pass draws plus a margin) as of the last calcNearbyLights: the lighting pass tests it against the camera's frustum before it
        // touches the drawable, the volume and its parameters. mSize < 0: not worked out yet (a light that is new this frame), the pass does the full test.
        F32 mCenter[3] = { 0.f, 0.f, 0.f };
        F32 mSize = -1.f;
        struct compare
        {
            bool operator()(const Light& a, const Light& b) const
            {
                if ( a.dist < b.dist )
                    return true;
                else if ( a.dist > b.dist )
                    return false;
                else
                    return a.drawable < b.drawable;
            }
        };
    };
    // The nearby lights, sorted by distance (begin() is the closest). A sorted vector, not a std::set: the set was rebuilt and copied every frame
    // (two node allocations and frees for each of the 1,400 lights at a crowded plaza, 0.46 ms a frame: doc/vulkan/MAINTHREAD-candidates.md); the
    // vector is updated in place and sorted only when the order changed.
    class light_set_t
    {
    public:
        typedef std::vector<Light>::iterator iterator;
        typedef std::vector<Light>::const_iterator const_iterator;
        typedef std::vector<Light>::reverse_iterator reverse_iterator;

        iterator begin() { return mLights.begin(); }
        iterator end() { return mLights.end(); }
        const_iterator begin() const { return mLights.begin(); }
        const_iterator end() const { return mLights.end(); }
        reverse_iterator rbegin() { return mLights.rbegin(); }
        size_t size() const { return mLights.size(); }
        bool empty() const { return mLights.empty(); }
        void clear() { mLights.clear(); }
        iterator insert(const Light& light)
        {
            return mLights.insert(std::upper_bound(mLights.begin(), mLights.end(), light, Light::compare()), light);
        }
        iterator erase(iterator it) { return mLights.erase(it); }
        std::vector<Light>& vec() { return mLights; }
        void sortIfNeeded()
        {
            if (!std::is_sorted(mLights.begin(), mLights.end(), Light::compare()))
            {
                std::sort(mLights.begin(), mLights.end(), Light::compare());
            }
        }
    private:
        std::vector<Light> mLights;
    };

    LLDrawable::ordered_drawable_set_t  mLights;
    U32                             mLightsVersion = 0;     // counts the inserts and erases of mLights
    U32                             mLightScanFrame = 0;    // the frame of the last scan of mLights for new nearby lights, its camera position and mLightsVersion
    U32                             mLightScanVersion = ~0u;
    LLVector3                       mLightScanPos;
    light_set_t                     mNearbyLights; // lights near camera
    LLColor4                        mHWLightColors[8];

    /////////////////////////////////////////////
    //
    // Different queues of drawables being processed.
    //
    LLDrawable::drawable_list_t     mBuildQ1; // priority
    LLSpatialGroup::sg_vector_t     mGroupQ1; //priority

    LLSpatialGroup::sg_vector_t     mGroupSaveQ1; // a place to save mGroupQ1 until it is safe to unref

    LLSpatialGroup::sg_vector_t     mMeshDirtyGroup; //groups that need rebuildMesh called

    // <FS:ND> A vector is much better suited for the use case of mPartitionQ
    // LLDrawable::drawable_list_t      mPartitionQ; //drawables that need to update their spatial partition radius
    LLDrawable::drawable_vector_t   mPartitionQ; //drawables that need to update their spatial partition radius
    // </FS:ND>

    bool mGroupQ1Locked;

    bool mResetVertexBuffers; //if true, clear vertex buffers on next update

    LLViewerObject::vobj_list_t     mCreateQ;

    LLDrawable::drawable_set_t      mRetexturedList;

    class HighlightItem
    {
    public:
        const LLPointer<LLDrawable> mItem;
        mutable F32 mFade;

        HighlightItem(LLDrawable* item)
        : mItem(item), mFade(0)
        {
        }

        bool operator<(const HighlightItem& rhs) const
        {
            return mItem < rhs.mItem;
        }

        bool operator==(const HighlightItem& rhs) const
        {
            return mItem == rhs.mItem;
        }

        void incrFade(F32 val) const
        {
            mFade = llclamp(mFade+val, 0.f, 1.f);
        }
    };

    //////////////////////////////////////////////////
    //
    // Draw pools are responsible for storing all rendered data,
    // and performing the actual rendering of objects.
    //
    struct compare_pools
    {
        bool operator()(const LLDrawPool* a, const LLDrawPool* b) const
        {
            if (!a)
                return true;
            else if (!b)
                return false;
            else
            {
                S32 atype = a->getType();
                S32 btype = b->getType();
                if (atype < btype)
                    return true;
                else if (atype > btype)
                    return false;
                else
                    return a->getId() < b->getId();
            }
        }
    };
    typedef std::set<LLDrawPool*, compare_pools > pool_set_t;
    pool_set_t mPools;
    LLDrawPool* mLastRebuildPool;

    // For quick-lookups into mPools (mapped by texture pointer)
    std::map<uintptr_t, LLDrawPool*>    mTerrainPools;
    std::map<uintptr_t, LLDrawPool*>    mTreePools;
    LLDrawPoolAlpha*            mAlphaPoolPreWater = nullptr;
    LLDrawPoolAlpha*            mAlphaPoolPostWater = nullptr;
    LLDrawPool*                 mSkyPool = nullptr;
    LLDrawPool*                 mTerrainPool = nullptr;
    LLDrawPool*                 mWaterPool = nullptr;
    LLRenderPass*               mSimplePool = nullptr;
    LLRenderPass*               mGrassPool = nullptr;
    LLRenderPass*               mAlphaMaskPool = nullptr;
    LLRenderPass*               mFullbrightAlphaMaskPool = nullptr;
    LLRenderPass*               mFullbrightPool = nullptr;
    LLDrawPool*                 mGlowPool = nullptr;
    LLDrawPool*                 mBumpPool = nullptr;
    LLDrawPool*                 mMaterialsPool = nullptr;
    LLDrawPool*                 mWLSkyPool = nullptr;
    LLDrawPool*                 mPBROpaquePool = nullptr;
    LLDrawPool*                 mPBRAlphaMaskPool = nullptr;
    LLDrawPool*                 mWaterExclusionPool      = nullptr;

    // Note: no need to keep an quick-lookup to avatar pools, since there's only one per avatar

public:
    std::vector<LLFace*>        mHighlightFaces;    // highlight faces on physical objects
protected:
    std::vector<LLFace*>        mSelectedFaces;

    class DebugBlip
    {
    public:
        LLColor4 mColor;
        LLVector3 mPosition;
        F32 mAge;

        DebugBlip(const LLVector3& position, const LLColor4& color)
            : mColor(color), mPosition(position), mAge(0.f)
        { }
    };

    std::list<DebugBlip> mDebugBlips;

    LLPointer<LLViewerFetchedTexture>   mFaceSelectImagep;

    U32                     mLightMask;
    U32                     mLightMovingMask;

    // <FS:Beq> Add snapshot guides as part of UI rendering to avoid issues in compositor
    struct SnapshotGuideState
    {
        enum class Style : U8
        {
            RuleOfThirds,
            GoldenRatio,
            Diagonal
        };

        enum class GoldenOrientation : U8
        {
            TopLeft,
            TopRight,
            BottomLeft,
            BottomRight
        };

        bool        active = false;
        bool        show_guides = false;
        F32         left = 0.f;
        F32         right = 1.f;
        F32         bottom = 0.f;
        F32         top = 1.f;
        LLColor3    color = LLColor3(1.f, 1.f, 1.f);
        F32         thickness = 0.f;
        F32         visibility = 0.f;
        Style       style = Style::RuleOfThirds;
        GoldenOrientation golden_orientation = GoldenOrientation::TopLeft;
    };

    SnapshotGuideState      mSnapshotGuideState;
    // </FS:Beq>
    static bool             sRenderPhysicalBeacons;
    static bool             sRenderMOAPBeacons;
    static bool             sRenderScriptedTouchBeacons;
    static bool             sRenderScriptedBeacons;
    static bool             sRenderParticleBeacons;
    static bool             sRenderSoundBeacons;
    static bool             sRenderRegionCornerBeacons; // <FS:PP> FIRE-33085 Region corner markers
public:
    static bool             sRenderBeacons;
    static bool             sRenderHighlight;

    // Determines which set of UVs to use in highlight display
    //
    static LLRender::eTexIndex sRenderHighlightTextureChannel;

    //debug use
    static U32              sCurRenderPoolType ;

    //cached settings
    static bool WindLightUseAtmosShaders;
    static bool RenderDeferred;
    static F32 RenderDeferredSunWash;
    static U32 RenderFSAAType;
    static U32 RenderResolutionDivisor;
// [SL:KB] - Patch: Settings-RenderResolutionMultiplier | Checked: Catznip-5.4
    static F32 RenderResolutionMultiplier;
// [/SL:KB]
    static bool RenderUIBuffer;
    static S32 RenderShadowDetail;
    static S32 RenderShadowSplits;
    static bool RenderDeferredSSAO;
    static F32 RenderShadowResolutionScale;
    static bool RenderDelayCreation;
//  static bool RenderAnimateRes; <FS:Beq> FIRE-23122 BUG-225920 Remove broken RenderAnimateRes functionality.
    static bool FreezeTime;
    static S32 DebugBeaconLineWidth;
    static F32 RenderHighlightBrightness;
    static LLColor4 RenderHighlightColor;
    static F32 RenderHighlightThickness;
    static bool RenderSpotLightsInNondeferred;
    static LLColor4 PreviewAmbientColor;
    static LLColor4 PreviewDiffuse0;
    static LLColor4 PreviewSpecular0;
    static LLColor4 PreviewDiffuse1;
    static LLColor4 PreviewSpecular1;
    static LLColor4 PreviewDiffuse2;
    static LLColor4 PreviewSpecular2;
    static LLVector3 PreviewDirection0;
    static LLVector3 PreviewDirection1;
    static LLVector3 PreviewDirection2;
    static F32 RenderGlowMinLuminance;
    static F32 RenderGlowMaxExtractAlpha;
    static F32 RenderGlowWarmthAmount;
    static LLVector3 RenderGlowLumWeights;
    static LLVector3 RenderGlowWarmthWeights;
    static S32 RenderGlowResolutionPow;
    static S32 RenderGlowIterations;
    static F32 RenderGlowWidth;
    static F32 RenderGlowStrength;
    static bool RenderGlowNoise;
    static bool RenderDepthOfField;
    static bool RenderDepthOfFieldInEditMode;
    // <FS:Beq> FIRE-16728
    static bool FSFocusPointLocked;
    static bool FSFocusPointFollowsPointer;
    // </FS:Beq>
    static F32 CameraFocusTransitionTime;
    static F32 CameraFNumber;
    static F32 CameraFocalLength;
    static F32 CameraFieldOfView;
    static F32 RenderShadowNoise;
    static F32 RenderShadowBlurSize;
    static F32 RenderSSAOScale;
    static U32 RenderSSAOMaxScale;
    static F32 RenderSSAOFactor;
    static LLVector3 RenderSSAOEffect;
    static F32 RenderShadowOffsetError;
    static F32 RenderShadowBiasError;
    static F32 RenderShadowOffset;
    static F32 RenderShadowBias;
    static F32 RenderSpotShadowOffset;
    static F32 RenderSpotShadowBias;
    static LLDrawable* RenderSpotLight;
    static F32 RenderEdgeDepthCutoff;
    static F32 RenderEdgeNormCutoff;
    static LLVector3 RenderShadowGaussian;
    static F32 RenderShadowBlurDistFactor;
    static bool RenderDeferredAtmospheric;
    static F32 RenderHighlightFadeTime;
    static F32 RenderFarClip;
    static LLVector3 RenderShadowSplitExponent;
    static F32 RenderShadowErrorCutoff;
    static F32 RenderShadowFOVCutoff;
    static bool CameraOffset;
    static F32 CameraMaxCoF;
    static F32 CameraDoFResScale;
    static LLVector3 RenderVignette; // <FS:Beq/> refresh of vignette support
    static F32 RenderAutoHideSurfaceAreaLimit;
    static bool RenderScreenSpaceReflections;
    static S32 RenderScreenSpaceReflectionIterations;
    static F32 RenderScreenSpaceReflectionRayStep;
    static F32 RenderScreenSpaceReflectionDistanceBias;
    static F32 RenderScreenSpaceReflectionDepthRejectBias;
    static F32 RenderScreenSpaceReflectionAdaptiveStepMultiplier;
    static S32 RenderScreenSpaceReflectionGlossySamples;
    static S32 RenderBufferVisualization;
    static bool RenderMirrors;
    static S32 RenderHeroProbeUpdateRate;
    static S32 RenderHeroProbeConservativeUpdateMultiplier;
    static bool RenderAvatarCloth;
};

void render_bbox(const LLVector3 &min, const LLVector3 &max);
void render_hud_elements();
// <FS> ray scene materials: the material id (pipeline.cpp registry) of an avatar body mesh drawn with this texture, 0 when materials are off. color: 4 floats or null.
class LLViewerTexture;
U32 rt_body_material(LLViewerTexture* texture, const F32* color, bool masked);

extern LLPipeline gPipeline;
extern bool gDebugPipeline;
extern const LLMatrix4* gRenderLastMatrix;

#endif
