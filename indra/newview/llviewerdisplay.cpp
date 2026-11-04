/**
 * @file llviewerdisplay.cpp
 * @brief LLViewerDisplay class implementation
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
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
#include <array>
#include "llasyncpool.h"
#include "lltracethreadrecorder.h"
#include "llvksubstream.h"
#include "lldrawpoolmaterials.h"
#include <chrono>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>

#include "llviewerdisplay.h"
#include "llgpuzonetimer.h"
#include "llvkbridge.h"

#include "fsyspath.h"
#include "hexdump.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcoord.h"
#include "llcriticaldamp.h"
#include "llcubemap.h"
#include "lldir.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolbump.h"
#include "lldrawpoolwater.h"
#include "lldynamictexture.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llfeaturemanager.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "lldevicecaps.h"
#include "llrenderheaders.h"
#include "llgltfmateriallist.h"
#include "llhudmanager.h"
#include "llimagepng.h"
#include "llmachineid.h"
#include "llmemory.h"
#include "llparcel.h"
#include "llperfstats.h"
#include "llpostprocess.h"
#include "llrender.h"
#include "llscenemonitor.h"
#include "llsdjson.h"
#include "llselectmgr.h"
#include "llsky.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "llstartup.h"
#include "lltooldraganddrop.h"
#include "lltoolfocus.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "lltracker.h"
#include "lltrans.h"
#include "llui.h"
#include "lluuid.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llcontrolavatar.h"
#include "llvograss.h"
#include "llworld.h"
#include "pipeline.h"

#include <boost/json.hpp>
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "llvisualeffect.h"
#include "rlvactions.h"
#include "rlvlocks.h"
// [/RLVa:KB]
#include "llpresetsmanager.h"
#include "fsdata.h"

// <FS:PP> Render chat range spheres in 3D world
#include "lfsimfeaturehandler.h"
#include "llrendersphere.h"
#include "lluicolortable.h"
// </FS:PP>

#include <filesystem>
#include <iomanip>
#include <sstream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

extern LLPointer<LLViewerTexture> gStartTexture;
extern bool gShiftFrame;

LLPointer<LLViewerTexture> gDisconnectedImagep = nullptr;

// used to toggle renderer back on after teleport
bool         gTeleportDisplay = false;
LLFrameTimer gTeleportDisplayTimer;
LLFrameTimer gTeleportArrivalTimer;
constexpr F32 RESTORE_GL_TIME = 5.f;  // Wait this long while reloading textures before we raise the curtain
// <FS:Ansariel> Draw Distance stepping; originally based on SpeedRez by Henri Beauchamp, licensed under LGPL
F32         gSavedDrawDistance = 0.0f;
F32         gLastDrawDistanceStep = 0.0f;
// <FS:Ansariel> FIRE-12004: Attachments getting lost on TP
LLFrameTimer gPostTeleportFinishKillObjectDelayTimer;

bool gForceRenderLandFence = false;
bool gDisplaySwapBuffers = false;
bool gDepthDirty = false;
bool gResizeScreenTexture = false;
bool gResizeShadowTexture = false;
bool gWindowResized = false;
bool gSnapshot = false;
bool gCubeSnapshot = false;
bool gSnapshotNoPost = false;
bool gShaderProfileFrame = false;

// This is how long the sim will try to teleport you before giving up.
constexpr F32 TELEPORT_EXPIRY = 15.0f;
// Additional time (in seconds) to wait per attachment
constexpr F32 TELEPORT_EXPIRY_PER_ATTACHMENT = 3.f;

U32 gRecentFrameCount = 0; // number of 'recent' frames
LLFrameTimer gRecentFPSTime;
LLFrameTimer gRecentMemoryTime;
LLFrameTimer gAssetStorageLogTime;

// Rendering stuff
void render_ui(F32 zoom_factor = 1.f, int subfield = 0);
void swap();
void render_hud_attachments();
void render_ui_3d();
void render_ui_2d();
void render_disconnected_background();

void getProfileStatsContext(boost::json::object& stats);
std::string getProfileStatsFilename();

void display_startup()
{
    if (   !gViewerWindow
        || !gViewerWindow->getActive()
        || !gViewerWindow->getWindow()->getVisible()
        || gViewerWindow->getWindow()->getMinimized()
        || gNonInteractive)
    {
        return;
    }

    { LL_CPU_ZONE("display: updateResources"); gPipeline.updateResources(); }

    // <FS> Vulkan mirror frame (does nothing unless LL_VK_MIRROR is set)
    LLVkBridge::beginFrame(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());

    // Written as branch to appease GCC which doesn't like different
    // pointer types across ternary ops
    //
    if (!LLViewerFetchedTexture::sWhiteImagep.isNull())
    {
    LLTexUnit::sWhiteTexture = LLViewerFetchedTexture::sWhiteImagep->getTexName();
    }

    LLRenderPresetDefault gls_default;

    // Required for HTML update in login screen
    static S32 frame_count = 0;


    if (frame_count++ > 1) // make sure we have rendered a frame first
    {
        LLViewerDynamicTexture::updateAllInstances();
    }
    else
    {
        LL_DEBUGS("Window") << "First display_startup frame" << LL_ENDL;
    }


    gRender.clearBuffers(ClearBits::Depth | ClearBits::Color); // | ClearBits::Stencil);
    LLRenderPresetUIDefault gls_ui;
    gPipeline.disableLights();

    if (gViewerWindow)
    gViewerWindow->setup2DRender();
    if (gViewerWindow)
    { LL_CPU_ZONE("ui: gViewerWindow->draw (widgets)"); gViewerWindow->draw(); }
    gRender.flush();

    LLVertexBuffer::unbind();


    LLVkBridge::endFrame(); // <FS>
    if (gViewerWindow && gViewerWindow->getWindow())
    gViewerWindow->getWindow()->swapBuffers();

    gRender.clearBuffers(ClearBits::Depth);
}

void display_update_camera()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Update Camera");
    // TODO: cut draw distance down if customizing avatar?
    // TODO: cut draw distance on per-parcel basis?

    // Cut draw distance in half when customizing avatar,
    // but on the viewer only.
    F32 final_far = gAgentCamera.mDrawDistance;
    // <FS:TJ> [FIRE-35748] Only enable the LL Draw Distance VRAM optimization when the setting is enabled
    static LLCachedControl<bool> use_vram_optimization(gSavedSettings, "FSDrawDistanceVRAMOptimization", false);
    // </FS:TJ>
    if (gCubeSnapshot)
    {
        static LLCachedControl<F32> reflection_probe_draw_distance(gSavedSettings, "RenderReflectionProbeDrawDistance", 64.f);
        final_far = reflection_probe_draw_distance();
    }
    else if (CAMERA_MODE_CUSTOMIZE_AVATAR == gAgentCamera.getCameraMode())
    {
        final_far *= 0.5f;
    }
    // When system memory is critically low or recovering, shrink draw distance.
    // <FS:TJ> [FIRE-35748] Only enable the LL Draw Distance VRAM optimization when the setting is enabled
    //else if (const F32 mem_factor = LLMemory::getSystemMemoryBudgetFactor(); mem_factor > 1.f)
    else if (const F32 mem_factor = LLMemory::getSystemMemoryBudgetFactor(); use_vram_optimization && mem_factor > 1.f)
    {
        final_far = llmax(32.f, final_far / mem_factor);
    }
// <FS:CR> Aurora sim
    if (LLWorld::getInstance()->getLockedDrawDistance())
    {
        //Reset the draw distance and do not update with the new val
        final_far = LLViewerCamera::getInstance()->getFar();
    }
// </FS:CR> Aurora sim
    LLViewerCamera::getInstance()->setFar(final_far);
    LLVOAvatar::sRenderDistance = llclamp(final_far, 16.f, 256.f);
    gPipeline.updateJitter();   // FSVkDLSS: this projection is the main scene's
    gViewerWindow->setup3DRender();
    LLViewerCamera::sJitterActive = false;

    if (!gCubeSnapshot)
    {
        // Update land visibility too
        LLWorld::getInstance()->setLandFarClip(final_far);
    }
}

// Write some stats to LL_INFOS()
void display_stats()
{
    LL_PROFILE_ZONE_SCOPED;
    {
        // how much of the world has arrived, every 5 s: objects known to the viewer and vertex buffer draws per frame (for telling a world still loading from a rendering fault)
        static LLTimer load_timer;
        static U64 last_draws = 0;
        static U32 load_frames = 0;
        ++load_frames;
        if (load_timer.getElapsedTimeF32() >= 5.f)
        {
            const U64 draws = LLVertexBuffer::sDrawCalls;
            LL_INFOS("VkLoad") << "world: " << gObjectList.getNumObjects() << " objects, " << (load_frames ? (draws - last_draws) / load_frames : 0) << " vertex buffer draws per frame, " << load_frames << " frames" << LL_ENDL;
            last_draws = draws;
            load_frames = 0;
            load_timer.reset();
        }
    }
    constexpr F32 FPS_LOG_FREQUENCY = 10.f;
    if (gRecentFPSTime.getElapsedTimeF32() >= FPS_LOG_FREQUENCY)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - FPS");
        LLTrace::Recording& recording = LLTrace::get_frame_recording().getLastRecording();
        F64 normalized_session_jitter = recording.getLastValue(LLStatViewer::NOTRMALIZED_FRAMETIME_JITTER_SESSION);
        F64 normalized_period_jitter = recording.getLastValue(LLStatViewer::NORMALIZED_FRAMTIME_JITTER_PERIOD);
        F32 fps = gRecentFrameCount / FPS_LOG_FREQUENCY;
        LL_INFOS() << llformat("FPS: %.02f SESSION JITTER: %.4f PERIOD JITTER: %.4f", fps, normalized_session_jitter, normalized_period_jitter) << LL_ENDL;
        gRecentFrameCount = 0;
        gRecentFPSTime.reset();
    }
    static LLCachedControl<F32> mem_log_freq(gSavedSettings, "MemoryLogFrequency", 600.f);
    if (mem_log_freq > 0.f && gRecentMemoryTime.getElapsedTimeF32() >= mem_log_freq)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - Memory");
        gMemoryAllocated = U64Bytes(LLMemory::getCurrentRSS());
        U32Megabytes memory = gMemoryAllocated;
        LL_INFOS() << "MEMORY: " << memory << LL_ENDL;
        LLMemory::logMemoryInfo(true) ;
        gRecentMemoryTime.reset();
    }
    constexpr F32 ASSET_STORAGE_LOG_FREQUENCY = 60.f;
    if (gAssetStorageLogTime.getElapsedTimeF32() >= ASSET_STORAGE_LOG_FREQUENCY)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("DS - Asset Storage");
        gAssetStorageLogTime.reset();
        gAssetStorage->logAssetStorageInfo();
    }
}

static void update_tp_display(bool minimized)
{
    static LLCachedControl<F32> teleport_arrival_delay(gSavedSettings, "TeleportArrivalDelay");
    static LLCachedControl<F32> teleport_local_delay(gSavedSettings, "TeleportLocalDelay");

    // <FS:PP> Speed optimisation
    static LLCachedControl<bool> disable_teleport_screens(gSavedSettings, "FSDisableTeleportScreens");
    static LLCachedControl<bool> reset_camera_on_tp(gSavedSettings, "FSResetCameraOnTP");
    static LLCachedControl<LLVector3> nacl_ml_fov_values(gSavedSettings, "_NACL_MLFovValues");
    // </FS:PP>

    S32 attach_count = 0;
    if (isAgentAvatarValid())
    {
        attach_count = gAgentAvatarp->getAttachmentCount();
    }
    F32 teleport_save_time = TELEPORT_EXPIRY + TELEPORT_EXPIRY_PER_ATTACHMENT * attach_count;
    F32 teleport_elapsed = gTeleportDisplayTimer.getElapsedTimeF32();
    F32 teleport_percent = teleport_elapsed * (100.f / teleport_save_time);
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_START && teleport_percent > 100.f)
    {
        // Give up.  Don't keep the UI locked forever.
        LL_WARNS("Teleport") << "Giving up on teleport. elapsed time " << teleport_elapsed << " exceeds max time " << teleport_save_time << LL_ENDL;
        gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
        gAgent.setTeleportMessage(std::string());
    }

    // Make sure the TP progress panel gets hidden in case the viewer window
    // is minimized *during* a TP. HB
    if (minimized)
    {
        gViewerWindow->setShowProgress(false, false);
    }

    const std::string& message = gAgent.getTeleportMessage();
    switch (gAgent.getTeleportState())
    {
        case LLAgent::TELEPORT_PENDING:
        {
            gTeleportDisplayTimer.reset();
            const std::string& msg = LLAgent::sTeleportProgressMessages["pending"];
            if (!minimized)
            {
                // <FS:PP> Speed optimisation
                // gViewerWindow->setShowProgress(true, !gSavedSettings.getBOOL("FSDisableTeleportScreens"));
                gViewerWindow->setShowProgress(true, !disable_teleport_screens());
                // </FS:PP>
                gViewerWindow->setProgressPercent(llmin(teleport_percent, 0.0f));
                gViewerWindow->setProgressString(msg);
            }
            gAgent.setTeleportMessage(msg);
            break;
        }

        case LLAgent::TELEPORT_START:
        {
            // Transition to REQUESTED.  Viewer has sent some kind
            // of TeleportRequest to the source simulator

            // Reset view angle if in mouselook. Fixes camera angle getting stuck on teleport. -Zi
            if (gAgentCamera.cameraMouselook())
            {
                // If someone knows how to call "View.ZoomDefault" by hand, we should do that instead of
                // replicating the behavior here. -Zi
                LLViewerCamera::instance().setDefaultFOV(DEFAULT_FIELD_OF_VIEW);
                // <FS:PP> Speed optimisation
                // if (gSavedSettings.getBOOL("FSResetCameraOnTP"))
                if (reset_camera_on_tp())
                // </FS:PP>
                {
                    gSavedSettings.setF32("CameraAngle", LLViewerCamera::instance().getView()); // FS:LO Dont reset rightclick zoom when we teleport however. Fixes FIRE-6246.
                }
                // also, reset the marker for "currently zooming" in the mouselook zoom settings. -Zi
                // <FS:PP> Speed optimisation
                // LLVector3 vTemp = gSavedSettings.getVector3("_NACL_MLFovValues");
                LLVector3 vTemp = nacl_ml_fov_values();
                // </FS:PP>
                vTemp.mV[VZ] = 0.0f;
                gSavedSettings.setVector3("_NACL_MLFovValues", vTemp);
            }

            gTeleportDisplayTimer.reset();
            const std::string& msg = LLAgent::sTeleportProgressMessages["requesting"];
            LL_INFOS("Teleport") << "A teleport request has been sent, setting state to TELEPORT_REQUESTED" << LL_ENDL;
            gAgent.setTeleportState(LLAgent::TELEPORT_REQUESTED);
            gAgent.setTeleportMessage(msg);
            FSData::instance().selectNextMOTD();
            if (!minimized)
            {
                // <FS:PP> Speed optimisation
                // gViewerWindow->setShowProgress(true, !gSavedSettings.getBOOL("FSDisableTeleportScreens"));
                gViewerWindow->setShowProgress(true, !disable_teleport_screens());
                // </FS:PP>
                gViewerWindow->setProgressPercent(llmin(teleport_percent, 0.0f));
                gViewerWindow->setProgressString(msg);
                gViewerWindow->setProgressMessage(gAgent.mMOTD);
            }
            break;
        }

        case LLAgent::TELEPORT_REQUESTED:
            // Waiting for source simulator to respond
            if (!minimized)
            {
                gViewerWindow->setProgressPercent(llmin(teleport_percent, 37.5f));
                gViewerWindow->setProgressString(message);
            }
            break;

        case LLAgent::TELEPORT_MOVING:
            // Viewer has received destination location from source simulator
            if (!minimized)
            {
                gViewerWindow->setProgressPercent(llmin(teleport_percent, 75.f));
                gViewerWindow->setProgressString(message);
            }
            break;

        case LLAgent::TELEPORT_START_ARRIVAL:
            // Transition to ARRIVING.  Viewer has received avatar update, etc.,
            // from destination simulator
            gTeleportArrivalTimer.reset();
            LL_INFOS("Teleport") << "Changing state to TELEPORT_ARRIVING" << LL_ENDL;
            gAgent.setTeleportState(LLAgent::TELEPORT_ARRIVING);
            gAgent.setTeleportMessage(LLAgent::sTeleportProgressMessages["arriving"]);
            gAgent.sheduleTeleportIM();
            gTextureList.mForceResetTextureStats = true;
            gAgentCamera.resetView(true, true);
            if (!minimized)
            {
                gViewerWindow->setProgressCancelButtonVisible(false, LLTrans::getString("Cancel"));
                gViewerWindow->setProgressPercent(75.f);
            }

            // <FS:Ansariel> FIRE-12004: Attachments getting lost on TP
            gPostTeleportFinishKillObjectDelayTimer.reset();
            break;

        case LLAgent::TELEPORT_ARRIVING:
        // Make the user wait while content "pre-caches"
        {
            F32 arrival_fraction = (gTeleportArrivalTimer.getElapsedTimeF32() / teleport_arrival_delay());
            if (arrival_fraction > 1.f)
            {
                arrival_fraction = 1.f;
                //LLFirstUse::useTeleport();
                LL_INFOS("Teleport") << "arrival_fraction is " << arrival_fraction << " changing state to TELEPORT_NONE" << LL_ENDL;
                gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
            }
            if (!minimized)
            {
                gViewerWindow->setProgressCancelButtonVisible(false, LLTrans::getString("Cancel"));
                gViewerWindow->setProgressPercent(arrival_fraction * 25.f + 75.f);
                gViewerWindow->setProgressString(message);
            }
            break;
        }

        case LLAgent::TELEPORT_LOCAL:
        // Short delay when teleporting in the same sim (progress screen active but not shown - did not
        // fall-through from TELEPORT_START)
        {
            // <FS:CR> FIRE-8721 - Remove local teleport delay
            //if (gTeleportDisplayTimer.getElapsedTimeF32() > teleport_local_delay())
            // </FS:CR>
            {
                //LLFirstUse::useTeleport();
                LL_INFOS("Teleport") << "State is local and gTeleportDisplayTimer " << gTeleportDisplayTimer.getElapsedTimeF32()
                                     << " exceeds teleport_local_delete " << teleport_local_delay
                                     << "; setting state to TELEPORT_NONE"
                                     << LL_ENDL;
                gAgent.setTeleportState(LLAgent::TELEPORT_NONE);
            }
            break;
        }

        case LLAgent::TELEPORT_NONE:
            // No teleport in progress
            gViewerWindow->setShowProgress(false, false);
            gTeleportDisplay = false;
    }
}

// Paint the display!
U32 gEarlySortResorts = 0;     // <FS> frames in which the early sort had to be redone (see LLBuilderGuard); logged with the texture list line

void display(bool rebuild, F32 zoom_factor, int subfield, bool for_snapshot)
{
    // <FS> per-zone GPU timing in the log (only does anything while FSGpuZoneTimers is on).
    // Done first, before any zone of this frame opens, so the previous frame is complete.
    LLGpuZoneTimer::onFrameEnd();

    // <FS> A/B of the early sort inside one run (LL_VK_EARLY_SORT=alt: 60 frames on, 60 off): the interval between two display() calls, summed per mode of the frame it ended
    static int sFrameMode = -1;
    static std::chrono::steady_clock::time_point sLastDisplay;
    static double sModeMs[2] = {0, 0};
    static U32 sModeFrames[2] = {0, 0};
    {
        const auto now = std::chrono::steady_clock::now();
        if (sFrameMode >= 0)
        {
            sModeMs[sFrameMode] += std::chrono::duration<double, std::milli>(now - sLastDisplay).count();
            ++sModeFrames[sFrameMode];
            if ((sModeFrames[0] + sModeFrames[1]) % 1200 == 0 && sModeFrames[0] && sModeFrames[1])
            {
                LL_INFOS("VkBuilder") << "early sort A/B: frame interval with early sort " << sModeMs[1] / sModeFrames[1] << " ms over " << sModeFrames[1] << " frames, without "
                    << sModeMs[0] / sModeFrames[0] << " ms over " << sModeFrames[0] << " frames; early sort redone in " << gEarlySortResorts << " frames" << LL_ENDL;
            }
        }
        sLastDisplay = now;
    }

    // <FS> Vulkan mirror frame (does nothing unless LL_VK_MIRROR is set)
    if (gViewerWindow)
    {
        LLVkBridge::beginFrame(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
    }


    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Render");
    LL_PROFILE_GPU_ZONE("Render");

    LLPerfStats::RecordSceneTime T (LLPerfStats::StatType_t::RENDER_DISPLAY); // render time capture - This is the main stat for overall rendering.

    LLViewerCamera& camera = LLViewerCamera::instance(); // <FS:Ansariel> Factor out calls to getInstance

    if (gWindowResized)
    { //skip render on frames where window has been resized
        LL_DEBUGS("Window") << "Resizing window" << LL_ENDL;
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Resize Window");
        gRender.flush();
        gRender.clearBuffers(ClearBits::Color);
        LLVkBridge::endFrame(); // <FS>
        gViewerWindow->getWindow()->swapBuffers();
        LLPipeline::refreshCachedSettings();
        gPipeline.resizeScreenTexture();
        gResizeScreenTexture = false;
        gWindowResized = false;
        return;
    }

    if (gResizeShadowTexture)
    { //skip render on frames where window has been resized
        gPipeline.resizeShadowTexture();
        // gResizeShadowTexture = false; // <FS:Beq/> This prevents the deferred resize from working properly.
    }

    gSnapshot = for_snapshot;

    if (LLPipeline::sRenderDeferred)
    { //hack to make sky show up in deferred snapshots
        for_snapshot = false;
    }

    LLRenderPresetDefault gls_default;
    LLRenderState::DepthScope gls_depth(true, true, LLRenderState::CompareOp::LessEqual);

    LLVertexBuffer::unbind();


    gPipeline.disableLights();

    // Don't draw if the window is hidden or minimized.
    // In fact, must explicitly check the minimized state before drawing.
    // Attempting to draw into a minimized window causes a GL error. JC
    if (   !gViewerWindow->getActive()
        || !gViewerWindow->getWindow()->getVisible()
        || gViewerWindow->getWindow()->getMinimized()
        || gNonInteractive)
    {
        // Clean up memory the pools may have allocated
        if (rebuild)
        {
            { LL_CPU_ZONE("display: rebuildPools"); gPipeline.rebuildPools(); }
        }

        // <FS:ND> FIRE-15789; Make sure there's not backlog for thousands and thousands of beam objects
        LLHUDObject::renderAllForTimer();
        // </FS:ND>

        gViewerWindow->returnEmptyPicks();

        // We still need to update the teleport progress (to get changes done
        // in TP states, else the sim does not get the messages signaling the
        // agent's arrival). This fixes BUG-230616. HB
        if (gTeleportDisplay)
        {
            // true = minimized, do not show/update the TP screen. HB
            update_tp_display(true);
        }

        // Run texture subsystem to discard memory while backgrounded
        if (!gNonInteractive)
        {
            LL_PROFILE_ZONE_NAMED("Update Images");

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Class");
                LLViewerTexture::updateClass();
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Image Update Bump");
                { LL_CPU_ZONE("display: bump image list"); gBumpImageList.updateImages(); }  // must be called before gTextureList version so that it's textures are thrown out first.
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("List");
                F32 max_image_decode_time = 0.050f * gFrameIntervalSeconds.value();          // 50 ms/second decode time
                max_image_decode_time     = llclamp(max_image_decode_time, 0.002f, 0.005f);  // min 2ms/frame, max 5ms/frame)
                { LL_CPU_ZONE("display: gTextureList.updateImages"); gTextureList.updateImages(max_image_decode_time); }
                LLImageTex::flushDirtyTexOptions(); // <FS> the sampler options that changed are sent here, not by whichever bind comes first
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("GLTF Materials Cleanup");
                // remove dead gltf materials
                gGLTFMaterialList.flushMaterials();
            }
        }
        return;
    }

    gViewerWindow->checkSettings();

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Picking");
        gViewerWindow->performPick();
    }

    LLAppViewer::instance()->pingMainloopTimeout("Display:CheckStates");

    //////////////////////////////////////////////////////////
    //
    // Logic for forcing window updates if we're in drone mode.
    //

    // *TODO: Investigate running display() during gHeadlessClient.  See if this early exit is needed DK 2011-02-18
    if (gHeadlessClient)
    {
#if LL_WINDOWS
        static F32 last_update_time = 0.f;
        if ((gFrameTimeSeconds - last_update_time) > 1.f)
        {
            InvalidateRect((HWND)gViewerWindow->getPlatformWindow(), NULL, false);
            last_update_time = gFrameTimeSeconds;
        }
#elif LL_DARWIN
        // MBW -- Do something clever here.
#endif
        // Not actually rendering, don't bother.
        return;
    }


    //
    // Bail out if we're in the startup state and don't want to try to
    // render the world.
    //
    // <FS:Ansariel> Revert to original state to prevent flickering if login progress screen is disabled
    //if (LLStartUp::getStartupState() < STATE_PRECACHE)
    if (LLStartUp::getStartupState() < STATE_STARTED)
    // </FS:Ansariel>
    {
        LLAppViewer::instance()->pingMainloopTimeout("Display:Startup");
        display_startup();
        return;
    }


    if (gShaderProfileFrame)
    {
        LLShaderProgram::initProfile();
    }

    //LLGLState::verify(false);

    /////////////////////////////////////////////////
    //
    // Update GL Texture statistics (used for discard logic?)
    //

    LLAppViewer::instance()->pingMainloopTimeout("Display:TextureStats");

    LLImageTex::updateStats(gFrameTimeSeconds);

    static LLCachedControl<S32> avatar_name_tag_mode(gSavedSettings, "AvatarNameTagMode", 1);
    static LLCachedControl<S32> name_tag_show_group_titles(gSavedSettings, "GroupTitlesTagMode", 2 /*all group tags*/);
