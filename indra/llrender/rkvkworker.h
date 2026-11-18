/**
 * @file rkvkworker.h
 * @brief The Vulkan mirror's worker side: what the render thread keeps and runs for the typed command records (see rkvkworker.cpp and rkvkasync.cpp).
 *
 * There are no intercepted GL functions any more (F2.1): the viewer writes typed records (rkvkstate.h, rkvktex.h, rkvkbuf.h, rkvktarget.h, rkvkquery.h, rkvkasync.h)
 * and the worker plays them. This header only has what the two translation units and the mirror share.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#if defined(RK_VULKAN) && !defined(RK_RKVKGL_H)
#define RK_RKVKGL_H

#include <mutex>
#include "rkvkbridge.h"
#include "rkvkasync.h"

namespace RKVkWorker
{
    // true while the mirror is initialised and accepting calls
    extern bool gActive;
    // every mirror entry point of the inline mode (before the queue starts) runs under this lock
    extern RKVkBridge::Mutex gMutex;


    // true when the mirror owns the resources

    // names for the resources the producer creates: handed out on the calling thread, created on the worker (RKVkAsync::resourceCreate)
    void own_gen(int kind, S32 n, U32* out);
    // the worker side of the lifetime records: kind is an RKVkAsync::ResourceKind
    void resource_create(unsigned kind, int n, const unsigned* ids);
    void resource_destroy(unsigned kind, int n, const unsigned* ids);
    // S6.2 (LL_VK_PASSCLOSE=3): forgets everything a pass could inherit: program uniform blocks, texture units, uniform block bindings, constant attributes, the vertex binding, the render state, the program
    void pass_poison();
    // the draw refusals counted so far in this frame: no program, no target, no vertex input, no index buffer, refused by the renderer, outside a frame (S6 census, per pass)
    void refusals(unsigned long long out[6]);
    // DLSS: runs the upscaler on four mirror-owned textures (colour, depth, motion vectors -> output); the gMutex must be held
    void on_upscale(const RKVkBridge::UpscaleArgs& args);
    // the AMD reflection denoiser on mirror-owned textures (the gMutex must be held)
    void on_denoise_reflections(const RKVkBridge::DenoiseArgs& args);
    // the GL texture whose image has this view id (device lost report)
    std::string describe_texture_of_view(U64 view_id);
    // what the worker holds for each GL texture name (its image, its levels, its bindless handle and what that handle shows now, with the GL texture that owns the image the handle points at): logged
    // as "own avatar texture <label>" lines (worker thread; the viewer's Show Textures command asks for it for the bake slots of the agent's avatar, see RKVkBridge::describeTextures)
    // finest[i] is the sampled hash of the finest BC7 level the viewer uploaded for names[i] (0 = none known): for a BC7 image the worker reads level 0 back from the GPU and compares. quiet: only a line with
    // something wrong in it is logged (the periodic check of the agent's bake slots)
    void describe_textures(const std::vector<U32>& names, const std::vector<std::string>& labels, const std::vector<U64>& finest, const std::vector<std::vector<U64>>& level_hashes, bool quiet);
}

#endif // RK_VULKAN
