// The one function of the SDK's frame interpolation swapchain (FrameInterpolationSwapchain/, Windows code, not built here) that the Vulkan backend names. Frame generation is not used.
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* /*config*/)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}
