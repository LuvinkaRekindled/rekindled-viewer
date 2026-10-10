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

## Slices (each ends in something measurable, behind a switch, old path intact until a slice proves itself)
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
S0: list every per-draw value the three pull shaders and the alpha shaders read; design `Materials`; implement the persistent bindless table with stable slots (it also removes the per-frame slot reassignment cost, 1.6 ms of worker time today). Review this document first: the scope of S0-S3 is the decision.
