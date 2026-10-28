/**
 * @file llvkdlssnr.h
 * @brief DLSS-NR (DLSS 5 neural rendering) for the Vulkan mirror, through the NGX parameter interface (no public header exists). doc/vulkan/PLAN-dlss-nr.md.
 *        LL_VK_DLSS_NR=1: at device creation the viewer asks the NGX core what it knows about the feature (NGX feature id 18) and logs every answer (step one).
 *        2: the feature is created and evaluated on the finished window-size picture every frame, the result is not shown (step two, gate G2). 3: the result is shown (gate G3).
 *        Local testing only, never shipped: the only nvngx_dlssnr.dll we have is the NVIDIA-signed one from a mod bundle; with the flag unset not one line here runs.
 */

#ifndef LL_LLVKDLSSNR_H
#define LL_LLVKDLSSNR_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"

#include <vector>

typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;
typedef struct NVSDK_NGX_FeatureDiscoveryInfo NVSDK_NGX_FeatureDiscoveryInfo;

namespace LLVkDlssNr
{
    // the value of LL_VK_DLSS_NR (0 when unset): 1 probe only, 2 create and evaluate, 3 show the result
    int requested();

    // the NGX feature id of DLSS-NR: 18 (NVSDK_NGX_Feature_Reserved18 in the public enum; the driver's NGX core lists DLSSNR there)
    int featureId();

    // After NGX is initialised, with the capability parameters NGX owns and the discovery info the viewer uses (its project id, data path): logs the
    // DLSSNR.* capabilities, the feature requirements of ids 14..18, the extension lists of 18 and the scratch buffer size, and remembers the device and
    // whether the core said the feature is available. Only with requested() > 0.
    void probe(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* capabilities, const NVSDK_NGX_FeatureDiscoveryInfo& base);

    // the probe ran, the core said available and supported, and no creation has failed since
    bool ready();

    struct Params
    {
        LLVk::Image* mColor = nullptr;      // the finished picture, output size, sampled
        LLVk::Image* mDepth = nullptr;      // scene depth, render size (depth aspect)
        LLVk::Image* mMotion = nullptr;     // motion vectors, render size, RG: previous minus current position in render pixels
        LLVk::Image* mOutput = nullptr;     // output size, storage
        U32 mRenderWidth = 0, mRenderHeight = 0;    // of the depth and the motion vectors
        U32 mOutputWidth = 0, mOutputHeight = 0;    // of the colour and the output
        float mMotionScaleX = 1.f, mMotionScaleY = 1.f;
        bool mReset = false;                // history is useless (camera cut)
        U32 mQuality = 1;                   // PerfQualityValue (the feature's "performance mode"): 0 performance, 1 balanced, 2 quality, 3 ultra performance, 4 ultra quality, 5 DLAA
        S32 mPreset = 0;                    // DLSSNR.Hint.Render.Preset
        S32 mAutoMask = 0;                  // > 0: DLSSNR.UseAutoMask is set to it
        float mScalingRatio = 0.f;          // > 0: DLSSNR.ScalingRatio at creation (the network runs at the output size times this and its answer is upsampled; cost falls with the square)
        // the look controls (ranges unknown; 0..1 assumed)
        float mStyle = 0.f, mIntensity = 1.f, mGlobalTone = 0.5f, mLocalTone = 0.5f, mLocalStructure = 0.5f, mSkinStructure = 0.5f;
    };

    // Records the evaluation into 'cmd'. The images must already be in the layouts LLVkDlss::inputLayout / outputLayout name. Creates the feature on first
    // use and when the output size, quality or preset change (the old one is released once its frame has completed). False if anything failed; a failed
    // creation turns the feature off for the session.
    bool evaluate(VkCommandBuffer cmd, const Params& p);

    // before NGX shuts down: releases the feature and the parameter object
    void shutdown();
}

#endif // LL_VULKAN
#endif // LL_LLVKDLSSNR_H
