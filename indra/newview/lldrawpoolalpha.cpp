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
        F32 scale = 2.f;                    // chosen by eye (Adam, plaza: a logo on the floor next to the ground): twice the sun disc, 6 rays per sample setting
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

void LLDrawPoolAlpha::renderAlpha(U32 mask, bool depth_only, bool rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
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
                continue;
            }
            // </FS:LO>

            bool disable_cull = is_particle_or_hud_particle;
            LLRenderState::CullScope cull(false, disable_cull);

            LLSpatialGroup::drawmap_elem_t& draw_info = rigged ? group->mDrawMap[LLRenderPass::PASS_ALPHA_RIGGED] : group->mDrawMap[LLRenderPass::PASS_ALPHA];

            for (LLSpatialGroup::drawmap_elem_t::iterator k = draw_info.begin(); k != draw_info.end(); ++k)
            {
                LLDrawInfo& params = **k;
                if ((bool)params.mAvatar != rigged)
                {
                    continue;
                }

                LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("ra - push batch");

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
                        current_shader->uniform4f(LLShaderMgr::SPECULAR_COLOR, spec_color.mV[VRED], spec_color.mV[VGREEN], spec_color.mV[VBLUE], spec_color.mV[VALPHA]);
                        current_shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, env_intensity);
                        current_shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, brightness);
                    }
                }

                if (params.mAvatar && !uploadMatrixPalette(params.mAvatar, params.mSkinInfo, lastAvatar, lastMeshId, lastAvatarShader, skipLastSkin))
                {
                    continue;
                }

                bool tex_setup = TexSetup(&params, (mat != nullptr));

                if (mDepthCutoff > 0.f && current_shader)
                {   // <FS> depth only pass and colour pass of the rigged alpha (hair halo): the shader of this draw cuts at the cutoff instead of 1/255 (a custom blend draw below sets 0 and puts this back)
                    if (std::find(mDepthCutoffShaders.begin(), mDepthCutoffShaders.end(), current_shader) == mDepthCutoffShaders.end())
                    {
                        mDepthCutoffShaders.push_back(current_shader);
                    }
                    current_shader->setMinimumAlpha(mDepthCutoff);
                }

                {
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
            }

            // render emissive faces into alpha channel for bloom effects
            if (!depth_only && !mFringePass)
            {
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
            }
        }
    }

    gRender.setSceneBlendType(LLRender::BT_ALPHA);

    LLVertexBuffer::unbind();

    if (!light_enabled)
    {
        gPipeline.enableLightsDynamic();
    }
}
