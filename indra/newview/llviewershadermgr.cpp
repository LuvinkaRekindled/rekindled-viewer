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
#include "llvkbridge.h"
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
LLShaderProgram    gOcclusionProgram;
LLShaderProgram    gSkinnedOcclusionProgram;
LLShaderProgram    gOcclusionCubeProgram;
LLShaderProgram    gGlowCombineProgram;
LLShaderProgram    gReflectionMipProgram;
LLShaderProgram    gGaussianProgram;
LLShaderProgram    gRadianceGenProgram;
LLShaderProgram    gHeroRadianceGenProgram;
LLShaderProgram    gIrradianceGenProgram;
LLShaderProgram    gGlowCombineFXAAProgram;
LLShaderProgram    gTwoTextureCompareProgram;
LLShaderProgram    gOneTextureFilterProgram;
LLShaderProgram    gDebugProgram;
LLShaderProgram    gSkinnedDebugProgram;
LLShaderProgram    gNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT];
LLShaderProgram    gSkinnedNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT];
LLShaderProgram    gClipProgram;
LLShaderProgram    gAlphaMaskProgram;
LLShaderProgram    gBenchmarkProgram;
LLShaderProgram    gReflectionProbeDisplayProgram;
LLShaderProgram    gCopyProgram;
LLShaderProgram    gCopyDepthProgram;
LLShaderProgram    gPBRTerrainBakeProgram;
LLShaderProgram    gDrawColorProgram;

//object shaders
LLShaderProgram        gObjectPreviewProgram;
LLShaderProgram        gSkinnedObjectPreviewProgram;
LLShaderProgram        gPhysicsPreviewProgram;
LLShaderProgram        gObjectFullbrightAlphaMaskProgram;
LLShaderProgram        gObjectBumpProgram;
LLShaderProgram        gSkinnedObjectBumpProgram;
LLShaderProgram        gObjectAlphaMaskNoColorProgram;

//environment shaders
LLShaderProgram        gWaterProgram;
LLShaderProgram        gUnderWaterProgram;

//interface shaders
LLShaderProgram        gHighlightProgram;
LLShaderProgram        gSkinnedHighlightProgram;
LLShaderProgram        gHighlightNormalProgram;
LLShaderProgram        gHighlightSpecularProgram;

LLShaderProgram        gDeferredHighlightProgram;

LLShaderProgram        gPathfindingProgram;
LLShaderProgram        gPathfindingNoNormalsProgram;

//avatar shader handles
LLShaderProgram        gAvatarProgram;
LLShaderProgram        gImpostorProgram;

// Effects Shaders
LLShaderProgram            gGlowProgram;
LLShaderProgram            gGlowExtractProgram;
LLShaderProgram            gPostScreenSpaceReflectionProgram;
LLShaderProgram            gPostVignetteProgram;   // <FS:CR> Import Vignette from Exodus
LLShaderProgram            gPostSnapshotFrameProgram;   // <FS:Beq/> Add Snapshot frame guide

// Deferred rendering shaders
LLShaderProgram            gDeferredImpostorProgram;
LLShaderProgram            gDeferredDiffuseProgram;
LLShaderProgram            gDeferredDiffuseAlphaMaskProgram;
LLShaderProgram            gDeferredSkinnedDiffuseAlphaMaskProgram;
LLShaderProgram            gDeferredNonIndexedDiffuseAlphaMaskProgram;
LLShaderProgram            gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram;
LLShaderProgram            gDeferredSkinnedDiffuseProgram;
LLShaderProgram            gDeferredSkinnedBumpProgram;
LLShaderProgram            gDeferredBumpProgram;
LLShaderProgram            gDeferredTerrainProgram;
LLShaderProgram            gDeferredTreeProgram;
LLShaderProgram            gDeferredTreeShadowProgram;
LLShaderProgram            gDeferredSkinnedTreeShadowProgram;
LLShaderProgram            gDeferredAvatarProgram;
LLShaderProgram            gDeferredAvatarAlphaProgram;
LLShaderProgram            gDeferredLightProgram;
LLShaderProgram            gDeferredMultiLightProgram[16];
LLShaderProgram            gDeferredSpotLightProgram;
LLShaderProgram            gDeferredMultiSpotLightProgram;
LLShaderProgram            gDeferredSunProgram;
LLShaderProgram            gDeferredSunProbeProgram;
LLShaderProgram            gHazeProgram;
LLShaderProgram            gHazeWaterProgram;
LLShaderProgram            gDeferredBlurLightProgram;
LLShaderProgram            gDeferredSoftenProgram;
LLShaderProgram            gDeferredShadowProgram;
LLShaderProgram            gDeferredSkinnedShadowProgram;
LLShaderProgram            gDeferredShadowCubeProgram;
LLShaderProgram            gDeferredShadowPullProgram;
LLShaderProgram            gDeferredShadowAlphaPullProgram;
LLShaderProgram            gDeferredShadowSkinPullProgram;
LLShaderProgram            gDeferredDiffusePullProgram;
LLShaderProgram            gDeferredAlphaPullProgram;
LLShaderProgram            gDeferredFullbrightAlphaPullProgram;
bool                       gAlphaPullReady = false;
bool                       gAlphaMaterialPullReady = false;
LLShaderProgram            gDeferredPBRAlphaPullProgram;
bool                       gAlphaPbrPullReady = false;
LLShaderProgram            gDeferredMaterialPullProgram[16];
LLShaderProgram            gDeferredDiffusePullSkinProgram;        // <FS> S4 A1: the same for rigged draws (kind 25 records: weights and palette by address)
LLShaderProgram            gDeferredMaterialPullSkinProgram[16];
LLShaderProgram            gDeferredShadowAlphaMaskProgram;
LLShaderProgram            gDeferredSkinnedShadowAlphaMaskProgram;
LLShaderProgram            gDeferredShadowGLTFAlphaMaskProgram;
LLShaderProgram            gDeferredSkinnedShadowGLTFAlphaMaskProgram;
LLShaderProgram            gDeferredShadowGLTFAlphaBlendProgram;
LLShaderProgram            gDeferredSkinnedShadowGLTFAlphaBlendProgram;
LLShaderProgram            gDeferredShadowFullbrightAlphaMaskProgram;
LLShaderProgram            gDeferredSkinnedShadowFullbrightAlphaMaskProgram;
LLShaderProgram            gDeferredAvatarShadowProgram;
LLShaderProgram            gDeferredAvatarAlphaShadowProgram;
LLShaderProgram            gDeferredAvatarAlphaMaskShadowProgram;
LLShaderProgram            gDeferredAlphaProgram;
LLShaderProgram            gHUDAlphaProgram;
LLShaderProgram            gDeferredSkinnedAlphaProgram;
LLShaderProgram            gDeferredAlphaImpostorProgram;
LLShaderProgram            gDeferredSkinnedAlphaImpostorProgram;
LLShaderProgram            gDeferredAvatarEyesProgram;
LLShaderProgram            gDeferredFullbrightProgram;
LLShaderProgram            gHUDFullbrightProgram;
LLShaderProgram            gDeferredFullbrightAlphaMaskProgram;
LLShaderProgram            gHUDFullbrightAlphaMaskProgram;
LLShaderProgram            gDeferredFullbrightAlphaMaskAlphaProgram;
LLShaderProgram            gHUDFullbrightAlphaMaskAlphaProgram;
LLShaderProgram            gDeferredEmissiveProgram;
LLShaderProgram            gDeferredSkinnedEmissiveProgram;
LLShaderProgram            gDeferredPostProgram;
LLShaderProgram            gDeferredCoFProgram;
LLShaderProgram            gDeferredDoFCombineProgram;
LLShaderProgram            gDeferredPostTonemapProgram;
LLShaderProgram            gNoPostTonemapProgram;
LLShaderProgram            gDeferredPostTonemapGammaCorrectProgram;
LLShaderProgram            gNoPostTonemapGammaCorrectProgram;
LLShaderProgram            gDeferredPostTonemapLegacyGammaCorrectProgram;
LLShaderProgram            gNoPostTonemapLegacyGammaCorrectProgram;
LLShaderProgram            gDeferredPostGammaCorrectProgram;
LLShaderProgram            gLegacyPostGammaCorrectProgram;
LLShaderProgram            gExposureProgram;
LLShaderProgram            gExposureProgramNoFade;
LLShaderProgram            gLuminanceProgram;
LLShaderProgram            gMotionVectorProgram;
LLShaderProgram            gAvatarVelocityProgram;         // <FS> avatar motion vectors: rigged draws
LLShaderProgram            gAvatarBodyVelocityProgram;     // <FS> avatar motion vectors: the avatar's own body meshes
LLShaderProgram            gAvatarVelocityPullProgram;     // <FS> avatar motion vectors: rigged draws from render packets (kind 8)
LLShaderProgram            gObjectVelocityPullProgram;     // <FS> avatar motion vectors: moving objects and rigid attachments from render packets (kind 9)
LLShaderProgram            gShadowAccumProgram;            // <FS> temporal accumulation of the ray traced sun shadow
LLShaderProgram            gRRHitDistProgram;              // <FS> DLSS ray reconstruction: the hit distance of the traced reflections out of the alpha of the lit picture
LLShaderProgram            gRRReflMotionProgram;           // <FS> DLSS ray reconstruction: motion vectors of what the traced reflections show
LLShaderProgram            gRRGuidesProgram;               // <FS> DLSS ray reconstruction: diffuse / specular albedo and normal + roughness from the G-buffer
LLShaderProgram            gAvatarVelocityDebugProgram;    // <FS> debug view of a motion target
LLShaderProgram            gRtMaterialDebugProgram;        // <FS> debug view of the ray scene's material table (RenderBufferVisualization 9)
LLShaderProgram            gFXAAProgram[4];
LLShaderProgram            gSMAAEdgeDetectProgram[4];
LLShaderProgram            gSMAABlendWeightsProgram[4];
LLShaderProgram            gSMAANeighborhoodBlendProgram[4];
LLShaderProgram            gCASProgram;
LLShaderProgram            gCASLegacyGammaProgram;
LLShaderProgram            gDeferredPostNoDoFProgram;
LLShaderProgram            gDeferredPostNoDoFNoiseProgram;
LLShaderProgram            gDeferredWLSkyProgram;
LLShaderProgram            gEnvironmentMapProgram;
LLShaderProgram            gDeferredWLCloudProgram;
LLShaderProgram            gDeferredWLSunProgram;
LLShaderProgram            gDeferredWLMoonProgram;
LLShaderProgram            gDeferredStarProgram;
LLShaderProgram            gDeferredFullbrightShinyProgram;
LLShaderProgram            gHUDFullbrightShinyProgram;
LLShaderProgram            gDeferredSkinnedFullbrightShinyProgram;
LLShaderProgram            gDeferredSkinnedFullbrightProgram;
LLShaderProgram            gDeferredSkinnedFullbrightAlphaMaskProgram;
LLShaderProgram            gDeferredSkinnedFullbrightAlphaMaskAlphaProgram;
LLShaderProgram            gNormalMapGenProgram;
LLShaderProgram            gDeferredGenBrdfLutProgram;
LLShaderProgram            gDeferredBufferVisualProgram;
// [RLVa:KB] - @setsphere
LLShaderProgram            gRlvSphereProgram;
// [/RLVa:KB]

// Deferred materials shaders
LLShaderProgram            gDeferredMaterialProgram[LLMaterial::SHADER_COUNT*2];
LLShaderProgram            gHUDPBROpaqueProgram;
LLShaderProgram            gPBRGlowProgram;
LLShaderProgram            gPBRGlowSkinnedProgram;
LLShaderProgram            gDeferredPBROpaqueProgram;
LLShaderProgram            gDeferredSkinnedPBROpaqueProgram;
LLShaderProgram            gHUDPBRAlphaProgram;
LLShaderProgram            gDeferredPBRAlphaProgram;
LLShaderProgram            gDeferredSkinnedPBRAlphaProgram;
LLShaderProgram            gDeferredPBRTerrainProgram[TERRAIN_PAINT_TYPE_COUNT];

LLShaderProgram            gGLTFPBRMetallicRoughnessProgram;


//helper for making a rigged variant of a given shader
extern bool ll_rigged_indexed();       // lldrawpool.cpp (S4 A1)

static bool make_rigged_variant(LLShaderProgram& shader, LLShaderProgram& riggedShader)
{
    riggedShader.mName = llformat("Skinned %s", shader.mName.c_str());
    riggedShader.mFeatures = shader.mFeatures;
    riggedShader.mFeatures.hasObjectSkinning = true;
    riggedShader.mDefines = shader.mDefines;    // NOTE: Must come before addPermutation

    riggedShader.addPermutation("HAS_SKIN", "1");
    riggedShader.mShaderFiles = shader.mShaderFiles;
    riggedShader.mShaderLevel = shader.mShaderLevel;
    riggedShader.mShaderGroup = shader.mShaderGroup;

    shader.mRiggedVariant = &riggedShader;
    return riggedShader.createShader();
}

static void add_common_permutations(LLShaderProgram* shader)
{
    static LLCachedControl<bool> emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);

    if (emissive)
    {
        shader->addPermutation("HAS_EMISSIVE", "1");
    }
}