// <FS:CR> Aurora sim
    //LLVOAvatar::sRenderName = avatar_name_tag_mode;
    //LLVOAvatar::sRenderGroupTitles = avatar_name_tag_mode > 0 ? name_tag_show_group_titles : 0;;
    auto& world_instance = LLWorld::instance();
    LLVOAvatar::sRenderName = avatar_name_tag_mode > world_instance.getAllowRenderName() ? world_instance.getAllowRenderName() : avatar_name_tag_mode;
    LLVOAvatar::sRenderGroupTitles = LLVOAvatar::sRenderName > 0 ? name_tag_show_group_titles : 0;
// <FS:CR> Aurora sim

    gPipeline.mBackfaceCull = true;
    gFrameCount++;
    gRecentFrameCount++;
    if (gFocusMgr.getAppHasFocus())
    {
        gForegroundFrameCount++;
    }

    //////////////////////////////////////////////////////////
    //
    // Display start screen if we're teleporting, and skip render
    //

    if (gTeleportDisplay)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Teleport Display");
        LLAppViewer::instance()->pingMainloopTimeout("Display:Teleport");
        // Note: false = not minimized, do update the TP screen. HB
        update_tp_display(false);
    }
    else if(LLAppViewer::instance()->logoutRequestSent())
    {
        LLAppViewer::instance()->pingMainloopTimeout("Display:Logout");
        F32 percent_done = gLogoutTimer.getElapsedTimeF32() * 100.f / gLogoutMaxTime;
        if (percent_done > 100.f)
        {
            percent_done = 100.f;
        }

        if( LLApp::isExiting() )
        {
            percent_done = 100.f;
        }

        gViewerWindow->setProgressPercent( percent_done );
        gViewerWindow->setProgressMessage(std::string());
    }
    else
    if (gRestoreResources)
    {
        LLAppViewer::instance()->pingMainloopTimeout("Display:RestoreGL");
        F32 percent_done = gRestoreResourcesTimer.getElapsedTimeF32() * 100.f / RESTORE_GL_TIME;
        if( percent_done > 100.f )
        {
            gViewerWindow->setShowProgress(false,false);
            gRestoreResources = false;
        }
        else
        {

            if( LLApp::isExiting() )
            {
                percent_done = 100.f;
            }

            gViewerWindow->setProgressPercent( percent_done );
        }
        gViewerWindow->setProgressMessage(std::string());
    }

    // <FS::Ansariel> Draw Distance stepping; originally based on SpeedRez by Henri Beauchamp, licensed under LGPL
    // Progressively increase draw distance after TP when required.
    static LLCachedControl<F32> renderFarClip(gSavedSettings, "RenderFarClip");
    if (gSavedDrawDistance > 0.0f && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
    {
        if (gLastDrawDistanceStep != renderFarClip())
        {
            LLPresetsManager::instance().setIsDrawDistanceSteppingActive(false);
            gSavedDrawDistance = 0.0f;
            gLastDrawDistanceStep = 0.0f;
            gSavedSettings.setF32("FSSavedRenderFarClip", 0.0f);
        }

        if (gTeleportArrivalTimer.getElapsedTimeF32() >=
            (F32)gSavedSettings.getU32("FSRenderFarClipSteppingInterval"))
        {
            gTeleportArrivalTimer.reset();
            F32 current = gSavedSettings.getF32("RenderFarClip");
            if (gSavedDrawDistance > current)
            {
                current *= 2.0f;
                if (current > gSavedDrawDistance)
                {
                    current = gSavedDrawDistance;
                }
                gSavedSettings.setF32("RenderFarClip", current);
                gLastDrawDistanceStep = current;
            }
            if (current >= gSavedDrawDistance)
            {
                LLPresetsManager::instance().setIsDrawDistanceSteppingActive(false);
                gSavedDrawDistance = 0.0f;
                gLastDrawDistanceStep = 0.0f;
                gSavedSettings.setF32("FSSavedRenderFarClip", 0.0f);
            }
        }
    }
    // </FS::Ansariel>

    //////////////////////////
    //
    // Prepare for the next frame
    //

    /////////////////////////////
    //
    // Update the camera
    //
    //

    LLAppViewer::instance()->pingMainloopTimeout("Display:Camera");
    if (LLViewerCamera::instanceExists())
    {
        LLViewerCamera::getInstance()->setZoomParameters(zoom_factor, subfield);
        LLViewerCamera::getInstance()->setNear(MIN_NEAR_PLANE);
    }

    //////////////////////////
    //
    // clear the next buffer
    // (must follow dynamic texture writing since that uses the frame buffer)
    //

    if (gDisconnected)
    {
        LLAppViewer::instance()->pingMainloopTimeout("Display:Disconnected");
        { LL_CPU_ZONE("display: render_ui"); render_ui(); }
        { LL_CPU_ZONE("display: swap"); swap(); }
    }

    //////////////////////////
    //
    // Set rendering options
    //
    //
    LLAppViewer::instance()->pingMainloopTimeout("Display:RenderSetup");

    ///////////////////////////////////////
    //
    // Slam lighting parameters back to our defaults.
    // Note that these are not the same as GL defaults...

    gRender.setAmbientLightColor(LLColor4::white);

    /////////////////////////////////////
    //
    // Render
    //
    // Actually push all of our triangles to the screen.
    //

    // do render-to-texture stuff here
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_DYNAMIC_TEXTURES))
    {
        LLAppViewer::instance()->pingMainloopTimeout("Display:DynamicTextures");
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Update Dynamic Textures");
        if (LLViewerDynamicTexture::updateAllInstances())
        {
            gRender.setColorMask(true, true);
            gRender.clearBuffers(ClearBits::Depth);
        }
    }

    gViewerWindow->setup3DViewport();

    gPipeline.resetFrameStats();    // Reset per-frame statistics.

    if (!gDisconnected && !LLApp::isExiting())
    {
        // Render mirrors and associated hero probes before we render the rest of the scene.
        // This ensures the scene state in the hero probes are exactly the same as the rest of the scene before we render it.
        if (gPipeline.RenderMirrors && !gSnapshot)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Update hero probes");
            LL_PROFILE_GPU_ZONE("hero manager")
            { LL_CPU_ZONE("hero: update"); gPipeline.mHeroProbeManager.update(); }
            { LL_CPU_ZONE("hero: renderProbes"); gPipeline.mHeroProbeManager.renderProbes(); }
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("display - 1");
        LLAppViewer::instance()->pingMainloopTimeout("Display:Update");
        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD))
        { //don't draw hud objects in this frame
            gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
        }

        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES))
        { //don't draw hud particles in this frame
            gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);
        }

        { LL_CPU_ZONE("display: display_update_camera"); display_update_camera(); }

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Env Update");
            // update all the sky/atmospheric/water settings
            LLEnvironment::instance().update(&camera); // <FS:Ansariel> Factor out calls to getInstance
        }

        // *TODO: merge these two methods
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("HUD Update");
            LLHUDManager::getInstance()->updateEffects();
            { LL_CPU_ZONE("display: HUD objects update"); LLHUDObject::updateAll(); }
        }

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Update Geom");
            const F32 max_geom_update_time = 0.005f*10.f*gFrameIntervalSeconds.value(); // 50 ms/second update time
            {
                LL_CPU_ZONE("display: createObjects");
                gPipeline.createObjects(max_geom_update_time);
            }
            { LL_CPU_ZONE("display: processPartitionQ"); gPipeline.processPartitionQ(); }
            {
                LL_CPU_ZONE("display: updateGeom");
                gPipeline.updateGeom(max_geom_update_time);
            }
            {
                // <FS> an animated object has a skeleton of its own that is only fully updated every few frames (its update period), but its root follows the object every frame (LLControlAvatar::matchVolumeTransform, run by the
                // drawable's update just above): all its joints are dirty from then on, and the first thread that asks for a joint matrix recomputes it and writes it. The palette job on the pool and the main thread ask
                // at the same time: two threads writing one skeleton's matrices tear them, which showed as a few triangles of that attachment (and only that one) jumping. Cleaned here, on this thread, once the roots have moved.
                LL_CPU_ZONE("display: clean animated object skeletons");
                for (LLCharacter* character : LLCharacter::sInstances)
                {
                    LLControlAvatar* cav = dynamic_cast<LLControlAvatar*>(character);
                    if (cav && !cav->isDead())
                    {
                        cav->cleanJointMatrices();
                    }
                }
            }
        }

        { LL_CPU_ZONE("display: updateResources"); gPipeline.updateResources(); }


        LLAppViewer::instance()->pingMainloopTimeout("Display:Cull");

        //Increment drawable frame counter
        LLDrawable::incrementVisible();

        LLSpatialGroup::sNoDelete = true;
        LLTexUnit::sWhiteTexture = LLViewerFetchedTexture::sWhiteImagep->getTexName();

        S32 occlusion = LLPipeline::sUseOcclusion;
        if (gDepthDirty)
        { //depth buffer is invalid, don't overwrite occlusion state
            LLPipeline::sUseOcclusion = llmin(occlusion, 1);
        }
        gDepthDirty = false;


        static LLCullResult result;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        LLPipeline::sUnderWaterRender = LLViewerCamera::getInstance()->cameraUnderWater();
        LLRenderPass::finishGeometryBuilds();       // <FS> builds of the last frame that nobody took must be done before the cull result is cleared
        LLBuilderGuard::sMainMapLive = false;
        {
            LL_CPU_ZONE("display: updateCull");
            gPipeline.updateCull(*LLViewerCamera::getInstance(), result);
        }

        // <FS> The sort of the main view runs before the shadow passes (and the impostors): what it builds (the render map, the rebuilt groups) does not depend on them, and the packet
        // builders of the deferred passes can start on the async pool while the main thread draws the shadows. The pool face lists, which the shadow passes reset and refill for
        // themselves, are filled again after them (refillPools). LL_VK_EARLY_SORT=0 puts it back where it was.
        static const char* early_env = getenv("LL_VK_EARLY_SORT");
        static const bool early_alternate = early_env && early_env[0] == 'a';
        static const bool early_env_enabled = !(early_env && early_env[0] == '0');
        static LLCachedControl<bool> early_sort_setting(gSavedSettings, "FSEarlySort", true);        // live: Debug Settings
        static LLCachedControl<bool> async_builders_setting(gSavedSettings, "FSAsyncBuilders", true);
        LLAsyncPool::sRunInline = !async_builders_setting;
        const bool early_sort = early_env_enabled && early_sort_setting && !for_snapshot && gFrameCount > 1 && (!early_alternate || ((gFrameCount / 60) % 2) == 1);
        sFrameMode = early_sort ? 1 : 0;
        unsigned early_epoch = 0;
        if (early_sort)
        {
            LL_CPU_ZONE("display: stateSort (early)");
            gPipeline.stateSort(camera, result, true);
            early_epoch = LLBuilderGuard::sEpoch.load();
            LLBuilderGuard::sMainMapLive = true;        // from here a change to a group of the main view is noticed (see LLBuilderGuard)
            {
                LL_CPU_ZONE("display: start geometry builds (early)");
                LLDrawPoolMaterials::kickGeometryBuilds();
            }
        }


        LLAppViewer::instance()->pingMainloopTimeout("Display:Swap");

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("display - 2")
            if (gResizeScreenTexture)
            {
                gPipeline.resizeScreenTexture();
                gResizeScreenTexture = false;
            }

            gRender.setColorMask(true, true);
            gRender.setClearColor(0.f, 0.f, 0.f, 0.f);


            if (!for_snapshot)
            {
                if (gFrameCount > 1 && !for_snapshot)
                { //for some reason, ATI 4800 series will error out if you
                  //try to generate a shadow before the first frame is through
                    LL_CPU_ZONE("display: generateSunShadow");
                    gPipeline.generateSunShadow(*LLViewerCamera::getInstance());
                }

                LLVertexBuffer::unbind();


                glm::mat4 proj = get_current_projection();
                glm::mat4 mod = get_current_modelview();
                gRender.setViewport(0,0,512,512);

                LLVOAvatar::updateImpostors();

                set_current_projection(proj);
                set_current_modelview(mod);
                LLRenderMatrix::load(LLRenderMatrix::Kind::Projection, glm::value_ptr(proj));
                LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, glm::value_ptr(mod));
                gViewerWindow->setup3DViewport();

            }
            gRender.clearBuffers(ClearBits::Depth);
        }

        //////////////////////////////////////
        //
        // Update images, using the image stats generated during object update/culling
        //
        // Can put objects onto the retextured list.
        //
        // Doing this here gives hardware occlusion queries extra time to complete
        LLAppViewer::instance()->pingMainloopTimeout("Display:UpdateImages");

        {
            LL_PROFILE_ZONE_NAMED("Update Images");

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Class");
                LLViewerTexture::updateClass();
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Image Update Bump");
                { LL_CPU_ZONE("display: bump image list"); gBumpImageList.updateImages(); }  // must be called before gTextureList version so that it's textures are thrown out first.
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("List");
                F32 max_image_decode_time = 0.050f*gFrameIntervalSeconds.value(); // 50 ms/second decode time
                max_image_decode_time = llclamp(max_image_decode_time, 0.002f, 0.005f ); // min 2ms/frame, max 5ms/frame)
                { LL_CPU_ZONE("display: gTextureList.updateImages"); gTextureList.updateImages(max_image_decode_time); }
                LLImageTex::flushDirtyTexOptions(); // <FS> the sampler options that changed are sent here, not by whichever bind comes first
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("GLTF Materials Cleanup");
                //remove dead gltf materials
                gGLTFMaterialList.flushMaterials();
            }
        }


        ///////////////////////////////////
        //
        // StateSort
        //
        // Responsible for taking visible objects, and adding them to the appropriate draw orders.
        // In the case of alpha objects, z-sorts them first.
        // Also creates special lists for outlines and selected face rendering.
        //
        LLAppViewer::instance()->pingMainloopTimeout("Display:StateSort");
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("display - 4")
            LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
            if (early_sort && LLBuilderGuard::sEpoch.load() == early_epoch)
            {
                gPipeline.refillPools(result);
            }
            else if (early_sort)
            {
                // a group of the main view was rebuilt after its render map was made (by the shadow preparation or an impostor): the map may point to freed draw infos. Sort again, as the
                // frame would have without the early sort; the builds started for the old map are dropped.
                if (++gEarlySortResorts <= 5)
                {
                    LL_INFOS("VkBuilder") << "early sort: a group of the main view changed after the sort (" << gEarlySortResorts << " frames so far): sorting again" << LL_ENDL;
                }
                LLRenderPass::finishGeometryBuilds();
                LLBuilderGuard::sMainMapLive = false;
                gPipeline.dropOcclusionMarks(result);       // the groups marked for a query by the first cull would never be queried again
                LL_CPU_ZONE("display: cull + stateSort (again)");
                LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
                gPipeline.updateCull(camera, result);       // the sort pushes into the cull result as well (bridges, drawables): it starts from a clean one
                gPipeline.stateSort(camera, result);
            }
            else
            {
                LL_CPU_ZONE("display: stateSort");
                gPipeline.stateSort(camera, result); // <FS:Ansariel> Factor out calls to getInstance
            }

            if (rebuild)
            {
                //////////////////////////////////////
                //
                // rebuildPools
                //
                //
                { LL_CPU_ZONE("display: rebuildPools"); gPipeline.rebuildPools(); }
            }
        }

        {
            // <FS> debug (LL_VK_VISDIP=1): what the main view's cull left each frame (visible groups and drawables), the frames where either fell below 80 % of the median of the window are listed: an object
            // that blinks out of the scene shows as a dip here (the draw path is not involved: the cull has not drawn anything yet)
            static const bool wanted = getenv("LL_VK_VISDIP") != nullptr;
            if (wanted && !for_snapshot)
            {
                // the groups of the last frame and what they were, to say which ones left at the start of a dip
                static std::unordered_map<LLSpatialGroup*, std::array<U32, 3> > last_groups;
                static std::unordered_map<LLSpatialGroup*, std::string> last_descr;
                static bool last_ok = true;
                {
                    std::unordered_map<LLSpatialGroup*, std::array<U32, 3> > now_groups;
                    std::unordered_map<LLSpatialGroup*, std::string> now_descr;
                    for (LLCullResult::sg_iterator it = result.beginVisibleGroups(); it != result.endVisibleGroups(); ++it)
                    {
                        LLSpatialGroup* g = *it;
                        if (g && !g->isDead())
                        {
                            now_groups[g] = { (U32)g->getSpatialPartition()->mDrawableType, (U32)g->getElementCount(), (U32)g->isOcclusionState(LLSpatialGroup::OCCLUDED) };
                            {
                                std::string descr = "empty";
                                for (LLSpatialGroup::element_iter e = g->getDataBegin(); e != g->getDataEnd(); ++e)
                                {
                                    LLDrawable* dr = (LLDrawable*)(*e)->getDrawable();
                                    LLViewerObject* vo = dr ? dr->getVObj().get() : nullptr;
                                    if (vo)
                                    {
                                        descr = llformat("pcode %u%s%s at (%.1f %.1f %.1f) scale (%.2f %.2f %.2f) id %s", (U32)vo->getPCode(), vo->isAttachment() ? " attached" : "", vo->isAvatar() ? " avatar" : "",
                                            vo->getPositionAgent().mV[0], vo->getPositionAgent().mV[1], vo->getPositionAgent().mV[2], vo->getScale().mV[0], vo->getScale().mV[1], vo->getScale().mV[2], vo->getID().asString().substr(0, 8).c_str());
                                        break;
                                    }
                                }
                                now_descr[g] = descr;
                            }
                        }
                    }
                    const bool ok = now_groups.size() * 10 >= last_groups.size() * 8;
                    if (last_ok && !ok && !last_groups.empty())
                    {
                        std::map<U32, U32> by_type;
                        U32 occluded = 0, empty = 0, listed = 0;
                        std::string some;
                        for (const auto& kv : last_groups)
                        {
                            if (!now_groups.count(kv.first))
                            {
                                ++by_type[kv.second[0]];
                                occluded += kv.first->isDead() ? 0 : (U32)kv.first->isOcclusionState(LLSpatialGroup::OCCLUDED);
                                empty += kv.second[1] == 0;
                                if (++listed <= 8)
                                {
                                    auto di = last_descr.find(kv.first);
                                    some += " [" + (di != last_descr.end() ? di->second : std::string("?")) + "]";
                                }
                            }
                        }
                        std::string types;
                        for (const auto& kv : by_type) types += llformat(" type%u:%u", kv.first, kv.second);
                        LL_INFOS("VisDip") << "dip starts at frame " << gFrameCount << ": " << last_groups.size() << " groups before, " << now_groups.size() << " now; left by partition type:" << types << "; now OCCLUDED " << occluded << " of " << listed << ", empty " << empty << some << LL_ENDL;
                    }
                    // at the start of a dip: the groups that left are remembered; at its end it is counted how many of them are back (the same group objects) and how many are dead
                    static std::vector<LLSpatialGroup*> missing;
                    static U32 dip_frame = 0;
                    if (last_ok && !ok && !last_groups.empty())
                    {
                        missing.clear();
                        dip_frame = gFrameCount;
                        for (const auto& kv : last_groups)
                        {
                            if (!now_groups.count(kv.first)) missing.push_back(kv.first);
                        }
                    }
                    else if (!last_ok && ok && !missing.empty())
                    {
                        U32 back = 0, dead = 0;
                        for (LLSpatialGroup* g : missing)
                        {
                            back += (U32)now_groups.count(g);
                            dead += g->isDead() ? 1u : 0u;
                        }
                        LL_INFOS("VisDip") << "dip over after " << (gFrameCount - dip_frame) << " frames: " << back << " of the " << missing.size() << " groups that left are visible again, " << dead << " of them dead" << LL_ENDL;
                        missing.clear();
                    }
                    // a flicker: a live group that was visible, is not for a few frames and is visible again (what an object that blinks out of the scene looks like in the cull)
                    {
                        static std::unordered_map<LLSpatialGroup*, std::pair<U32, std::string> > absent;
                        static U32 flickers = 0, flicker_frames = 0, window_start = 0;
                        static std::string samples;
                        for (const auto& kv : last_groups)
                        {
                            if (!now_groups.count(kv.first) && !kv.first->isDead())
                            {
                                auto di = last_descr.find(kv.first);
                                absent[kv.first] = { (U32)gFrameCount, di != last_descr.end() ? di->second : std::string("?") };
                            }
                        }
                        for (const auto& kv : now_groups)
                        {
                            auto it = absent.find(kv.first);
                            if (it != absent.end())
                            {
                                const U32 len = (U32)gFrameCount - it->second.first;
                                if (len <= 30)
                                {
                                    ++flickers;
                                    flicker_frames += len;
                                    if (samples.size() < 900)
                                    {
                                        samples += llformat(" [%u frames at %u: %s]", len, it->second.first, it->second.second.c_str());
                                    }
                                }
                                absent.erase(it);
                            }
                        }
                        for (auto it = absent.begin(); it != absent.end(); )
                        {
                            it = (it->first->isDead() || (U32)gFrameCount - it->second.first > 30) ? absent.erase(it) : std::next(it);
                        }
                        if (!window_start) window_start = (U32)gFrameCount;
                        if ((U32)gFrameCount - window_start >= 600)
                        {
                            LL_INFOS("VisDip") << "flickers (a live group out of the visible list for up to 30 frames) in " << ((U32)gFrameCount - window_start) << " frames: " << flickers << " (" << flicker_frames << " group-frames)" << samples << LL_ENDL;
                            flickers = flicker_frames = 0;
                            samples.clear();
                            window_start = (U32)gFrameCount;
                        }
                    }
                    last_ok = ok;
                    last_groups.swap(now_groups);
                    last_descr.swap(now_descr);
                }
                static std::vector<std::array<U32, 3> > window;
                window.push_back({ (U32)gFrameCount, result.getVisibleGroupsSize(), result.getVisibleListSize() });
                if (window.size() >= 600)
                {
                    std::vector<U32> g, l;
                    for (const auto& w : window) { g.push_back(w[1]); l.push_back(w[2]); }
                    std::sort(g.begin(), g.end());
                    std::sort(l.begin(), l.end());
                    const U32 gm = g[g.size() / 2], lm = l[l.size() / 2];
                    std::string dips;
                    U32 count = 0;
                    for (const auto& w : window)
                    {
                        if (w[1] < gm * 8 / 10 || w[2] < lm * 8 / 10)
                        {
                            if (++count <= 8)
                            {
                                dips += llformat(" [frame %u groups %u drawables %u]", w[0], w[1], w[2]);
                            }
                        }
                    }
                    LL_INFOS("VisDip") << "visible groups min/median/max " << g.front() << "/" << gm << "/" << g.back() << ", drawables " << l.front() << "/" << lm << "/" << l.back() << " over " << window.size() << " frames; frames below 80 % of the median: " << count << dips << LL_ENDL;
                    window.clear();
                }
            }
        }

        LLSceneMonitor::getInstance()->fetchQueryResult();


        LLPipeline::sUseOcclusion = occlusion;

        {
            LLAppViewer::instance()->pingMainloopTimeout("Display:Sky");
            LL_PROFILE_ZONE_NAMED_CATEGORY_ENVIRONMENT("update sky"); //LL_RECORD_BLOCK_TIME(FTM_UPDATE_SKY);
            { LL_CPU_ZONE("display: updateSky"); gSky.updateSky(); }
        }

        if(gUseWireframe)
        {
            gRender.setClearColor(0.5f, 0.5f, 0.5f, 0.f);
            gRender.clearBuffers(ClearBits::Color);
        }

        LLAppViewer::instance()->pingMainloopTimeout("Display:RenderStart");

        //// render frontmost floater opaque for occlusion culling purposes
        //LLFloater* frontmost_floaterp = gFloaterView->getFrontmost();
        //// assumes frontmost floater with focus is opaque
        //if (frontmost_floaterp && gFocusMgr.childHasKeyboardFocus(frontmost_floaterp))
        //{
        //  gRender.matrixMode(LLRender::MM_MODELVIEW);
        //  gRender.pushMatrix();
        //  {
        //      gRender.getTexUnit(0)->bindWhite();

        //      glColorMask(false, false, false, true);
        //      gRender.loadIdentity();

        //      LLRect floater_rect = frontmost_floaterp->calcScreenRect();
        //      // deflate by one pixel so rounding errors don't occlude outside of floater extents
        //      floater_rect.stretch(-1);
        //      LLRectf floater_3d_rect((F32)floater_rect.mLeft / (F32)gViewerWindow->getWindowWidthScaled(),
        //                              (F32)floater_rect.mTop / (F32)gViewerWindow->getWindowHeightScaled(),
        //                              (F32)floater_rect.mRight / (F32)gViewerWindow->getWindowWidthScaled(),
        //                              (F32)floater_rect.mBottom / (F32)gViewerWindow->getWindowHeightScaled());
        //      floater_3d_rect.translate(-0.5f, -0.5f);
        //      gRender.translatef(0.f, 0.f, -LLViewerCamera::getInstance()->getNear());
        //      gRender.scalef(LLViewerCamera::getInstance()->getNear() * LLViewerCamera::getInstance()->getAspect() / sinf(LLViewerCamera::getInstance()->getView()), LLViewerCamera::getInstance()->getNear() / sinf(LLViewerCamera::getInstance()->getView()), 1.f);
        //      gRender.color4fv(LLColor4::white.mV);
        //      gRender.begin(LLVertexBuffer::QUADS);
        //      {
        //          gRender.vertex3f(floater_3d_rect.mLeft, floater_3d_rect.mBottom, 0.f);
        //          gRender.vertex3f(floater_3d_rect.mLeft, floater_3d_rect.mTop, 0.f);
        //          gRender.vertex3f(floater_3d_rect.mRight, floater_3d_rect.mTop, 0.f);
        //          gRender.vertex3f(floater_3d_rect.mRight, floater_3d_rect.mBottom, 0.f);
        //      }
        //      gRender.end();
        //      glColorMask(true, true, true, true);
        //  }
        //  gRender.popMatrix();
        //}

        LLPipeline::sUnderWaterRender = LLViewerCamera::getInstance()->cameraUnderWater();

