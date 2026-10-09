# Cleanup and update pass before ray tracing (2026-10-09)

Why: the native path is Vulkan only now, so the GL-era leftovers (listed in `AUDIT-copy-vs-native.md`) can be removed or redone natively, and some of them are prerequisites of ray tracing (buffer flushes, queues, capability reporting). Doing them first gives a before/after for each, instead of one blurred result after the new feature.

Rules (from the project notes): choose by measured ms; read a function body before deleting it; one commit per step with the gate numbers in the message; the regression checklist plus two snapshots (before/after, same spot, warm-up on) after each step; every step ends with a backup refresh. One topic per session.

## C0. Measurement protocol (the "before")
One route, always the same: busy spot (2 min), teleport, light sim (2 min), windowed 2558x1360, DLSS Quality, `run-viewer-clean-buffers.bat` (adds the buffer lines) and a second pass with `run-viewer-clean-gpuzones.bat`. Numbers to keep per step, in `GAINS.md`:
- steady frame time and fps at each spot (clean run, frame pacing line), main thread waited / worker waited
- worker ms per frame, main-thread queue ms, records per frame (OPSTATS)
- GPU ms per frame and the big zones (gpuzones run)
- loading: renames, main-thread ms per 5 s while loading, time from teleport until the frame time is steady
- memory: system-memory shadow MB, GL buffer MB, process private bytes at the end of the route
- the picture: a snapshot at each spot (golden pair)
The first baseline already exists for the buffer numbers (PLAN-stable-vertex-buffers.md, phase 0 table); C1 adds the missing lines.

## C1. Diagnostics bundle (one test run, no behaviour change)
New in the `VkPhases` block (commit f53efc7b): forced buffer flushes per frame by cause (a draw read a pending buffer / frame end) and buffers with pending writes per flush; the six buffers written most per interval (name, size, path, writes, KB); the queue families of the device, once at start. Already there and read in the same run: "graphics pipelines built in this period: N (total ms in the driver, slowest ms)" and "texture uploads per frame ... N ended a render pass in progress" (C4). Gate: none; the numbers decide C3, C4 and C7, and give the C8 baseline.

## C1 results (2026-10-09 run, `run-viewer-clean-buffers.bat`, commit f53efc7b)
Scene this time: 700 device buffers (275 MB), 27.7k host-visible (210 MB, shadow copies 210 MB), settled frame 4.9 ms (about 200 fps), p99 7.9 ms.
- **Forced flushes: about 1 per frame** (by a draw 1.0-1.15, frame end 0-0.29, 4-7 buffers per flush), also while loading. The hypothesis of tens of pass breaks per frame is false. Texture uploads ended a render pass 0.015/frame settled, at most 0.16/frame while loading (84-138 uploads/frame, 2.8-6 MB/frame then). So C3 as a standalone step is small: roughly one pass break and two full barriers per frame. Folded into the arena's phase 2 (which has to move the flush anyway); not a separate step unless gpuzones show otherwise.
- **Device-path sub-writes (16/frame, 802 KB/frame) come from only 4 buffers**, named #1700, #1702, #1705, #1707 (128-384 KB; 3, 7, 3, 3 writes per frame, about 50 KB each, the same every frame). Host side: #1701 (56 KB, 1 write/frame) and #1695 (48 KB, 3 writes/frame) lead; total 116 writes/frame (461 KB) busy, 23 (199 KB) light. Low buffer names = created at start-up and long lived. Owner still unknown; to find it, record the `LLVertexBuffer` type mask and a creation call site for named buffers (cheap: capture 6 return addresses at creation of device-path buffers, resolve only the top writers). Matters for ray tracing: these four are the buffers whose BLAS would be rebuilt every frame, so they must be identified before phase 3 (likely avatar or particle data).
- **Pipelines:** settled = 0 built. While loading/wandering: 67 builds in one 3 s window cost 7.8 ms of driver time in total, slowest 0.55 ms; all sub-millisecond with the warm cache. Cold-cache baseline not measured yet. C8 stays (ray tracing).
- **Image barriers per frame: 151 settled, 114-505 while loading.** New item, see C9.
- **Queue families (RTX 5060 Ti):** family 0: 16 queues graphics+compute+transfer; family 1: 2 queues transfer only; family 2: **8 queues compute+transfer (dedicated async compute)**; 3: video decode; 4: transfer; 5: optical flow. We use family 0, queue 0 only. Dedicated async compute and DMA transfer queues exist: C7 is a real option, not a hypothetical.
- **Single-frame stalls of 0.1-1.0 s** at login and teleport (max 1003 ms at 15:00:06; 695 and 550 ms at the teleport at 15:03:21-30). Cause not identified (renames, texture upload, region change, shader/asset work are all candidates). Not in the plan yet; see C10.

## C2. Delete the scaffolding (low risk, per-call win)
The inline (non-async) mirror path with its hook timers, mutex and HookStat objects; `LL_VK_VERIFY`/`llvkverify.h`; remaining `ll_nogl()` guards; dead `mhRC` code; the 22-entry stats and debug-filter branches in `prepare_draw` behind one flag. Read each body first (lesson: `before_end_frame`). Gate: build clean, checklist, picture identical, worker ms and main-thread queue ms not worse (expected better), startup unchanged.

