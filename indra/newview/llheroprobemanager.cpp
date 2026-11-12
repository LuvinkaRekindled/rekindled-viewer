/**
 * @file LLHeroProbeManager.cpp
 * @brief LLHeroProbeManager class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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
#include "llgpuzonetimer.h"
#include "llvoavatar.h"

#include "llheroprobemanager.h"
#include "llreflectionmapmanager.h"
#include "llviewercamera.h"
#include "llspatialpartition.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llstartup.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llviewerwindow.h"
#include "llviewerjoystick.h"
#include "llviewermediafocus.h"

extern bool gCubeSnapshot;
extern bool gTeleportDisplay;

// get the next highest power of two of v (or v if v is already a power of two)
//defined in llvertexbuffer.cpp
extern U32 nhpo2(U32 v);

static void touch_default_probe(LLReflectionMap* probe)
{
    if (LLViewerCamera::getInstance())
    {
        LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
        origin.mV[2] += 64.f;

        probe->mOrigin.load3(origin.mV);
    }
}

// <FS> the hero probe census (LL_VK_CPUPROBE=1, in the CpuZones line): is a mirror picked, how far, is it worn, does the probe render or does something skip it
namespace
{
    struct HeroCensus
    {
        U64 mUpdateCalls = 0, mPicked = 0, mPickedAttached = 0, mSwitches = 0;
        U64 mRenderCalls = 0, mSkipMirrorsOff = 0, mSkipProbesOff = 0, mSkipState = 0, mNoMirror = 0, mSkipOccluded = 0, mRendered = 0;
        F64 mPickedDistance = 0.0;
    } sHero;
}

std::string fs_hero_probe_report(double frames)
{
    const HeroCensus& h = sHero;
    std::string text = llformat(", hero probe over %.0f frames: update ran %llu, a mirror picked in %llu (%llu worn by another or self; %.0f m away on average), mirror changes %llu; renderProbes called %llu, skipped: Mirrors setting off %llu, probes off %llu, teleport or startup %llu, no mirror %llu, occluded %llu; rendered faces in %llu",
        frames, (unsigned long long)h.mUpdateCalls, (unsigned long long)h.mPicked, (unsigned long long)h.mPickedAttached, h.mPicked ? h.mPickedDistance / (F64)h.mPicked : 0.0, (unsigned long long)h.mSwitches,
        (unsigned long long)h.mRenderCalls, (unsigned long long)h.mSkipMirrorsOff, (unsigned long long)h.mSkipProbesOff, (unsigned long long)h.mSkipState, (unsigned long long)h.mNoMirror, (unsigned long long)h.mSkipOccluded, (unsigned long long)h.mRendered);
    sHero = HeroCensus();
    return text;
}

LLHeroProbeManager::LLHeroProbeManager()
{
}

LLHeroProbeManager::~LLHeroProbeManager()
{
    cleanup();

    mHeroVOList.clear();
    mNearestHero = nullptr;
}

// helper class to seed octree with probes
void LLHeroProbeManager::update()
{
    if (!LLPipeline::RenderMirrors || !LLPipeline::sReflectionProbesEnabled || gTeleportDisplay || LLStartUp::getStartupState() < STATE_STARTED)
    {
        return;
    }

    // Part of a hacky workaround to fix #3331.
    // For some reason clearing shaders will cause mirrors to actually work.
    // There's likely some deeper state issue that needs to be resolved.
    // - Geenz 2025-02-25
    // <FS:Beq> [FIRE-35007][#3331] mirrors not working after relog. hack no longer needed.
    // if (!mInitialized && LLStartUp::getStartupState() > STATE_PRECACHE)
    // {
    //     LLViewerShaderMgr::instance()->clearShaderCache();
    //     LLViewerShaderMgr::instance()->setShaders();
    //     mInitialized = true;
    // }
    // </FS:Beq>

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("hero manager update");
    llassert(!gCubeSnapshot); // assert a snapshot is not in progress
    if (LLAppViewer::instance()->logoutRequestSent())
    {
        return;
    }

    initReflectionMaps();

    static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

    if (!mRenderTarget.isComplete())
    {
        TexelFormat color_fmt = render_hdr ? TexelFormat::RGBA16F : TexelFormat::RGBA8;
        mRenderTarget.allocate(mProbeResolution, mProbeResolution, color_fmt, true);
    }

    if (mMipChain.empty())
    {
        U32 res = mProbeResolution;
        U32 count = (U32)(log2((F32)res) + 0.5f);

        mMipChain.resize(count);
        for (U32 i = 0; i < count; ++i)
        {
            mMipChain[i].allocate(res, res, render_hdr ? TexelFormat::RGBA16F : TexelFormat::RGBA8);
            res /= 2;
        }
    }

    llassert(mProbes[0] == mDefaultProbe);

    sHero.mUpdateCalls++;

    LLVector4a probe_pos;
    LLVector3 camera_pos = LLViewerCamera::instance().mOrigin;
    bool       probe_present = false;
    LLQuaternion cameraOrientation = LLViewerCamera::instance().getQuaternion();
    LLVector3    cameraDirection   = LLVector3::z_axis * cameraOrientation;

    if (mHeroVOList.size() > 0)
    {
        // Find our nearest hero candidate.
        float last_distance = 99999.f;
        float camera_center_distance = 99999.f;
        mNearestHero = nullptr;
        std::vector<std::pair<LLVOVolume*, LLVector3> > candidates;       // <FS> the mirrors that are in view and face the camera
        for (auto vo : mHeroVOList)
        {
            // <FS> a mirror worn by another avatar (a sword, a handbag: a thin box flagged as a mirror) puts a whole scene render and its shadows on every frame; FSHeroProbeIgnoreOthersAttachments (default on) leaves those out
            static LLCachedControl<bool> ignore_others(gSavedSettings, "FSHeroProbeIgnoreOthersAttachments", true);
            if (ignore_others && vo && vo->isAttachment() && vo->getAvatar() && !vo->getAvatar()->isSelf())
            {
                continue;
            }
            if (vo && !vo->isDead() && vo->mDrawable.notNull() && vo->isReflectionProbe() && vo->getReflectionProbeIsBox())
            {
                float distance = (LLViewerCamera::instance().getOrigin() - vo->getPositionAgent()).magVec();
                float center_distance = cameraDirection * (vo->getPositionAgent() - camera_pos);

                if (distance > LLViewerCamera::instance().getFar())
                    continue;

                // <FS> FSHeroProbeMaxDistance: a mirror that is not very large is not visible far away, and its probe renders the scene around it (objects created there that stay after the probe moves on)
                static LLCachedControl<F32> max_distance(gSavedSettings, "FSHeroProbeMaxDistance", 30.f);
                if (max_distance > 0.f)
                {
                    const LLVector3& mirror_scale = vo->getScale();
                    const F32 allowed = llmax((F32)max_distance, 6.f * llmax(mirror_scale.mV[VX], llmax(mirror_scale.mV[VY], mirror_scale.mV[VZ])));
                    if (distance > allowed)
                        continue;
                }

                LLVector4a center;
                center.load3(vo->getPositionAgent().mV);
                LLVector4a size;

                size.load3(vo->getScale().mV);

                bool visible = LLViewerCamera::instance().AABBInFrustum(center, size);

                // <FS:Beq> Check if the reflection normal (Z-Up axis) is facing towards the camera
                auto reflection_normal = LLVector3(0, 0, 1);
                if(visible)
                {
                    vo->mDrawable->getWorldRotation();
                    reflection_normal *= vo->mDrawable->getWorldRotation();
                    reflection_normal.normalize();
                    LLVector3 offset = camera_pos - vo->getPositionAgent();
                    visible  = (reflection_normal * offset >= 0.0);
                }
                // </FS:Beq>

                if (visible)
                {
                    candidates.push_back({ vo, reflection_normal });
                }
                if (distance < last_distance && center_distance < camera_center_distance && visible)
                {
                    probe_present = true;
                    mNearestHero = vo;
                    mMirrorNormal = reflection_normal;
                    last_distance = distance;
                    camera_center_distance = center_distance;
                }
            }
            else
            {
                unregisterViewerObject(vo);
            }
        }

        // <FS> the probe keeps the mirror it has for at least 60 frames before it takes another one that is in view: two mirrors of about the same distance made it flip several times a second (each
        // flip starts the cube over)
        {
            static LLVOVolume* current = nullptr;
            static U32 since = 0;
            if (probe_present)
            {
                if (mNearestHero.get() != current)
                {
                    bool kept = false;
                    if (current && gFrameCount - since < 60)
                    {
                        for (const auto& c : candidates)
                        {
                            if (c.first == current)
                            {
                                mNearestHero = current;
                                mMirrorNormal = c.second;
                                kept = true;
                                break;
                            }
                        }
                    }
                    if (!kept)
                    {
                        current = mNearestHero.get();
                        since = gFrameCount;
                    }
                }
            }
            else
            {
                current = nullptr;
            }
        }

        // <FS> which mirror is the hero probe's, logged when it changes (a mirror in view doubles the frame's scene work: the log says whether a run had one)
        {
            static const LLViewerObject* last_hero = nullptr;
            const LLViewerObject* now_hero = probe_present ? mNearestHero : nullptr;
            if (probe_present)
            {
                ++sHero.mPicked;
                sHero.mPickedAttached += now_hero->isAttachment();
                sHero.mPickedDistance += (camera_pos - now_hero->getPositionAgent()).magVec();
            }
            if (now_hero != last_hero)
            {
                ++sHero.mSwitches;
                last_hero = now_hero;
                if (now_hero)
                {
                    LL_INFOS("HeroProbe") << "hero probe: mirror object " << now_hero->getID() << " at " << now_hero->getPositionAgent() << " scale " << now_hero->getScale() << (now_hero->isAttachment() ? " (attached)" : "") << LL_ENDL;
                }
                else
                {
                    LL_INFOS("HeroProbe") << "hero probe: no mirror in view" << LL_ENDL;
                }
            }
        }

        // Don't even try to do anything if we didn't find a single mirror present.
        if (!probe_present)
            return;

        if (mNearestHero != nullptr && !mNearestHero->isDead() && mNearestHero->mDrawable.notNull())
        {
            LLVector3 hero_pos = mNearestHero->getPositionAgent();
            // <FS:Beq> let's not redo work we already did
            // LLVector3 face_normal = LLVector3(0, 0, 1);

            // face_normal *= mNearestHero->mDrawable->getWorldRotation();
            // face_normal.normalize();
            const LLVector3 face_normal = mMirrorNormal; // this is redundant could use mMirrorNormal but as the code following is likely to change this might make merges simpler.
            // </FS:Beq>

            LLVector3 offset = camera_pos - hero_pos;
            LLVector3 project = face_normal * (offset * face_normal);
            LLVector3 reject  = offset - project;
            LLVector3 point   = (reject - project) + hero_pos;

            mCurrentClipPlane.setVec(hero_pos, face_normal);
            mMirrorPosition = hero_pos;
            // mMirrorNormal   = face_normal; // <FS:Beq/> no need to assign back

            probe_pos.load3(point.mV);

            // Detect visible faces of a cube based on camera direction and distance

            // Define the cube faces
            static LLVector3 cubeFaces[6] = {
                LLVector3(1, 0, 0),
                LLVector3(-1, 0, 0),
                LLVector3(0, 1, 0),
                LLVector3(0, -1, 0),
                LLVector3(0, 0, 1),
                LLVector3(0, 0, -1)
            };

            mProbes[0]->mOrigin = probe_pos;
            mProbes[0]->mRadius = mNearestHero->getScale().magVec() * 0.5f;
        }
        else
        {
            mNearestHero = nullptr;
            mDefaultProbe->mViewerObject = nullptr;
        }

        mHeroProbeStrength = 1;
    }
    else
    {
        mNearestHero = nullptr;
        mDefaultProbe->mViewerObject = nullptr;
    }
}

void LLHeroProbeManager::renderProbes()
{
    ++sHero.mRenderCalls;
    mViewRendered = false;
    if (!LLPipeline::RenderMirrors || !LLPipeline::sReflectionProbesEnabled || gTeleportDisplay ||
        LLStartUp::getStartupState() < STATE_STARTED)
    {
        // <FS> which of the four said no (LL_VK_CPUPROBE=1, the hero probe census in the CpuZones line)
        if (!LLPipeline::RenderMirrors) { ++sHero.mSkipMirrorsOff; }
        else if (!LLPipeline::sReflectionProbesEnabled) { ++sHero.mSkipProbesOff; }
        else { ++sHero.mSkipState; }
        return;
    }

    // <FS> (mirror 2026-11-11) the hero probe follows the general reflection detail setting (1 = static + dynamic: avatars, animated objects and particles are in the faces);
    // the settings it read before (RenderHeroReflectionProbeDetail, ...Level) exist in no settings file, so `dynamic` was always false and no avatar ever reached a mirror
    static LLCachedControl<S32> sDetail(gSavedSettings, "RenderReflectionProbeDetail", 1);
    static LLCachedControl<S32> sUpdateRate(gSavedSettings, "RenderHeroProbeUpdateRate", 0);

    F32 near_clip = 0.01f;
    if (mNearestHero != nullptr && !mNearestHero->isDead() &&
        !gTeleportDisplay && !gDisconnected && !LLAppViewer::instance()->logoutRequestSent())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("hpmu - realtime");

        bool radiance_pass = gPipeline.mReflectionMapManager.isRadiancePass();

        gPipeline.mReflectionMapManager.mRadiancePass = true;
        mRenderingMirror = true;

        S32 rate = sUpdateRate;

        // rate must be divisor of 6 (1, 2, 3, or 6)
        if (rate < 1)
        {
            rate = 1;
        }
        else if (rate > 3)
        {
            rate = 6;
        }

        S32 face = gFrameCount % 6;

        if (mProbes.empty() || mProbes[0].isNull() || mProbes[0]->mOccluded)
        {
            ++sHero.mSkipOccluded;
        }
        else
        {
            ++sHero.mRendered;
        }
        if (!mProbes.empty() && !mProbes[0].isNull() && !mProbes[0]->mOccluded)
        {
            LL_PROFILE_ZONE_NUM(gFrameCount % rate);
            LL_PROFILE_ZONE_NUM(rate);

            bool dynamic = mNearestHero->getReflectionProbeIsDynamic() && sDetail() > 0;
            // <FS> (mirror 2026-11-11) the faces are rendered from the mirrored camera, behind the mirror's plane, and everything between that camera and the plane is clipped: the near plane can sit near the glass.
            // With 0.01 the depth buffer resolved about 0.7 mm at the avatar's 11 m: a thin blended layer hugging a surface (the pink shell over the metal limbs) lost its depth test everywhere but at grazing edges.
            // Every point of the kept half space is at least the plane distance from the camera and, in a face's 45 degree frustum, at least plane distance / sqrt(3) along the face's axis: half the distance is safe for all six.
            F32 face_near = near_clip;
            {
                const LLVector3 origin(mProbes[0]->mOrigin);
                face_near = llmax(near_clip, 0.5f * fabsf((origin - mMirrorPosition) * mMirrorNormal));
            }
            if (ll_mirror_planar())
            {
                // the planar mirror: one reflected view (the size set by RenderHeroProbeResolution) every `rate + 1` frames, 6 at the most (RenderHeroProbeUpdateRate: 1 Ultra every 2nd frame, 2 High every 3rd, 3 Medium every 4th, 6 Low every 6th); the glass reads it
                // (heroMipCount < 0) and keeps showing the last one in between; the cube is not rendered
                const S32 mirror_interval = rate < 6 ? rate + 1 : 6;     // (the user 2026-11-12: even the best setting is every other frame)
                if (mMirrorLastFrame == 0 || (U64)gFrameCount >= mMirrorLastFrame + (U64)mirror_interval)
                {
                    mMirrorLastFrame = (U64)gFrameCount;
                    updateMirrorView(dynamic, face_near);
                }
            }
            else
            {
                for (U32 i = 0; i < 6; ++i)
                {
                    if ((gFrameCount % rate) == (i % rate))
                    { // update 6/rate faces per frame
                        LL_PROFILE_ZONE_NUM(i);
                        updateProbeFace(mProbes[0], i, dynamic, face_near);
                    }
                }
            }
            // <FS> (mirror strobe hunt 2026-11-11) no radiance pass for the hero probe any more: each face's mip chain goes straight into the face's layer of the hero cube (updateProbeFace),
            // nothing samples the cube array while it is written, and every level of every layer holds data
        }

        mRenderingMirror = false;

        gPipeline.mReflectionMapManager.mRadiancePass = radiance_pass;

        mProbes[0]->mViewerObject = mNearestHero;
        mProbes[0]->autoAdjustOrigin();
    }
    else
    {
        ++sHero.mNoMirror;
    }
}

// Do the reflection map update render passes.
// For every 12 calls of this function, one complete reflection probe radiance map and irradiance map is generated
// First six passes render the scene with direct lighting only into a scratch space cube map at the end of the cube map array and generate
// a simple mip chain (not convolution filter).
// At the end of these passes, an irradiance map is generated for this probe and placed into the irradiance cube map array at the index for this probe
// The next six passes render the scene with both radiance and irradiance into the same scratch space cube map and generate a simple mip chain.
// At the end of these passes, a radiance map is generated for this probe and placed into the radiance cube map array at the index for this probe.
// In effect this simulates single-bounce lighting.
void LLHeroProbeManager::updateProbeFace(LLReflectionMap* probe, U32 face, bool is_dynamic, F32 near_clip)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("hero probe update");

    // <FS> ray scene: the casters this face names belong to this face (a face renders every third frame: its casters stay until it renders again)
    struct ProbePassScope { S32 mPrev; explicit ProbePassScope(S32 pass) : mPrev(LLPipeline::sRtProbePass) { LLPipeline::sRtProbePass = pass; } ~ProbePassScope() { LLPipeline::sRtProbePass = mPrev; } } probe_pass_scope(7 + (S32)(face % 6));

    // hacky hot-swap of camera specific render targets
    gPipeline.mRT = &gPipeline.mHeroProbeRT;
    mViewRendered = true;

    { LL_CPU_ZONE("hero: face render (scene)"); probe->update(mRenderTarget.getWidth(), face, is_dynamic, near_clip); }

    gPipeline.mRT = &gPipeline.mMainRT;

    // <FS> the face's mip chain goes into the face's own layer of the hero cube (the layer the lighting samples), not a scratch cube that a radiance pass filters
    const S32 destIdx = probe->mCubeIndex;

    gRender.setColorMask(true, true);
    LLRenderState::DepthScope depth(false, false);
    LLRenderState::CullScope cull(false);
    LLRenderState::BlendScope blend(false);

    // downsample to placeholder map
    {
        LLRenderMatrix::push(LLRenderMatrix::Kind::Modelview);
        LLRenderMatrix::identity(LLRenderMatrix::Kind::Modelview);

        LLRenderMatrix::push(LLRenderMatrix::Kind::Projection);
        LLRenderMatrix::identity(LLRenderMatrix::Kind::Projection);

        gRender.flush();
        U32 res = mProbeResolution * 2;

        static LLStaticHashedString resScale("resScale");
        static LLStaticHashedString direction("direction");
        static LLStaticHashedString znear("znear");
        static LLStaticHashedString zfar("zfar");

        LLRenderTarget *screen_rt = &gPipeline.mHeroProbeRT.screen;
        LLRenderTarget *depth_rt  = &gPipeline.mHeroProbeRT.deferredScreen;

        // perform a gaussian blur on the super sampled render before downsampling
        {
            gGaussianProgram.bind();
            gGaussianProgram.uniform1f(resScale, 1.f / (mProbeResolution * 2));
            S32 diffuseChannel = gGaussianProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, LLTexUnit::TT_TEXTURE);

            // horizontal
            gGaussianProgram.uniform2f(direction, 1.f, 0.f);
            gRender.getTexUnit(diffuseChannel)->bind(screen_rt);
            RenderPassScope pass_366(mRenderTarget);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            pass_366.end();

            // vertical
            gGaussianProgram.uniform2f(direction, 0.f, 1.f);
            gRender.getTexUnit(diffuseChannel)->bind(&mRenderTarget);
            RenderPassScope pass_375(*screen_rt);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            pass_375.end();
            gGaussianProgram.unbind();
        }

        S32 mips = (S32)(log2((F32)mProbeResolution) + 0.5f);

        gReflectionMipProgram.bind();
        S32 diffuseChannel = gReflectionMipProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, LLTexUnit::TT_TEXTURE);
        S32 depthChannel   = gReflectionMipProgram.enableTexture(LLShaderMgr::DEFERRED_DEPTH, LLTexUnit::TT_TEXTURE);

        for (int i = 0; i < mMipChain.size(); ++i)
        {
            LL_PROFILE_GPU_ZONE("hero probe mip");
            RenderPassScope pass(mMipChain[i]);
            if (i == 0)
            {
                gRender.getTexUnit(diffuseChannel)->bind(screen_rt);
            }
            else
            {
                gRender.getTexUnit(diffuseChannel)->bind(&(mMipChain[i - 1]));
            }

            gRender.getTexUnit(depthChannel)->bind(depth_rt, true);

            gReflectionMipProgram.uniform1f(resScale, 1.f / (mProbeResolution * 2));
            gReflectionMipProgram.uniform1f(znear, probe->getNearClip());
            gReflectionMipProgram.uniform1f(zfar, MAX_FAR_CLIP);

            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            res /= 2;

            llassert(mMipChain.size() <= size_t(S32_MAX));
            S32 mip = i - (S32(mMipChain.size()) - mips);

            if (mip >= 0)
            {
                LL_PROFILE_GPU_ZONE("hero probe mip copy");
                mTexture->bind(0);

                gRender.copyTexSubImage3D(mTexture->getTextureHandle(), TextureTarget::CubeArray, mip, 0, 0, destIdx * 6 + face, 0, 0, res, res, LLRenderTarget::getCurrentBoundTarget());

                mTexture->unbind();
            }
        }

        LLRenderMatrix::pop(LLRenderMatrix::Kind::Projection);
        LLRenderMatrix::pop(LLRenderMatrix::Kind::Modelview);

        gRender.getTexUnit(diffuseChannel)->bindWhite();
        gReflectionMipProgram.unbind();
    }
}

bool ll_mirror_planar()
{
    // on by default since 2026-11-11 (the user: "looks good"); LL_VK_MIRROR_PLANAR=0 goes back to the cube faces, whose code stays for as long as the planar mirror has not proven itself
    static const bool on = []() { const char* e = getenv("LL_VK_MIRROR_PLANAR"); return !e || atoi(e) != 0; }();
    return on;
}

// <FS> the planar mirror, step 1 (doc/vulkan/PLAN-planar-mirror.md): the world seen from the viewer's camera reflected across the mirror's plane, at the main view's size, into mMirrorViewRT.
// The glass reads it (heroMipCount < 0); the cube faces are the fallback (LL_VK_MIRROR_PLANAR=0)
void LLHeroProbeManager::updateMirrorView(bool is_dynamic, F32 near_clip)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("mirror view");
    LLRenderTarget& view_rt = gPipeline.mMirrorViewRT.screen;
    if (!view_rt.isComplete())
    {
        return;
    }
    // (the ray scene names the casters of a probe pass under its number: the cube's face 5 is the least used one; the cube goes in step 7)
    struct ProbePassScope { S32 mPrev; explicit ProbePassScope(S32 pass) : mPrev(LLPipeline::sRtProbePass) { LLPipeline::sRtProbePass = pass; } ~ProbePassScope() { LLPipeline::sRtProbePass = mPrev; } } probe_pass_scope(12);

    gPipeline.mRT = &gPipeline.mMirrorViewRT;
    mViewRendered = true;
    gViewerWindow->mirrorSnapshot(mMirrorPosition, mMirrorNormal, near_clip, is_dynamic);

    gPipeline.mRT = &gPipeline.mMainRT;
    gPipeline.upscaleMirrorView();      // (a view rendered below the main view's size is enlarged for the glass; no-op at full size)
}

void LLHeroProbeManager::updateUniforms()
{
    if (!gPipeline.RenderMirrors)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LL_PROFILE_GPU_ZONE("hpmu - uniforms")

    LLMatrix4a modelview;
    modelview.loadu(gRenderModelView);
    LLVector4a oa; // scratch space for transformed origin
    oa.set(0, 0, 0, 0);
    mHeroData.heroProbeCount = 1;

    if (mNearestHero != nullptr && !mNearestHero->isDead())
    {
        if (mNearestHero->getReflectionProbeIsBox())
        {
            LLVector3 s = mNearestHero->getScale().scaledVec(LLVector3(0.5f, 0.5f, 0.5f));
            mProbes[0]->mRadius = s.magVec();
        }
        else
        {
            mProbes[0]->mRadius = mNearestHero->getScale().mV[0] * 0.5f;
        }

        modelview.affineTransform(mProbes[0]->mOrigin, oa);
        mHeroData.heroShape = 0;
        if (!mProbes[0]->getBox(mHeroData.heroBox))
        {
            mHeroData.heroShape = 1;
        }

        mHeroData.heroSphere.set(oa.getF32ptr());
        mHeroData.heroSphere.mV[3] = mProbes[0]->mRadius;
    }

    llassert(mMipChain.size() <= size_t(S32_MAX));
    // <FS> the planar mirror: a negative mip count tells the shaders to read mirrorView at the pixel's own position instead of the cube (no change to the uniform block)
    mHeroData.heroMipCount = ll_mirror_planar() ? -1 : S32(mMipChain.size());
}

void LLHeroProbeManager::renderDebug()
{
    gDebugProgram.bind();
    // <FS:Beq> Add a bit more metadata to the probe debug view
    std::map<LLSpatialGroup*, int>  groupCount;
    std::map<LLViewerObject*, int>  objCount;
    std::map<F32*,            int>  locCount;

    for (LLReflectionMap* probe : mProbes)
    {
        if (!probe->isRelevant()) continue;
        groupCount[ probe->mGroup ]++;
        objCount[ probe->mViewerObject ]++;
        locCount[ probe->mOrigin.getF32ptr() ]++;
    }
    // </FS:Beq>
    for (auto& probe : mProbes)
    {
        renderReflectionProbe(probe, groupCount, objCount, locCount);    // <FS:Beq/> Add a bit more metadata to the probe debug view
    }

    gDebugProgram.unbind();
}


void LLHeroProbeManager::initReflectionMaps()
{
    U32 count = LL_MAX_HERO_PROBE_COUNT;

    if ((mTexture.isNull() || mReflectionProbeCount != count || mReset) && LLPipeline::RenderMirrors)
    {

        if (mReset)
        {
            cleanup();
        }

        mReset = false;
        mReflectionProbeCount = count;
        mProbeResolution      = gSavedSettings.getS32("RenderHeroProbeResolution");
        mMaxProbeLOD = log2f((F32)mProbeResolution) - 1.f; // number of mips - 1

        mTexture = new LLCubeMapArray();

        static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);

        // store mReflectionProbeCount+2 cube maps, final two cube maps are used for render target and radiance map generation source)
        mTexture->allocate(mProbeResolution, 3, mReflectionProbeCount + 2, true, render_hdr);

        if (mDefaultProbe.isNull())
        {
            llassert(mProbes.empty()); // default probe MUST be the first probe created
            mDefaultProbe = new LLReflectionMap();
            mProbes.push_back(mDefaultProbe);
        }

        llassert(mProbes[0] == mDefaultProbe);

        // For hero probes, we treat this as the main mirror probe.

        mDefaultProbe->mCubeIndex = 0;
        mDefaultProbe->mCubeArray = mTexture;
        mDefaultProbe->mDistance  = gSavedSettings.getF32("RenderHeroProbeDistance");
        mDefaultProbe->mRadius = 4096.f;
        mDefaultProbe->mProbeIndex = 0;
        touch_default_probe(mDefaultProbe);

        mProbes.push_back(mDefaultProbe);
    }

    if (mVertexBuffer.isNull())
    {
        U32 mask = LLVertexBuffer::MAP_VERTEX;
        LLPointer<LLVertexBuffer> buff = new LLVertexBuffer(mask);
        buff->allocateBuffer(4, 0);

        LLStrider<LLVector3> v;

        buff->getVertexStrider(v);

        v[0] = LLVector3(-1, -1, -1);
        v[1] = LLVector3(1, -1, -1);
        v[2] = LLVector3(-1, 1, -1);
        v[3] = LLVector3(1, 1, -1);

        buff->unmapBuffer();

        mVertexBuffer = buff;
    }
}

void LLHeroProbeManager::cleanup()
{
    mVertexBuffer = nullptr;
    mRenderTarget.release();

    mMipChain.clear();

    mTexture = nullptr;

    mProbes.clear();

    mDefaultProbe = nullptr;
}

void LLHeroProbeManager::doOcclusion()
{
    LLVector4a eye;
    eye.load3(LLViewerCamera::instance().getOrigin().mV);

    for (auto& probe : mProbes)
    {
        if (probe != nullptr)
        {
            probe->hizCollect(eye);
        }
    }
}

void LLHeroProbeManager::reset()
{
    mReset = true;
    mMirrorLastFrame = 0;       // the planar view is rendered again at once (its target may have been made again)
}

bool LLHeroProbeManager::registerViewerObject(LLVOVolume* drawablep)
{
    llassert(drawablep != nullptr);

    if (std::find(mHeroVOList.begin(), mHeroVOList.end(), drawablep) == mHeroVOList.end())
    {
        // Probe isn't in our list for consideration.  Add it.
        mHeroVOList.push_back(drawablep);
        return true;
    }

    return false;
}

void LLHeroProbeManager::unregisterViewerObject(LLVOVolume* drawablep)
{
    std::vector<LLPointer<LLVOVolume>>::iterator found_itr = std::find(mHeroVOList.begin(), mHeroVOList.end(), drawablep);
    if (found_itr != mHeroVOList.end())
    {
        mHeroVOList.erase(found_itr);
    }
    // <FS:Beq> FIRE-34587 - Bugsplat crash in getIsDynamic due to lifetime management of hero probe and underlying objects
    // If the unregistered object is mNearestHero, reset mNearestHero and mDefaultProbe->mViewerObject
    if (drawablep == mNearestHero)
    {
        mNearestHero = nullptr;
        if (mDefaultProbe.notNull())
        {
            mDefaultProbe->mViewerObject = nullptr;
        }
    }
    // </FS:Beq>

}