static bool make_gltf_variant(LLShaderProgram& shader, LLShaderProgram& variant, bool alpha_blend, bool rigged, bool unlit, bool multi_uv, bool use_sun_shadow)
{
    variant.mName = shader.mName.c_str();
    variant.mFeatures = shader.mFeatures;
    variant.mShaderFiles = shader.mShaderFiles;
    variant.mShaderLevel = shader.mShaderLevel;
    variant.mShaderGroup = shader.mShaderGroup;

    variant.mDefines = shader.mDefines;    // NOTE: Must come before addPermutation

    U32 node_size = 16 * 3;
    U32 max_nodes = gDeviceCaps.mMaxUniformBlockSize / node_size;
    variant.addPermutation("MAX_NODES_PER_GLTF_OBJECT", std::to_string(max_nodes));

    U32 material_size = 16 * 12;
    U32 max_materials = gDeviceCaps.mMaxUniformBlockSize / material_size;
    LLShaderProgram::sMaxGLTFMaterials = max_materials;

    variant.addPermutation("MAX_MATERIALS_PER_GLTF_OBJECT", std::to_string(max_materials));

    U32 max_vec4s = gDeviceCaps.mMaxUniformBlockSize / 16;
    variant.addPermutation("MAX_UBO_VEC4S", std::to_string(max_vec4s));

    if (rigged)
    {
        variant.addPermutation("HAS_SKIN", "1");
    }

    if (unlit)
    {
        variant.addPermutation("UNLIT", "1");
    }

    if (multi_uv)
    {
        variant.addPermutation("MULTI_UV", "1");
    }

    if (alpha_blend)
    {
        variant.addPermutation("ALPHA_BLEND", "1");

        variant.mFeatures.calculatesLighting = false;
        variant.mFeatures.hasLighting = false;
        variant.mFeatures.isAlphaLighting = true;
        variant.mFeatures.hasSrgb = true;
        variant.mFeatures.calculatesAtmospherics = true;
        variant.mFeatures.hasAtmospherics = true;
        variant.mFeatures.hasGamma = true;
        variant.mFeatures.hasShadows = use_sun_shadow;
        variant.mFeatures.isDeferred = true; // include deferredUtils
        variant.mFeatures.hasReflectionProbes = true;

        if (use_sun_shadow)
        {
            variant.addPermutation("HAS_SUN_SHADOW", "1");
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

static bool make_gltf_variants(LLShaderProgram& shader, bool use_sun_shadow)
{
    shader.mFeatures.mGLTF = true;
    shader.mGLTFVariants.resize(LLShaderProgram::NUM_GLTF_VARIANTS);

    for (U32 i = 0; i < LLShaderProgram::NUM_GLTF_VARIANTS; ++i)
    {
        bool alpha_blend = i & LLShaderProgram::GLTFVariant::ALPHA_BLEND;
        bool rigged = i & LLShaderProgram::GLTFVariant::RIGGED;
        bool unlit = i & LLShaderProgram::GLTFVariant::UNLIT;
        bool multi_uv = i & LLShaderProgram::GLTFVariant::MULTI_UV;

        if (!make_gltf_variant(shader, shader.mGLTFVariants[i], alpha_blend, rigged, unlit, multi_uv, use_sun_shadow))
        {
            return false;
        }
    }

    return true;
}

#ifdef SHOW_ASSERT
// return true if there are no redundant shaders in the given vector
// also checks for redundant variants
static bool no_redundant_shaders(const std::vector<LLShaderProgram*>& shaders)
{
    std::set<std::string> names;
    for (LLShaderProgram* shader : shaders)
    {
        if (names.find(shader->mName) != names.end())
        {
            LL_WARNS("Shader") << "Redundant shader: " << shader->mName << LL_ENDL;
            return false;
        }
        names.insert(shader->mName);

        if (shader->mRiggedVariant)
        {
            if (names.find(shader->mRiggedVariant->mName) != names.end())
            {
                LL_WARNS("Shader") << "Redundant shader: " << shader->mRiggedVariant->mName << LL_ENDL;
                return false;
            }
            names.insert(shader->mRiggedVariant->mName);
        }
    }
    return true;
}
#endif


LLViewerShaderMgr::LLViewerShaderMgr() :
    mShaderLevel(SHADER_COUNT, 0),
    mMaxAvatarShaderLevel(0)
{
}

LLViewerShaderMgr::~LLViewerShaderMgr()
{
    mShaderLevel.clear();
    mShaderList.clear();
}

void LLViewerShaderMgr::finalizeShaderList()
{
    //ONLY shaders that need WL Param management should be added here
    mShaderList.push_back(&gAvatarProgram);
    mShaderList.push_back(&gWaterProgram);
    mShaderList.push_back(&gImpostorProgram);
    mShaderList.push_back(&gObjectBumpProgram);
    mShaderList.push_back(&gObjectFullbrightAlphaMaskProgram);
    mShaderList.push_back(&gObjectAlphaMaskNoColorProgram);
    mShaderList.push_back(&gUnderWaterProgram);
    mShaderList.push_back(&gDeferredSunProgram);
    mShaderList.push_back(&gDeferredSunProbeProgram);
    mShaderList.push_back(&gHazeProgram);
    mShaderList.push_back(&gHazeWaterProgram);
    mShaderList.push_back(&gDeferredSoftenProgram);
    mShaderList.push_back(&gDeferredAlphaProgram);
    mShaderList.push_back(&gHUDAlphaProgram);
    mShaderList.push_back(&gDeferredAlphaImpostorProgram);
    mShaderList.push_back(&gDeferredFullbrightProgram);
    mShaderList.push_back(&gHUDFullbrightProgram);
    mShaderList.push_back(&gDeferredFullbrightAlphaMaskProgram);
    mShaderList.push_back(&gHUDFullbrightAlphaMaskProgram);
    mShaderList.push_back(&gDeferredFullbrightAlphaMaskAlphaProgram);
    mShaderList.push_back(&gHUDFullbrightAlphaMaskAlphaProgram);
    mShaderList.push_back(&gDeferredFullbrightShinyProgram);
    mShaderList.push_back(&gHUDFullbrightShinyProgram);
    mShaderList.push_back(&gDeferredEmissiveProgram);
    mShaderList.push_back(&gDeferredAvatarEyesProgram);
    mShaderList.push_back(&gDeferredAvatarAlphaProgram);
    mShaderList.push_back(&gEnvironmentMapProgram);
    mShaderList.push_back(&gDeferredWLSkyProgram);
    mShaderList.push_back(&gDeferredWLCloudProgram);
    mShaderList.push_back(&gDeferredWLMoonProgram);
    mShaderList.push_back(&gDeferredWLSunProgram);
    mShaderList.push_back(&gDeferredPBRAlphaProgram);
    mShaderList.push_back(&gHUDPBRAlphaProgram);
    mShaderList.push_back(&gDeferredPostTonemapProgram);
    mShaderList.push_back(&gNoPostTonemapProgram);
    mShaderList.push_back(&gDeferredPostTonemapGammaCorrectProgram);
    mShaderList.push_back(&gNoPostTonemapGammaCorrectProgram);
    mShaderList.push_back(&gDeferredPostTonemapLegacyGammaCorrectProgram);
    mShaderList.push_back(&gNoPostTonemapLegacyGammaCorrectProgram);
    mShaderList.push_back(&gCASLegacyGammaProgram);
    mShaderList.push_back(&gDeferredPostGammaCorrectProgram); // for gamma
    mShaderList.push_back(&gLegacyPostGammaCorrectProgram);
    mShaderList.push_back(&gDeferredDiffuseProgram);
    mShaderList.push_back(&gDeferredBumpProgram);
    mShaderList.push_back(&gDeferredPBROpaqueProgram);

    if (gSavedSettings.getBOOL("GLTFEnabled"))
    {
        mShaderList.push_back(&gGLTFPBRMetallicRoughnessProgram);
    }

    mShaderList.push_back(&gDeferredAvatarProgram);
    mShaderList.push_back(&gDeferredTerrainProgram);

    for (U32 paint_type = 0; paint_type < TERRAIN_PAINT_TYPE_COUNT; ++paint_type)
    {
        mShaderList.push_back(&gDeferredPBRTerrainProgram[paint_type]);
    }

    mShaderList.push_back(&gDeferredDiffuseAlphaMaskProgram);
    mShaderList.push_back(&gDeferredNonIndexedDiffuseAlphaMaskProgram);
    mShaderList.push_back(&gDeferredTreeProgram);

    // make sure there are no redundancies
    llassert(no_redundant_shaders(mShaderList));
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

    mShaderList.clear();

    if (!gDeviceCaps.mHasRequirements)
    {
        // Viewer will show 'hardware requirements' warning later
        LL_INFOS("ShaderLoading") << "Not supported hardware/software" << LL_ENDL;
        return;
    }

    {
        static LLCachedControl<bool> shader_cache_enabled(gSavedSettings, "RenderShaderCacheEnabled", true);
        static LLUUID old_cache_version;
        static LLUUID current_cache_version;
        if (current_cache_version.isNull())
        {
            HBXXH128 hash_obj;
            hash_obj.update(LLVersionInfo::instance().getVersion());
            current_cache_version = hash_obj.digest();

            old_cache_version = LLUUID(gSavedSettings.getString("RenderShaderCacheVersion"));
            gSavedSettings.setString("RenderShaderCacheVersion", current_cache_version.asString());
        }

        initShaderCache(
            shader_cache_enabled,
            old_cache_version,
            current_cache_version,
            LLAppViewer::instance()->isSecondInstance());
    }

    static LLCachedControl<U32> max_texture_index(gSavedSettings, "RenderMaxTextureIndex", 16);

    // when using indexed texture rendering, leave some texture units available for shadow and reflection maps
    static LLCachedControl<S32> reserved_texture_units(gSavedSettings, "RenderReservedTextureIndices", 14);

    LLShaderProgram::sIndexedTextureChannels = 4;
    if (const char* channels = getenv("LL_VK_INDEXED_CHANNELS"))      // test switch: 1 = every draw one texture, no per-vertex texture index
    {
        LLShaderProgram::sIndexedTextureChannels = llclamp<S32>(atoi(channels), 1, 4);
    }
        //llclamp<S32>(max_texture_index, 1, gDeviceCaps.mNumTextureImageUnits-reserved_texture_units);

    reentrance = true;

    // Make sure the compiled shader map is cleared before we recompile shaders.
    mVertexShaderObjects.clear();
    mFragmentShaderObjects.clear();

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

    if (!LLAppViewer::instance()->isSecondInstance())
    {
        persistShaderCacheMetadata();
    }

    if (gViewerWindow)
    {
        gViewerWindow->setCursor(UI_CURSOR_ARROW);
    }
    gPipeline.createRenderBuffers();

    finalizeShaderList();

    reentrance = false;
}

void LLViewerShaderMgr::unloadShaders()
{
    while (!LLShaderProgram::sInstances.empty())
    {
        LLShaderProgram* shader = *(LLShaderProgram::sInstances.begin());
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
    // Load basic dependency shaders first
    // All of these have to load for any shaders to function

    S32 sum_lights_class = 3;

#if LL_DARWIN
    // Work around driver crashes on older Macs when using deferred rendering
    // NORSPEC-59
    //
    if (gDeviceCaps.mIsMobileGF)
        sum_lights_class = 3;
#endif

    // Use the feature table to mask out the max light level to use.  Also make sure it's at least 1.
    S32 max_light_class = gSavedSettings.getS32("RenderShaderLightingMaxLevel");
    sum_lights_class = llclamp(sum_lights_class, 1, max_light_class);

    // Load the Basic Vertex Shaders at the appropriate level.
    // (in order of shader function call depth for reference purposes, deepest level first)

    vector< pair<string, S32> > shaders;
    shaders.push_back( make_pair( "windlight/atmosphericsVarsV.glsl",       mShaderLevel[SHADER_WINDLIGHT] ) );
    shaders.push_back( make_pair( "windlight/atmosphericsHelpersV.glsl",    mShaderLevel[SHADER_WINDLIGHT] ) );
    shaders.push_back( make_pair( "lighting/lightFuncV.glsl",               mShaderLevel[SHADER_LIGHTING] ) );
    shaders.push_back( make_pair( "lighting/sumLightsV.glsl",               sum_lights_class ) );
    shaders.push_back( make_pair( "lighting/lightV.glsl",                   mShaderLevel[SHADER_LIGHTING] ) );
    shaders.push_back( make_pair( "lighting/lightFuncSpecularV.glsl",       mShaderLevel[SHADER_LIGHTING] ) );
    shaders.push_back( make_pair( "lighting/sumLightsSpecularV.glsl",       sum_lights_class ) );
    shaders.push_back( make_pair( "lighting/lightSpecularV.glsl",           mShaderLevel[SHADER_LIGHTING] ) );
    shaders.push_back( make_pair( "windlight/atmosphericsFuncs.glsl",       mShaderLevel[SHADER_WINDLIGHT] ) );
    shaders.push_back( make_pair( "windlight/atmosphericsV.glsl",           mShaderLevel[SHADER_WINDLIGHT] ) );
    shaders.push_back( make_pair( "environment/srgbF.glsl",                 1 ) );
    shaders.push_back( make_pair( "avatar/avatarSkinV.glsl",                1 ) );
    shaders.push_back( make_pair( "avatar/objectSkinV.glsl",                1 ) );
    shaders.push_back( make_pair( "deferred/textureUtilV.glsl",             1 ) );
    if (gDeviceCaps.mShaderLangMajor >= 2 || gDeviceCaps.mShaderLangMinor >= 30)
    {
        shaders.push_back( make_pair( "objects/indexedTextureV.glsl",           1 ) );
    }
    shaders.push_back( make_pair( "objects/nonindexedTextureV.glsl",        1 ) );

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

        // <FS> ray query sun shadows (LL_VK_RT_SHADOWS, doc/vulkan/PLAN-rt-shadows.md): 1 replace the cascades, 2 split screen, 3 disagreement map
        if (const U32 rt_mode = LLVkBridge::rtShadowMode())
        {
            attribs["LL_RT_SHADOWS"] = std::to_string(rt_mode);
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

    LLShaderProgram::sGlobalDefines = attribs;

    // We no longer have to bind the shaders to global glhandles, they are automatically added to a map now.
    for (U32 i = 0; i < shaders.size(); i++)
    {
        // Note usage of ShaderStage::Vertex
        if (loadShaderFile(shaders[i].first, shaders[i].second, ShaderStage::Vertex, &attribs) == 0)
        {
            LL_WARNS("Shader") << "Failed to load basic vertex shader " << i << ": " << shaders[i].first << LL_ENDL;
            return shaders[i].first;
        }
    }

    // Load the Basic Fragment Shaders at the appropriate level.
    // (in order of shader function call depth for reference purposes, deepest level first)

    shaders.clear();
    S32 ch = 1;

    if (gDeviceCaps.mShaderLangMajor > 1 || gDeviceCaps.mShaderLangMinor >= 30)
    { //use indexed texture rendering for GLSL >= 1.30
        ch = llmax(LLShaderProgram::sIndexedTextureChannels, 1);
    }


    std::vector<S32> index_channels;
    index_channels.push_back(-1);    shaders.push_back( make_pair( "windlight/atmosphericsVarsF.glsl",      mShaderLevel[SHADER_WINDLIGHT] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "windlight/atmosphericsHelpersF.glsl",       mShaderLevel[SHADER_WINDLIGHT] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "windlight/gammaF.glsl",                 mShaderLevel[SHADER_WINDLIGHT]) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "windlight/atmosphericsFuncs.glsl",       mShaderLevel[SHADER_WINDLIGHT] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "windlight/atmosphericsF.glsl",          mShaderLevel[SHADER_WINDLIGHT] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "environment/waterFogF.glsl",                mShaderLevel[SHADER_WATER] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "environment/srgbF.glsl",                    mShaderLevel[SHADER_ENVIRONMENT] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/deferredUtil.glsl",                    1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/gbufferUtil.glsl",                    1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/globalF.glsl",                          1));
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/shadowUtil.glsl",                      1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/aoUtil.glsl",                          1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/pbrterrainUtilF.glsl",                 1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/tonemapUtilF.glsl",                    1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/reflectionProbeF.glsl",                has_reflection_probes ? 3 : 2) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "deferred/screenSpaceReflUtil.glsl",             ssr ? 3 : 1) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "lighting/lightNonIndexedF.glsl",                    mShaderLevel[SHADER_LIGHTING] ) );
    index_channels.push_back(-1);    shaders.push_back( make_pair( "lighting/lightAlphaMaskNonIndexedF.glsl",                   mShaderLevel[SHADER_LIGHTING] ) );
    index_channels.push_back(ch);    shaders.push_back( make_pair( "lighting/lightF.glsl",                  mShaderLevel[SHADER_LIGHTING] ) );
    index_channels.push_back(ch);    shaders.push_back( make_pair( "lighting/lightAlphaMaskF.glsl",                 mShaderLevel[SHADER_LIGHTING] ) );

    for (U32 i = 0; i < shaders.size(); i++)
    {
        // Note usage of ShaderStage::Fragment
        if (loadShaderFile(shaders[i].first, shaders[i].second, ShaderStage::Fragment, &attribs, index_channels[i]) == 0)
        {
            LL_WARNS("Shader") << "Failed to load fragment shader " << shaders[i].first << LL_ENDL;
            return shaders[i].first;
        }
    }

    return std::string();
}

// <FS> ray traced reflections (rtReflection in shadowUtil.glsl): the programs that use it see them as the permutation LL_RT_REFLECTIONS. The device is not up yet when the water is configured
// (LLVkBridge::rtShadowMode() still says 0), so the launcher's environment is the early answer: ray shadows on and the material table on.
static bool rt_reflections_possible()
{
    const char* rt_env = getenv("LL_VK_RT_SHADOWS");
    const char* rt_materials_env = getenv("LL_VK_RT_MATERIALS");
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
        gWaterProgram.unload();
        gUnderWaterProgram.unload();
        return true;
    }

    if (success)
    {
        // load water shader
        gWaterProgram.mName = "Water Shader";
        gWaterProgram.mFeatures.calculatesAtmospherics = true;
        gWaterProgram.mFeatures.hasAtmospherics = true;
        gWaterProgram.mFeatures.hasGamma = true;
        gWaterProgram.mFeatures.hasSrgb = true;
        gWaterProgram.mFeatures.hasReflectionProbes = true;
        gWaterProgram.mFeatures.hasTonemap = true;
        gWaterProgram.mFeatures.hasShadows = use_sun_shadow;
        gWaterProgram.mShaderFiles.clear();
        gWaterProgram.mShaderFiles.push_back(make_pair("environment/waterV.glsl", ShaderStage::Vertex));
        gWaterProgram.mShaderFiles.push_back(make_pair("environment/waterF.glsl", ShaderStage::Fragment));
        // (the device is not up yet when the water is configured: LLVkBridge::rtShadowMode() still says 0; the launcher's environment is the early answer. The ray traced reflection (rtReflection in shadowUtil.glsl) needs the scene and the material table; the program's own files only see the permutations, the global defines such as SUN_SHADOW reach the library files only)
        const bool rt_water_reflections_possible = use_sun_shadow && rt_reflections_possible();
        gWaterProgram.clearPermutations();
        if (rt_water_reflections_possible)
        {
            gWaterProgram.addPermutation("LL_RT_REFLECTIONS", "1");      // after clearPermutations: it wipes them
        }
        if (LLPipeline::sRenderTransparentWater)
        {
            gWaterProgram.addPermutation("TRANSPARENT_WATER", "1");
        }

        if (use_sun_shadow)
        {
            gWaterProgram.addPermutation("HAS_SUN_SHADOW", "1");
        }

        gWaterProgram.mShaderGroup = LLShaderProgram::SG_WATER;
        gWaterProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        success = gWaterProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        //load under water vertex shader
        gUnderWaterProgram.mName = "Underwater Shader";
        gUnderWaterProgram.mFeatures.calculatesAtmospherics = true;
        gUnderWaterProgram.mFeatures.hasAtmospherics = true;
        gUnderWaterProgram.mShaderFiles.clear();
        gUnderWaterProgram.mShaderFiles.push_back(make_pair("environment/waterV.glsl", ShaderStage::Vertex));
        gUnderWaterProgram.mShaderFiles.push_back(make_pair("environment/underWaterF.glsl", ShaderStage::Fragment));
        gUnderWaterProgram.mShaderLevel = mShaderLevel[SHADER_WATER];
        gUnderWaterProgram.mShaderGroup = LLShaderProgram::SG_WATER;
        gUnderWaterProgram.clearPermutations();
        if (LLPipeline::sRenderTransparentWater)
        {
            gUnderWaterProgram.addPermutation("TRANSPARENT_WATER", "1");
        }
        success = gUnderWaterProgram.createShader();
        llassert(success);
    }

    /// Keep track of water shader levels
    if (gWaterProgram.mShaderLevel != mShaderLevel[SHADER_WATER]
        || gUnderWaterProgram.mShaderLevel != mShaderLevel[SHADER_WATER])
    {
        mShaderLevel[SHADER_WATER] = llmin(gWaterProgram.mShaderLevel, gUnderWaterProgram.mShaderLevel);
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
        gGlowProgram.mName = "Glow Shader (Post)";
        gGlowProgram.mShaderFiles.clear();
        gGlowProgram.mShaderFiles.push_back(make_pair("effects/glowV.glsl", ShaderStage::Vertex));
        gGlowProgram.mShaderFiles.push_back(make_pair("effects/glowF.glsl", ShaderStage::Fragment));
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
        gGlowExtractProgram.mShaderFiles.clear();
        gGlowExtractProgram.mShaderFiles.push_back(make_pair("effects/glowExtractV.glsl", ShaderStage::Vertex));
        gGlowExtractProgram.mShaderFiles.push_back(make_pair("effects/glowExtractF.glsl", ShaderStage::Fragment));
        gGlowExtractProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];

        if (use_glow_noise)
        {
            gGlowExtractProgram.addPermutation("HAS_NOISE", "1");
        }

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
        gPostVignetteProgram.mShaderFiles.clear();
        gPostVignetteProgram.mShaderFiles.push_back(make_pair("post/exoPostBaseV.glsl", ShaderStage::Vertex));
        gPostVignetteProgram.mShaderFiles.push_back(make_pair("post/exoVignetteF.glsl", ShaderStage::Fragment));
        gPostVignetteProgram.mShaderLevel = mShaderLevel[SHADER_EFFECT];
        success = gPostVignetteProgram.createShader();
    }
// </FS:CR>
// <FS:Beq> Add Snapshot framing shader
    if (success)
    {
        gPostSnapshotFrameProgram.mName = "Snapshot Frame Post";
        gPostSnapshotFrameProgram.mShaderFiles.clear();
        gPostSnapshotFrameProgram.mShaderFiles.push_back(make_pair("post/exoPostBaseV.glsl", ShaderStage::Vertex));
        gPostSnapshotFrameProgram.mShaderFiles.push_back(make_pair("post/snapshotFrameF.glsl", ShaderStage::Fragment));
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

    if (mShaderLevel[SHADER_DEFERRED] == 0)
    {
        gDeferredTreeProgram.unload();
        gDeferredTreeShadowProgram.unload();
        gDeferredSkinnedTreeShadowProgram.unload();
        gDeferredDiffuseProgram.unload();
        gDeferredSkinnedDiffuseProgram.unload();
        gDeferredDiffuseAlphaMaskProgram.unload();
        gDeferredSkinnedDiffuseAlphaMaskProgram.unload();
        gDeferredNonIndexedDiffuseAlphaMaskProgram.unload();
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.unload();
        gDeferredBumpProgram.unload();
        gDeferredSkinnedBumpProgram.unload();
        gDeferredImpostorProgram.unload();
        gDeferredTerrainProgram.unload();
        gDeferredLightProgram.unload();
        for (U32 i = 0; i < LL_DEFERRED_MULTI_LIGHT_COUNT; ++i)
        {
            gDeferredMultiLightProgram[i].unload();
        }
        gDeferredSpotLightProgram.unload();
        gDeferredMultiSpotLightProgram.unload();
        gDeferredSunProgram.unload();
        gDeferredBlurLightProgram.unload();
        gDeferredSoftenProgram.unload();
        gDeferredShadowProgram.unload();
        gDeferredSkinnedShadowProgram.unload();
        gDeferredShadowCubeProgram.unload();
        gDeferredShadowPullProgram.unload();
        gDeferredShadowAlphaPullProgram.unload();
        gDeferredShadowSkinPullProgram.unload();
        gDeferredDiffusePullProgram.unload();
        gDeferredAlphaPullProgram.unload();
        gDeferredFullbrightAlphaPullProgram.unload();
        gDeferredPBRAlphaPullProgram.unload();
        gAlphaPbrPullReady = false;
        gAlphaPullReady = false;
        for (LLShaderProgram& pull : gDeferredMaterialPullProgram)
        {
            pull.unload();
        }
        gDeferredDiffusePullSkinProgram.unload();
        for (LLShaderProgram& pull : gDeferredMaterialPullSkinProgram)
        {
            pull.unload();
        }
        gDeferredShadowAlphaMaskProgram.unload();
        gDeferredSkinnedShadowAlphaMaskProgram.unload();
        gDeferredShadowGLTFAlphaMaskProgram.unload();
        gDeferredSkinnedShadowGLTFAlphaMaskProgram.unload();
        gDeferredShadowFullbrightAlphaMaskProgram.unload();
        gDeferredSkinnedShadowFullbrightAlphaMaskProgram.unload();
        gDeferredAvatarShadowProgram.unload();
        gDeferredAvatarAlphaShadowProgram.unload();
        gDeferredAvatarAlphaMaskShadowProgram.unload();
        gDeferredAvatarProgram.unload();
        gDeferredAvatarAlphaProgram.unload();
        gDeferredAlphaProgram.unload();
        gHUDAlphaProgram.unload();
        gDeferredSkinnedAlphaProgram.unload();
        gDeferredFullbrightProgram.unload();
        gHUDFullbrightProgram.unload();
        gDeferredFullbrightAlphaMaskProgram.unload();
        gHUDFullbrightAlphaMaskProgram.unload();
        gDeferredFullbrightAlphaMaskAlphaProgram.unload();
        gHUDFullbrightAlphaMaskAlphaProgram.unload();
        gDeferredEmissiveProgram.unload();
        gDeferredSkinnedEmissiveProgram.unload();
        gDeferredAvatarEyesProgram.unload();
        gDeferredPostProgram.unload();
        gDeferredCoFProgram.unload();
        gDeferredDoFCombineProgram.unload();
        gExposureProgram.unload();
        gExposureProgramNoFade.unload();
        gLuminanceProgram.unload();
        gMotionVectorProgram.unload();
        gAvatarVelocityProgram.unload();
        gAvatarBodyVelocityProgram.unload();
        gAvatarVelocityPullProgram.unload();
        gObjectVelocityPullProgram.unload();
        gShadowAccumProgram.unload();
        gRRGuidesProgram.unload();
        gRRReflMotionProgram.unload();
        gRRHitDistProgram.unload();
        gAvatarVelocityDebugProgram.unload();
        gRtMaterialDebugProgram.unload();
        gDeferredPostGammaCorrectProgram.unload();
        gLegacyPostGammaCorrectProgram.unload();
        gDeferredPostTonemapProgram.unload();
        gNoPostTonemapProgram.unload();
        gDeferredPostTonemapGammaCorrectProgram.unload();
        gNoPostTonemapGammaCorrectProgram.unload();
        gDeferredPostTonemapLegacyGammaCorrectProgram.unload();
        gNoPostTonemapLegacyGammaCorrectProgram.unload();

        for (auto i = 0; i < 4; ++i)
        {
            gFXAAProgram[i].unload();
            gSMAAEdgeDetectProgram[i].unload();
            gSMAABlendWeightsProgram[i].unload();
            gSMAANeighborhoodBlendProgram[i].unload();
        }
        gCASProgram.unload();
        gCASLegacyGammaProgram.unload();
        gEnvironmentMapProgram.unload();
        gDeferredWLSkyProgram.unload();
        gDeferredWLCloudProgram.unload();
        gDeferredWLSunProgram.unload();
        gDeferredWLMoonProgram.unload();
        gDeferredStarProgram.unload();
        gDeferredFullbrightShinyProgram.unload();
        gHUDFullbrightShinyProgram.unload();
        gDeferredSkinnedFullbrightShinyProgram.unload();
        gDeferredSkinnedFullbrightProgram.unload();
        gDeferredSkinnedFullbrightAlphaMaskProgram.unload();
        gDeferredSkinnedFullbrightAlphaMaskAlphaProgram.unload();

        gDeferredHighlightProgram.unload();

        gNormalMapGenProgram.unload();
        gDeferredGenBrdfLutProgram.unload();
        gDeferredBufferVisualProgram.unload();

        for (U32 i = 0; i < LLMaterial::SHADER_COUNT*2; ++i)
        {
            gDeferredMaterialProgram[i].unload();
        }

        gHUDPBROpaqueProgram.unload();
        gPBRGlowProgram.unload();
        gDeferredPBROpaqueProgram.unload();
        gGLTFPBRMetallicRoughnessProgram.unload();
        gDeferredSkinnedPBROpaqueProgram.unload();
        gDeferredPBRAlphaProgram.unload();
        gDeferredSkinnedPBRAlphaProgram.unload();
        for (U32 paint_type = 0; paint_type < TERRAIN_PAINT_TYPE_COUNT; ++paint_type)
        {
            gDeferredPBRTerrainProgram[paint_type].unload();
        }

        // [RLVa:KB] - @setsphere
        gRlvSphereProgram.unload();
        // [/RLVa:KB]
        return true;
    }

    bool success = true;

    if (success)
    {
        gDeferredHighlightProgram.mName = "Deferred Highlight Shader";
        gDeferredHighlightProgram.mShaderFiles.clear();
        gDeferredHighlightProgram.mShaderFiles.push_back(make_pair("interface/highlightV.glsl", ShaderStage::Vertex));
        gDeferredHighlightProgram.mShaderFiles.push_back(make_pair("deferred/highlightF.glsl", ShaderStage::Fragment));
        gDeferredHighlightProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        add_common_permutations(&gDeferredHighlightProgram);
        success = gDeferredHighlightProgram.createShader();
    }

    if (success)
    {
        gDeferredDiffuseProgram.mName = "Deferred Diffuse Shader";
        gDeferredDiffuseProgram.mFeatures.hasSrgb = true;
        gDeferredDiffuseProgram.mShaderFiles.clear();
        gDeferredDiffuseProgram.mShaderFiles.push_back(make_pair("deferred/diffuseV.glsl", ShaderStage::Vertex));
        gDeferredDiffuseProgram.mShaderFiles.push_back(make_pair("deferred/diffuseIndexedF.glsl", ShaderStage::Fragment));
        gDeferredDiffuseProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredDiffuseProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredDiffuseProgram);
        success = make_rigged_variant(gDeferredDiffuseProgram, gDeferredSkinnedDiffuseProgram);
        success = success && gDeferredDiffuseProgram.createShader();
    }

    if (success)
    {
        gDeferredDiffuseAlphaMaskProgram.mName = "Deferred Diffuse Alpha Mask Shader";
        gDeferredDiffuseAlphaMaskProgram.mShaderFiles.clear();
        gDeferredDiffuseAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/diffuseV.glsl", ShaderStage::Vertex));
        gDeferredDiffuseAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/diffuseAlphaMaskIndexedF.glsl", ShaderStage::Fragment));
        gDeferredDiffuseAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredDiffuseAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredDiffuseAlphaMaskProgram);
        success = make_rigged_variant(gDeferredDiffuseAlphaMaskProgram, gDeferredSkinnedDiffuseAlphaMaskProgram);
        success = success && gDeferredDiffuseAlphaMaskProgram.createShader();
    }

    if (success)
    {
        gDeferredNonIndexedDiffuseAlphaMaskProgram.mName = "Deferred Diffuse Non-Indexed Alpha Mask Shader";
        gDeferredNonIndexedDiffuseAlphaMaskProgram.mShaderFiles.clear();
        gDeferredNonIndexedDiffuseAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/diffuseV.glsl", ShaderStage::Vertex));
        gDeferredNonIndexedDiffuseAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/diffuseAlphaMaskF.glsl", ShaderStage::Fragment));
        gDeferredNonIndexedDiffuseAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredNonIndexedDiffuseAlphaMaskProgram);
        success = gDeferredNonIndexedDiffuseAlphaMaskProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mName = "Deferred Diffuse Non-Indexed Alpha Mask No Color Shader";
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mShaderFiles.clear();
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mShaderFiles.push_back(make_pair("deferred/diffuseNoColorV.glsl", ShaderStage::Vertex));
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mShaderFiles.push_back(make_pair("deferred/diffuseAlphaMaskNoColorF.glsl", ShaderStage::Fragment));
        gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram);
        success = gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredBumpProgram.mName = "Deferred Bump Shader";
        gDeferredBumpProgram.mShaderFiles.clear();
        gDeferredBumpProgram.mShaderFiles.push_back(make_pair("deferred/bumpV.glsl", ShaderStage::Vertex));
        gDeferredBumpProgram.mShaderFiles.push_back(make_pair("deferred/bumpF.glsl", ShaderStage::Fragment));
        gDeferredBumpProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gDeferredBumpProgram);
        success = make_rigged_variant(gDeferredBumpProgram, gDeferredSkinnedBumpProgram);
        success = success && gDeferredBumpProgram.createShader();
        llassert(success);
    }

    gDeferredMaterialProgram[1].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[5].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[9].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[13].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[1+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[5+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[9+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = false;
    gDeferredMaterialProgram[13+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = false;

    for (U32 i = 0; i < LLMaterial::SHADER_COUNT*2; ++i)
    {
        if (success)
        {
            bool has_skin = i & 0x10;

            if (!has_skin)
            {
                mShaderList.push_back(&gDeferredMaterialProgram[i]);
                gDeferredMaterialProgram[i].mName = llformat("Material Shader %d", i);
            }
            else
            {
                gDeferredMaterialProgram[i].mName = llformat("Skinned Material Shader %d", i);
            }

            U32 alpha_mode = i & 0x3;

            gDeferredMaterialProgram[i].mShaderFiles.clear();
            gDeferredMaterialProgram[i].mShaderFiles.push_back(make_pair("deferred/materialV.glsl", ShaderStage::Vertex));
            gDeferredMaterialProgram[i].mShaderFiles.push_back(make_pair("deferred/materialF.glsl", ShaderStage::Fragment));
            gDeferredMaterialProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];

            gDeferredMaterialProgram[i].clearPermutations();

            bool has_normal_map   = (i & 0x8) > 0;
            bool has_specular_map = (i & 0x4) > 0;

            if (has_normal_map)
            {
                gDeferredMaterialProgram[i].addPermutation("HAS_NORMAL_MAP", "1");
            }

            if (has_specular_map)
            {
                gDeferredMaterialProgram[i].addPermutation("HAS_SPECULAR_MAP", "1");
            }

            gDeferredMaterialProgram[i].addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", alpha_mode));

            if (alpha_mode != 0)
            {
                gDeferredMaterialProgram[i].mFeatures.hasAlphaMask = true;
                gDeferredMaterialProgram[i].addPermutation("HAS_ALPHA_MASK", "1");
            }

            if (use_sun_shadow)
            {
                gDeferredMaterialProgram[i].addPermutation("HAS_SUN_SHADOW", "1");
            }

            add_common_permutations(&gDeferredMaterialProgram[i]);

            gDeferredMaterialProgram[i].mFeatures.hasSrgb = true;
            gDeferredMaterialProgram[i].mFeatures.calculatesAtmospherics = true;
            gDeferredMaterialProgram[i].mFeatures.hasAtmospherics = true;
            gDeferredMaterialProgram[i].mFeatures.hasGamma = true;
            gDeferredMaterialProgram[i].mFeatures.hasShadows = use_sun_shadow;
            gDeferredMaterialProgram[i].mFeatures.hasReflectionProbes = true;

            if (has_skin)
            {
                gDeferredMaterialProgram[i].addPermutation("HAS_SKIN", "1");
                gDeferredMaterialProgram[i].mFeatures.hasObjectSkinning = true;
            }
            else
            {
                gDeferredMaterialProgram[i].mRiggedVariant = &gDeferredMaterialProgram[i + 0x10];
            }

            success = gDeferredMaterialProgram[i].createShader();
            llassert(success);
        }
    }

    gDeferredMaterialProgram[1].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[5].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[9].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[13].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[1+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[5+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[9+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = true;
    gDeferredMaterialProgram[13+LLMaterial::SHADER_COUNT].mFeatures.hasLighting = true;

    if (success)
    {
        gDeferredPBROpaqueProgram.mName = "Deferred PBR Opaque Shader";
        gDeferredPBROpaqueProgram.mFeatures.hasSrgb = true;

        gDeferredPBROpaqueProgram.mShaderFiles.clear();
        gDeferredPBROpaqueProgram.mShaderFiles.push_back(make_pair("deferred/pbropaqueV.glsl", ShaderStage::Vertex));
        gDeferredPBROpaqueProgram.mShaderFiles.push_back(make_pair("deferred/pbropaqueF.glsl", ShaderStage::Fragment));
        gDeferredPBROpaqueProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredPBROpaqueProgram.clearPermutations();

        add_common_permutations(&gDeferredPBROpaqueProgram);

        success = make_rigged_variant(gDeferredPBROpaqueProgram, gDeferredSkinnedPBROpaqueProgram);
        if (success)
        {
            success = gDeferredPBROpaqueProgram.createShader();
        }
        llassert(success);
    }

    if (gSavedSettings.getBOOL("GLTFEnabled"))
    {
        if (success)
        {
            gGLTFPBRMetallicRoughnessProgram.mName = "GLTF PBR Metallic Roughness Shader";
            gGLTFPBRMetallicRoughnessProgram.mFeatures.hasSrgb = true;

            gGLTFPBRMetallicRoughnessProgram.mShaderFiles.clear();
            gGLTFPBRMetallicRoughnessProgram.mShaderFiles.push_back(make_pair("gltf/pbrmetallicroughnessV.glsl", ShaderStage::Vertex));
            gGLTFPBRMetallicRoughnessProgram.mShaderFiles.push_back(make_pair("gltf/pbrmetallicroughnessF.glsl", ShaderStage::Fragment));
            gGLTFPBRMetallicRoughnessProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            gGLTFPBRMetallicRoughnessProgram.clearPermutations();

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
        gPBRGlowProgram.mName = " PBR Glow Shader";
        gPBRGlowProgram.mFeatures.hasSrgb = true;
        gPBRGlowProgram.mShaderFiles.clear();
        gPBRGlowProgram.mShaderFiles.push_back(make_pair("deferred/pbrglowV.glsl", ShaderStage::Vertex));
        gPBRGlowProgram.mShaderFiles.push_back(make_pair("deferred/pbrglowF.glsl", ShaderStage::Fragment));
        gPBRGlowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gPBRGlowProgram);

        success = make_rigged_variant(gPBRGlowProgram, gPBRGlowSkinnedProgram);
        if (success)
        {
            success = gPBRGlowProgram.createShader();
        }
        llassert(success);
    }

    if (success)
    {
        gHUDPBROpaqueProgram.mName = "HUD PBR Opaque Shader";
        gHUDPBROpaqueProgram.mFeatures.hasSrgb = true;
        gHUDPBROpaqueProgram.mShaderFiles.clear();
        gHUDPBROpaqueProgram.mShaderFiles.push_back(make_pair("deferred/pbropaqueV.glsl", ShaderStage::Vertex));
        gHUDPBROpaqueProgram.mShaderFiles.push_back(make_pair("deferred/pbropaqueF.glsl", ShaderStage::Fragment));
        gHUDPBROpaqueProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gHUDPBROpaqueProgram.clearPermutations();
        gHUDPBROpaqueProgram.addPermutation("IS_HUD", "1");

        add_common_permutations(&gHUDPBROpaqueProgram);

        success = gHUDPBROpaqueProgram.createShader();

        llassert(success);
    }

    if (success)
    {
        LLShaderProgram* shader = &gDeferredPBRAlphaProgram;
        shader->mName = "Deferred PBR Alpha Shader";

        shader->mFeatures.calculatesLighting = false;
        shader->mFeatures.hasLighting = false;
        shader->mFeatures.isAlphaLighting = true;
        shader->mFeatures.hasSrgb = true;
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasAtmospherics = true;
        shader->mFeatures.hasGamma = true;
        shader->mFeatures.hasShadows = use_sun_shadow;
        shader->mFeatures.isDeferred = true; // include deferredUtils
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];

        shader->mShaderFiles.clear();
        shader->mShaderFiles.push_back(make_pair("deferred/pbralphaV.glsl", ShaderStage::Vertex));
        shader->mShaderFiles.push_back(make_pair("deferred/pbralphaF.glsl", ShaderStage::Fragment));

        shader->clearPermutations();

        U32 alpha_mode = LLMaterial::DIFFUSE_ALPHA_MODE_BLEND;
        shader->addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", alpha_mode));
        shader->addPermutation("HAS_NORMAL_MAP", "1");
        shader->addPermutation("HAS_SPECULAR_MAP", "1"); // PBR: Packed: Occlusion, Metal, Roughness
        shader->addPermutation("HAS_EMISSIVE_MAP", "1");
        shader->addPermutation("USE_VERTEX_COLOR", "1");

        add_common_permutations(shader);

        if (use_sun_shadow)
        {
            shader->addPermutation("HAS_SUN_SHADOW", "1");
        }

        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = make_rigged_variant(*shader, gDeferredSkinnedPBRAlphaProgram);
        if (success)
        {
            success = shader->createShader();
        }
        llassert(success);

        // Alpha Shader Hack
        // See: LLRender::syncMatrices()
        shader->mFeatures.calculatesLighting = true;
        shader->mFeatures.hasLighting = true;

        shader->mRiggedVariant->mFeatures.calculatesLighting = true;
        shader->mRiggedVariant->mFeatures.hasLighting = true;
    }

    if (success)
    {
        LLShaderProgram* shader = &gHUDPBRAlphaProgram;
        shader->mName = "HUD PBR Alpha Shader";

        shader->mFeatures.hasSrgb = true;

        shader->mShaderFiles.clear();
        shader->mShaderFiles.push_back(make_pair("deferred/pbralphaV.glsl", ShaderStage::Vertex));
        shader->mShaderFiles.push_back(make_pair("deferred/pbralphaF.glsl", ShaderStage::Fragment));

        shader->clearPermutations();

        shader->addPermutation("IS_HUD", "1");

        add_common_permutations(shader);

        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = shader->createShader();
        llassert(success);
    }

    if (success)
    {
        S32 detail = gSavedSettings.getS32("RenderTerrainPBRDetail");
        detail = llclamp(detail, TERRAIN_PBR_DETAIL_MIN, TERRAIN_PBR_DETAIL_MAX);
        const S32 mapping = clamp_terrain_mapping(gSavedSettings.getS32("RenderTerrainPBRPlanarSampleCount"));
        for (U32 paint_type = 0; paint_type < TERRAIN_PAINT_TYPE_COUNT; ++paint_type)
        {
            LLShaderProgram* shader = &gDeferredPBRTerrainProgram[paint_type];
            shader->mName = llformat("Deferred PBR Terrain Shader %d %s %s",
                    detail,
                    (paint_type == TERRAIN_PAINT_TYPE_PBR_PAINTMAP ? "paintmap" : "heightmap-with-noise"),
                    (mapping == 1 ? "flat" : "triplanar"));
            shader->mFeatures.hasSrgb = true;
            shader->mFeatures.isAlphaLighting = true;
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.hasAtmospherics = true;
            shader->mFeatures.hasGamma = true;
            shader->mFeatures.hasTransport = true;
            shader->mFeatures.isPBRTerrain = true;

            shader->mShaderFiles.clear();
            shader->mShaderFiles.push_back(make_pair("deferred/pbrterrainV.glsl", ShaderStage::Vertex));
            shader->mShaderFiles.push_back(make_pair("deferred/pbrterrainF.glsl", ShaderStage::Fragment));
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            shader->addPermutation("TERRAIN_PBR_DETAIL", llformat("%d", detail));
            shader->addPermutation("TERRAIN_PAINT_TYPE", llformat("%d", paint_type));
            shader->addPermutation("TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT", llformat("%d", mapping));

            add_common_permutations(shader);

            success = success && shader->createShader();
            llassert(success);
        }
    }

    if (success)
    {
        gDeferredTreeProgram.mName = "Deferred Tree Shader";
        gDeferredTreeProgram.mShaderFiles.clear();
        gDeferredTreeProgram.mShaderFiles.push_back(make_pair("deferred/treeV.glsl", ShaderStage::Vertex));
        gDeferredTreeProgram.mShaderFiles.push_back(make_pair("deferred/treeF.glsl", ShaderStage::Fragment));
        gDeferredTreeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredTreeProgram);

        success = gDeferredTreeProgram.createShader();
    }

    if (success)
    {
        gDeferredTreeShadowProgram.mName = "Deferred Tree Shadow Shader";
        gDeferredTreeShadowProgram.mShaderFiles.clear();
        gDeferredTreeShadowProgram.mShaderFiles.push_back(make_pair("deferred/treeShadowV.glsl", ShaderStage::Vertex));
        gDeferredTreeShadowProgram.mShaderFiles.push_back(make_pair("deferred/treeShadowF.glsl", ShaderStage::Fragment));
        gDeferredTreeShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredTreeShadowProgram.mRiggedVariant = &gDeferredSkinnedTreeShadowProgram;
        success = gDeferredTreeShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredSkinnedTreeShadowProgram.mName = "Deferred Skinned Tree Shadow Shader";
        gDeferredSkinnedTreeShadowProgram.mShaderFiles.clear();
        gDeferredSkinnedTreeShadowProgram.mFeatures.hasObjectSkinning = true;
        gDeferredSkinnedTreeShadowProgram.mShaderFiles.push_back(make_pair("deferred/treeShadowSkinnedV.glsl", ShaderStage::Vertex));
        gDeferredSkinnedTreeShadowProgram.mShaderFiles.push_back(make_pair("deferred/treeShadowF.glsl", ShaderStage::Fragment));
        gDeferredSkinnedTreeShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredSkinnedTreeShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredImpostorProgram.mName = "Deferred Impostor Shader";
        gDeferredImpostorProgram.mFeatures.hasSrgb = true;
        gDeferredImpostorProgram.mShaderFiles.clear();
        gDeferredImpostorProgram.mShaderFiles.push_back(make_pair("deferred/impostorV.glsl", ShaderStage::Vertex));
        gDeferredImpostorProgram.mShaderFiles.push_back(make_pair("deferred/impostorF.glsl", ShaderStage::Fragment));
        gDeferredImpostorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredImpostorProgram);

        success = gDeferredImpostorProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredLightProgram.mName = "Deferred Light Shader";
        gDeferredLightProgram.mFeatures.isDeferred = true;
        gDeferredLightProgram.mFeatures.hasFullGBuffer = true;
        gDeferredLightProgram.mFeatures.hasShadows = true;
        gDeferredLightProgram.mFeatures.hasSrgb = true;

        gDeferredLightProgram.mShaderFiles.clear();
        gDeferredLightProgram.mShaderFiles.push_back(make_pair("deferred/pointLightV.glsl", ShaderStage::Vertex));
        gDeferredLightProgram.mShaderFiles.push_back(make_pair("deferred/pointLightF.glsl", ShaderStage::Fragment));
        gDeferredLightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        gDeferredLightProgram.clearPermutations();

        add_common_permutations(&gDeferredLightProgram);

        success = gDeferredLightProgram.createShader();
        llassert(success);
    }

    for (U32 i = 0; i < LL_DEFERRED_MULTI_LIGHT_COUNT; i++)
    {
        if (success)
        {
            gDeferredMultiLightProgram[i].mName = llformat("Deferred MultiLight Shader %d", i);
            gDeferredMultiLightProgram[i].mFeatures.isDeferred = true;
            gDeferredMultiLightProgram[i].mFeatures.hasFullGBuffer = true;
            gDeferredMultiLightProgram[i].mFeatures.hasShadows = true;
            gDeferredMultiLightProgram[i].mFeatures.hasSrgb = true;

            gDeferredMultiLightProgram[i].clearPermutations();
            gDeferredMultiLightProgram[i].mShaderFiles.clear();
            gDeferredMultiLightProgram[i].mShaderFiles.push_back(make_pair("deferred/multiPointLightV.glsl", ShaderStage::Vertex));
            gDeferredMultiLightProgram[i].mShaderFiles.push_back(make_pair("deferred/multiPointLightF.glsl", ShaderStage::Fragment));
            gDeferredMultiLightProgram[i].mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            gDeferredMultiLightProgram[i].addPermutation("LIGHT_COUNT", llformat("%d", i+1));

            add_common_permutations(&gDeferredMultiLightProgram[i]);

            success = gDeferredMultiLightProgram[i].createShader();
            llassert(success);
        }
    }

    if (success)
    {
        gDeferredSpotLightProgram.mName = "Deferred SpotLight Shader";
        gDeferredSpotLightProgram.mShaderFiles.clear();
        gDeferredSpotLightProgram.mFeatures.hasSrgb = true;
        gDeferredSpotLightProgram.mFeatures.isDeferred = true;
        gDeferredSpotLightProgram.mFeatures.hasFullGBuffer = true;
        gDeferredSpotLightProgram.mFeatures.hasShadows = true;

        gDeferredSpotLightProgram.clearPermutations();
        gDeferredSpotLightProgram.mShaderFiles.push_back(make_pair("deferred/pointLightV.glsl", ShaderStage::Vertex));
        gDeferredSpotLightProgram.mShaderFiles.push_back(make_pair("deferred/spotLightF.glsl", ShaderStage::Fragment));
        gDeferredSpotLightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredSpotLightProgram);

        success = gDeferredSpotLightProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredMultiSpotLightProgram.mName = "Deferred MultiSpotLight Shader";
        gDeferredMultiSpotLightProgram.mFeatures.hasSrgb = true;
        gDeferredMultiSpotLightProgram.mFeatures.isDeferred = true;
        gDeferredMultiSpotLightProgram.mFeatures.hasFullGBuffer = true;
        gDeferredMultiSpotLightProgram.mFeatures.hasShadows = true;

        gDeferredMultiSpotLightProgram.clearPermutations();
        gDeferredMultiSpotLightProgram.addPermutation("MULTI_SPOTLIGHT", "1");
        gDeferredMultiSpotLightProgram.mShaderFiles.clear();
        gDeferredMultiSpotLightProgram.mShaderFiles.push_back(make_pair("deferred/multiPointLightV.glsl", ShaderStage::Vertex));
        gDeferredMultiSpotLightProgram.mShaderFiles.push_back(make_pair("deferred/spotLightF.glsl", ShaderStage::Fragment));
        gDeferredMultiSpotLightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredMultiSpotLightProgram);

        success = gDeferredMultiSpotLightProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        std::string fragment;
        bool use_ao = gSavedSettings.getBOOL("RenderDeferredSSAO");
        if (use_ao)
        {
            fragment = "deferred/sunLightSSAOF.glsl";
        }
        else
        {
            fragment = "deferred/sunLightF.glsl";
        }

        gDeferredSunProgram.mName = "Deferred Sun Shader";
        gDeferredSunProgram.mFeatures.isDeferred    = true;
        gDeferredSunProgram.mFeatures.hasShadows    = true;
        gDeferredSunProgram.mFeatures.hasAmbientOcclusion = use_ao;

        gDeferredSunProgram.mShaderFiles.clear();
        gDeferredSunProgram.mShaderFiles.push_back(make_pair("deferred/sunLightV.glsl", ShaderStage::Vertex));
        gDeferredSunProgram.mShaderFiles.push_back(make_pair(fragment, ShaderStage::Fragment));
        gDeferredSunProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredSunProgram);

        success = gDeferredSunProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredSunProbeProgram.mName = "Deferred Sun Probe Shader";
        gDeferredSunProbeProgram.mFeatures.isDeferred = true;
        gDeferredSunProbeProgram.mFeatures.hasShadows = true;

        gDeferredSunProbeProgram.mShaderFiles.clear();
        gDeferredSunProbeProgram.mShaderFiles.push_back(make_pair("deferred/sunLightV.glsl", ShaderStage::Vertex));
        gDeferredSunProbeProgram.mShaderFiles.push_back(make_pair("deferred/sunLightF.glsl", ShaderStage::Fragment));
        gDeferredSunProbeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredSunProbeProgram);

        success = gDeferredSunProbeProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredBlurLightProgram.mName = "Deferred Blur Light Shader";
        gDeferredBlurLightProgram.mFeatures.isDeferred = true;

        gDeferredBlurLightProgram.mShaderFiles.clear();
        gDeferredBlurLightProgram.mShaderFiles.push_back(make_pair("deferred/blurLightV.glsl", ShaderStage::Vertex));
        gDeferredBlurLightProgram.mShaderFiles.push_back(make_pair("deferred/blurLightF.glsl", ShaderStage::Fragment));
        gDeferredBlurLightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredBlurLightProgram);

        success = gDeferredBlurLightProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        for (int i = 0; i < 3 && success; ++i)
        {
            LLShaderProgram* shader = nullptr;
            bool rigged = (i == 1);
            bool hud = (i == 2);

            if (hud)
            {
                shader = &gHUDAlphaProgram;
                shader->mName = "HUD Alpha Shader";
            }
            else if (!rigged)
            {
                shader = &gDeferredAlphaProgram;
                shader->mName = "Deferred Alpha Shader";
                shader->mRiggedVariant = &gDeferredSkinnedAlphaProgram;
            }
            else
            {
                shader = &gDeferredSkinnedAlphaProgram;
                shader->mName = "Skinned Deferred Alpha Shader";
                shader->mFeatures.hasObjectSkinning = true;
            }

            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;
            shader->mFeatures.isAlphaLighting = true;
            shader->mFeatures.hasSrgb = true;
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.hasAtmospherics = true;
            shader->mFeatures.hasGamma = true;
            shader->mFeatures.hasShadows = use_sun_shadow;
            shader->mFeatures.hasReflectionProbes = true;
            shader->mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;

            shader->mShaderFiles.clear();
            shader->mShaderFiles.push_back(make_pair("deferred/alphaV.glsl", ShaderStage::Vertex));
            shader->mShaderFiles.push_back(make_pair("deferred/alphaF.glsl", ShaderStage::Fragment));

            shader->clearPermutations();
            shader->addPermutation("USE_VERTEX_COLOR", "1");
            shader->addPermutation("HAS_ALPHA_MASK", "1");
            shader->addPermutation("USE_INDEXED_TEX", "1");
            if (use_sun_shadow)
            {
                shader->addPermutation("HAS_SUN_SHADOW", "1");
            }

            add_common_permutations(shader);

            if (rigged)
            {
                shader->addPermutation("HAS_SKIN", "1");
            }

            if (hud)
            {
                shader->addPermutation("IS_HUD", "1");
            }

            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];

            success = shader->createShader();
            llassert(success);

            // Hack
            shader->mFeatures.calculatesLighting = true;
            shader->mFeatures.hasLighting = true;
        }
    }

    if (success)
    {
        LLShaderProgram* shaders[] = {
            &gDeferredAlphaImpostorProgram,
            &gDeferredSkinnedAlphaImpostorProgram
        };

        for (int i = 0; i < 2 && success; ++i)
        {
            bool rigged = i == 1;
            LLShaderProgram* shader = shaders[i];

            shader->mName = rigged ? "Skinned Deferred Alpha Impostor Shader" : "Deferred Alpha Impostor Shader";

            // Begin Hack
            shader->mFeatures.calculatesLighting = false;
            shader->mFeatures.hasLighting = false;

            shader->mFeatures.hasSrgb = true;
            shader->mFeatures.isAlphaLighting = true;
            shader->mFeatures.hasShadows = use_sun_shadow;
            shader->mFeatures.hasReflectionProbes = true;
            shader->mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;

            shader->mShaderFiles.clear();
            shader->mShaderFiles.push_back(make_pair("deferred/alphaV.glsl", ShaderStage::Vertex));
            shader->mShaderFiles.push_back(make_pair("deferred/alphaF.glsl", ShaderStage::Fragment));

            shader->clearPermutations();
            shader->addPermutation("USE_INDEXED_TEX", "1");
            shader->addPermutation("FOR_IMPOSTOR", "1");
            shader->addPermutation("HAS_ALPHA_MASK", "1");
            shader->addPermutation("USE_VERTEX_COLOR", "1");
            if (rigged)
            {
                shader->mFeatures.hasObjectSkinning = true;
                shader->addPermutation("HAS_SKIN", "1");
            }

            if (use_sun_shadow)
            {
                shader->addPermutation("HAS_SUN_SHADOW", "1");
            }

            add_common_permutations(shader);

            shader->mRiggedVariant = &gDeferredSkinnedAlphaImpostorProgram;
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!rigged)
            {
                shader->mRiggedVariant = shaders[1];
            }
            success = shader->createShader();
            llassert(success);

            // End Hack
            shader->mFeatures.calculatesLighting = true;
            shader->mFeatures.hasLighting = true;
        }
    }

    // <FS> S5a (doc/vulkan/S5-design.md): the lit and the fullbright alpha programs with the vertices read by address and the diffuse texture from the bindless table. Optional: without them the alpha
    // pass stays on the old path.
    gAlphaPullReady = false;
    if (success && LLVkBridge::shadowAlphaPacketsAvailable())
    {
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
        {
            const bool fullbright = i == 1;
            LLShaderProgram* shader = fullbright ? &gDeferredFullbrightAlphaPullProgram : &gDeferredAlphaPullProgram;
            shader->mName = fullbright ? "Deferred Fullbright Alpha Pull Shader" : "Deferred Alpha Pull Shader";
            shader->mFeatures.calculatesAtmospherics = true;
            shader->mFeatures.hasGamma = true;
            shader->mFeatures.hasSrgb = true;
            shader->mShaderFiles.clear();
            shader->clearPermutations();
            if (fullbright)
            {
                shader->mFeatures.hasAtmospherics = true;
                shader->mFeatures.isDeferred = true;
                shader->mShaderFiles.push_back(make_pair("deferred/alphaPullV.glsl", ShaderStage::Vertex));
                shader->mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
                shader->addPermutation("HAS_ALPHA_MASK", "1");
                shader->addPermutation("IS_ALPHA", "1");
                shader->addPermutation("IS_FULLBRIGHT_PULL", "1");
            }
            else
            {
                shader->mFeatures.calculatesLighting = false;
                shader->mFeatures.hasLighting = false;
                shader->mFeatures.isAlphaLighting = true;
                shader->mFeatures.hasAtmospherics = true;
                shader->mFeatures.hasShadows = use_sun_shadow;
                shader->mFeatures.hasReflectionProbes = true;
                shader->mShaderFiles.push_back(make_pair("deferred/alphaPullV.glsl", ShaderStage::Vertex));
                shader->mShaderFiles.push_back(make_pair("deferred/alphaF.glsl", ShaderStage::Fragment));
                shader->addPermutation("USE_VERTEX_COLOR", "1");
                shader->addPermutation("HAS_ALPHA_MASK", "1");
                shader->addPermutation("USE_INDEXED_TEX", "1");
                if (use_sun_shadow)
                {
                    shader->addPermutation("HAS_SUN_SHADOW", "1");
                }
            }
            shader->addPermutation("LL_BINDLESS_DIFFUSE", "1");
            if (const char* dbg = getenv("LL_VK_SCENE_ALPHA_DBG"))
            {
                shader->addPermutation("ALPHA_PULL_DBG", dbg);       // (diagnostic, see alphaPullV.glsl)
            }
            if (const char* dbgf = getenv("LL_VK_SCENE_ALPHA_DBGF"))
            {
                shader->addPermutation("ALPHA_PULL_DBGF", dbgf);     // (diagnostic, see alphaF.glsl: the lit program only)
            }
            add_common_permutations(shader);
            shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            ok = shader->createShader();
            if (!ok)
            {
                LL_WARNS("ShadowPackets") << "the " << shader->mName << " could not be made: the alpha pass stays on the old path" << LL_ENDL;
            }
            else if (!fullbright)
            {
                shader->mFeatures.calculatesLighting = true;        // (as the ordinary alpha program: the hack that follows its createShader)
                shader->mFeatures.hasLighting = true;
            }
        }
        gAlphaPullReady = ok;
        if (!ok)
        {
            gDeferredAlphaPullProgram.unload();
            gDeferredFullbrightAlphaPullProgram.unload();
        }
    }

    // <FS> S5c: the PBR (glTF) blend program with the vertices read by address (pbrAlphaPullV + pbralphaF with LL_BINDLESS_PBR). Optional.
    gAlphaPbrPullReady = false;
    if (success && LLVkBridge::shadowAlphaPacketsAvailable())
    {
        LLShaderProgram* shader = &gDeferredPBRAlphaPullProgram;
        shader->mName = "Deferred PBR Alpha Pull Shader";
        shader->mFeatures.calculatesLighting = false;
        shader->mFeatures.hasLighting = false;
        shader->mFeatures.isAlphaLighting = true;
        shader->mFeatures.hasSrgb = true;
        shader->mFeatures.calculatesAtmospherics = true;
        shader->mFeatures.hasAtmospherics = true;
        shader->mFeatures.hasGamma = true;
        shader->mFeatures.hasShadows = use_sun_shadow;
        shader->mFeatures.isDeferred = true;
        shader->mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED];
        shader->mShaderFiles.clear();
        shader->mShaderFiles.push_back(make_pair("deferred/pbrAlphaPullV.glsl", ShaderStage::Vertex));
        shader->mShaderFiles.push_back(make_pair("deferred/pbralphaF.glsl", ShaderStage::Fragment));
        shader->clearPermutations();
        shader->addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", LLMaterial::DIFFUSE_ALPHA_MODE_BLEND));
        shader->addPermutation("HAS_NORMAL_MAP", "1");
        shader->addPermutation("HAS_SPECULAR_MAP", "1");
        shader->addPermutation("HAS_EMISSIVE_MAP", "1");
        shader->addPermutation("USE_VERTEX_COLOR", "1");
        shader->addPermutation("LL_BINDLESS_PBR", "1");
        add_common_permutations(shader);
        if (use_sun_shadow)
        {
            shader->addPermutation("HAS_SUN_SHADOW", "1");
        }
        shader->mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (getenv("LL_VK_SCENE_ALPHA_DBGF"))
        {
            shader->addPermutation("ALPHA_PULL_DBGF", getenv("LL_VK_SCENE_ALPHA_DBGF"));
        }
        if (shader->createShader())
        {
            shader->mFeatures.calculatesLighting = true;        // (as the ordinary PBR alpha program: "Alpha Shader Hack", see LLRender::syncMatrices())
            shader->mFeatures.hasLighting = true;
            gAlphaPbrPullReady = true;
        }
        else
        {
            LL_WARNS("ShadowPackets") << "the PBR alpha pull shader could not be made: the PBR alpha draws stay on the old path" << LL_ENDL;
            shader->unload();
        }
    }

    if (success)
    {
        gDeferredAvatarEyesProgram.mName = "Deferred Avatar Eyes Shader";
        gDeferredAvatarEyesProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredAvatarEyesProgram.mFeatures.hasGamma = true;
        gDeferredAvatarEyesProgram.mFeatures.hasAtmospherics = true;
        gDeferredAvatarEyesProgram.mFeatures.hasSrgb = true;
        gDeferredAvatarEyesProgram.mFeatures.hasShadows = true;

        gDeferredAvatarEyesProgram.mShaderFiles.clear();
        gDeferredAvatarEyesProgram.mShaderFiles.push_back(make_pair("deferred/avatarEyesV.glsl", ShaderStage::Vertex));
        gDeferredAvatarEyesProgram.mShaderFiles.push_back(make_pair("deferred/diffuseF.glsl", ShaderStage::Fragment));
        gDeferredAvatarEyesProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredAvatarEyesProgram);

        success = gDeferredAvatarEyesProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredFullbrightProgram.mName = "Deferred Fullbright Shader";
        gDeferredFullbrightProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredFullbrightProgram.mFeatures.hasGamma = true;
        gDeferredFullbrightProgram.mFeatures.hasAtmospherics = true;
        gDeferredFullbrightProgram.mFeatures.hasSrgb = true;
        gDeferredFullbrightProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredFullbrightProgram.mShaderFiles.clear();
        gDeferredFullbrightProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gDeferredFullbrightProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gDeferredFullbrightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredFullbrightProgram);

        success = make_rigged_variant(gDeferredFullbrightProgram, gDeferredSkinnedFullbrightProgram);
        success = gDeferredFullbrightProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHUDFullbrightProgram.mName = "HUD Fullbright Shader";
        gHUDFullbrightProgram.mFeatures.calculatesAtmospherics = true;
        gHUDFullbrightProgram.mFeatures.hasGamma = true;
        gHUDFullbrightProgram.mFeatures.hasAtmospherics = true;
        gHUDFullbrightProgram.mFeatures.hasSrgb = true;
        gHUDFullbrightProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gHUDFullbrightProgram.mShaderFiles.clear();
        gHUDFullbrightProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gHUDFullbrightProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gHUDFullbrightProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gHUDFullbrightProgram.clearPermutations();
        gHUDFullbrightProgram.addPermutation("IS_HUD", "1");

        add_common_permutations(&gHUDFullbrightProgram);

        success = gHUDFullbrightProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredFullbrightAlphaMaskProgram.mName = "Deferred Fullbright Alpha Masking Shader";
        gDeferredFullbrightAlphaMaskProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredFullbrightAlphaMaskProgram.mFeatures.hasGamma = true;
        gDeferredFullbrightAlphaMaskProgram.mFeatures.hasAtmospherics = true;
        gDeferredFullbrightAlphaMaskProgram.mFeatures.hasSrgb = true;
        gDeferredFullbrightAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredFullbrightAlphaMaskProgram.mShaderFiles.clear();
        gDeferredFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gDeferredFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gDeferredFullbrightAlphaMaskProgram.clearPermutations();
        gDeferredFullbrightAlphaMaskProgram.addPermutation("HAS_ALPHA_MASK","1");
        gDeferredFullbrightAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredFullbrightAlphaMaskProgram);

        success = make_rigged_variant(gDeferredFullbrightAlphaMaskProgram, gDeferredSkinnedFullbrightAlphaMaskProgram);
        success = success && gDeferredFullbrightAlphaMaskProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHUDFullbrightAlphaMaskProgram.mName = "HUD Fullbright Alpha Masking Shader";
        gHUDFullbrightAlphaMaskProgram.mFeatures.calculatesAtmospherics = true;
        gHUDFullbrightAlphaMaskProgram.mFeatures.hasGamma = true;
        gHUDFullbrightAlphaMaskProgram.mFeatures.hasAtmospherics = true;
        gHUDFullbrightAlphaMaskProgram.mFeatures.hasSrgb = true;
        gHUDFullbrightAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gHUDFullbrightAlphaMaskProgram.mShaderFiles.clear();
        gHUDFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gHUDFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gHUDFullbrightAlphaMaskProgram.clearPermutations();
        gHUDFullbrightAlphaMaskProgram.addPermutation("HAS_ALPHA_MASK", "1");
        gHUDFullbrightAlphaMaskProgram.addPermutation("IS_HUD", "1");

        add_common_permutations(&gHUDFullbrightAlphaMaskProgram);

        gHUDFullbrightAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gHUDFullbrightAlphaMaskProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredFullbrightAlphaMaskAlphaProgram.mName = "Deferred Fullbright Alpha Masking Alpha Shader";
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.hasGamma = true;
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.hasAtmospherics = true;
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.hasSrgb = true;
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.isDeferred = true;
        gDeferredFullbrightAlphaMaskAlphaProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredFullbrightAlphaMaskAlphaProgram.mShaderFiles.clear();
        gDeferredFullbrightAlphaMaskAlphaProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gDeferredFullbrightAlphaMaskAlphaProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gDeferredFullbrightAlphaMaskAlphaProgram.clearPermutations();
        gDeferredFullbrightAlphaMaskAlphaProgram.addPermutation("HAS_ALPHA_MASK", "1");
        gDeferredFullbrightAlphaMaskAlphaProgram.addPermutation("IS_ALPHA", "1");

        add_common_permutations(&gDeferredFullbrightAlphaMaskAlphaProgram);

        gDeferredFullbrightAlphaMaskAlphaProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = make_rigged_variant(gDeferredFullbrightAlphaMaskAlphaProgram, gDeferredSkinnedFullbrightAlphaMaskAlphaProgram);
        success = success && gDeferredFullbrightAlphaMaskAlphaProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHUDFullbrightAlphaMaskAlphaProgram.mName = "HUD Fullbright Alpha Masking Alpha Shader";
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.calculatesAtmospherics = true;
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.hasGamma = true;
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.hasAtmospherics = true;
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.hasSrgb = true;
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.isDeferred = true;
        gHUDFullbrightAlphaMaskAlphaProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gHUDFullbrightAlphaMaskAlphaProgram.mShaderFiles.clear();
        gHUDFullbrightAlphaMaskAlphaProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightV.glsl", ShaderStage::Vertex));
        gHUDFullbrightAlphaMaskAlphaProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightF.glsl", ShaderStage::Fragment));
        gHUDFullbrightAlphaMaskAlphaProgram.clearPermutations();
        gHUDFullbrightAlphaMaskAlphaProgram.addPermutation("HAS_ALPHA_MASK", "1");
        gHUDFullbrightAlphaMaskAlphaProgram.addPermutation("IS_ALPHA", "1");
        gHUDFullbrightAlphaMaskAlphaProgram.addPermutation("IS_HUD", "1");

        add_common_permutations(&gHUDFullbrightAlphaMaskAlphaProgram);

        gHUDFullbrightAlphaMaskAlphaProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = success && gHUDFullbrightAlphaMaskAlphaProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredFullbrightShinyProgram.mName = "Deferred FullbrightShiny Shader";
        gDeferredFullbrightShinyProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredFullbrightShinyProgram.mFeatures.hasAtmospherics = true;
        gDeferredFullbrightShinyProgram.mFeatures.hasGamma = true;
        gDeferredFullbrightShinyProgram.mFeatures.hasSrgb = true;
        gDeferredFullbrightShinyProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredFullbrightShinyProgram.mShaderFiles.clear();
        gDeferredFullbrightShinyProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightShinyV.glsl", ShaderStage::Vertex));
        gDeferredFullbrightShinyProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightShinyF.glsl", ShaderStage::Fragment));
        gDeferredFullbrightShinyProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredFullbrightShinyProgram.mFeatures.hasReflectionProbes = true;

        add_common_permutations(&gDeferredFullbrightShinyProgram);

        success = make_rigged_variant(gDeferredFullbrightShinyProgram, gDeferredSkinnedFullbrightShinyProgram);
        success = success && gDeferredFullbrightShinyProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHUDFullbrightShinyProgram.mName = "HUD FullbrightShiny Shader";
        gHUDFullbrightShinyProgram.mFeatures.calculatesAtmospherics = true;
        gHUDFullbrightShinyProgram.mFeatures.hasAtmospherics = true;
        gHUDFullbrightShinyProgram.mFeatures.hasGamma = true;
        gHUDFullbrightShinyProgram.mFeatures.hasSrgb = true;
        gHUDFullbrightShinyProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gHUDFullbrightShinyProgram.mShaderFiles.clear();
        gHUDFullbrightShinyProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightShinyV.glsl", ShaderStage::Vertex));
        gHUDFullbrightShinyProgram.mShaderFiles.push_back(make_pair("deferred/fullbrightShinyF.glsl", ShaderStage::Fragment));
        gHUDFullbrightShinyProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gHUDFullbrightShinyProgram.mFeatures.hasReflectionProbes = true;
        gHUDFullbrightShinyProgram.clearPermutations();
        gHUDFullbrightShinyProgram.addPermutation("IS_HUD", "1");

        add_common_permutations(&gHUDFullbrightShinyProgram);

        success = gHUDFullbrightShinyProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredEmissiveProgram.mName = "Deferred Emissive Shader";
        gDeferredEmissiveProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredEmissiveProgram.mFeatures.hasGamma = true;
        gDeferredEmissiveProgram.mFeatures.hasAtmospherics = true;
        gDeferredEmissiveProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;
        gDeferredEmissiveProgram.mShaderFiles.clear();
        gDeferredEmissiveProgram.mShaderFiles.push_back(make_pair("deferred/emissiveV.glsl", ShaderStage::Vertex));
        gDeferredEmissiveProgram.mShaderFiles.push_back(make_pair("deferred/emissiveF.glsl", ShaderStage::Fragment));
        gDeferredEmissiveProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredEmissiveProgram);

        success = make_rigged_variant(gDeferredEmissiveProgram, gDeferredSkinnedEmissiveProgram);
        success = success && gDeferredEmissiveProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredSoftenProgram.mName = "Deferred Soften Shader";
        gDeferredSoftenProgram.mShaderFiles.clear();
        gDeferredSoftenProgram.mFeatures.hasSrgb = true;
        gDeferredSoftenProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredSoftenProgram.mFeatures.hasAtmospherics = true;
        gDeferredSoftenProgram.mFeatures.hasGamma = true;
        gDeferredSoftenProgram.mFeatures.isDeferred = true;
        gDeferredSoftenProgram.mFeatures.hasFullGBuffer = true;
        gDeferredSoftenProgram.mFeatures.hasShadows = use_sun_shadow;
        gDeferredSoftenProgram.mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED] > 2;

        gDeferredSoftenProgram.clearPermutations();
        add_common_permutations(&gDeferredSoftenProgram);
        gDeferredSoftenProgram.mShaderFiles.push_back(make_pair("deferred/softenLightV.glsl", ShaderStage::Vertex));
        gDeferredSoftenProgram.mShaderFiles.push_back(make_pair("deferred/softenLightF.glsl", ShaderStage::Fragment));

        gDeferredSoftenProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        if (use_sun_shadow)
        {
            gDeferredSoftenProgram.addPermutation("HAS_SUN_SHADOW", "1");
            if (rt_reflections_possible())
            {
                gDeferredSoftenProgram.addPermutation("LL_RT_REFLECTIONS", "1");        // glossy surfaces trace their reflection (softenLightF.glsl)
            }
        }

        if (gSavedSettings.getBOOL("RenderDeferredSSAO"))
        { //if using SSAO, take screen space light map into account as if shadows are enabled
            gDeferredSoftenProgram.mShaderLevel = llmax(gDeferredSoftenProgram.mShaderLevel, 2);
            gDeferredSoftenProgram.addPermutation("HAS_SSAO", "1");
        }

        success = gDeferredSoftenProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gHazeProgram.mName = "Haze Shader";
        gHazeProgram.mShaderFiles.clear();
        gHazeProgram.mFeatures.hasSrgb                = true;
        gHazeProgram.mFeatures.calculatesAtmospherics = true;
        gHazeProgram.mFeatures.hasAtmospherics        = true;
        gHazeProgram.mFeatures.hasGamma               = true;
        gHazeProgram.mFeatures.isDeferred             = true;
        gHazeProgram.mFeatures.hasShadows             = use_sun_shadow;
        gHazeProgram.mFeatures.hasReflectionProbes    = mShaderLevel[SHADER_DEFERRED] > 2;

        gHazeProgram.clearPermutations();
        gHazeProgram.mShaderFiles.push_back(make_pair("deferred/softenLightV.glsl", ShaderStage::Vertex));
        gHazeProgram.mShaderFiles.push_back(make_pair("deferred/hazeF.glsl", ShaderStage::Fragment));

        add_common_permutations(&gHazeProgram);

        gHazeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gHazeProgram.createShader();
        llassert(success);
    }


    if (success)
    {
        gHazeWaterProgram.mName = "Water Haze Shader";
        gHazeWaterProgram.mShaderFiles.clear();
        gHazeWaterProgram.mShaderGroup           = LLShaderProgram::SG_WATER;
        gHazeWaterProgram.mFeatures.hasSrgb                = true;
        gHazeWaterProgram.mFeatures.calculatesAtmospherics = true;
        gHazeWaterProgram.mFeatures.hasAtmospherics        = true;
        gHazeWaterProgram.mFeatures.hasGamma               = true;
        gHazeWaterProgram.mFeatures.isDeferred             = true;
        gHazeWaterProgram.mFeatures.hasShadows             = use_sun_shadow;
        gHazeWaterProgram.mFeatures.hasReflectionProbes    = mShaderLevel[SHADER_DEFERRED] > 2;

        gHazeWaterProgram.clearPermutations();
        gHazeWaterProgram.mShaderFiles.push_back(make_pair("deferred/waterHazeV.glsl", ShaderStage::Vertex));
        gHazeWaterProgram.mShaderFiles.push_back(make_pair("deferred/waterHazeF.glsl", ShaderStage::Fragment));

        add_common_permutations(&gHazeWaterProgram);

        gHazeWaterProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        success = gHazeWaterProgram.createShader();
        llassert(success);
    }


    if (success)
    {
        gDeferredShadowProgram.mName = "Deferred Shadow Shader";
        gDeferredShadowProgram.mShaderFiles.clear();
        gDeferredShadowProgram.mShaderFiles.push_back(make_pair("deferred/shadowV.glsl", ShaderStage::Vertex));
        gDeferredShadowProgram.mShaderFiles.push_back(make_pair("deferred/shadowF.glsl", ShaderStage::Fragment));
        gDeferredShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredShadowProgram.mRiggedVariant = &gDeferredSkinnedShadowProgram;
        success = gDeferredShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredSkinnedShadowProgram.mName = "Deferred Skinned Shadow Shader";
        gDeferredSkinnedShadowProgram.mFeatures.isDeferred = true;
        gDeferredSkinnedShadowProgram.mFeatures.hasShadows = true;
        gDeferredSkinnedShadowProgram.mFeatures.hasObjectSkinning = true;
        gDeferredSkinnedShadowProgram.mShaderFiles.clear();
        gDeferredSkinnedShadowProgram.mShaderFiles.push_back(make_pair("deferred/shadowSkinnedV.glsl", ShaderStage::Vertex));
        gDeferredSkinnedShadowProgram.mShaderFiles.push_back(make_pair("deferred/shadowF.glsl", ShaderStage::Fragment));
        gDeferredSkinnedShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredSkinnedShadowProgram);

        // gDeferredSkinnedShadowProgram.addPermutation("DEPTH_CLAMP", "1"); // disable depth clamp for now
        success = gDeferredSkinnedShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredShadowCubeProgram.mName = "Deferred Shadow Cube Shader";
        gDeferredShadowCubeProgram.mFeatures.isDeferred = true;
        gDeferredShadowCubeProgram.mFeatures.hasShadows = true;
        gDeferredShadowCubeProgram.mShaderFiles.clear();
        gDeferredShadowCubeProgram.mShaderFiles.push_back(make_pair("deferred/shadowCubeV.glsl", ShaderStage::Vertex));
        gDeferredShadowCubeProgram.mShaderFiles.push_back(make_pair("deferred/shadowF.glsl", ShaderStage::Fragment));
        // gDeferredShadowCubeProgram.addPermutation("DEPTH_CLAMP", "1");
        gDeferredShadowCubeProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredShadowCubeProgram.createShader();
        llassert(success);
    }

    // <FS> Render packets: the shadow program that reads vertices and indices by address. Not needed to run: if it cannot be made the packets stay off.
    if (success && LLVkBridge::shadowPacketsAvailable())
    {
        gDeferredShadowPullProgram.mName = "Deferred Shadow Pull Shader";
        gDeferredShadowPullProgram.mFeatures.isDeferred = true;
        gDeferredShadowPullProgram.mFeatures.hasShadows = true;
        gDeferredShadowPullProgram.mShaderFiles.clear();
        gDeferredShadowPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowPullV.glsl", ShaderStage::Vertex));
        gDeferredShadowPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowF.glsl", ShaderStage::Fragment));
        gDeferredShadowPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gDeferredShadowPullProgram.createShader())
        {
            LL_WARNS("ShadowPackets") << "the shadow pull shader could not be made: shadow packets stay off" << LL_ENDL;
            gDeferredShadowPullProgram.unload();
        }
    }

    // <FS> ... and the one for the alpha masked casters, which needs the device to index sampled images at run time. Also optional.
    if (success && LLVkBridge::shadowAlphaPacketsAvailable())
    {
        gDeferredShadowAlphaPullProgram.mName = "Deferred Shadow Alpha Pull Shader";
        gDeferredShadowAlphaPullProgram.mFeatures.isDeferred = true;
        gDeferredShadowAlphaPullProgram.mFeatures.hasShadows = true;
        gDeferredShadowAlphaPullProgram.mShaderFiles.clear();
        gDeferredShadowAlphaPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaPullV.glsl", ShaderStage::Vertex));
        gDeferredShadowAlphaPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaPullF.glsl", ShaderStage::Fragment));
        gDeferredShadowAlphaPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gDeferredShadowAlphaPullProgram.createShader())
        {
            LL_WARNS("ShadowPackets") << "the shadow alpha pull shader could not be made: alpha masked casters stay on the old path" << LL_ENDL;
            gDeferredShadowAlphaPullProgram.unload();
        }
    }

    // <FS> ... and the one for rigged casters
    if (success && LLVkBridge::shadowPacketsAvailable())
    {
        gDeferredShadowSkinPullProgram.mName = "Deferred Shadow Skin Pull Shader";
        gDeferredShadowSkinPullProgram.mFeatures.isDeferred = true;
        gDeferredShadowSkinPullProgram.mFeatures.hasShadows = true;
        gDeferredShadowSkinPullProgram.mShaderFiles.clear();
        gDeferredShadowSkinPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowSkinPullV.glsl", ShaderStage::Vertex));
        gDeferredShadowSkinPullProgram.mShaderFiles.push_back(make_pair("deferred/shadowF.glsl", ShaderStage::Fragment));
        gDeferredShadowSkinPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gDeferredShadowSkinPullProgram.createShader())
        {
            LL_WARNS("ShadowPackets") << "the shadow skin pull shader could not be made: rigged casters stay on the old path" << LL_ENDL;
            gDeferredShadowSkinPullProgram.unload();
        }
    }

    // <FS> The deferred diffuse pass from render packets (same fragment shader as gDeferredDiffuseProgram, the diffuse lookup from the bindless table). Optional.
    if (success && LLVkBridge::shadowAlphaPacketsAvailable())
    {
        gDeferredDiffusePullProgram.mName = "Deferred Diffuse Pull Shader";
        gDeferredDiffusePullProgram.mFeatures.hasSrgb = true;
        gDeferredDiffusePullProgram.mShaderFiles.clear();
        gDeferredDiffusePullProgram.mShaderFiles.push_back(make_pair("deferred/diffusePullV.glsl", ShaderStage::Vertex));
        gDeferredDiffusePullProgram.mShaderFiles.push_back(make_pair("deferred/diffuseIndexedF.glsl", ShaderStage::Fragment));
        gDeferredDiffusePullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredDiffusePullProgram.addPermutation("LL_BINDLESS_DIFFUSE", "1");
        add_common_permutations(&gDeferredDiffusePullProgram);
        if (!gDeferredDiffusePullProgram.createShader())
        {
            LL_WARNS("ShadowPackets") << "the deferred diffuse pull shader could not be made: the diffuse pass stays on the old path" << LL_ENDL;
            gDeferredDiffusePullProgram.unload();
        }
        // <FS> S4 A1: ... and its rigged variant (HAS_SKIN: the draw's weights and palette are read by address). Optional.
        gDeferredDiffusePullSkinProgram.mName = "Deferred Diffuse Pull Skin Shader";
        gDeferredDiffusePullSkinProgram.mFeatures.hasSrgb = true;
        gDeferredDiffusePullSkinProgram.mShaderFiles.clear();
        gDeferredDiffusePullSkinProgram.mShaderFiles.push_back(make_pair("deferred/diffusePullV.glsl", ShaderStage::Vertex));
        gDeferredDiffusePullSkinProgram.mShaderFiles.push_back(make_pair("deferred/diffuseIndexedF.glsl", ShaderStage::Fragment));
        gDeferredDiffusePullSkinProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredDiffusePullSkinProgram.addPermutation("LL_BINDLESS_DIFFUSE", "1");
        gDeferredDiffusePullSkinProgram.addPermutation("HAS_SKIN", "1");
        if (ll_rigged_indexed())
        {
            gDeferredDiffusePullSkinProgram.addPermutation("HAS_INDEXED_DRAW", "1");
        }
        if (getenv("LL_VK_SKIN_TEST"))
        {
            gDeferredDiffusePullSkinProgram.addPermutation("SKIN_NOOP", "1");       // (diagnostic: a wrong picture, only for the GPU time)
        }
        add_common_permutations(&gDeferredDiffusePullSkinProgram);
        if (!gDeferredDiffusePullSkinProgram.createShader())
        {
            LL_WARNS("ShadowPackets") << "the rigged deferred diffuse pull shader could not be made: the rigged diffuse draws stay on the old path" << LL_ENDL;
            gDeferredDiffusePullSkinProgram.unload();
        }
    }

    // <FS> ... and the materials passes: the unskinned programs of the 12 deferred passes (alpha mode in bits 0-1, specular map bit 2, normal map bit 3). Optional too.
    if (success && LLVkBridge::shadowAlphaPacketsAvailable())
    {
        // (S5b: + 1, 5, 9, 13, the blend variants the alpha pass draws materials with; no rigged variant of those yet)
        static const U32 pull_indices[] = { 0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15, 1, 5, 9, 13 };
        bool blend_pull_ok = true;
        for (U32 i : pull_indices)
        {
            LLShaderProgram& pull = gDeferredMaterialPullProgram[i];
            pull.mName = llformat("Material Pull Shader %d", i);
            pull.mShaderFiles.clear();
            pull.mShaderFiles.push_back(make_pair("deferred/materialPullV.glsl", ShaderStage::Vertex));
            pull.mShaderFiles.push_back(make_pair("deferred/materialF.glsl", ShaderStage::Fragment));
            pull.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            pull.clearPermutations();
            if (i & 0x8)
            {
                pull.addPermutation("HAS_NORMAL_MAP", "1");
            }
            if (i & 0x4)
            {
                pull.addPermutation("HAS_SPECULAR_MAP", "1");
            }
            const U32 alpha_mode = i & 0x3;
            pull.addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", alpha_mode));
            if (alpha_mode != 0)
            {
                pull.mFeatures.hasAlphaMask = true;
                pull.addPermutation("HAS_ALPHA_MASK", "1");
            }
            if (use_sun_shadow)
            {
                pull.addPermutation("HAS_SUN_SHADOW", "1");
            }
            pull.addPermutation("LL_BINDLESS_MATERIAL", "1");
            add_common_permutations(&pull);
            pull.mFeatures.hasSrgb = true;
            pull.mFeatures.calculatesAtmospherics = true;
            pull.mFeatures.hasAtmospherics = true;
            pull.mFeatures.hasGamma = true;
            pull.mFeatures.hasShadows = use_sun_shadow;
            pull.mFeatures.hasReflectionProbes = true;
            if (!pull.createShader())
            {
                LL_WARNS("ShadowPackets") << "a materials pull shader could not be made (index " << i << "): that materials pass stays on the old path" << LL_ENDL;
                pull.unload();
                if ((i & 3) == 1)
                {
                    blend_pull_ok = false;
                }
            }
            else if ((i & 3) == 1)
            {
                pull.mFeatures.hasLighting = true;       // (as the ordinary blend variants, see gDeferredMaterialProgram[1])
            }
            if ((i & 3) == 1)
            {
                continue;       // S5b: the alpha pass has no rigged variant of these yet
            }

            // <FS> S4 A1: the rigged variant of the same pass (HAS_SKIN)
            LLShaderProgram& skin = gDeferredMaterialPullSkinProgram[i];
            skin.mName = llformat("Material Pull Skin Shader %d", i);
            skin.mShaderFiles.clear();
            skin.mShaderFiles.push_back(make_pair("deferred/materialPullV.glsl", ShaderStage::Vertex));
            skin.mShaderFiles.push_back(make_pair("deferred/materialF.glsl", ShaderStage::Fragment));
            skin.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            skin.clearPermutations();
            if (i & 0x8)
            {
                skin.addPermutation("HAS_NORMAL_MAP", "1");
            }
            if (i & 0x4)
            {
                skin.addPermutation("HAS_SPECULAR_MAP", "1");
            }
            skin.addPermutation("DIFFUSE_ALPHA_MODE", llformat("%d", alpha_mode));
            if (alpha_mode != 0)
            {
                skin.mFeatures.hasAlphaMask = true;
                skin.addPermutation("HAS_ALPHA_MASK", "1");
            }
            if (use_sun_shadow)
            {
                skin.addPermutation("HAS_SUN_SHADOW", "1");
            }
            skin.addPermutation("LL_BINDLESS_MATERIAL", "1");
            skin.addPermutation("HAS_SKIN", "1");
            if (ll_rigged_indexed())
            {
                skin.addPermutation("HAS_INDEXED_DRAW", "1");
            }
            if (getenv("LL_VK_SKIN_TEST"))
            {
                skin.addPermutation("SKIN_NOOP", "1");       // (diagnostic: a wrong picture, only for the GPU time)
            }
            add_common_permutations(&skin);
            skin.mFeatures.hasSrgb = true;
            skin.mFeatures.calculatesAtmospherics = true;
            skin.mFeatures.hasAtmospherics = true;
            skin.mFeatures.hasGamma = true;
            skin.mFeatures.hasShadows = use_sun_shadow;
            skin.mFeatures.hasReflectionProbes = true;
            if (!skin.createShader())
            {
                LL_WARNS("ShadowPackets") << "a rigged materials pull shader could not be made (index " << i << "): that rigged pass stays on the old path" << LL_ENDL;
                skin.unload();
            }
        }
        gAlphaMaterialPullReady = blend_pull_ok;
    }

    if (success)
    {
        gDeferredShadowFullbrightAlphaMaskProgram.mName = "Deferred Shadow Fullbright Alpha Mask Shader";
        gDeferredShadowFullbrightAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;

        gDeferredShadowFullbrightAlphaMaskProgram.mShaderFiles.clear();
        gDeferredShadowFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaMaskV.glsl", ShaderStage::Vertex));
        gDeferredShadowFullbrightAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaMaskF.glsl", ShaderStage::Fragment));

        gDeferredShadowFullbrightAlphaMaskProgram.clearPermutations();
        gDeferredShadowFullbrightAlphaMaskProgram.addPermutation("DEPTH_CLAMP", "1");
        gDeferredShadowFullbrightAlphaMaskProgram.addPermutation("IS_FULLBRIGHT", "1");

        add_common_permutations(&gDeferredShadowFullbrightAlphaMaskProgram);

        gDeferredShadowFullbrightAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = make_rigged_variant(gDeferredShadowFullbrightAlphaMaskProgram, gDeferredSkinnedShadowFullbrightAlphaMaskProgram);
        success = success && gDeferredShadowFullbrightAlphaMaskProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredShadowAlphaMaskProgram.mName = "Deferred Shadow Alpha Mask Shader";
        gDeferredShadowAlphaMaskProgram.mFeatures.mIndexedTextureChannels = LLShaderProgram::sIndexedTextureChannels;

        gDeferredShadowAlphaMaskProgram.mShaderFiles.clear();
        gDeferredShadowAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaMaskV.glsl", ShaderStage::Vertex));
        gDeferredShadowAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/shadowAlphaMaskF.glsl", ShaderStage::Fragment));
        gDeferredShadowAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = make_rigged_variant(gDeferredShadowAlphaMaskProgram, gDeferredSkinnedShadowAlphaMaskProgram);
        success = success && gDeferredShadowAlphaMaskProgram.createShader();
        llassert(success);
    }


    if (success)
    {
        gDeferredShadowGLTFAlphaMaskProgram.mName = "Deferred GLTF Shadow Alpha Mask Shader";
        gDeferredShadowGLTFAlphaMaskProgram.mShaderFiles.clear();
        gDeferredShadowGLTFAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/pbrShadowAlphaMaskV.glsl", ShaderStage::Vertex));
        gDeferredShadowGLTFAlphaMaskProgram.mShaderFiles.push_back(make_pair("deferred/pbrShadowAlphaMaskF.glsl", ShaderStage::Fragment));
        gDeferredShadowGLTFAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredShadowGLTFAlphaMaskProgram.clearPermutations();

        add_common_permutations(&gDeferredShadowGLTFAlphaMaskProgram);

        success = make_rigged_variant(gDeferredShadowGLTFAlphaMaskProgram, gDeferredSkinnedShadowGLTFAlphaMaskProgram);
        success = success && gDeferredShadowGLTFAlphaMaskProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredShadowGLTFAlphaBlendProgram.mName = "Deferred GLTF Shadow Alpha Blend Shader";
        gDeferredShadowGLTFAlphaBlendProgram.mShaderFiles.clear();
        gDeferredShadowGLTFAlphaBlendProgram.mShaderFiles.push_back(make_pair("deferred/pbrShadowAlphaMaskV.glsl", ShaderStage::Vertex));
        gDeferredShadowGLTFAlphaBlendProgram.mShaderFiles.push_back(make_pair("deferred/pbrShadowAlphaBlendF.glsl", ShaderStage::Fragment));
        gDeferredShadowGLTFAlphaBlendProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredShadowGLTFAlphaBlendProgram.clearPermutations();

        add_common_permutations(&gDeferredShadowGLTFAlphaBlendProgram);

        success = make_rigged_variant(gDeferredShadowGLTFAlphaBlendProgram, gDeferredSkinnedShadowGLTFAlphaBlendProgram);
        success = success && gDeferredShadowGLTFAlphaBlendProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredAvatarShadowProgram.mName = "Deferred Avatar Shadow Shader";
        gDeferredAvatarShadowProgram.mFeatures.hasSkinning = true;

        gDeferredAvatarShadowProgram.mShaderFiles.clear();
        gDeferredAvatarShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarShadowV.glsl", ShaderStage::Vertex));
        gDeferredAvatarShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarShadowF.glsl", ShaderStage::Fragment));
        gDeferredAvatarShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredAvatarAlphaShadowProgram.mName = "Deferred Avatar Alpha Shadow Shader";
        gDeferredAvatarAlphaShadowProgram.mFeatures.hasSkinning = true;
        gDeferredAvatarAlphaShadowProgram.mShaderFiles.clear();
        gDeferredAvatarAlphaShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarAlphaShadowV.glsl", ShaderStage::Vertex));
        gDeferredAvatarAlphaShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarAlphaShadowF.glsl", ShaderStage::Fragment));
        gDeferredAvatarAlphaShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarAlphaShadowProgram.createShader();
        llassert(success);
    }
    if (success)
    {
        gDeferredAvatarAlphaMaskShadowProgram.mName = "Deferred Avatar Alpha Mask Shadow Shader";
        gDeferredAvatarAlphaMaskShadowProgram.mFeatures.hasSkinning  = true;
        gDeferredAvatarAlphaMaskShadowProgram.mShaderFiles.clear();
        gDeferredAvatarAlphaMaskShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarAlphaShadowV.glsl", ShaderStage::Vertex));
        gDeferredAvatarAlphaMaskShadowProgram.mShaderFiles.push_back(make_pair("deferred/avatarAlphaMaskShadowF.glsl", ShaderStage::Fragment));
        gDeferredAvatarAlphaMaskShadowProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredAvatarAlphaMaskShadowProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredTerrainProgram.mName = "Deferred Terrain Shader";
        gDeferredTerrainProgram.mFeatures.hasSrgb = true;
        gDeferredTerrainProgram.mFeatures.isAlphaLighting = true;
        gDeferredTerrainProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredTerrainProgram.mFeatures.hasAtmospherics = true;
        gDeferredTerrainProgram.mFeatures.hasGamma = true;

        gDeferredTerrainProgram.mShaderFiles.clear();
        gDeferredTerrainProgram.mShaderFiles.push_back(make_pair("deferred/terrainV.glsl", ShaderStage::Vertex));
        gDeferredTerrainProgram.mShaderFiles.push_back(make_pair("deferred/terrainF.glsl", ShaderStage::Fragment));

        add_common_permutations(&gDeferredTerrainProgram);

        gDeferredTerrainProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredTerrainProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredAvatarProgram.mName = "Deferred Avatar Shader";
        gDeferredAvatarProgram.mFeatures.hasSkinning = true;
        gDeferredAvatarProgram.mShaderFiles.clear();
        gDeferredAvatarProgram.mShaderFiles.push_back(make_pair("deferred/avatarV.glsl", ShaderStage::Vertex));
        gDeferredAvatarProgram.mShaderFiles.push_back(make_pair("deferred/avatarF.glsl", ShaderStage::Fragment));
        gDeferredAvatarProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        gDeferredAvatarProgram.clearPermutations();
        add_common_permutations(&gDeferredAvatarProgram);
        gDeferredAvatarProgram.addPermutation("AVATAR_CLOTH", LLPipeline::RenderAvatarCloth ? "1" : "0");

        success = gDeferredAvatarProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredAvatarAlphaProgram.mName = "Deferred Avatar Alpha Shader";
        gDeferredAvatarAlphaProgram.mFeatures.hasSkinning = true;
        gDeferredAvatarAlphaProgram.mFeatures.calculatesLighting = false;
        gDeferredAvatarAlphaProgram.mFeatures.hasLighting = false;
        gDeferredAvatarAlphaProgram.mFeatures.isAlphaLighting = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasSrgb = true;
        gDeferredAvatarAlphaProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasAtmospherics = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasGamma = true;
        gDeferredAvatarAlphaProgram.mFeatures.isDeferred = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasShadows = true;
        gDeferredAvatarAlphaProgram.mFeatures.hasReflectionProbes = true;

        gDeferredAvatarAlphaProgram.mShaderFiles.clear();
        gDeferredAvatarAlphaProgram.mShaderFiles.push_back(make_pair("deferred/alphaV.glsl", ShaderStage::Vertex));
        gDeferredAvatarAlphaProgram.mShaderFiles.push_back(make_pair("deferred/alphaF.glsl", ShaderStage::Fragment));

        gDeferredAvatarAlphaProgram.clearPermutations();
        gDeferredAvatarAlphaProgram.addPermutation("USE_DIFFUSE_TEX", "1");
        gDeferredAvatarAlphaProgram.addPermutation("IS_AVATAR_SKIN", "1");
        if (use_sun_shadow)
        {
            gDeferredAvatarAlphaProgram.addPermutation("HAS_SUN_SHADOW", "1");
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
        gExposureProgram.mFeatures.hasSrgb = true;
        gExposureProgram.mFeatures.isDeferred = true;
        gExposureProgram.mShaderFiles.clear();
        gExposureProgram.clearPermutations();
        gExposureProgram.addPermutation("USE_LAST_EXPOSURE", "1");
        gExposureProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gExposureProgram.mShaderFiles.push_back(make_pair("deferred/exposureF.glsl", ShaderStage::Fragment));
        gExposureProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gExposureProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gExposureProgramNoFade.mName = "Exposure (no fade)";
        gExposureProgramNoFade.mFeatures.hasSrgb = true;
        gExposureProgramNoFade.mFeatures.isDeferred = true;
        gExposureProgramNoFade.mShaderFiles.clear();
        gExposureProgramNoFade.clearPermutations();
        gExposureProgramNoFade.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gExposureProgramNoFade.mShaderFiles.push_back(make_pair("deferred/exposureF.glsl", ShaderStage::Fragment));
        gExposureProgramNoFade.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gExposureProgramNoFade.createShader();
        llassert(success);
    }

    if (success)
    {
        gLuminanceProgram.mName = "Luminance";
        gLuminanceProgram.mShaderFiles.clear();
        gLuminanceProgram.clearPermutations();
        gLuminanceProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gLuminanceProgram.mShaderFiles.push_back(make_pair("deferred/luminanceF.glsl", ShaderStage::Fragment));
        gLuminanceProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gLuminanceProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gMotionVectorProgram.mName = "Motion Vectors";
        gMotionVectorProgram.mShaderFiles.clear();
        gMotionVectorProgram.clearPermutations();
        gMotionVectorProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gMotionVectorProgram.mShaderFiles.push_back(make_pair("deferred/motionVectorsF.glsl", ShaderStage::Fragment));
        gMotionVectorProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gMotionVectorProgram.createShader();
        llassert(success);
    }

    // <FS> avatar motion vectors (doc/vulkan/PLAN-ray-tracing.md, item 6). Optional: a failure here must not stop the programs that follow (the pass checks isComplete()).
    if (success)
    {
        const std::string max_joints = std::to_string(LLSkinningUtil::getMaxJointCount());     // the Vulkan translator wants literal array sizes
        gAvatarVelocityProgram.mName = "Avatar Velocity (rigged)";
        gAvatarVelocityProgram.mShaderFiles.clear();
        gAvatarVelocityProgram.clearPermutations();
        gAvatarVelocityProgram.addPermutation("MAX_JOINTS_PER_MESH_OBJECT", max_joints);
        gAvatarVelocityProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityV.glsl", ShaderStage::Vertex));
        gAvatarVelocityProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityF.glsl", ShaderStage::Fragment));
        gAvatarVelocityProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gAvatarVelocityProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the avatar velocity program (rigged) did not compile: no avatar motion vectors" << LL_ENDL;
        }

        if (LLVkBridge::shadowPacketsAvailable())
        {
            gAvatarVelocityPullProgram.mName = "Avatar Velocity (rigged, packets)";
            gAvatarVelocityPullProgram.mShaderFiles.clear();
            gAvatarVelocityPullProgram.clearPermutations();
            gAvatarVelocityPullProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityPullV.glsl", ShaderStage::Vertex));
            gAvatarVelocityPullProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityF.glsl", ShaderStage::Fragment));
            gAvatarVelocityPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!gAvatarVelocityPullProgram.createShader())
            {
                LL_WARNS("VkMotion") << "the avatar velocity pull program did not compile: the pass draws the rigged parts one by one" << LL_ENDL;
                gAvatarVelocityPullProgram.unload();
            }
        }

        if (LLVkBridge::shadowPacketsAvailable())
        {
            gObjectVelocityPullProgram.mName = "Object Velocity (packets)";
            gObjectVelocityPullProgram.mShaderFiles.clear();
            gObjectVelocityPullProgram.clearPermutations();
            gObjectVelocityPullProgram.mShaderFiles.push_back(make_pair("deferred/objectVelocityPullV.glsl", ShaderStage::Vertex));
            gObjectVelocityPullProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityF.glsl", ShaderStage::Fragment));
            gObjectVelocityPullProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
            if (!gObjectVelocityPullProgram.createShader())
            {
                LL_WARNS("VkMotion") << "the object velocity pull program did not compile: no motion vectors for moving objects" << LL_ENDL;
                gObjectVelocityPullProgram.unload();
            }
        }

        gAvatarBodyVelocityProgram.mName = "Avatar Velocity (body)";
        gAvatarBodyVelocityProgram.mShaderFiles.clear();
        gAvatarBodyVelocityProgram.clearPermutations();
        gAvatarBodyVelocityProgram.mShaderFiles.push_back(make_pair("deferred/avatarBodyVelocityV.glsl", ShaderStage::Vertex));
        gAvatarBodyVelocityProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityF.glsl", ShaderStage::Fragment));
        gAvatarBodyVelocityProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gAvatarBodyVelocityProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the avatar velocity program (body) did not compile: no avatar motion vectors" << LL_ENDL;
        }

        gShadowAccumProgram.mName = "Shadow Accumulation";
        gShadowAccumProgram.mFeatures.isDeferred = true;
        gShadowAccumProgram.mShaderFiles.clear();
        gShadowAccumProgram.clearPermutations();
        gShadowAccumProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gShadowAccumProgram.mShaderFiles.push_back(make_pair("deferred/shadowAccumF.glsl", ShaderStage::Fragment));
        gShadowAccumProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gShadowAccumProgram);
        if (!gShadowAccumProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the shadow accumulation program did not compile: ray shadows are not accumulated" << LL_ENDL;
        }

        gRRGuidesProgram.mName = "Ray Reconstruction Guides";
        gRRGuidesProgram.mFeatures.isDeferred = true;
        gRRGuidesProgram.mFeatures.hasFullGBuffer = true;
        gRRGuidesProgram.mShaderFiles.clear();
        gRRGuidesProgram.clearPermutations();
        gRRGuidesProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gRRGuidesProgram.mShaderFiles.push_back(make_pair("deferred/rrGuidesF.glsl", ShaderStage::Fragment));
        gRRGuidesProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gRRGuidesProgram);
        if (!gRRGuidesProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the ray reconstruction guides program did not compile: no ray reconstruction" << LL_ENDL;
        }

        gRRReflMotionProgram.mName = "Ray Reconstruction Reflection Motion";
        gRRReflMotionProgram.mFeatures.isDeferred = true;
        gRRReflMotionProgram.mShaderFiles.clear();
        gRRReflMotionProgram.clearPermutations();
        gRRReflMotionProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gRRReflMotionProgram.mShaderFiles.push_back(make_pair("deferred/rrReflMotionF.glsl", ShaderStage::Fragment));
        gRRReflMotionProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        add_common_permutations(&gRRReflMotionProgram);
        if (!gRRReflMotionProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the ray reconstruction reflection motion program did not compile: no reflection motion vectors" << LL_ENDL;
        }

        gRRHitDistProgram.mName = "Ray Reconstruction Hit Distance";
        gRRHitDistProgram.mShaderFiles.clear();
        gRRHitDistProgram.clearPermutations();
        gRRHitDistProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gRRHitDistProgram.mShaderFiles.push_back(make_pair("deferred/rrHitDistF.glsl", ShaderStage::Fragment));
        gRRHitDistProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gRRHitDistProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the ray reconstruction hit distance program did not compile: no hit distance guide" << LL_ENDL;
        }

        gAvatarVelocityDebugProgram.mName = "Avatar Velocity Debug";
        gAvatarVelocityDebugProgram.mShaderFiles.clear();
        gAvatarVelocityDebugProgram.clearPermutations();
        gAvatarVelocityDebugProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gAvatarVelocityDebugProgram.mShaderFiles.push_back(make_pair("deferred/avatarVelocityDebugF.glsl", ShaderStage::Fragment));
        gAvatarVelocityDebugProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        if (!gAvatarVelocityDebugProgram.createShader())
        {
            LL_WARNS("VkMotion") << "the avatar velocity debug program did not compile" << LL_ENDL;
        }

        if (LLVkBridge::rtShadowMode() && getenv("LL_VK_RT_MATERIALS") && getenv("LL_VK_RT_MATERIALS")[0] == '1')
        {
            gRtMaterialDebugProgram.mName = "Ray Scene Material Debug";
            gRtMaterialDebugProgram.mFeatures.isDeferred = true;
            gRtMaterialDebugProgram.mShaderFiles.clear();
            gRtMaterialDebugProgram.clearPermutations();
            gRtMaterialDebugProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
            gRtMaterialDebugProgram.mShaderFiles.push_back(make_pair("deferred/rtMaterialDebugF.glsl", ShaderStage::Fragment));
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
        gDeferredPostGammaCorrectProgram.mFeatures.hasSrgb = true;
        gDeferredPostGammaCorrectProgram.mFeatures.isDeferred = true;
        gDeferredPostGammaCorrectProgram.mShaderFiles.clear();
        gDeferredPostGammaCorrectProgram.clearPermutations();
        gDeferredPostGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredGammaCorrect.glsl", ShaderStage::Fragment));
        gDeferredPostGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gLegacyPostGammaCorrectProgram.mName = "Legacy Gamma Correction Post Process";
        gLegacyPostGammaCorrectProgram.mFeatures.hasSrgb = true;
        gLegacyPostGammaCorrectProgram.mFeatures.isDeferred = true;
        gLegacyPostGammaCorrectProgram.mShaderFiles.clear();
        gLegacyPostGammaCorrectProgram.clearPermutations();
        gLegacyPostGammaCorrectProgram.addPermutation("LEGACY_GAMMA", "1");
        gLegacyPostGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gLegacyPostGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredGammaCorrect.glsl", ShaderStage::Fragment));
        gLegacyPostGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gLegacyPostGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapProgram.mName = "Deferred Tonemap Post Process";
        gDeferredPostTonemapProgram.mFeatures.hasSrgb = true;
        gDeferredPostTonemapProgram.mFeatures.isDeferred = true;
        gDeferredPostTonemapProgram.mFeatures.hasTonemap = true;
        gDeferredPostTonemapProgram.mShaderFiles.clear();
        gDeferredPostTonemapProgram.clearPermutations();
        gDeferredPostTonemapProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostTonemapProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
        gDeferredPostTonemapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapProgram.mName = "No Post Tonemap Post Process";
        gNoPostTonemapProgram.mFeatures.hasSrgb = true;
        gNoPostTonemapProgram.mFeatures.isDeferred = true;
        gNoPostTonemapProgram.mFeatures.hasTonemap = true;
        gNoPostTonemapProgram.mShaderFiles.clear();
        gNoPostTonemapProgram.clearPermutations();
        gNoPostTonemapProgram.addPermutation("NO_POST", "1");
        gNoPostTonemapProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gNoPostTonemapProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
        gNoPostTonemapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gNoPostTonemapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapGammaCorrectProgram.mName = "Deferred Tonemap Gamma Post Process";
        gDeferredPostTonemapGammaCorrectProgram.mFeatures.hasSrgb = true;
        gDeferredPostTonemapGammaCorrectProgram.mFeatures.isDeferred = true;
        gDeferredPostTonemapGammaCorrectProgram.mFeatures.hasTonemap = true;
        gDeferredPostTonemapGammaCorrectProgram.mShaderFiles.clear();
        gDeferredPostTonemapGammaCorrectProgram.clearPermutations();
        gDeferredPostTonemapGammaCorrectProgram.addPermutation("GAMMA_CORRECT", "1");
        gDeferredPostTonemapGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostTonemapGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
        gDeferredPostTonemapGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapGammaCorrectProgram.mName = "No Post Tonemap Gamma Post Process";
        gNoPostTonemapGammaCorrectProgram.mFeatures.hasSrgb = true;
        gNoPostTonemapGammaCorrectProgram.mFeatures.isDeferred = true;
        gNoPostTonemapGammaCorrectProgram.mFeatures.hasTonemap = true;
        gNoPostTonemapGammaCorrectProgram.mShaderFiles.clear();
        gNoPostTonemapGammaCorrectProgram.clearPermutations();
        gNoPostTonemapGammaCorrectProgram.addPermutation("GAMMA_CORRECT", "1");
        gNoPostTonemapGammaCorrectProgram.addPermutation("NO_POST", "1");
        gNoPostTonemapGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gNoPostTonemapGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
        gNoPostTonemapGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gNoPostTonemapGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostTonemapLegacyGammaCorrectProgram.mName = "Deferred Tonemap Legacy Gamma Post Process";
        gDeferredPostTonemapLegacyGammaCorrectProgram.mFeatures.hasSrgb = true;
        gDeferredPostTonemapProgram.mFeatures.isDeferred = true;
        gDeferredPostTonemapLegacyGammaCorrectProgram.mFeatures.hasTonemap = true;
        gDeferredPostTonemapLegacyGammaCorrectProgram.mShaderFiles.clear();
        gDeferredPostTonemapLegacyGammaCorrectProgram.clearPermutations();
        gDeferredPostTonemapLegacyGammaCorrectProgram.addPermutation("GAMMA_CORRECT", "1");
        gDeferredPostTonemapLegacyGammaCorrectProgram.addPermutation("LEGACY_GAMMA", "1");
        gDeferredPostTonemapLegacyGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostTonemapLegacyGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
        gDeferredPostTonemapLegacyGammaCorrectProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostTonemapLegacyGammaCorrectProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNoPostTonemapLegacyGammaCorrectProgram.mName = "No Post Tonemap Legacy Gamma Post Process";
        gNoPostTonemapLegacyGammaCorrectProgram.mFeatures.hasSrgb = true;
        gNoPostTonemapLegacyGammaCorrectProgram.mFeatures.isDeferred = true;
        gNoPostTonemapLegacyGammaCorrectProgram.mFeatures.hasTonemap = true;
        gNoPostTonemapLegacyGammaCorrectProgram.mShaderFiles.clear();
        gNoPostTonemapLegacyGammaCorrectProgram.clearPermutations();
        gNoPostTonemapLegacyGammaCorrectProgram.addPermutation("NO_POST", "1");
        gNoPostTonemapLegacyGammaCorrectProgram.addPermutation("GAMMA_CORRECT", "1");
        gNoPostTonemapLegacyGammaCorrectProgram.addPermutation("LEGACY_GAMMA", "1");
        gNoPostTonemapLegacyGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gNoPostTonemapLegacyGammaCorrectProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredTonemap.glsl", ShaderStage::Fragment));
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
                gFXAAProgram[i].mFeatures.isDeferred = true;
                gFXAAProgram[i].mShaderFiles.clear();
                gFXAAProgram[i].mShaderFiles.push_back(make_pair("deferred/postDeferredV.glsl", ShaderStage::Vertex));
                gFXAAProgram[i].mShaderFiles.push_back(make_pair("deferred/fxaaF.glsl", ShaderStage::Fragment));

                gFXAAProgram[i].clearPermutations();
                gFXAAProgram[i].addPermutation("FXAA_QUALITY__PRESET", quality_pair.first);
                if (gDeviceCaps.mApiVersionNumber > 3.9)
                {
                    gFXAAProgram[i].addPermutation("FXAA_GLSL_400", "1");
                }
                else
                {
                    gFXAAProgram[i].addPermutation("FXAA_GLSL_130", "1");
                }

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
                gSMAAEdgeDetectProgram[i].mFeatures.isDeferred = true;

                gSMAAEdgeDetectProgram[i].clearPermutations();
                gSMAAEdgeDetectProgram[i].addPermutations(defines);

                gSMAAEdgeDetectProgram[i].mShaderFiles.clear();
                gSMAAEdgeDetectProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAAEdgeDetectF.glsl", ShaderStage::Fragment));
                gSMAAEdgeDetectProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAAEdgeDetectV.glsl", ShaderStage::Vertex));
                gSMAAEdgeDetectProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Fragment));
                gSMAAEdgeDetectProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Vertex));
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
                gSMAABlendWeightsProgram[i].mFeatures.isDeferred = true;

                gSMAABlendWeightsProgram[i].clearPermutations();
                gSMAABlendWeightsProgram[i].addPermutations(defines);

                gSMAABlendWeightsProgram[i].mShaderFiles.clear();
                gSMAABlendWeightsProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAABlendWeightsF.glsl", ShaderStage::Fragment));
                gSMAABlendWeightsProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAABlendWeightsV.glsl", ShaderStage::Vertex));
                gSMAABlendWeightsProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Fragment));
                gSMAABlendWeightsProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Vertex));
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
                gSMAANeighborhoodBlendProgram[i].mFeatures.isDeferred = true;

                gSMAANeighborhoodBlendProgram[i].clearPermutations();
                gSMAANeighborhoodBlendProgram[i].addPermutations(defines);

                gSMAANeighborhoodBlendProgram[i].mShaderFiles.clear();
                gSMAANeighborhoodBlendProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAANeighborhoodBlendF.glsl", ShaderStage::Fragment));
                gSMAANeighborhoodBlendProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAANeighborhoodBlendV.glsl", ShaderStage::Vertex));
                gSMAANeighborhoodBlendProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Fragment));
                gSMAANeighborhoodBlendProgram[i].mShaderFiles.push_back(make_pair("deferred/SMAA.glsl", ShaderStage::Vertex));
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
        gCASProgram.mFeatures.hasSrgb = true;
        gCASProgram.mShaderFiles.clear();
        gCASProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gCASProgram.mShaderFiles.push_back(make_pair("deferred/CASF.glsl", ShaderStage::Fragment));
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
        gCASLegacyGammaProgram.mFeatures.hasSrgb = true;
        gCASLegacyGammaProgram.mShaderFiles.clear();
        gCASLegacyGammaProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gCASLegacyGammaProgram.mShaderFiles.push_back(make_pair("deferred/CASF.glsl", ShaderStage::Fragment));
        gCASLegacyGammaProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gCASLegacyGammaProgram.clearPermutations();
        gCASLegacyGammaProgram.addPermutation("GAMMA_CORRECT", "1");
        gCASLegacyGammaProgram.addPermutation("LEGACY_GAMMA", "1");
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
        gDeferredPostProgram.mFeatures.isDeferred = true;
        gDeferredPostProgram.mShaderFiles.clear();
        gDeferredPostProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredF.glsl", ShaderStage::Fragment));
        gDeferredPostProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredCoFProgram.mName = "Deferred CoF Shader";
        gDeferredCoFProgram.mShaderFiles.clear();
        gDeferredCoFProgram.mFeatures.isDeferred = true;
        gDeferredCoFProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredCoFProgram.mShaderFiles.push_back(make_pair("deferred/cofF.glsl", ShaderStage::Fragment));
        gDeferredCoFProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredCoFProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredDoFCombineProgram.mName = "Deferred DoFCombine Shader";
        gDeferredDoFCombineProgram.mFeatures.isDeferred = true;
        gDeferredDoFCombineProgram.mShaderFiles.clear();
        gDeferredDoFCombineProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredDoFCombineProgram.mShaderFiles.push_back(make_pair("deferred/dofCombineF.glsl", ShaderStage::Fragment));
        gDeferredDoFCombineProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredDoFCombineProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostNoDoFProgram.mName = "Deferred Post NoDoF Shader";
        gDeferredPostNoDoFProgram.mFeatures.isDeferred = true;
        gDeferredPostNoDoFProgram.mShaderFiles.clear();
        gDeferredPostNoDoFProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostNoDoFProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoDoFF.glsl", ShaderStage::Fragment));
        gDeferredPostNoDoFProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostNoDoFProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredPostNoDoFNoiseProgram.mName = "Deferred Post NoDoF Noise Shader";
        gDeferredPostNoDoFNoiseProgram.mFeatures.isDeferred = true;
        gDeferredPostNoDoFNoiseProgram.mShaderFiles.clear();
        gDeferredPostNoDoFNoiseProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredPostNoDoFNoiseProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoDoFF.glsl", ShaderStage::Fragment));

        gDeferredPostNoDoFNoiseProgram.clearPermutations();
        gDeferredPostNoDoFNoiseProgram.addPermutation("HAS_NOISE", "1");

        gDeferredPostNoDoFNoiseProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredPostNoDoFNoiseProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gEnvironmentMapProgram.mName = "Environment Map Program";
        gEnvironmentMapProgram.mShaderFiles.clear();
        gEnvironmentMapProgram.mFeatures.calculatesAtmospherics = true;
        gEnvironmentMapProgram.mFeatures.hasAtmospherics = true;
        gEnvironmentMapProgram.mFeatures.hasGamma = true;
        gEnvironmentMapProgram.mFeatures.hasSrgb = true;

        gEnvironmentMapProgram.clearPermutations();
        gEnvironmentMapProgram.addPermutation("HAS_HDRI", "1");
        add_common_permutations(&gEnvironmentMapProgram);
        gEnvironmentMapProgram.mShaderFiles.push_back(make_pair("deferred/skyV.glsl", ShaderStage::Vertex));
        gEnvironmentMapProgram.mShaderFiles.push_back(make_pair("deferred/skyF.glsl", ShaderStage::Fragment));
        gEnvironmentMapProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gEnvironmentMapProgram.mShaderGroup = LLShaderProgram::SG_SKY;

        success = gEnvironmentMapProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredWLSkyProgram.mName = "Deferred Windlight Sky Shader";
        gDeferredWLSkyProgram.mShaderFiles.clear();
        gDeferredWLSkyProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLSkyProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLSkyProgram.mFeatures.hasGamma = true;
        gDeferredWLSkyProgram.mFeatures.hasSrgb = true;

        gDeferredWLSkyProgram.mShaderFiles.push_back(make_pair("deferred/skyV.glsl", ShaderStage::Vertex));
        gDeferredWLSkyProgram.mShaderFiles.push_back(make_pair("deferred/skyF.glsl", ShaderStage::Fragment));
        gDeferredWLSkyProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLSkyProgram.mShaderGroup = LLShaderProgram::SG_SKY;

        add_common_permutations(&gDeferredWLSkyProgram);

        success = gDeferredWLSkyProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredWLCloudProgram.mName = "Deferred Windlight Cloud Program";
        gDeferredWLCloudProgram.mShaderFiles.clear();
        gDeferredWLCloudProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLCloudProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLCloudProgram.mFeatures.hasGamma = true;
        gDeferredWLCloudProgram.mFeatures.hasSrgb = true;

        gDeferredWLCloudProgram.mShaderFiles.push_back(make_pair("deferred/cloudsV.glsl", ShaderStage::Vertex));
        gDeferredWLCloudProgram.mShaderFiles.push_back(make_pair("deferred/cloudsF.glsl", ShaderStage::Fragment));
        gDeferredWLCloudProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLCloudProgram.mShaderGroup = LLShaderProgram::SG_SKY;
        gDeferredWLCloudProgram.addConstant( LLShaderProgram::SHADER_CONST_CLOUD_MOON_DEPTH ); // SL-14113

        add_common_permutations(&gDeferredWLCloudProgram);

        success = gDeferredWLCloudProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredWLSunProgram.mName = "Deferred Windlight Sun Program";
        gDeferredWLSunProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLSunProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLSunProgram.mFeatures.hasGamma = true;
        gDeferredWLSunProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLSunProgram.mFeatures.hasSrgb = true;
        gDeferredWLSunProgram.mShaderFiles.clear();
        gDeferredWLSunProgram.mShaderFiles.push_back(make_pair("deferred/sunDiscV.glsl", ShaderStage::Vertex));
        gDeferredWLSunProgram.mShaderFiles.push_back(make_pair("deferred/sunDiscF.glsl", ShaderStage::Fragment));
        gDeferredWLSunProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLSunProgram.mShaderGroup = LLShaderProgram::SG_SKY;

        add_common_permutations(&gDeferredWLSunProgram);

        success = gDeferredWLSunProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredWLMoonProgram.mName = "Deferred Windlight Moon Program";
        gDeferredWLMoonProgram.mFeatures.calculatesAtmospherics = true;
        gDeferredWLMoonProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLMoonProgram.mFeatures.hasGamma = true;
        gDeferredWLMoonProgram.mFeatures.hasAtmospherics = true;
        gDeferredWLMoonProgram.mFeatures.hasSrgb = true;

        gDeferredWLMoonProgram.mShaderFiles.clear();
        gDeferredWLMoonProgram.mShaderFiles.push_back(make_pair("deferred/moonV.glsl", ShaderStage::Vertex));
        gDeferredWLMoonProgram.mShaderFiles.push_back(make_pair("deferred/moonF.glsl", ShaderStage::Fragment));
        gDeferredWLMoonProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredWLMoonProgram.mShaderGroup = LLShaderProgram::SG_SKY;
        gDeferredWLMoonProgram.addConstant( LLShaderProgram::SHADER_CONST_CLOUD_MOON_DEPTH ); // SL-14113

        add_common_permutations(&gDeferredWLMoonProgram);

        success = gDeferredWLMoonProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gDeferredStarProgram.mName = "Deferred Star Program";
        gDeferredStarProgram.mShaderFiles.clear();
        gDeferredStarProgram.mShaderFiles.push_back(make_pair("deferred/starsV.glsl", ShaderStage::Vertex));
        gDeferredStarProgram.mShaderFiles.push_back(make_pair("deferred/starsF.glsl", ShaderStage::Fragment));
        gDeferredStarProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gDeferredStarProgram.mShaderGroup = LLShaderProgram::SG_SKY;
        gDeferredStarProgram.addConstant( LLShaderProgram::SHADER_CONST_STAR_DEPTH ); // SL-14113

        add_common_permutations(&gDeferredStarProgram);

        success = gDeferredStarProgram.createShader();
        llassert(success);
    }

    if (success)
    {
        gNormalMapGenProgram.mName = "Normal Map Generation Program";
        gNormalMapGenProgram.mShaderFiles.clear();
        gNormalMapGenProgram.mShaderFiles.push_back(make_pair("deferred/normgenV.glsl", ShaderStage::Vertex));
        gNormalMapGenProgram.mShaderFiles.push_back(make_pair("deferred/normgenF.glsl", ShaderStage::Fragment));
        gNormalMapGenProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        gNormalMapGenProgram.mShaderGroup = LLShaderProgram::SG_SKY;
        success = gNormalMapGenProgram.createShader();
    }

    if (success)
    {
        gDeferredGenBrdfLutProgram.mName = "Brdf Gen Shader";
        gDeferredGenBrdfLutProgram.mShaderFiles.clear();
        gDeferredGenBrdfLutProgram.mShaderFiles.push_back(make_pair("deferred/genbrdflutV.glsl", ShaderStage::Vertex));
        gDeferredGenBrdfLutProgram.mShaderFiles.push_back(make_pair("deferred/genbrdflutF.glsl", ShaderStage::Fragment));
        gDeferredGenBrdfLutProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];
        success = gDeferredGenBrdfLutProgram.createShader();
    }

    if (success) {
        gPostScreenSpaceReflectionProgram.mName = "Screen Space Reflection Post";
        gPostScreenSpaceReflectionProgram.mShaderFiles.clear();
        gPostScreenSpaceReflectionProgram.mShaderFiles.push_back(make_pair("deferred/screenSpaceReflPostV.glsl", ShaderStage::Vertex));
        gPostScreenSpaceReflectionProgram.mShaderFiles.push_back(make_pair("deferred/screenSpaceReflPostF.glsl", ShaderStage::Fragment));
        gPostScreenSpaceReflectionProgram.mFeatures.hasScreenSpaceReflections = true;
        gPostScreenSpaceReflectionProgram.mFeatures.isDeferred                = true;
        gPostScreenSpaceReflectionProgram.mShaderLevel = 3;
        success = gPostScreenSpaceReflectionProgram.createShader();
    }

    if (success) {
        gDeferredBufferVisualProgram.mName = "Deferred Buffer Visualization Shader";
        gDeferredBufferVisualProgram.mShaderFiles.clear();
        gDeferredBufferVisualProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredNoTCV.glsl", ShaderStage::Vertex));
        gDeferredBufferVisualProgram.mShaderFiles.push_back(make_pair("deferred/postDeferredVisualizeBuffers.glsl", ShaderStage::Fragment));
        gDeferredBufferVisualProgram.mShaderLevel = mShaderLevel[SHADER_DEFERRED];

        add_common_permutations(&gDeferredBufferVisualProgram);

        success = gDeferredBufferVisualProgram.createShader();
    }
    // [RLVa:KB] - @setsphere
    if(success)
    {
        gRlvSphereProgram.mName = "RLVa Sphere Post Processing Shader";
        gRlvSphereProgram.mFeatures.isDeferred = true;
        gRlvSphereProgram.mShaderFiles.clear();
        gRlvSphereProgram.mShaderFiles.push_back(make_pair("deferred/rlvV.glsl", ShaderStage::Vertex));
        gRlvSphereProgram.mShaderFiles.push_back(make_pair("deferred/rlvF.glsl", ShaderStage::Fragment));
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
        gObjectBumpProgram.mName = "Bump Shader";
        gObjectBumpProgram.mShaderFiles.clear();
        gObjectBumpProgram.mShaderFiles.push_back(make_pair("objects/bumpV.glsl", ShaderStage::Vertex));
        gObjectBumpProgram.mShaderFiles.push_back(make_pair("objects/bumpF.glsl", ShaderStage::Fragment));
        gObjectBumpProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = make_rigged_variant(gObjectBumpProgram, gSkinnedObjectBumpProgram);
        success = success && gObjectBumpProgram.createShader();
        if (success)
        { //lldrawpoolbump assumes "texture0" has channel 0 and "texture1" has channel 1
            LLShaderProgram* shader[] = { &gObjectBumpProgram, &gSkinnedObjectBumpProgram };
            for (int i = 0; i < 2; ++i)
            {
                shader[i]->bind();
                shader[i]->uniform1i(sTexture0, 0);
                shader[i]->uniform1i(sTexture1, 1);
                shader[i]->unbind();
            }
        }
    }

    if (success)
    {
        gObjectAlphaMaskNoColorProgram.mName = "No color alpha mask Shader";
        gObjectAlphaMaskNoColorProgram.mFeatures.calculatesLighting = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.calculatesAtmospherics = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.hasGamma = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.hasAtmospherics = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.hasLighting = true;
        gObjectAlphaMaskNoColorProgram.mFeatures.hasAlphaMask = true;
        gObjectAlphaMaskNoColorProgram.mShaderFiles.clear();
        gObjectAlphaMaskNoColorProgram.mShaderFiles.push_back(make_pair("objects/simpleNoColorV.glsl", ShaderStage::Vertex));
        gObjectAlphaMaskNoColorProgram.mShaderFiles.push_back(make_pair("objects/simpleF.glsl", ShaderStage::Fragment));
        gObjectAlphaMaskNoColorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gObjectAlphaMaskNoColorProgram.createShader();
    }

    if (success)
    {
        gImpostorProgram.mName = "Impostor Shader";
        gImpostorProgram.mFeatures.hasSrgb = true;
        gImpostorProgram.mShaderFiles.clear();
        gImpostorProgram.mShaderFiles.push_back(make_pair("objects/impostorV.glsl", ShaderStage::Vertex));
        gImpostorProgram.mShaderFiles.push_back(make_pair("objects/impostorF.glsl", ShaderStage::Fragment));
        gImpostorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gImpostorProgram.createShader();
    }

    if (success)
    {
        gObjectPreviewProgram.mName = "Object Preview Shader";
        gObjectPreviewProgram.mShaderFiles.clear();
        gObjectPreviewProgram.mShaderFiles.push_back(make_pair("objects/previewV.glsl", ShaderStage::Vertex));
        gObjectPreviewProgram.mShaderFiles.push_back(make_pair("objects/previewF.glsl", ShaderStage::Fragment));
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
        gPhysicsPreviewProgram.mFeatures.hasGamma = false;
        gPhysicsPreviewProgram.mFeatures.hasAtmospherics = false;
        gPhysicsPreviewProgram.mFeatures.hasLighting = false;
        gPhysicsPreviewProgram.mShaderFiles.clear();
        gPhysicsPreviewProgram.mShaderFiles.push_back(make_pair("objects/previewPhysicsV.glsl", ShaderStage::Vertex));
        gPhysicsPreviewProgram.mShaderFiles.push_back(make_pair("objects/previewPhysicsF.glsl", ShaderStage::Fragment));
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
        gAvatarProgram.mFeatures.hasSkinning = true;
        gAvatarProgram.mFeatures.calculatesAtmospherics = true;
        gAvatarProgram.mFeatures.calculatesLighting = true;
        gAvatarProgram.mFeatures.hasGamma = true;
        gAvatarProgram.mFeatures.hasAtmospherics = true;
        gAvatarProgram.mFeatures.hasLighting = true;
        gAvatarProgram.mFeatures.hasAlphaMask = true;
        gAvatarProgram.mShaderFiles.clear();
        gAvatarProgram.mShaderFiles.push_back(make_pair("avatar/avatarV.glsl", ShaderStage::Vertex));
        gAvatarProgram.mShaderFiles.push_back(make_pair("avatar/avatarF.glsl", ShaderStage::Fragment));
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
        gHighlightProgram.mShaderFiles.clear();
        gHighlightProgram.mShaderFiles.push_back(make_pair("interface/highlightV.glsl", ShaderStage::Vertex));
        gHighlightProgram.mShaderFiles.push_back(make_pair("interface/highlightF.glsl", ShaderStage::Fragment));
        gHighlightProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = make_rigged_variant(gHighlightProgram, gSkinnedHighlightProgram);
        success = success && gHighlightProgram.createShader();
    }

    if (success)
    {
        gHighlightNormalProgram.mName = "Highlight Normals Shader";
        gHighlightNormalProgram.mShaderFiles.clear();
        gHighlightNormalProgram.mShaderFiles.push_back(make_pair("interface/highlightNormV.glsl", ShaderStage::Vertex));
        gHighlightNormalProgram.mShaderFiles.push_back(make_pair("interface/highlightF.glsl", ShaderStage::Fragment));
        gHighlightNormalProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gHighlightNormalProgram.createShader();
    }

    if (success)
    {
        gHighlightSpecularProgram.mName = "Highlight Spec Shader";
        gHighlightSpecularProgram.mShaderFiles.clear();
        gHighlightSpecularProgram.mShaderFiles.push_back(make_pair("interface/highlightSpecV.glsl", ShaderStage::Vertex));
        gHighlightSpecularProgram.mShaderFiles.push_back(make_pair("interface/highlightF.glsl", ShaderStage::Fragment));
        gHighlightSpecularProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gHighlightSpecularProgram.createShader();
    }

    if (success)
    {
        gUIProgram.mName = "UI Shader";
        gUIProgram.mShaderFiles.clear();
        gUIProgram.mShaderFiles.push_back(make_pair("interface/uiV.glsl", ShaderStage::Vertex));
        gUIProgram.mShaderFiles.push_back(make_pair("interface/uiF.glsl", ShaderStage::Fragment));
        gUIProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gUIProgram.createShader();
    }

    if (success)
    {
        gPathfindingProgram.mName = "Pathfinding Shader";
        gPathfindingProgram.mShaderFiles.clear();
        gPathfindingProgram.mShaderFiles.push_back(make_pair("interface/pathfindingV.glsl", ShaderStage::Vertex));
        gPathfindingProgram.mShaderFiles.push_back(make_pair("interface/pathfindingF.glsl", ShaderStage::Fragment));
        gPathfindingProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gPathfindingProgram.createShader();
    }

    if (success)
    {
        gPathfindingNoNormalsProgram.mName = "PathfindingNoNormals Shader";
        gPathfindingNoNormalsProgram.mShaderFiles.clear();
        gPathfindingNoNormalsProgram.mShaderFiles.push_back(make_pair("interface/pathfindingNoNormalV.glsl", ShaderStage::Vertex));
        gPathfindingNoNormalsProgram.mShaderFiles.push_back(make_pair("interface/pathfindingF.glsl", ShaderStage::Fragment));
        gPathfindingNoNormalsProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gPathfindingNoNormalsProgram.createShader();
    }

    if (success)
    {
        gGlowCombineProgram.mName = "Glow Combine Shader";
        gGlowCombineProgram.mShaderFiles.clear();
        gGlowCombineProgram.mShaderFiles.push_back(make_pair("interface/glowcombineV.glsl", ShaderStage::Vertex));
        gGlowCombineProgram.mShaderFiles.push_back(make_pair("interface/glowcombineF.glsl", ShaderStage::Fragment));
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
        gGlowCombineFXAAProgram.mShaderFiles.clear();
        gGlowCombineFXAAProgram.mShaderFiles.push_back(make_pair("interface/glowcombineFXAAV.glsl", ShaderStage::Vertex));
        gGlowCombineFXAAProgram.mShaderFiles.push_back(make_pair("interface/glowcombineFXAAF.glsl", ShaderStage::Fragment));
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
        gTwoTextureCompareProgram.mShaderFiles.clear();
        gTwoTextureCompareProgram.mShaderFiles.push_back(make_pair("interface/twotexturecompareV.glsl", ShaderStage::Vertex));
        gTwoTextureCompareProgram.mShaderFiles.push_back(make_pair("interface/twotexturecompareF.glsl", ShaderStage::Fragment));
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
        gOneTextureFilterProgram.mShaderFiles.clear();
        gOneTextureFilterProgram.mShaderFiles.push_back(make_pair("interface/onetexturefilterV.glsl", ShaderStage::Vertex));
        gOneTextureFilterProgram.mShaderFiles.push_back(make_pair("interface/onetexturefilterF.glsl", ShaderStage::Fragment));
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
        gSolidColorProgram.mName = "Solid Color Shader";
        gSolidColorProgram.mShaderFiles.clear();
        gSolidColorProgram.mShaderFiles.push_back(make_pair("interface/solidcolorV.glsl", ShaderStage::Vertex));
        gSolidColorProgram.mShaderFiles.push_back(make_pair("interface/solidcolorF.glsl", ShaderStage::Fragment));
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
        gOcclusionProgram.mName = "Occlusion Shader";
        gOcclusionProgram.mShaderFiles.clear();
        gOcclusionProgram.mShaderFiles.push_back(make_pair("interface/occlusionV.glsl", ShaderStage::Vertex));
        gOcclusionProgram.mShaderFiles.push_back(make_pair("interface/occlusionF.glsl", ShaderStage::Fragment));
        gOcclusionProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        gOcclusionProgram.mRiggedVariant = &gSkinnedOcclusionProgram;
        success = gOcclusionProgram.createShader();
    }

    if (success)
    {
        gSkinnedOcclusionProgram.mName = "Skinned Occlusion Shader";
        gSkinnedOcclusionProgram.mFeatures.hasObjectSkinning = true;
        gSkinnedOcclusionProgram.mShaderFiles.clear();
        gSkinnedOcclusionProgram.mShaderFiles.push_back(make_pair("interface/occlusionSkinnedV.glsl", ShaderStage::Vertex));
        gSkinnedOcclusionProgram.mShaderFiles.push_back(make_pair("interface/occlusionF.glsl", ShaderStage::Fragment));
        gSkinnedOcclusionProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gSkinnedOcclusionProgram.createShader();
    }

    if (success)
    {
        gOcclusionCubeProgram.mName = "Occlusion Cube Shader";
        gOcclusionCubeProgram.mShaderFiles.clear();
        gOcclusionCubeProgram.mShaderFiles.push_back(make_pair("interface/occlusionCubeV.glsl", ShaderStage::Vertex));
        gOcclusionCubeProgram.mShaderFiles.push_back(make_pair("interface/occlusionF.glsl", ShaderStage::Fragment));
        gOcclusionCubeProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gOcclusionCubeProgram.createShader();
    }

    if (success)
    {
        gDebugProgram.mName = "Debug Shader";
        gDebugProgram.mShaderFiles.clear();
        gDebugProgram.mShaderFiles.push_back(make_pair("interface/debugV.glsl", ShaderStage::Vertex));
        gDebugProgram.mShaderFiles.push_back(make_pair("interface/debugF.glsl", ShaderStage::Fragment));
        gDebugProgram.mRiggedVariant = &gSkinnedDebugProgram;
        gDebugProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = make_rigged_variant(gDebugProgram, gSkinnedDebugProgram);
        success = success && gDebugProgram.createShader();
    }

    if (success)
    {
        for (S32 variant = 0; variant < NORMAL_DEBUG_SHADER_COUNT; ++variant)
        {
            LLShaderProgram& shader = gNormalDebugProgram[variant];
            LLShaderProgram& skinned_shader = gSkinnedNormalDebugProgram[variant];
            shader.mName = "Normal Debug Shader";
            shader.mShaderFiles.clear();
            shader.mShaderFiles.push_back(make_pair("interface/normaldebugV.glsl", ShaderStage::Vertex));
            // *NOTE: Geometry shaders have a reputation for being slow.
            // Consider using compute shaders instead, which have a reputation
            // for being fast. This geometry shader in particular seems to run
            // fine on my machine, but I won't vouch for this in
            // performance-critical areas.  -Cosmic,2023-09-28
            shader.mShaderFiles.push_back(make_pair("interface/normaldebugG.glsl", ShaderStage::Geometry));
            shader.mShaderFiles.push_back(make_pair("interface/normaldebugF.glsl", ShaderStage::Fragment));
            shader.mRiggedVariant = &skinned_shader;
            shader.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
            if (variant == NORMAL_DEBUG_SHADER_WITH_TANGENTS)
            {
                shader.addPermutation("HAS_ATTRIBUTE_TANGENT", "1");
            }
            success = make_rigged_variant(shader, skinned_shader);
            success = success && shader.createShader();
        }
    }

    if (success)
    {
        gClipProgram.mName = "Clip Shader";
        gClipProgram.mShaderFiles.clear();
        gClipProgram.mShaderFiles.push_back(make_pair("interface/clipV.glsl", ShaderStage::Vertex));
        gClipProgram.mShaderFiles.push_back(make_pair("interface/clipF.glsl", ShaderStage::Fragment));
        gClipProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gClipProgram.createShader();
    }

    if (success)
    {
        gBenchmarkProgram.mName = "Benchmark Shader";
        gBenchmarkProgram.mShaderFiles.clear();
        gBenchmarkProgram.mShaderFiles.push_back(make_pair("interface/benchmarkV.glsl", ShaderStage::Vertex));
        gBenchmarkProgram.mShaderFiles.push_back(make_pair("interface/benchmarkF.glsl", ShaderStage::Fragment));
        gBenchmarkProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gBenchmarkProgram.createShader();
    }

    if (success)
    {
        gReflectionProbeDisplayProgram.mName = "Reflection Probe Display Shader";
        gReflectionProbeDisplayProgram.mFeatures.hasReflectionProbes = true;
        gReflectionProbeDisplayProgram.mFeatures.hasSrgb = true;
        gReflectionProbeDisplayProgram.mFeatures.calculatesAtmospherics = true;
        gReflectionProbeDisplayProgram.mFeatures.hasAtmospherics = true;
        gReflectionProbeDisplayProgram.mFeatures.hasGamma = true;
        gReflectionProbeDisplayProgram.mFeatures.isDeferred = true;
        gReflectionProbeDisplayProgram.mShaderFiles.clear();
        gReflectionProbeDisplayProgram.mShaderFiles.push_back(make_pair("interface/reflectionprobeV.glsl", ShaderStage::Vertex));
        gReflectionProbeDisplayProgram.mShaderFiles.push_back(make_pair("interface/reflectionprobeF.glsl", ShaderStage::Fragment));
        gReflectionProbeDisplayProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gReflectionProbeDisplayProgram.createShader();
    }

    if (success)
    {
        gCopyProgram.mName = "Copy Shader";
        gCopyProgram.mShaderFiles.clear();
        gCopyProgram.mShaderFiles.push_back(make_pair("interface/copyV.glsl", ShaderStage::Vertex));
        gCopyProgram.mShaderFiles.push_back(make_pair("interface/copyF.glsl", ShaderStage::Fragment));
        gCopyProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gCopyProgram.createShader();
    }

    if (success)
    {
        gCopyDepthProgram.mName = "Copy Depth Shader";
        gCopyDepthProgram.mShaderFiles.clear();
        gCopyDepthProgram.mShaderFiles.push_back(make_pair("interface/copyV.glsl", ShaderStage::Vertex));
        gCopyDepthProgram.mShaderFiles.push_back(make_pair("interface/copyF.glsl", ShaderStage::Fragment));
        gCopyDepthProgram.clearPermutations();
        gCopyDepthProgram.addPermutation("COPY_DEPTH", "1");
        gCopyDepthProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gCopyDepthProgram.createShader();
    }

    if (success)
    {
        gDrawColorProgram.mName = "Draw Color Shader";
        gDrawColorProgram.mShaderFiles.clear();
        gDrawColorProgram.mShaderFiles.push_back(make_pair("objects/simpleNoAtmosV.glsl", ShaderStage::Vertex));
        gDrawColorProgram.mShaderFiles.push_back(make_pair("objects/simpleColorF.glsl", ShaderStage::Fragment));
        gDrawColorProgram.clearPermutations();
        gDrawColorProgram.mShaderLevel = mShaderLevel[SHADER_OBJECT];
        success = gDrawColorProgram.createShader();
    }

    if (gSavedSettings.getBOOL("LocalTerrainPaintEnabled"))
    {
        if (success)
        {
            LLShaderProgram* shader = &gPBRTerrainBakeProgram;
            U32 bit_depth = gSavedSettings.getU32("TerrainPaintBitDepth");
            // LLTerrainPaintMap currently uses an RGB8 texture internally
            bit_depth = llclamp(bit_depth, 1, 8);
            shader->mName = llformat("Terrain Bake Shader RGB%o", bit_depth);
            shader->mFeatures.isPBRTerrain = true;

            shader->mShaderFiles.clear();
            shader->mShaderFiles.push_back(make_pair("interface/pbrTerrainBakeV.glsl", ShaderStage::Vertex));
            shader->mShaderFiles.push_back(make_pair("interface/pbrTerrainBakeF.glsl", ShaderStage::Fragment));
            shader->mShaderLevel = mShaderLevel[SHADER_INTERFACE];
            const U32 value_range = (1 << bit_depth) - 1;
            shader->addPermutation("TERRAIN_PAINT_PRECISION", llformat("%d", value_range));
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
        gAlphaMaskProgram.mShaderFiles.clear();
        gAlphaMaskProgram.mShaderFiles.push_back(make_pair("interface/alphamaskV.glsl", ShaderStage::Vertex));
        gAlphaMaskProgram.mShaderFiles.push_back(make_pair("interface/alphamaskF.glsl", ShaderStage::Fragment));
        gAlphaMaskProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gAlphaMaskProgram.createShader();
    }

    if (success)
    {
        gReflectionMipProgram.mName = "Reflection Mip Shader";
        gReflectionMipProgram.mFeatures.isDeferred = true;
        gReflectionMipProgram.mFeatures.hasGamma = true;
        gReflectionMipProgram.mFeatures.hasAtmospherics = true;
        gReflectionMipProgram.mFeatures.calculatesAtmospherics = true;
        gReflectionMipProgram.mShaderFiles.clear();
        gReflectionMipProgram.mShaderFiles.push_back(make_pair("interface/splattexturerectV.glsl", ShaderStage::Vertex));
        gReflectionMipProgram.mShaderFiles.push_back(make_pair("interface/reflectionmipF.glsl", ShaderStage::Fragment));
        gReflectionMipProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gReflectionMipProgram.createShader();
    }

    if (success)
    {
        gGaussianProgram.mName = "Reflection Mip Shader";
        gGaussianProgram.mFeatures.isDeferred = true;
        gGaussianProgram.mFeatures.hasGamma = true;
        gGaussianProgram.mFeatures.hasAtmospherics = true;
        gGaussianProgram.mFeatures.calculatesAtmospherics = true;
        gGaussianProgram.mShaderFiles.clear();
        gGaussianProgram.mShaderFiles.push_back(make_pair("interface/splattexturerectV.glsl", ShaderStage::Vertex));
        gGaussianProgram.mShaderFiles.push_back(make_pair("interface/gaussianF.glsl", ShaderStage::Fragment));
        gGaussianProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        success = gGaussianProgram.createShader();
    }

    if (success && gDeviceCaps.mHasCubeMapArray)
    {
        gRadianceGenProgram.mName = "Radiance Gen Shader";
        gRadianceGenProgram.mShaderFiles.clear();
        gRadianceGenProgram.mShaderFiles.push_back(make_pair("interface/radianceGenV.glsl", ShaderStage::Vertex));
        gRadianceGenProgram.mShaderFiles.push_back(make_pair("interface/radianceGenF.glsl", ShaderStage::Fragment));
        gRadianceGenProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        gRadianceGenProgram.addPermutation("PROBE_FILTER_SAMPLES", "32");
        success = gRadianceGenProgram.createShader();
    }

    if (success && gDeviceCaps.mHasCubeMapArray)
    {
        gHeroRadianceGenProgram.mName = "Hero Radiance Gen Shader";
        gHeroRadianceGenProgram.mShaderFiles.clear();
        gHeroRadianceGenProgram.mShaderFiles.push_back(make_pair("interface/radianceGenV.glsl", ShaderStage::Vertex));
        gHeroRadianceGenProgram.mShaderFiles.push_back(make_pair("interface/radianceGenF.glsl", ShaderStage::Fragment));
        gHeroRadianceGenProgram.mShaderLevel = mShaderLevel[SHADER_INTERFACE];
        gHeroRadianceGenProgram.addPermutation("HERO_PROBES", "1");
        gHeroRadianceGenProgram.addPermutation("PROBE_FILTER_SAMPLES", "4");
        success                              = gHeroRadianceGenProgram.createShader();
    }

    if (success && gDeviceCaps.mHasCubeMapArray)
    {
        gIrradianceGenProgram.mName = "Irradiance Gen Shader";
        gIrradianceGenProgram.mShaderFiles.clear();
        gIrradianceGenProgram.mShaderFiles.push_back(make_pair("interface/irradianceGenV.glsl", ShaderStage::Vertex));
        gIrradianceGenProgram.mShaderFiles.push_back(make_pair("interface/irradianceGenF.glsl", ShaderStage::Fragment));
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

void LLViewerShaderMgr::updateShaderUniforms(LLShaderProgram * shader)
{
    LLEnvironment::instance().updateShaderUniforms(shader);
}

LLViewerShaderMgr::shader_iter LLViewerShaderMgr::beginShaders() const
{
    return mShaderList.begin();
}

LLViewerShaderMgr::shader_iter LLViewerShaderMgr::endShaders() const
{
    return mShaderList.end();
}

