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

    // Add shaders to mShaderList for later uniform propagation
    // Will assert on redundant shader entries in debug builds
    void finalizeShaderList();

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

    // simple model of forward iterator
    // http://www.sgi.com/tech/stl/ForwardIterator.html
    class shader_iter
    {
    private:
        friend bool operator == (shader_iter const & a, shader_iter const & b);
        friend bool operator != (shader_iter const & a, shader_iter const & b);

        typedef std::vector<LLShaderProgram *>::const_iterator base_iter_t;
    public:
        shader_iter()
        {
        }

        shader_iter(base_iter_t iter) : mIter(iter)
        {
        }

        LLShaderProgram & operator * () const
        {
            return **mIter;
        }

        LLShaderProgram * operator -> () const
        {
            return *mIter;
        }

        shader_iter & operator++ ()
        {
            ++mIter;
            return *this;
        }

        shader_iter operator++ (int)
        {
            return mIter++;
        }

    private:
        base_iter_t mIter;
    };

    shader_iter beginShaders() const;
    shader_iter endShaders() const;

    /* virtual */ std::string getShaderDirPrefix(void);

    /* virtual */ void updateShaderUniforms(LLShaderProgram * shader);

private:
    // the list of shaders we need to propagate parameters to.
    std::vector<LLShaderProgram *> mShaderList;

}; //LLViewerShaderMgr

inline bool operator == (LLViewerShaderMgr::shader_iter const & a, LLViewerShaderMgr::shader_iter const & b)
{
    return a.mIter == b.mIter;
}

inline bool operator != (LLViewerShaderMgr::shader_iter const & a, LLViewerShaderMgr::shader_iter const & b)
{
    return a.mIter != b.mIter;
}

extern LLVector4            gShinyOrigin;

//utility shaders
extern LLShaderProgram         gGlowCombineProgram;
extern LLShaderProgram         gReflectionMipProgram;
extern LLShaderProgram         gGaussianProgram;
extern LLShaderProgram         gRadianceGenProgram;
extern LLShaderProgram         gIrradianceGenProgram;
extern LLShaderProgram         gGlowCombineFXAAProgram;
extern LLShaderProgram         gDebugProgram;
enum NormalDebugShaderVariant : S32
{
    NORMAL_DEBUG_SHADER_DEFAULT,
    NORMAL_DEBUG_SHADER_WITH_TANGENTS,
    NORMAL_DEBUG_SHADER_COUNT
};
extern LLShaderProgram         gNormalDebugProgram[NORMAL_DEBUG_SHADER_COUNT];
extern LLShaderProgram         gBenchmarkProgram;
extern LLShaderProgram         gReflectionProbeDisplayProgram;
extern LLShaderProgram         gCopyProgram;
extern LLShaderProgram         gCopyDepthProgram;
extern LLShaderProgram         gPBRTerrainBakeProgram;
extern LLShaderProgram         gDrawColorProgram;

//output tex0[tc0] - tex1[tc1]
extern LLShaderProgram         gTwoTextureCompareProgram;
//discard some fragments based on user-set color tolerance
extern LLShaderProgram         gOneTextureFilterProgram;


//object shaders
extern LLShaderProgram     gObjectPreviewProgram;
extern LLShaderProgram        gPhysicsPreviewProgram;
extern LLShaderProgram     gObjectAlphaMaskNoColorProgram;

//environment shaders
extern LLShaderProgram         gWaterPullProgram;          // <FS> the table form of the water planes (WATER_PULL: the planes' vertex buffers read by address)
extern LLShaderProgram         gUnderWaterPullProgram;
extern LLShaderProgram         gWaterPullRtProgram;        // <FS> the water with the ray queries of the stock water (a session with ray queries)
extern LLShaderProgram         gWaterPullRtReflectionProgram;       // <FS> and the one that also traces the reflection (FSVkRTWater)
bool ll_water_rt_reflection();   // it was made (the material table can be on)
bool ll_water_rt();              // the water seen from above is drawn with it (a session with ray queries and sun shadows)
extern LLShaderProgram         gGlowProgram;
extern LLShaderProgram         gGlowExtractProgram;