// <FS:CR> Aurora Sim
        if (!LLWorld::getInstance()->getAllowRenderWater())
        {
            LLPipeline::sUnderWaterRender = false;
        }
// </FS:CR> Aurora Sim


        gRender.setColorMask(true, true);

        gPipeline.rtSceneEarly(1);      // <FS> LL_VK_RT_EARLY=1: the ray scene is built here (before the world pass) when the switch is on

        RenderPassScope world_pass;     // F3.5b: the world pass on the deferred screen target: begun here, ended after the render flush below
        {
        LL_CPU_ZONE("display: deferredScreen bind + clear");
        world_pass.begin(gPipeline.mRT->deferredScreen, gUseWireframe ? RenderPass::Clear(0.5f, 0.5f, 0.5f, 1.f) : RenderPass::Clear(1, 0, 1, 1));
        }

        gRender.setColorMask(true, false);

        LLAppViewer::instance()->pingMainloopTimeout("Display:RenderGeom");

        if (!(LLAppViewer::instance()->logoutRequestSent() && LLAppViewer::instance()->hasSavedFinalSnapshot())
                && !gRestoreResources)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("display - 5")
            LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

            static LLCachedControl<bool> render_depth_pre_pass(gSavedSettings, "RenderDepthPrePass", false);
            if (render_depth_pre_pass)
            {
                gRender.setColorMask(false, false);

                constexpr U32 types[] = {
                    LLRenderPass::PASS_SIMPLE,
                    LLRenderPass::PASS_FULLBRIGHT,
                    LLRenderPass::PASS_SHINY
                };

                U32 num_types = LL_ARRAY_SIZE(types);
                gOcclusionProgram.bind();
                for (U32 i = 0; i < num_types; i++)
                {
                    gPipeline.renderObjects(types[i], LLVertexBuffer::MAP_VERTEX, false);
                }

                gOcclusionProgram.unbind();

            }

            gRender.setColorMask(true, true);
            {
                LL_CPU_ZONE("display: renderGeomDeferred");
                gPipeline.beginAvatarVelocity();        // <FS> avatar motion vectors: starts recording the body meshes the G-buffer draws
                gPipeline.renderGeomDeferred(*LLViewerCamera::getInstance(), true);
            }
            LLBuilderGuard::sMainMapLive = false;
        }

        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Texture Unbind");
            LL_CPU_ZONE("display: texture unbind");
            static U32 sUnbindLogged = 0;
            U32 unbound = 0;
            for (S32 i = 0; i < gDeviceCaps.mNumTextureImageUnits; i++)
            { //dummy cleanup of any currently bound textures
                if (gRender.getTexUnit(i)->getCurrType() != LLTexUnit::TT_NONE)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    gRender.getTexUnit(i)->unbind(gRender.getTexUnit(i)->getCurrType());
                    const auto t1 = std::chrono::steady_clock::now();
                    gRender.getTexUnit(i)->clear();
                    const auto t2 = std::chrono::steady_clock::now();
                    ++unbound;
                    const auto us_unbind = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                    const auto us_disable = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
                    if ((us_unbind > 100 || us_disable > 100) && sUnbindLogged < 24)
                    {
                        ++sUnbindLogged;
                        LL_INFOS("DisplayStall") << "texture unit " << i << ": unbind " << us_unbind << " us, disable " << us_disable << " us (units " << gDeviceCaps.mNumTextureImageUnits << ")" << LL_ENDL;
                    }
                }
            }
            static U32 sUnbindReports = 0;
            if (sUnbindReports < 3 && (++sUnbindReports == 1 || (gFrameCount % 600) == 0))
            {
                LL_INFOS("DisplayStall") << "texture unbind loop: " << unbound << " units were bound, loop over " << gDeviceCaps.mNumTextureImageUnits << " units" << LL_ENDL;
            }
        }

        LLAppViewer::instance()->pingMainloopTimeout("Display:RenderFlush");

        LLRenderTarget &rt = (gPipeline.sRenderDeferred ? gPipeline.mRT->deferredScreen : gPipeline.mRT->screen);
        (void)rt;
        { LL_CPU_ZONE("display: deferredScreen flush"); world_pass.end(); }
        gPipeline.rtSceneEarly(2);      // <FS> LL_VK_RT_EARLY=2: after the G-buffer (diagnostic)

        if (LLPipeline::sRenderDeferred)
        {
            gPipeline.renderAvatarVelocity();       // <FS> avatar motion vectors: after the G-buffer, before the lighting (does nothing when it was not asked for)
            LL_CPU_ZONE("display: renderDeferredLighting (incl. post-deferred)");
            gPipeline.renderDeferredLighting();
        }

        LLPipeline::sUnderWaterRender = false;

        {
            //capture the frame buffer.
            LL_CPU_ZONE("display: scene monitor capture");
            LLSceneMonitor::getInstance()->capture();
        }

        LLAppViewer::instance()->pingMainloopTimeout("Display:RenderUI");
        if (!for_snapshot)
        {
            { LL_CPU_ZONE("display: render_ui"); render_ui(); }
            { LL_CPU_ZONE("display: swap"); swap(); }
        }


        LLSpatialGroup::sNoDelete = false;

        { LL_CPU_ZONE("display: clearReferences"); gPipeline.clearReferences(); }
    }

    LLAppViewer::instance()->pingMainloopTimeout("Display:FrameStats");


    { LL_CPU_ZONE("display: display_stats"); display_stats(); }

    LLAppViewer::instance()->pingMainloopTimeout("Display:Done");

    gShiftFrame = false;

    if (gShaderProfileFrame)
    {
        gShaderProfileFrame = false;
        boost::json::value stats{ boost::json::object_kind };
        getProfileStatsContext(stats.as_object());
        LLShaderProgram::finishProfile(stats);

        auto report_name = getProfileStatsFilename();
        std::ofstream outf(report_name);
        if (! outf)
        {
            LL_WARNS() << "Couldn't write to " << std::quoted(report_name) << LL_ENDL;
        }
        else
        {
            outf << stats;
            LL_INFOS() << "(also dumped to " << std::quoted(report_name) << ")" << LL_ENDL;
        }
    }
}

