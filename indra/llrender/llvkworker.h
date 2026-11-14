/**
 * @file llvkworker.h
 * @brief The Vulkan mirror's worker side: what the render thread keeps and runs for the typed command records (see llvkworker.cpp and llvkasync.cpp).
 *
 * There are no intercepted GL functions any more (F2.1): the viewer writes typed records (llvkstate.h, llvktex.h, llvkbuf.h, llvktarget.h, llvkquery.h, llvkasync.h)
 * and the worker plays them. This header only has what the two translation units and the mirror share.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#if defined(LL_VULKAN) && !defined(LL_LLVKGL_H)
#define LL_LLVKGL_H

#include <mutex>
#include "llvkbridge.h"
#include "llvkasync.h"

namespace LLVkWorker
{
    // true while the mirror is initialised and accepting calls
    extern bool gActive;
    // every mirror entry point of the inline mode (before the queue starts) runs under this lock
    extern LLVkBridge::Mutex gMutex;


    // true when the mirror owns the resources

    // names for the resources the producer creates: handed out on the calling thread, created on the worker (LLVkAsync::resourceCreate)
    void own_gen(int kind, S32 n, U32* out);
    // the worker side of the lifetime records: kind is an LLVkAsync::ResourceKind
    void resource_create(unsigned kind, int n, const unsigned* ids);
    void resource_destroy(unsigned kind, int n, const unsigned* ids);
    // S6.2 (LL_VK_PASSCLOSE=3): forgets everything a pass could inherit: program uniform blocks, texture units, uniform block bindings, constant attributes, the vertex binding, the render state, the program
    void pass_poison();
    // the draw refusals counted so far in this frame: no program, no target, no vertex input, no index buffer, refused by the renderer, outside a frame (S6 census, per pass)
    void refusals(unsigned long long out[6]);
    // DLSS: runs the upscaler on four mirror-owned textures (colour, depth, motion vectors -> output); the gMutex must be held
    void on_upscale(const LLVkBridge::UpscaleArgs& args);
    // the AMD reflection denoiser on mirror-owned textures (the gMutex must be held)
    void on_denoise_reflections(const LLVkBridge::DenoiseArgs& args);
    // the GL texture whose image has this view id (device lost report)
    std::string describe_texture_of_view(U64 view_id);
    // what the worker holds for each GL texture name (its image, its levels, its bindless handle and what that handle shows now, with the GL texture that owns the image the handle points at): logged
    // as "own avatar texture <label>" lines (worker thread; the viewer's Show Textures command asks for it for the bake slots of the agent's avatar, see LLVkBridge::describeTextures)
    void describe_textures(const std::vector<U32>& names, const std::vector<std::string>& labels);
}

#endif // LL_VULKAN
