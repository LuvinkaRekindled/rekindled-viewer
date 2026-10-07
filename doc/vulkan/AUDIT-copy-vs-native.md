# Audit: what of the Vulkan-only path copies GL, and what is native (2026-10-07)

Asked for after the typed-record work: look back at everything built for the Vulkan-only path and check it for the same mistake (reproducing GL's behaviour
instead of asking what is efficient). Read: the worker's draw path (`prepare_draw`, `sync_textures`, `build_vertex_input`, `on_glDraw*`), `Renderer::applyState`,
`drawIndexed`, the buffer code (`on_bufferData`, `write_buffer`, renames), `initGLNoGL`, `llglstubs.cpp`. Not read in detail: texture upload/mip code, query code,
shader translation, `llvkmirror.cpp` frame handling.

## Native in design already (keep)
- `Renderer` state: extended dynamic state (cull, depth, blend, vertex input are dynamic commands set only when changed); a pipeline is keyed by program + target formats + topology, not by GL state. No pipeline explosion.
- Uniform ring with dynamic offsets (`Renderer::ringAlloc`): uniforms are already copied into one per-frame buffer.
- Bindless table + indirect draws for shadow packets (render-clean).
- Presenting into the main window, async worker, timestamps, DLSS: all Vulkan concepts.
- Typed records for texture slots / draws / buffers by name / render targets: the producer holds plain state, the draw carries deltas.

## Copies of GL that cost something (ranked)
1. **Buffer shadow copies and renames (GL write-after-use semantics).** Every buffer under 128 KB keeps a system-memory shadow (117-140 MB held at Adam's spot) and
   is "renamed" (a fresh Vulkan buffer plus a copy of the old contents) when a frame in flight may still read it. Measured in the log: about 22,000 copies / 286 MB per
   5 s in steady state, 56,000 copies / 739 MB per 5 s while the world loads. GL needed this behaviour because the driver hides it; Vulkan code should write into a
   frame-lifetime upload arena and use device-local buffers for geometry that stays. The by-name upload records make this changeable now (the worker sees every write
   with its buffer name and range).
2. **Worker per-draw GL-to-Vulkan mapping.** Each draw does, per sampler, `sGL.mTextures.find(sCtx.mBound[unit][class])` (hash lookup) and a sampler cache lookup,
   rebuilds the vertex input from emulated VAO attributes (`build_vertex_input`), syncs uniform blocks, and runs stats/phase-timer/lap bookkeeping in the hot path.
   About 0.8 us per draw on the worker (2.7-3 ms per frame). Native: texture ids are direct table indices with the sampler stored in the entry; a vertex layout is an id
   created once and referenced by the draw; no timers on the hot path unless profiling.
3. **Texture upload path** (not touched yet, so still GL-shaped): bind, set pixel-store state, `glTexImage2D`; the main thread copies the pixels into the queue and the
   worker copies them again into a staging buffer. Native: hand the decoded image buffer over (ownership transfer) and copy once, into staging, on the worker; creation and
   mip generation as named commands.
4. **Faked GL identity** (`initGLNoGL`): GL strings, version, limits and the `/PCIe/SSE2` suffix are made up so the viewer's feature table, GPU-change detection and
   `gGLManager` flags keep working. No speed cost, but it ties the viewer to GL-era checks. Replace with a capability struct and an own GPU identity once the
   feature code no longer reads GL names. Same for the dummy shared-context token that keeps the upload threads alive: the upload threads should become real
   Vulkan transfer work, not a fake GL context.

## Scaffolding to delete (no design problem, but it is in the way and in the hot path)
- The inline (non-async) mirror path in the generated wrappers: the hook timers, mutex and `HookStat` objects that run on every GL call that is not a typed record.
- `LL_VK_VERIFY` (GL-versus-Vulkan comparison, dead without GL), `llvkverify.h`, remaining `ll_nogl()` guards, dead `mhRC` close code.
- `llglstubs.cpp` (the `__imp_` stubs): needed only while dead GL names remain in viewer code; goes when those are removed.
- The 22-entry stats / debug-filter branches inside `prepare_draw`: keep behind one `if` for profiling builds, not on the normal path.

## Changes to the plan (PLAN-draw-records.md)
- Step 1b (done): texture ids as slot table + draws carry deltas. The worker side must now read the table directly (bindless index, sampler in the entry) instead of replaying `glBindTexture`: do it together with step 2.
- Step 2 (uniform ring): front end writes values into the ring; the worker only gets offsets. The ring on the worker side exists already.
- New step 2b: buffer arena. Upload records write into a per-frame arena (host-visible, mapped) with a copy into device-local buffers for geometry that is kept; remove the shadow vectors and renames.
- New step 3b: vertex layout ids instead of VAO emulation.
- Texture creation/upload as named commands with ownership transfer (the old step 5).
