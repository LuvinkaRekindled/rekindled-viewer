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
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <intrin.h>

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
#include "pipeline.h"
#include "llvkbridge.h"          // <FS> rtShadowMode()
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
        LLRenderPass(type), target_shader(NULL),
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
        static LLCachedControl<S32> rt_samples(gSavedSettings, "RenderRTShadowSamples", 2);
        static LLStaticHashedString radius_s("rt_sun_radius"), samples_s("rt_sun_samples"), frame_s("rt_noise_frame");
        F32 scale = 2.f;                    // chosen by eye (the user, plaza: a logo on the floor next to the ground): twice the sun disc, 6 rays per sample setting
        S32 samples = rt_samples() * 6;
        if (alpha_sun == 3) { scale = 4.f; samples = rt_samples() * 6; }
        else if (alpha_sun >= 4) { scale = 8.f; samples = rt_samples() * 6; }
        shader->uniform1f(radius_s, alpha_sun == 1 ? 0.f : tanf(llclamp(rt_softness() * scale, 0.f, 20.f) * DEG_TO_RAD));
        shader->uniform1i(samples_s, alpha_sun == 1 ? 1 : llclamp(samples, 1, 16));
        shader->uniform1f(frame_s, LLPipeline::sDLSS ? (F32)(gPipeline.mJitterIndex % 64) : 0.f);
        static LLCachedControl<F32> rt_lod_near(gSavedSettings, "RenderRTShadowAlphaLodNear", 4.f);
        static LLCachedControl<F32> rt_lod_far(gSavedSettings, "RenderRTShadowAlphaLodFar", 20.f);
        static LLStaticHashedString lod_s("rt_sun_lod");
        shader->uniform2f(lod_s, alpha_sun == 1 ? 0.f : rt_lod_near(), rt_lod_far());
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

    emissive_shader = &gDeferredEmissiveProgram;
    prepare_alpha_shader(emissive_shader, false, water_sign);

    pbr_emissive_shader = &gPBRGlowProgram;
    prepare_alpha_shader(pbr_emissive_shader, false, water_sign);


    fullbright_shader   =
        (LLPipeline::sImpostorRender) ? &gDeferredFullbrightAlphaMaskProgram :
        (LLPipeline::sRenderingHUDs) ? &gHUDFullbrightAlphaMaskAlphaProgram :
        &gDeferredFullbrightAlphaMaskAlphaProgram;
    prepare_alpha_shader(fullbright_shader, true, water_sign);

    simple_shader   =
        (LLPipeline::sImpostorRender) ? &gDeferredAlphaImpostorProgram :
        (LLPipeline::sRenderingHUDs) ? &gHUDAlphaProgram :
        &gDeferredAlphaProgram;

    prepare_alpha_shader(simple_shader, true, water_sign); //prime simple shader (loads shadow relevant uniforms)

    LLShaderProgram* materialShader = gDeferredMaterialProgram;
    for (int i = 0; i < LLMaterial::SHADER_COUNT*2; ++i)
    {
        prepare_alpha_shader(&materialShader[i], true, water_sign);
    }

    pbr_shader =
        (LLPipeline::sRenderingHUDs) ? &gHUDPBRAlphaProgram :
        &gDeferredPBRAlphaProgram;

    prepare_alpha_shader(pbr_shader, true, water_sign);

    if (gAlphaPullReady && ll_alpha_serving())
    {   // <FS> S5a: the pull programs of the runs drawn from the scene table
        prepare_alpha_shader(&gDeferredAlphaPullProgram, true, water_sign);
        prepare_alpha_shader(&gDeferredFullbrightAlphaPullProgram, true, water_sign);
        if (gAlphaPbrPullReady)
        {   // S5c: and the PBR program
            prepare_alpha_shader(&gDeferredPBRAlphaPullProgram, true, water_sign);
        }
        if (gAlphaMaterialPullReady)
        {   // S5b: and the blend variants of the materials programs
            for (U32 mask : { 1u, 5u, 9u, 13u })
            {
                prepare_alpha_shader(&gDeferredMaterialPullProgram[mask], true, water_sign);
            }
        }
    }

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
    if (!LLPipeline::sImpostorRender && LLPipeline::RenderDepthOfField && !gCubeSnapshot && !LLPipeline::sRenderingHUDs && getType() == LLDrawPool::POOL_ALPHA_POST_WATER)
    {
        //update depth buffer sampler
        simple_shader = fullbright_shader = &gDeferredFullbrightAlphaMaskProgram;

        simple_shader->bind();
        simple_shader->setMinimumAlpha(0.33f);

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
        LLShaderProgram* saved_simple = simple_shader;
        LLShaderProgram* saved_fullbright = fullbright_shader;
        LLShaderProgram* mask_shader = &gDeferredFullbrightAlphaMaskProgram;
        simple_shader = fullbright_shader = mask_shader;

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

        simple_shader = saved_simple;
        fullbright_shader = saved_fullbright;
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

inline bool IsFullbright(LLDrawInfo& params)
{
    return params.mFullbright;
}

inline bool IsMaterial(LLDrawInfo& params)
{
    return params.mMaterial != nullptr;
}

inline bool IsEmissive(LLDrawInfo& params)
{
    return params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
}

inline void Draw(LLDrawInfo* draw, U32 mask)
{
    draw->mVertexBuffer->setBuffer();
    LLRenderPass::applyModelMatrix(*draw);
    draw->mVertexBuffer->drawRange(LLRender::TRIANGLES, draw->mStart, draw->mEnd, draw->mCount, draw->mOffset);
}

bool LLDrawPoolAlpha::TexSetup(LLDrawInfo* draw, bool use_material)
{
    bool tex_setup = false;

    if (draw->mGLTFMaterial)
    {
        if (draw->mTextureMatrix)
        {
            tex_setup = true;
            LLRenderMatrix::load(LLRenderMatrix::Kind::Texture0, (F32*)draw->mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;
        }
    }
    else
    {
        if (!LLPipeline::sRenderingHUDs && use_material && current_shader)
        {
            if (draw->mNormalMap)
            {
                current_shader->bindTexture(LLShaderMgr::BUMP_MAP, draw->mNormalMap);
            }

            if (draw->mSpecularMap)
            {
                current_shader->bindTexture(LLShaderMgr::SPECULAR_MAP, draw->mSpecularMap);
            }
        }
        else if (current_shader == simple_shader || current_shader == simple_shader->mRiggedVariant)
        {
            current_shader->bindTexture(LLShaderMgr::BUMP_MAP, LLViewerFetchedTexture::sFlatNormalImagep);
            current_shader->bindTexture(LLShaderMgr::SPECULAR_MAP, LLViewerFetchedTexture::sWhiteImagep);
        }
        if (draw->mTextureList.size() > 1)
        {
            for (U32 i = 0; i < draw->mTextureList.size(); ++i)
            {
                if (draw->mTextureList[i].notNull())
                {
                    gRender.getTexUnit(i)->bindFast(draw->mTextureList[i]);
                }
            }
        }
        else
        { //not batching textures or batch has only 1 texture -- might need a texture matrix
            if (draw->mTexture.notNull())
            {
                if (use_material)
                {
                    current_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, draw->mTexture);
                }
                else
                {
                    gRender.getTexUnit(0)->bindFast(draw->mTexture);
                }

                if (draw->mTextureMatrix)
                {
                    tex_setup = true;
                    LLRenderMatrix::load(LLRenderMatrix::Kind::Texture0, (F32*)draw->mTextureMatrix->mMatrix);
                    gPipeline.mTextureMatrixOps++;
                }
            }
            else
            {
                gRender.getTexUnit(0)->bindWhiteFast();
            }
        }
    }

    return tex_setup;
}

void LLDrawPoolAlpha::RestoreTexSetup(bool tex_setup)
{
    if (tex_setup)
    {
        LLRenderMatrix::identity(LLRenderMatrix::Kind::Texture0);
    }
}

void LLDrawPoolAlpha::drawEmissive(LLDrawInfo* draw)
{
    LLShaderProgram::sCurBoundShaderPtr->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);
    draw->mVertexBuffer->setBuffer();
    draw->mVertexBuffer->drawRange(LLRender::TRIANGLES, draw->mStart, draw->mEnd, draw->mCount, draw->mOffset);
}


