/**
 * @file llvkprobe.h
 * @brief Phase 1 Vulkan bring-up: a separate window with its own Vulkan swapchain that is cleared to an
 *        animated colour every viewer frame. Proves that instance/device/swapchain creation, command
 *        recording, submission and presentation all work inside the viewer process. Does not touch the
 *        OpenGL pipeline.
 *
 * Compiled only when the viewer is configured with -DUSE_VULKAN=ON; otherwise every call is a no-op.
 */

#ifndef LL_LLVKPROBE_H
#define LL_LLVKPROBE_H

namespace LLVkProbe
{
    // true if this build includes the Vulkan code
    bool isCompiledIn();

    // create (true) or destroy (false) the probe window and Vulkan objects; cheap if already in that state
    void setEnabled(bool enabled);

    // draw one frame; no-op unless enabled and initialised
    void frame();

    // destroy everything (also called by setEnabled(false) and when the probe window is closed)
    void shutdown();
}

#endif // LL_LLVKPROBE_H
