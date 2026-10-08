# Stable vertex and index buffers (a buffer arena), the first step toward ray tracing

Status: plan, 2026-10-08. Written from reading `llvkgl.cpp` (buffer hooks, `write_buffer`, `on_bufferData`), `llvkcontext.cpp` (`createBuffer`) and the logs of the 2026-10-08 runs. Nothing is changed yet. Goal for Monday: phases 0-2 (the arena without ray tracing); phase 3 is the first ray tracing probe.

## Why
A bottom-level acceleration structure (BLAS) is built from a vertex buffer and an index buffer by device address, and must be rebuilt or refitted when their content changes. So the renderer has to be able to say, for every vertex/index range: "this lives at address A, content version V, and nobody moves it". Today that is true for some buffers and false for most of them (below). The same change also removes real costs of the rasteriser path, so it is not ray tracing only.

## What is there today (measured and read)
- Two paths, chosen in `on_bufferData` by size (`LL_VK_DEVICE_BUF_MIN`, default 128 KB; `LL_VK_NO_DEVICE_BUFFERS` turns the first off):
  1. **Device path** (>= 128 KB, vertex or index): a plain `VkBuffer` in video memory, written by GPU copies (`Renderer::updateBuffer`, flushed by `flush_buffer_updates` / `ensure_buffers_current`). Never renamed: copies are ordered in the command stream, so a frame in flight cannot see a half written buffer. Re-specified with the same size keeps the buffer; a different size makes a new `VkBuffer`. About 398 of these, 241 MB (log line "GL buffers: 398 in video memory").
  2. **Host-visible path** (< 128 KB, and every uniform buffer): a mapped `VkBuffer` (video memory behind resizable BAR when there is room), written by the CPU. If a frame in flight may still read it, `write_buffer` **renames** it: creates a new `VkBuffer`, copies the kept part from a **system-memory shadow copy** (`Buf::mShadow`; reading write-combined video memory is too slow), destroys the old one. About 11,810 of these, 51 MB (average about 4 KB), plus 51-265 MB of shadow copies in system memory.
- Rename cost shows up in loading and teleport: "buffer renames in the last 5 s: 58,167 copies, 906 MB, 111 ms on the main thread". In a steady view it is 0.
- Every `VkBuffer` already gets `SHADER_DEVICE_ADDRESS` and the shadow packets already use addresses (`sPacketBufferGeneration` is bumped when a name goes). So addresses exist; what is missing is stability, versions, and the acceleration structure usage flags.
- `LLVertexBuffer` layout (read in `llvertexbuffer.cpp`): not interleaved. Each attribute is its own contiguous array in one buffer (`mOffsets[type]`, stride `sTypeSize[type]`, position is 12 bytes of float x3), indices are 16-bit. That is directly usable as BLAS input (`R32G32B32_SFLOAT`, stride 12, `UINT16` indices).
- The worker per-draw cost of vertex input is about 0.45 ms of a frame (measured earlier). The vertex layout ids / arena item was left alone for rasterising on purpose (GAINS.md: "well under 0.4 ms, a stale buffer handle is a GPU fault").

## Design
**One arena per usage class, suballocated.** Large `VkBuffer` blocks (start with 256 MB device-local for vertex+index; uniform buffers stay as they are), created once with `VERTEX | INDEX | TRANSFER_DST | SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY | STORAGE`. Suballocation with VMA's virtual blocks (`vmaCreateVirtualBlock`; VMA is already the allocator), alignment 16 for vertex arrays and 4 for indices (the 12-byte position stride is allowed as acceleration structure input).

- **`BufferRef`** per GL buffer name: `{arena, offset, size, address, epoch, version}`. `epoch` changes when the name gets a new allocation (re-specified with another size, or deleted); `version` changes on every write (`glBufferSubData`, same size `glBufferData`). Ray tracing keys its BLAS cache on `(name, epoch)` and rebuilds or refits when `version` moved.
- **Writes are always GPU copies** from a staging ring (the device path as it is now, generalised). A write into an existing allocation is ordered after the earlier frames' draws by the command stream, so there is **no rename and no shadow copy**. A re-specified buffer of another size takes a new suballocation, and the old one goes on a **retire list** and is freed when `frameCount + 1` frames have completed (the rule `destroyBuffer` already uses).
- **Draws bind the arena buffer plus offset** (they already bind buffer + offset per attribute): far fewer distinct `VkBuffer` handles, so fewer vertex-buffer rebinds and a simpler key for state changes. The packets use `arena address + offset`, and their generation counter is bumped on every new allocation or free.
- **Uniform buffers and pixel buffers stay outside** (separate path; a per-frame ring later if wanted).
- A switch keeps the old path: `LL_VK_ARENA=0` (and `FSVkBufferArena` while it is being proven). Remove both when the checklist has been run twice.

