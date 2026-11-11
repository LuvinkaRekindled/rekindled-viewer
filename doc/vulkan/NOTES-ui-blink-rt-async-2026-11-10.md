# UI blink with ray tracing and materials: findings so far (2026-11-10)

Status: **cause narrowed to the async compute split (E2) with ray scene materials on; the mechanism is not found.** One dedicated diagnostic run (22:35-22:43) did not support the one mechanism with log evidence. Everything here was read from the code and from the viewer's logs; the only viewer runs were Adam's. Nothing in this note is committed code except what is said under "State of the tree".

## The symptom (Adam)

- The UI, the name tags and "everything in the UI group" blink together, once in a while, for about a frame: they look bold, perhaps one pixel off, "like drawn twice over its already translucent self". Not one element at a time: all of them at once, in a cluster.
- Only with ray tracing on (RT mode 1, `LL_VK_RT_SHADOWS=1`) **and ray scene materials on** (`LL_VK_RT_MATERIALS=1`, the launcher `run-viewer-clean-rtmaterials.bat`). Not with RT off. Seen once earlier when materials were first tested, not in a while with RT on and materials off.
- Setup of the runs: DLSS 67%, ray reconstruction off, build of 2026-11-10 21:09 (steps 3a and 3b of the shadow walk removal, nothing that touches the UI), the 21:45 and later runs also had the RT penumbra filter shader change.

## Confirmed

