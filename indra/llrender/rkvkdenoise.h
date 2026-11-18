/**
 * @file rkvkdenoise.h
 * @brief The AMD FidelityFX Denoiser 1.3 (MIT, SDK v1.1.4) for the traced reflections and the traced sun shadow of the Vulkan mirror (doc/vulkan/PLAN-amd-denoiser.md)
 *
 * The denoiser component and its Vulkan backend are linked from static libraries built from the SDK's source (see llrender/CMakeLists.txt); without them every call here says no.
 * The caller (the worker thread: RKVkWorker::on_denoise_reflections / on_denoise_shadows) hands it images and the camera; it keeps the denoiser's own helper images (for the reflections the second
 * radiance and the two variance images of the ping-pong, the tile list of the dispatch and its indirect arguments; for the shadows the hit mask) and the denoiser context, and makes them again when
 * the render size changes. Each kind has a context of its own, so turning one kind on or off never rebuilds the other's.
 *
 * The matrices are the viewer's (OpenGL conventions: the depth buffer holds (z_ndc + 1) / 2, the texture coordinate v of a row is (y_ndc + 1) / 2); the conversion to the denoiser's (Direct3D
 * conventions: v = 1 - (y_ndc + 1) / 2, the depth is z_ndc) is done here.
 */
#ifndef RK_RKVKDENOISE_H
#define RK_RKVKDENOISE_H

#include "rkvkcontext.h"

#include <vector>

namespace RKVkDenoise
{
    enum Kind : U32 { KIND_REFLECTIONS = 1, KIND_SHADOWS = 2 };

    struct ReflectionParams
    {
        // images, all of the render size; every one is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL when the call is made and is left in it
        RKVk::Image* mRadiance = nullptr;       // RGBA16F, STORAGE usage: the traced reflection (rgb) and the length of the reflected ray (a, metres)
        RKVk::Image* mOutput = nullptr;         // RGBA16F, STORAGE usage: the denoised reflection (rgb)
        RKVk::Image* mDepth = nullptr;          // R32F: the raw depth buffer value
        RKVk::Image* mNormals = nullptr;        // RGBA16F: the world normal (xyz, -1 .. 1)
        RKVk::Image* mRoughness = nullptr;      // R16F or R8: the perceptual roughness (r)
        RKVk::Image* mMotion = nullptr;         // RG16F: the motion of the surface in pixels (previous minus current)
        U32 mWidth = 0, mHeight = 0;
        float mInvProjection[16] = {};          // column major; inverse of the projection that made the depth buffer (OpenGL clip space)
        float mInvView[16] = {};                // column major; eye space -> world
        float mPrevViewProjection[16] = {};     // column major; world -> OpenGL clip space of the last frame
        float mTemporalStability = 0.7f;        // 0 .. 1: how much of the history stays
        float mRoughnessThreshold = 0.5f;       // the roughness above which the denoiser leaves a pixel alone
        U32 mFrameIndex = 0;
        bool mReset = false;                    // the history is useless (camera cut, new size)
    };

    struct ShadowParams
    {
        RKVk::Image* mHitSource = nullptr;      // RGBA8, ceil(width / 8) x ceil(height / 4): the hit mask as four bytes a tile (dnsr_sunpack.frag); copied into the denoiser's R32_UINT image
        RKVk::Image* mOutput = nullptr;         // RGBA8 render size, STORAGE usage: the filtered visibility in r (1 lit)
        RKVk::Image* mDepth = nullptr;          // R32F: the raw depth buffer value
        RKVk::Image* mNormals = nullptr;        // RGBA16F: the world normal (xyz, -1 .. 1)
        RKVk::Image* mMotion = nullptr;         // RG16F: the motion of the surface in pixels (previous minus current)
        U32 mWidth = 0, mHeight = 0;
        float mInvProjection[16] = {};          // column major; inverse of the projection that made the depth buffer (OpenGL clip space)
        float mReprojection[16] = {};           // column major; previous view projection * inverse(current view projection), both OpenGL
        float mViewProjInverse[16] = {};        // column major; inverse(current view projection) (OpenGL): clip -> world
        float mEye[3] = {};                     // the camera in the same world
        float mDepthSigma = 1.f;                // the denoiser's depth similarity factor
        float mHistoryBlend = 0.f;              // the weight of the new sample once the temporal history is settled (0 = the SDK's 0.05; larger: shorter trails, more noise; a patch of the SDK: doc/vulkan/ffx-sdk-patches)
        float mHistorySamples = 0.f;            // the samples it takes the history to settle (0 = the SDK's 8)
        U32 mFrameIndex = 0;
    };

    // called with the other upscaler initialisations: the SDK's Vulkan backend needs the instance, the physical device and the device
    bool init(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    bool ready();
    void shutdown();

    // makes the helper images and buffers for this size and these kinds (and the denoiser context); the images the dispatch needs in SHADER_READ_ONLY_OPTIMAL are added to `own`
    bool prepare(U32 width, U32 height, U32 kinds, std::vector<RKVk::Image*>& own);
    // records the denoiser into `cmd` (the caller is inside Renderer::recordExternal with every image in its layout)
    bool reflections(VkCommandBuffer cmd, const ReflectionParams& params);
    bool shadows(VkCommandBuffer cmd, const ShadowParams& params);
}

#endif // RK_RKVKDENOISE_H