## C3. Buffer flushes without breaking the render pass (folded into arena phase 2)
C1 measured about 1 forced flush per frame, so this alone is a small gain (one pass break, two full barriers). The design (queue device-buffer writes ahead of the pass that reads them, flush at pass boundaries with a transfer-write -> vertex/index-read barrier, never end a pass for a copy) is built as part of the arena's phase 2, where the copy path is rewritten anyway. If a separate quick win is wanted first: narrow the two barriers from ALL_COMMANDS to the real stages. Gate if done alone: GPU ms (gpuzones), picture identical.

## C4. Texture upload path, one copy
Main thread hands the decoded image buffer over (ownership transfer) and the worker copies once into staging; creation and mip generation as named commands. Gate: main-thread ms while loading, time to steady after a teleport, no black textures (see the regression lessons: minimized window, snapshots).

## C5. Per-draw worker mapping (conditional)
Texture ids as direct table indices with the sampler in the entry, vertex layout ids instead of VAO emulation. Only if the C2 numbers show the worker is near the limit; measured earlier at 2.7-3 ms of worker time per frame with the main thread as the limit. Gate: worker ms.

## C6. Real capability reporting instead of a faked GL identity
`initGLNoGL` reports invented GL strings and limits so the feature table works. Replace with a capability struct (limits, bindless sizes, DLSS, ray tracing support flags: acceleration structure, ray query, ray tracing pipeline, DLSS-RR) and an own GPU identity; then drop `llglstubs.cpp` and the dummy shared-context token (upload threads become plain transfer work). Gate: viewer starts, GPU-change detection and graphics presets behave, no GL-name checks left (grep), regression checklist.

## C7. Queues and synchronisation (a decision, needed by ray tracing phase 3)
Today: family 0, queue 0 only, fences. C1 showed the device has a dedicated async-compute family (8 queues, compute+transfer) and two transfer-only queues. Ray tracing wants BLAS builds and compute skinning off the graphics queue timeline, and texture/buffer uploads on a transfer queue. Design only unless the data says a second queue pays: a transfer queue for uploads with timeline semaphores (`timelineSemaphore` is Vulkan 1.2 core), compute on the graphics queue first. Gate: a written design in this file, and a probe if cheap.

## C8. Pipeline creation off the draw path (committed, not conditional)
Decided 2026-10-09: this is done whether or not the rasteriser numbers show a gain, because ray tracing needs it. Today pipelines are created synchronously where the draw needs them (`Program::getPipeline` -> `vkCreateGraphicsPipelines` on the worker), backed by an on-disk pipeline cache: free when warm, a worker stall on every miss when cold (new driver, new variant, first run). Ray tracing multiplies the problem: ray query and compute shaders (skinning, BLAS helpers, denoiser inputs) add pipelines, any ray tracing pipeline compiles far slower than a graphics one, and hit/material variants grow with the material table. A miss in the middle of a frame would cost many milliseconds, and the C1 line only measures today's small number.
Work: (1) a pipeline request path that does not block: a miss queues the build on background threads (a small pool; `VK_EXT_graphics_pipeline_library` or plain separate `vkCreate*Pipelines` calls on a second thread with the shared cache), and the draw either uses a ready fallback variant or is skipped for that frame, with a log line per miss so nothing is silent; (2) pre-warming at startup from a recorded list of variants used in earlier sessions (stored next to the cache), so a warm start has zero misses; (3) the same path for compute pipelines now, ready for ray query / RT pipelines later; (4) a switch to turn it off. Gate: with a deleted cache file, the first minutes show no worker stall over a few ms (slowest build on the worker side, frame-time max and p99); with a warm cache, pipelines built on the worker = 0; picture identical after the first frames; no validation messages.
Measurement first: the C1 line "pipelines built in this period (slowest ms)" from the normal run, and one run with the cache file deleted as the cold baseline.

## C9. Image barriers (new, from C1)
151 image barriers per frame settled, up to 505 while loading. Each is a separate `vkCmdPipelineBarrier2`, usually between passes that read the previous pass's targets. Unknown whether they cost anything measurable: first read where they come from (per transition: target, old/new layout, stages; some may be redundant, e.g. same layout, or splittable into one batched call or a narrower stage mask), then try batching and narrowing and read gpuzones. Gate: barriers per frame, GPU ms, picture identical, no validation messages. Also relevant to ray tracing: BLAS/TLAS builds and the trace pass add their own barriers, and a clean barrier model avoids stacking waits.

## C10. Login and teleport stalls (new, from C1)
Single frames of 0.1-1.0 s at login and after a teleport. Add a stall attribution: when a frame exceeds 100 ms, log what the main thread and worker did in it (queue records, buffer renames and ms, texture uploads, pipelines built, waits on fences and acquires). Then fix the largest piece. Gate: max frame time after a teleport.

## Order
C0/C1 (done) -> C2 -> C4 -> C9 -> C10 -> C8 -> C6 -> C7 design (C3 goes into arena phase 2) -> (C5 only if the numbers ask for it) -> buffer arena phases 0-3 (`PLAN-stable-vertex-buffers.md`, phase 0 is done) -> ray tracing (`PLAN-ray-tracing.md`).

## Not looked at yet (C1 and C0 numbers will tell if they matter)
Occlusion query cost (about 100-140 begin/end per frame, results handed back asynchronously); present mode and latency settings (`LL_VK_PRESENT`, vsync preference).