void getProfileStatsContext(boost::json::object& stats)
{
    // populate the context with info from LLFloaterAbout
    auto contextit = stats.emplace("context",
                                   LlsdToJson(LLAppViewer::instance()->getViewerInfo())).first;
    auto& context = contextit->value().as_object();

    // then add a few more things
    unsigned char unique_id[MAC_ADDRESS_BYTES]{};
    LLMachineID::getUniqueID(unique_id, sizeof(unique_id));
    context.emplace("machine", stringize(LL::hexdump(unique_id, sizeof(unique_id))));
    context.emplace("grid", LLGridManager::instance().getGrid());
    LLViewerRegion* region = gAgent.getRegion();
    if (region)
    {
        context.emplace("regionid", stringize(region->getRegionID()));
    }
    LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();
    if (parcel)
    {
        context.emplace("parcel", parcel->getName());
        context.emplace("parcelid", parcel->getLocalID());
    }
    context.emplace("time", LLDate::now().toHTTPDateString("%Y-%m-%dT%H:%M:%S"));
}

std::string getProfileStatsFilename()
{
    std::ostringstream basebuff;
    // viewer build
    basebuff << "profile.v" << LLVersionInfo::instance().getBuild();
    // machine ID: zero-initialize unique_id in case LLMachineID fails
    unsigned char unique_id[MAC_ADDRESS_BYTES]{};
    LLMachineID::getUniqueID(unique_id, sizeof(unique_id));
    basebuff << ".m" << LL::hexdump(unique_id, sizeof(unique_id));
    // region ID
    LLViewerRegion *region = gAgent.getRegion();
    basebuff << ".r" << (region? region->getRegionID() : LLUUID());
    // local parcel ID
    LLParcel* parcel = LLViewerParcelMgr::instance().getAgentParcel();
    basebuff << ".p" << (parcel? parcel->getLocalID() : 0);
    // date/time -- omit seconds for now
    auto now = LLDate::now();
    basebuff << ".t" << LLDate::now().toHTTPDateString("%Y-%m-%dT%H-%M-");
    // put this candidate file in our logs directory
    auto base = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, basebuff.str());
    S32 sec;
    now.split(nullptr, nullptr, nullptr, nullptr, nullptr, &sec);
    // Loop over finished filename, incrementing sec until we find one that
    // doesn't yet exist. Should rarely loop (only if successive calls within
    // same second), may produce (e.g.) sec==61, but avoids collisions and
    // preserves chronological filename sort order.
    std::string name;
    std::error_code ec;
    do
    {
        // base + missing 2-digit seconds, append ".json"
        // post-increment sec in case we have to try again
        name = stringize(base, std::setw(2), std::setfill('0'), sec++, ".json");
    } while (std::filesystem::exists(fsyspath(name), ec));
    // Ignoring ec means we might potentially return a name that does already
    // exist -- but if we can't check its existence, what more can we do?
    return name;
}

