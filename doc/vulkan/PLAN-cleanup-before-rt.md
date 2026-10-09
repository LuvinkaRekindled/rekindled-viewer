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
Added to the `VkPhases` buffer line: forced flushes per frame by cause (a draw read a pending buffer / frame end), copies and bytes per flush; the top writers per interval (buffer name, size, path, writes, bytes); graphic pipeline creations per interval with time, and a log line for any single one over 1 ms (hitching); the queue families of the device (flags, counts) once at start. Gate: none; the numbers decide C3, C7 and whether pipeline pre-warming is needed.

## C2. Delete the scaffolding (low risk, per-call win)
The inline (non-async) mirror path with its hook timers, mutex and HookStat objects; `LL_VK_VERIFY`/`llvkverify.h`; remaining `ll_nogl()` guards; dead `mhRC` code; the 22-entry stats and debug-filter branches in `prepare_draw` behind one flag. Read each body first (lesson: `before_end_frame`). Gate: build clean, checklist, picture identical, worker ms and main-thread queue ms not worse (expected better), startup unchanged.

## C3. Buffer flushes without breaking the render pass
Depends on C1. If flushes per frame are in the tens: queue device-buffer writes ahead of the pass that reads them and flush at pass boundaries (and once before the first draw of a pass that reads pending data), with a transfer-write -> vertex/index-read barrier instead of ALL_COMMANDS, and never end a pass for a copy. This is also arena phase 2's flush design, done earlier and measured alone. Gate: flushes per frame (target: a handful), GPU ms (gpuzones), picture identical, no validation messages.

## C4. Texture upload path, one copy
Main thread hands the decoded image buffer over (ownership transfer) and the worker copies once into staging; creation and mip generation as named commands. Gate: main-thread ms while loading, time to steady after a teleport, no black textures (see the regression lessons: minimized window, snapshots).

## C5. Per-draw worker mapping (conditional)
Texture ids as direct table indices with the sampler in the entry, vertex layout ids instead of VAO emulation. Only if the C2 numbers show the worker is near the limit; measured earlier at 2.7-3 ms of worker time per frame with the main thread as the limit. Gate: worker ms.

## C6. Real capability reporting instead of a faked GL identity
`initGLNoGL` reports invented GL strings and limits so the feature table works. Replace with a capability struct (limits, bindless sizes, DLSS, ray tracing support flags: acceleration structure, ray query, ray tracing pipeline, DLSS-RR) and an own GPU identity; then drop `llglstubs.cpp` and the dummy shared-context token (upload threads become plain transfer work). Gate: viewer starts, GPU-change detection and graphics presets behave, no GL-name checks left (grep), regression checklist.

## C7. Queues and synchronisation (a decision, needed by ray tracing phase 3)
Today: one queue family, one queue (`vkGetDeviceQueue(.., 0)`), fences. Ray tracing wants BLAS builds and compute skinning off the graphics queue timeline, and texture/buffer uploads on a transfer queue. C1 logs which families exist. Design only unless the data says a second queue pays: a transfer queue for uploads with timeline semaphores (`timelineSemaphore` is Vulkan 1.2 core), compute on the graphics queue first. Gate: a written design in this file, and a probe if cheap.

## Order
C0/C1 (one run) -> C2 -> C3 -> C4 -> C6 -> C7 design -> (C5 only if the numbers ask for it) -> buffer arena phases 0-3 (`PLAN-stable-vertex-buffers.md`, phase 0 is done) -> ray tracing (`PLAN-ray-tracing.md`).

## Not looked at yet (C1 and C0 numbers will tell if they matter)
Shader/pipeline compile hitching (a persistent pipeline cache exists: `llvkrenderer.cpp` saves `vkGetPipelineCacheData`); occlusion query cost (about 100-140 begin/end per frame, results handed back asynchronously); present mode and latency settings (`LL_VK_PRESENT`, vsync preference).
