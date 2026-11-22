/**
 * @file llviewershadermgr.h
 * @brief Viewer Shader Manager
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

#ifndef LL_VIEWER_SHADER_MGR_H
#define LL_VIEWER_SHADER_MGR_H

#include "llshadermgr.h"
#include "llmaterial.h"

#define LL_DEFERRED_MULTI_LIGHT_COUNT 16

class LLViewerShaderMgr: public LLShaderMgr
{
public:
    static bool sInitialized;
    static bool sSkipReload;

    LLViewerShaderMgr();
    /* virtual */ ~LLViewerShaderMgr();

    // singleton pattern implementation
    static LLViewerShaderMgr * instance();
    static void releaseInstance();

    void initAttribsAndUniforms(void);
    void setShaders();
    void unloadShaders();
    S32  getShaderLevel(S32 type);

    // loadBasicShaders in case of a failure returns
    // name of a file error happened at, otherwise
    // returns an empty string
    std::string loadBasicShaders();
    bool loadShadersEffects();
    bool loadShadersDeferred();
    bool loadShadersObject();
    bool loadShadersAvatar();
    bool loadShadersWater();
    bool loadShadersInterface();

    std::vector<S32> mShaderLevel;
    S32 mMaxAvatarShaderLevel;

    enum EShaderClass
    {
        SHADER_LIGHTING,
        SHADER_OBJECT,
        SHADER_AVATAR,
        SHADER_ENVIRONMENT,
        SHADER_INTERFACE,
        SHADER_EFFECT,
        SHADER_WINDLIGHT,
        SHADER_WATER,
        SHADER_DEFERRED,
        SHADER_COUNT
    };

    /* virtual */ std::string getShaderDirPrefix(void);

    /* virtual */ void updateShaderUniforms(RKShaderProgram * shader);

}; //LLViewerShaderMgr

extern LLVector4            gShinyOrigin;

//utility shaders
extern RKShaderProgram         gGlowCombineProgram;
extern RKShaderProgram         gReflectionMipProgram;
extern RKShaderProgram         gGaussianProgram;
extern RKShaderProgram         gRadianceGenProgram;
extern RKShaderProgram         gIrradianceGenProgram;
extern RKShaderProgram         gGlowCombineFXAAProgram;
extern RKShaderProgram         gDebugProgram;
enum NormalDebugShaderVariant : S32
{
    NORMAL_DEBUG_SHADER_DEFAULT,
    NORMAL_DEBUG_SHADER_WITH_TANGENTS,
    NORMAL_DEBUG_SHADER_COUNT
};
extern RKShaderProgram         gNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT];
extern RKShaderProgram         gBenchmarkProgram;
extern RKShaderProgram         gReflectionProbeDisplayProgram;
extern RKShaderProgram         gCopyProgram;
extern RKShaderProgram         gCopyDepthProgram;
extern RKShaderProgram         gPBRTerrainBakeProgram;
extern RKShaderProgram         gDrawColorProgram;

//output tex0[tc0] - tex1[tc1]
extern RKShaderProgram         gTwoTextureCompareProgram;
//discard some fragments based on user-set color tolerance
extern RKShaderProgram         gOneTextureFilterProgram;


//object shaders
extern RKShaderProgram     gObjectPreviewProgram;
extern RKShaderProgram        gPhysicsPreviewProgram;
extern RKShaderProgram     gObjectAlphaMaskNoColorProgram;

//environment shaders
extern RKShaderProgram         gWaterPullProgram;          // <FS> the table form of the water planes (WATER_PULL: the planes' vertex buffers read by address)
extern RKShaderProgram         gUnderWaterPullProgram;
extern RKShaderProgram         gWaterPullRtProgram;        // <FS> the water with the ray queries of the stock water (a session with ray queries)
extern RKShaderProgram         gWaterPullRtReflectionProgram;       // <FS> and the one that also traces the reflection (RKVkRTWater)
bool rk_water_rt_reflection();   // it was made (the material table can be on)
bool rk_water_rt();              // the water seen from above is drawn with it (a session with ray queries and sun shadows)
extern RKShaderProgram         gGlowProgram;
extern RKShaderProgram         gGlowExtractProgram;

//interface shaders
extern RKShaderProgram         gHighlightProgram;
extern RKShaderProgram         gHighlightPullSkinProgram;      // K6 B1: the rigged Highlight Transparent draws (native stand-in)
extern RKShaderProgram         gDebugBatchPullProgram;         // K6 B1: the Batch Size / Texture Animation views (native stand-ins)
extern RKShaderProgram         gDebugBatchPullSkinProgram;
extern RKShaderProgram         gHighlightNormalProgram;
extern RKShaderProgram         gHighlightSpecularProgram;

extern RKShaderProgram         gDeferredHighlightProgram;