// WIP simplified copy of display() that does minimal work
void display_cube_face()
{
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Render Cube Face");
    LL_PROFILE_GPU_ZONE("display cube face");

    llassert(!gSnapshot);
    llassert(!gTeleportDisplay);
    llassert(LLStartUp::getStartupState() >= STATE_PRECACHE);
    llassert(!LLAppViewer::instance()->logoutRequestSent());
    llassert(!gRestoreResources);

    bool rebuild = false;

    LLRenderPresetDefault gls_default;
    LLRenderState::DepthScope gls_depth(true, true, LLRenderState::CompareOp::LessEqual);

    LLVertexBuffer::unbind();

    gPipeline.disableLights();

    gPipeline.mBackfaceCull = true;

    gViewerWindow->setup3DViewport();

    if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD))
    { //don't draw hud objects in this frame
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
    }

    if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES))
    { //don't draw hud particles in this frame
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);
    }

    { LL_CPU_ZONE("display: display_update_camera"); display_update_camera(); }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Env Update");
        // update all the sky/atmospheric/water settings
        LLEnvironment::instance().update(LLViewerCamera::getInstance());
    }

    LLSpatialGroup::sNoDelete = true;

    S32 occlusion = LLPipeline::sUseOcclusion;
    LLPipeline::sUseOcclusion = 0; // occlusion data is from main camera point of view, don't read or write it during cube snapshots
    //gDepthDirty = true; //let "real" render pipe know it can't trust the depth buffer for occlusion data

    static LLCullResult result;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
    LLPipeline::sUnderWaterRender = LLViewerCamera::getInstance()->cameraUnderWater();
    gPipeline.updateCull(*LLViewerCamera::getInstance(), result);

    gRender.setColorMask(true, true);

    gRender.setClearColor(0.f, 0.f, 0.f, 0.f);
    gPipeline.generateSunShadow(*LLViewerCamera::getInstance());

    gRender.clearBuffers(ClearBits::Depth); // | ClearBits::Stencil);

    {
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        gPipeline.stateSort(*LLViewerCamera::getInstance(), result);

        if (rebuild)
        {
            //////////////////////////////////////
            //
            // rebuildPools
            //
            //
            { LL_CPU_ZONE("display: rebuildPools"); gPipeline.rebuildPools(); }
        }
    }

    LLPipeline::sUseOcclusion = occlusion;

    LLAppViewer::instance()->pingMainloopTimeout("Display:RenderStart");

    LLPipeline::sUnderWaterRender = LLViewerCamera::getInstance()->cameraUnderWater();

    gRender.setColorMask(true, true);

    {
        RenderPassScope pass(gPipeline.mRT->deferredScreen, gUseWireframe ? RenderPass::Clear(0.5f, 0.5f, 0.5f, 1.f) : RenderPass::Clear(1.f, 0.f, 1.f, 1.f));

        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

        gPipeline.renderGeomDeferred(*LLViewerCamera::getInstance());

    }

    gPipeline.renderDeferredLighting();

    LLPipeline::sUnderWaterRender = false;

    // Finalize scene
    //gPipeline.renderFinalize();

    LLSpatialGroup::sNoDelete = false;
    gPipeline.clearReferences();
}

