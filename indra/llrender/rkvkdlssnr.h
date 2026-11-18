/**
 * @file rkvkdlssnr.h
 * @brief DLSS-NR (DLSS 5 neural rendering) for the Vulkan mirror, through the NGX parameter interface (no public header exists). doc/vulkan/PLAN-dlss-nr.md.
 *        LL_VK_DLSS_NR=1: at device creation the viewer asks the NGX core what it knows about the feature (NGX feature id 18) and logs every answer (step one).
 *        2: the feature is created and evaluated on the finished window-size picture every frame, the result is not shown (step two, gate G2). 3: the result is shown (gate G3).
 *        Local testing only, never shipped: the only nvngx_dlssnr.dll we have is the NVIDIA-signed one from a mod bundle; with the flag unset not one line here runs.
 */

#ifndef RK_RKVKDLSSNR_H
#define RK_RKVKDLSSNR_H

#if defined(RK_VULKAN)

#include "rkvkcontext.h"

#include <vector>

typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;
typedef struct NVSDK_NGX_FeatureDiscoveryInfo NVSDK_NGX_FeatureDiscoveryInfo;

namespace RKVkDlssNr
{
    // the value of LL_VK_DLSS_NR (0 when unset): 1 probe only, 2 create and evaluate, 3 show the result
    int requested();

    // the NGX feature id of DLSS-NR: 18 (NVSDK_NGX_Feature_Reserved18 in the public enum; the driver's NGX core lists DLSSNR there)
    int featureId();

    // After NGX is initialised, with the capability parameters NGX owns and the discovery info the viewer uses (its project id, data path): logs the
    // DLSSNR.* capabilities, the feature requirements of ids 14..18, the extension lists of 18 and the scratch buffer size, and remembers the device and
    // whether the core said the feature is available. Only with requested() > 0.
    void probe(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* capabilities, const NVSDK_NGX_FeatureDiscoveryInfo& base);

    // Always, after NGX is initialised (the flag need not be set): asks the core whether DLSS-NR is available and supported here (DLSSNR.Available, the feature
    // requirements of id 18; both say no when nvngx_dlssnr.dll is not next to the viewer) and remembers the answer and the device. One log line.
    void queryAvailability(VkInstance instance, VkPhysicalDevice physical, VkDevice device, NVSDK_NGX_Parameter* capabilities, const NVSDK_NGX_FeatureDiscoveryInfo& base);
    // the core said available and supported (the preferences checkbox is enabled on this)
    bool available();

    // the core said available and supported, and no creation has failed since (whether to evaluate is the caller's: the preference, live, or the launcher's flag)
    bool ready();

    // the feature is not wanted for now (the preference went off): it is released once the frame that last used it has completed; call for a few frames
    void release();

    struct Params
    {
        RKVk::Image* mColor = nullptr;      // the finished picture, output size, sampled
        RKVk::Image* mDepth = nullptr;      // scene depth, render size (depth aspect)
        RKVk::Image* mMotion = nullptr;     // motion vectors, render size, RG: previous minus current position in render pixels
        RKVk::Image* mOutput = nullptr;     // output size, storage
        U32 mRenderWidth = 0, mRenderHeight = 0;    // of the depth and the motion vectors
        U32 mOutputWidth = 0, mOutputHeight = 0;    // of the colour and the output
        float mMotionScaleX = 1.f, mMotionScaleY = 1.f;
        bool mReset = false;                // history is useless (camera cut)
        U32 mQuality = 2;                   // PerfQualityValue (the feature's "performance mode"): 2 quality is the only one the snippet supports (the ratio callback rejects 0 and 1)
        S32 mPreset = 0;                    // DLSSNR.Hint.Render.Preset
        S32 mAutoMask = 0;                  // > 0: DLSSNR.UseAutoMask is set to it
        float mScalingRatio = 0.f;          // > 0: DLSSNR.ScalingRatio at creation (the network runs at the output size times this and its answer is upsampled; cost falls with the square)
        // the look controls (the bundle's config: intensity, local tone and local structure 0..2 with 1 the model's own answer; skin -1..2, -1 follows structure; style 0 standard, 1 natural, 2 cinematic)
        float mStyle = 0.f, mIntensity = 1.f, mGlobalTone = 1.f, mLocalTone = 1.f, mLocalStructure = 1.f, mSkinStructure = -1.f;
    };

    // Records the evaluation into 'cmd'. The images must already be in the layouts RKVkNgx::inputLayout / outputLayout name. Creates the feature on first
    // use and when the output size, quality or preset change (the old one is released once its frame has completed). False if anything failed; a failed
    // creation turns the feature off for the session.
    bool evaluate(VkCommandBuffer cmd, const Params& p);

    // before NGX shuts down: releases the feature and the parameter object
    void shutdown();
}

#endif // RK_VULKAN
#endif // RK_RKVKDLSSNR_H
