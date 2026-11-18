# Research (2026-10-29, Adam away): the cost that stays after a turn, Device Generated Commands, mesh shaders

Reading only: the notes in `doc/vulkan/`, the code, and a web search. No lap was run (the plaza laps need Adam at the screen). Every number below is from the notes or a log cited in them, not re-measured today.

## 1. Why the GPU does not go back to 100% after a turn

### What is known (EXPIRY-design.md section 1, PLAN-texture-handles.md 17.16 / 17.22, S7-design.md sections 20 and 26)
- The GPU is not slower after a turn: GPU time per frame 12.63 -> 12.56 ms, the same passes. The main thread is: loop interval +1.0 to +3.7 ms (five turn laps; +2.5 on average), no decay over a 120 s settle. The worker waits for work 0.8 -> 5.4 ms a frame, and nvidia-smi goes 96-98% -> 79-84%. The 100% -> 80% is the main thread feeding the GPU late.
- The scene doubles during the turn and stays: objects 24.3k -> 48.6k, spatial groups 7.1k -> 14.2k, groups built and outside the frustum 4.2k -> 10.9k, draw infos 16.9k -> 39.3k (the visible set does not grow: 6.4k -> 6.9k), moved list 21 -> 107.
- Nothing is a single zone: renderShadow +0.2-0.35, updateMovedList +0.18, partition move/cull +0.1-0.15 each, texture list +0.4, UI zones that do not depend on the scene +7-25%. The thread is not slower (CPU probe lap), placement is not it (core census), pinning does not touch the growth.
- What was tried and what it gave back: Cold (moved list, animated textures of groups no camera accepted for 10 s) 0.2-0.3 ms; Released (draw infos and scene fragments of groups stale for 60 s) 0.44 ms; the texture priority stamp about 0.3 ms; the local light frustum test 0.1 ms. About 1.2-1.5 ms of the 1.8-2.8 ms is still unexplained; the open hypothesis is the cache and memory footprint of the live objects, groups and drawables.

### What this research found: the stock "forget what is behind you" rule is switched off in this tree
- `LLViewerRegion::killInvisibleObjects` (`llviewerregion.cpp:1821`) is the viewer's own way to drop instantiated objects that no camera has seen: it asks `LLVOCacheEntry::isAnyVisible(...)` about every active cache entry and kills the objects of the entries that answer false.
- `LLVOCacheEntry::isAnyVisible` (`llvocache.cpp:567`) is now `#if 0 ... #else return true; #endif`. The comment, from Linden Lab (davep, 2024-06-07), calls the rule "ill-conceived" because it evicted objects still within view distance and caused paging and glitches when rotating. So since that merge the function never answers false and `killInvisibleObjects` kills nothing. (The settings that fed it, `SceneLoadMinRadius` 32 m, `SceneLoadRearMaxRadiusFraction` 0.75, `NonvisibleObjectsInMemoryTime`, are still read but have no effect on this.)
- Consequence: an object is instantiated (LLViewerObject, LLDrawable, faces, volume) when a camera first sees its cache entry, and from then on stays until the sim kills it, the region goes, or the viewer shuts down. A 360 degree turn in a dense place instantiates everything within draw distance behind you, and nothing lets it go. That matches the doubling (24k -> 48k objects) exactly and explains why Cold and Released, which only drop what is derived from the objects (draw infos, fragments, moved list entries), recover about a third of the growth at best: the objects, drawables and faces themselves are what the culls, the moved list, `gObjectList.update`, the octree and the caches keep walking.
- EXPIRY-design.md section 4 assumes "objects, drawables and faces stay (the server owns object lifetime)". The code says otherwise for cached objects: `killObject` (`llviewerregion.cpp:1911`) removes the instantiated object (`gObjectList.killObject`) and keeps the cache entry, and `createVisibleObjects` makes the object again from the entry when a camera sees it. So there is a viewer-side release of the objects themselves, with a re-create cost on the way back. Not verified: that the entry holds everything a re-create needs when the cache is incomplete (the old comment says an incomplete cache makes the sim resend full updates in a swarm), and the glitches upstream saw.

### What I would try (needs Adam at the plaza)
1. A switch (`RK_VK_EXPIRY` stage on top of Released, default off) that brings the old kill back with our relevance model instead of the old radius rule: an object whose group no camera has accepted for T (start 60 s, as Released) and that is beyond `SceneLoadMinRadius`, not selected, not an animation source, no children in use, not an avatar or attachment, is killed through the existing `killObject`; re-creation uses the existing paced path (`NewObjectCreationThrottle` 200 a frame, `max_time` budgets).
2. The gate from EXPIRY-design section 8: loop interval after the turn within 10% of before; the turn back paying no frame above 25 ms; objects 48k -> about 26k; no holes, nothing popping; no device lost over 10 teleports.
3. Expected gain: the part of the +1.8-2.8 ms that scales with objects and groups, possibly most of the unexplained 1.2-1.5 ms; the price is the reverse hitch (re-creating up to 20k objects, about 100 frames at the throttle).
Risk named by upstream: "holes when flying back and forth between regions", glitches when rotating. That is why it must be behind a switch with a count of late wakes.

