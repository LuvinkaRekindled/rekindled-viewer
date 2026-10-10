# The redesign: a persistent GPU scene, GPU-driven passes, a thin render thread

Status: DESIGN for review, 2026-10-10. Not built. Replaces the "tune the existing path" detour in `PLAN-render-threads.md` (its measurements stay valid as the baseline and as the argument for this). This is the ground-up route of `PLAN-native-renderer.md` (N2 + N3 + N4) made concrete; it is NOT the retained-records alternative (`PLAN-retained-records.md`), which keeps per-frame draw-list building and the GL-shaped stream.

## Why the current limits are not limits of the redesign (measured 2026-10-10, scene of 7.5-10k draws, frame 15.7 ms)
| Where it is today | ms | Why it exists | In the redesign |
|---|---|---|---|
| Main thread: renderShadow + cull/render-map jobs + rebuild | 3.2 | shadow casters are collected again from the octree every frame, per cascade | gone: casters are scene entries, the GPU culls them |
| Main thread: stateSort + postSort + updateCull | 2.9 | visible groups and draw infos are re-sorted and re-listed every frame | gone: no per-frame lists, GPU culling |
| Main thread: renderGeomDeferred + pools | 1.9 | one record builder pass per frame over the visible draw infos | gone: indirect draws from the scene |
| Worker: 5-8k draws at 1.5-2 us (state replay, uniform copies of 13 MB/frame, sampler slots, layouts) | 10-12 | the stream is GL-shaped: every draw re-states program, uniforms, textures | gone: tens of indirect draws per pass, per-draw data pulled from scene tables |
| Ray scene: 7.8k `noteCaster` calls per frame, 1.4 ms/call of scene preparation | about 3 | the ray scene is fed by the same per-frame caster lists | gone: it reads the scene's entries and changes only |
What stays on the CPU: network, object updates, avatar logic and animation, LOD and geometry generation, UI. Those (about 6-8 ms on the main thread today) are what the thread pool then speeds up (avatar motion is already parallel).

## Foundation F: nothing GL-shaped (first, before any scene slice)
Decided again 2026-10-10 (Adam: "nothing should be GL-shaped anymore, that was supposed to be part of the previous cleanup"). Today the viewer-facing classes still emit GL calls (about 400 call sites, about 60 distinct functions) into `llvkgl.h` wrappers, an async queue (`llvkasync`, 2.4k lines) and a GL state machine replayed on the worker (`llvkgl.cpp`, 6k lines; `llvkmirror`, 1.3k). The scene design below cannot sit on that: the GL stream is why every draw costs 1.5-2 us on the worker and why uniforms are copied whole.

Measured surface (calls per file; the whole of it is in `llrender`, only 40-odd stray calls outside): llglslshader 86 (uniforms, queries, program objects), llimagegl 72 (uploads, parameters, fences, readback), llrender 60 (state, queries, target copies), llgl 37 (caps, enable/disable, fences), llshadermgr 32 (compile/link/binary cache), llvertexbuffer 27 (buffers, attribute arrays, draws), llrendertarget 24 (FBOs), llpostprocess 13, cubemaps 10. Outside: pipeline.cpp 9, llspatialpartition 8, llviewerwindow 6, llmaniptranslate 6, llface 5, others 1-4 each.