void render_hud_attachments()
{
    LLPerfStats::RecordSceneTime T ( LLPerfStats::StatType_t::RENDER_HUDS); // render time capture - Primary contributor to HUDs (though these end up in render batches)
    LLRenderMatrix::push(LLRenderMatrix::Kind::Projection);
    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);

    glm::mat4 current_proj = get_current_projection();
    glm::mat4 current_mod = get_current_modelview();

    // clamp target zoom level to reasonable values
//  gAgentCamera.mHUDTargetZoom = llclamp(gAgentCamera.mHUDTargetZoom, 0.1f, 1.f);
// [RLVa:KB] - Checked: 2010-08-22 (RLVa-1.2.1a) | Modified: RLVa-1.0.0c
    gAgentCamera.mHUDTargetZoom = llclamp(gAgentCamera.mHUDTargetZoom, (!gRlvAttachmentLocks.hasLockedHUD()) ? 0.1f : 0.85f, 1.f);
// [/RLVa:KB]

    // smoothly interpolate current zoom level
    gAgentCamera.mHUDCurZoom = lerp(gAgentCamera.mHUDCurZoom, gAgentCamera.getAgentHUDTargetZoom(), LLSmoothInterpolation::getInterpolant(0.03f));

    if (LLPipeline::sShowHUDAttachments && !gDisconnected && setup_hud_matrices())
    {
        LLPipeline::sRenderingHUDs = true;
        LLCamera hud_cam = *LLViewerCamera::getInstance();
        hud_cam.setOrigin(-1.f, 0.f, 0.f);
        hud_cam.setAxes(LLVector3(1.f, 0.f, 0.f), LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, 0.f, 1.f));
        LLViewerCamera::updateFrustumPlanes(hud_cam, true);

        static LLCachedControl<bool> render_hud_particles(gSavedSettings, "RenderHUDParticles", false);
        bool render_particles = gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_PARTICLES) && render_hud_particles;

        //only render hud objects
        gPipeline.pushRenderTypeMask();

        // turn off everything
        gPipeline.andRenderTypeMask(LLPipeline::END_RENDER_TYPES);
        // turn on HUD
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
        // turn on HUD particles
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);

        // if particles are off, turn off hud-particles as well
        if (!render_particles)
        {
            // turn back off HUD particles
            gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD_PARTICLES);
        }

        bool has_ui = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (has_ui)
        {
            gPipeline.toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }

        S32 use_occlusion = LLPipeline::sUseOcclusion;
        LLPipeline::sUseOcclusion = 0;

        //cull, sort, and render hud objects
        static LLCullResult result;
        LLSpatialGroup::sNoDelete = true;

        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;
        gPipeline.updateCull(hud_cam, result, true);

        // Toggle render types
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_BUMP);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_SIMPLE);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_VOLUME);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_ALPHA);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_ALPHA_MASK);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_FULLBRIGHT_ALPHA_MASK);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_FULLBRIGHT);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_GLTF_PBR);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_GLTF_PBR_ALPHA_MASK);

        // Toggle render passes
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_ALPHA);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_ALPHA_MASK);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_BUMP);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_MATERIAL);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_SHINY);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_SHINY);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_INVISIBLE);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_INVISI_SHINY);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_GLTF_PBR);
        gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK);

        gPipeline.stateSort(hud_cam, result);

        gPipeline.renderGeomPostDeferred(hud_cam);

        LLSpatialGroup::sNoDelete = false;
        //gPipeline.clearReferences();

        { LL_CPU_ZONE("ui: render_hud_elements"); render_hud_elements(); }

        //restore type mask
        gPipeline.popRenderTypeMask();

        if (has_ui)
        {
            gPipeline.toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }
        LLPipeline::sUseOcclusion = use_occlusion;
        LLPipeline::sRenderingHUDs = false;
    }
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Projection);
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);

    set_current_projection(current_proj);
    set_current_modelview(current_mod);
}

LLRect get_whole_screen_region()
{
    // <FS:Ansariel> Factor out calls to getInstance
    LLViewerCamera& camera = LLViewerCamera::instance();

    LLRect whole_screen = gViewerWindow->getWorldViewRectScaled();

    // apply camera zoom transform (for high res screenshots)
    F32 zoom_factor = camera.getZoomFactor(); // <FS:Ansariel> Factor out calls to getInstance
    S16 sub_region = camera.getZoomSubRegion(); // <FS:Ansariel> Factor out calls to getInstance
    if (zoom_factor > 1.f)
    {
        S32 num_horizontal_tiles = llceil(zoom_factor);
        S32 tile_width = ll_round((F32)gViewerWindow->getWorldViewWidthScaled() / zoom_factor);
        S32 tile_height = ll_round((F32)gViewerWindow->getWorldViewHeightScaled() / zoom_factor);
        int tile_y = sub_region / num_horizontal_tiles;
        int tile_x = sub_region - (tile_y * num_horizontal_tiles);

        whole_screen.setLeftTopAndSize(tile_x * tile_width, gViewerWindow->getWorldViewHeightScaled() - (tile_y * tile_height), tile_width, tile_height);
    }
    return whole_screen;
}

bool get_hud_matrices(const LLRect& screen_region, glm::mat4 &proj, glm::mat4&model)
{
    if (isAgentAvatarValid() && gAgentAvatarp->hasHUDAttachment())
    {
        F32 zoom_level = gAgentCamera.mHUDCurZoom;
        LLBBox hud_bbox = gAgentAvatarp->getHUDBBox();

        F32 hud_depth = llmax(1.f, hud_bbox.getExtentLocal().mV[VX] * 1.1f);

        // <FS:Ansariel> Factor out calls to getInstance
        //proj = glm::ortho(-0.5f * LLViewerCamera::getInstance()->getAspect(), 0.5f * LLViewerCamera::getInstance()->getAspect(), -0.5f, 0.5f, 0.f, hud_depth);
        //proj[2][2] = -0.01f;

        F32 aspect_ratio = LLViewerCamera::getInstance()->getAspect();
        proj = glm::ortho(-0.5f * aspect_ratio, 0.5f * aspect_ratio, -0.5f, 0.5f, 0.f, hud_depth);
        proj[2][2] = -0.01f;
        // <//FS:Ansariel> Factor out calls to getInstance

        F32 scale_x = (F32)gViewerWindow->getWorldViewWidthScaled() / (F32)screen_region.getWidth();
        F32 scale_y = (F32)gViewerWindow->getWorldViewHeightScaled() / (F32)screen_region.getHeight();

        glm::mat4 mat = glm::identity<glm::mat4>();
        mat = glm::translate(mat,
            glm::vec3(clamp_rescale((F32)(screen_region.getCenterX() - screen_region.mLeft), 0.f, (F32)gViewerWindow->getWorldViewWidthScaled(), 0.5f * scale_x * aspect_ratio, -0.5f * scale_x * aspect_ratio),
                clamp_rescale((F32)(screen_region.getCenterY() - screen_region.mBottom), 0.f, (F32)gViewerWindow->getWorldViewHeightScaled(), 0.5f * scale_y, -0.5f * scale_y),
                0.f));
        mat = glm::scale(mat, glm::vec3(scale_x, scale_y, 1.f));
        proj *= mat;

        glm::mat4 tmp_model = glm::make_mat4(OGL_TO_CFR_ROTATION);
        mat = glm::identity<glm::mat4>();
        mat = glm::translate(mat, glm::vec3(-hud_bbox.getCenterLocal().mV[VX] + (hud_depth * 0.5f), 0.f, 0.f));
        mat = glm::scale(mat, glm::vec3(zoom_level));
        tmp_model *= mat;
        model = tmp_model;

        return true;
    }
    else
    {
        return false;
    }
}

bool get_hud_matrices(glm::mat4 &proj, glm::mat4&model)
{
    LLRect whole_screen = get_whole_screen_region();
    return get_hud_matrices(whole_screen, proj, model);
}

bool setup_hud_matrices()
{
    LLRect whole_screen = get_whole_screen_region();
    return setup_hud_matrices(whole_screen);
}

bool setup_hud_matrices(const LLRect& screen_region)
{
    glm::mat4 proj, model;
    bool result = get_hud_matrices(screen_region, proj, model);
    if (!result) return result;

    // set up transform to keep HUD objects in front of camera
    LLRenderMatrix::load(LLRenderMatrix::Kind::Projection, glm::value_ptr(proj));
    set_current_projection(proj);

    LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, glm::value_ptr(model));
    set_current_modelview(model);
    return true;
}

