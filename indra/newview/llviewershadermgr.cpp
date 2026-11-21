/**
 * @file llviewershadermgr.cpp
 * @brief Viewer shader manager implementation.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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

#include <boost/lexical_cast.hpp>

#include "llfeaturemanager.h"
#include "llviewershadermgr.h"
#include "llviewercontrol.h"
#include "llversioninfo.h"

#include "llrender.h"
#include "llenvironment.h"
#include "llerrorcontrol.h"
#include "llworld.h"
#include "llsky.h"

#include "pipeline.h"

#include "llfile.h"
#include "llviewerwindow.h"
#include "rkvkbridge.h"
#include "llwindow.h"

#include "lljoint.h"
#include "llskinningutil.h"

static LLStaticHashedString sTexture0("texture0");
static LLStaticHashedString sTexture1("texture1");
static LLStaticHashedString sTex0("tex0");
static LLStaticHashedString sTex1("tex1");
static LLStaticHashedString sDitherTex("dither_tex");
static LLStaticHashedString sGlowMap("glowMap");
static LLStaticHashedString sScreenMap("screenMap");

// Lots of STL stuff in here, using namespace std to keep things more readable
using std::vector;
using std::pair;
using std::make_pair;
using std::string;

bool                LLViewerShaderMgr::sInitialized = false;
bool                LLViewerShaderMgr::sSkipReload = false;

LLVector4           gShinyOrigin;

S32 clamp_terrain_mapping(S32 mapping)
{
    // 1 = "flat", 2 not implemented, 3 = triplanar mapping
    mapping = llclamp(mapping, 1, 3);
    if (mapping == 2) { mapping = 1; }
    return mapping;
}

//utility shaders
RKShaderProgram    gGlowCombineProgram;
RKShaderProgram    gReflectionMipProgram;
RKShaderProgram    gGaussianProgram;
RKShaderProgram    gRadianceGenProgram;
RKShaderProgram    gIrradianceGenProgram;
RKShaderProgram    gGlowCombineFXAAProgram;
RKShaderProgram    gTwoTextureCompareProgram;
RKShaderProgram    gOneTextureFilterProgram;
RKShaderProgram    gDebugProgram;
RKShaderProgram    gSkinnedDebugProgram;
RKShaderProgram    gNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT];
RKShaderProgram    gAlphaMaskProgram;
RKShaderProgram    gBenchmarkProgram;
RKShaderProgram    gReflectionProbeDisplayProgram;
RKShaderProgram    gCopyProgram;
RKShaderProgram    gCopyDepthProgram;
RKShaderProgram    gPBRTerrainBakeProgram;
RKShaderProgram    gDrawColorProgram;

//object shaders
RKShaderProgram        gObjectPreviewProgram;
RKShaderProgram        gSkinnedObjectPreviewProgram;
RKShaderProgram        gPhysicsPreviewProgram;
RKShaderProgram        gObjectFullbrightAlphaMaskProgram;
RKShaderProgram        gObjectAlphaMaskNoColorProgram;

//environment shaders
RKShaderProgram        gWaterPullProgram;          // <FS> the table form of the water planes (native only: water.vert / water.frag with WATER_PULL)
RKShaderProgram        gUnderWaterPullProgram;
RKShaderProgram        gWaterPullRtReflectionProgram;   // <FS> the same that also traces the reflection of the water (the setting RKVkRTWater; needs the material table)
RKShaderProgram        gWaterPullRtProgram;        // <FS> the same with the ray queries of the stock water (native only: manifest Water Pull RT Shader, RK_RT_QUERY), made in a session with ray queries
static bool            sWaterRtReflectionMade = false;
static bool            sWaterRtMade = false;       // the program above was made in this session
static bool            sRtSessionShaders = false;     // the stock programs of this session carry ray query code (the `rt` lines of the manifest keep them): the water planes keep the stock water program then

//interface shaders
RKShaderProgram        gHighlightProgram;
RKShaderProgram        gSkinnedHighlightProgram;
RKShaderProgram        gHighlightPullSkinProgram;       // <FS> K6 B1: "Highlight Pull Skin Shader": a placeholder (the highlight shaders with HAS_SKIN) for its native stand-in (highlight.frag + alpha.vert), which draws the rigged table items of the Highlight Transparent view
RKShaderProgram        gDebugBatchPullProgram;          // <FS> K6 B1: "Debug Batch Pull Shader" and its skinned twin: placeholders (the highlight shaders) for the native programs of the Batch Size and Texture Animation views
RKShaderProgram        gDebugBatchPullSkinProgram;
RKShaderProgram        gHighlightNormalProgram;
RKShaderProgram        gHighlightSpecularProgram;

RKShaderProgram        gDeferredHighlightProgram;

RKShaderProgram        gPathfindingProgram;
RKShaderProgram        gPathfindingNoNormalsProgram;

//avatar shader handles
RKShaderProgram        gAvatarProgram;
RKShaderProgram        gImpostorProgram;

// Effects Shaders
RKShaderProgram            gGlowProgram;
RKShaderProgram            gGlowExtractProgram;
RKShaderProgram            gPostVignetteProgram;   // <FS:CR> Import Vignette from Exodus
RKShaderProgram            gPostSnapshotFrameProgram;   // <FS:Beq/> Add Snapshot frame guide

// Deferred rendering shaders
RKShaderProgram            gDeferredImpostorProgram;
RKShaderProgram            gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram;
RKShaderProgram            gDeferredTerrainPullProgram;

RKShaderProgram            gDeferredPBRTerrainPullProgram[TERRAIN_PAINT_TYPE_COUNT];
RKShaderProgram            gDeferredTreePullProgram;
RKShaderProgram            gDeferredAvatarProgram;
RKShaderProgram            gDeferredAvatarAlphaProgram;
RKShaderProgram            gDeferredMultiLightProgram;
RKShaderProgram            gDeferredMultiSpotLightProgram;
RKShaderProgram            gDeferredSunProgram;
RKShaderProgram            gDeferredSunProbeProgram;
RKShaderProgram            gHazeProgram;
RKShaderProgram            gWaterHazePullProgram;        // <FS> the table form of the water patches of the haze (native only: waterhaze.vert / waterhaze.frag with WATER_PULL)

// <FS> the water seen from above in a session with ray queries (the ray traced sun shadow and reflection of the water): the pull program with the ray queries, native/manifest.txt `Water Pull RT Shader`
bool rk_water_rt()
{
    return sWaterRtMade;
}

bool rk_water_rt_reflection()
{
    return sWaterRtReflectionMade;
}
RKShaderProgram            gDeferredBlurLightProgram;
RKShaderProgram            gNativeSunHalfProgram;
RKShaderProgram            gNativeLightUpsampleProgram;
RKShaderProgram            gDeferredSoftenProgram;
RKShaderProgram            gDeferredRtReflectionProgram;
RKShaderProgram            gDeferredShadowCubeProgram;
RKShaderProgram            gDeferredShadowPullProgram;
RKShaderProgram            gDeferredShadowAlphaPullProgram;
RKShaderProgram            gDeferredShadowSkinPullProgram;
RKShaderProgram            gDeferredDiffusePullProgram;
RKShaderProgram            gDeferredAlphaPullProgram;
RKShaderProgram            gDeferredFullbrightAlphaPullProgram;
RKShaderProgram            gDeferredFullbrightPullProgram;        // S6 X1: the OPAQUE fullbright pass from the scene table (alphaPullV with IS_FULLBRIGHT_PULL + fullbrightF, no alpha, no blending in the shader)
RKShaderProgram            gDeferredFullbrightPullSkinProgram;
RKShaderProgram            gDeferredFullbrightShinyPullProgram;       // J1 K4: the fullbright shiny pass from the scene table
RKShaderProgram            gDeferredFullbrightShinyPullSkinProgram;
RKShaderProgram            gHUDFullbrightPullProgram;       // J1 K6 S1: the HUD's fullbright pass from the scene table
RKShaderProgram            gHUDFullbrightMaskPullProgram;
RKShaderProgram            gHUDFullbrightShinyPullProgram;
RKShaderProgram            gHUDPBROpaquePullProgram;        // J1 K6 S1: the HUD's glTF opaque / mask pass from the scene table
RKShaderProgram            gDeferredBumpPullProgram;       // J1 K4: the deferred bump pass from the scene table
RKShaderProgram            gDeferredBumpPullSkinProgram;
RKShaderProgram            gDeferredEmbossPullProgram;       // J1 K4: the post deferred emboss pass from the scene table
RKShaderProgram            gDeferredEmbossPullSkinProgram;
RKShaderProgram            gDeferredDiffuseAlphaMaskPullProgram;           // S6 X3: the alpha masked diffuse G-buffer pass and the alpha masked fullbright pass from the table (the cutoff of each draw as a flat varying)
RKShaderProgram            gDeferredDiffuseAlphaMaskPullSkinProgram;
RKShaderProgram            gDeferredFullbrightAlphaMaskPullProgram;
RKShaderProgram            gDeferredFullbrightAlphaMaskPullSkinProgram;
RKShaderProgram            gDeferredPBROpaquePullProgram;                 // S6 X4: the glTF opaque / mask G-buffer pass from the scene table (pbrAlphaPullV + pbropaqueF)
RKShaderProgram            gDeferredPBROpaquePullSkinProgram;
RKShaderProgram            gDeferredSpotLightPullProgram;                 // S6 LV2: the projectors of a lighting pass from one record (native light_volume.vert + light_spot.frag)
RKShaderProgram            gDeferredLightPullProgram;                     // S6 LV1: the point light volumes of a lighting pass from one record (native light_volume.vert + light_point.frag)
RKShaderProgram            gDeferredAlphaUberPullProgram;          // <FS> item D1: the lit alpha pull program that also draws the fullbright alpha draws (alphaF.glsl with ALPHA_UBER)
RKShaderProgram            gDeferredAlphaUberPullSkinProgram;
RKShaderProgram            gDeferredAlphaUberMatPullProgram;       // <FS> item D2: the merged lit alpha program that also draws the material alpha draws (ALPHA_UBER_MAT)
RKShaderProgram            gDeferredAlphaUberMatPullSkinProgram;
RKShaderProgram            gDeferredPBRAlphaPullProgram;
RKShaderProgram            gDeferredAlphaPullSkinProgram;
RKShaderProgram            gDeferredFullbrightAlphaPullSkinProgram;
RKShaderProgram            gDeferredPBRAlphaPullSkinProgram;
RKShaderProgram            gDeferredEmissivePullProgram;
RKShaderProgram            gDeferredEmissivePullSkinProgram;
RKShaderProgram            gPBRGlowPullProgram;
RKShaderProgram            gPBRGlowPullSkinProgram;
RKShaderProgram            gHUDFullbrightAlphaPullProgram;
RKShaderProgram            gHUDPBRAlphaPullProgram;
RKShaderProgram            gDeferredAlphaImpostorPullProgram;
RKShaderProgram            gDeferredAlphaImpostorPullSkinProgram;
RKShaderProgram            gDeferredFullbrightMaskPullProgram;
RKShaderProgram            gDeferredFullbrightMaskPullSkinProgram;
RKShaderProgram            gDeferredMaterialPullProgram[16];
RKShaderProgram            gDeferredDiffusePullSkinProgram;        // <FS> S4 A1: the same for rigged draws (kind 25 records: weights and palette by address)
RKShaderProgram            gDeferredMaterialPullSkinProgram[16];
RKShaderProgram            gDeferredAvatarShadowProgram;
RKShaderProgram            gDeferredAvatarAlphaShadowProgram;
RKShaderProgram            gDeferredAvatarAlphaMaskShadowProgram;
RKShaderProgram            gDeferredPostProgram;
RKShaderProgram            gDeferredCoFProgram;
RKShaderProgram            gDeferredDoFCombineProgram;
RKShaderProgram            gDeferredPostTonemapProgram;
RKShaderProgram            gNoPostTonemapProgram;
RKShaderProgram            gDeferredPostTonemapGammaCorrectProgram;
RKShaderProgram            gNoPostTonemapGammaCorrectProgram;
RKShaderProgram            gDeferredPostTonemapLegacyGammaCorrectProgram;
RKShaderProgram            gNoPostTonemapLegacyGammaCorrectProgram;
RKShaderProgram            gDeferredPostGammaCorrectProgram;
RKShaderProgram            gLegacyPostGammaCorrectProgram;
RKShaderProgram            gExposureProgram;
RKShaderProgram            gExposureProgramNoFade;
RKShaderProgram            gLuminanceProgram;
RKShaderProgram            gMotionVectorProgram;
RKShaderProgram            gWorldDepthSnapProgram;         // <FS> the world pass's depth as a colour texture, for the motion pass (native/depth_snap.frag)
RKShaderProgram            gRtShadowFilterProgram;         // <FS> the penumbra of the ray traced sun shadow from the blocker distance (PLAN-rt-shadow-penumbra.md)
RKShaderProgram            gRtShadowPrepProgram;           // <FS> its input pass
RKShaderProgram            gNRRimLiftProgram;              // <FS> DLSS-NR: the rim lift after the neural renderer
RKShaderProgram            gDnsrSunPackProgram, gDnsrSunUnpackProgram;       // <FS> the AMD shadow denoiser's hit mask of the sun lightmap and its result back into it
RKShaderProgram            gDnsrPrepProgram, gDnsrPackProgram, gDnsrUnpackProgram;       // <FS> the AMD reflection denoiser's inputs and its result back (PLAN-amd-denoiser.md)
RKShaderProgram            gAvatarVelocityDebugProgram;    // <FS> debug view of a motion target
RKShaderProgram            gRtMaterialDebugProgram;        // <FS> debug view of the ray scene's material table (RenderBufferVisualization 9)
RKShaderProgram            gFXAAProgram[4];
RKShaderProgram            gSMAAEdgeDetectProgram[4];
RKShaderProgram            gSMAABlendWeightsProgram[4];
RKShaderProgram            gSMAANeighborhoodBlendProgram[4];
RKShaderProgram            gCASProgram;
RKShaderProgram            gCASLegacyGammaProgram;
RKShaderProgram            gDeferredPostNoDoFProgram;
RKShaderProgram            gDeferredPostNoDoFNoiseProgram;
RKShaderProgram            gDeferredWLSkyProgram;
RKShaderProgram            gEnvironmentMapProgram;
RKShaderProgram            gDeferredWLCloudProgram;
RKShaderProgram            gDeferredWLSunProgram;
RKShaderProgram            gDeferredWLMoonProgram;
RKShaderProgram            gDeferredStarProgram;
RKShaderProgram            gNormalMapGenProgram;
RKShaderProgram            gDeferredGenBrdfLutProgram;
RKShaderProgram            gDeferredBufferVisualProgram;
// [RLVa:KB] - @setsphere
RKShaderProgram            gRlvSphereProgram;
// [/RLVa:KB]

// Deferred materials shaders
RKShaderProgram            gDeferredPBRAlphaProgram;

RKShaderProgram            gGLTFPBRMetallicRoughnessProgram;


//helper for making a rigged variant of a given shader

static bool make_rigged_variant(RKShaderProgram& shader, RKShaderProgram& riggedShader)
{
    riggedShader.mName = llformat("Skinned %s", shader.mName.c_str());
    riggedShader.mFeatures = shader.mFeatures;
    riggedShader.mDefines = shader.mDefines;    // NOTE: Must come before addPermutation

    riggedShader.mShaderLevel = shader.mShaderLevel;
    riggedShader.mShaderGroup = shader.mShaderGroup;

    shader.mRiggedVariant = &riggedShader;
    return riggedShader.createShader();
}

static void add_common_permutations(RKShaderProgram* shader)
{
    static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);

    if (emissive)
    {
    }
}


static bool make_gltf_variant(RKShaderProgram& shader, RKShaderProgram& variant, bool alpha_blend, bool rigged, bool unlit, bool multi_uv, bool use_sun_shadow)
{
    // each variant is a program of its own (a manifest line each: gltf_scene.vert / gltf_scene.frag with ALPHA_BLEND, HAS_SKIN, UNLIT, MULTI_UV)
    variant.mName = shader.mName + (alpha_blend ? " blend" : "") + (rigged ? " rigged" : "") + (unlit ? " unlit" : "") + (multi_uv ? " multiuv" : "");
    variant.mFeatures = shader.mFeatures;
    variant.mShaderLevel = shader.mShaderLevel;
    variant.mShaderGroup = shader.mShaderGroup;

    variant.mDefines = shader.mDefines;    // NOTE: Must come before addPermutation

    U32 node_size = 16 * 3;
    U32 max_nodes = gDeviceCaps.mMaxUniformBlockSize / node_size;

    U32 material_size = 16 * 12;
    U32 max_materials = gDeviceCaps.mMaxUniformBlockSize / material_size;
    RKShaderProgram::sMaxGLTFMaterials = max_materials;


    U32 max_vec4s = gDeviceCaps.mMaxUniformBlockSize / 16;

    if (rigged)
    {
    }

    if (unlit)
    {
    }

    if (multi_uv)
    {
    }

    if (alpha_blend)
    {

        variant.mFeatures.calculatesLighting = false;
        variant.mFeatures.hasLighting = false;
        variant.mFeatures.calculatesAtmospherics = true;
        variant.mFeatures.hasReflectionProbes = true;

        if (use_sun_shadow)
        {
        }

        bool success = variant.createShader();
        llassert(success);

        // Alpha Shader Hack
        // See: LLRender::syncMatrices()
        variant.mFeatures.calculatesLighting = true;
        variant.mFeatures.hasLighting = true;

        return success;
    }
    else
    {
        return variant.createShader();
    }
}

static bool make_gltf_variants(RKShaderProgram& shader, bool use_sun_shadow)
{
    shader.mGLTFVariants.resize(RKShaderProgram::NUM_GLTF_VARIANTS);

    for (U32 i = 0; i < RKShaderProgram::NUM_GLTF_VARIANTS; ++i)
    {
        bool alpha_blend = i & RKShaderProgram::GLTFVariant::ALPHA_BLEND;
        bool rigged = i & RKShaderProgram::GLTFVariant::RIGGED;
        bool unlit = i & RKShaderProgram::GLTFVariant::UNLIT;
        bool multi_uv = i & RKShaderProgram::GLTFVariant::MULTI_UV;

        if (!make_gltf_variant(shader, shader.mGLTFVariants[i], alpha_blend, rigged, unlit, multi_uv, use_sun_shadow))
        {
            return false;
        }
    }

    return true;
}

LLViewerShaderMgr::LLViewerShaderMgr() :
    mShaderLevel(SHADER_COUNT, 0),
    mMaxAvatarShaderLevel(0)
{
}

LLViewerShaderMgr::~LLViewerShaderMgr()
{
    mShaderLevel.clear();
}

// static
LLViewerShaderMgr * LLViewerShaderMgr::instance()
{
    if(NULL == sInstance)
    {
        sInstance = new LLViewerShaderMgr();
    }

    return static_cast<LLViewerShaderMgr*>(sInstance);
}

// static
void LLViewerShaderMgr::releaseInstance()
{
    if (sInstance != NULL)
    {
        delete sInstance;
        sInstance = NULL;
    }
}

void LLViewerShaderMgr::initAttribsAndUniforms(void)
{
    if (mReservedAttribs.empty())
    {
        LLShaderMgr::initAttribsAndUniforms();
    }
}


//============================================================================
// Set Levels

S32 LLViewerShaderMgr::getShaderLevel(S32 type)
{
    return mShaderLevel[type];
}

//============================================================================
// Shader Management

void LLViewerShaderMgr::setShaders()
{
    LL_PROFILE_ZONE_SCOPED;
    //setShaders might be called redundantly by gSavedSettings, so return on reentrance
    static bool reentrance = false;

    if (!gPipeline.mInitialized || !sInitialized || reentrance || sSkipReload)
    {
        return;
    }


    if (!gDeviceCaps.mHasRequirements)
    {
        // Viewer will show 'hardware requirements' warning later
        LL_INFOS("ShaderLoading") << "Not supported hardware/software" << LL_ENDL;
        return;
    }

    static LLCachedControl<U32> max_texture_index(gSavedSettings, "RenderMaxTextureIndex", 16);

    // when using indexed texture rendering, leave some texture units available for shadow and reflection maps
    static LLCachedControl<S32> reserved_texture_units(gSavedSettings, "RenderReservedTextureIndices", 14);

    RKShaderProgram::sIndexedTextureChannels = 4;
    if (const char* channels = getenv("RK_VK_INDEXED_CHANNELS"))      // test switch: 1 = every draw one texture, no per-vertex texture index
    {
        RKShaderProgram::sIndexedTextureChannels = llclamp<S32>(atoi(channels), 1, 4);
    }
        //llclamp<S32>(max_texture_index, 1, gDeviceCaps.mNumTextureImageUnits-reserved_texture_units);

    reentrance = true;

    initAttribsAndUniforms();
    gPipeline.releaseRenderBuffers();

    unloadShaders();

    LLPipeline::sRenderGlow = gSavedSettings.getBOOL("RenderGlow");
    LLPipeline::RenderAvatarCloth = gSavedSettings.getBOOL("RenderAvatarCloth");

    if (gViewerWindow)
    {
        gViewerWindow->setCursor(UI_CURSOR_WAIT);
    }

    // Shaders
    LL_INFOS("ShaderLoading") << "\n~~~~~~~~~~~~~~~~~~\n Loading Shaders:\n~~~~~~~~~~~~~~~~~~" << LL_ENDL;
    LL_INFOS("ShaderLoading") << llformat("Using GLSL %d.%d", gDeviceCaps.mShaderLangMajor, gDeviceCaps.mShaderLangMinor) << LL_ENDL;

    for (S32 i = 0; i < SHADER_COUNT; i++)
    {
        mShaderLevel[i] = 0;
    }
    mMaxAvatarShaderLevel = 0;

    LLVertexBuffer::unbind();

    llassert((gDeviceCaps.mShaderLangMajor > 1 || gDeviceCaps.mShaderLangMinor >= 10));


    S32 light_class = 3;
    S32 interface_class = 2;
    S32 env_class = 2;
    S32 obj_class = 2;
    S32 effect_class = 2;
    S32 wl_class = 2;
    S32 water_class = 3;
    S32 deferred_class = 3;

    // Trigger a full rebuild of the fallback skybox / cubemap if we've toggled windlight shaders
    if (!wl_class || (mShaderLevel[SHADER_WINDLIGHT] != wl_class && gSky.mVOSkyp.notNull()))
    {
        gSky.mVOSkyp->forceSkyUpdate();
    }

    // Load lighting shaders
    mShaderLevel[SHADER_LIGHTING] = light_class;
    mShaderLevel[SHADER_INTERFACE] = interface_class;
    mShaderLevel[SHADER_ENVIRONMENT] = env_class;
    mShaderLevel[SHADER_WATER] = water_class;
    mShaderLevel[SHADER_OBJECT] = obj_class;
    mShaderLevel[SHADER_EFFECT] = effect_class;
    mShaderLevel[SHADER_WINDLIGHT] = wl_class;
    mShaderLevel[SHADER_DEFERRED] = deferred_class;

    std::string shader_name = loadBasicShaders();
    if (shader_name.empty())
    {
        LL_INFOS("Shader") << "Loaded basic shaders." << LL_ENDL;
    }
    else
    {
        // "ShaderLoading" and "Shader" need to be logged
        LL_WARNS("Shader") << "Failed loading basic shaders.  Retrying with increased log level..." << LL_ENDL;

        LLError::ELevel lvl = LLError::getDefaultLevel();
        LLError::setDefaultLevel(LLError::LEVEL_DEBUG);
        loadBasicShaders();
        LLError::setDefaultLevel(lvl);
        gDeviceCaps.printDeviceInfoString();
        LL_ERRS() << "Unable to load basic shader " << shader_name << ", verify graphics driver installed and current." << LL_ENDL;
        reentrance = false; // For hygiene only, re-try probably helps nothing
        return;
    }

    gPipeline.mShadersLoaded = true;

    bool loaded = loadShadersWater();

    if (loaded)
    {
        LL_INFOS() << "Loaded water shaders." << LL_ENDL;
    }
    else
    {
        LL_WARNS() << "Failed to load water shaders." << LL_ENDL;
        llassert(loaded);
    }

    if (loaded)
    {
        loaded = loadShadersEffects();
        if (loaded)
        {
            LL_INFOS() << "Loaded effects shaders." << LL_ENDL;
        }
        else
        {
            LL_WARNS() << "Failed to load effects shaders." << LL_ENDL;
            llassert(loaded);
        }
    }

    if (loaded)
    {
        loaded = loadShadersInterface();
        if (loaded)
        {
            LL_INFOS() << "Loaded interface shaders." << LL_ENDL;
        }
        else
        {
            LL_WARNS() << "Failed to load interface shaders." << LL_ENDL;
            llassert(loaded);
        }
    }

    if (loaded)
    {
        // Load max avatar shaders to set the max level
        mShaderLevel[SHADER_AVATAR] = 3;
        mMaxAvatarShaderLevel = 3;

        if (loadShadersObject())
        { //hardware skinning is enabled and rigged attachment shaders loaded correctly
            // cloth is a class3 shader
            constexpr S32 avatar_class = 1;

            // Set the actual level
            mShaderLevel[SHADER_AVATAR] = avatar_class;

            loaded = loadShadersAvatar();
            llassert(loaded);
        }
        else
        {
            // SHOULD NOT BE POSSIBLE
            // Hardware skinning not possible, neither is deferred rendering
            // but both are a hard requirement, viewer will crash without them
            LL_ERRS() << "Failed to load object shaders, cannot continue." << LL_ENDL;
        }
    }

    llassert(loaded);
    loaded = loaded && loadShadersDeferred();
    llassert(loaded);
    if (gViewerWindow)
    {
        gViewerWindow->setCursor(UI_CURSOR_ARROW);
    }
    gPipeline.createRenderBuffers();

    reentrance = false;
}

void LLViewerShaderMgr::unloadShaders()
{
    RKShaderProgram::dumpProgramCensus();       // <FS> RK_VK_PROGRAM_CENSUS=1: what was loaded and bound (before the programs forget their files)
    while (!RKShaderProgram::sInstances.empty())
    {
        RKShaderProgram* shader = *(RKShaderProgram::sInstances.begin());
        shader->unload();
    }

    mShaderLevel[SHADER_LIGHTING] = 0;
    mShaderLevel[SHADER_OBJECT] = 0;
    mShaderLevel[SHADER_AVATAR] = 0;
    mShaderLevel[SHADER_ENVIRONMENT] = 0;
    mShaderLevel[SHADER_WATER] = 0;
    mShaderLevel[SHADER_INTERFACE] = 0;
    mShaderLevel[SHADER_EFFECT] = 0;
    mShaderLevel[SHADER_WINDLIGHT] = 0;

    gPipeline.mShadersLoaded = false;
}

std::string LLViewerShaderMgr::loadBasicShaders()
{
    // (the stock library files of the basic shaders were loaded here; the native programs are built from the manifest: the defines below are what they and the stock text saw)

    std::map<std::string, std::string> attribs;
    attribs["MAX_JOINTS_PER_MESH_OBJECT"] =
        std::to_string(LLSkinningUtil::getMaxJointCount());

    static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);

    if (emissive)
    {
        attribs["HAS_EMISSIVE"] = "1";
    }

    bool ssr = gSavedSettings.getBOOL("RenderScreenSpaceReflections");

    bool mirrors = gSavedSettings.getBOOL("RenderMirrors");

    bool has_reflection_probes = gSavedSettings.getBOOL("RenderReflectionsEnabled") && gDeviceCaps.mApiVersionNumber > 3.99f;

    S32 probe_level = llclamp(gSavedSettings.getS32("RenderReflectionProbeLevel"), 0, 3);

    S32 shadow_detail            = gSavedSettings.getS32("RenderShadowDetail");

    if (shadow_detail >= 1)
    {
        attribs["SUN_SHADOW"] = "1";

        // <FS> ray query sun shadows (RK_VK_RT_SHADOWS, doc/vulkan/PLAN-rt-shadows.md): 1 replace the cascades, 4 and 5 debug views (2 split screen and 3 disagreement map are retired)
        if (const U32 rt_mode = RKVkBridge::rtShadowMode())
        {
            attribs["RK_RT_SHADOWS"] = std::to_string(rt_mode);
            // <FS> the sun ray's limit of masked candidates (see shadowUtil.glsl) and the debug that shows the rays that stopped at it
            if (const char* cand = getenv("RK_VK_RT_CANDIDATES"))
            {
                const S32 n = atoi(cand);
                if (n >= 8 && n <= 4096)
                {
                    attribs["RK_RT_MAX_CANDIDATES"] = std::to_string(n) + "u";
                }
            }
            if (const char* bias = getenv("RK_VK_RT_SUN_BIAS"))
            {
                const F32 b = (F32)atof(bias);
                if (b > 0.f && b < 5.f)
                {
                    attribs["RK_RT_SUN_BIAS"] = llformat("%.4f", b);
                }
            }
            if (const char* strength = getenv("RK_VK_RT_SUN_ALPHA_STRENGTH"))
            {
                const F32 s = (F32)atof(strength);
                if (s > 0.f && s <= 1.f)
                {
                    attribs["RK_RT_SUN_ALPHA_STRENGTH"] = llformat("%.3f", s);     // how much of its coverage a hair card blocks (soft alpha sun ray)
                }
            }
            if (const char* pen = getenv("RK_VK_RT_SUN_PENUMBRA"))
            {
                const F32 w = (F32)atof(pen);
                if (w >= 0.f && w < 1.f)
                {
                    attribs["RK_RT_SUN_PENUMBRA"] = llformat("%.4f", w);       // metres: the penumbra width of a close avatar self shadow on the skin (0 = off)
                }
            }
            if (const char* pen_rays = getenv("RK_VK_RT_SUN_PENUMBRA_RAYS"))
            {
                const S32 n = atoi(pen_rays);
                if (n >= 1 && n <= 16)
                {
                    attribs["RK_RT_SUN_PENUMBRA_RAYS"] = std::to_string(n);    // the rays of a pixel whose first ray met a close avatar surface
                }
            }
            if (const char* fade = getenv("RK_VK_RT_SUN_SELFFADE"))
            {
                const F32 f = (F32)atof(fade);
                if (f > 0.f && f <= 1.f)
                {
                    attribs["RK_RT_SUN_SELF_FADE"] = llformat("%.3f", f);      // fraction of SELFNEAR where the floor starts to fade (1 = no fade)
                }
            }
            if (const char* near_s = getenv("RK_VK_RT_SUN_SELFNEAR"))
            {
                const F32 n = (F32)atof(near_s);
                if (n > 0.f && n < 20.f)
                {
                    attribs["RK_RT_SUN_SELF_NEAR"] = llformat("%.3f", n);      // metres: a skinned avatar surface this close to the pixel it shadows counts as a translucent self shadow
                }
            }
            if (const char* dbg = getenv("RK_VK_RT_SUN_DEBUG"))
            {
                const S32 d = atoi(dbg);
                if (d == 1 || d == 2)
                {
                    attribs["RK_RT_SUN_DEBUG"] = std::to_string(d);        // test: 1 only solid hits make a sun shadow, 2 only what the alpha loop of the soft ray found
                }
            }
            if (const char* recv = getenv("RK_VK_RT_SUN_SELFRECV"))
            {
                const F32 r = (F32)atof(recv);
                if (r >= 0.f && r < 1.f)
                {
                    attribs["RK_RT_SUN_SELF_RECV"] = llformat("%.4f", r);       // metres: the probe through the receiver's surface that says it is an avatar (the floor applies only then); 0 = off
                }
            }
            if (const char* sss = getenv("RK_VK_RT_SUN_SSS"))
            {
                const F32 s = (F32)atof(sss);
                if (s >= 0.f && s < 0.1f)
                {
                    attribs["RK_RT_SUN_SSS"] = llformat("%.4f", s);             // metres: radius of the disc on the receiver where a pixel's rays start (skin scattering: a soft edge on both sides); 0 = off
                }
            }
            if (const char* blend = getenv("RK_VK_RT_SUN_BLENDCOVER"))
            {
                attribs["RK_RT_SUN_BLEND_COVER"] = (blend[0] && blend[0] != '0') ? "1" : "0";     // 0: a blended card covers by the masked ramp around the cutoff again (the look before)
            }
            if (const char* all = getenv("RK_VK_RT_SUN_STRENGTH_ALL"))
            {
                if (all[0] && all[0] != '0')
                {
                    attribs["RK_RT_SUN_STRENGTH_ALL"] = "1";
                }
            }
            if (const char* fill = getenv("RK_VK_RT_SUN_HAIRFILL"))
            {
                const F32 f = (F32)atof(fill);
                if (f > 0.f && f <= 1.f)
                {
                    attribs["RK_RT_SUN_HAIRFILL"] = llformat("%.3f", f);           // how much of the shadow on a blended surface (hair) is filled in
                }
            }
            if (const char* bias_alpha = getenv("RK_VK_RT_SUN_BIAS_ALPHA"))
            {
                const F32 b = (F32)atof(bias_alpha);
                if (b > 0.f && b < 5.f)
                {
                    attribs["RK_RT_SUN_BIAS_ALPHA"] = llformat("%.4f", b);
                }
            }
            if (const char* tmin = getenv("RK_VK_RT_SUN_TMIN"))
            {
                const F32 t = (F32)atof(tmin);
                if (t >= 0.f && t < 5.f)            // (0 switches the minimum distance off)
                {
                    attribs["RK_RT_SUN_TMIN"] = llformat("%.4f", t);
                }
            }
            if (const char* soft = getenv("RK_VK_RT_SUN_SOFTALPHA"))
            {
                const F32 d = (F32)atof(soft);
                if (d >= 0.f && d < 5000.f)
                {
                    attribs["RK_RT_SUN_SOFTALPHA"] = llformat("%.1f", d);      // metres (0 = off): up to this view distance the sun ray lets masked and blended cards through by their alpha
                }
            }
            if (const char* noface = getenv("RK_VK_RT_SUN_NOFACE"))
            {
                if (noface[0] && noface[0] != '0')
                {
                    attribs["RK_RT_SUN_NOFACE"] = "1";
                }
            }
            if (const char* cap = getenv("RK_VK_RT_CAPDEBUG"))
            {
                if (cap[0] && cap[0] != '0')
                {
                    attribs["RK_RT_CAP_DEBUG"] = "1";
                }
            }
        }

        if (shadow_detail >= 2)
        {
            attribs["SPOT_SHADOW"] = "1";
        }
    }

    if (ssr)
    {
        attribs["SSR"] = "1";
    }

    if (has_reflection_probes)
    {
        attribs["REFMAP_LEVEL"] = std::to_string(probe_level);
        attribs["REF_SAMPLE_COUNT"] = "32";
    }

    if (mirrors)
    {
        attribs["HERO_PROBES"] = "1";
    }

    { // PBR terrain
        const S32 mapping = clamp_terrain_mapping(gSavedSettings.getS32("RenderTerrainPBRPlanarSampleCount"));
        attribs["TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT"] = llformat("%d", mapping);
        const F32 triplanar_factor = gSavedSettings.getF32("RenderTerrainPBRTriplanarBlendFactor");
        attribs["TERRAIN_TRIPLANAR_BLEND_FACTOR"] = llformat("%.2f", triplanar_factor);
        S32 detail = gSavedSettings.getS32("RenderTerrainPBRDetail");
        detail = llclamp(detail, TERRAIN_PBR_DETAIL_MIN, TERRAIN_PBR_DETAIL_MAX);
        attribs["TERRAIN_PBR_DETAIL"] = llformat("%d", detail);
    }

    RKShaderProgram::sGlobalDefines = attribs;
    {   // <FS> the native shaders (rkvknative.h) are built with the defines the graphics settings decide, like the viewer's own shaders
        static const char* const native_keys[] = { "SSR", "REFMAP_LEVEL", "REF_SAMPLE_COUNT", "HERO_PROBES", "SUN_SHADOW", "HAS_EMISSIVE", "TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT", "TERRAIN_TRIPLANAR_BLEND_FACTOR", "TERRAIN_PBR_DETAIL" };
        std::vector<std::pair<std::string, std::string>> native_defines;
        for (const char* k : native_keys)
        {
            auto found = attribs.find(k);
            if (found != attribs.end())
            {
                native_defines.push_back({ k, found->second });
            }
        }
        if (LLPipeline::sRenderTransparentWater)
        {
            native_defines.push_back({ "TRANSPARENT_WATER", "1" });     // (the native water programs read the picture below the water only then: the stock ones get it as a permutation, loadShadersWater)
        }
        if (gSavedSettings.getBOOL("RenderDeferredSSAO"))
        {
            native_defines.push_back({ "HAS_SSAO", "1" });      // (the native lighting resolve reads the occlusion half of the lightmap only then. Not put in `attribs`: the stock shaders would take it for their own HAS_SSAO)
        }
        sRtSessionShaders = attribs.find("RK_RT_SHADOWS") != attribs.end();
        // the ray query code's own defines (RK_RT_SHADOWS: the mode of the traced sun shadow, 1 the rays, 4 and 5 the test views of lib/rt.glsl rt_sun_shadow; RK_RT_SUN_*, RK_RT_MAX_CANDIDATES, RK_RT_CAP_DEBUG: the tuning
        // and test switches of the environment, set only when the environment asks) and the spot shadow switch: what the stock programs' library files saw, the native ones see too
        for (const auto& a : attribs)
        {
            if (a.first.compare(0, 6, "RK_RT_") == 0 || a.first == "SPOT_SHADOW")
            {
                native_defines.push_back({ a.first, a.second });
            }
        }
        {
            // the sizes of the glTF scene programs' uniform blocks (make_gltf_variant: the nodes, the materials, the vec4s of a block) from the device, as the stock programs' permutations had them
            const U32 ubo_bytes = gDeviceCaps.mMaxUniformBlockSize;
            native_defines.push_back({ "MAX_NODES_PER_GLTF_OBJECT", std::to_string(ubo_bytes / (16 * 3)) });
            native_defines.push_back({ "MAX_MATERIALS_PER_GLTF_OBJECT", std::to_string(ubo_bytes / (16 * 12)) });
            native_defines.push_back({ "MAX_UBO_VEC4S", std::to_string(ubo_bytes / 16) });
        }
        if (getenv("RK_VK_RTREFL_DEBUG"))
        {
            native_defines.push_back({ "RT_REFL_DEBUG", "1" });        // the test views of the traced hit (rt-debug.txt hitq 3, 4, 6, 7 - 15, 17, 18, 22 - 24): five more call sites of the hit function, a longer compile
        }
        RKVkBridge::nativeRtSession(sRtSessionShaders);      // (the stock programs of this session carry ray query code: the `rt` lines of the manifest keep them)
        RKVkBridge::nativeGlobalDefines(native_defines);
    }

    return std::string();
}

// <FS> ray traced reflections (rtReflection in shadowUtil.glsl): the programs that use it see them as the permutation RK_RT_REFLECTIONS. The device is not up yet when the water is configured
// (RKVkBridge::rtShadowMode() still says 0), so the launcher's environment is the early answer: ray shadows on and the material table on.
static bool rt_reflections_possible()
{
    const char* rt_env = getenv("RK_VK_RT_SHADOWS");
    const char* rt_materials_env = getenv("RK_VK_RT_MATERIALS");
    return rt_env && rt_env[0] && rt_env[0] != '0' && rt_materials_env && rt_materials_env[0] == '1';
}

bool LLViewerShaderMgr::loadShadersWater()
{
    LL_PROFILE_ZONE_SCOPED;
    bool success = true;
    bool terrainWaterSuccess = true;

    bool use_sun_shadow = mShaderLevel[SHADER_DEFERRED] > 1 &&
        gSavedSettings.getS32("RenderShadowDetail") > 0;

    if (mShaderLevel[SHADER_WATER] == 0)
    {
        gWaterPullProgram.unload();
        gUnderWaterPullProgram.unload();
        gWaterPullRtProgram.unload();
        gWaterPullRtReflectionProgram.unload();
        return true;
    }

    // <FS> the water programs are native only (native/manifest.txt, family water: water.vert + water.frag / underwater.frag): no stock files, no stock program. The planes are drawn as one record of table
    // entries (WATER_PULL: their vertex buffers read by address); in a session with ray queries the water seen from above has the ray traced sun shadow and reflection (Water Pull RT Shader, RK_RT_QUERY).
    // The global defines (SUN_SHADOW, TRANSPARENT_WATER, ...) are pushed with the native defines
    if (success)
    {
        gWaterPullProgram.mName = "Water Pull Shader";
        gWaterPullProgram.mFeatures.calculatesAtmospherics = true;
        gWaterPullProgram.mFeatures.hasReflectionProbes = true;
        gWaterPullProgram.addPermutation("RK_VERTEX_PULL", "1");
        gWaterPullProgram.mShaderGroup = RKShaderProgram::SG_WATER;
        gWaterPullProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        success = gWaterPullProgram.createShader();
        llassert(success);
    }

    sWaterRtMade = false;
    if (success && sRtSessionShaders && use_sun_shadow)
    {
        gWaterPullRtProgram.mName = "Water Pull RT Shader";
        gWaterPullRtProgram.mFeatures = gWaterPullProgram.mFeatures;
        gWaterPullRtProgram.addPermutation("RK_VERTEX_PULL", "1");
        gWaterPullRtProgram.mShaderGroup = RKShaderProgram::SG_WATER;
        gWaterPullRtProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        success = gWaterPullRtProgram.createShader();
        llassert(success);
        sWaterRtMade = success;
    }

    // the variant with the traced reflection: only when the material table can be on (the reflection reads it); a program of its own, the water draws with it when the setting RKVkRTWater is on
    sWaterRtReflectionMade = false;
    if (success && sWaterRtMade && rt_reflections_possible())
    {
        gWaterPullRtReflectionProgram.mName = "Water Pull RT Reflection Shader";
        gWaterPullRtReflectionProgram.mFeatures = gWaterPullProgram.mFeatures;
        gWaterPullRtReflectionProgram.addPermutation("RK_VERTEX_PULL", "1");
        gWaterPullRtReflectionProgram.mShaderGroup = RKShaderProgram::SG_WATER;
        gWaterPullRtReflectionProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        success = gWaterPullRtReflectionProgram.createShader();
        llassert(success);
        sWaterRtReflectionMade = success;
    }

    if (success)
    {
        gUnderWaterPullProgram.mName = "Underwater Pull Shader";
        gUnderWaterPullProgram.mFeatures.calculatesAtmospherics = true;
        gUnderWaterPullProgram.addPermutation("RK_VERTEX_PULL", "1");
        gUnderWaterPullProgram.mShaderGroup = RKShaderProgram::SG_WATER;
        gUnderWaterPullProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        success = gUnderWaterPullProgram.createShader();
        llassert(success);
    }

    /// Keep track of water shader levels
    if (gWaterPullProgram.mShaderLevel != mShaderLevel[SHADER_WATER]
        || gUnderWaterPullProgram.mShaderLevel != mShaderLevel[SHADER_WATER])
    {
        mShaderLevel[SHADER_WATER] = llmin(gWaterPullProgram.mShaderLevel, gUnderWaterPullProgram.mShaderLevel);
    }

    if (!success)
    {
        mShaderLevel[SHADER_WATER] = 0;
        return false;
    }

    // if we failed to load the terrain water shaders and we need them (using class2 water),
    // then drop down to class1 water.
    if (mShaderLevel[SHADER_WATER] > 1 && !terrainWaterSuccess)
    {
        mShaderLevel[SHADER_WATER]--;
        return loadShadersWater();
    }

    LLWorld::getInstance()->updateWaterObjects();

    return true;
}

bool LLViewerShaderMgr::loadShadersEffects()
{
    LL_PROFILE_ZONE_SCOPED;
    bool success = true;

    if (mShaderLevel[SHADER_EFFECT] == 0)
    {
        gGlowProgram.unload();
        gGlowExtractProgram.unload();
        gPostVignetteProgram.unload();  // <FS:Ansariel> Import Vignette from Exodus
        gPostSnapshotFrameProgram.unload();  // <FS:Beq/> Add Snapshot framing shader
        return true;
    }

    if (success)
    {
        gGlowProgram.mName = "Glow Shader (Post)";       // <FS> native only (native/manifest.txt, family post): no stock files
        gGlowProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];
        success = gGlowProgram.createShader();
        if (!success)
        {
            LLPipeline::sRenderGlow = false;
        }
    }

    if (success)
    {
        const bool use_glow_noise = gSavedSettings.getBOOL("RenderGlowNoise");
        const std::string glow_noise_label = use_glow_noise ? " (+Noise)" : "";

        gGlowExtractProgram.mName = llformat("Glow Extract Shader (Post)%s", glow_noise_label.c_str());
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gGlowExtractProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];

        success = gGlowExtractProgram.createShader();
        if (!success)
        {
            LLPipeline::sRenderGlow = false;
        }
    }

// <FS:CR> Import Vignette from Exodus
    if (success)
    {
        gPostVignetteProgram.mName = "Vignette Post";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gPostVignetteProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];
        success = gPostVignetteProgram.createShader();
    }
// </FS:CR>
// <FS:Beq> Add Snapshot framing shader
    if (success)
    {
        gPostSnapshotFrameProgram.mName = "Snapshot Frame Post";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gPostSnapshotFrameProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];
        success = gPostSnapshotFrameProgram.createShader();
    }
// </FS:Beq>

    return success;

}

bool LLViewerShaderMgr::loadShadersDeferred()
{
    LL_PROFILE_ZONE_SCOPED;
    bool use_sun_shadow = mShaderLevel[SHADER_DEFERRED] > 1 &&
        gSavedSettings.getS32("RenderShadowDetail") > 0;

    bool success = true;

    if (success)
    {
        gDeferredHighlightProgram.mName = "Deferred Highlight Shader";
        // <FS> a native only program (native/manifest.txt, family post: highlight_face.vert + highlight_face.frag with HL_GBUFFER; the viewer's HAS_EMISSIVE reaches it as a global define): no stock files
        gDeferredHighlightProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gDeferredHighlightProgram.createShader();
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family alpha: mask_nocolor.vert + mask_nocolor.frag): no stock files (it was the pull twin of a stock program of this name, deleted)
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mName = "Deferred Diffuse Non-Indexed Alpha Mask No Color Shader pull";
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.createShader();
        llassert(success);
    }

    // <FS> the old Material Shader programs of the vertex buffer path (16 and their skinned variants, "Material Shader N"): gone; the passes draw the pull programs below

    if (gSavedSettings.getBOOL("GLTFEnabled"))
    {
        if (success)
        {
            gGLTFPBRMetallicRoughnessProgram.mName = "GLTF PBR Metallic Roughness Shader";

            // <FS> native only programs (native/manifest.txt, family gbuffer: gltf_scene.vert + gltf_scene.frag, one line for each of the 16 variants): no stock files
            gGLTFPBRMetallicRoughnessProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

            add_common_permutations(&gGLTFPBRMetallicRoughnessProgram);

            success = make_gltf_variants(gGLTFPBRMetallicRoughnessProgram, use_sun_shadow);

            //llassert(success);
            if (!success)
            {
                LL_WARNS() << "Failed to create GLTF PBR Metallic Roughness Shader, disabling!" << LL_ENDL;
                gSavedSettings.setBOOL("RenderCanUseGLTFPBROpaqueShaders", false);
                // continue as if this shader never happened
                success = true;
            }
        }
    }

    if (success)
    {
        RKShaderProgram* shader = &gDeferredPBRAlphaProgram;
        shader->mName = "Deferred PBR Alpha Shader";

        shader->mFeatures.calculatesLighting = false;
        shader->mFeatures.hasLighting = false;
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];

        // <FS> a native only program (native/manifest.txt, family alpha: alpha_pbr_preview.vert + alpha_pbr.frag): no stock files. The material preview floater draws with it (llgltfmaterialpreviewmgr.cpp)
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        llassert(success);

        // Alpha Shader Hack
        // See: LLRender::syncMatrices()
        shader->mFeatures.calculatesLighting = true;
        shader->mFeatures.hasLighting = true;
    }

    if (success)
    {
        S32 detail = gSavedSettings.getS32("RenderTerrainPBRDetail");
        detail = llclamp(detail, TERRAIN_PBR_DETAIL_MIN, TERRAIN_PBR_DETAIL_MAX);
        const S32 mapping = clamp_terrain_mapping(gSavedSettings.getS32("RenderTerrainPBRPlanarSampleCount"));
        for (U32 paint_type = 0; paint_type < TERRAIN_PAINT_TYPE_COUNT && success; ++paint_type)
        {
            // <FS> the PBR terrain pass from the geometry packet path (the patch's vertex buffer read by address): native only programs, one for each detail level, paint type and mapping (native/manifest.txt, family
            // gbuffer: pbrterrain.vert + pbrterrain.frag); required (PLAN-twin-removal.md, section 8)
            RKShaderProgram* shader = &gDeferredPBRTerrainPullProgram[paint_type];
            shader->mName = llformat("Deferred PBR Terrain Shader %d %s %s pull",
                    detail,
                    (paint_type == TERRAIN_PAINT_TYPE_PBR_PAINTMAP ? "paintmap" : "heightmap-with-noise"),
                    (mapping == 1 ? "flat" : "triplanar"));
            shader->mFeatures.calculatesAtmospherics = true;
            shader->addPermutation("RK_VERTEX_PULL", "1");       // (as the pull twin had it: a draw of a vertex buffer under it is one entry of a geometry record)
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (!success)
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
            }
            llassert(success);
        }
    }

    // <FS> the Linden tree pass from the geometry packet path, as the terrain's: the tree faces' buffers are read by address (LLDrawPoolTree::renderDeferred sends them as one record). A native only program (native/manifest.txt,
    // family gbuffer: tree.vert + tree.frag): no stock files; required (PLAN-twin-removal.md, section 8)
    if (success)
    {
        gDeferredTreePullProgram.mName = "Deferred Tree Pull Shader";
        gDeferredTreePullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredTreePullProgram.createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the Deferred Tree Pull Shader could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    if (success)
    {
        gDeferredImpostorProgram.mName = "Deferred Impostor Shader";
        // <FS> a native only program (native/manifest.txt, family gbuffer): no stock files
        gDeferredImpostorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gDeferredImpostorProgram.createShader();
        llassert(success);
    }

    // <FS> the local light passes (native/manifest.txt, family light): native only programs, no stock files. The point light volumes and the projectors are drawn from one record each (the table of their rows),
    // the lights that have the camera inside their box on the full screen triangle: one program for any number of point lights (it loops to light_count) and one for a projector.
    for (RKShaderProgram* shader : { &gDeferredLightPullProgram, &gDeferredSpotLightPullProgram, &gDeferredMultiLightProgram, &gDeferredMultiSpotLightProgram })
    {
        if (!success)
        {
            break;
        }
        shader->mName = shader == &gDeferredLightPullProgram ? "Deferred Light Pull Shader"
            : shader == &gDeferredSpotLightPullProgram ? "Deferred SpotLight Pull Shader"
            : shader == &gDeferredMultiLightProgram ? "Deferred MultiLight Shader" : "Deferred MultiSpotLight Shader";
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        llassert(success);
    }

    if (success)
    {
        bool use_ao = gSavedSettings.getBOOL("RenderDeferredSSAO");

        gDeferredSunProgram.mName = "Deferred Sun Shader";

        // <FS> a native only program (native/manifest.txt, family sun: sun_lightmap.frag; the ray traced sun lightmap is its rtquery variant, a session without occlusion its RK_NO_AO one): no stock files
        gDeferredSunProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredSunProgram);

        success = gDeferredSunProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredSunProbeProgram.mName = "Deferred Sun Probe Shader";

        // <FS> a native only program (native/manifest.txt, family sun: sun_lightmap.frag with RK_NO_AO): no stock files
        gDeferredSunProbeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gDeferredSunProbeProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredBlurLightProgram.mName = "Deferred Blur Light Shader";

        // <FS> a native only program (native/manifest.txt, family blur): no stock files
        gDeferredBlurLightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gDeferredBlurLightProgram.createShader();
        llassert(success);

        // <FS> native only programs (PLAN-shaders-rewrite.md, SHADERS-F1-lighting.md): the half resolution lightmap. A failure leaves them incomplete: the lighting pass then keeps the full resolution path.
        gNativeSunHalfProgram.mName = "Native Sun Lightmap Half";
        gNativeSunHalfProgram.mNativeVertex = "fullscreen.vert";
        gNativeSunHalfProgram.mNativeFragment = "sun_lightmap.frag";
        gNativeSunHalfProgram.mNativeDefines = { { "RK_HALF", "1" } };
        gNativeSunHalfProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gNativeSunHalfProgram.createShader();

        gNativeLightUpsampleProgram.mName = "Native Lightmap Upsample";
        gNativeLightUpsampleProgram.mNativeVertex = "fullscreen.vert";
        gNativeLightUpsampleProgram.mNativeFragment = "lightmap_upsample.frag";
        gNativeLightUpsampleProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gNativeLightUpsampleProgram.createShader();
    }

    // <FS> the old alpha programs of the vertex buffer path (Deferred Alpha, its skinned and HUD variants, the impostor pair) are gone: no code bound them; the alpha passes draw the pull programs below

    // <FS> S5a (doc/vulkan/S5-design.md): the lit and the fullbright alpha programs with the vertices read by address and the diffuse texture from the bindless table. Required (PLAN-twin-removal.md, section 8): a program
    // that cannot be made ends the shader load.
    if (success)
    {
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool fullbright = i == 1;
            RKShaderProgram* shader = fullbright ? &gDeferredFullbrightAlphaPullProgram : &gDeferredAlphaPullProgram;
            shader->mName = fullbright ? "Deferred Fullbright Alpha Pull Shader" : "Deferred Alpha Pull Shader";
            shader->mFeatures.calculatesAtmospherics = true;
            if (fullbright)
            {
                // <FS> a native only program (native/manifest.txt, family alpha: alpha.vert + alpha_fullbright.frag): no stock files (the lit program below keeps its own for the sessions with ray queries)
            }
            else
            {
                shader->mFeatures.calculatesLighting = false;
                shader->mFeatures.hasLighting = false;
                shader->mFeatures.hasReflectionProbes = true;
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (!success)
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
            }
            else if (!fullbright)
            {
                shader->mFeatures.calculatesLighting = true;        // (as the ordinary alpha program: the hack that follows its createShader)
                shader->mFeatures.hasLighting = true;
            }
            llassert(success);
        }
    }

    // <FS> S5c: the PBR (glTF) blend program with the vertices read by address (pbrAlphaPullV + pbralphaF with RK_BINDLESS_PBR). Required.
    if (success)
    {
        RKShaderProgram* shader = &gDeferredPBRAlphaPullProgram;
        shader->mName = "Deferred PBR Alpha Pull Shader";
        shader->mFeatures.calculatesLighting = false;
        shader->mFeatures.hasLighting = false;
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(shader);
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (success)
        {
            shader->mFeatures.calculatesLighting = true;        // (as the ordinary PBR alpha program: "Alpha Shader Hack", see LLRender::syncMatrices())
            shader->mFeatures.hasLighting = true;
        }
        else
        {
            LL_WARNS("ShadowPackets") << "the PBR alpha pull shader could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> S5d: the rigged variants of the alpha pull programs: the same vertex shaders with HAS_SKIN (weights and palette read by address) and indexed draws. Required.
    if (success)
    {
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool fullbright = i == 1;
            RKShaderProgram* shader = fullbright ? &gDeferredFullbrightAlphaPullSkinProgram : &gDeferredAlphaPullSkinProgram;
            shader->mName = fullbright ? "Deferred Fullbright Alpha Pull Skin Shader" : "Deferred Alpha Pull Skin Shader";
            shader->mFeatures.calculatesAtmospherics = true;
            if (fullbright)
            {
                // <FS> a native only program (native/manifest.txt, family alpha: alpha.vert + alpha_fullbright.frag): no stock files (the lit program below keeps its own for the sessions with ray queries)
            }
            else
            {
                shader->mFeatures.calculatesLighting = false;
                shader->mFeatures.hasLighting = false;
                shader->mFeatures.hasReflectionProbes = true;
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (!success)
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
            }
            else if (!fullbright)
            {
                shader->mFeatures.calculatesLighting = true;
                shader->mFeatures.hasLighting = true;
            }
            llassert(success);
        }
        // ... and the PBR one
        if (success)
        {
            RKShaderProgram* shader = &gDeferredPBRAlphaPullSkinProgram;
            shader->mName = "Deferred PBR Alpha Pull Skin Shader";
            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];
            add_common_permutations(shader);
            if (use_sun_shadow)
            {
            }
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (success)
            {
                shader->mFeatures.calculatesLighting = true;
                shader->mFeatures.hasLighting = true;
            }
            else
            {
                LL_WARNS("ShadowPackets") << "the PBR alpha pull skin shader could not be made" << LL_ENDL;
            }
            llassert(success);
        }
    }

    // <FS> item D1 (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the lit alpha pull program with ALPHA_UBER, which also draws the fullbright alpha draws (the row's GEO_FULLBRIGHT flag), so that the runs of a lit
    // and a fullbright draw in the blend order are one run. Static and rigged. Required: the colour pass of the world draws the fullbright alpha draws with it.
    if (success)
    {
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool skin = i == 1;
            RKShaderProgram* shader = skin ? &gDeferredAlphaUberPullSkinProgram : &gDeferredAlphaUberPullProgram;
            shader->mName = skin ? "Deferred Alpha Uber Pull Skin Shader" : "Deferred Alpha Uber Pull Shader";
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;
            shader->mFeatures.hasReflectionProbes = true;
            if (skin)
            {
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (success)
            {
                shader->mFeatures.calculatesLighting = true;        // (as the lit program: the hack that follows its createShader)
                shader->mFeatures.hasLighting = true;
            }
            else
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
            }
            llassert(success);
        }
    }

    // <FS> item D2 (doc/vulkan/PLAN-crowd-worker-and-gpu.md): the same merged program with the material draws too (ALPHA_UBER_MAT: the row's GEO_MATERIAL flags, the blend path of materialF.glsl). Optional: without it
    // the material runs are drawn by it too. Required.
    if (success)
    {
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool skin = i == 1;
            RKShaderProgram* shader = skin ? &gDeferredAlphaUberMatPullSkinProgram : &gDeferredAlphaUberMatPullProgram;
            shader->mName = skin ? "Deferred Alpha Uber Material Pull Skin Shader" : "Deferred Alpha Uber Material Pull Shader";
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;
            shader->mFeatures.hasReflectionProbes = true;
            if (skin)
            {
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            success = shader->createShader();
            if (success)
            {
                shader->mFeatures.calculatesLighting = true;
                shader->mFeatures.hasLighting = true;
            }
            else
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
            }
            llassert(success);
        }
    }

    // <FS> S6 X1: the opaque fullbright pass from the table: the same vertex shader as the fullbright alpha pull program (rows and matrices as the G-buffer table's) with the plain fullbright fragment shader. Required
    // (PLAN-twin-removal.md, section 8), static and rigged.
    for (int i = 0; i < 2 && success; ++i)
    {
        const bool skin = i == 1;
        RKShaderProgram* shader = skin ? &gDeferredFullbrightPullSkinProgram : &gDeferredFullbrightPullProgram;
        shader->mName = skin ? "Deferred Fullbright Pull Skin Shader" : "Deferred Fullbright Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        // <FS> a native only program (native/manifest.txt): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K6 S1: the HUD's fullbright pass from the table: the fullbright pull program of the world with IS_HUD (no atmospherics, as the HUD fullbright shader). Required.
    if (success)
    {
        RKShaderProgram* shader = &gHUDFullbrightPullProgram;
        shader->mName = "HUD Fullbright Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        // <FS> a native only program (native/manifest.txt): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K6 S1: the HUD's fullbright shiny pass from the table (the shiny pull program with IS_HUD: no environment lighting, as the HUD shiny shader). Required (PLAN-twin-removal.md, section 8): a program
    // that cannot be made ends the shader load.
    if (success)
    {
        RKShaderProgram* shader = &gHUDFullbrightShinyPullProgram;
        shader->mName = "HUD Fullbright Shiny Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = true;
        // <FS> a native only program (native/manifest.txt): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K4: the fullbright shiny pass from the table: the fullbright pull vertex shader with the eye space normal (IS_SHINY_PULL) and the shiny fragment shader (environment map or reflection probes, bound by the
    // pool to whichever program draws), the diffuse texture of each row from the bindless table. Required, static and rigged.
    for (int i = 0; i < 2 && success; ++i)
    {
        const bool skin = i == 1;
        RKShaderProgram* shader = skin ? &gDeferredFullbrightShinyPullSkinProgram : &gDeferredFullbrightShinyPullProgram;
        shader->mName = skin ? "Deferred Fullbright Shiny Pull Skin Shader" : "Deferred Fullbright Shiny Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = true;
        // <FS> a native only program (native/manifest.txt): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K4: the deferred bump pass from the table: the pull vertex shader with the tangent frame (IS_BUMP_PULL) and bumpF with its diffuse and bump images from the bindless table by the row's slots
    // (the materials rewrite, RK_BINDLESS_MATERIAL), the cutoff of each draw from its row (RK_MASK_CUTOFF). Required, static and rigged.
    for (int i = 0; i < 2 && success; ++i)
    {
        const bool skin = i == 1;
        RKShaderProgram* shader = skin ? &gDeferredBumpPullSkinProgram : &gDeferredBumpPullProgram;
        shader->mName = skin ? "Deferred Bump Pull Skin Shader" : "Deferred Bump Pull Shader";
        // <FS> a native only program (native/manifest.txt, family gbuffer: gbuffer.vert + gbuffer_bump.frag): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K4: the post deferred emboss pass from the table: the pull vertex shader with the second texture coordinate set (IS_EMBOSS_PULL) and the object bump fragment shader, its two samplers both reading the
    // bump image's slot of the row (the materials rewrite). Required, static and rigged.
    for (int i = 0; i < 2 && success; ++i)
    {
        const bool skin = i == 1;
        RKShaderProgram* shader = skin ? &gDeferredEmbossPullSkinProgram : &gDeferredEmbossPullProgram;
        shader->mName = skin ? "Deferred Emboss Pull Skin Shader" : "Deferred Emboss Pull Shader";
        // <FS> a native only program (native/manifest.txt, family gbuffer: gbuffer.vert + emboss.frag): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> S6 X3: the alpha masked passes from the table: the diffuse mask G-buffer pass (diffusePullV + diffuseAlphaMaskIndexedF) and the fullbright mask pass (alphaPullV + fullbrightF with HAS_ALPHA_MASK), the cutoff
    // of each draw read from its row (RK_MASK_CUTOFF: a flat varying instead of the uniform). Required (PLAN-twin-removal.md, section 8).
    if (success)
    {
        // <FS> all of these are native only programs (native/manifest.txt, families gbuffer and post: the diffuse and the fullbright alpha mask pull programs, static, rigged and the HUD's): no stock files
        auto make_mask = [&](RKShaderProgram* shader, const char* name, bool fullbright) -> bool
        {
            shader->mName = name;
            if (fullbright)
            {
                shader->mFeatures.calculatesAtmospherics = true;
            }
            else
            {
            }
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!shader->createShader())
            {
                LL_WARNS("ShadowPackets") << "the " << name << " could not be made" << LL_ENDL;
                return false;
            }
            return true;
        };
        success = make_mask(&gDeferredDiffuseAlphaMaskPullProgram, "Deferred Diffuse Alpha Mask Pull Shader", false)
            && make_mask(&gDeferredFullbrightAlphaMaskPullProgram, "Deferred Fullbright Alpha Mask Pull Shader", true)
            && make_mask(&gHUDFullbrightMaskPullProgram, "HUD Fullbright Alpha Mask Pull Shader", true)
            && make_mask(&gDeferredDiffuseAlphaMaskPullSkinProgram, "Deferred Diffuse Alpha Mask Pull Skin Shader", false)
            && make_mask(&gDeferredFullbrightAlphaMaskPullSkinProgram, "Deferred Fullbright Alpha Mask Pull Skin Shader", true);
        llassert(success);
    }

    // <FS> S6 X4: the glTF opaque and mask G-buffer pass from the table: the PBR alpha pull vertex shader (the same rows) with pbropaqueF rewritten like pbralphaF (RK_BINDLESS_PBR). Required, static and rigged.
    for (int i = 0; i < 2 && success; ++i)
    {
        const bool skin = i == 1;
        RKShaderProgram* shader = skin ? &gDeferredPBROpaquePullSkinProgram : &gDeferredPBROpaquePullProgram;
        shader->mName = skin ? "Deferred PBR Opaque Pull Skin Shader" : "Deferred PBR Opaque Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];
        // <FS> a native only program (native/manifest.txt, family gbuffer: alpha_pbr.vert + pbr_opaque.frag): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> J1 K6 S1: the HUD's glTF opaque and mask pass from the table: the PBR opaque pull program with IS_HUD (pbropaqueF has no lighting outputs in a HUD). Required.
    if (success)
    {
        RKShaderProgram* shader = &gHUDPBROpaquePullProgram;
        shader->mName = "HUD PBR Opaque Pull Shader";
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];
        // <FS> a native only program (native/manifest.txt, family gbuffer: alpha_pbr.vert + pbr_opaque_hud.frag): no stock files
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> S5e: the glow second draw of the alpha items from the table: emissiveF / pbrglowF (alpha only) with the vertices read by address, static and rigged. Required.
    if (success)
    {
        auto make_glow = [&](RKShaderProgram* shader, const char* name, const char* bindless, bool skin, bool srgb) -> bool
        {
            shader->mName = name;
            shader->addPermutation(bindless, "1");
            if (skin)
            {
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!shader->createShader())
            {
                LL_WARNS("ShadowPackets") << "the " << name << " could not be made" << LL_ENDL;
                return false;
            }
            return true;
        };
        success = make_glow(&gDeferredEmissivePullProgram, "Deferred Emissive Pull Shader", "RK_BINDLESS_DIFFUSE", false, false)
            && make_glow(&gDeferredEmissivePullSkinProgram, "Deferred Emissive Pull Skin Shader", "RK_BINDLESS_DIFFUSE", true, false)
            && make_glow(&gPBRGlowPullProgram, "PBR Glow Pull Shader", "RK_BINDLESS_PBR", false, true)
            && make_glow(&gPBRGlowPullSkinProgram, "PBR Glow Pull Skin Shader", "RK_BINDLESS_PBR", true, true);
        llassert(success);
    }

    // <FS> S5f F4: the pull variants of the programs the HUD, impostor and depth only alpha passes use (their fragment shaders are the old programs' own: IS_HUD, FOR_IMPOSTOR, the mask program without IS_ALPHA).
    // Required (PLAN-twin-removal.md, section 8).
    if (success)
    {
        auto make_pull = [&](RKShaderProgram* shader, const char* name, std::initializer_list<std::pair<const char*, const char*> > permutations, bool skin, bool sun_shadow = false) -> bool
        {
            shader->mName = name;
            for (const auto& p : permutations)
            {
                shader->addPermutation(p.first, p.second);
            }
            if (skin)
            {
            }
            if (sun_shadow)
            {
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!shader->createShader())
            {
                LL_WARNS("ShadowPackets") << "the " << name << " could not be made" << LL_ENDL;
                return false;
            }
            return true;
        };
        // the HUD's fullbright alpha program (all the HUD's plain draws: its lit program is never used) and its glTF program
        {
            RKShaderProgram* shader = &gHUDFullbrightAlphaPullProgram;
            shader->mFeatures.calculatesAtmospherics = true;
            success = make_pull(shader, "HUD Fullbright Alpha Pull Shader", {}, false);
            if (success)
            {
                shader = &gHUDPBRAlphaPullProgram;
                success = make_pull(shader, "HUD PBR Alpha Pull Shader", {}, false);       // (a native only program: alpha_pbr.vert + alpha_pbr_hud.frag)
            }
        }
        // the impostor render's lit program, static and rigged
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool rigged = i == 1;
            RKShaderProgram* shader = rigged ? &gDeferredAlphaImpostorPullSkinProgram : &gDeferredAlphaImpostorPullProgram;
            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;
            shader->mFeatures.hasReflectionProbes = true;
            // <FS> a native only program (native/manifest.txt, family alpha): no stock files
            success = make_pull(shader, rigged ? "Skinned Deferred Alpha Impostor Pull Shader" : "Deferred Alpha Impostor Pull Shader", {}, rigged);
            if (success)
            {
                shader->mFeatures.calculatesLighting = true;        // (as the ordinary impostor alpha program: the hack that follows its createShader)
                shader->mFeatures.hasLighting = true;
            }
        }
        // the mask programs: the impostor render's fullbright draws and the depth only passes. <FS> native only programs (native/manifest.txt, family post: fullbright_mask.frag with the cutoff uniform); the
        // static one has its own name (the G-buffer pass's program of the table has "Deferred Fullbright Alpha Mask Pull Shader")
        for (int i = 0; i < 2 && success; ++i)
        {
            const bool rigged = i == 1;
            RKShaderProgram* shader = rigged ? &gDeferredFullbrightMaskPullSkinProgram : &gDeferredFullbrightMaskPullProgram;
            shader->mFeatures.calculatesAtmospherics = true;
            success = make_pull(shader, rigged ? "Skinned Deferred Fullbright Alpha Mask Pull Shader" : "Deferred Fullbright Mask Alpha Pull Shader", {}, rigged);
        }
        llassert(success);
    }

    if (success)
    {
        gDeferredSoftenProgram.mName = "Deferred Soften Shader";
        gDeferredSoftenProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredSoftenProgram.mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED] > 2;

        // <FS> a native only program (native/manifest.txt, family resolve): no stock files. The sun shadow and the ambient occlusion reach it as the native global defines SUN_SHADOW and HAS_SSAO.
        gDeferredSoftenProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gDeferredSoftenProgram.createShader();
        llassert(success);
    }

    if (success && use_sun_shadow && rt_reflections_possible())
    {
        // <FS> the traced glossy reflection (rtReflectionF.glsl): the code the lighting pass used to hold. The same libraries as the lighting pass, its own program and pipeline: the lighting pass
        // compiles without the ray queries, this one is built in the background (doc/vulkan/PLAN-rt-reflection-pass.md)
        gDeferredRtReflectionProgram.mName = "Deferred RT Reflection Shader";
        gDeferredRtReflectionProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredRtReflectionProgram.mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED] > 2;

        add_common_permutations(&gDeferredRtReflectionProgram);
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredRtReflectionProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredRtReflectionProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHazeProgram.mName = "Haze Shader";
        gHazeProgram.mFeatures.calculatesAtmospherics = true;
        gHazeProgram.mFeatures.hasReflectionProbes    = mShaderLevel[SHADER_DEFERRED] > 2;

        // <FS> a native only program (native/manifest.txt, family post: haze.frag): no stock files
        gHazeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gHazeProgram.createShader();
        llassert(success);
    }


    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family post: waterhaze.vert + waterhaze.frag): no stock files. The water patches above the water and the full screen triangle under it are entries of
        // a record (the vertex buffers read by address)
        gWaterHazePullProgram.mName = "Water Haze Pull Shader";
        gWaterHazePullProgram.mShaderGroup           = RKShaderProgram::SG_WATER;
        gWaterHazePullProgram.mFeatures.calculatesAtmospherics = true;
        gWaterHazePullProgram.mFeatures.hasReflectionProbes    = mShaderLevel[SHADER_DEFERRED] > 2;
        gWaterHazePullProgram.addPermutation("RK_VERTEX_PULL", "1");
        gWaterHazePullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gWaterHazePullProgram.createShader();
        llassert(success);
    }


    if (success)
    {
        gDeferredShadowCubeProgram.mName = "Deferred Shadow Cube Shader";
        // <FS> a native only program (native/manifest.txt, family shadow): no stock files
        gDeferredShadowCubeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredShadowCubeProgram.createShader();
        llassert(success);
    }

    // <FS> Render packets: the shadow program that reads vertices and indices by address. Required (there is no other shadow path): a failure to make it is a failure to load the shaders.
    if (success && RKVkBridge::shadowPacketsAvailable())
    {
        gDeferredShadowPullProgram.mName = "Deferred Shadow Pull Shader";
        // <FS> a native only program (native/manifest.txt, family shadow): no stock files
        gDeferredShadowPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredShadowPullProgram.createShader();
        llassert(success);
    }

    // <FS> ... and the one for the alpha masked casters, which needs the device to index sampled images at run time (a required device feature). Also required.
    if (success && RKVkBridge::shadowAlphaPacketsAvailable())
    {
        gDeferredShadowAlphaPullProgram.mName = "Deferred Shadow Alpha Pull Shader";
        // <FS> a native only program (native/manifest.txt, family shadow): no stock files
        gDeferredShadowAlphaPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredShadowAlphaPullProgram.createShader();
        llassert(success);
    }

    // <FS> ... and the one for rigged casters
    if (success && RKVkBridge::shadowPacketsAvailable())
    {
        gDeferredShadowSkinPullProgram.mName = "Deferred Shadow Skin Pull Shader";
        // <FS> a native only program (native/manifest.txt, family shadow): no stock files
        gDeferredShadowSkinPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredShadowSkinPullProgram.createShader();
        llassert(success);
    }

    // <FS> The deferred diffuse pass from render packets (same fragment shader as the old non-pull diffuse program (deleted), the diffuse lookup from the bindless table). Required.
    if (success)
    {
        gDeferredDiffusePullProgram.mName = "Deferred Diffuse Pull Shader";
        // <FS> a native only program (native/manifest.txt, family gbuffer): no stock files
        gDeferredDiffusePullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredDiffusePullProgram);
        success = gDeferredDiffusePullProgram.createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the deferred diffuse pull shader could not be made" << LL_ENDL;
        }
        llassert(success);
        // <FS> S4 A1: ... and its rigged variant (HAS_SKIN: the draw's weights and palette are read by address). Required.
        gDeferredDiffusePullSkinProgram.mName = "Deferred Diffuse Pull Skin Shader";
        // <FS> a native only program (native/manifest.txt, family gbuffer): no stock files
        gDeferredDiffusePullSkinProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (getenv("RK_VK_SKIN_TEST"))
        {
        }
        add_common_permutations(&gDeferredDiffusePullSkinProgram);
        success = success && gDeferredDiffusePullSkinProgram.createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the rigged deferred diffuse pull shader could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    // <FS> ... and the materials passes: the unskinned programs of the 12 deferred passes (alpha mode in bits 0-1, specular map bit 2, normal map bit 3). Required too.
    if (success)
    {
        // (S5b: + 1, 5, 9, 13, the blend variants the alpha pass draws materials with; no rigged variant of those yet)
        static const U32 pull_indices[] = { 0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15, 1, 5, 9, 13 };
        for (U32 i : pull_indices)
        {
            RKShaderProgram& pull = gDeferredMaterialPullProgram[i];
            pull.mName = llformat("Material Pull Shader %d", i);
            // <FS> the programs of the alpha modes 0, 2 and 3 are native only (native/manifest.txt, family gbuffer: gbuffer.vert + gbuffer_material.frag): no stock files; the blend variants (1, 5, 9, 13: native
            // outside the sessions with ray queries) keep theirs
            const U32 alpha_mode = i & 0x3;
            pull.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (alpha_mode != 0)
            {
            }
            if (alpha_mode == 1)
            {
                if (i & 0x8)
                {
                }
                if (i & 0x4)
                {
                }
                if (use_sun_shadow)
                {
                }
                add_common_permutations(&pull);
            }
            pull.mFeatures.calculatesAtmospherics = true;
            pull.mFeatures.hasReflectionProbes = true;
            if (!pull.createShader())
            {
                LL_WARNS("ShadowPackets") << "a materials pull shader could not be made (index " << i << ")" << LL_ENDL;
                success = false;
                llassert(success);
                break;
            }
            else if ((i & 3) == 1)
            {
                pull.mFeatures.hasLighting = true;       // (as the ordinary blend variants, see gDeferredMaterialProgram[1])
            }

            // <FS> S4 A1: the rigged variant of the same pass (HAS_SKIN)
            RKShaderProgram& skin = gDeferredMaterialPullSkinProgram[i];
            skin.mName = llformat("Material Pull Skin Shader %d", i);
            skin.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (alpha_mode != 0)
            {
            }
            if (alpha_mode == 1)
            {
                if (i & 0x8)
                {
                }
                if (i & 0x4)
                {
                }
                if (use_sun_shadow)
                {
                }
                if (getenv("RK_VK_SKIN_TEST"))
                {
                }
                add_common_permutations(&skin);
            }
            skin.mFeatures.calculatesAtmospherics = true;
            skin.mFeatures.hasReflectionProbes = true;
            if (!skin.createShader())
            {
                LL_WARNS("ShadowPackets") << "a rigged materials pull shader could not be made (index " << i << ")" << LL_ENDL;
                success = false;
                llassert(success);
                break;
            }
            else if ((i & 3) == 1)
            {
                skin.mFeatures.hasLighting = true;       // (S5d: the rigged blend variants, as the ordinary ones)
            }
        }
    }


    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family shadow: avatar_shadow.vert + avatar_shadow.frag): the system avatar's parts read by address; no stock files (it was the pull twin of a stock program of this name, deleted)
        gDeferredAvatarShadowProgram.mName = "Deferred Avatar Shadow Shader pull";
        gDeferredAvatarShadowProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDeferredAvatarShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredAvatarAlphaShadowProgram.mName = "Deferred Avatar Alpha Shadow Shader pull";       // <FS> a native only program (family shadow), as above
        gDeferredAvatarAlphaShadowProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDeferredAvatarAlphaShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarAlphaShadowProgram.createShader();
        llassert(success);
    }
    if (success)
    {
        gDeferredAvatarAlphaMaskShadowProgram.mName = "Deferred Avatar Alpha Mask Shadow Shader pull";       // <FS> a native only program (family shadow), as above
        gDeferredAvatarAlphaMaskShadowProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDeferredAvatarAlphaMaskShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarAlphaMaskShadowProgram.createShader();
        llassert(success);
    }

    // <FS> S6.6a: the terrain pass from the geometry packet path: the patch's vertex buffer read by address. A native only program (native/manifest.txt, family gbuffer: terrain.vert + terrain.frag): no stock files;
    // required (PLAN-twin-removal.md, section 8)
    if (success)
    {
        gDeferredTerrainPullProgram.mName = "Deferred Terrain Pull Shader";
        gDeferredTerrainPullProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredTerrainPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredTerrainPullProgram.createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the Deferred Terrain Pull Shader could not be made" << LL_ENDL;
        }
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: avatar.vert + avatar.frag): the system avatar's parts read by address; no stock files (it was the pull twin of a stock program of this name, deleted)
        gDeferredAvatarProgram.mName = "Deferred Avatar Shader pull";
        gDeferredAvatarProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDeferredAvatarProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gDeferredAvatarProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> the avatar pool's blended parts: the pull program itself (it was the stock program of this name and its pull twin: the twin machinery is gone, PLAN-twin-removal.md, section 9). Native only without ray
        // traced shadows (native/manifest.txt, `rt`: avatar.vert + avatar_alpha.frag), the stock files are for the sessions with ray queries
        gDeferredAvatarAlphaProgram.mName = "Deferred Avatar Alpha Shader pull";
        gDeferredAvatarAlphaProgram.mFeatures.calculatesLighting = false;
        gDeferredAvatarAlphaProgram.mFeatures.hasLighting = false;
        gDeferredAvatarAlphaProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasReflectionProbes = true;


        gDeferredAvatarAlphaProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        if (use_sun_shadow)
        {
        }

        gDeferredAvatarAlphaProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredAvatarAlphaProgram);

        success = gDeferredAvatarAlphaProgram.createShader();
        llassert(success);

        gDeferredAvatarAlphaProgram.mFeatures.calculatesLighting = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasLighting = true;
    }

    if (success)
    {
        gExposureProgram.mName = "Exposure";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gExposureProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gExposureProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gExposureProgramNoFade.mName = "Exposure (no fade)";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gExposureProgramNoFade.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gExposureProgramNoFade.createShader();
        llassert(success);
    }

    if (success)
    {
        gLuminanceProgram.mName = "Luminance";       // <FS> native only (native/manifest.txt, family post): no stock files
        gLuminanceProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gLuminanceProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gMotionVectorProgram.mName = "Motion Vectors";       // <FS> native only (native/manifest.txt, family post): no stock files
        gMotionVectorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gMotionVectorProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gWorldDepthSnapProgram.mName = "World Depth Snapshot";       // <FS> native only (native/manifest.txt, family post)
        gWorldDepthSnapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gWorldDepthSnapProgram.createShader();
        llassert(success);
    }

    // <FS> (the avatar velocity programs were here until 2026-11-15: the G-buffer programs write the motion now) a failure here must not stop the programs that follow
    if (success)
    {
        // <FS> the penumbra filter of the ray traced sun shadow (PLAN-rt-shadow-penumbra.md step 2b): two separable passes over the lightmap's sun channel, sized by the blocker distance map
        gRtShadowFilterProgram.mName = "RT Shadow Penumbra Filter";
        // <FS> a native only program (native/manifest.txt, family sun: rt_shadow_filter.frag): no stock files
        gRtShadowFilterProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gRtShadowFilterProgram.createShader())
        {
            LL_WARNS("VkRt") << "the RT shadow penumbra filter did not compile: the centre ray's shadow stays hard" << LL_ENDL;
        }
        gRtShadowPrepProgram.mName = "RT Shadow Penumbra Prep";
        // <FS> a native only program (native/manifest.txt, family sun: rt_shadow_prep.frag): no stock files
        gRtShadowPrepProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gRtShadowPrepProgram.createShader())
        {
            LL_WARNS("VkRt") << "the RT shadow penumbra prep pass did not compile: the centre ray's shadow stays hard" << LL_ENDL;
        }

        // <FS> the AMD FidelityFX reflection denoiser's helper passes (PLAN-amd-denoiser.md): native only programs (native/manifest.txt, family post)
        gDnsrPrepProgram.mName = "Denoise Prep";
        gDnsrPrepProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDnsrPrepProgram);
        if (!gDnsrPrepProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the denoiser prep program did not compile: no AMD reflection denoiser" << LL_ENDL;
        }
        gDnsrPackProgram.mName = "Denoise Pack";
        gDnsrPackProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDnsrPackProgram);
        if (!gDnsrPackProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the denoiser pack program did not compile: no AMD reflection denoiser" << LL_ENDL;
        }
        gDnsrSunPackProgram.mName = "Denoise Sun Pack";
        gDnsrSunPackProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDnsrSunPackProgram);
        if (!gDnsrSunPackProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the denoiser sun pack program did not compile: no AMD shadow denoiser" << LL_ENDL;
        }
        gDnsrSunUnpackProgram.mName = "Denoise Sun Unpack";
        gDnsrSunUnpackProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDnsrSunUnpackProgram);
        if (!gDnsrSunUnpackProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the denoiser sun unpack program did not compile: no AMD shadow denoiser" << LL_ENDL;
        }
        gDnsrUnpackProgram.mName = "Denoise Unpack";
        gDnsrUnpackProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDnsrUnpackProgram);
        if (!gDnsrUnpackProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the denoiser unpack program did not compile: no AMD reflection denoiser" << LL_ENDL;
        }

        gNRRimLiftProgram.mName = "DLSS-NR Rim Lift";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gNRRimLiftProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gNRRimLiftProgram.createShader())
        {
            LL_WARNS("VkDlssNr") << "the DLSS-NR rim lift program did not compile: no rim lift" << LL_ENDL;
        }

        gAvatarVelocityDebugProgram.mName = "Avatar Velocity Debug";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gAvatarVelocityDebugProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gAvatarVelocityDebugProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the avatar velocity debug program did not compile" << LL_ENDL;
        }

        // <FS> made whenever the ray traced shadows are on: its mode 17 (the blocker distance, PLAN-rt-shadow-penumbra.md) needs no material table; the material modes 1-16 do, and
        // visualizeRtMaterials keeps them out of reach when the table is not there (their table reads would go through a zero address)
        if (RKVkBridge::rtShadowMode())
        {
            gRtMaterialDebugProgram.mName = "Ray Scene Material Debug";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
            gRtMaterialDebugProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            add_common_permutations(&gRtMaterialDebugProgram);
            if (!gRtMaterialDebugProgram.createShader())
            {
                LL_WARNS("VkRtMaterials") << "the ray scene material debug program did not compile" << LL_ENDL;
            }
        }
    }

    if (success)
    {
        gDeferredPostGammaCorrectProgram.mName = "Deferred Gamma Correction Post Process";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredPostGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gLegacyPostGammaCorrectProgram.mName = "Legacy Gamma Correction Post Process";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gLegacyPostGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gLegacyPostGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapProgram.mName = "Deferred Tonemap Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gDeferredPostTonemapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapProgram.mName = "No Post Tonemap Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gNoPostTonemapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gNoPostTonemapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapGammaCorrectProgram.mName = "Deferred Tonemap Gamma Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gDeferredPostTonemapGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapGammaCorrectProgram.mName = "No Post Tonemap Gamma Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gNoPostTonemapGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gNoPostTonemapGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapLegacyGammaCorrectProgram.mName = "Deferred Tonemap Legacy Gamma Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gDeferredPostTonemapLegacyGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapLegacyGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapLegacyGammaCorrectProgram.mName = "No Post Tonemap Legacy Gamma Post Process";       // <FS> native only (native/manifest.txt, family post): no stock files
        gNoPostTonemapLegacyGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gNoPostTonemapLegacyGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success && gDeviceCaps.mApiVersionNumber > 3.9f)
    {
        std::vector<std::pair<std::string, std::string>> quality_levels = { {"12", "Low"},
                                                                             {"23", "Medium"},
                                                                             {"28", "High"},
                                                                             {"39", "Ultra"} };
        int i = 0;
        bool failed = false;
        for (const auto& quality_pair : quality_levels)
        {
            if (success)
            {
                gFXAAProgram[i].mName = llformat("FXAA Shader (%s)", quality_pair.second.c_str());
                // <FS> native only programs (native/manifest.txt, family post: fxaa.vert + fxaa.frag, FXAA_PRESET = the quality preset's number: 12, 23, 28, 39): no stock files
                gFXAAProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];
                success = gFXAAProgram[i].createShader();
                // llassert(success);
                if (!success)
                {
                    LL_WARNS() << "Failed to create shader '" << gFXAAProgram[i].mName << "', disabling!" << LL_ENDL;
                    // continue as if this shader never happened
                    failed = true;
                    success = true;
                    break;
                }
            }
            ++i;
        }

        if (failed)
        {
            for (auto i = 0; i < 4; ++i)
            {
                gFXAAProgram[i].unload();
            }
        }
    }

    if (gDeviceCaps.mApiVersionNumber > 3.15f && success)
    {
        std::vector<std::pair<std::string, std::string>> quality_levels = { {"SMAA_PRESET_LOW", "Low"},
                                                                             {"SMAA_PRESET_MEDIUM", "Medium"},
                                                                             {"SMAA_PRESET_HIGH", "High"},
                                                                          {"SMAA_PRESET_ULTRA", "Ultra"} };
        int i = 0;
        bool failed = false;
        for (const auto& smaa_pair : quality_levels)
        {
            std::map<std::string, std::string> defines;
            if (gDeviceCaps.mApiVersionNumber >= 4.f)
                defines.emplace("SMAA_GLSL_4", "1");
            else if (gDeviceCaps.mApiVersionNumber >= 3.1f)
                defines.emplace("SMAA_GLSL_3", "1");
            else
                defines.emplace("SMAA_GLSL_2", "1");
            defines.emplace("SMAA_PREDICATION", "0");
            defines.emplace("SMAA_REPROJECTION", "0");
            defines.emplace(smaa_pair.first, "1");

            if (success)
            {
                gSMAAEdgeDetectProgram[i].mName = llformat("SMAA Edge Detection (%s)", smaa_pair.second.c_str());


                gSMAAEdgeDetectProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];
                success = gSMAAEdgeDetectProgram[i].createShader();
                // llassert(success);
                if (!success)
                {
                    LL_WARNS() << "Failed to create shader '" << gSMAAEdgeDetectProgram[i].mName << "', disabling!" << LL_ENDL;
                    // continue as if this shader never happened
                    failed = true;
                    success = true;
                    break;
                }
            }

            if (success)
            {
                gSMAABlendWeightsProgram[i].mName = llformat("SMAA Blending Weights (%s)", smaa_pair.second.c_str());


                gSMAABlendWeightsProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];
                success = gSMAABlendWeightsProgram[i].createShader();
                // llassert(success);
                if (!success)
                {
                    LL_WARNS() << "Failed to create shader '" << gSMAABlendWeightsProgram[i].mName << "', disabling!" << LL_ENDL;
                    // continue as if this shader never happened
                    failed = true;
                    success = true;
                    break;
                }
            }

            if (success)
            {
                gSMAANeighborhoodBlendProgram[i].mName = llformat("SMAA Neighborhood Blending (%s)", smaa_pair.second.c_str());


                gSMAANeighborhoodBlendProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];
                success = gSMAANeighborhoodBlendProgram[i].createShader();
                // llassert(success);
                if (!success)
                {
                    LL_WARNS() << "Failed to create shader '" << gSMAANeighborhoodBlendProgram[i].mName << "', disabling!" << LL_ENDL;
                    // continue as if this shader never happened
                    failed = true;
                    success = true;
                    break;
                }
            }
            ++i;
        }

        if (failed)
        {
            for (auto i = 0; i < 4; ++i)
            {
                gSMAAEdgeDetectProgram[i].unload();
                gSMAABlendWeightsProgram[i].unload();
                gSMAANeighborhoodBlendProgram[i].unload();
            }
        }
    }

    if (success && gDeviceCaps.mApiVersionNumber > 4.05f)
    {
        gCASProgram.mName = "Contrast Adaptive Sharpening Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gCASProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gCASProgram.createShader();
        // llassert(success);
        if (!success)
        {
            LL_WARNS() << "Failed to create shader '" << gCASProgram.mName << "', disabling!" << LL_ENDL;
            // continue as if this shader never happened
            success = true;
        }
    }

    if (success && gDeviceCaps.mApiVersionNumber > 4.05f)
    {
        gCASLegacyGammaProgram.mName = "Contrast Adaptive Sharpening Legacy Gamma Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gCASLegacyGammaProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gCASLegacyGammaProgram.createShader();
        // llassert(success);
        if (!success)
        {
            LL_WARNS() << "Failed to create shader '" << gCASProgram.mName << "', disabling!" << LL_ENDL;
            // continue as if this shader never happened
            success = true;
        }
    }

    if (success)
    {
        gDeferredPostProgram.mName = "Deferred Post Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredPostProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredCoFProgram.mName = "Deferred CoF Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredCoFProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredCoFProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredDoFCombineProgram.mName = "Deferred DoFCombine Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredDoFCombineProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredDoFCombineProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostNoDoFProgram.mName = "Deferred Post NoDoF Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredPostNoDoFProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostNoDoFProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostNoDoFNoiseProgram.mName = "Deferred Post NoDoF Noise Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredPostNoDoFNoiseProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostNoDoFNoiseProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> the HDRI sky (use_hdri_sky(): an HDRI image is loaded): a native only program (native/manifest.txt, family gbuffer: sky.vert + sky.frag with HDRI), read by address like the sky: no stock files
        gEnvironmentMapProgram.mName = "Environment Map Program pull";
        gEnvironmentMapProgram.mFeatures.calculatesAtmospherics = true;
        gEnvironmentMapProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gEnvironmentMapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gEnvironmentMapProgram.mShaderGroup = RKShaderProgram::SG_SKY;

        success = gEnvironmentMapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: sky.vert + sky.frag): it reads the vertices by address and has no other form (it was the pull twin of a stock program of this name, deleted)
        gDeferredWLSkyProgram.mName = "Deferred Windlight Sky Shader pull";
        gDeferredWLSkyProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw; the twin set this)
        gDeferredWLSkyProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLSkyProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLSkyProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gDeferredWLSkyProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: clouds.vert + clouds.frag): it reads the vertices by address and has no other form (it was the pull twin of a stock program of this name, deleted)
        gDeferredWLCloudProgram.mName = "Deferred Windlight Cloud Program pull";
        gDeferredWLCloudProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw; the twin set this)
        gDeferredWLCloudProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLCloudProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLCloudProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gDeferredWLCloudProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: sundisc.vert + sundisc.frag): it reads the vertices by address and has no other form (it was the pull twin of a stock program of this name, deleted)
        gDeferredWLSunProgram.mName = "Deferred Windlight Sun Program pull";
        gDeferredWLSunProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw; the twin set this)
        gDeferredWLSunProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLSunProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLSunProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gDeferredWLSunProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: moon.vert + moon.frag): it reads the vertices by address and has no other form (it was the pull twin of a stock program of this name, deleted)
        gDeferredWLMoonProgram.mName = "Deferred Windlight Moon Program pull";
        gDeferredWLMoonProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw; the twin set this)
        gDeferredWLMoonProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLMoonProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLMoonProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gDeferredWLMoonProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family gbuffer: stars.vert + stars.frag): it reads the vertices by address and has no other form (it was the pull twin of a stock program of this name, deleted)
        gDeferredStarProgram.mName = "Deferred Star Program pull";
        gDeferredStarProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw; the twin set this)
        gDeferredStarProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredStarProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gDeferredStarProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNormalMapGenProgram.mName = "Normal Map Generation Program";
        // <FS> a native only program (native/manifest.txt, family post: normgen.vert + normgen.frag): no stock files
        gNormalMapGenProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gNormalMapGenProgram.mShaderGroup = RKShaderProgram::SG_SKY;
        success = gNormalMapGenProgram.createShader();
    }

    if (success)
    {
        gDeferredGenBrdfLutProgram.mName = "Brdf Gen Shader";
        // <FS> a native only program (native/manifest.txt, family post: copy.vert + brdf_gen.frag): no stock files
        gDeferredGenBrdfLutProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredGenBrdfLutProgram.createShader();
    }

    if (success) {
        gDeferredBufferVisualProgram.mName = "Deferred Buffer Visualization Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gDeferredBufferVisualProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredBufferVisualProgram);

        success = gDeferredBufferVisualProgram.createShader();
    }
    // [RLVa:KB] - @setsphere
    if(success)
    {
        gRlvSphereProgram.mName = "RLVa Sphere Post Processing Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gRlvSphereProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gRlvSphereProgram.createShader();
    }
    // [/RLV:KB]
    return success;
}

bool LLViewerShaderMgr::loadShadersObject()
{
    LL_PROFILE_ZONE_SCOPED;
    bool success = true;

    if (success)
    {
        gObjectAlphaMaskNoColorProgram.mName = "No color alpha mask Shader";
        gObjectAlphaMaskNoColorProgram.mFeatures.calculatesLighting = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.calculatesAtmospherics = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.hasLighting = true;
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gObjectAlphaMaskNoColorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gObjectAlphaMaskNoColorProgram.createShader();
    }

    if (success)
    {
        gImpostorProgram.mName = "Impostor Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gImpostorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gImpostorProgram.createShader();
    }

    if (success)
    {
        gObjectPreviewProgram.mName = "Object Preview Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gObjectPreviewProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = make_rigged_variant(gObjectPreviewProgram, gSkinnedObjectPreviewProgram);
        success = gObjectPreviewProgram.createShader();
        gObjectPreviewProgram.mFeatures.hasLighting = true;
        gSkinnedObjectPreviewProgram.mFeatures.hasLighting = true;
    }

    if (success)
    {
        gPhysicsPreviewProgram.mName = "Preview Physics Shader";
        gPhysicsPreviewProgram.mFeatures.calculatesLighting = false;
        gPhysicsPreviewProgram.mFeatures.calculatesAtmospherics = false;
        gPhysicsPreviewProgram.mFeatures.hasLighting = false;
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gPhysicsPreviewProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gPhysicsPreviewProgram.createShader();
        gPhysicsPreviewProgram.mFeatures.hasLighting = false;
    }

    if (!success)
    {
        mShaderLevel[SHADER_OBJECT] = 0;
        return false;
    }

    return true;
}

bool LLViewerShaderMgr::loadShadersAvatar()
{
    LL_PROFILE_ZONE_SCOPED;
#if 1 // DEPRECATED -- forward rendering is deprecated
    bool success = true;
    if (success)
    {
        gAvatarProgram.mName = "Avatar Shader";
        gAvatarProgram.mFeatures.calculatesAtmospherics = true;
        gAvatarProgram.mFeatures.calculatesLighting = true;
        gAvatarProgram.mFeatures.hasLighting = true;
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gAvatarProgram.mShaderLevel = mShaderLevel[SHADER_AVATAR];
        success = gAvatarProgram.createShader();

        /// Keep track of avatar levels
        if (gAvatarProgram.mShaderLevel != mShaderLevel[SHADER_AVATAR])
        {
            mMaxAvatarShaderLevel = mShaderLevel[SHADER_AVATAR] = gAvatarProgram.mShaderLevel;
        }
    }

    if( !success )
    {
        // This isn't supposed to fail, otherwise defferred shaders would have failed.
        // Is only used for avatar previews (like LLVisualParamHint)
        mShaderLevel[SHADER_AVATAR] = 0;
        mMaxAvatarShaderLevel = 0;
        LL_WARNS() << "Failed to create avatar shader!" << LL_ENDL;
        return false;
    }
#endif
    return true;
}

bool LLViewerShaderMgr::loadShadersInterface()
{
    LL_PROFILE_ZONE_SCOPED;
    bool success = true;

    if (success)
    {
        gHighlightProgram.mName = "Highlight Shader";
        gHighlightProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        // both forms are native only (native/manifest.txt, family post: highlight_face.vert + highlight_face.frag, the skinned one with HAS_SKIN: the old rigged draws of the alpha pool's debug view)
        gSkinnedHighlightProgram.mName = "Skinned Highlight Shader";
        gSkinnedHighlightProgram.mShaderLevel = gHighlightProgram.mShaderLevel;
        gHighlightProgram.mRiggedVariant = &gSkinnedHighlightProgram;
        success = gSkinnedHighlightProgram.createShader();
        success = success && gHighlightProgram.createShader();
    }

    if (success)
    {
        // <FS> K6 B1: the rigged highlight draws (the table's rigged items, the alpha loop's kind 29 record) go through this program: the native stand-in is the only one that draws. Required (PLAN-twin-removal.md, section 8)
        RKShaderProgram& pull = gHighlightPullSkinProgram;
        pull.mName = "Highlight Pull Skin Shader";
        // <FS> a native only program (native/manifest.txt, family alpha): no stock files (it declares `color` and the table's address itself)
        pull.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = pull.createShader();
        if (!success)
        {
            LL_WARNS("ShadowPackets") << "the Highlight Pull Skin Shader could not be made" << LL_ENDL;
        }
        llassert(success);

        // <FS> K6 B1: the Batch Size and Texture Animation views draw the table's items with these (native programs: alpha.vert + debug_batch.frag; the `color` uniform selects the view)
        for (int skin = 0; skin < 2 && success; ++skin)
        {
            RKShaderProgram& debug_program = skin ? gDebugBatchPullSkinProgram : gDebugBatchPullProgram;
            debug_program.mName = skin ? "Debug Batch Pull Skin Shader" : "Debug Batch Pull Shader";
            // <FS> a native only program (native/manifest.txt, family alpha): no stock files
            if (skin)
            {
            }
            debug_program.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
            success = debug_program.createShader();
            if (!success)
            {
                LL_WARNS("ShadowPackets") << "the " << debug_program.mName << " could not be made" << LL_ENDL;
            }
            llassert(success);
        }
    }

    if (success)
    {
        gHighlightNormalProgram.mName = "Highlight Normals Shader";
        // <FS> a native only program (native/manifest.txt, family post: highlight_face.vert with HL_UV=1): no stock files
        gHighlightNormalProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gHighlightNormalProgram.createShader();
    }

    if (success)
    {
        gHighlightSpecularProgram.mName = "Highlight Spec Shader";
        // <FS> a native only program (native/manifest.txt, family post: highlight_face.vert with HL_UV=2): no stock files
        gHighlightSpecularProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gHighlightSpecularProgram.createShader();
    }

    if (success)
    {
        gUIProgram.mName = "UI Shader";       // <FS> native only (native/manifest.txt, family post): no stock files
        gUIProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gUIProgram.createShader();
    }

    if (success)
    {
        gPathfindingProgram.mName = "Pathfinding Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gPathfindingProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gPathfindingProgram.createShader();
    }

    if (success)
    {
        gPathfindingNoNormalsProgram.mName = "PathfindingNoNormals Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gPathfindingNoNormalsProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gPathfindingNoNormalsProgram.createShader();
    }

    if (success)
    {
        gGlowCombineProgram.mName = "Glow Combine Shader";
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gGlowCombineProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gGlowCombineProgram.createShader();
        if (success)
        {
            gGlowCombineProgram.bind();
            gGlowCombineProgram.uniform1i(sGlowMap, 0);
            gGlowCombineProgram.uniform1i(sScreenMap, 1);
            gGlowCombineProgram.unbind();
        }
    }

    if (success)
    {
        gGlowCombineFXAAProgram.mName = "Glow CombineFXAA Shader";
        // <FS> a native only program (native/manifest.txt, family post: copy.vert + glow_fxaa.frag): no stock files
        gGlowCombineFXAAProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gGlowCombineFXAAProgram.createShader();
        if (success)
        {
            gGlowCombineFXAAProgram.bind();
            gGlowCombineFXAAProgram.uniform1i(sGlowMap, 0);
            gGlowCombineFXAAProgram.uniform1i(sScreenMap, 1);
            gGlowCombineFXAAProgram.unbind();
        }
    }

#ifdef LL_WINDOWS
    if (success)
    {
        gTwoTextureCompareProgram.mName = "Two Texture Compare Shader";
        // <FS> a native only program (native/manifest.txt, family post: texcompare.vert + texcompare.frag): no stock files
        gTwoTextureCompareProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gTwoTextureCompareProgram.createShader();
        if (success)
        {
            gTwoTextureCompareProgram.bind();
            gTwoTextureCompareProgram.uniform1i(sTex0, 0);
            gTwoTextureCompareProgram.uniform1i(sTex1, 1);
            gTwoTextureCompareProgram.uniform1i(sDitherTex, 2);
        }
    }

    if (success)
    {
        gOneTextureFilterProgram.mName = "One Texture Filter Shader";
        // <FS> a native only program (native/manifest.txt, family post: texfilter.vert + texfilter.frag): no stock files
        gOneTextureFilterProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gOneTextureFilterProgram.createShader();
        if (success)
        {
            gOneTextureFilterProgram.bind();
            gOneTextureFilterProgram.uniform1i(sTex0, 0);
        }
    }
#endif

    if (success)
    {
        gSolidColorProgram.mName = "Solid Color Shader";       // <FS> native only (native/manifest.txt, family post): no stock files
        gSolidColorProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gSolidColorProgram.createShader();
        if (success)
        {
            gSolidColorProgram.bind();
            gSolidColorProgram.uniform1i(sTex0, 0);
            gSolidColorProgram.unbind();
        }
    }

    if (success)
    {
        gDebugProgram.mName = "Debug Shader";
        gDebugProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        // both forms are native only (native/manifest.txt, family post: debug.vert + debug.frag, the skinned one with HAS_SKIN)
        gSkinnedDebugProgram.mName = "Skinned Debug Shader";
        gSkinnedDebugProgram.mShaderLevel = gDebugProgram.mShaderLevel;
        gDebugProgram.mRiggedVariant = &gSkinnedDebugProgram;
        success = gSkinnedDebugProgram.createShader();
        success = success && gDebugProgram.createShader();
    }

    if (success)
    {
        for (S32 variant = 0; variant < NORMAL_DEBUG_SHADER_COUNT; ++variant)
        {
            // native only (native/manifest.txt, family post: normal_debug.vert + normal_debug.geom + normal_debug.frag, the tangent form with HAS_ATTRIBUTE_TANGENT); the skinned form was never bound
            RKShaderProgram& shader = gNormalDebugProgram[variant];
            shader.mName = variant == NORMAL_DEBUG_SHADER_WITH_TANGENTS ? "Normal Debug Tangents Shader" : "Normal Debug Shader";
            shader.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
            success = shader.createShader();
        }
    }

    if (success)
    {
        gBenchmarkProgram.mName = "Benchmark Shader";
        // <FS> a native only program (native/manifest.txt, family copy: copy.vert + copy.frag): no stock files
        gBenchmarkProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gBenchmarkProgram.createShader();
    }

    if (success)
    {
        gReflectionProbeDisplayProgram.mName = "Reflection Probe Display Shader";
        gReflectionProbeDisplayProgram.mFeatures.hasReflectionProbes = true;
        gReflectionProbeDisplayProgram.mFeatures.calculatesAtmospherics = true;
        // <FS> a native only program (native/manifest.txt, family post): no stock files
        gReflectionProbeDisplayProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gReflectionProbeDisplayProgram.createShader();
    }

    if (success)
    {
        gCopyProgram.mName = "Copy Shader";
        // <FS> a native only program (native/manifest.txt, family copy): no stock files (copyV/copyF stay for the Copy Depth Shader)
        gCopyProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gCopyProgram.createShader();
    }

    if (success)
    {
        gCopyDepthProgram.mName = "Copy Depth Shader";       // <FS> native only (native/manifest.txt, family post): no stock files
        gCopyDepthProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gCopyDepthProgram.createShader();
    }

    if (success)
    {
        // <FS> a native only program (native/manifest.txt, family alpha: alpha.vert + highlight.frag with HIGHLIGHT_PASS): the flat colour of the water exclusion and the highlight views, read by address; no stock files (it was the pull twin of a stock program of this name, deleted)
        gDrawColorProgram.mName = "Draw Color Shader pull";
        gDrawColorProgram.addPermutation("RK_VERTEX_PULL", "1");       // (a draw of a vertex buffer under it is one entry of a geometry record: LLVertexBuffer::pullDraw)
        gDrawColorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gDrawColorProgram.createShader();
    }

    if (gSavedSettings.getBOOL("LocalTerrainPaintEnabled"))
    {
        if (success)
        {
            RKShaderProgram* shader = &gPBRTerrainBakeProgram;
            U32 bit_depth = gSavedSettings.getU32("TerrainPaintBitDepth");
            // LLTerrainPaintMap currently uses an RGB8 texture internally
            bit_depth = llclamp(bit_depth, 1, 8);
            shader->mName = llformat("Terrain Bake Shader RGB%o", bit_depth);

            shader->mShaderLevel = mShaderLevel[SHADER_INTERFACE];
            const U32 value_range = (1 << bit_depth) - 1;
            success = success && shader->createShader();
            //llassert(success);
            if (!success)
            {
                LL_WARNS() << "Failed to create shader '" << shader->mName << "', disabling!" << LL_ENDL;
                gSavedSettings.setBOOL("RenderCanUseTerrainBakeShaders", false);
                // continue as if this shader never happened
                success = true;
            }
        }
    }

    if (success)
    {
        gAlphaMaskProgram.mName = "Alpha Mask Shader";
        // <FS> a native only program (native/manifest.txt, family post: alphamask.vert + alphamask.frag): no stock files
        gAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gAlphaMaskProgram.createShader();
    }

    if (success)
    {
        gReflectionMipProgram.mName = "Reflection Mip Shader";
        gReflectionMipProgram.mFeatures.calculatesAtmospherics = true;
        // <FS> a native only program (native/manifest.txt, family post: reflectionmip.vert + reflectionmip.frag): no stock files
        gReflectionMipProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gReflectionMipProgram.createShader();
    }

    if (success)
    {
        gGaussianProgram.mName = "Reflection Mip Gaussian Shader";       // <FS> (it shared its name with the program above: the manifest tells programs apart by name) a native only program (family post: reflectionmip.vert + gaussian.frag): no stock files
        gGaussianProgram.mFeatures.calculatesAtmospherics = true;
        gGaussianProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gGaussianProgram.createShader();
    }

    if (success && gDeviceCaps.mHasCubeMapArray)
    {
        gRadianceGenProgram.mName = "Radiance Gen Shader";       // <FS> native only (native/manifest.txt, family post): no stock files
        gRadianceGenProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gRadianceGenProgram.createShader();
    }

    if (success && gDeviceCaps.mHasCubeMapArray)
    {
        gIrradianceGenProgram.mName = "Irradiance Gen Shader";       // <FS> native only (native/manifest.txt, family post): no stock files
        gIrradianceGenProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gIrradianceGenProgram.createShader();
    }

    if( !success )
    {
        mShaderLevel[SHADER_INTERFACE] = 0;
        return false;
    }

    return true;
}


std::string LLViewerShaderMgr::getShaderDirPrefix(void)
{
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders", "class");
}

void LLViewerShaderMgr::updateShaderUniforms(RKShaderProgram * shader)
{
    LLEnvironment::instance().updateShaderUniforms(shader);
}