### A possible link to the device losses (unproven, cheap to test)
17.24 of the plan names the repeated instruction-pointer-only device losses (4 of the day's losses; one 16 s into a turn, with p99 109 ms in the frames before) as stuck in `[1713x898 c1 #0000]`, the `Deferred Sun Shader` fullscreen triangle running the ray-traced sun shadow loops. Today's snapshot work found the same pass, the same shader (63,916 bytes of SPIR-V, the one Aftermath names), taking 0.05-2.2 s a render with the avatars' ray structures refitted, and never with them rebuilt (commits e0920243, 1a763cdc). A turn changes the draw sets of many avatars at once. A live lap with `norefit=2` (the avatars' structures rebuilt, never refitted: rt-debug.txt) through a turn would show whether the turn's p99 and worst frames fall. The cost is the avatar builds, so it is a test, not a setting.

## 2. Device Generated Commands (VK_EXT_device_generated_commands)

### What it is
The GPU writes the sequence of commands (a template, the "indirect commands layout", with tokens) and `vkCmdExecuteGeneratedCommandsEXT` runs it. Tokens available: push constants, vertex and index buffer binds, draws (also mesh), dispatches, ray tracing dispatches, and the choice of a pipeline or shader object from an "indirect execution set" (the members share all state and differ in their shaders). Not available: render pass begin, barriers, descriptor set binds. One work token per sequence, last; the execution set token first, once. Needs buffer device address and either Vulkan 1.3 or maintenance5 (we are on 1.3 and use buffer device address). A preprocess buffer is needed; explicit preprocessing (a separate call batched into its own command buffer) is the fast form on some drivers. Ratified, Vulkan 1.3.296 (2024-09).

### What it would replace in our renderer
- Today the CPU builds packet records and the worker issues multi-draw indirect calls (`vkCmdDrawIndirect` / `vkCmdDrawIndexedIndirect`, `rkvkrenderer.cpp:4060, 4109`, no `...Count` variants), one call per run of entries that share an index buffer (`draw_packet_runs`). The pull vertex shaders read per-draw data through buffer references, so no descriptor set changes between draws (bindless textures, one set): exactly the shape DGC needs.
- What DGC adds over a plain GPU-written indirect buffer: the index buffer bind per sequence (all the "runs of one" collapse into one execute: the static cascade record I measured +0.18 ms worse when indexed would stop paying for it), push constants per sequence, and a different pipeline per sequence inside one execution set.
- What it does not add: anything on the GPU side of the frame. The frame is GPU-bound at the plaza (GPU 12-12.5 ms of 12.8) and the worker is idle 6.7-7.9 ms of it (commit messages of 517ec25f, 12866543), so recording draws is not the limit. The sum of the geometry passes in a recent plaza lap (`sn10`, per frame): sun lighting 2.33 ms, alpha runs 1.58 + 0.72 + 0.24, ray scene builds 1.34, DLSS 1.28, G-buffer 0.86 + 0.31, the opaque `scene table draw` passes about 1.5 together. DGC does not change any of those.
- Where it would pay: GPU-driven lists (S7 C2 groundwork: the verdict becomes draws without a main-thread classifier, GPU LOD select for rigged avatars), and the main-thread cull (0.3-0.8 ms) and the worker's recording on low-spec CPUs.

