/**
 * @file lldrawpoolalpha.cpp
 * @brief LLDrawPoolAlpha class implementation
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

#include "lldrawpoolalpha.h"
#include "llviewerpartsim.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <unordered_set>
#include "llvkplatform.h"

#include "llrenderheaders.h"
#include "llviewercontrol.h"
#include "llcriticaldamp.h"
#include "llfasttimer.h"
#include "llrender.h"

#include "llcubemap.h"
#include "llsky.h"
#include "lldrawable.h"
#include "llface.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"    // For debugging
#include "llviewerobjectlist.h" // For debugging
#include "llviewerwindow.h"
#include "llitemcensus.h"
#include "pipeline.h"
#include "llvkbridge.h"          // <FS> rtShadowMode()
#include "llalphatransient.h"      // <FS> S5f F3
#include "llviewershadermgr.h"
#include "llviewerregion.h"
#include "lldrawpoolwater.h"
#include "llspatialpartition.h"
#include "llrendercommonfunc.h"
#include "llvoavatar.h"
#include "gltfscenemanager.h"

#include "llenvironment.h"
#include "glm/gtc/type_ptr.hpp"

bool LLDrawPoolAlpha::sShowDebugAlpha = false;
bool LLDrawPoolAlpha::sShowDebugAlphaRigged = false;

#define current_shader (LLShaderProgram::sCurBoundShaderPtr)

LLVector4 LLDrawPoolAlpha::sWaterPlane;

// minimum alpha before discarding a fragment
static const F32 MINIMUM_ALPHA = 0.004f; // ~ 1/255

// minimum alpha before discarding a fragment when rendering impostors
static const F32 MINIMUM_IMPOSTOR_ALPHA = 0.1f;

LLDrawPoolAlpha::LLDrawPoolAlpha(U32 type) :
        LLRenderPass(type),
        mColorSFactor(LLRender::BF_UNDEF), mColorDFactor(LLRender::BF_UNDEF),
        mAlphaSFactor(LLRender::BF_UNDEF), mAlphaDFactor(LLRender::BF_UNDEF)
{

}

LLDrawPoolAlpha::~LLDrawPoolAlpha()
{
}


void LLDrawPoolAlpha::prerender()
{
    mShaderLevel = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_OBJECT);
}

S32 LLDrawPoolAlpha::getNumPostDeferredPasses()
{
    return 1;
}

// set some common parameters on the given shader to prepare for alpha rendering
extern S32 rrAlphaSunOld();    // pipeline.cpp: test switch, 1 = the old one hard sun ray

// <FS> U1 (pipeline.cpp: LL_VK_PASSUNI, _VERIFY, _AB)
extern bool ll_pass_uni_on();
extern bool ll_rigged_indexed();       // lldrawpool.cpp (S4 A1)
extern bool ll_pass_uni_verify();
extern U64 gPassUniAlphaMade, gPassUniAlphaSkipped, gPassUniAlphaMismatch;
static U32 sAlphaEpoch = 1;

static void prepare_alpha_shader(LLShaderProgram* shader, bool deferredEnvironment, F32 water_sign)
{
    static LLCachedControl<F32> displayGamma(gSavedSettings, "RenderDeferredDisplayGamma");
    F32 gamma = displayGamma;

    static LLStaticHashedString waterSign("waterSign");

    // Does this deferred shader need environment uniforms set such as sun_dir, etc. ?
    // NOTE: We don't actually need a gbuffer since we are doing forward rendering (for transparency) post deferred rendering
    // TODO: bindDeferredShader() probably should have the updating of the environment uniforms factored out into updateShaderEnvironmentUniforms()
    // i.e. shaders\class1\deferred\alphaF.glsl
    if (deferredEnvironment)
    {
        shader->mCanBindFast = false;
    }
    // <FS> the environment uniforms of the program (the eye space water and mirror planes: they depend on the modelview of the view being drawn) are made again now, for THIS pass: the first bind after an environment
    // update takes them with whatever view was current then (an avatar impostor's camera, a probe), and the program would keep them for the main view's pass (leaves cut by a plane of another view)
    shader->mUniformsDirty = true;
    shader->bind();
    shader->uniform1f(LLShaderMgr::DISPLAY_GAMMA, (gamma > 0.1f) ? 1.0f / gamma : (1.0f / 2.2f));

    if (LLVkBridge::rtShadowMode())
    {
        // <FS> ray traced sun shadows on alpha blended surfaces (hair, glass, flat decals like a logo on the floor): these shaders used to leave the sun's size and the sample count at 0 and trace ONE HARD ray, so
        // the shadow of a leaf fell crisp on the hair while the opaque surfaces next to it (skin, clothes, the ground) get the soft sun disc of the lightmap, accumulated and blurred. They get the same
        // sun disc now, with more rays per pixel because nothing blurs or accumulates what they trace. Test switch (third number of rr-debug.txt): 1 = the old single hard ray (set explicitly: uniforms
        // stay in the program), 0 and 2 = a disc twice the sun's (the default), 3 / 4 = 4 / 8 times with more rays (the lightmap's blur softens the ground more than the sun disc alone does).
        const S32 alpha_sun = rrAlphaSunOld();
        static LLCachedControl<F32> rt_softness(gSavedSettings, "RenderRTShadowSoftness", 0.35f);
        static LLCachedControl<S32> rt_samples(gSavedSettings, "RenderRTShadowSamples", 1);
        static LLStaticHashedString radius_s("rt_sun_radius"), samples_s("rt_sun_samples"), frame_s("rt_noise_frame");
        F32 scale = 2.f;                    // chosen by eye (Adam, plaza: a logo on the floor next to the ground): twice the sun disc, 6 rays per sample setting
        static LLCachedControl<S32> alpha_rays(gSavedSettings, "RenderRTShadowAlphaRays", 1);
        S32 samples = alpha_rays() > 0 ? alpha_rays() : rt_samples() * 6;      // (one jittered ray by default: doc/vulkan/MAINTHREAD-candidates.md, the alpha sun ray count; 0 = the old 12)
        if (alpha_sun == 3) { scale = 4.f; samples = rt_samples() * 6; }
        else if (alpha_sun == 4) { scale = 8.f; samples = rt_samples() * 6; }
        else if (alpha_sun >= 5 && alpha_sun <= 8) { samples = alpha_sun == 5 ? 6 : (alpha_sun == 6 ? 3 : (alpha_sun == 7 ? 2 : 1)); }       // test (rr-debug.txt third number): the default disc with 6 / 3 / 2 rays, 8: ONE ray on the disc (a different point of it every frame, the upscaler averages)
        shader->uniform1f(radius_s, alpha_sun == 1 ? 0.f : tanf(llclamp(rt_softness() * scale, 0.f, 20.f) * DEG_TO_RAD));
        shader->uniform1i(samples_s, alpha_sun == 1 ? 1 : llclamp(samples, 1, 16));
        {
            const S32 noise = LLVkBridge::rtSunNoise();     // rt-debug.txt sunnoise: 1 every frame (default), 2 every 4th frame, 0 a fixed pattern (the same key as the sun lightmap pass)
            shader->uniform1f(frame_s, (LLPipeline::sDLSS && noise != 0) ? (F32)((noise == 2 ? (gPipeline.mJitterIndex / 4) : gPipeline.mJitterIndex) % 64) : 0.f);
        }
        static LLCachedControl<F32> rt_lod_near(gSavedSettings, "RenderRTShadowAlphaLodNear", 4.f);
        static LLCachedControl<F32> rt_lod_far(gSavedSettings, "RenderRTShadowAlphaLodFar", 20.f);
        static LLStaticHashedString lod_s("rt_sun_lod");
        shader->uniform2f(lod_s, alpha_sun == 1 ? 0.f : rt_lod_near(), rt_lod_far());
        static LLCachedControl<F32> rt_max_dist(gSavedSettings, "RenderRTShadowAlphaMaxDistance", 48.f);
        static LLStaticHashedString maxdist_s("rt_sun_maxdist");
        shader->uniform1f(maxdist_s, rt_max_dist());
        static LLStaticHashedString alpha_pass_s("rt_sun_alpha");
        shader->uniform1f(alpha_pass_s, 1.f);        // <FS> a blended surface (hair): its sun ray starts off the surface by LL_RT_SUN_BIAS_ALPHA, not LL_RT_SUN_BIAS (cards must not shade the cards next to them)
        static LLStaticHashedString self_off_s("rt_sun_self_off");
        const S32 self = LLVkBridge::rtSunSelfShadow();
        shader->uniform1f(self_off_s, self == 0 ? 1.f : (self == 2 ? 2.f : 0.f));      // rt-debug.txt sunself=0: the avatar self shadow work off, 2: on with the full hair walk (same-lap cost A/Bs)
        static LLStaticHashedString pen_rays_s("rt_sun_pen_rays");
        {
            // <FS> the rays of a pixel whose first ray met a close skinned surface (hair on the face): 2 while the upscaler accumulates the noise (laps of 2026-11-03: sun program -0.5 ms, rigged alpha -0.9 ms, Adam: fine by eye),
            // the shader's 8 without it (a snapshot, no upscaler). rt-debug.txt penrays=N overrides (8 = the old number)
            extern bool gSnapshot;
            const S32 pen = LLVkBridge::rtSunPenRays();
            shader->uniform1i(pen_rays_s, pen > 0 ? pen : ((LLPipeline::sDLSS && !gSnapshot) ? 2 : 0));
        }
    }

    if (LLPipeline::sRenderingHUDs)
    { // for HUD attachments, only the pre-water pass is executed and we never want to clip anything
        LLVector4 near_clip(0, 0, -1, 0);
        shader->uniform1f(waterSign, 1.f);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, near_clip.mV);
    }
    else
    {
        shader->uniform1f(waterSign, water_sign);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
    }

    if (LLPipeline::sImpostorRender)
    {
        shader->setMinimumAlpha(MINIMUM_IMPOSTOR_ALPHA);
    }
    else
    {
        shader->setMinimumAlpha(MINIMUM_ALPHA);
    }
    //also prepare rigged variant
    if (shader->mRiggedVariant && shader->mRiggedVariant != shader)
    {
        prepare_alpha_shader(shader->mRiggedVariant, deferredEnvironment, water_sign);
    }
}

extern bool gCubeSnapshot;

void LLDrawPoolAlpha::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    if (LLPipeline::isWaterClip() && getType() == LLDrawPool::POOL_ALPHA_PRE_WATER)
    { // don't render alpha objects on the other side of the water plane if water is opaque
        return;
    }

    F32 water_sign = 1.f;

    if (getType() == LLDrawPool::POOL_ALPHA_PRE_WATER)
    {
        water_sign = -1.f;
    }

    if (LLPipeline::sUnderWaterRender)
    {
        water_sign *= -1.f;
    }

    // prepare shaders
    llassert(LLPipeline::sRenderDeferred);

    // <FS> S5g: the pull programs are primed (prepare_alpha_shader) by renderAlpha when a run first binds them in a pass: only the programs a pass uses, in that pass's own view
    mWaterSign = water_sign;

    // explicitly unbind here so render loop doesn't make assumptions about the last shader
    // already being setup for rendering
    LLShaderProgram::unbind();

    if (!LLPipeline::sRenderingHUDs)
    {
        // first pass, render rigged objects only and render to depth buffer
        forwardRender(true);
    }

    // second pass, regular forward alpha rendering
    forwardRender();

    if (mFringePending)
    {   // <FS> the soft part of the rigged alpha (below the depth cutoff), over the static alpha it was left out of: see forwardRender
        mFringePending = false;
        forwardRender(true, true);
    }

    // final pass, render to depth for depth of field effects
    // (<FS> not when ray reconstruction is on: the depth of field is skipped there (postProcess: !rr_done), so this pass would draw all the alpha again for a depth nothing reads)
    if (!LLPipeline::sImpostorRender && LLPipeline::RenderDepthOfField && !gCubeSnapshot && !LLPipeline::sRenderingHUDs && getType() == LLDrawPool::POOL_ALPHA_POST_WATER && !gPipeline.rayReconstructionActive())
    {
        // mask off color buffer writes as we're only writing to depth buffer
        gRender.setColorMask(false, false);

        // If the face is more than 90% transparent, then don't update the Depth buffer for Dof
        // We don't want the nearly invisible objects to cause of DoF effects
        renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2,
            true); // <--- discard mostly transparent faces

        gRender.setColorMask(true, false);
    }
}

// <FS> is there any static alpha blended draw to be drawn after the rigged ones (foliage, glass, particles...)?
bool LLDrawPoolAlpha::hasStaticAlpha()
{
    for (LLCullResult::sg_iterator i = gPipeline.beginAlphaGroups(); i != gPipeline.endAlphaGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        if (group && !group->isDead() && !group->mDrawMap[LLRenderPass::PASS_ALPHA].empty())
        {
            return true;
        }
    }
    return false;
}

void LLDrawPoolAlpha::forwardRender(bool rigged, bool fringe)
{
    gPipeline.enableLightsDynamic();

    LLRenderPresetPipelineAlpha gls_pipeline_alpha;

    //enable writing to alpha for emissive effects
    gRender.setColorMask(true, true);

    // <FS> Hair halo. The rigged pass writes depth so that rigged layers hide each other and the static alpha drawn after it (foliage behind the avatar) is cut away
    // behind them, but every fragment above 1/255 alpha wrote it: the nearly transparent texels around hair strands then cut a halo of the mesh edge out of the foliage
    // behind. With static alpha to draw afterwards the rigged alpha is now drawn in three steps: (1) a depth only pass that writes depth just where the texture alpha
    // reaches the cutoff, (2) the colour of those texels (alpha >= cutoff) against that depth, (3) after the static alpha, the colour of the soft texels (alpha below
    // the cutoff) over everything, rejected where step 1 wrote depth (same triangles, drawn a little farther, depth test LESS). Foliage behind the soft part of the hair is
    // then under it, as it should be, and the cutoff can stay high enough that semi transparent front hair does not hide the layers behind it.
    static LLCachedControl<F32> rigged_depth_cutoff(gSavedSettings, "RenderRiggedAlphaDepthCutoff", 0.33f);
    // The depth only pass (RenderRiggedAlphaDepthPrepass, off by default: it skins the rigged alpha a second time, 1 ms of GPU in a crowd) makes the depth of the rigged layers independent of their
    // draw order; without it the colour pass itself writes depth for the texels at or above the cutoff, in draw order, as the viewer always did (with the cutoff instead of 1/255).
    static LLCachedControl<bool> rigged_prepass(gSavedSettings, "RenderRiggedAlphaDepthPrepass", false);
    const bool split = rigged && !fringe && rigged_depth_cutoff() > 0.f && !LLPipeline::sImpostorRender && !gCubeSnapshot && !LLPipeline::sRenderingHUDs && hasStaticAlpha();
    const bool depth_prepass = split && rigged_prepass();
    if (split)
    {
        mFringePending = true;
        mDepthCutoff = rigged_depth_cutoff();
        mDepthCutoffShaders.clear();
    }

    bool write_depth = (rigged && !depth_prepass && !fringe) ||
        LLDrawPoolWater::sSkipScreenCopy
        // we want depth written so that rendered alpha will
        // contribute to the alpha mask used for impostors
        || LLPipeline::sImpostorRenderAlphaDepthPass
        || getType() == LLDrawPoolAlpha::POOL_ALPHA_PRE_WATER; // needed for accurate water fog
    if (fringe)
    {
        write_depth = false;
    }

    LLRenderState::DepthScope depth(true, write_depth ? true : false, fringe ? LLRenderState::CompareOp::Less : LLRenderState::CompareOp::LessEqual);

    mColorSFactor = LLRender::BF_SOURCE_ALPHA;           // } regular alpha blend
    mColorDFactor = LLRender::BF_ONE_MINUS_SOURCE_ALPHA; // }
    mAlphaSFactor = LLRender::BF_ZERO;                         // } glow suppression
    mAlphaDFactor = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;       // }
    gRender.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);

    if (rigged && !fringe && mType == LLDrawPool::POOL_ALPHA_POST_WATER)
    { // draw GLTF scene to depth buffer before rigged alpha
        LL_GPU_TIMER_ZONE("alpha: gltf scene depth"); // <FS>
        LL::GLTFSceneManager::instance().render(false, false);
        LL::GLTFSceneManager::instance().render(false, true);
        LL::GLTFSceneManager::instance().render(false, false, true);
        LL::GLTFSceneManager::instance().render(false, true, true);
    }

    if (depth_prepass)
    {
        LL_GPU_TIMER_ZONE("alpha: rigged depth");
        gRender.setColorMask(false, false);
        {
            LLRenderState::DepthScope prepass_depth(true, true);
            // the colour pass draws the same triangles and tests with LEQUAL: written a hair farther so that no rounding between the two passes makes them fail
            LLRenderState::PolygonOffsetScope offset(true);
            gRender.setPolygonOffset(1.f, 4.f);
            renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2, true, true);
        }
        gRender.setColorMask(true, true);
        gRender.setPolygonOffset(0.f, 0.f);

        LLShaderProgram::unbind();
    }

    // If the face is more than 90% transparent, then don't update the Depth buffer for Dof
    // We don't want the nearly invisible objects to cause of DoF effects
    {
        // <FS> GPU timer zone per alpha pass
        LL_GPU_TIMER_ZONE(fringe ? "alpha: rigged fringe" : rigged ? (mType == LLDrawPool::POOL_ALPHA_POST_WATER ? "alpha: rigged (post water)" : "alpha: rigged (pre water)")
                                 : (mType == LLDrawPool::POOL_ALPHA_POST_WATER ? "alpha: static (post water)" : "alpha: static (pre water)"));
        // the soft part: drawn a little farther than the depth only pass wrote the same triangles, so that what wrote depth there fails the strict test
        LLRenderState::PolygonOffsetScope fringe_offset(true, fringe);
        // F3.5c: this alpha pass writes no depth (unless write_depth): its draws sample the depth buffer (soft particles, water fog) and the pass takes the read-only depth layout
        LLRenderState::DepthReadOnlyScope depth_read_only(true, !write_depth);
        if (fringe)
        {
            gRender.setPolygonOffset(1.f, 8.f);
            mFringePass = true;
        }
        renderAlpha(getVertexDataMask() | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2, false, rigged);
        mFringePass = false;
        if (fringe)
        {
            gRender.setPolygonOffset(0.f, 0.f);
        }
    }

    if (split)
    {   // every shader that drew with the cutoff goes back to the usual one (the static alpha and the soft part do not set it again)
        LLShaderProgram::unbind();
        mDepthCutoff = 0.f;
        for (LLShaderProgram* shader : mDepthCutoffShaders)
        {
            shader->bind();
            shader->setMinimumAlpha(MINIMUM_ALPHA);
        }
        mDepthCutoffShaders.clear();
        LLShaderProgram::unbind();
    }

    gRender.setColorMask(true, false);

    if (!rigged && (LLPipeline::sRenderingHUDs || getType() == LLDrawPoolAlpha::POOL_ALPHA_POST_WATER))
    { //render "highlight alpha" on final non-rigged pass for non-HUDs (HUDs only run pre-water alpha pass)
        // NOTE -- hacky call here protected by !rigged instead of alongside "forwardRender"
        // so renderDebugAlpha is executed while gls_pipeline_alpha and depth GL state
        // variables above are still in scope
        renderDebugAlpha();
    }
}

void LLDrawPoolAlpha::renderDebugAlpha()
{
    if (sShowDebugAlpha && !gCubeSnapshot && !LLPipeline::sReflectionRender)
    {
        gHighlightProgram.bind();
        gRender.diffuseColor4f(1, 0, 0, 1);
        gRender.getTexUnit(0)->bindFast(LLViewerFetchedTexture::getSmokeImage());


        renderAlphaHighlight();

        pushUntexturedBatches(LLRenderPass::PASS_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_ALPHA_INVISIBLE);

        // Material alpha mask
        gRender.diffuseColor4f(0, 0, 1, 1);
        pushUntexturedBatches(LLRenderPass::PASS_MATERIAL_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_NORMMAP_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_SPECMAP_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_NORMSPEC_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK);
        pushUntexturedBatches(LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK);

        gRender.diffuseColor4f(0, 1, 0, 1);
        pushUntexturedBatches(LLRenderPass::PASS_INVISIBLE);
        // <FS:Beq> FIRE-32132 et al. Allow rigged mesh transparency highlights to be toggled
        if (sShowDebugAlphaRigged)
        {
        // </FS:Beq>
        gHighlightProgram.mRiggedVariant->bind();
        gRender.diffuseColor4f(0, 1, 0, 1);// <FS:Beq/> FIRE-32132 et al. (can plain PASS_ALPHA_MASK_RIGGED exist?) paint it green if so.
        pushRiggedBatches(LLRenderPass::PASS_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_ALPHA_INVISIBLE_RIGGED, false);

        // Material alpha mask
        gRender.diffuseColor4f(0, 1, 1, 1);// <FS:Beq/> FIRE-32132 et al. Allow rigged mesh transparency highlights to be toggled
        pushRiggedBatches(LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_NORMMAP_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_SPECMAP_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_NORMSPEC_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, false);
        pushRiggedBatches(LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK_RIGGED, false);

        gRender.diffuseColor4f(0, 1, 0, 1);
        pushRiggedBatches(LLRenderPass::PASS_INVISIBLE_RIGGED, false);
        // <FS:Beq> FIRE-32132 et al. Allow rigged mesh transparency highlights to be toggled
        }
        // </FS:Beq>
        LLShaderProgram::sCurBoundShaderPtr->unbind();
    }
}

void LLDrawPoolAlpha::renderAlphaHighlight()
{
    for (int pass = 0; pass < 2; ++pass)
    { //two passes, one rigged and one not
        const LLVOAvatar* lastAvatar = nullptr;
        U64 lastMeshId = 0;
        bool skipLastSkin = false;

        LLCullResult::sg_iterator begin = pass == 0 ? gPipeline.beginAlphaGroups() : gPipeline.beginRiggedAlphaGroups();
        LLCullResult::sg_iterator end = pass == 0 ? gPipeline.endAlphaGroups() : gPipeline.endRiggedAlphaGroups();

        for (LLCullResult::sg_iterator i = begin; i != end; ++i)
        {
            LLSpatialGroup* group = *i;
            if (group->getSpatialPartition()->mRenderByGroup &&
                !group->isDead())
            {
                LLSpatialGroup::drawmap_elem_t& draw_info = group->mDrawMap[LLRenderPass::PASS_ALPHA+pass]; // <-- hacky + pass to use PASS_ALPHA_RIGGED on second pass

                for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
                {
                    LLDrawInfo& params = **k;

                    bool rigged = (params.mAvatar != nullptr);
                    gHighlightProgram.bind(rigged);

                    if (rigged)
                    {
                        if (!uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                        { // failed to upload matrix palette, skip rendering
                            continue;
                        }
                    }

                    // <FS:Beq> FIRE-32132 et al. Allow rigged mesh transparency highlights to be toggled
                    if (rigged && !sShowDebugAlphaRigged)
                    {
                        // if we don't want to show rigged alpha highlights then skip
                        continue;
                    }
                    else if (rigged && sShowDebugAlphaRigged)
                    {
                        // if we do and this is rigged then use a different colour
                        gRender.diffuseColor4f(1, 0.5, 0, 1);
                    }
                    else // NB dangling else to drop through to "normal behaviour"
                    // </FS:Beq>
                    gRender.diffuseColor4f(1, 0, 0, 1);
                    LLRenderPass::applyModelMatrix(params);
                    params.mVertexBuffer->setBuffer();
                    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);
                }
            }
        }
    }

    // make sure static version of highlight shader is bound before returning
    gHighlightProgram.bind();
}

extern U32 gFrameCount;

// <FS> GPU particles G1: the order of a record store is written by the GPU (default on since 2026-11-02; LL_VK_GPU_PARTICLE_SORT=0 sorts on the CPU; implies the shader evaluation of G0)
bool ll_particle_sort_on()
{
    static const bool on = []() { const char* v = getenv("LL_VK_GPU_PARTICLE_SORT"); return !v || atoi(v) != 0; }();
    return on;
}

// <FS> S5a: what the alpha loop sent to the scene table (log tag VkScene, every 600 frames)
static struct AlphaSceneStats
{
    U64 mCalls = 0, mRecords = 0, mRanges = 0, mServed = 0, mDropped = 0, mMismatch = 0, mBytes = 0, mGlowServed = 0, mTrRecords = 0, mTrEntries = 0;
    U32 mLast = 0;
} sAlphaScene;

// item E (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the rigged alpha draws of the world's colour pass (and its depth passes) draw a simplified index list when the avatar is far enough that the error of the
// simplification is under half a pixel at its distance (a hair card with 100,000 triangles for a few hundred pixels). The lists are made on a background thread (item A's cache: ll_rigged_lod_lookup in
// pipeline.cpp); until one is ready the draw's own list is used. LL_VK_RIGGEDLOD=1 starts it on, rt-debug.txt riggedlod=0/1 switches it live (a same-lap A/B).
extern bool ll_rigged_lod_lookup(const LLDrawInfo* p, F32 error_m, U32& ib_name, U32& count);
static bool rigged_lod_wanted(bool opaque)
{
    // (the alpha draws and the scene table's opaque rigged draws have a key each: riggedlod= and riggedlodopq=, so a lap can switch one of them)
    static const bool env[2] = { []() { const char* e = getenv("LL_VK_RIGGEDLOD"); return !e || atoi(e) != 0; }(),         // on unless LL_VK_RIGGEDLOD=0 (Adam judged the picture: fine, 2026-11-04)
                                 []() { const char* e = getenv("LL_VK_RIGGEDLODOPQ"); return !e || atoi(e) != 0; }() };   // the opaque ones: on unless LL_VK_RIGGEDLODOPQ=0 (not judged yet)
    static S32 live[2] = { -1, -1 };
    static std::chrono::steady_clock::time_point last;
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::milliseconds(500))
    {
        last = now;
        S32 v[2] = { -1, -1 };
        std::ifstream in("<work>/rt-debug.txt");
        std::string line;
        while (std::getline(in, line))
        {
            if (line.compare(0, 10, "riggedlod=") == 0)
            {
                v[0] = atoi(line.c_str() + 10);
            }
            else if (line.compare(0, 13, "riggedlodopq=") == 0)
            {
                v[1] = atoi(line.c_str() + 13);
            }
        }
        live[0] = v[0];
        live[1] = v[1];
    }
    const int i = opaque ? 1 : 0;
    return live[i] >= 0 ? live[i] != 0 : env[i];
}

static bool rigged_lod_for(const LLDrawInfo* info, U32& ib_name, U32& count, bool opaque = false)
{
    ib_name = 0;
    count = 0;
    extern bool gSnapshot;
    if (!rigged_lod_wanted(opaque) || gSnapshot || LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return false;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (camera->getZoomFactor() > 1.f)
    {
        return false;
    }
    LLVOAvatar* avatar = info->mAvatar.get();
    if (!avatar || avatar->isSelf() || avatar->mDrawable.isNull())
    {
        return false;
    }
    const F32 distance = avatar->mDrawable->mDistanceWRTCamera - 1.5f;       // (to the nearest part of the avatar, not its centre)
    if (distance < 3.f)
    {
        return false;
    }
    const F32 height = (F32)gViewerWindow->getWorldViewHeightRaw();
    const F32 pixel = 2.f * distance * tanf(0.5f * camera->getView()) / llmax(height, 1.f);        // the world size of a pixel of the window at that distance
    return ll_rigged_lod_lookup(info, 0.5f * pixel, ib_name, count);
}

// the scene table's opaque rigged items (pipeline.cpp, the frame record's palette bindings)
bool ll_rigged_lod_opaque(const LLDrawInfo* info, U32& ib_name, U32& count)
{
    return rigged_lod_for(info, ib_name, count, true);
}

void LLDrawPoolAlpha::renderAlpha(U32 mask, bool depth_only, bool rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    ++sAlphaEpoch;      // <FS> U1: what the loop below last wrote to a program is not known any more
    const bool alpha_values_on = ll_pass_uni_on();
    // <FS> S5g (doc/vulkan/S5-design.md): the scene table is the ONLY alpha path. The main view's passes draw the groups' plans from the fragments (one record per run, in the loop's order), every other draw as a
    // transient entry; a draw that neither can draw is not drawn (counted, logged once by ll_alpha_transient)
    const bool post_water = getType() == LLDrawPool::POOL_ALPHA_POST_WATER;
    // diagnostics (crash / blink hunt 2026-10-22): LL_VK_ALPHA_NO_FRAGMENTS=1 draws every alpha draw as a transient entry (the fragments' plans are not used), LL_VK_ALPHA_NO_TRANSIENT=1 draws no transient entry (what only they draw is missing)
    static const bool dbg_no_fragments = getenv("LL_VK_ALPHA_NO_FRAGMENTS") != nullptr, dbg_no_transient = getenv("LL_VK_ALPHA_NO_TRANSIENT") != nullptr;
    const bool alpha_table = !dbg_no_fragments && !LLPipeline::sImpostorRender && !LLPipeline::sRenderingHUDs && !gCubeSnapshot && !LLPipeline::sReflectionRender && (!rigged || (gAlphaRiggedPullReady && ll_rigged_indexed())) && ll_alpha_serving();       // S5d: the rigged passes (colour and fringe) too
    // S5f F4: the draws the fragments do not keep are sent as transient entries in EVERY view that has its pull programs (HUD, impostor render, probe and reflection renders, the depth only passes): a pass
    // that is not the main view's has no fragments to serve (alpha_table false) but draws all of its draws that way
    const bool transient_ok = !dbg_no_transient && ll_alpha_ab_on() && (!rigged || (gAlphaRiggedPullReady && ll_rigged_indexed())) && !(LLPipeline::sRenderingHUDs && rigged);
    struct PendingRange { U32 mHandle, mFirst, mCount; };
    struct PendingSrc { const LLDrawInfo* const* mInfo; const U32* mWire; U32 mRank, mCount, mHandle; };       // (S5d: where the items of a rigged range are, for their palette bindings)
    static thread_local std::vector<PendingRange> pending;
    static thread_local std::vector<PendingSrc> pending_src;
    pending.clear();
    pending_src.clear();
    // S5e: the glow second draw of the served alpha items, per group: ranges by class (0 plain, 1 glTF, 2 glTF double sided)
    static thread_local std::vector<PendingRange> glow_range[3];
    static thread_local std::vector<PendingSrc> glow_src[3];
    const bool glow_pass = post_water && !depth_only && !mFringePass;       // (the glow second draw is made by the colour passes only)
    const bool glow_plain_table = (alpha_table || transient_ok) && glow_pass && ll_alpha_glow_table(false);
    const bool glow_pbr_table = (alpha_table || transient_ok) && glow_pass && ll_alpha_glow_table(true);
    // S5f F1: the depth only passes (the depth of field depth pass: minimum alpha 0.33 on the mask program; the rigged depth prepass: mDepthCutoff) are served by the same runs: the lit key draws with the fullbright
    // (mask) pull program, as the old code sets simple_shader = fullbright_shader = the mask program for them
    const bool dof_pass = transient_ok && depth_only && mDepthCutoff <= 0.f;
    auto min_alpha_of = [&](LLShaderProgram* program) -> F32
    {
        if (mDepthCutoff > 0.f)
        {
            return mDepthCutoff;
        }
        return dof_pass && (program == &gDeferredFullbrightMaskPullProgram || program == &gDeferredFullbrightMaskPullSkinProgram) ? 0.33f : MINIMUM_ALPHA;
    };
    // item D1 (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the colour pass of the world draws the fullbright alpha draws with the lit program (ALPHA_UBER, the row's GEO_FULLBRIGHT flag), so a lit and a fullbright
    // draw in the blend order are one run. LL_VK_ALPHAUBER=1 starts it on, rt-debug.txt alphauber=0/1 switches it live (a same-lap A/B).
    static const bool uber_env = []() { const char* e = getenv("LL_VK_ALPHAUBER"); return !e || atoi(e) != 0; }();      // on unless LL_VK_ALPHAUBER=0 (Adam judged the picture: fine, 2026-11-04)
    static S32 uber_live = -1;
    // item D2: the merged program also draws the material alpha draws (the row's GEO_MATERIAL flags). LL_VK_ALPHAMAT=0 starts it off, rt-debug.txt alphamat=0/1 switches it live (a same-lap A/B against D1).
    static const bool mat_env = []() { const char* e = getenv("LL_VK_ALPHAMAT"); return !e || atoi(e) != 0; }();
    static S32 mat_live = -1;
    {
        static std::chrono::steady_clock::time_point uber_last;
        const auto uber_now = std::chrono::steady_clock::now();
        if (uber_now - uber_last >= std::chrono::milliseconds(500))
        {
            uber_last = uber_now;
            S32 v = -1, vm = -1;
            std::ifstream in("<work>/rt-debug.txt");
            std::string line;
            while (std::getline(in, line))
            {
                if (line.compare(0, 10, "alphauber=") == 0)
                {
                    v = atoi(line.c_str() + 10);
                }
                else if (line.compare(0, 9, "alphamat=") == 0)
                {
                    vm = atoi(line.c_str() + 9);
                }
            }
            uber_live = v;
            mat_live = vm;
        }
    }
    const bool uber_ctx = (uber_live >= 0 ? uber_live != 0 : uber_env) && gAlphaUberReady && (!rigged || gAlphaUberSkinReady) && !depth_only && !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender;
    // (the HUD draws all its plain draws with its fullbright program; the depth only passes draw lit and fullbright ones with the mask program)
    const bool mat_ctx = uber_ctx && (mat_live >= 0 ? mat_live != 0 : mat_env) && gAlphaUberMatReady && (!rigged || gAlphaUberMatSkinReady);
    auto run_key = [&](U32 key) -> int
    {
        if (uber_ctx && (key & 0xFF) == 1)
        {
            return (int)(key & ~0xFFu);         // a fullbright draw runs with the lit ones (the merged program)
        }
        if (mat_ctx && ((key & 0xFF) == 3 || (key & 0xFF) == 7 || (key & 0xFF) == 11 || (key & 0xFF) == 15))
        {
            return (int)(key & ~0xFFu);         // item D2: a material blend draw (the materials 1, 5, 9, 13: the program 2 + the mask) runs with the lit ones too
        }
        return (depth_only || LLPipeline::sRenderingHUDs) && (key & 0xFF) == 0 ? (int)((key & ~0xFFu) | 1u) : (int)key;
    };
    static thread_local std::vector<LLShaderProgram*> primed_programs;
    primed_programs.clear();
    bool pass_begun = false;
    LLShaderProgram* min_alpha_zero_program = nullptr;       // the pull program whose minimum alpha a custom blend run set to 0 (put back by the next run of it with a standard blend)
    int setup_key = -1;     // the key whose pull program is bound with its blend function, lights and values in place, since the last old draw or glow pass (anything that could change them clears it)
    // S5f F3: the draws the table does not keep (particle groups, a group without a current fragment, ...) of this pass as transient entries (kind 32): batched like the ranges of a run, in the pass's order (the pending
    // ranges and the pending entries are never both non-empty: whichever comes second flushes the other first)
    struct TransientBatch
    {
        std::vector<LLVkBridge::GeoEntry> mEntries;
        std::vector<F32> mMatrices, mTexmats, mPbr;
        std::vector<const LLMatrix4*> mMatrixKeys;
        std::vector<U8> mBlob;
    };
    if (ll_particle_sort_on() && !LLPipeline::sImpostorRender && !LLPipeline::sRenderingHUDs && !gCubeSnapshot && !LLPipeline::sReflectionRender && !depth_only && !rigged)
    {
        LLViewerPartSim::getInstance()->sortStores();       // G1: the GPU sorts the particle stores of the frame before the first main view alpha pass draws them
    }
    static thread_local TransientBatch tr;
    tr.mEntries.clear();
    tr.mMatrices.clear();
    tr.mTexmats.clear();
    tr.mPbr.clear();
    tr.mMatrixKeys.clear();
    const glm::mat4 tr_view = gRender.getModelviewMatrix();         // (what the old draws start from before their model matrix: the HUD and probe renders set it on the stack)
    const glm::mat4 tr_proj = gRender.getProjectionMatrix();
    // the matrices the shader would get from LLRender::syncMatrices() for a model matrix: modelview, modelview * projection, normal matrix (as the geometry packets of the deferred passes)
    auto tr_matrix = [&](const LLMatrix4* model) -> U32
    {
        if (tr.mMatrices.empty() || model)
        {
            for (size_t k = 0; k < tr.mMatrixKeys.size(); ++k)
            {
                if (tr.mMatrixKeys[k] == model)
                {
                    return (U32)k;
                }
            }
        }
        glm::mat4 mv = tr_view;
        if (model)
        {
            mv = tr_view * glm::make_mat4((const F32*)model->mMatrix);
        }
        const glm::mat4 mvp = tr_proj * mv;
        const glm::mat4 n = glm::transpose(glm::inverse(mv));
        const size_t at = tr.mMatrices.size();
        tr.mMatrices.resize(at + 44);
        memcpy(&tr.mMatrices[at], glm::value_ptr(mv), 64);
        memcpy(&tr.mMatrices[at + 16], glm::value_ptr(mvp), 64);
        for (int col = 0; col < 3; ++col)
        {
            F32* d = &tr.mMatrices[at + 32 + 4 * col];
            d[0] = n[col][0];
            d[1] = n[col][1];
            d[2] = n[col][2];
            d[3] = 0.f;
        }
        tr.mMatrixKeys.push_back(model);
        return (U32)(at / 44);
    };
    int pending_key = -1;
    const bool tr_rigged = rigged;
    auto tr_flush = [&]()
    {
        if (tr.mEntries.empty())
        {
            return;
        }
        ItemCensus::Scope census_scope(ItemCensus::ALPHA_TR_FLUSH);
        ItemCensus::add(ItemCensus::ALPHA_TR_FLUSH, tr.mEntries.size());
        const size_t entries_bytes = tr.mEntries.size() * sizeof(LLVkBridge::GeoEntry), matrix_bytes = tr.mMatrices.size() * sizeof(F32), texmat_bytes = tr.mTexmats.size() * sizeof(F32), pbr_bytes = tr.mPbr.size() * sizeof(F32);
        tr.mBlob.resize(entries_bytes + matrix_bytes + texmat_bytes + pbr_bytes);
        U8* at = tr.mBlob.data();
        memcpy(at, tr.mEntries.data(), entries_bytes);
        at += entries_bytes;
        memcpy(at, tr.mMatrices.data(), matrix_bytes);
        at += matrix_bytes;
        if (texmat_bytes)
        {
            memcpy(at, tr.mTexmats.data(), texmat_bytes);
            at += texmat_bytes;
        }
        if (pbr_bytes)
        {
            memcpy(at, tr.mPbr.data(), pbr_bytes);
        }
        LLVkBridge::ShadowPacketsHeader header;
        header.mKind = 32;
        header.mEntries = (U32)tr.mEntries.size();
        header.mMatrices = (U32)(tr.mMatrices.size() / 44);
        header.mFragment = (U32)(tr.mTexmats.size() / 16);
        header.mPaletteFloats = (U32)(tr.mPbr.size() / 40);
        header.mPass = tr_rigged ? 1u : 0u;
        if (tr_rigged)
        {
            ll_frame_palette_flush();       // (the shared palettes the entries point into go first)
        }
        {
            LLRenderState::CullScope cull_face(false, pending_key >= 0 && (((pending_key >> 16) & 1) != 0 || ((pending_key >> 18) & 1) != 0));       // a double sided glTF material, a particle group: culling off
            LLVkBridge::shadowPackets(header, tr.mBlob.data(), tr.mBlob.size());
        }
        ++sAlphaScene.mTrRecords;
        sAlphaScene.mTrEntries += tr.mEntries.size();
        sAlphaScene.mBytes += tr.mBlob.size();
        tr.mEntries.clear();
        tr.mMatrices.clear();
        tr.mTexmats.clear();
        tr.mPbr.clear();
        tr.mMatrixKeys.clear();
        pending_key = -1;
    };
    // an entry for the draw (its GL names made now: the old bind marks a texture in use, regenerates a deleted one and applies its sampler options, prepareFast does the same)
    auto tr_add = [&](const LLAlphaTransient& t, const LLDrawInfo& draw)
    {
        ItemCensus::Scope census_scope(ItemCensus::ALPHA_TR_ADD);
        ItemCensus::add(ItemCensus::ALPHA_TR_ADD, tr_rigged ? 1 : 0);
        if (tr.mMatrices.empty())
        {
            tr_matrix(nullptr);         // matrix 0: the view alone
        }
        LLVkBridge::GeoEntry e = t.mEntry;
        if (tr_rigged)
        {
            U32 offset = 0, joints = 0;
            if (!ll_frame_palette(draw.mAvatar.get(), draw.mSkinInfo.get(), offset, joints))
            {
                return;                 // the skin is not loaded yet: not drawn, as the old path
            }
            e.mPaletteOffset = offset;
            e.mJoints = joints;
        }
        U32* fields[6] = { &e.mTextures[0], &e.mTextures[1], &e.mTextures[2], &e.mTextures[3], &e.mNormalMap, &e.mSpecularMap };
        for (int k = 0; k < 6; ++k)
        {
            *fields[k] = t.mTextures[k] ? gRender.getTexUnit(0)->prepareFast(t.mTextures[k]) : 0u;
        }
        e.mMatrix = draw.mModelMatrix ? tr_matrix(draw.mModelMatrix) : 0u;
        if (t.mUseTexMatrix && draw.mTextureMatrix)
        {
            tr.mTexmats.insert(tr.mTexmats.end(), (const F32*)draw.mTextureMatrix->mMatrix, (const F32*)draw.mTextureMatrix->mMatrix + 16);
            e.mPad3 = (U32)(tr.mTexmats.size() / 16);
        }
        if (t.mPbr)
        {
            e.mPbrRow = (U32)(tr.mPbr.size() / 40);
            tr.mPbr.insert(tr.mPbr.end(), t.mPbrValues, t.mPbrValues + 40);
        }
        tr.mEntries.push_back(e);
    };
    // S5f F3: the glow second draw of the transient draws, per group by class (as glow_range for the fragment items): the draws, built again as entries at the glow pass
    static thread_local std::vector<LLDrawInfo*> glow_draws[3];
    auto run_flush = [&]()
    {
        tr_flush();
        if (pending.empty())
        {
            return;
        }
        ItemCensus::Scope census_scope(ItemCensus::ALPHA_RUN_FLUSH);
        ItemCensus::add(ItemCensus::ALPHA_RUN_FLUSH, pending.size());
        if (!pass_begun)
        {   // the pass's matrices, once: the runs that follow carry only their ranges
            F32 matrices[32];
            // the matrix stack's top (what the old draws started from, and what the transient entries use): gRenderModelView is the camera's copy; they must agree, and the log says when they do not
            memcpy(matrices, glm::value_ptr(gRender.getModelviewMatrix()), 16 * sizeof(F32));
            memcpy(matrices + 16, glm::value_ptr(gRender.getProjectionMatrix()), 16 * sizeof(F32));
            {
                F32 worst = 0.f;
                for (int k = 0; k < 16; ++k)
                {
                    worst = std::max(worst, fabsf(matrices[k] - gRenderModelView[k]));
                }
                static U32 said = 0;
                if (worst > 1e-5f && said++ < 20)
                {
                    LL_WARNS("VkScene") << "[alpha matrices] the stack's modelview differs from gRenderModelView by " << worst << " (rigged " << rigged << ", HUD " << LLPipeline::sRenderingHUDs << ", impostor " << LLPipeline::sImpostorRender << ")" << LL_ENDL;
                }
            }
            LLVkBridge::ShadowPacketsHeader begin_header;
            begin_header.mKind = 29;
            begin_header.mEntries = 0;
            LLVkBridge::shadowPackets(begin_header, matrices, sizeof(matrices));
            pass_begun = true;
            sAlphaScene.mBytes += sizeof(matrices);
        }
        if (rigged)
        {   // S5d: the palettes of this run's rigged items for the frame (the shared list: a skin once), bound to the items' rows before the run draws
            struct Binding { U32 mHandle, mItem, mOffset, mJoints, mLodIb, mLodCount; };       // (item E: the simplified list of the item this frame, 0 = its own)
            static thread_local std::vector<Binding> bindings;
            bindings.clear();
            LLVOAvatar* last_avatar = nullptr;
            const LLMeshSkinInfo* last_skin = nullptr;
            U32 last_offset = ~0u, last_joints = 0;
            for (const PendingSrc& src : pending_src)
            {
                for (U32 k = 0; k < src.mCount; ++k)
                {
                    const LLDrawInfo* info = src.mInfo[src.mRank + k];
                    LLVOAvatar* avatar = info->mAvatar.get();
                    const LLMeshSkinInfo* skin = info->mSkinInfo.get();
                    if (avatar != last_avatar || skin != last_skin)
                    {
                        last_avatar = avatar;
                        last_skin = skin;
                        U32 offset = 0, joints = 0;
                        if (ll_frame_palette(avatar, skin, offset, joints))
                        {
                            last_offset = offset;
                            last_joints = joints;
                        }
                        else
                        {
                            last_offset = ~0u;
                            last_joints = 0;
                        }
                    }
                    U32 lod_ib = 0, lod_count = 0;
                    if (last_offset != ~0u)
                    {
                        rigged_lod_for(info, lod_ib, lod_count);
                    }
                    bindings.push_back({ src.mHandle, src.mWire[src.mRank + k], last_offset, last_joints, lod_ib, lod_count });
                }
            }
            ll_frame_palette_flush();
            LLVkBridge::ShadowPacketsHeader palette_header;
            palette_header.mKind = 31;
            palette_header.mEntries = (U32)bindings.size();
            LLVkBridge::shadowPackets(palette_header, bindings.data(), bindings.size() * sizeof(Binding));
            sAlphaScene.mBytes += bindings.size() * sizeof(Binding);
        }
        LLVkBridge::ShadowPacketsHeader header;
        header.mKind = 29;
        header.mEntries = (U32)pending.size();
        header.mPass = rigged ? 1 : 0;
        header.mFragment = depth_only ? 2u : (mFringePass ? 1u : 0u);       // (the worker names the GPU time of the kinds of pass: 1 the rigged fringe, 2 a depth only pass)
        {
            LLRenderState::CullScope cull_face(false, ((pending_key >> 16) & 1) != 0 || ((pending_key >> 18) & 1) != 0);       // a double sided glTF material, a particle group: culling off for its run
            LLVkBridge::shadowPackets(header, pending.data(), pending.size() * sizeof(PendingRange));
        }
        ++sAlphaScene.mRecords;
        sAlphaScene.mRanges += pending.size();
        sAlphaScene.mBytes += pending.size() * sizeof(PendingRange);
        pending.clear();
        pending_src.clear();
        pending_key = -1;
    };
    auto add_range = [&](const LLAlphaPlanView& pv, const LLAlphaPlanSeg& seg)
    {
        pending.push_back({ pv.mHandle, seg.mRank, seg.mCount });
        if (rigged)
        {
            pending_src.push_back({ pv.mItemInfo, pv.mItemWire, seg.mRank, seg.mCount, pv.mHandle });
        }
    };
    // S5e: a served draw with a glow buffer: into the group's glow ranges when the table draws its glow (true); false when its class has no glow pull program (no glow is drawn for it)
    auto glow_add = [&](const LLAlphaPlanView& pv, const LLDrawInfo* draw, U32 rank) -> bool
    {
        const bool pbr_draw = draw->mGLTFMaterial.notNull();
        if (!(pbr_draw ? glow_pbr_table : glow_plain_table))
        {
            return false;
        }
        const int cls = pbr_draw ? (draw->mGLTFMaterial->mDoubleSided ? 2 : 1) : 0;
        std::vector<PendingRange>& r = glow_range[cls];
        std::vector<PendingSrc>& src = glow_src[cls];
        if (!r.empty() && r.back().mHandle == pv.mHandle && r.back().mFirst + r.back().mCount == rank)
        {
            ++r.back().mCount;
            ++src.back().mCount;
        }
        else
        {
            r.push_back({ pv.mHandle, rank, 1 });
            src.push_back({ pv.mItemInfo, pv.mItemWire, rank, 1, pv.mHandle });
        }
        ++sAlphaScene.mGlowServed;
        return true;
    };
    // a new run: the pull program of its key bound, the blend function, the per-draw values a plain draw has (spec 1, env 0, brightness 1)
    auto run_begin = [&](int key)
    {
        // LL_VK_ALPHAKEYCENSUS=1 (doc/vulkan/PLAN-crowd-worker-and-gpu.md item D): which keys the alpha runs have and how they follow each other. A run starts where the key of the next draw in the
        // blend order differs (program, blend function pair, double sided, rigged): a transition that keeps the program only changes the blend or the culling, one that changes the program needs
        // another pull program. Logged every 6000 runs.
        static const bool key_census = getenv("LL_VK_ALPHAKEYCENSUS") != nullptr;
        if (key_census)
        {
            static int prev = -1;
            static U64 runs = 0, same_program = 0, other_program = 0, same_key = 0;
            static std::map<int, U64> keys;
            static std::map<std::pair<int, int>, U64> trans;
            ++runs;
            ++keys[key];
            if (prev >= 0)
            {
                const bool same_prog = ((prev & 0xFF) == (key & 0xFF)) && (((prev >> 17) & 1) == ((key >> 17) & 1));
                ++(prev == key ? same_key : (same_prog ? same_program : other_program));
                ++trans[{ prev, key }];
            }
            prev = key;
            if (runs % 6000 == 0)
            {
                auto name = [](int k) { return std::string("p") + std::to_string(k & 0xFF) + "/b" + std::to_string((k >> 8) & 0xFF) + ((k >> 16) & 1 ? "/ds" : "") + ((k >> 17) & 1 ? "/rig" : ""); };
                std::vector<std::pair<U64, int>> topk;
                for (const auto& kv : keys) topk.push_back({ kv.second, kv.first });
                std::sort(topk.rbegin(), topk.rend());
                std::vector<std::pair<U64, std::pair<int, int>>> topt;
                for (const auto& kv : trans) topt.push_back({ kv.second, kv.first });
                std::sort(topt.rbegin(), topt.rend());
                std::string text = "alpha run keys over " + std::to_string(runs) + " runs: " + std::to_string(keys.size()) + " distinct keys; consecutive runs with the same key " + std::to_string(same_key)
                    + ", the same program and another blend or side " + std::to_string(same_program) + ", another program " + std::to_string(other_program) + "; top keys:";
                for (size_t k = 0; k < topk.size() && k < 8; ++k) text += " [" + name(topk[k].second) + " " + std::to_string(topk[k].first) + "]";
                text += "; top transitions:";
                for (size_t k = 0; k < topt.size() && k < 10; ++k) text += " [" + name(topt[k].second.first) + " > " + name(topt[k].second.second) + " " + std::to_string(topt[k].first) + "]";
                LL_INFOS("VkScene") << text << LL_ENDL;
            }
        }
        ItemCensus::Scope census_scope(ItemCensus::ALPHA_RUN_BEGIN);
        const int pkey = key & 0xFF;        // the program: 0 lit, 1 fullbright, 2 + mask a material, 18 PBR; the blend function pair id is in bits 8-15, bit 16: double sided, bit 17: rigged
        const bool skinned = ((key >> 17) & 1) != 0;
        LLShaderProgram* pull;
        if (pkey == 18)
        {
            pull = LLPipeline::sRenderingHUDs ? &gHUDPBRAlphaPullProgram : (skinned ? &gDeferredPBRAlphaPullSkinProgram : &gDeferredPBRAlphaPullProgram);
        }
        else if (pkey >= 2)
        {
            pull = skinned ? &gDeferredMaterialPullSkinProgram[pkey - 2] : &gDeferredMaterialPullProgram[pkey - 2];
        }
        else if (pkey == 1)
        {   // fullbright: the HUD's program in the HUD; the mask program (no atmosphere, no water clip) in the depth only passes and the impostor render; else the fullbright alpha program
            pull = LLPipeline::sRenderingHUDs ? &gHUDFullbrightAlphaPullProgram
                : ((depth_only || LLPipeline::sImpostorRender) ? (skinned ? &gDeferredFullbrightMaskPullSkinProgram : &gDeferredFullbrightMaskPullProgram)
                : (skinned ? &gDeferredFullbrightAlphaPullSkinProgram : &gDeferredFullbrightAlphaPullProgram));
        }
        else
        {
            pull = LLPipeline::sImpostorRender ? (skinned ? &gDeferredAlphaImpostorPullSkinProgram : &gDeferredAlphaImpostorPullProgram)
                : (mat_ctx ? (skinned ? &gDeferredAlphaUberMatPullSkinProgram : &gDeferredAlphaUberMatPullProgram)
                    : (uber_ctx ? (skinned ? &gDeferredAlphaUberPullSkinProgram : &gDeferredAlphaUberPullProgram) : (skinned ? &gDeferredAlphaPullSkinProgram : &gDeferredAlphaPullProgram)));
        }
        if (std::find(primed_programs.begin(), primed_programs.end(), pull) == primed_programs.end())
        {   // the first run of this program in this pass: its per pass uniforms (gamma, water planes, sun shadow settings, minimum alpha) for this view
            prepare_alpha_shader(pull, true, mWaterSign);
            primed_programs.push_back(pull);
            if (dof_pass && (pull == &gDeferredFullbrightMaskPullProgram || pull == &gDeferredFullbrightMaskPullSkinProgram))
            {
                pull->setMinimumAlpha(0.33f);       // (prepare_alpha_shader is bound to it)
            }
            LLShaderProgram::unbind();              // (it left the program bound: without this the bindDeferredShaderFast below, which binds the shadow, light and probe maps, would be skipped)
            setup_key = -1;
        }
        if (setup_key == key && current_shader == pull)
        {
            pending_key = key;
            return;
        }
        if (current_shader != pull)
        {
            gPipeline.bindDeferredShaderFast(*pull);
            if (pkey == 1)
            {
                S32 channel = pull->enableTexture(LLShaderMgr::EXPOSURE_MAP);
                if (channel > -1)
                {
                    gRender.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
                }
            }
        }
        int blend_src = (int)LLRender::BF_SOURCE_ALPHA, blend_dst = (int)LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
        ll_alpha_blend_pair(((U32)key >> 8) & 0xFF, &blend_src, &blend_dst);
        gRender.blendFunc((LLRender::eBlendFactor)blend_src, (LLRender::eBlendFactor)blend_dst, mAlphaSFactor, mAlphaDFactor);
        {   // a custom blend function may need "invisible" fragments: the minimum alpha is 0 for it, as the old loop sets it per draw (and back for the next run of this program)
            const bool zero_alpha = blend_dst != (int)LLRender::BF_SOURCE_ALPHA && blend_src != (int)LLRender::BF_SOURCE_ALPHA;
            if (zero_alpha)
            {
                current_shader->setMinimumAlpha(0.f);
                min_alpha_zero_program = current_shader;
            }
            else if (min_alpha_zero_program == current_shader)
            {
                current_shader->setMinimumAlpha(min_alpha_of(current_shader));
                min_alpha_zero_program = nullptr;
            }
            if (mDepthCutoff > 0.f && !zero_alpha)
            {   // the colour pass of the rigged alpha with a depth cutoff (hair halo, see forwardRender): the shader cuts at the cutoff instead of 1/255, and goes back to the usual one after the pass
                if (std::find(mDepthCutoffShaders.begin(), mDepthCutoffShaders.end(), current_shader) == mDepthCutoffShaders.end())
                {
                    mDepthCutoffShaders.push_back(current_shader);
                }
                current_shader->setMinimumAlpha(mDepthCutoff);
            }
        }
        gRender.syncLightState();       // the old draws upload the lights (point lights, sun up factor, ambient) into the bound program in syncMatrices(); a table run has no such draw: without this the program's lights stay 0 and the lit shader divides by them (NaN: black)
        const F32 vals[6] = { 1.f, 1.f, 1.f, 1.f, 0.f, 1.f };
        if (pkey < 2 && !(alpha_values_on && current_shader->mAlphaEpoch == sAlphaEpoch && memcmp(current_shader->mAlphaVals, vals, sizeof(vals)) == 0))
        {
            current_shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, 1.f, 1.f, 1.f, 1.f);
            current_shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, 0.f);
            current_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
            memcpy(current_shader->mAlphaVals, vals, sizeof(vals));
            current_shader->mAlphaEpoch = sAlphaEpoch;
        }
        pending_key = key;
        setup_key = key;
    };
    if (alpha_table)
    {
        ++sAlphaScene.mCalls;
    }
    LLCullResult::sg_iterator begin;
    LLCullResult::sg_iterator end;

    if (rigged)
    {
        begin = gPipeline.beginRiggedAlphaGroups();
        end = gPipeline.endRiggedAlphaGroups();
    }
    else
    {
        begin = gPipeline.beginAlphaGroups();
        end = gPipeline.endAlphaGroups();
    }

    LLEnvironment& env = LLEnvironment::instance();
    F32 water_height = env.getWaterHeight();

    bool above_water = getType() == LLDrawPool::POOL_ALPHA_POST_WATER;
    if (LLPipeline::sUnderWaterRender)
    {
        above_water = !above_water;
    }


    for (LLCullResult::sg_iterator i = begin; i != end; ++i)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("renderAlpha - group");
        LLSpatialGroup* group = *i;
        llassert(group);
        llassert(group->getSpatialPartition());

        if (group->getSpatialPartition()->mRenderByGroup &&
            !group->isDead())
        {

            LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : group->getExtents();

            // <FS> the fringe pass (the soft texels of the rigged alpha, drawn a second time over the static alpha) is left out for the avatars farther than RenderRiggedAlphaFringeMaxDistance:
            // the halo of a hair texture is a fraction of a pixel there (0.37 ms of GPU a frame in a crowd)
            static LLCachedControl<F32> fringe_max_distance(gSavedSettings, "RenderRiggedAlphaFringeMaxDistance", 20.f);
            if (mFringePass && rigged && fringe_max_distance > 0.f && LLVOAvatar::isLiveInstance(group->mAvatarp) && !group->mAvatarp->isSelf() && group->mAvatarp->mDrawable.notNull()
                && group->mAvatarp->mDrawable->mDistanceWRTCamera * LLPipeline::farDistanceScale() > fringe_max_distance)
            {
                continue;
            }

            if (!LLPipeline::sRenderingHUDs) // ignore above/below water for HUD render
            {
                if (above_water)
                { // reject any spatial groups that have no part above water
                    if (ext[1].getF32ptr()[2] < water_height)
                    {
                        continue;
                    }
                }
                else
                { // reject any spatial groups that he no part below water
                    if (ext[0].getF32ptr()[2] > water_height)
                    {
                        continue;
                    }
                }
            }

            for (int cls = 0; cls < 3; ++cls)
            {
                glow_range[cls].clear();
                glow_src[cls].clear();
                glow_draws[cls].clear();
            }

            bool is_particle_or_hud_particle = group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE
                                                      || group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE;

            // <FS:LO> Dont suspend partical processing while particles are hidden, just skip over drawing them
            if(!(gPipeline.sRenderParticles) && (
                                                 group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE ||
                                                 group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE))
            {
                continue;
            }
            // </FS:LO>

            bool disable_cull = is_particle_or_hud_particle;
            LLAlphaPlanView plan;
            bool group_plan;
            {
                ItemCensus::Scope census_scope(ItemCensus::ALPHA_PLAN);
                group_plan = alpha_table && !is_particle_or_hud_particle && ll_alpha_plan(group, plan, rigged) == 1 && plan.mHandle != 0;
                ItemCensus::add(ItemCensus::ALPHA_PLAN, group_plan ? 1 : 0);
            }
            U32 plan_ord = 0, plan_seg = 0;
            if (!group_plan)
            {
                run_flush();        // (the culling state of a group the table does not continue is its own)
                setup_key = -1;
            }
            LLRenderState::CullScope cull(false, disable_cull);

            LLSpatialGroup::drawmap_elem_t& draw_info = rigged ? group->mDrawMap[LLRenderPass::PASS_ALPHA_RIGGED] : group->mDrawMap[LLRenderPass::PASS_ALPHA];

            const bool plan_direct = group_plan && plan.mDirect;
            U32 glow_at = 0;
            for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
            {
                if (plan_direct)
                {
                    // S5a-2: a served segment is stepped over as a whole (its draw infos are the plan's, in the plan's order: the group's draw map has no draw of an avatar)
                    const U32 o = plan_ord;
                    while (plan_seg < plan.mSegCount && o >= plan.mSegs[plan_seg].mFirst + plan.mSegs[plan_seg].mCount)
                    {
                        ++plan_seg;
                    }
                    if (plan_seg < plan.mSegCount && plan.mSegs[plan_seg].mServed && o == plan.mSegs[plan_seg].mFirst)
                    {
                        const LLAlphaPlanSeg& seg = plan.mSegs[plan_seg];
                        if (pending_key != run_key(seg.mKey) || !tr.mEntries.empty())
                        {
                            run_flush();
                            run_begin(run_key(seg.mKey));
                        }
                        add_range(plan, seg);
                        sAlphaScene.mServed += seg.mCount;
                        if (glow_pass)
                        {
                            while (glow_at < plan.mGlowCount && plan.mGlowIdx[glow_at] < seg.mFirst)
                            {
                                ++glow_at;
                            }
                            while (glow_at < plan.mGlowCount && plan.mGlowIdx[glow_at] < seg.mFirst + seg.mCount)
                            {
                                LLDrawInfo* glow_draw = const_cast<LLDrawInfo*>(plan.mOrder[plan.mGlowIdx[glow_at]]);
                                glow_add(plan, glow_draw, seg.mRank + (plan.mGlowIdx[glow_at] - seg.mFirst));       // (drawn from the table after the group)
                                ++glow_at;
                            }
                        }
                        plan_ord = seg.mFirst + seg.mCount;
                        k += seg.mCount - 1;
                        continue;
                    }
                }
                LLDrawInfo& params = **k;
                if ((bool)params.mAvatar != rigged)
                {
                    continue;
                }

                LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("ra - push batch");
                if (group_plan)
                {
                    const U32 o = plan_ord++;
                    while (plan_seg < plan.mSegCount && o >= plan.mSegs[plan_seg].mFirst + plan.mSegs[plan_seg].mCount)
                    {
                        ++plan_seg;
                    }
                    if (plan_seg < plan.mSegCount && plan.mSegs[plan_seg].mServed && o < plan.mOrderCount && plan.mOrder[o] == &params)
                    {
                        // S5a-2: a served draw: its segment is one range of one run (the first draw of the segment adds it); a glow buffer is still drawn by the old glow pass after the group
                        const LLAlphaPlanSeg& seg = plan.mSegs[plan_seg];
                        if (o == seg.mFirst)
                        {
                            if (pending_key != run_key(seg.mKey) || !tr.mEntries.empty())
                            {
                                run_flush();
                                run_begin(run_key(seg.mKey));
                            }
                            add_range(plan, seg);
                        }
                        ++sAlphaScene.mServed;
                        if (glow_pass && params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
                        {
                            glow_add(plan, &params, seg.mRank + (o - seg.mFirst));       // (drawn from the table after the group)
                        }
                        continue;
                    }
                    if (!(plan_seg < plan.mSegCount && !plan.mSegs[plan_seg].mServed && o < plan.mOrderCount && plan.mOrder[o] == &params))
                    {
                        ++sAlphaScene.mMismatch;        // (the plan does not list this draw here: it is a transient entry)
                    }
                }
                if (transient_ok)
                {   // S5f F3: the table does not keep this draw (or this view has no fragments): a transient entry (the same pull program as a fragment item of its key), in the pass's order
                    LLAlphaTransient transient;
                    if (ll_alpha_transient(&params, rigged, depth_only, transient))
                    {
                        int key = run_key(transient.mKey);
                        if (is_particle_or_hud_particle)
                        {
                            key |= 1 << 18;         // (particle groups draw with culling off)
                        }
                        if (pending_key != key || !pending.empty())
                        {
                            run_flush();
                            run_begin(key);
                        }
                        tr_add(transient, params);
                        if (glow_pass && params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE) && (!params.mParticleRows || params.mHasGlow))        // (a particle draw without glow adds nothing to the alpha)
                        {
                            const bool pbr_draw = params.mGLTFMaterial.notNull();
                            if (pbr_draw ? glow_pbr_table : glow_plain_table)
                            {
                                glow_draws[pbr_draw ? (params.mGLTFMaterial->mDoubleSided ? 2 : 1) : 0].push_back(&params);
                            }
                        }
                        continue;
                    }
                }
                // S5g: no old path: a draw neither the fragments nor a transient entry can draw is not drawn
                ++sAlphaScene.mDropped;
            }

            // render emissive faces into alpha channel for bloom effects (the glow second draw of the group's draws, from the table: one run a class)
            if (glow_pass)
            {
                const bool glow_served = !glow_range[0].empty() || !glow_range[1].empty() || !glow_range[2].empty() || !glow_draws[0].empty() || !glow_draws[1].empty() || !glow_draws[2].empty();
                if (glow_served)
                {
                    run_flush();
                    setup_key = -1;
                    gPipeline.enableLightsDynamic();

                    // install glow-accumulating blend mode
                    // don't touch color, add to alpha (glow)
                    gRender.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE, LLRender::BF_ONE, LLRender::BF_ONE);

                    LLShaderProgram* lastShader = current_shader;
                    // glTF double sided: culling off; the rigged ones with depth writes off ("emissive" is additive, so sorting does not matter)
                    for (int cls = 0; cls < 3; ++cls)
                    {
                        LLShaderProgram* glow_program = cls == 0 ? (rigged ? &gDeferredEmissivePullSkinProgram : &gDeferredEmissivePullProgram) : (rigged ? &gPBRGlowPullSkinProgram : &gPBRGlowPullProgram);
                        if (!glow_draws[cls].empty())
                        {   // the transient draws' glow: entries made again, one record
                            glow_program->bind();
                            pending_key = (cls == 2 ? (1 << 16) : 0) | (is_particle_or_hud_particle ? (1 << 18) : 0);       // (a particle group draws with culling off, its glow too)
                            for (LLDrawInfo* glow_draw : glow_draws[cls])
                            {
                                LLAlphaTransient glow_transient;
                                if (ll_alpha_transient(glow_draw, rigged, false, glow_transient))
                                {
                                    tr_add(glow_transient, *glow_draw);
                                }
                            }
                            if (rigged)
                            {   // (the rigged glow draws with depth writes off, as the old rigged glow passes; the static glow draws with the pass's own depth state, which writes none: a glow quad that wrote depth would hide whatever is drawn behind it after, wherever its fragments are invisible)
                                LLRenderState::DepthScope depth(true, false);
                                tr_flush();
                            }
                            else
                            {
                                tr_flush();
                            }
                            glow_draws[cls].clear();
                        }
                        if (!glow_range[cls].empty())
                        {   // the fragment items' glow: ranges
                            glow_program->bind();
                            pending.swap(glow_range[cls]);
                            pending_src.swap(glow_src[cls]);
                            pending_key = (cls == 2 ? (1 << 16) : 0) | (is_particle_or_hud_particle ? (1 << 18) : 0);
                            if (rigged)
                            {
                                LLRenderState::DepthScope depth(true, false);
                                run_flush();
                            }
                            else
                            {
                                run_flush();
                            }
                            glow_range[cls].clear();
                            glow_src[cls].clear();
                        }
                    }

                    // restore our alpha blend mode
                    gRender.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);

                    if (lastShader)
                    {
                        lastShader->bind();
                    }
                    ++sAlphaEpoch;      // <FS> U1: the glow passes wrote their own values
                }
            }
        }
    }

    run_flush();
    if (alpha_table && gFrameCount >= sAlphaScene.mLast + 600)
    {
        const double frames = (double)std::max<U32>(1, gFrameCount - sAlphaScene.mLast);
        sAlphaScene.mLast = gFrameCount;
        LL_INFOS("VkScene") << "alpha from the scene table, viewer side per frame (main view's passes): " << (double)sAlphaScene.mCalls / frames << " passes, " << (double)sAlphaScene.mRecords / frames << " records, "
            << (double)sAlphaScene.mRanges / frames << " ranges, " << (double)sAlphaScene.mServed / frames << " draws served from fragments, " << (double)sAlphaScene.mBytes / frames << " bytes sent, glow draws served "
            << (double)sAlphaScene.mGlowServed / frames << ", transient " << (double)sAlphaScene.mTrEntries / frames << " draws in " << (double)sAlphaScene.mTrRecords / frames << " records, plan mismatches "
            << sAlphaScene.mMismatch << ", draws not drawn (neither fragment nor transient) " << (double)sAlphaScene.mDropped / frames << LL_ENDL;
        sAlphaScene.mCalls = sAlphaScene.mRecords = sAlphaScene.mRanges = sAlphaScene.mServed = sAlphaScene.mDropped = sAlphaScene.mBytes = sAlphaScene.mGlowServed = sAlphaScene.mTrRecords = sAlphaScene.mTrEntries = 0;
    }

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    LLVertexBuffer::unbind();
}
