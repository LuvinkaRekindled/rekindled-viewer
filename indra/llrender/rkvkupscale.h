/**
 * @file rkvkupscale.h
 * @brief The upscaler of the Vulkan mirror: AMD FSR 3.1 (rkvkfsr.h), built from the FidelityFX SDK source. The callers talk to this. There is no other backend: DLSS super resolution and ray
 *        reconstruction were removed (doc/vulkan/PLAN-remove-nvidia.md); DLSS-NR is a separate local-build feature (rkvkngx.h, rkvkdlssnr.h).
 *        The facade stays so the callers need not know which library is behind it.
 */

#ifndef RK_RKVKUPSCALE_H
#define RK_RKVKUPSCALE_H

#if defined(RK_VULKAN)

#include "rkvkcontext.h"

#include <vector>

namespace RKVkUpscale
{
    struct Params
    {
        RKVk::Image* mColor = nullptr;            // render size, sampled
        RKVk::Image* mDepth = nullptr;            // render size, sampled (depth aspect)
        RKVk::Image* mMotion = nullptr;           // render size, RG: previous minus current position in pixels
        RKVk::Image* mOutput = nullptr;           // window size, storage
        RKVk::Image* mReactive = nullptr;         // render size, R8, sampled: the reactive mask (1 = blended content: trust the history less), optional
        U32 mRenderWidth = 0, mRenderHeight = 0;
        U32 mOutputWidth = 0, mOutputHeight = 0;
        float mJitterX = 0.f, mJitterY = 0.f;   // pixels of the render size, +x right, +y down
        float mMotionScaleX = 1.f, mMotionScaleY = 1.f;
        float mSharpness = 0.f;             // FSR's RCAS sharpening, 0 (off) to 1
        bool mReset = false;                // history is useless (camera cut)
        bool mHdr = false;                  // the colour is linear HDR rather than display-referred
        float mCameraNear = 0.1f, mCameraFar = 512.f;   // metres
        float mCameraFovY = 1.f;                        // vertical field of view, radians
        float mFrameTimeMs = 16.6f;                     // the time since the last frame
    };

    // an upscaler is wanted at all (RK_VK_DLSS is set by the RKVkDLSS setting: the name is a left over, it is the upscaler switch)
    bool requested();

    // before the instance / device are created: what the backend wants on top of ours
    void addInstanceExtensions(std::vector<const char*>& out);
    void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out);

    // after the device exists: starts the backend; false (and log lines saying why) if it cannot be used here
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    // the upscaler was started but could not make its context on this device (see RKVkFsr::failed)
    bool failed();
    void shutdown();

    // the active backend's name ("FSR", "none")
    const char* backendName();
    bool backendIsFsr();                // the upscaler in use is AMD FSR

    bool evaluate(VkCommandBuffer cmd, const Params& p);

    // the mip bias for filtered texture lookups at the current render scale (log2 of the scale; FSR asks for one more stop when it upscales).
    // RK_VK_DLSS_LODBIAS overrides it, RK_VK_DLSS_SCALE gives the start scale
    float lodBias();
    void setLodBias(float bias);

    VkImageLayout inputLayout(const RKVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // RK_VULKAN
#endif // RK_RKVKUPSCALE_H