## Phases (each one ends with a measurable gate and a test run)
0. **Baseline (one run, no code).** At one busy spot and one light sim, windowed, `run-viewer-clean-phases.bat` + `LL_VK_OPSTATS`: records/frame, worker ms and "vertex input" ms, buffer counts and MB (device and host-visible), shadow copy MB, rename counts during a teleport.
1. **Arena for what is already on the device path** (vertex/index >= 128 KB). Same behaviour, new allocator, `BufferRef` and retire list. Gate: picture identical (two snapshots before/after; repeatable now with the warm-up fix), records and worker times unchanged within noise, no validation messages (run once with the validation layer on).
2. **Move the small buffers into the arena**, delete `mShadow`, the rename path and `report_renames`, route small writes through the staging ring (batched into one copy submission per flush, not one barrier per buffer). Gate: system memory down by the shadow copy size (51-265 MB), rename copies 0 during a teleport (log), main-thread time while loading not worse, steady fps not worse, worker vertex-input time better or equal.
3. **Addresses and versions API** (`getBufferAddress(name)`, epoch and version) and a **BLAS probe** behind a setting: one BLAS per static group buffer (position + 16-bit indices, opaque flag), no TLAS, no tracing. Log count, total size, build time per frame and refit/rebuild rates while walking around and during a teleport. Gate: the numbers decide whether per-group BLAS is affordable (see risks) and what to do about rigged avatars.
4. Hand over to the ray tracing plan (inline ray query sun shadows, step 2 of `PLAN-ray-tracing.md`).

## Risks and how each is covered
- **A stale handle is a GPU fault.** Epoch + retire list; the arena only reuses a range after the frame fence of its last use; a debug mode fills freed ranges with a pattern (visible garbage instead of a fault), switched on by an environment variable.
- **Small-write cost.** About 92 `bufferSubData` per frame in a steady view and thousands per second while loading; a `vkCmdCopyBuffer` with its own barrier per write would be slower than a `memcpy`. Hence the staging ring with batched copies and one barrier per flush; measure in phase 2 before deleting the old path.
- **Fragmentation** in a long session: virtual block statistics logged every few minutes; big blocks first, a second block when one is 85% full; compaction only if the log says it is needed.
- **Memory:** about 290 MB of vertex/index data today in two paths; the arena must not need more than that plus slack (log it). Resizable BAR is not assumed (device-local copies only).
- **Packets and caches that remember buffer addresses** (shadow packets, the `tex_ptr/buf_ptr` per-draw caches): bump their generation on every new allocation; test with shadow packets on and off.
- **Per-group BLAS may be wrong in a changing world** (edited prims, sculpts, animated meshes, rigged avatars, crowds of 75). Phase 3 is the probe that measures it before anything depends on it.

## To verify on Monday before writing code (not yet read)
- Which faces carry a model matrix and which are baked into group buffers (`LLVolumeGeometryManager::rebuildGeom`, `LLFace` render matrix): decides BLAS granularity (group buffer vs per object instance).
- Where rigged avatars keep skinned positions (skinned in the vertex shader from a palette; a BLAS needs the skinned positions, so a compute skinning pass writing into an arena range is likely, which is also an arena user).
- All `glMapBuffer*` / persistent mapping uses on the worker (expected none for vertex/index; check `mapBuffer` in `LLVertexBuffer`).
- How often `glBufferData` re-specifies with a different size in a steady view (log it in phase 0; decides how hot the retire list is).
- `Renderer::updateBuffer` / `flushBufferUpdates`: whether the copies are already batched and where the barrier sits.

## Files expected to change
`llvkgl.cpp` (buffer hooks, `Buf`, packets generation), `llvkcontext.cpp/.h` (arena blocks, virtual blocks, flags), `llvkrenderer.cpp/.h` (`updateBuffer` to a staging ring, retire list), `llvkasync.cpp` (the typed buffer records already name the buffer; check them), `llvertexbuffer.cpp` only if the sizes or offsets it passes need to change (not expected). Docs: `GAINS.md`, `REGRESSION-CHECKLIST.md` (add a teleport and a snapshot after each phase).
