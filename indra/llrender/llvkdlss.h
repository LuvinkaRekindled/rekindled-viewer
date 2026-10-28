/**
 * @file llvkdlss.h
 * @brief NVIDIA DLSS (super resolution) for the Vulkan mirror, through the NGX SDK. Experimental; compiled in only when the SDK was found
 *        at configure time (LL_DLSS). Everything here is a stub that reports "not available" otherwise.
 *
 * The viewer renders the world at a reduced size with a jittered projection, hands over the finished picture, the depth buffer and camera
 * motion vectors, and gets the window-sized picture back. All of it runs on the thread that replays the GL stream.
 */

#ifndef LL_LLVKDLSS_H
#define LL_LLVKDLSS_H

#if defined(LL_VULKAN)

#include "llvkcontext.h"

#include <vector>

namespace LLVkDlss
{
    // LL_VK_DLSS is set (the FSVkDLSS setting)
    bool requested();

    // before the instance / device are created: the extensions NGX wants on top of ours (only the ones that exist are added)
    void addInstanceExtensions(std::vector<const char*>& out);
    void addDeviceExtensions(VkInstance instance, VkPhysicalDevice device, std::vector<const char*>& out);

    // after the device exists; false (and a log line saying why) if DLSS cannot be used here
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // C6: NGX says DLSS ray reconstruction (the denoiser + upscaler for ray traced input, nvngx_dlssd.dll) can run here. Known after init(); false while DLSS is off.
    bool rayReconstructionAvailable();

    struct Params
    {
        LLVk::Image* mColor = nullptr;            // render size, sampled
        LLVk::Image* mDepth = nullptr;            // render size, sampled (depth aspect)
        LLVk::Image* mMotion = nullptr;           // render size, RG: previous minus current position in pixels
        LLVk::Image* mOutput = nullptr;           // window size, storage
        U32 mRenderWidth = 0, mRenderHeight = 0;
        U32 mOutputWidth = 0, mOutputHeight = 0;
        float mJitterX = 0.f, mJitterY = 0.f;   // pixels of the render size, +x right, +y down
        float mMotionScaleX = 1.f, mMotionScaleY = 1.f;
        bool mReset = false;                // history is useless (camera cut)
        bool mHdr = false;                  // the colour is linear HDR rather than display-referred
        // ray reconstruction (the denoiser + upscaler for noisy ray traced input) runs instead of super resolution when this is set: the colour must be noisy linear HDR radiance, the guides at render size
        bool mRayReconstruction = false;
        LLVk::Image* mDiffuseAlbedo = nullptr;      // RGBA16F: the diffuse albedo of the visible surface (linear)
        LLVk::Image* mSpecularAlbedo = nullptr;     // RGBA16F: the specular albedo (F0 times the environment BRDF)
        LLVk::Image* mNormals = nullptr;            // RGBA16F: world space normal in xyz, roughness in w
        LLVk::Image* mReflectionMotion = nullptr;   // RG16F: motion vectors of the reflected image (same units as mMotion)
        LLVk::Image* mHitDistance = nullptr;        // R16F: specular hit distance (traced reflections), with the two matrices below
        float mWorldToView[16] = {}, mViewToClip[16] = {};
        LLVk::Image* mExposure = nullptr;           // 1x1 R16F: the factor the tonemap multiplies the radiance with (the network works on exposed values)
        S32 mPreset = 0;                            // the ray reconstruction model: 0 default, 4 D, 5 E (transformer), 6 F
        // <FS> what the FSR backend wants and DLSS does not (llvkfsr.cpp)
        float mCameraNear = 0.1f, mCameraFar = 512.f;   // metres
        float mCameraFovY = 1.f;                        // vertical field of view, radians
        float mFrameTimeMs = 16.6f;                     // the time since the last frame
    };

    // Records the upscale into 'cmd'. The images must already be in the layouts layoutNeeded() names. Creates the DLSS feature on first use
    // and whenever the sizes change. False if anything failed (then nothing was recorded that matters).
    bool evaluate(VkCommandBuffer cmd, const Params& p);

    // The extra texture sharpness DLSS wants because it draws fewer pixels than it shows: log2(render scale), a negative number added to the mip level
    // every filtered texture lookup picks (0 when DLSS is off). LL_VK_DLSS_LODBIAS overrides it.
    float lodBias();
    // the render scale changed (a different DLSS mode, or DLSS switched on or off): samplers made from now on use this bias; ignored while LL_VK_DLSS_LODBIAS is set
    void setLodBias(float bias);

    // layouts NGX reads / writes the images in
    VkImageLayout inputLayout(const LLVk::Image& image);
    VkImageLayout outputLayout();
}

#endif // LL_VULKAN
#endif // LL_LLVKDLSS_H