void render_ui(F32 zoom_factor, int subfield)
{
    LLPerfStats::RecordSceneTime T ( LLPerfStats::StatType_t::RENDER_UI ); // render time capture - Primary UI stat can have HUD time overlap (TODO)
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI; //LL_RECORD_BLOCK_TIME(FTM_RENDER_UI);
    LL_PROFILE_GPU_ZONE("ui");

    glm::mat4 saved_view = get_current_modelview();

    if (!gSnapshot)
    {
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::load(LLRenderMatrix::Kind::Modelview, gRenderLastModelView);
        set_current_modelview(glm::make_mat4(gRenderLastModelView));
    }

    if(LLSceneMonitor::getInstance()->needsUpdate())
    {
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        gViewerWindow->setup2DRender();
        LLSceneMonitor::getInstance()->compare();
        gViewerWindow->setup3DRender();
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }

    // apply gamma correction and post effects
    {
        LL_CPU_ZONE("display: renderFinalize");
        gPipeline.renderFinalize();
    }

    // <FS> DLSS: from here on everything is drawn straight into the full-size window, after the upscale. It must not carry the camera jitter of the scene (nothing
    // averages it out any more, so billboards such as the voice dot, beams and selection outlines would vibrate by up to a pixel).
    LLUnjitteredScope unjittered_ui;

    {


        LL_PROFILE_ZONE_NAMED_CATEGORY_UI("HUD");
    { LL_CPU_ZONE("ui: render_hud_elements"); render_hud_elements(); }
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
        if (RlvActions::hasBehaviour(RLV_BHVR_SETOVERLAY))
        {
            LLVfxManager::instance().runEffect(EVisualEffect::RlvOverlay);
        }
// [/RLVa:KB]
        { LL_CPU_ZONE("display: render_hud_attachments"); render_hud_attachments(); }


        LLRenderPresetDefault gls_default;
        LLRenderPresetUIDefault gls_ui;
        {
            gPipeline.disableLights();
        }

        bool render_ui = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        if (render_ui)
        {
            if (!gDisconnected)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_UI("UI 3D"); //LL_RECORD_BLOCK_TIME(FTM_RENDER_UI_3D);
                { LL_CPU_ZONE("ui: render_ui_3d"); render_ui_3d(); }
            }
            else
            {
                render_disconnected_background();
            }
        }
        else
        {
            // Make sure particle effects disappear
            LLHUDObject::renderAllForTimer();
        }

        if (render_ui)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_UI("UI 2D"); //LL_RECORD_BLOCK_TIME(FTM_RENDER_UI_2D);
            { LL_CPU_ZONE("ui: LLHUDObject::renderAll (name tags)"); LLHUDObject::renderAll(); }
            render_ui_2d();
        }
        // <FS:Beq> FIRE-33239 - particles do not sie when UI is disabled
        if (!render_ui)
        {
            // Make sure particle effects disappear
            LLHUDObject::renderAllForTimer();
        }
        // </FS:Beq>

        gViewerWindow->setup2DRender();
        { LL_CPU_ZONE("ui: updateDebugText"); gViewerWindow->updateDebugText(); }
        { LL_CPU_ZONE("ui: drawDebugText"); gViewerWindow->drawDebugText(); }
    }

    if (!gSnapshot)
    {
        set_current_modelview(saved_view);
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }
}

void swap()
{
    LLPerfStats::RecordSceneTime T ( LLPerfStats::StatType_t::RENDER_SWAP ); // render time capture - Swap buffer time - can signify excessive data transfer to/from GPU
    LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("Swap");
    LL_PROFILE_GPU_ZONE("swap");
    if (gDisplaySwapBuffers)
    {
        RenderPass::frameEnd(); // <FS> F3.5a: a render target still bound at the end of the frame (LL_VK_PASS_CHECK=1)
        LLRenderMatrix::checkFrameEnd(); // <FS> F3.3: matrix stacks that are not empty at the end of the frame (LL_VK_MATRIX_GUARD_CHECK=1)
        LLVkBridge::presentFrame(); // <FS>
        gViewerWindow->getWindow()->swapBuffers();
    }
    gDisplaySwapBuffers = true;

}

void renderCoordinateAxes()
{
    gRender.getTexUnit(0)->bindWhite();
    gRender.begin(LLRender::LINES);
        gRender.color3f(1.0f, 0.0f, 0.0f);   // i direction = X-Axis = red
        gRender.vertex3f(0.0f, 0.0f, 0.0f);
        gRender.vertex3f(2.0f, 0.0f, 0.0f);
        gRender.vertex3f(3.0f, 0.0f, 0.0f);
        gRender.vertex3f(5.0f, 0.0f, 0.0f);
        gRender.vertex3f(6.0f, 0.0f, 0.0f);
        gRender.vertex3f(8.0f, 0.0f, 0.0f);
        // Make an X
        gRender.vertex3f(11.0f, 1.0f, 1.0f);
        gRender.vertex3f(11.0f, -1.0f, -1.0f);
        gRender.vertex3f(11.0f, 1.0f, -1.0f);
        gRender.vertex3f(11.0f, -1.0f, 1.0f);

        gRender.color3f(0.0f, 1.0f, 0.0f);   // j direction = Y-Axis = green
        gRender.vertex3f(0.0f, 0.0f, 0.0f);
        gRender.vertex3f(0.0f, 2.0f, 0.0f);
        gRender.vertex3f(0.0f, 3.0f, 0.0f);
        gRender.vertex3f(0.0f, 5.0f, 0.0f);
        gRender.vertex3f(0.0f, 6.0f, 0.0f);
        gRender.vertex3f(0.0f, 8.0f, 0.0f);
        // Make a Y
        gRender.vertex3f(1.0f, 11.0f, 1.0f);
        gRender.vertex3f(0.0f, 11.0f, 0.0f);
        gRender.vertex3f(-1.0f, 11.0f, 1.0f);
        gRender.vertex3f(0.0f, 11.0f, 0.0f);
        gRender.vertex3f(0.0f, 11.0f, 0.0f);
        gRender.vertex3f(0.0f, 11.0f, -1.0f);

        gRender.color3f(0.0f, 0.0f, 1.0f);   // Z-Axis = blue
        gRender.vertex3f(0.0f, 0.0f, 0.0f);
        gRender.vertex3f(0.0f, 0.0f, 2.0f);
        gRender.vertex3f(0.0f, 0.0f, 3.0f);
        gRender.vertex3f(0.0f, 0.0f, 5.0f);
        gRender.vertex3f(0.0f, 0.0f, 6.0f);
        gRender.vertex3f(0.0f, 0.0f, 8.0f);
        // Make a Z
        gRender.vertex3f(-1.0f, 1.0f, 11.0f);
        gRender.vertex3f(1.0f, 1.0f, 11.0f);
        gRender.vertex3f(1.0f, 1.0f, 11.0f);
        gRender.vertex3f(-1.0f, -1.0f, 11.0f);
        gRender.vertex3f(-1.0f, -1.0f, 11.0f);
        gRender.vertex3f(1.0f, -1.0f, 11.0f);
    gRender.end();
}


void draw_axes()
{
    LLRenderPresetUIDefault gls_ui;
    gRender.getTexUnit(0)->bindWhite();
    // A vertical white line at origin
    LLVector3 v = gAgent.getPositionAgent();
    gRender.begin(LLRender::LINES);
        gRender.color3f(1.0f, 1.0f, 1.0f);
        gRender.vertex3f(0.0f, 0.0f, 0.0f);
        gRender.vertex3f(0.0f, 0.0f, 40.0f);
    gRender.end();
    // Some coordinate axes
    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, v.mV[VX], v.mV[VY], v.mV[VZ]);
        renderCoordinateAxes();
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
}

// <FS:PP> Render chat range spheres in 3D world
static void renderChatRangeSphere(const LLVector3& center, F32 radius, const LLColor4& color)
{
    LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
    {
        LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, center.mV[VX], center.mV[VY], center.mV[VZ]);
        gDebugProgram.bind();
        LLRenderState::BlendScope blend(true);
        LLRenderState::DepthScope depth(true, true);
        gRender.color4fv(color.mV);
        gRender.diffuseColor4fv(color.mV);
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        {
            LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, radius, radius, radius);
            gRender.setCullFace(LLRenderState::CullFace::Front);
            gSphere.render();
            gRender.setCullFace(LLRenderState::CullFace::Back);
            gSphere.render();
        }
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
        gUIProgram.bind();
    }
    LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
}

static void drawChatRangeSpheres()
{
    if (!isAgentAvatarValid())
    {
        return;
    }

    LFSimFeatureHandler& simfeatures = LFSimFeatureHandler::instance();
    F32 whisper_range = (F32)simfeatures.whisperRange();
    F32 say_range = (F32)simfeatures.sayRange();
    F32 shout_range = (F32)simfeatures.shoutRange();

    LLVector3 avatar_pos = gAgent.getPositionAgent();
    static LLUIColor whisper_color = LLUIColorTable::instance().getColor("MapWhisperRingColor", LLColor4::blue);
    static LLUIColor say_color = LLUIColorTable::instance().getColor("MapChatRingColor", LLColor4::yellow);
    static LLUIColor shout_color = LLUIColorTable::instance().getColor("MapShoutRingColor", LLColor4::red);
    renderChatRangeSphere(avatar_pos, whisper_range, whisper_color);
    renderChatRangeSphere(avatar_pos, say_range, say_color);
    renderChatRangeSphere(avatar_pos, shout_range, shout_color);
}
// </FS:PP>

void render_ui_3d()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    LLRenderPresetPipeline gls_pipeline;

    //////////////////////////////////////
    //
    // Render 3D UI elements
    // NOTE: zbuffer is cleared before we get here by LLDrawPoolHUD,
    //       so 3d elements requiring Z buffer are moved to LLDrawPoolHUD
    //

    /////////////////////////////////////////////////////////////
    //
    // Render 2.5D elements (2D elements in the world)
    // Stuff without z writes
    //

    // Debugging stuff goes before the UI.


    gUIProgram.bind();
    gRender.color4f(1.f, 1.f, 1.f, 1.f);

    // Coordinate axes
    static LLCachedControl<bool> show_axes(gSavedSettings, "ShowAxes");
    if (show_axes())
    {
        draw_axes();
    }

    // <FS:PP> Render chat range spheres in 3D world
    static LLCachedControl<bool> show_spheres(gSavedSettings, "FSShowChatRangeSpheres", false);
    if (show_spheres())
    {
        drawChatRangeSpheres();
    }
    // </FS:PP>

    { LL_CPU_ZONE("ui: renderSelections"); gViewerWindow->renderSelections(false, false, true); } // Non HUD call in render_hud_elements

    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        // Render debugging beacons.
        { LL_CPU_ZONE("ui: object beacons"); gObjectList.renderObjectBeacons(); }
        gObjectList.resetObjectBeacons();
        gSky.addSunMoonBeacons();
    }
    else
    {
        // Make sure particle effects disappear
        LLHUDObject::renderAllForTimer();
    }

}

#if LL_WINDOWS
#include <dbghelp.h>
// <FS> test aid: an exception on the helper thread is logged with its stack (symbols from the pdb next to the exe) before the viewer goes down
static LONG ui_thread_filter(EXCEPTION_POINTERS* info)
{
    static bool said = false;
    if (said)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    said = true;
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    LL_WARNS("UiThread") << "exception " << std::hex << info->ExceptionRecord->ExceptionCode << " at " << info->ExceptionRecord->ExceptionAddress << std::dec << LL_ENDL;
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
        {
            break;
        }
        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        std::string name = "?";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
        {
            name = symbol->Name;
        }
        IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
        DWORD line_displacement = 0;
        std::string where;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line))
        {
            where = std::string(line.FileName) + ":" + std::to_string(line.LineNumber);
        }
        LL_WARNS("UiThread") << "  #" << i << " " << name << " " << where << LL_ENDL;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool ui_thread_run_guarded(void (*fn)(void*), void* arg)
{
    __try
    {
        fn(arg);
        return true;
    }
    __except (ui_thread_filter(GetExceptionInformation()))
    {
        return false;
    }
}
#endif

namespace LLTrace { void set_frame_recording_override(PeriodicRecording* recording); }      // lltracerecording.cpp (not in the header: it is included everywhere)

// <FS> One persistent thread for the UI draw: the statistics (LLTrace) of the widgets are tied to the thread that started them (a stat graph or the status bar creates a recording while it
// draws and keeps it from frame to frame), so the thread that draws the UI is always the same one, with a recorder of its own that reports to the master's like any LLThread's.
class UiDrawThread
{
public:
    static UiDrawThread& instance()
    {
        static UiDrawThread* thread = new UiDrawThread;       // never destroyed: it ends with the process
        return *thread;
    }

    // runs the job on the thread and returns when it is done (false: it raised an exception)
    bool run(const std::function<void()>& job)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mJob = &job;
        mOk = true;
        mDone = false;
        mWake.notify_one();
        mFinished.wait(lock, [this] { return mDone; });
        return mOk;
    }