extern RKShaderProgram         gPathfindingProgram;
extern RKShaderProgram         gPathfindingNoNormalsProgram;

// avatar shader handles
extern RKShaderProgram         gAvatarProgram;
extern RKShaderProgram         gImpostorProgram;

// Post Process Shaders
extern RKShaderProgram         gPostVignetteProgram;   // <FS:CR> Import Vignette from Exodus
extern RKShaderProgram         gPostSnapshotFrameProgram;   // <FS:Beq/> Snapshot Frame overlay

// Deferred rendering shaders
extern RKShaderProgram         gDeferredImpostorProgram;
extern RKShaderProgram         gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram;
extern RKShaderProgram         gDeferredNonIndexedDiffuseProgram;
extern RKShaderProgram         gDeferredMultiLightProgram;
extern RKShaderProgram         gDeferredMultiSpotLightProgram;
extern RKShaderProgram         gDeferredSunProgram;
extern RKShaderProgram         gDeferredSunProbeProgram;
extern RKShaderProgram         gHazeProgram;
extern RKShaderProgram         gWaterHazePullProgram;       // <FS> the table form of the water patches of the haze
extern RKShaderProgram         gDeferredBlurLightProgram;
extern RKShaderProgram         gNativeSunHalfProgram;           // <FS> native only (rkvknative.h): the sun lightmap at half resolution
extern RKShaderProgram         gNativeLightUpsampleProgram;     // <FS> native only: the depth aware upsample of the half resolution lightmap
extern RKShaderProgram         gDeferredAvatarProgram;
extern RKShaderProgram         gDeferredSoftenProgram;
extern RKShaderProgram         gDeferredRtReflectionProgram;     // <FS> the traced glossy reflection as a pass of its own (rtReflectionF.glsl, doc/vulkan/PLAN-rt-reflection-pass.md)
extern RKShaderProgram         gDeferredShadowCubeProgram;
extern RKShaderProgram         gDeferredShadowPullProgram;   // <FS> render packets: reads vertices and indices by address (Vulkan mirror only)
extern RKShaderProgram         gDeferredShadowAlphaPullProgram;   // <FS> render packets: the same for alpha masked casters, textures from the bindless table
extern RKShaderProgram         gDeferredShadowSkinPullProgram;   // <FS> render packets: rigged casters (weights and palette read by address)
extern RKShaderProgram         gDeferredAlphaPullProgram;            // <FS> S5a: the lit alpha pass drawn from the scene table (alphaPullV + alphaF, textures from the bindless table)
extern RKShaderProgram         gDeferredFullbrightAlphaPullProgram;  // <FS> S5a: ... and the fullbright alpha pass (alphaPullV with IS_FULLBRIGHT_PULL + fullbrightF)
extern RKShaderProgram         gDeferredAlphaUberPullProgram;          // <FS> item D1: the lit alpha pull program that also draws fullbright alpha draws (ALPHA_UBER)
extern RKShaderProgram         gDeferredAlphaUberPullSkinProgram;
extern RKShaderProgram         gDeferredAlphaUberMatPullProgram;       // <FS> item D2: the merged lit alpha program that also draws the material alpha draws
extern RKShaderProgram         gDeferredAlphaUberMatPullSkinProgram;
extern RKShaderProgram         gDeferredTerrainPullProgram;           // <FS> S6.6a: the terrain pass from the geometry packet path (terrainPullV + terrainF)
extern RKShaderProgram         gDeferredTreePullProgram;              // <FS> the Linden tree pass of the G-buffer from the geometry packet path (treeV + treeF with RK_VERTEX_PULL)
extern RKShaderProgram         gDeferredPBRTerrainPullProgram[];       // <FS> S6.6a: the same for the PBR terrain (one per paint type)
extern RKShaderProgram         gDeferredFullbrightPullProgram;        // <FS> S6 X1: the opaque fullbright pass from the scene table, static and rigged
extern RKShaderProgram         gDeferredFullbrightPullSkinProgram;
extern RKShaderProgram         gDeferredFullbrightShinyPullProgram;        // <FS> J1 K4: the fullbright shiny pass from the scene table, static and rigged (alphaPullV with IS_SHINY_PULL + fullbrightShinyF)
extern RKShaderProgram         gDeferredFullbrightShinyPullSkinProgram;
extern RKShaderProgram         gHUDFullbrightPullProgram;        // <FS> J1 K6 S1: the HUD's fullbright pass from the scene table (the fullbright pull program with IS_HUD)
extern RKShaderProgram         gHUDFullbrightMaskPullProgram;        // <FS> J1 K6 S1: the HUD's alpha masked fullbright and fullbright shiny passes from the scene table
extern RKShaderProgram         gHUDFullbrightShinyPullProgram;
extern RKShaderProgram         gHUDPBROpaquePullProgram;         // <FS> J1 K6 S1: the HUD's glTF opaque / mask pass from the scene table (the PBR opaque pull program with IS_HUD)
extern RKShaderProgram         gDeferredBumpPullProgram;        // <FS> J1 K4: the deferred bump pass from the scene table, static and rigged (alphaPullV with IS_BUMP_PULL + bumpF)
extern RKShaderProgram         gDeferredBumpPullSkinProgram;
extern RKShaderProgram         gDeferredEmbossPullProgram;        // <FS> J1 K4: the post deferred emboss (post bump) pass from the scene table, static and rigged (alphaPullV with IS_EMBOSS_PULL + objects/bumpF)
extern RKShaderProgram         gDeferredEmbossPullSkinProgram;
extern RKShaderProgram         gDeferredDiffuseAlphaMaskPullProgram;       // <FS> S6 X3: the alpha masked passes from the scene table (static and rigged)
extern RKShaderProgram         gDeferredDiffuseAlphaMaskPullSkinProgram;
extern RKShaderProgram         gDeferredFullbrightAlphaMaskPullProgram;
extern RKShaderProgram         gDeferredFullbrightAlphaMaskPullSkinProgram;
extern RKShaderProgram         gDeferredPBROpaquePullProgram;             // <FS> S6 X4: the glTF opaque / mask G-buffer pass from the scene table
extern RKShaderProgram         gDeferredPBROpaquePullSkinProgram;
extern RKShaderProgram         gDeferredLightPullProgram;                 // <FS> S6 LV1: the point light volumes from one record
extern RKShaderProgram         gDeferredSpotLightPullProgram;             // <FS> S6 LV2: the projectors from one record
extern RKShaderProgram         gDeferredPBRAlphaPullProgram;          // <FS> S5c: the PBR (glTF) blend pass, vertices by address, the four textures from the bindless table
extern RKShaderProgram         gDeferredAlphaPullSkinProgram;          // <FS> S5d: the rigged variants of the alpha pull programs (weights and palette by address, indexed draws)
extern RKShaderProgram         gDeferredFullbrightAlphaPullSkinProgram;
extern RKShaderProgram         gDeferredPBRAlphaPullSkinProgram;
extern RKShaderProgram         gDeferredEmissivePullProgram;          // <FS> S5e: the glow second draw of the alpha items from the table (alpha only): plain, rigged, PBR, PBR rigged
extern RKShaderProgram         gDeferredEmissivePullSkinProgram;
extern RKShaderProgram         gPBRGlowPullProgram;
extern RKShaderProgram         gPBRGlowPullSkinProgram;
extern RKShaderProgram         gHUDFullbrightAlphaPullProgram;         // <FS> S5f F4: the HUD, impostor and depth only (mask) variants of the alpha pull programs
extern RKShaderProgram         gHUDPBRAlphaPullProgram;
extern RKShaderProgram         gDeferredAlphaImpostorPullProgram;
extern RKShaderProgram         gDeferredAlphaImpostorPullSkinProgram;
extern RKShaderProgram         gDeferredFullbrightMaskPullProgram;
extern RKShaderProgram         gDeferredFullbrightMaskPullSkinProgram;
extern RKShaderProgram         gDeferredDiffusePullProgram;      // <FS> render packets: the deferred diffuse pass, vertices by address, textures from the bindless table
extern RKShaderProgram         gDeferredMaterialPullProgram[16]; // <FS> render packets: the deferred materials passes (the unskinned indices of gDeferredMaterialProgram)
extern RKShaderProgram         gDeferredDiffusePullSkinProgram;  // <FS> S4 A1: the rigged variants of the two (weights and palette read by address)
extern RKShaderProgram         gDeferredMaterialPullSkinProgram[16];
extern RKShaderProgram         gDeferredPostProgram;
extern RKShaderProgram         gDeferredCoFProgram;
extern RKShaderProgram         gDeferredDoFCombineProgram;
extern RKShaderProgram         gFXAAProgram[4];
extern RKShaderProgram         gSMAAEdgeDetectProgram[4];
extern RKShaderProgram         gSMAABlendWeightsProgram[4];
extern RKShaderProgram         gSMAANeighborhoodBlendProgram[4];
extern RKShaderProgram         gCASProgram;
extern RKShaderProgram         gCASLegacyGammaProgram;
extern RKShaderProgram         gDeferredPostNoDoFProgram;
extern RKShaderProgram         gDeferredPostNoDoFNoiseProgram;
extern RKShaderProgram         gDeferredPostGammaCorrectProgram;
extern RKShaderProgram         gLegacyPostGammaCorrectProgram;
extern RKShaderProgram         gDeferredPostTonemapProgram;
extern RKShaderProgram         gNoPostTonemapProgram;
extern RKShaderProgram         gDeferredPostTonemapGammaCorrectProgram;
extern RKShaderProgram         gNoPostTonemapGammaCorrectProgram;
extern RKShaderProgram         gDeferredPostTonemapLegacyGammaCorrectProgram;
extern RKShaderProgram         gNoPostTonemapLegacyGammaCorrectProgram;
extern RKShaderProgram         gExposureProgram;
extern RKShaderProgram         gExposureProgramNoFade;
extern RKShaderProgram         gLuminanceProgram;
extern RKShaderProgram         gMotionVectorProgram;
extern RKShaderProgram         gOpaqueSnapProgram;            // <FS> the lit scene colour before the post-deferred geometry (opaque_snap.frag)
extern RKShaderProgram         gCoverageClearProgram;         // <FS> sets the blended pass's coverage attachment to 0 (coverage_clear.frag)
extern RKShaderProgram         gReactiveMaskProgram;          // <FS> the upscaler's reactive mask (reactive_mask.frag)
extern RKShaderProgram         gWorldDepthSnapProgram;       // <FS> the world pass's depth as a colour texture (depth_snap.frag)
extern RKShaderProgram         gRtShadowFilterProgram;        // <FS> the penumbra of the ray traced sun shadow from the blocker distance (rtShadowFilterF.glsl)
extern RKShaderProgram         gRtShadowPrepProgram;          // <FS> its input: the hard sun shadow, the half-width and the blocker mask into the mipped target (rtShadowPrepF.glsl)
extern RKShaderProgram         gDnsrSunPackProgram, gDnsrSunUnpackProgram;       // <FS> the AMD shadow denoiser's hit mask of the sun lightmap and its result back into it
extern RKShaderProgram         gDnsrPrepProgram, gDnsrPackProgram, gDnsrUnpackProgram;       // <FS> the AMD reflection denoiser's inputs and its result back (PLAN-amd-denoiser.md)
extern RKShaderProgram         gNRRimLiftProgram;             // <FS> DLSS-NR: the rim lift after the neural renderer (nrRimLiftF.glsl)
extern RKShaderProgram         gAvatarVelocityDebugProgram;
extern RKShaderProgram         gRtMaterialDebugProgram;
extern RKShaderProgram         gDeferredAvatarShadowProgram;
extern RKShaderProgram         gDeferredAvatarAlphaShadowProgram;
extern RKShaderProgram         gDeferredAvatarAlphaMaskShadowProgram;
extern RKShaderProgram         gDeferredAvatarAlphaProgram;
extern RKShaderProgram         gEnvironmentMapProgram;
extern RKShaderProgram         gDeferredWLSkyProgram;
extern RKShaderProgram         gDeferredWLCloudProgram;
extern RKShaderProgram         gDeferredWLSunProgram;
extern RKShaderProgram         gDeferredWLMoonProgram;
extern RKShaderProgram         gDeferredStarProgram;
extern RKShaderProgram         gNormalMapGenProgram;
extern RKShaderProgram         gDeferredGenBrdfLutProgram;
extern RKShaderProgram         gDeferredBufferVisualProgram;
// [RLVa:KB] - @setsphere
extern RKShaderProgram         gRlvSphereProgram;
// [/RLVa:KB]

