/**
 * @file llvkgl.h
 * @brief The Vulkan mirror's worker side: what the render thread keeps and runs for the typed command records (see llvkgl.cpp and llvkasync.cpp).
 *
 * There are no intercepted GL functions any more (F2.1): the viewer writes typed records (llvkstate.h, llvktex.h, llvkbuf.h, llvktarget.h, llvkquery.h, llvkasync.h)
 * and the worker plays them. This header only has what the two translation units and the mirror share.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#if defined(LL_VULKAN) && !defined(LL_LLVKGL_H)
#define LL_LLVKGL_H

#include <mutex>
#include "llvkmirror.h"
#include "llvkasync.h"

namespace LLVkGL
{
    // true while the mirror is initialised and accepting calls
    extern bool gActive;
    // every mirror entry point of the inline mode (before the queue starts) runs under this lock
    extern LLVkMirror::Mutex gMutex;

    // CPU time this thread spent inside real OpenGL calls (ticks of the performance counter); read by the benchmark
    inline thread_local long long gGlTicks = 0;

    // true when the mirror owns the resources
    extern bool gOwn;

    // names for the resources the producer creates: handed out on the calling thread, created on the worker (LLVkAsync::resourceCreate)
    void own_gen(int kind, GLsizei n, GLuint* out);
    // the worker side of the lifetime records: kind is an LLVkAsync::ResourceKind
    void resource_create(unsigned kind, int n, const unsigned* ids);
    void resource_destroy(unsigned kind, int n, const unsigned* ids);
    // DLSS: runs the upscaler on four mirror-owned textures (colour, depth, motion vectors -> output); the gMutex must be held
    void on_upscale(const LLVkMirror::UpscaleArgs& args);
}

#endif // LL_VULKAN