**What replaces it (front end, main thread; executor, render thread):**
- *Handles.* `TextureId`, `BufferId`, `TargetId`, `ProgramId`: index + generation, allocated on the producing thread, created/destroyed by commands, so ordering with draws is by construction. No GL names anywhere (`getGLBuffer()`, `getTexName()`-style accessors become ids).
- *RenderContext* (plain struct, no globals: one per producer thread, the main thread's is `gRender`). It holds the current state as values: raster, depth/stencil, blend, viewport/scissor, target set, program, vertex/index buffer binding, texture slot array, and the uniform values of the bound program in its reflected layout. `LLRender`, `LLGLState`, `LLTexUnit`, `LLRenderTarget::bindTarget`, `LLVertexBuffer::setBuffer` and `LLGLSLShader::uniform*` write only into it.
- *State blocks.* Raster+depth+stencil+blend+target formats+program are interned by hash into a state id; a block is announced once, a draw names the id. The executor builds pipelines from blocks (cache as today).
- *Uniforms by frequency, not one loose block.* Each uniform of the 209 programs is classed by name (table in `LLShaderMgr`, built from the existing uniform enum): **pass** (camera, lights, shadow matrices, probes, fog: written once per pass into a pass block), **material** (colour, specular, environment, cutoff, brightness, texture matrix: read from the `Materials` table by index), **draw** (model/normal matrix, instance index, palette offset: a few dozen bytes, push constants or an instance record). The legacy `uniform*()` API routes by class, so callers do not change; a draw never copies a 4-9 KB block (13 MB/frame today).
- *Textures.* A texture is a persistent bindless slot (S0); `LLTexUnit::bind` writes the slot index into the context's slot array; samplers are part of the slot (small immutable set chosen by filter/wrap). No active unit, no per-draw layout work: layouts are tracked per image on the executor and transitions are batched before the pass (the frame graph, N2).
- *Command stream.* Typed POD records: draw (state id, buffer ids/offsets, range, instance count, pass-block id, draw-data offset, slot-array delta), clear, set target, copy, upload, query, present, resource create/destroy. 18k records/frame today (45k GL calls); target about 3k (draws plus rare changes).
- *Executor.* The render thread owns Vulkan: resolves ids, binds, records, submits. It has no GL state, no VAO/bound-texture/active-unit emulation.
- *Readbacks/queries* (snapshot, picking, texture save, occlusion, timestamps) are commands with completions the caller waits on only where it must.

**Class by class** (public API stays, so the 145 files that call `gGL` and the 83 that use `LLVertexBuffer` do not change): `LLVertexBuffer` keeps its CPU shadow and strider API, uploads on `unmapBuffer` as a copy command into arena ranges, `setBuffer`/`draw*` write the context and append a draw; `LLImageGL` owns a `TextureId` and issues upload/mip/readback commands (GL enum values stay as constants mapped to VkFormat in one table); `LLRenderTarget` owns images and an attachment list; `LLGLSLShader` keeps the text assembly in `LLShaderMgr` and hands final sources to `llvkshader`, `mUniform[]` becomes the index into the reflected layout; `LLGLState` and the `LLRender` blend/depth/cull/scissor/mask setters write context state; immediate mode batches into per-frame vertex scratch and appends draws like any other.

**Stages (each a commit with a gate; the old path is deleted only at F5):**
- **F0 inventory (read-only):** every GL-shaped call site and every reader of GL names (the 40 outside `llrender`), the uniform class table for all 209 programs, the list of per-draw values read by the pull shaders and by the alpha shaders. Output: the table that S0 and the `Materials` layout need.
- **F1 handles + context + state blocks** behind the existing class APIs, producing the typed records next to the old stream (both can run; verify mode compares draw counts and state ids).
- **F2 executor for typed records** (replaces `on_gl*` for draws, state, targets, textures, buffers); programs/uniform classes; resource lifetime commands.
- **F3 move the world passes over** (sky, terrain, objects, avatars, water, post, DLSS) pass by pass, comparing pictures with the old path (kept until F5).
- **F4 readbacks, queries, snapshots, media textures** natively.
- **F5 delete** `llvkgl*`, `llvkasync*`, the GL parts of `llvkmirror*`, `gen_llvkgl.py`, the GL stubs (about 10k lines). Gate: no GL-shaped code left, same pictures, fps not lower.
Because the viewer keeps running on the old path until F3 passes are moved one by one, this is not the "broken viewer for weeks" big-bang of `PLAN-n1-native-backend.md`; it keeps that plan's design and its checkpoints, with the old path as the reference.

## Architecture
**Three roles.**
1. *Simulation (main thread):* as today, plus it publishes **scene deltas**: "entry added / changed / removed", "instance matrix changed", "material changed". No draw lists, no per-frame walk of visible objects.
2. *Render thread:* applies deltas to the GPU scene buffers, then records the frame: a few compute dispatches (cull, build indirect arguments) and a few `vkCmdDrawIndexedIndirectCount` per pass. Recording a frame is tens of commands, so one thread is enough; the thread pool is used for delta application and for anything CPU-heavy that remains.
3. *Job pool* (`max(1, hardware threads / 2 - 1)`): geometry generation, avatar motion (existing), delta packing, texture decode.

**The scene (device-local buffers in the existing arena, all addressed by buffer device address):**
- `Instances[]`: transform (3x4), previous transform (for motion vectors, N7), flags, region id. Static objects share their region's matrix entry.
- `Entries[]` (one per draw batch = today's `LLDrawInfo`, per face group of an object): vertex buffer address + layout bits, index buffer address, first index, index count, bounding sphere/box, instance id, material id, pass mask (opaque, alpha-mask, alpha-blend, emissive, rigged, shadow caster...).
- `Materials[]`: bindless texture slots (diffuse, normal, specular, emissive, ORM), colour, specular, environment intensity, alpha cutoff, fullbright, glTF parameters, texture matrix id.
- A **persistent bindless texture table**: stable slot per (texture, sampler), freed on texture deletion. Today slots are reassigned every frame per view, which is why records cannot live across frames; this is the first piece to build.
- Skinned output: the compute skinning pass the ray scene already has (`kSkinShader`) writes posed vertices once per frame, shared by shadows, G-buffer and rays (N6).

**Passes (all GPU-driven, N4):** per view (4 sun cascades are unnecessary in ray shadow mode; G-buffer; reflection probe faces) a cull compute shader reads `Entries` (frustum, then Hi-Z occlusion later), appends survivors to per-pass indirect argument buffers; the draw reads vertices by address (the pull shaders for shadow, diffuse and materials already exist as the "packets"), material by `Materials[entry.material]`, matrix by `Instances[entry.instance]`. No per-draw state changes: a pass is a handful of pipelines, ordered by pass mask.
**Ray scene:** its instances are exactly the scene's static entries (BLAS per vertex/index buffer group as now, TLAS instances from `Instances`); the 7.8k per-frame notes and the per-call scene preparation disappear. This part is already persistent and incremental (the cache built 2026-10-10), so it is the proof that the delta model works.
**Transparency (N5):** alpha-blend entries are drawn after opaque in a separate pass; the first version sorts per entry on the GPU (sort keys from the cull pass) and draws indirectly; order-independent transparency replaces it later.

## What is reused from what exists
Native Vulkan RHI (context, renderer, shader compiler, pipeline cache), buffer arena with stable addresses, bindless tables, the pull shaders and packet records (shadow kinds 0-2, geometry kind 3), the compute skinning pass, the persistent ray scene and its incremental cache, DLSS/DLAA integration. New: the scene tables, the persistent texture table, the delta protocol, the cull/indirect compute, the render thread frame loop.

## Where the deltas come from (to verify by reading the code first, slice S1)
`LLVolumeGeometryManager::rebuildGeom` / `LLVOVolume::registerFace` create `LLDrawInfo`s per spatial group (`LLSpatialGroup::mDrawMap`); `clearDrawMap()` marks a rebuild; `mModelMatrix` is the region render matrix for static drawables. The scene entry set of a group is therefore replaced when its draw map is rebuilt: delta = (group id, new entries). Active/moving objects change only their instance transform. Removal = group destroyed.

## Scene slices S0-S7 (after F1-F2 exist; each ends in something measurable, behind a switch, old path intact until a slice proves itself)
- **S0. Persistent bindless table + material table.** Stable slots; the existing packets use them instead of per-frame slots. Gate: same pictures; draws/frame and worker texture ms unchanged or lower. (Prerequisite for everything textured.)
- **S1. Scene tables and deltas for static opaque geometry.** Entries/Instances/Materials filled from group rebuilds; a verify mode compares the scene's entry count and ranges per group against the old draw maps (like `verify=1` for the ray scene). No rendering change yet. Gate: 0 mismatches over a long run.
- **S2. GPU-driven shadow casters.** Cull + indirect for the sun cascades (non-ray modes) and for the ray scene's caster set; removes renderShadow and the per-frame noting. Gate: shadow pictures identical, main thread -3 ms, scene call -1.5 ms.
- **S3. GPU-driven G-buffer for static opaque and material passes.** Replaces renderGeomDeferred and the deferred packets. Gate: pictures identical, main thread -2 ms, worker draws about -4k.
- **S4. Moving objects and avatars in the scene** (instance deltas, shared skinned output); impostors; attachments follow joints by index.
- **S5. Alpha pass from the scene** (GPU sort, indirect), emissive second draw folded in.
- **S6. Render thread loop and removal of the GL-shaped stream** (N1b/N2): the frame description is immutable, the render thread owns Vulkan; delete `llvkasync`/`llvkgl` translation for the passes that moved.
- **S7. Hi-Z occlusion, LOD selection on the GPU, motion vectors, OIT.**

## Risks
- Objects whose geometry is not in group buffers (particles, UI, terrain patches, water, sky, HUD, text, media) stay on the old path until they get their own entries; the old path must keep working side by side for a long time.
- Per-face texture animation, texture matrices, glTF material overrides and per-draw uniforms (specular, environment, brightness) must all live in `Materials`; every field the old shaders read per draw has to be accounted for (list them in S0).
- Correctness oracle: no GL reference exists any more; the old Vulkan path is the reference until it is removed (scene verify mode + A/B pictures per slice).
- The sim is the bottleneck for object churn: heavy rez/edit load turns into many deltas; deltas must be cheap and batched.

## First action
F0, then S0: list every per-draw value the three pull shaders and the alpha shaders read; design `Materials`; implement the persistent bindless table with stable slots (it also removes the per-frame slot reassignment cost, 1.6 ms of worker time today). Review this document first: the scope of S0-S3 is the decision.
