/**
 * @file fstexturearrivaltrace.h
 * @brief A per-texture arrival trace (doc/vulkan/PLAN-aurora-ideas.md section 6, step one).
 *
 * With FSTextureArrivalTrace on, every step a texture takes on its way to the GPU is written as one row of <logs>/texture-arrival.csv:
 * wanted (a face first asked for it), request (the fetch request was created), each fetch worker state change (the state's name, with the desired
 * and loaded discard and the bytes of the step), dechit (the decoded cache served it), created (the GPU image was made), createbreak (the create loop
 * stopped on its time budget with textures still queued), world (the world appeared). The file is read offline (a script), never by the viewer.
 *
 * Cost when off: one relaxed atomic load per call site. When on: a mutex and a small record per event, written out once a frame by the main thread.
 */

#ifndef FS_TEXTUREARRIVALTRACE_H
#define FS_TEXTUREARRIVALTRACE_H

#include "lluuid.h"
#include <atomic>

namespace FSTextureArrivalTrace
{
    extern std::atomic<bool> sEnabled;
    inline bool enabled() { return sEnabled.load(std::memory_order_relaxed); }

    // any thread. 'event' must be a string literal (the pointer is kept until the row is written)
    void note(const char* event, const LLUUID& id, S32 a = -1, S32 b = -1, S64 bytes = 0, F32 extra = 0.f);

    // main thread, every frame: reads the setting, opens or closes the file, writes the rows gathered since the last call
    void update();
    void shutdown();
}

#endif // FS_TEXTUREARRIVALTRACE_H