// Deferred materials shaders

extern RKShaderProgram         gDeferredPBRAlphaProgram;

// GLTF shaders
extern RKShaderProgram         gGLTFPBRMetallicRoughnessProgram;

// Encodes detail level for dropping textures, in accordance with the GLTF spec where possible
// 0 is highest detail, -1 drops emissive, etc
// Dropping metallic roughness is off-spec - Reserve for potato machines as needed
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#additional-textures
enum TerrainPBRDetail : S32
{
    TERRAIN_PBR_DETAIL_MAX                = 0,
    TERRAIN_PBR_DETAIL_EMISSIVE           = 0,
    TERRAIN_PBR_DETAIL_OCCLUSION          = -1,
    TERRAIN_PBR_DETAIL_NORMAL             = -2,
    TERRAIN_PBR_DETAIL_METALLIC_ROUGHNESS = -3,
    TERRAIN_PBR_DETAIL_BASE_COLOR         = -4,
    TERRAIN_PBR_DETAIL_MIN                = -4,
};
enum TerrainPaintType : U32
{
    // Use LLVLComposition::mDatap (heightmap) generated by generateHeights, plus noise from TERRAIN_ALPHARAMP
    TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE = 0,
    // Use paint map if PBR terrain, otherwise fall back to TERRAIN_PAINT_TYPE_HEIGHTMAP_WITH_NOISE
    TERRAIN_PAINT_TYPE_PBR_PAINTMAP         = 1,
    TERRAIN_PAINT_TYPE_COUNT                = 2,
};
#endif