- **The async compute split causes it.** In one session (21:53-22:02, log times local = UTC-6; the log's own timestamps are UTC): default (`rtearly=3`, ray scene built on the compute queue) blinked; live `rtearly=1` in `<work>\rt-debug.txt` at 21:55:28 (frame 9962): no blink for 2.4 minutes; `rtearly=3` again at 21:57:52 (frame 23377): the blink came back. An earlier run with `LL_VK_RT_EARLY=1` set at start (21:34-21:44, no compute queue at all) had no blink in about 10 minutes.
- It also happens with DLSS off (the sync validation run, below), so the upscaler is not involved.
- `LL_VK_RT_EARLY=1` is a workaround without a visible cost besides about 0.35 ms of GPU a frame (the A/B figure from 2026-11-05 recorded in the source and in the async compute memory note). Adam's decision: leave the default at 3 and find the ordering problem.

## Ruled out (with the evidence)

| Idea | Evidence against |
|---|---|
| The UI record cache (`FSUIRecordCache`) | still blinks with it at 0; mode 0 makes `ui_draw_view` call `viewp->draw()` directly (`llview.cpp:1598`). Its refresh interval N did flip between 3 and 4 (frame time near 9.5 ms) but it is not the cause |
| GPU hitches | about 97% of presented frames under 12 ms in the logs (VkPhases histograms), the rest mostly 12-25 ms |
| Camera jitter on the UI | the UI and the tags are drawn after the upscale inside `LLUnjitteredScope` (`llviewerdisplay.cpp:1868`), straight into the window |
| The dirty rectangle UI buffer (`RenderUIBuffer`) | off by default and in no settings file of Adam's; Adam had assumed the UI was drawn in rectangles: it is not |
| A skipped present pass in `renderFinalize` or `display()` | no early return in `renderFinalize`; no RT-dependent skip in `display()` |
| A missing barrier the validation layer can see | sync validation run, below: 0 SYNC-HAZARD lines |
| The sky-weighted penumbra fix, the fleck work | unrelated (the flecks were the RT filter, solved, see the fleck note) |

Adam's own reasoning that mattered: the sections of the UI are drawn at different times, so one section drawing twice could not blink all of them at once; the cause is below the sections (what they draw over) or after them.

## What the code does in the async path (read, no error found)

Files: `llvkcontext.cpp` (`createDevice`, `splitFrame`, `submitCompute`, `endFrame`, `flushFrameAndWait`), `llvkrenderer.cpp` (`Renderer::splitFrame`, `recordExternal`, `handleSet`, `bindlessFlush`), `llvkworker.cpp` (~11925, the scene call phases), `llvkrt.cpp` (`buildScene`, `sceneReadyBarrier`), `llvkbridge.cpp` (the mirror frame), `llappviewer.cpp:1942`.

- E2 is on whenever RT is on, the device has a compute-only family and timeline semaphores, and `LL_VK_RT_EARLY` is unset or 3 (`llvkcontext.cpp:946`). The frame is split into up to three command buffers: part 1 is submitted at the scene call (phase 1) with a timeline signal; the compute queue waits for that value, builds the ray scene (skin dispatch, BLAS builds, TLAS) in its own command buffer (`setComputeRedirect`), and signals; the join (phase 2) submits part 2 and sets the next submit to wait for the build's value; the last part (the lighting, the post chain, the UI) waits for it and also waits for the acquire semaphore; the frame's fence is on that last submit.
- Every buffer is created with concurrent sharing between the two queue families when async is on (`createBuffer`, `llvkcontext.cpp:2507` and `:2802`); images are exclusive but the build does not use images.
- The build starts with a full barrier inside the compute command buffer; it orders the compute queue only, the cross-queue order comes from the timeline wait on the first split's signal (which covers the earlier graphics submissions).
- The frame draws into a persistent offscreen backbuffer (`sMirror.mBackbuffer`) and blits it to the swapchain image at the end (`endFrameImpl`); the present pass and the UI both draw into that backbuffer. One mirror frame is held open across `display()`, the reflection probe update and the snapshot floaters (`holdFrame`, `llappviewer.cpp:1942`).
- `Renderer::handleSet` does not write a texture handle's entry into the frame's handle map in a split frame (`llvkrenderer.cpp:3229`): the first part is already on the GPU and reads that map, and a write that reached it before the descriptor was written was a device lost on 2026-11-09. The comment there says the later parts of the frame show the handle's old content until the next frame (a handle first shown shows the placeholder).

## The sync validation run (22:12-22:17, `run-viewer-clean-rtmaterials-syncval.bat`)

- Validation layer with synchronization validation, DLSS off, materials on, async on. Adam saw a few UI blinks in it.
- **0 SYNC-HAZARD lines.** Only three old messages, all noted as real defects on 2026-11-03 and still open: "Deferred Avatar Alpha Shader pull" has a fragment input at location 6 with no vertex output; "Deferred Shadow Pull Shader" and "Deferred Shadow Shader pull" write `frag_color` with no colour attachment (unused).
- The layer cannot see memory accessed through buffer device addresses (the viewer's render packets), the inside of acceleration structure builds, or the compute skinning, so it clears the barriers it can see, not this bug. The log is `<work>\run-logs\rtmaterials-syncval-20261110-221710.log`.

## The handle skip diagnostic run (22:35-22:43, Adam's last dedicated run)

- A temporary log in `handleSet` for entries a split frame's map does not get, with the part of the frame (1 = after the first split, 2 = after the join: lighting, post, UI) and whether the handle was first shown or changed; and a live key `mark=N` in `rt-debug.txt` that logs a marker with the frame number. Log: `<work>\run-logs\rtmaterials-handlelog-20261110-224353.log`.
- Counts: async runs skip 27,000 (syncval run) to 39,000 (21:30 run) entries; the no-async run skips 0. Most are part 1 (the shadow and G-buffer window), a few thousand around loading; part 2 is 0-8 entries per 1,200 frames in steady state, **all first shown, none changed**, mostly `512x512 fmt16 mip1` (probably glyph atlas pages), also a few small `fmt37` and BC7 (`fmt145`) textures.
- Adam marked **one cluster: mark 1 at frame 34662 (22:38:34 local)**. There are no part 2 entries in the 5,000 frames before it; the next one is a `512x512 fmt16` page at frame 35064, after the mark. So the handle map does **not** explain that cluster. (Adam: "at least that I caught".)
- Near the cluster the log has two dedicated 32 MB acceleration structure memory blocks created at 22:38:29 (the count of such blocks moves between 56 and 63 all run: they are created and freed all the time) and a burst of HTTP request timeouts at 22:38:21. Only a time coincidence.

## Hypotheses left, none shown

1. A hazard on memory the validation layer cannot see, shared by the compute queue and the graphics parts: buffer-address accessed data (render packets, the ray records, the material table when materials are on), the skinning output, scratch memory, the acceleration structure arena blocks (created and freed during the run).
2. Timing of something the async path moves earlier: the scene is built at phase 1 and read at the lighting, so any viewer or worker state that changes between them (bindless slots named by the ray records, the material table rows, deferred frees) is read stale; materials add the most of such state.
3. The handle map defect above (real, rare after the join, not this cluster).
4. A second UI draw in the held mirror frame (a snapshot preview with the UI): nothing seen, no floater was open.

## A second reading (another session, later the same night): nothing found, three shapes to tell apart

Read, no error found:

- Saved settings first: Adam's user settings have no override of `FSUIRecordCache`, `FSUIRecordCacheHz` or `RenderUIBuffer`.
- The full-screen draw that puts the finished picture into the backbuffer (`renderFinalize`, `pipeline.cpp` ~24182-24206) is unconditional and runs with blending off (`BlendScope blend(false)` at the top of the function), so it cannot let the old backbuffer through by its blend state.
- No pipeline was built in the background in the 22:02 and 22:43 runs (0 "pipeline built in the background" lines), so that draw was not dropped for a pipeline that was still compiling.
- `Context::splitFrame`, `submitCompute`, `endFrame`, `flushFrameAndWait`, `Renderer::splitFrame`, `on_rt_scene`: the parts are submitted in order, the last part (lighting, post, present pass, UI, the blit) is one command buffer, and a flush in a split frame records into the current part again. Nothing replays a part.
- The frame structure: `display()` opens a present frame, `swap()` presents it and opens an off-screen frame whose target is also the backbuffer (probe update, snapshots), the main loop closes that one. A draw that lands in the off-screen frame goes into the backbuffer and is covered by the next frame's full-screen draw.

"Bold for a frame" can be three different things, and the code read so far does not say which:

1. the UI drawn on a backbuffer the full-screen draw did not refresh (last frame's UI is still in it);
2. the UI drawn twice in one frame;
3. the UI drawn once with the wrong state (blend factors, filter, a sub-pixel offset).

**Written and built (exe 23:21, BUILD_EXIT=0), not run yet, uncommitted, to be removed afterwards:** `BbCensus`, `bb_note_draw` and `backbuffer_census` in `llvkworker.cpp` (tag `VkBbCensus`), the call in `endFrameImpl` and the `bbclear` key in `beginFrameImpl` (`llvkbridge.cpp`). What was proposed: counters on the render worker, no GPU reads. For every presented frame: the number of draws into the backbuffer by the present program and by the UI program. A log line when a frame has no present draw or more than one, and when the UI draw count is a one-frame spike (well above the frame before and the frame after). 1 shows as "0 present draws", 2 as a spike, 3 as neither. With it a live key that clears the backbuffer to magenta when a present frame begins: under 1 the blink becomes a magenta flash.

## How to resume

- The stop-gap that needs no more runs: when `LL_VK_RT_MATERIALS` is 1 and `LL_VK_RT_EARLY` is not set, set `LL_VK_RT_EARLY=1` at start-up in `llappviewer.cpp` (next to the other RT defaults, ~line 4003, before the window and Vulkan start); both the device setup and the pipeline read that variable. Asked of Adam, not decided.
- To confirm a blink live: start the normal materials launcher, add `rtearly=1` to `rt-debug.txt` when it blinks, remove it again.
- The next code reading, if the cause is still wanted: what the compute build reads and writes through addresses, and what the graphics parts between the split and the join write to the same buffers, material table and record buffers first; the arena block free path against the in-flight compute work.
- Launchers kept: `run-viewer-clean-rtmaterials.bat`, `-noasync.bat` (`LL_VK_RT_EARLY=1`), `-syncval.bat`, `-handlelog.bat` (only meaningful with the diagnostic build).

## State of the tree

- **Uncommitted diagnostic code, to be reverted or removed:** the handle skip block at the end of `Renderer::handleSet` (`llvkrenderer.cpp`), `Context::frameCmdIndex()` (`llvkcontext.h`), and the `mark` key with its log line (`llvkrt.cpp`). Tag `VkHandleSkip`.
- Committed this session and unrelated to the blink: shadow walk removal steps 1, 2, 3a, 3b (`pipeline.cpp`), the RT penumbra filter fleck fix (`db5d23a010`).
- Related notes: [[project-ui-blink-rt-async-2026-11-10]] (memory), [[project-async-compute-e1]] (the E1/E2 history), `NOTES-avatar-shadow-blink-2026-11-10.md`, `PLAN-async-compute.md`.