private:
    UiDrawThread() : mThread([this] { loop(); }) { mThread.detach(); }

    static void call(void* arg)
    {
        (*static_cast<const std::function<void()>*>(arg))();
    }

    void loop()
    {
        LLTrace::ThreadRecorder recorder(*LLTrace::get_master_thread_recorder());
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;)
        {
            mWake.wait(lock, [this] { return mJob != nullptr; });
            const std::function<void()>* job = mJob;
            lock.unlock();
#if LL_WINDOWS
            const bool ok = ui_thread_run_guarded(&UiDrawThread::call, const_cast<std::function<void()>*>(job));
#else
            (*job)();
            const bool ok = true;
#endif
            recorder.pushToParent();
            lock.lock();
            mOk = ok;
            mJob = nullptr;
            mDone = true;
            mFinished.notify_one();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake, mFinished;
    const std::function<void()>* mJob = nullptr;
    bool mDone = false;
    bool mOk = true;
    std::thread mThread;
};

// <FS> The widgets of the UI recorded as a sub-stream (F3, see doc/vulkan/F2-start.md, "the UI draw as a builder"). LL_VK_UI_SUBSTREAM=1: on this thread, spliced at once (the stream must
// come out the same). =2: on a helper thread while this one waits (the thread gets the state of this one's gRender, hands it back when it is done): shows what a second thread needs
// without racing anything.
namespace LLVkState { void setScissorTraceFrame(U32 frame); }       // llvkstate.cpp

static void draw_widgets()
{
    static const char* mode_env = getenv("LL_VK_UI_SUBSTREAM");
    static const bool alternate = mode_env && mode_env[0] == 'a';       // "alt": 60 frames on the main stream, 60 as a sub-stream (the same session, so the UI is the same)
    const int mode = alternate ? (int)((LLFrameTimer::getFrameCount() / 60) % 2) : (mode_env ? atoi(mode_env) : 0);
    static bool trace_set = false;
    if (!trace_set)
    {
        trace_set = true;
        // LL_VK_SCISSOR_TRACE=rel: a frame in the odd 60-frame block (the sub-stream one) about ten seconds from now, and the one 60 frames later (the main one)
        U32 block = LLFrameTimer::getFrameCount() / 60 + 10;
        if ((block % 2) == 0)
        {
            ++block;
        }
        LLVkState::setScissorTraceFrame(block * 60 + 5);
    }
    static int last_logged = -2;
    if (mode_env && last_logged != mode)
    {
        last_logged = mode;
        LL_INFOS("UiThread") << "frame " << LLFrameTimer::getFrameCount() << ": UI draw mode " << mode << " (LL_VK_UI_SUBSTREAM=" << mode_env << ", sub-streams usable: " << LLVkSubStream::usable() << ")" << LL_ENDL;
    }
    if (mode == 0 || !LLVkSubStream::usable())
    {
        gViewerWindow->draw();
        return;
    }
    LLVkSubStream sub;
    if (mode == 1)
    {
        sub.begin();
        gViewerWindow->draw();
        sub.end();
        sub.splice();
        return;
    }
    std::unique_ptr<LLRender> state_after;
    const LLRender* state_before = &gRender;
    LLTrace::PeriodicRecording* main_frame_recording = &LLTrace::get_frame_recording();
    static U32 traced = 0;
    const bool trace = traced < 3;
    if (trace) { LL_INFOS("UiThread") << "frame " << traced << ": handing the widget draw to the UI thread" << LL_ENDL; }
    const bool ok = UiDrawThread::instance().run([&]()
    {
        if (trace) { LL_INFOS("UiThread") << "ui thread: copying gRender" << LL_ENDL; }
        LLTrace::set_frame_recording_override(main_frame_recording);       // the status bar and the stat graphs read the frame statistics, which only the main thread's recording has
        gRender = *state_before;                    // what the main thread's gRender believes and holds (matrix stacks, texture units, the immediate buffer)
        sub.begin();
        gViewerWindow->draw();
        sub.end();
        state_after.reset(new LLRender(gRender));   // and what it believes now
        LLTrace::set_frame_recording_override(nullptr);
        if (trace) { LL_INFOS("UiThread") << "ui thread: done" << LL_ENDL; }
    });
    if (!ok)
    {
        LL_ERRS("UiThread") << "the widget draw on the UI thread raised an exception (see the lines above)" << LL_ENDL;
    }
    if (trace) { LL_INFOS("UiThread") << "back on the main thread: taking the state back and splicing" << LL_ENDL; }
    gRender = *state_after;
    sub.splice();
    ++traced;
}

void render_ui_2d()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    LLRenderPresetUIDefault gls_ui;

    /////////////////////////////////////////////////////////////
    //
    // Render 2D UI elements that overlay the world (no z compare)

    //  Disable wireframe mode below here, as this is HUD/menus
    gRender.setPolygonMode(LLRenderState::PolygonMode::Fill);

    //  Menu overlays, HUD, etc
    gViewerWindow->setup2DRender();

    // <FS:Ansariel> Factor out instance() call
    //F32 zoom_factor = LLViewerCamera::getInstance()->getZoomFactor();
    //S16 sub_region = LLViewerCamera::getInstance()->getZoomSubRegion();
    LLViewerCamera& camera = LLViewerCamera::instance();
    F32 zoom_factor = camera.getZoomFactor();
    S16 sub_region = camera.getZoomSubRegion();
    LLVector2& ui_scale_factor = LLUI::getScaleFactor();

    if (zoom_factor > 1.f)
    {
        //decompose subregion number to x and y values
        int pos_y = sub_region / llceil(zoom_factor);
        int pos_x = sub_region - (pos_y*llceil(zoom_factor));
        // offset for this tile
        LLFontGL::sCurOrigin.mX -= ll_round((F32)gViewerWindow->getWindowWidthScaled() * (F32)pos_x / zoom_factor);
        LLFontGL::sCurOrigin.mY -= ll_round((F32)gViewerWindow->getWindowHeightScaled() * (F32)pos_y / zoom_factor);
    }


    // render outline for HUD
    if (isAgentAvatarValid() && gAgentCamera.mHUDCurZoom < 0.98f)
    {
        gUIProgram.bind();
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        S32 half_width = (gViewerWindow->getWorldViewWidthScaled() / 2);
        S32 half_height = (gViewerWindow->getWorldViewHeightScaled() / 2);
        LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, LLUI::getScaleFactor().mV[VX], LLUI::getScaleFactor().mV[VY], 1.f);
        LLRenderMatrix::translate(LLRenderMatrix::Kind::Modelview, (F32)half_width, (F32)half_height, 0.f);
        F32 zoom = gAgentCamera.mHUDCurZoom;
        LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, zoom,zoom,1.f);
        gRender.color4fv(LLColor4::white.mV);
        gl_rect_2d(-half_width, half_height, half_width, -half_height, false);
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
        gUIProgram.unbind();
    }


    if (LLPipeline::RenderUIBuffer)
    {
        if (LLView::sIsRectDirty)
        {
            LLView::sIsRectDirty = false;
            LLRect t_rect;

            {
                RenderPassScope pass(gPipeline.mUIScreen);
                gRender.setColorMask(true, true);
                {
                    constexpr S32 pad = 8;

                    LLView::sDirtyRect.mLeft -= pad;
                    LLView::sDirtyRect.mRight += pad;
                    LLView::sDirtyRect.mBottom -= pad;
                    LLView::sDirtyRect.mTop += pad;

                    LLRenderState::ScissorScope scissor(true);
                    static LLRect last_rect = LLView::sDirtyRect;

                    //union with last rect to avoid mouse poop
                    last_rect.unionWith(LLView::sDirtyRect);

                    t_rect = LLView::sDirtyRect;
                    LLView::sDirtyRect = last_rect;
                    last_rect = t_rect;

                    // <FS:Ansariel> Factor out instance() call
                    //last_rect.mLeft = LLRect::tCoordType(last_rect.mLeft / LLUI::getScaleFactor().mV[0]);
                    //last_rect.mRight = LLRect::tCoordType(last_rect.mRight / LLUI::getScaleFactor().mV[0]);
                    //last_rect.mTop = LLRect::tCoordType(last_rect.mTop / LLUI::getScaleFactor().mV[1]);
                    //last_rect.mBottom = LLRect::tCoordType(last_rect.mBottom / LLUI::getScaleFactor().mV[1]);
                    last_rect.mLeft = LLRect::tCoordType(last_rect.mLeft / ui_scale_factor.mV[0]);
                    last_rect.mRight = LLRect::tCoordType(last_rect.mRight / ui_scale_factor.mV[0]);
                    last_rect.mTop = LLRect::tCoordType(last_rect.mTop / ui_scale_factor.mV[1]);
                    last_rect.mBottom = LLRect::tCoordType(last_rect.mBottom / ui_scale_factor.mV[1]);

                    LLRect clip_rect(last_rect);

                    gRender.clearBuffers(ClearBits::Color);

                    { LL_CPU_ZONE("ui: gViewerWindow->draw (widgets)"); draw_widgets(); }
                }

            }
            gRender.setColorMask(true, false);

            LLView::sDirtyRect = t_rect;
        }

        LLRenderState::CullScope cull(false);
        LLRenderState::BlendScope blend(false);
        S32 width = gViewerWindow->getWindowWidthScaled();
        S32 height = gViewerWindow->getWindowHeightScaled();
        gRender.getTexUnit(0)->bind(&gPipeline.mUIScreen);
        gRender.begin(LLRender::TRIANGLE_STRIP);
        gRender.color4f(1.f,1.f,1.f,1.f);
        gRender.texCoord2f(0.f, 0.f);                 gRender.vertex2i(0, 0);
        gRender.texCoord2f((F32)width, 0.f);          gRender.vertex2i(width, 0);
        gRender.texCoord2f(0.f, (F32)height);         gRender.vertex2i(0, height);
        gRender.texCoord2f((F32)width, (F32)height);  gRender.vertex2i(width, height);
        gRender.end();
    }
    else
    {
        { LL_CPU_ZONE("ui: gViewerWindow->draw (widgets)"); draw_widgets(); }
    }

    // reset current origin for font rendering, in case of tiling render
    LLFontGL::sCurOrigin.set(0, 0);
}

void render_disconnected_background()
{
    gUIProgram.bind();

    gRender.color4f(1.f, 1.f, 1.f, 1.f);
    if (!gDisconnectedImagep && gDisconnected)
    {
        LL_INFOS() << "Loading last bitmap..." << LL_ENDL;

        std::string temp_str;
        temp_str = gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter() + LLStartUp::getScreenLastFilename();

        LLPointer<LLImagePNG> image_png = new LLImagePNG;
        if( !image_png->load(temp_str) )
        {
            //LL_INFOS() << "Bitmap load failed" << LL_ENDL;
            return;
        }

        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!image_png->decode(raw, 0.0f))
        {
            LL_INFOS() << "Bitmap decode failed" << LL_ENDL;
            gDisconnectedImagep = NULL;
            return;
        }

        U8 *rawp = raw->getData();
        S32 npixels = (S32)image_png->getWidth()*(S32)image_png->getHeight();
        for (S32 i = 0; i < npixels; i++)
        {
            S32 sum = 0;
            sum = *rawp + *(rawp+1) + *(rawp+2);
            sum /= 3;
            *rawp = ((S32)sum*6 + *rawp)/7;
            rawp++;
            *rawp = ((S32)sum*6 + *rawp)/7;
            rawp++;
            *rawp = ((S32)sum*6 + *rawp)/7;
            rawp++;
        }


        raw->expandToPowerOfTwo();
        gDisconnectedImagep = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        gStartTexture = gDisconnectedImagep;
        gRender.getTexUnit(0)->bindWhite();
    }

    // Make sure the progress view always fills the entire window.
    S32 width = gViewerWindow->getWindowWidthScaled();
    S32 height = gViewerWindow->getWindowHeightScaled();

    if (gDisconnectedImagep)
    {
        LLRenderPresetUIDefault gls_ui;
        gViewerWindow->setup2DRender();
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        {
            // scale ui to reflect UIScaleFactor
            // this can't be done in setup2DRender because it requires a
            // pushMatrix/popMatrix pair
            const LLVector2& display_scale = gViewerWindow->getDisplayScale();
            LLRenderMatrix::scale(LLRenderMatrix::Kind::Modelview, display_scale.mV[VX], display_scale.mV[VY], 1.f);

            gRender.getTexUnit(0)->bind(gDisconnectedImagep);
            gRender.color4f(1.f, 1.f, 1.f, 1.f);
            gl_rect_2d_simple_tex(width, height);
            gRender.getTexUnit(0)->bindWhite();
        }
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);
    }
    gRender.flush();

    gUIProgram.unbind();
}

void display_cleanup()
{
    gDisconnectedImagep = nullptr;
}
