/**
 * @file llvkdenoise.h
 * @brief The AMD FidelityFX Denoiser 1.3 (MIT, SDK v1.1.4) for the traced reflections of the Vulkan mirror (doc/vulkan/PLAN-amd-denoiser.md)
 *
 * The denoiser component and its Vulkan backend are linked from static libraries built from the SDK's source (see llrender/CMakeLists.txt); without them every call here says no.
 * The caller (the worker thread: LLVkWorker::on_denoise_reflections) hands it images and the camera; it keeps the denoiser's own helper images (the second radiance and the two variance images of
 * the ping-pong, the tile list of the dispatch and its indirect arguments) and the denoiser context, and makes them again when the render size changes.
 *
 * The matrices are the viewer's (OpenGL conventions: the depth buffer holds (z_ndc + 1) / 2, the texture coordinate v of a row is (y_ndc + 1) / 2); the conversion to the denoiser's (Direct3D
 * conventions: v = 1 - (y_ndc + 1) / 2, the depth is z_ndc) is done here.
 */
#ifndef LL_LLVKDENOISE_H
#define LL_LLVKDENOISE_H

#include "llvkcontext.h"

#include <vector>

namespace LLVkDenoise
{
    struct ReflectionParams
    {
        // images, all of the render size; every one is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL when the call is made and is left in it
        LLVk::Image* mRadiance = nullptr;       // RGBA16F, STORAGE usage: the traced reflection (rgb) and the length of the reflected ray (a, metres)
        LLVk::Image* mOutput = nullptr;         // RGBA16F, STORAGE usage: the denoised reflection (rgb)
        LLVk::Image* mDepth = nullptr;          // R32F: the raw depth buffer value
        LLVk::Image* mNormals = nullptr;        // RGBA16F: the world normal (xyz, -1 .. 1)
        LLVk::Image* mRoughness = nullptr;      // R16F or R8: the perceptual roughness (r)
        LLVk::Image* mMotion = nullptr;         // RG16F: the motion of the surface in pixels (previous minus current)
        U32 mWidth = 0, mHeight = 0;
        float mInvProjection[16] = {};          // column major; inverse of the projection that made the depth buffer (OpenGL clip space)
        float mInvView[16] = {};                // column major; eye space -> world
        float mPrevViewProjection[16] = {};     // column major; world -> OpenGL clip space of the last frame
        float mTemporalStability = 0.7f;        // 0 .. 1: how much of the history stays
        float mRoughnessThreshold = 0.5f;       // the roughness above which the denoiser leaves a pixel alone
        U32 mFrameIndex = 0;
        bool mReset = false;                    // the history is useless (camera cut, new size)
    };

    // called with the other upscaler initialisations: the SDK's Vulkan backend needs the instance, the physical device and the device
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // makes the helper images and buffers for this size (and the denoiser context); the images the dispatch needs in SHADER_READ_ONLY_OPTIMAL are added to `own`
    bool prepare(U32 width, U32 height, std::vector<LLVk::Image*>& own);
    // records the denoiser into `cmd` (the caller is inside Renderer::recordExternal with every image in its layout)
    bool reflections(VkCommandBuffer cmd, const ReflectionParams& params);
}

#endif // LL_LLVKDENOISE_H