void LLDrawPoolAlpha::renderEmissives(std::vector<LLDrawInfo*>& emissives)
{
    emissive_shader->bind();
    emissive_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);

    for (LLDrawInfo* draw : emissives)
    {
        bool tex_setup = TexSetup(draw, false);
        drawEmissive(draw);
        RestoreTexSetup(tex_setup);
    }
}

void LLDrawPoolAlpha::renderPbrEmissives(std::vector<LLDrawInfo*>& emissives)
{
    pbr_emissive_shader->bind();

    for (LLDrawInfo* draw : emissives)
    {
        llassert(draw->mGLTFMaterial);
        LLRenderState::CullScope cull_face(false, draw->mGLTFMaterial->mDoubleSided);
        draw->mGLTFMaterial->bind(draw->mTexture);
        draw->mVertexBuffer->setBuffer();
        draw->mVertexBuffer->drawRange(LLRender::TRIANGLES, draw->mStart, draw->mEnd, draw->mCount, draw->mOffset);
    }
}

void LLDrawPoolAlpha::renderRiggedEmissives(std::vector<LLDrawInfo*>& emissives)
{
    LLRenderState::DepthScope depth(true, false); //disable depth writes since "emissive" is additive so sorting doesn't matter
    LLShaderProgram* shader = emissive_shader->mRiggedVariant;
    shader->bind();
    shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, 1.f);

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLDrawInfo* draw : emissives)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("Emissives");

        if (uploadMatrixPalette(draw->mAvatar, draw->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        {
            bool tex_setup = TexSetup(draw, false);
            drawEmissive(draw);
            RestoreTexSetup(tex_setup);
        }
    }
}