//interface shaders
extern LLShaderProgram         gHighlightProgram;
extern LLShaderProgram         gHighlightPullSkinProgram;      // K6 B1: the rigged Highlight Transparent draws (native stand-in)
extern LLShaderProgram         gDebugBatchPullProgram;         // K6 B1: the Batch Size / Texture Animation views (native stand-ins)
extern LLShaderProgram         gDebugBatchPullSkinProgram;
extern LLShaderProgram         gHighlightNormalProgram;
extern LLShaderProgram         gHighlightSpecularProgram;

extern LLShaderProgram         gDeferredHighlightProgram;

extern LLShaderProgram         gPathfindingProgram;
extern LLShaderProgram         gPathfindingNoNormalsProgram;

// avatar shader handles
extern LLShaderProgram         gAvatarProgram;
extern LLShaderProgram         gImpostorProgram;

// Post Process Shaders
extern LLShaderProgram         gPostVignetteProgram;   // <FS:CR> Import Vignette from Exodus
extern LLShaderProgram         gPostSnapshotFrameProgram;   // <FS:Beq/> Snapshot Frame overlay

// Deferred rendering shaders
extern LLShaderProgram         gDeferredImpostorProgram;
extern LLShaderProgram         gDeferredNonIndexedDiffuseAlphaMaskNoColorProgram;
extern LLShaderProgram         gDeferredNonIndexedDiffuseProgram;
extern LLShaderProgram         gDeferredMultiLightProgram;
extern LLShaderProgram         gDeferredMultiSpotLightProgram;
extern LLShaderProgram         gDeferredSunProgram;
extern LLShaderProgram         gDeferredSunProbeProgram;
extern LLShaderProgram         gHazeProgram;
extern LLShaderProgram         gWaterHazePullProgram;       // <FS> the table form of the water patches of the haze
extern LLShaderProgram         gDeferredBlurLightProgram;
extern LLShaderProgram         gNativeSunHalfProgram;           // <FS> native only (llvknative.h): the sun lightmap at half resolution
extern LLShaderProgram         gNativeLightUpsampleProgram;     // <FS> native only: the depth aware upsample of the half resolution lightmap
extern LLShaderProgram         gDeferredAvatarProgram;
extern LLShaderProgram         gDeferredSoftenProgram;
extern LLShaderProgram         gDeferredRtReflectionProgram;     // <FS> the traced glossy reflection as a pass of its own (rtReflectionF.glsl, doc/vulkan/PLAN-rt-reflection-pass.md)
extern LLShaderProgram         gDeferredShadowCubeProgram;
extern LLShaderProgram         gDeferredShadowPullProgram;   // <FS> render packets: reads vertices and indices by address (Vulkan mirror only)
extern LLShaderProgram         gDeferredShadowAlphaPullProgram;   // <FS> render packets: the same for alpha masked casters, textures from the bindless table
extern LLShaderProgram         gDeferredShadowSkinPullProgram;   // <FS> render packets: rigged casters (weights and palette read by address)
extern LLShaderProgram         gDeferredAlphaPullProgram;            // <FS> S5a: the lit alpha pass drawn from the scene table (alphaPullV + alphaF, textures from the bindless table)
extern LLShaderProgram         gDeferredFullbrightAlphaPullProgram;  // <FS> S5a: ... and the fullbright alpha pass (alphaPullV with IS_FULLBRIGHT_PULL + fullbrightF)
extern LLShaderProgram         gDeferredAlphaUberPullProgram;          // <FS> item D1: the lit alpha pull program that also draws fullbright alpha draws (ALPHA_UBER)
extern LLShaderProgram         gDeferredAlphaUberPullSkinProgram;
extern LLShaderProgram         gDeferredAlphaUberMatPullProgram;       // <FS> item D2: the merged lit alpha program that also draws the material alpha draws
extern LLShaderProgram         gDeferredAlphaUberMatPullSkinProgram;
extern LLShaderProgram         gDeferredTerrainPullProgram;           // <FS> S6.6a: the terrain pass from the geometry packet path (terrainPullV + terrainF)
extern LLShaderProgram         gDeferredTreePullProgram;              // <FS> the Linden tree pass of the G-buffer from the geometry packet path (treeV + treeF with LL_VERTEX_PULL)
extern LLShaderProgram         gDeferredPBRTerrainPullProgram[];       // <FS> S6.6a: the same for the PBR terrain (one per paint type)
extern LLShaderProgram         gDeferredFullbrightPullProgram;        // <FS> S6 X1: the opaque fullbright pass from the scene table, static and rigged
extern LLShaderProgram         gDeferredFullbrightPullSkinProgram;
extern LLShaderProgram         gDeferredFullbrightShinyPullProgram;        // <FS> J1 K4: the fullbright shiny pass from the scene table, static and rigged (alphaPullV with IS_SHINY_PULL + fullbrightShinyF)
extern LLShaderProgram         gDeferredFullbrightShinyPullSkinProgram;
extern LLShaderProgram         gHUDFullbrightPullProgram;        // <FS> J1 K6 S1: the HUD's fullbright pass from the scene table (the fullbright pull program with IS_HUD)
extern LLShaderProgram         gHUDFullbrightMaskPullProgram;        // <FS> J1 K6 S1: the HUD's alpha masked fullbright and fullbright shiny passes from the scene table
extern LLShaderProgram         gHUDFullbrightShinyPullProgram;
extern LLShaderProgram         gHUDPBROpaquePullProgram;         // <FS> J1 K6 S1: the HUD's glTF opaque / mask pass from the scene table (the PBR opaque pull program with IS_HUD)
extern LLShaderProgram         gDeferredBumpPullProgram;        // <FS> J1 K4: the deferred bump pass from the scene table, static and rigged (alphaPullV with IS_BUMP_PULL + bumpF)
extern LLShaderProgram         gDeferredBumpPullSkinProgram;
extern LLShaderProgram         gDeferredEmbossPullProgram;        // <FS> J1 K4: the post deferred emboss (post bump) pass from the scene table, static and rigged (alphaPullV with IS_EMBOSS_PULL + objects/bumpF)
extern LLShaderProgram         gDeferredEmbossPullSkinProgram;
extern LLShaderProgram         gDeferredDiffuseAlphaMaskPullProgram;       // <FS> S6 X3: the alpha masked passes from the scene table (static and rigged)
extern LLShaderProgram         gDeferredDiffuseAlphaMaskPullSkinProgram;
extern LLShaderProgram         gDeferredFullbrightAlphaMaskPullProgram;
extern LLShaderProgram         gDeferredFullbrightAlphaMaskPullSkinProgram;
extern LLShaderProgram         gDeferredPBROpaquePullProgram;             // <FS> S6 X4: the glTF opaque / mask G-buffer pass from the scene table
extern LLShaderProgram         gDeferredPBROpaquePullSkinProgram;
extern LLShaderProgram         gDeferredLightPullProgram;                 // <FS> S6 LV1: the point light volumes from one record
extern LLShaderProgram         gDeferredSpotLightPullProgram;             // <FS> S6 LV2: the projectors from one record
extern LLShaderProgram         gDeferredPBRAlphaPullProgram;          // <FS> S5c: the PBR (glTF) blend pass, vertices by address, the four textures from the bindless table
extern LLShaderProgram         gDeferredAlphaPullSkinProgram;          // <FS> S5d: the rigged variants of the alpha pull programs (weights and palette by address, indexed draws)
extern LLShaderProgram         gDeferredFullbrightAlphaPullSkinProgram;
extern LLShaderProgram         gDeferredPBRAlphaPullSkinProgram;
extern LLShaderProgram         gDeferredEmissivePullProgram;          // <FS> S5e: the glow second draw of the alpha items from the table (alpha only): plain, rigged, PBR, PBR rigged
extern LLShaderProgram         gDeferredEmissivePullSkinProgram;
extern LLShaderProgram         gPBRGlowPullProgram;
extern LLShaderProgram         gPBRGlowPullSkinProgram;
extern LLShaderProgram         gHUDFullbrightAlphaPullProgram;         // <FS> S5f F4: the HUD, impostor and depth only (mask) variants of the alpha pull programs
extern LLShaderProgram         gHUDPBRAlphaPullProgram;
extern LLShaderProgram         gDeferredAlphaImpostorPullProgram;
extern LLShaderProgram         gDeferredAlphaImpostorPullSkinProgram;
extern LLShaderProgram         gDeferredFullbrightMaskPullProgram;
extern LLShaderProgram         gDeferredFullbrightMaskPullSkinProgram;
extern LLShaderProgram         gDeferredDiffusePullProgram;      // <FS> render packets: the deferred diffuse pass, vertices by address, textures from the bindless table
extern LLShaderProgram         gDeferredMaterialPullProgram[16]; // <FS> render packets: the deferred materials passes (the unskinned indices of gDeferredMaterialProgram)
extern LLShaderProgram         gDeferredDiffusePullSkinProgram;  // <FS> S4 A1: the rigged variants of the two (weights and palette read by address)
extern LLShaderProgram         gDeferredMaterialPullSkinProgram[16];
extern LLShaderProgram         gDeferredPostProgram;
extern LLShaderProgram         gDeferredCoFProgram;
extern LLShaderProgram         gDeferredDoFCombineProgram;
extern LLShaderProgram         gFXAAProgram[4];
extern LLShaderProgram         gSMAAEdgeDetectProgram[4];
extern LLShaderProgram         gSMAABlendWeightsProgram[4];
extern LLShaderProgram         gSMAANeighborhoodBlendProgram[4];
extern LLShaderProgram         gCASProgram;
extern LLShaderProgram         gCASLegacyGammaProgram;
extern LLShaderProgram         gDeferredPostNoDoFProgram;
extern LLShaderProgram         gDeferredPostNoDoFNoiseProgram;
extern LLShaderProgram         gDeferredPostGammaCorrectProgram;
extern LLShaderProgram         gLegacyPostGammaCorrectProgram;
extern LLShaderProgram         gDeferredPostTonemapProgram;
extern LLShaderProgram         gNoPostTonemapProgram;
extern LLShaderProgram         gDeferredPostTonemapGammaCorrectProgram;
extern LLShaderProgram         gNoPostTonemapGammaCorrectProgram;
extern LLShaderProgram         gDeferredPostTonemapLegacyGammaCorrectProgram;
extern LLShaderProgram         gNoPostTonemapLegacyGammaCorrectProgram;
extern LLShaderProgram         gExposureProgram;
extern LLShaderProgram         gExposureProgramNoFade;
extern LLShaderProgram         gLuminanceProgram;
extern LLShaderProgram         gMotionVectorProgram;
extern LLShaderProgram         gAvatarBodyVelocityProgram;
extern LLShaderProgram         gAvatarVelocityPullProgram;
extern LLShaderProgram         gObjectVelocityPullProgram;
extern LLShaderProgram         gRtShadowFilterProgram;        // <FS> the penumbra of the ray traced sun shadow from the blocker distance (rtShadowFilterF.glsl)
extern LLShaderProgram         gRtShadowPrepProgram;          // <FS> its input: the hard sun shadow, the half-width and the blocker mask into the mipped target (rtShadowPrepF.glsl)
extern LLShaderProgram         gRRGuidesProgram;
extern LLShaderProgram         gNRRimLiftProgram;             // <FS> DLSS-NR: the rim lift after the neural renderer (nrRimLiftF.glsl)
extern LLShaderProgram         gRRReflMotionProgram;
extern LLShaderProgram         gAvatarVelocityDebugProgram;
extern LLShaderProgram         gRtMaterialDebugProgram;
extern LLShaderProgram         gDeferredAvatarShadowProgram;
extern LLShaderProgram         gDeferredAvatarAlphaShadowProgram;
extern LLShaderProgram         gDeferredAvatarAlphaMaskShadowProgram;
extern LLShaderProgram         gDeferredAvatarAlphaProgram;
extern LLShaderProgram         gEnvironmentMapProgram;
extern LLShaderProgram         gDeferredWLSkyProgram;
extern LLShaderProgram         gDeferredWLCloudProgram;
extern LLShaderProgram         gDeferredWLSunProgram;
extern LLShaderProgram         gDeferredWLMoonProgram;
extern LLShaderProgram         gDeferredStarProgram;
extern LLShaderProgram         gNormalMapGenProgram;
extern LLShaderProgram         gDeferredGenBrdfLutProgram;
extern LLShaderProgram         gDeferredBufferVisualProgram;
// [RLVa:KB] - @setsphere
extern LLShaderProgram         gRlvSphereProgram;
// [/RLVa:KB]

// Deferred materials shaders

extern LLShaderProgram         gDeferredPBRAlphaProgram;

// GLTF shaders
extern LLShaderProgram         gGLTFPBRMetallicRoughnessProgram;

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