### What we would have to do
1. Caps: query `VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT` and properties (`maxIndirectSequenceCount`, `maxIndirectPipelineCount` or shader object counts, supported stages for execution sets, `deviceGeneratedCommands` with `dynamicGeneratedPipelineLayout`); enable the extension next to the existing optional ones (ray tracing is the model: optional, a classic path stays).
2. Pipelines or shader objects: create the pipelines that go into an execution set with `VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT` (maintenance5 flags2); our pipelines come from `Program::createPipelineObject` with a `PipelineKey` and the async pool, so the key would need the flag and a family of programs that share state. NVIDIA reports pipelines in a set slower than shader objects (90 ms -> 34 ms in their sample), which means VK_EXT_shader_object as a second dependency.
3. A compute pass that writes the sequences in the layout's token order and stride, a preprocess buffer sized per layout, explicit preprocess in a batched command buffer, barriers (compute write -> preprocess -> indirect read), a count buffer.
4. Keep the classic draw path (hardware without the extension) and a switch.
5. Tooling: Nsight Graphics does not support the extension (reported on NVIDIA's developer forum); our Aftermath decode and the GPU trace launcher would lose sight of the draws inside an execute. The validation layers support it.

### Support
- AMD: Windows Adrenalin 25.8.1 (2025-08-04) lists VK_EXT_device_generated_commands among its new Vulkan extensions; Mesa RADV since 24.3 (GFX8 and newer).
- NVIDIA: works on an RTX 3080 in NVIDIA's own sample, but a developer report (NVIDIA forum thread 324189) has the EXT form 10x slower than the NV form on identical input (draw 72 ms against 7.1 ms), 3x better with shader objects, and NVIDIA's answer was that it is an artificial stress test and that the real gain is GPU-driven culling; DGC compute dispatch was also much slower than a plain dispatch loop (14 ms against 0.6 ms, skinning included). A `vkCmdPipelineBarrier2` over 1 ms each with DGC was reported too. I could not confirm the driver version or whether this is fixed; our card (RTX 5060 Ti, driver 617.42 in the Aftermath blob) is a newer generation.
- Intel: ANV merge was still open in the sources I found; not confirmed.
- I could not get the gpuinfo.org coverage numbers (the pages load dynamically).

### Verdict
Not worth starting now. It changes CPU recording, and the CPU recording is not the limit at the plaza; the vendor with our development card has a reported performance gap and the profiler does not see inside it. It becomes worth a prototype only as part of a GPU-driven direction (C2 verdict into draws, GPU LOD select), and then as a measured prototype on one pass (the static opaque G-buffer entries, the pass that has the most "runs of one"), with the plain indirect draw as the A/B. A cheaper step first: the core `vkCmdDrawIndexedIndirectCount` (Vulkan 1.2) lets a compute shader write the draw count and arguments for a fixed pipeline today, which covers GPU culling and GPU LOD select without DGC.

## 3. Mesh shaders (VK_EXT_mesh_shader), only on supported cards

### Support
Cross-vendor since 2022-08. Hardware: NVIDIA Turing and newer, AMD RDNA2 and newer, Intel Arc and newer (general knowledge; gpuinfo.org figures not retrieved). That is the same hardware class as ray queries, which the viewer already treats as an optional path with a fallback, so "just on supported cards" is the way it would be done, and the fallback stays the classic vertex path. The rule "a second path only for hardware" in the notes allows exactly this.

### What it would give
- Meshlet culling (frustum, back-face cone, Hi-Z) before any vertex work, and fewer wasted invocations for tiny draws. The source I found (NVIDIA's own early numbers, RTX 3090) shows 0.8-0.9 ms against 1.1-1.8 ms in one scene, and NVIDIA's guidance is that mesh shaders extend the pipeline and will not always win; draw-level culling should stay indirect draws.
- The best fit in our scene is the rigged avatars: 13.6 M triangles a frame, 91% of all triangles (S6 census, older). It is also where the guard against the "rigged triangle count" lives.

### What it would cost
- A meshlet build for every mesh the viewer creates. Prims and meshes are made at runtime from LLVolume in the pull-ready layout; avatar meshes are static in bind pose, so they are the easy part, but their culling bounds would have to hold under skinning (bind-pose bounds plus the joints' reach), or be recomputed.
- A mesh shader variant of every vertex shader: the `RK_VERTEX_PULL` text rewrite in `llshadermgr.cpp` already turns vertex shaders into pull form, so a task+mesh rewrite is another transform of the same kind, but the viewer has hundreds of program permutations.
- A second geometry path to keep in step with the classic one, and the ray scene (which skins on the GPU for its own structures) is unaffected.

### Where the time is, in our own numbers
Per frame at the plaza (`sn10`, GPU about 12 ms): the geometry front end that mesh shaders act on is the opaque `scene table draw` passes (about 1.5 ms together) and part of the G-buffer pass (0.86 + 0.31). A mesh shader path that halved that would save 0.7-1 ms (6-8%). The big items are not geometry: ray-traced sun lighting 2.33 (19%), alpha runs about 2.5 (pixel overdraw), ray scene builds 1.66, DLSS 1.28. With ray tracing off the cascades are drawn again (27,000 draws, GPU 7.8-9 ms and the CPU the limit): there the front end matters more, but that is the lower-quality mode.

### Verdict
Possible on supported cards as an optional path, but not worth it now: its ceiling here is below 1 ms of a GPU-bound 12 ms frame, and it needs a second geometry path and meshlet data for everything the viewer draws. The levers that match our GPU profile are alpha (OIT or cheaper alpha runs), the ray scene builds, and the sun pass (including the refit slowness found today). Worth revisiting for a no-ray-tracing mode on supported cards, or if the rigged triangle count becomes the limit (an indirect GPU LOD select needs no mesh shaders).

## Sources
- Vulkan Documentation Project, VK_EXT_device_generated_commands (proposal and reference page); Khronos registry entry (extension 573, ratified).
- Phoronix: RADV merges device generated commands; Vulkan 1.3.296 released with the extension.
- AMD Vulkan driver release notes (Windows), 25.8.1.
- NVIDIA Developer Forums, "Extremely poor VK_EXT_device_generated_commands performance" (thread 324189).
- Igalia (rg3.name), "Device-Generated Commands at Vulkanised 2025".
- Khronos, VK_EXT_mesh_shader announcement (2022-08) and the mesh shader how-to; NVIDIA developer blog on mesh shaders; Nathan Reed, "Mesh shader possibilities".