void LLDrawPoolAlpha::renderRiggedPbrEmissives(std::vector<LLDrawInfo*>& emissives)
{
    LLRenderState::DepthScope depth(true, false); //disable depth writes since "emissive" is additive so sorting doesn't matter
    pbr_emissive_shader->bind(true);

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin = false;

    for (LLDrawInfo* draw : emissives)
    {
        if (!uploadMatrixPalette(draw->mAvatar, draw->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
        { // failed to upload matrix palette, skip rendering
            continue;
        }

        LLRenderState::CullScope cull_face(false, draw->mGLTFMaterial->mDoubleSided);
        draw->mGLTFMaterial->bind(draw->mTexture);
        draw->mVertexBuffer->setBuffer();
        draw->mVertexBuffer->drawRange(LLRender::TRIANGLES, draw->mStart, draw->mEnd, draw->mCount, draw->mOffset);
    }
}

// <FS> S5 A0 (doc/vulkan/S5-design.md): census of the alpha pools, log only (LL_VK_ALPHACENSUS=1), per frame over 600 frames. What it answers: how many draws a table could serve (reasons for
// the others), how long the runs of equal state are in the real order, and where the main-thread time of the loop goes (cycle counter stamps; the stamps cost something, see the stamps line).
extern bool ll_group_fragment_state(const LLSpatialGroup* group, bool* resident);      // pipeline.cpp
extern U32 gFrameCount;
namespace AlphaCensus
{
    static const bool sOn = []() { const char* v = getenv("LL_VK_ALPHACENSUS"); return v && atoi(v) != 0; }();
    enum Reason { R_ELIGIBLE, R_HUDIMP, R_PARTICLE, R_RIGGED, R_NO_FRAGMENT, R_GLTF, R_MATERIAL, R_BLEND, R_TEXMATRIX, R_LAYOUT, R_MANYTEX, R_COUNT };
    static const char* sReasonName[R_COUNT] = { "plain (S5a)", "hud/impostor", "particle group", "rigged (S5d)", "group without current fragment", "glTF (S5c)", "material (S5b)", "custom blend",
                                                "texture matrix", "buffer layout", "more than 4 textures" };
    enum Sect { S_CHOOSE, S_UNIFORMS, S_PALETTE, S_TEXTURES, S_DRAW, S_REST, S_COUNT };
    static const char* sSectName[S_COUNT] = { "program choice + bind", "per-draw uniforms", "palette upload", "texture setup", "blend + draw call", "rest of the draw" };
    struct Key
    {
        const void* mShader = nullptr; int mSrc = 0, mDst = 0; bool mDouble = false, mNoCull = false;
        bool operator==(const Key& o) const { return mShader == o.mShader && mSrc == o.mSrc && mDst == o.mDst && mDouble == o.mDouble && mNoCull == o.mNoCull; }
    };
    struct Ctx
    {
        U64 mCalls = 0, mGroups = 0, mGrpWater = 0, mGrpHidden = 0, mGrpParticle = 0, mGrpBridge = 0, mGrpFragment = 0, mGrpResident = 0, mGrpGlow = 0;
        U64 mDraws = 0, mTris = 0, mReason[R_COUNT] = {}, mReasonTris[R_COUNT] = {}, mDropped = 0;
        U64 mFlagGlow = 0, mFlagFullbright = 0, mFlagBatch = 0, mFlagTexMat = 0, mFlagBlend = 0, mFlagMaterial = 0, mFlagGLTF = 0, mFlagModelMatrix = 0, mFlagNoTexture = 0;
        U64 mShaderBinds = 0, mBlendChanges = 0, mCullChanges = 0;
        U64 mRuns = 0, mRunHist[7] = {}, mEligRuns = 0, mEligHist[7] = {}, mEligEndedByOther = 0, mEligEndedByGlow = 0, mEligDraws = 0;
        U64 mGlowDraws[4] = {};
        U64 mDistinctTex = 0, mTexHist[4] = {};
        U64 mSect[S_COUNT] = {}, mGroupTicks = 0, mCallTicks = 0, mGlowTicks = 0;
        U64 mPlanGroups = 0, mPlanStale = 0, mPlanNone = 0, mPlanServed = 0, mPlanOld = 0, mPlanSegs = 0, mPlanRanges = 0, mPlanRecords = 0, mPlanMismatch = 0, mPlanGlowGroups = 0;
    };
    static Ctx sCtx[2][2][2][3];            // view (0 main, 1 other: cube / reflection / impostor / HUD), pool (0 pre water, 1 post), rigged, mode (0 colour, 1 depth only, 2 fringe)
    static U32 sFrom = 0;
    static U64 sTscFrom = 0;
    static std::chrono::steady_clock::time_point sClockFrom;
    static U64 sStamps = 0;
    static U32 hist_bucket(U32 n) { return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : n <= 32 ? 5 : 6; }

    struct Call
    {
        bool mOn;
        Ctx* mCtx = nullptr;
        U64 mT0 = 0, mLast = 0, mGroupT0 = 0, mEmT0 = 0;
        bool mUsePlan = false, mHudImp = false, mGroupFragment = false, mGroupParticle = false, mDrawn = false, mCurEligible = false, mHaveKey = false, mPost = false, mPlanActive = false, mPlanGlow = false;
        LLAlphaPlanView mPlan;
        U32 mPlanIdx = 0;
        int mOpenKey = -1;
        Key mLastKey, mRunKey, mEligKey;
        U32 mRunLen = 0, mEligLen = 0;
        std::unordered_set<const void*> mTex;
        Call(bool post, bool rigged, bool depth_only, bool fringe) : mOn(sOn), mPost(post)
        {
            if (!mOn) return;
            mHudImp = LLPipeline::sImpostorRender || LLPipeline::sRenderingHUDs;
            const int view = (gCubeSnapshot || LLPipeline::sReflectionRender || mHudImp) ? 1 : 0;
            mCtx = &sCtx[view][post ? 1 : 0][rigged ? 1 : 0][depth_only ? 1 : fringe ? 2 : 0];
            mUsePlan = !rigged && view == 0;
            ++mCtx->mCalls;
            mT0 = __rdtsc();
            if (!sFrom) { sFrom = gFrameCount; sTscFrom = mT0; sClockFrom = std::chrono::steady_clock::now(); }
        }
        void endRun() { if (mRunLen) { ++mCtx->mRuns; ++mCtx->mRunHist[hist_bucket(mRunLen)]; mRunLen = 0; } }
        void endElig(bool by_glow)
        {
            if (mEligLen)
            {
                ++mCtx->mEligRuns; ++mCtx->mEligHist[hist_bucket(mEligLen)]; mEligLen = 0;
                if (by_glow) ++mCtx->mEligEndedByGlow; else ++mCtx->mEligEndedByOther;
            }
        }
        // sections: the time since the last stamp goes to the section named
        void stamp(Sect s) { if (!mOn || !mDrawn) return; const U64 t = __rdtsc(); mCtx->mSect[s] += t - mLast; mLast = t; ++sStamps; }
        void groupBegin() { if (mOn) { mGroupT0 = __rdtsc(); mGroupFragment = mGroupParticle = false; } }
        void groupEnd()
        {
            if (!mOn) return;
            mCtx->mGroupTicks += __rdtsc() - mGroupT0;
            if (mPlanActive)
            {
                if (mPlanIdx != mPlan.mOrderCount) ++mCtx->mPlanMismatch;       // the loop drew fewer draw infos than the plan lists
                if (mPlanGlow && mPost) mOpenKey = -1;                          // the glow pass after the group changes program and blend
                mPlanActive = false;
            }
        }
        void groupSkip(int why)
        {
            if (!mOn) return;
            ++mCtx->mGroups;
            if (why == 0) ++mCtx->mGrpWater; else ++mCtx->mGrpHidden;
        }
        void groupAccepted(const LLSpatialGroup* group, bool particle, bool bridge)
        {
            if (!mOn) return;
            ++mCtx->mGroups;
            bool resident = false;
            mGroupFragment = ll_group_fragment_state(group, &resident);
            mGroupParticle = particle;
            if (particle) ++mCtx->mGrpParticle;
            if (bridge) ++mCtx->mGrpBridge;
            if (mGroupFragment) ++mCtx->mGrpFragment;
            if (resident) ++mCtx->mGrpResident;
            // S5a-1: the group's plan (only with LL_VK_SCENE_ALPHA=1): segments, the records a loop that sent runs would make (adjacent served segments with one key merge until an old draw or a glow pass), the order check
            mPlanActive = false;
            mPlan = LLAlphaPlanView();
            const int st = mUsePlan ? ll_alpha_plan(group, mPlan) : 0;
            if (st == 1)
            {
                mPlanActive = true;
                mPlanIdx = 0;
                mPlanGlow = mPlan.mGlow;
                ++mCtx->mPlanGroups;
                if (mPlan.mGlow) ++mCtx->mPlanGlowGroups;
                mCtx->mPlanSegs += mPlan.mSegCount;
                for (U32 k = 0; k < mPlan.mSegCount; ++k)
                {
                    const LLAlphaPlanSeg& seg = mPlan.mSegs[k];
                    if (seg.mServed)
                    {
                        mCtx->mPlanServed += seg.mCount;
                        ++mCtx->mPlanRanges;
                        if (mOpenKey != (int)seg.mKey) { ++mCtx->mPlanRecords; mOpenKey = (int)seg.mKey; }
                    }
                    else
                    {
                        mCtx->mPlanOld += seg.mCount;
                        mOpenKey = -1;
                    }
                }
            }
            else
            {
                if (st == 2) ++mCtx->mPlanStale; else if (mUsePlan) ++mCtx->mPlanNone;
                mOpenKey = -1;
            }
        }
        void drawBegin(const LLDrawInfo& p, bool rigged)
        {
            if (!mOn) return;
            mDrawn = true;
            Ctx& c = *mCtx;
            if (mPlanActive)
            {
                if (mPlanIdx < mPlan.mOrderCount && mPlan.mOrder[mPlanIdx] == &p) ++mPlanIdx; else ++c.mPlanMismatch;       // S5a-1: the plan lists the draw infos in the loop's order
            }
            ++c.mDraws;
            c.mTris += p.mCount / 3;
            const LLVertexBuffer* vb = p.mVertexBuffer.get();
            const bool material = !LLPipeline::sRenderingHUDs && p.mMaterial.notNull();
            const bool custom_blend = p.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA || p.mBlendFuncDst != LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
            const bool tex_matrix = p.mTextureMatrix != nullptr;
            const bool layout_bad = !vb || !vb->getVertexBufferId() || !vb->getIndexBufferId() || vb->getIndicesType() != ComponentType::U16 || !vb->hasDataType(LLVertexBuffer::TYPE_VERTEX) || vb->getOffset(LLVertexBuffer::TYPE_VERTEX) != 0;
            const bool glow = vb && vb->hasDataType(LLVertexBuffer::TYPE_EMISSIVE);
            Reason r = R_ELIGIBLE;
            if (mHudImp) r = R_HUDIMP;
            else if (mGroupParticle) r = R_PARTICLE;
            else if (rigged || p.mAvatar) r = R_RIGGED;
            else if (!mGroupFragment) r = R_NO_FRAGMENT;
            else if (p.mGLTFMaterial) r = R_GLTF;
            else if (material) r = R_MATERIAL;
            else if (custom_blend) r = R_BLEND;
            else if (tex_matrix) r = R_TEXMATRIX;
            else if (layout_bad) r = R_LAYOUT;
            else if (p.mTextureList.size() > 4) r = R_MANYTEX;
            ++c.mReason[r];
            c.mReasonTris[r] += p.mCount / 3;
            mCurEligible = r == R_ELIGIBLE;
            if (glow) ++c.mFlagGlow;
            if (p.mFullbright) ++c.mFlagFullbright;
            if (p.mTextureList.size() > 1) ++c.mFlagBatch;
            if (tex_matrix) ++c.mFlagTexMat;
            if (custom_blend) ++c.mFlagBlend;
            if (material) ++c.mFlagMaterial;
            if (p.mGLTFMaterial) ++c.mFlagGLTF;
            if (p.mModelMatrix) ++c.mFlagModelMatrix;
            const size_t ntex = p.mTextureList.size() > 1 ? p.mTextureList.size() : 1;
            if (p.mTexture.isNull() && p.mTextureList.size() <= 1) ++c.mFlagNoTexture;
            ++c.mTexHist[ntex <= 1 ? 0 : ntex == 2 ? 1 : ntex <= 4 ? 2 : 3];
            mTex.insert(p.mTexture.get());
            for (const auto& t : p.mTextureList) mTex.insert(t.get());
            mLast = __rdtsc();
        }
        // the state the draw is made with: runs of equal (program, blend function, culling) in the real order, and the runs of plain draws
        void state(const void* shader, const LLDrawInfo& p, bool double_sided, bool no_cull)
        {
            if (!mOn) return;
            Key k; k.mShader = shader; k.mSrc = (int)p.mBlendFuncSrc; k.mDst = (int)p.mBlendFuncDst; k.mDouble = double_sided; k.mNoCull = no_cull;
            if (!mHaveKey)
            {
                ++mCtx->mShaderBinds; ++mCtx->mBlendChanges; ++mCtx->mCullChanges;
            }
            else
            {
                if (k.mShader != mLastKey.mShader) ++mCtx->mShaderBinds;
                if (k.mSrc != mLastKey.mSrc || k.mDst != mLastKey.mDst) ++mCtx->mBlendChanges;
                if (k.mDouble != mLastKey.mDouble || k.mNoCull != mLastKey.mNoCull) ++mCtx->mCullChanges;
            }
            if (mRunLen && !(k == mRunKey)) endRun();
            mRunKey = k; ++mRunLen;
            if (mCurEligible)
            {
                ++mCtx->mEligDraws;
                if (mEligLen && !(k == mEligKey)) endElig(false);
                mEligKey = k; ++mEligLen;
            }
            else
            {
                endElig(false);
            }
            mLastKey = k; mHaveKey = true;
        }
        void dropped() { if (mOn) ++mCtx->mDropped; mDrawn = false; }
        void drawEnd() { stamp(S_REST); mDrawn = false; }
        void glowBegin(size_t a, size_t b, size_t c, size_t d)
        {
            if (!mOn) return;
            const size_t n[4] = { a, b, c, d };
            bool any = false;
            for (int i = 0; i < 4; ++i) { if (n[i]) { mCtx->mGlowDraws[i] += n[i]; any = true; } }
            if (any)
            {
                ++mCtx->mGrpGlow;
                endRun();
                endElig(true);
                mHaveKey = false;       // the glow draws change program and blend; the next draw starts from a bound state again
            }
            mEmT0 = any ? __rdtsc() : 0;
        }
        void glowEnd() { if (mOn && mEmT0) { mCtx->mGlowTicks += __rdtsc() - mEmT0; mEmT0 = 0; } }
        ~Call();
    };

    static void report();
    Call::~Call()
    {
        if (!mOn) return;
        endRun();
        endElig(false);
        mCtx->mDistinctTex += mTex.size();
        mCtx->mCallTicks += __rdtsc() - mT0;
        if (gFrameCount >= sFrom + 600) report();
    }

    static void report()
    {
        const U64 tsc = __rdtsc();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sClockFrom).count();
        const double frames = (double)(gFrameCount - sFrom);
        const double tick_ms = (double)(tsc - sTscFrom) / ms;       // cycle counter ticks per millisecond
        auto per = [&](U64 v) { return (double)v / frames; };
        auto tm = [&](U64 v) { return (double)v / tick_ms / frames; };
        static const char* sMode[3] = { "colour", "depth only", "fringe" };
        for (int view = 0; view < 2; ++view) for (int post = 0; post < 2; ++post) for (int rig = 0; rig < 2; ++rig) for (int mode = 0; mode < 3; ++mode)
        {
            const Ctx& c = sCtx[view][post][rig][mode];
            if (!c.mCalls) continue;
            std::string reasons;
            for (int r = 0; r < R_COUNT; ++r)
            {
                if (c.mReason[r]) reasons += llformat(" | %s %.1f (%.0f tris)", sReasonName[r], per(c.mReason[r]), per(c.mReasonTris[r]));
            }
            std::string runs, eligs, texes, sects;
            for (int h = 0; h < 7; ++h) { runs += llformat(" %.1f", per(c.mRunHist[h])); eligs += llformat(" %.1f", per(c.mEligHist[h])); }
            for (int h = 0; h < 4; ++h) texes += llformat(" %.1f", per(c.mTexHist[h]));
            U64 sum = 0;
            for (int s = 0; s < S_COUNT; ++s) { sects += llformat(" | %s %.3f", sSectName[s], tm(c.mSect[s])); sum += c.mSect[s]; }
            LL_INFOS("AlphaCensus") << llformat("%s %s %s %s over %d frames: calls %.2f; groups %.1f (water rejected %.1f, hidden particles %.1f, particle %.1f, bridge %.1f, with a current fragment %.1f, resident %.1f, with glow draws %.1f); draws %.1f, triangles %.0f; dropped %.2f",
                                                 view ? "other views" : "main view", post ? "post water" : "pre water", rig ? "rigged" : "static", sMode[mode], (int)frames, per(c.mCalls), per(c.mGroups), per(c.mGrpWater), per(c.mGrpHidden),
                                                 per(c.mGrpParticle), per(c.mGrpBridge), per(c.mGrpFragment), per(c.mGrpResident), per(c.mGrpGlow), per(c.mDraws), per(c.mTris), per(c.mDropped))
                << "; draws a frame by class:" << reasons << LL_ENDL;
            LL_INFOS("AlphaCensus") << llformat("  flags (not exclusive): glow buffer %.1f, fullbright %.1f, texture batch %.1f, texture matrix %.1f, custom blend %.1f, material %.1f, glTF %.1f, own model matrix %.1f, no texture %.1f; texture count 1 / 2 / 3-4 / 5+:%s; distinct textures a call %.1f",
                                                 per(c.mFlagGlow), per(c.mFlagFullbright), per(c.mFlagBatch), per(c.mFlagTexMat), per(c.mFlagBlend), per(c.mFlagMaterial), per(c.mFlagGLTF), per(c.mFlagModelMatrix), per(c.mFlagNoTexture),
                                                 texes.c_str(), (double)c.mDistinctTex / (double)c.mCalls) << LL_ENDL;
            LL_INFOS("AlphaCensus") << llformat("  state changes a frame: program %.1f, blend function %.1f, culling %.1f; runs of equal (program, blend, cull) in the real order %.1f (length 1 / 2 / 3-4 / 5-8 / 9-16 / 17-32 / 33+:%s); runs of plain draws %.1f for %.1f plain draws (ended by another draw %.1f, by a glow pass %.1f; lengths:%s); glow draws a frame: plain %.1f, PBR %.1f, rigged %.1f, PBR rigged %.1f",
                                                 per(c.mShaderBinds), per(c.mBlendChanges), per(c.mCullChanges), per(c.mRuns), runs.c_str(), per(c.mEligRuns), per(c.mEligDraws), per(c.mEligEndedByOther), per(c.mEligEndedByGlow), eligs.c_str(),
                                                 per(c.mGlowDraws[0]), per(c.mGlowDraws[1]), per(c.mGlowDraws[2]), per(c.mGlowDraws[3])) << LL_ENDL;
            const double group_loop = tm(c.mGroupTicks), draws_ms = tm(sum), glow_ms = tm(c.mGlowTicks);
            if (c.mPlanGroups || c.mPlanStale)
            {
                LL_INFOS("AlphaCensus") << llformat("  plan (S5a-1): groups with a plan %.1f (stale fragment %.1f, no plan %.1f, with glow %.1f), draws in served segments %.1f, in old segments %.1f, segments %.1f, served ranges %.1f, records after merging %.1f; order mismatches %llu",
                                                     per(c.mPlanGroups), per(c.mPlanStale), per(c.mPlanNone), per(c.mPlanGlowGroups), per(c.mPlanServed), per(c.mPlanOld), per(c.mPlanSegs), per(c.mPlanRanges), per(c.mPlanRecords), (unsigned long long)c.mPlanMismatch) << LL_ENDL;
            }
            LL_INFOS("AlphaCensus") << llformat("  main thread ms a frame: calls %.3f; group loop %.3f of which draws %.3f (sections%s), glow passes %.3f, group loop overhead (water test, cull scope, emissive vectors, stamps) %.3f",
                                                 tm(c.mCallTicks), group_loop, draws_ms, sects.c_str(), glow_ms, group_loop - draws_ms - glow_ms) << LL_ENDL;
        }
        LL_INFOS("AlphaCensus") << llformat("stamps %.0f a frame (a few ns each: that is part of the draw section numbers above)", (double)sStamps / frames) << LL_ENDL;
        for (int view = 0; view < 2; ++view) for (int post = 0; post < 2; ++post) for (int rig = 0; rig < 2; ++rig) for (int mode = 0; mode < 3; ++mode)
        {
            sCtx[view][post][rig][mode] = Ctx();
        }
        sStamps = 0;
        sFrom = gFrameCount;
        sTscFrom = tsc;
        sClockFrom = std::chrono::steady_clock::now();
    }

    struct GroupScope
    {
        Call& mCall;
        explicit GroupScope(Call& c) : mCall(c) { mCall.groupBegin(); }
        ~GroupScope() { mCall.groupEnd(); }
    };
}

// <FS> S5a: what the alpha loop sent to the scene table (log tag VkScene, every 600 frames)
static struct AlphaSceneStats
{
    U64 mCalls = 0, mRecords = 0, mRanges = 0, mServed = 0, mOld = 0, mMismatch = 0, mBytes = 0;
    U32 mLast = 0;
} sAlphaScene;

void LLDrawPoolAlpha::renderAlpha(U32 mask, bool depth_only, bool rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    ++sAlphaEpoch;      // <FS> U1: what the loop below last wrote to a program is not known any more
    const bool alpha_values_on = ll_pass_uni_on();
    const bool alpha_values_verify = alpha_values_on && ll_pass_uni_verify();
    AlphaCensus::Call cen(getType() == LLDrawPool::POOL_ALPHA_POST_WATER, rigged, depth_only, mFringePass);       // <FS> S5 A0
    // <FS> S5a-2: the static main-view colour pass draws the plain runs of the groups' plans from the scene table (one record per run, in the loop's order); everything else is drawn by the code below
    const bool post_water = getType() == LLDrawPool::POOL_ALPHA_POST_WATER;
    const bool alpha_table = !rigged && !depth_only && !mFringePass && !LLPipeline::sImpostorRender && !LLPipeline::sRenderingHUDs && !gCubeSnapshot && !LLPipeline::sReflectionRender && ll_alpha_serving();
    struct PendingRange { U32 mHandle, mFirst, mCount; };
    static thread_local std::vector<PendingRange> pending;
    pending.clear();
    int pending_key = -1;
    bool pass_begun = false;
    LLShaderProgram* min_alpha_zero_program = nullptr;       // the pull program whose minimum alpha a custom blend run set to 0 (put back by the next run of it with a standard blend)
    int setup_key = -1;     // the key whose pull program is bound with its blend function, lights and values in place, since the last old draw or glow pass (anything that could change them clears it)
    auto run_flush = [&]()
    {
        if (pending.empty())
        {
            return;
        }
        if (!pass_begun)
        {   // the pass's matrices, once: the runs that follow carry only their ranges
            F32 matrices[32];
            memcpy(matrices, gRenderModelView, 16 * sizeof(F32));
            memcpy(matrices + 16, glm::value_ptr(gRender.getProjectionMatrix()), 16 * sizeof(F32));
            LLVkBridge::ShadowPacketsHeader begin_header;
            begin_header.mKind = 29;
            begin_header.mEntries = 0;
            LLVkBridge::shadowPackets(begin_header, matrices, sizeof(matrices));
            pass_begun = true;
            sAlphaScene.mBytes += sizeof(matrices);
        }
        LLVkBridge::ShadowPacketsHeader header;
        header.mKind = 29;
        header.mEntries = (U32)pending.size();
        {
            LLRenderState::CullScope cull_face(false, ((pending_key >> 16) & 1) != 0);       // a double sided glTF material: culling off for its run
            LLVkBridge::shadowPackets(header, pending.data(), pending.size() * sizeof(PendingRange));
        }
        ++sAlphaScene.mRecords;
        sAlphaScene.mRanges += pending.size();
        sAlphaScene.mBytes += pending.size() * sizeof(PendingRange);
        pending.clear();
        pending_key = -1;
    };
    // a new run: the pull program of its key bound, the blend function, the per-draw values a plain draw has (spec 1, env 0, brightness 1)
    auto run_begin = [&](int key)
    {
        const int pkey = key & 0xFF;        // the program: 0 lit, 1 fullbright, 2 + mask a material, 18 PBR; the blend function pair id is in bits 8-15, bit 16: double sided
        LLShaderProgram* pull = pkey == 18 ? &gDeferredPBRAlphaPullProgram : (pkey >= 2 ? &gDeferredMaterialPullProgram[pkey - 2] : (pkey ? &gDeferredFullbrightAlphaPullProgram : &gDeferredAlphaPullProgram));
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
                current_shader->setMinimumAlpha(MINIMUM_ALPHA);
                min_alpha_zero_program = nullptr;
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
    bool initialized_lighting = false;
    bool light_enabled = true;

    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    const LLShaderProgram* lastAvatarShader = nullptr;
    bool skipLastSkin = false;

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
        AlphaCensus::GroupScope census_group(cen);
        llassert(group);
        llassert(group->getSpatialPartition());

        if (group->getSpatialPartition()->mRenderByGroup &&
            !group->isDead())
        {

            LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : group->getExtents();

            if (!LLPipeline::sRenderingHUDs) // ignore above/below water for HUD render
            {
                if (above_water)
                { // reject any spatial groups that have no part above water
                    if (ext[1].getF32ptr()[2] < water_height)
                    {
                        cen.groupSkip(0);
                        continue;
                    }
                }
                else
                { // reject any spatial groups that he no part below water
                    if (ext[0].getF32ptr()[2] > water_height)
                    {
                        cen.groupSkip(0);
                        continue;
                    }
                }
            }

            static std::vector<LLDrawInfo*> emissives;
            static std::vector<LLDrawInfo*> rigged_emissives;
            static std::vector<LLDrawInfo*> pbr_emissives;
            static std::vector<LLDrawInfo*> pbr_rigged_emissives;

            emissives.resize(0);
            rigged_emissives.resize(0);
            pbr_emissives.resize(0);
            pbr_rigged_emissives.resize(0);

            bool is_particle_or_hud_particle = group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE
                                                      || group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE;

            // <FS:LO> Dont suspend partical processing while particles are hidden, just skip over drawing them
            if(!(gPipeline.sRenderParticles) && (
                                                 group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_PARTICLE ||
                                                 group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_HUD_PARTICLE))
            {
                cen.groupSkip(1);
                continue;
            }
            // </FS:LO>

            bool disable_cull = is_particle_or_hud_particle;
            LLAlphaPlanView plan;
            const bool group_plan = alpha_table && !is_particle_or_hud_particle && ll_alpha_plan(group, plan) == 1 && plan.mHandle != 0;
            U32 plan_ord = 0, plan_seg = 0;
            if (!group_plan)
            {
                run_flush();        // (the culling state of a group the table does not continue is its own)
                setup_key = -1;
            }
            LLRenderState::CullScope cull(false, disable_cull);
            cen.groupAccepted(group, is_particle_or_hud_particle, bridge != nullptr);

            LLSpatialGroup::drawmap_elem_t& draw_info = rigged ? group->mDrawMap[LLRenderPass::PASS_ALPHA_RIGGED] : group->mDrawMap[LLRenderPass::PASS_ALPHA];

            const bool plan_direct = group_plan && plan.mDirect && !cen.mOn;
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
                        if (pending_key != (int)seg.mKey)
                        {
                            run_flush();
                            run_begin((int)seg.mKey);
                        }
                        pending.push_back({ plan.mHandle, seg.mRank, seg.mCount });
                        sAlphaScene.mServed += seg.mCount;
                        if (post_water)
                        {
                            while (glow_at < plan.mGlowCount && plan.mGlowIdx[glow_at] < seg.mFirst)
                            {
                                ++glow_at;
                            }
                            while (glow_at < plan.mGlowCount && plan.mGlowIdx[glow_at] < seg.mFirst + seg.mCount)
                            {
                                LLDrawInfo* glow_draw = const_cast<LLDrawInfo*>(plan.mOrder[plan.mGlowIdx[glow_at]]);
                                (glow_draw->mGLTFMaterial.notNull() ? pbr_emissives : emissives).push_back(glow_draw);
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
                cen.drawBegin(params, rigged);
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
                            if (pending_key != (int)seg.mKey)
                            {
                                run_flush();
                                run_begin((int)seg.mKey);
                            }
                            pending.push_back({ plan.mHandle, seg.mRank, seg.mCount });
                        }
                        ++sAlphaScene.mServed;
                        if (post_water && params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
                        {
                            (params.mGLTFMaterial.notNull() ? pbr_emissives : emissives).push_back(&params);
                        }
                        cen.drawEnd();
                        continue;
                    }
                    if (!(plan_seg < plan.mSegCount && !plan.mSegs[plan_seg].mServed && o < plan.mOrderCount && plan.mOrder[o] == &params))
                    {
                        ++sAlphaScene.mMismatch;        // (the plan does not list this draw here: drawn the old way)
                    }
                }
                run_flush();        // an old draw: the pending run goes first (its program is still bound)
                setup_key = -1;
                if (alpha_table)
                {
                    ++sAlphaScene.mOld;
                }

                LLRenderPass::applyModelMatrix(params);

                LLMaterial* mat = NULL;
                LLGLTFMaterial *gltf_mat = params.mGLTFMaterial;

                LLRenderState::CullScope cull_face(false, gltf_mat && gltf_mat->mDoubleSided);

                if (gltf_mat && gltf_mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND)
                {
                    target_shader = pbr_shader;
                    if (params.mAvatar != nullptr)
                    {
                        target_shader = target_shader->mRiggedVariant;
                    }

                    // shader must be bound before LLGLTFMaterial::bind
                    if (current_shader != target_shader)
                    {
                        gPipeline.bindDeferredShaderFast(*target_shader);
                    }

                    params.mGLTFMaterial->bind(params.mTexture);
                    cen.stamp(AlphaCensus::S_CHOOSE);
                }
                else
                {
                    mat = LLPipeline::sRenderingHUDs ? nullptr : params.mMaterial;

                    if (params.mFullbright)
                    {
                        // Turn off lighting if it hasn't already been so.
                        if (light_enabled || !initialized_lighting)
                        {
                            initialized_lighting = true;
                            target_shader = fullbright_shader;

                            light_enabled = false;
                        }
                    }
                    // Turn on lighting if it isn't already.
                    else if (!light_enabled || !initialized_lighting)
                    {
                        initialized_lighting = true;
                        target_shader = simple_shader;
                        light_enabled = true;
                    }

                    if (LLPipeline::sRenderingHUDs)
                    {
                        target_shader = fullbright_shader;
                    }
                    else if (mat)
                    {
                        U32 mask = params.mShaderMask;

                        llassert(mask < LLMaterial::SHADER_COUNT);
                        target_shader = &(gDeferredMaterialProgram[mask]);
                    }
                    else if (!params.mFullbright)
                    {
                        target_shader = simple_shader;
                    }
                    else
                    {
                        target_shader = fullbright_shader;
                    }

                    if (params.mAvatar != nullptr)
                    {
                        llassert(target_shader->mRiggedVariant != nullptr);
                        target_shader = target_shader->mRiggedVariant;
                    }

                    if (current_shader != target_shader)
                    {// If we need shaders, and we're not ALREADY using the proper shader, then bind it
                    // (this way we won't rebind shaders unnecessarily).
                        gPipeline.bindDeferredShaderFast(*target_shader);

                        if (params.mFullbright)
                        { // make sure the bind the exposure map for fullbright shaders so they can cancel out exposure
                            S32 channel = target_shader->enableTexture(LLShaderMgr::EXPOSURE_MAP);
                            if (channel > -1)
                            {
                                gRender.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
                            }
                        }
                    }

                    cen.stamp(AlphaCensus::S_CHOOSE);
                    LLVector4 spec_color(1, 1, 1, 1);
                    F32 env_intensity = 0.0f;
                    F32 brightness = 1.0f;

                    // We have a material.  Supply the appropriate data here.
                    if (mat)
                    {
                        spec_color = params.mSpecColor;
                        env_intensity = params.mEnvIntensity;
                        brightness = params.mFullbright ? 1.f : 0.f;
                    }

                    if (current_shader)
                    {
                        // <FS> U1: the three per-draw values are written only when they differ from what this loop last wrote to the program (a per-program record, valid in this epoch only)
                        const F32 vals[6] = { spec_color.mV[VRED], spec_color.mV[VGREEN], spec_color.mV[VBLUE], spec_color.mV[VALPHA], env_intensity, brightness };
                        const bool same = alpha_values_on && current_shader->mAlphaEpoch == sAlphaEpoch && memcmp(current_shader->mAlphaVals, vals, sizeof(vals)) == 0;
                        if (!same || alpha_values_verify)
                        {
                            std::vector<U8> blocks[5];
                            if (same)
                            {   // verify: the writes must change nothing
                                for (U32 c = 0; c < 5; ++c) blocks[c] = current_shader->vkBlock(c);
                            }
                            current_shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, spec_color.mV[VRED], spec_color.mV[VGREEN], spec_color.mV[VBLUE], spec_color.mV[VALPHA]);
                            current_shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, env_intensity);
                            current_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, brightness);
                            if (same)
                            {
                                ++gPassUniAlphaSkipped;
                                for (U32 c = 0; c < 5; ++c)
                                {
                                    if (current_shader->vkBlock(c) != blocks[c])
                                    {
                                        if (gPassUniAlphaMismatch++ < 20)
                                        {
                                            LL_WARNS("VkScene") << "[PassUni] MISMATCH alpha pool setters on program " << current_shader->getName() << " class " << c << LL_ENDL;
                                        }
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                ++gPassUniAlphaMade;
                            }
                            memcpy(current_shader->mAlphaVals, vals, sizeof(vals));
                            current_shader->mAlphaEpoch = sAlphaEpoch;
                        }
                        else
                        {
                            ++gPassUniAlphaSkipped;
                        }
                    }
                }

                cen.stamp(AlphaCensus::S_UNIFORMS);
                if (params.mAvatar && !uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, lastAvatarShader, skipLastSkin))
                {
                    cen.dropped();
                    continue;
                }

                cen.stamp(AlphaCensus::S_PALETTE);
                bool tex_setup = TexSetup(&params, (mat != nullptr));
                cen.stamp(AlphaCensus::S_TEXTURES);

                if (mDepthCutoff > 0.f && current_shader)
                {   // <FS> depth only pass and colour pass of the rigged alpha (hair halo): the shader of this draw cuts at the cutoff instead of 1/255 (a custom blend draw below sets 0 and puts this back)
                    if (std::find(mDepthCutoffShaders.begin(), mDepthCutoffShaders.end(), current_shader) == mDepthCutoffShaders.end())
                    {
                        mDepthCutoffShaders.push_back(current_shader);
                    }
                    current_shader->setMinimumAlpha(mDepthCutoff);
                }

                {
                    cen.state(target_shader, params, gltf_mat && gltf_mat->mDoubleSided, disable_cull);
                    gRender.blendFunc((LLRender::eBlendFactor) params.mBlendFuncSrc, (LLRender::eBlendFactor) params.mBlendFuncDst, mAlphaSFactor, mAlphaDFactor);

                    bool reset_minimum_alpha = false;
                    if (!LLPipeline::sImpostorRender &&
                        params.mBlendFuncDst != LLRender::BF_SOURCE_ALPHA &&
                        params.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA)
                    { // this draw call has a custom blend function that may require rendering of "invisible" fragments
                        current_shader->setMinimumAlpha(0.f);
                        reset_minimum_alpha = true;
                    }

                    params.mVertexBuffer->setBuffer();
                    params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

                    if (reset_minimum_alpha)
                    {
                        current_shader->setMinimumAlpha(mDepthCutoff > 0.f ? mDepthCutoff : MINIMUM_ALPHA);
                    }
                }

                cen.stamp(AlphaCensus::S_DRAW);
                // If this alpha mesh has glow, then draw it a second time to add the destination-alpha (=glow).  Interleaving these state-changing calls is expensive, but glow must be drawn Z-sorted with alpha.
                if (getType() != LLDrawPool::POOL_ALPHA_PRE_WATER &&
                    params.mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE))
                {
                    if (params.mAvatar != nullptr)
                    {
                        if (params.mGLTFMaterial.isNull())
                        {
                            rigged_emissives.push_back(&params);
                        }
                        else
                        {
                            pbr_rigged_emissives.push_back(&params);
                        }
                    }
                    else
                    {
                        if (params.mGLTFMaterial.isNull())
                        {
                            emissives.push_back(&params);
                        }
                        else
                        {
                            pbr_emissives.push_back(&params);
                        }
                    }
                }

                if (tex_setup)
                {
                    LLRenderMatrix::identity(LLRenderMatrix::Kind::Texture0);
                }
                cen.drawEnd();
            }

            // render emissive faces into alpha channel for bloom effects
            if (!depth_only && !mFringePass)
            {
                if (!emissives.empty() || !pbr_emissives.empty() || !rigged_emissives.empty() || !pbr_rigged_emissives.empty())
                {
                    run_flush();
                    setup_key = -1;
                }
                cen.glowBegin(emissives.size(), pbr_emissives.size(), rigged_emissives.size(), pbr_rigged_emissives.size());
                gPipeline.enableLightsDynamic();

                // install glow-accumulating blend mode
                // don't touch color, add to alpha (glow)
                gRender.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE, LLRender::BF_ONE, LLRender::BF_ONE);

                bool rebind = false;
                LLShaderProgram* lastShader = current_shader;
                if (!emissives.empty())
                {
                    light_enabled = true;
                    renderEmissives(emissives);
                    rebind = true;
                }

                if (!pbr_emissives.empty())
                {
                    light_enabled = true;
                    renderPbrEmissives(pbr_emissives);
                    rebind = true;
                }

                if (!rigged_emissives.empty())
                {
                    light_enabled = true;
                    renderRiggedEmissives(rigged_emissives);
                    rebind = true;
                }

                if (!pbr_rigged_emissives.empty())
                {
                    light_enabled = true;
                    renderRiggedPbrEmissives(pbr_rigged_emissives);
                    rebind = true;
                }

                // restore our alpha blend mode
                gRender.blendFunc(mColorSFactor, mColorDFactor, mAlphaSFactor, mAlphaDFactor);

                if (lastShader && rebind)
                {
                    lastShader->bind();
                }
                if (rebind)
                {
                    ++sAlphaEpoch;      // <FS> U1: the emissive passes wrote their own values
                }
                cen.glowEnd();
            }
        }
    }

    run_flush();
    if (alpha_table && gFrameCount >= sAlphaScene.mLast + 600)
    {
        const double frames = (double)std::max<U32>(1, gFrameCount - sAlphaScene.mLast);
        sAlphaScene.mLast = gFrameCount;
        LL_INFOS("VkScene") << "alpha from the scene table (S5a), viewer side per frame: " << (double)sAlphaScene.mCalls / frames << " passes, " << (double)sAlphaScene.mRecords / frames << " records, "
            << (double)sAlphaScene.mRanges / frames << " ranges, " << (double)sAlphaScene.mServed / frames << " draws served, " << (double)sAlphaScene.mOld / frames << " drawn the old way, "
            << (double)sAlphaScene.mBytes / frames << " bytes sent, plan mismatches " << sAlphaScene.mMismatch << LL_ENDL;
        sAlphaScene.mCalls = sAlphaScene.mRecords = sAlphaScene.mRanges = sAlphaScene.mServed = sAlphaScene.mOld = sAlphaScene.mBytes = 0;
    }

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    LLVertexBuffer::unbind();

    if (!light_enabled)
    {
        gPipeline.enableLightsDynamic();
    }
}
