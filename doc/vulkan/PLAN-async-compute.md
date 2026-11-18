# Overlapping the ray scene builds with the raster work (plan, 2026-11-03)

Adam asked what GPU wins are left; the first of the list was async compute ("let's go for it"). Reading the code first changed the shape of it: the cheapest test needs **no second queue**. This plan is the experiment order, what is verified and what is still to read before code.

## The problem in numbers
With ray traced sun shadows on, the plaza stills are GPU bound (98% busy, 12.3 ms of a 12.5 ms loop, `BALANCE-HISTORY.md` 2026-11-01). The ray scene is about 1.4 ms of that, all of it run in series on the graphics queue between the G-buffer and the lighting: `rt scene skin` 0.15, `rt scene builds` 0.96-1.2, `rt scene tlas` 0.32 (zone laps of 2026-10-29 and 11-01). With ray tracing off the GPU has nothing of this kind (the lightmap/AO pass is raster), so **this plan only pays with RT on**. A realistic target is 0.3-0.8 ms (GPU shaders are shared between the work that overlaps, so not the whole 1.4 ms), about 3-6% of the frame.

## What the code does today (read 2026-11-03)
- One command buffer a frame on the one graphics queue (`mQueue`, `vkQueueSubmit2`, `rkvkcontext.cpp`); `FRAMES_IN_FLIGHT = 6`. The device also has a compute-only queue family (8 queues) that nothing uses (`mCaps.mComputeFamily`; the arena can be created CONCURRENT between the two families already).
- `LLPipeline::renderDeferredLighting` (`pipeline.cpp` ~22150) names the main view's rigged casters (`rt_name_rigged_main_view`), flushes the materials, and calls `RKVkBridge::rtScene(scene)`: that is the whole of the viewer side, at lighting time, after the shadow passes have *named* the casters (with RT on the cascade maps are not drawn; the passes only name the casters).
- `RKVk::Rt::buildScene` (`rkvkrt.cpp` ~4800) records one block through `Renderer::recordExternal`: a **full barrier** (ALL_COMMANDS to build/compute: "earlier work, last frame's traces, the shadow passes that wrote the vertex data"), the avatar skinning dispatch (`rt scene skin`), the bottom level builds and refits (static structures built and compacted, **avatars refit in place**), a full barrier, the top level build (`rt scene tlas`, rebuilt in place by each scene call of the frame), then a full barrier to ALL_COMMANDS. So today nothing before it overlaps it and nothing after it overlaps it: the GPU drains, builds, drains.
- The refit in place is the reason a second queue is harder than it looks: frame N's refit of an avatar's structure must come after frame N-1's last ray query on it. With the GPU saturated that already holds (frame N-1 is finished when N's raster starts), but a second queue needs the timeline semaphores to say so, and double buffering of the avatar structures if frames are ever to overlap.

## The idea in three steps, cheapest first
**E1. Same queue: build early, wait late.** Split the scene call in two, both still on the graphics queue in the same command buffer:
1. `rtSceneBuild` right after the shadow passes have named the casters (after `generateSunShadow`, before the G-buffer): records skin, builds, compaction copies and the top level build with **no trailing full barrier**.
2. `rtSceneReady` where `rtScene` is called today: only a narrow barrier (acceleration structure build write to the ray query readers: the lighting fragment and compute stages).
The G-buffer, velocity, alpha-free passes recorded between the two have no dependency on the structures, so the GPU is free to run them while the builds execute (no barrier between them). The front barrier stays (it orders against last frame's traces and the earlier writes). This costs no new synchronisation object, nothing that can deadlock and no second queue, and it tells us the ceiling: if E1 gains nothing the GPU does not overlap this work and a second queue will not either.
Things E1 depends on, **to read before code**: what the shadow passes write that the builds read ("the shadow passes that wrote the vertex data": which buffers); whether `rt_name_rigged_main_view`, `rt_flush_materials` and the avatar palettes are final before the G-buffer (the camera matrix `mat` is); the multiple scene calls of a frame (main view, probes, snapshots: only the main view moves); `RKVkBridge::rtScene` goes through the async queue (`CMD_RT_SCENE`), so the two halves are two commands; and what else in the frame writes the vertex or index buffers between the early and the late point (a write while a build reads is the hazard validation must show clean).
Switch: `LL_VK_RT_EARLY=1` (default off while measuring; the old order stays under `0`, deleted after a week of use: one path after a move).

**E2. A second queue, only if E1 shows the overlap is real but the barrier is the limit.** Record skin + builds + tlas in a second command buffer on the compute family, signalled by a timeline semaphore the graphics queue waits on before the lighting; the compute work waits on the previous frame's trace completion (timeline value of the last submit that read the structures). Needs: queue creation (`rkvkcontext.cpp` creates one queue today), ownership of the structures (`CONCURRENT` sharing mode or release/acquire barriers, the arena already offers CONCURRENT), the acceleration structure storage and scratch created shared, DLSS untouched (it stays on graphics). Risks: validation of the cross-queue hazards, the refit in place (a double buffered avatar structure if frames overlap), the top level structure rebuilt several times a frame (probes and snapshots must stay in order on one queue). Only worth the work if E1 gains at least about 0.5 ms.

**E3. What else could move (small, check after E1/E2).** The particle sort (0.03 ms) and the Hi-Z pyramid are too small to matter alone; the sun lightmap/AO pass with RT off is raster, so no. If E1/E2 work, add these to the compute queue last, as a bonus, not as a goal.

## Measuring (the lesson of 2026-11-02)
GPU frame time drifts 7.6-8.3 ms between sessions, so only laps **back to back in one session** count. RT on, plaza, `h6-fixed.ps1 -Mode phaseson` (zone timers on for the pass breakdown; **fps from the plain lap**, `FSGpuZoneTimers` costs the main thread 4-8 ms), read with `python <work>\balance-read.py <run>`: the columns are GPU ms, GPU % of loop and the pass list. A/B: `LL_VK_RT_EARLY=0` then `=1`, twice each, alternating. Gates: **G1** the shadows identical (a still frame, debug view difference zero) and the validation layer (`LL_VK_VALIDATION=1`) clean through a lap with a turn and an avatar crowd; **G2** the GPU frame in the stills down by at least 0.3 ms with RT on, and the RT off lap unchanged; **G3** Adam's eye on avatar shadows and reflections (a stale structure shows as a lagging shadow or a torn avatar), plus a snapshot and a teleport.

## Risks
- A build that reads vertex data a pass writes later in the same frame gives torn shadows that validation can miss (the data race is on device addresses): the first step of E1 is the list of writers, not the code.
- Compaction (queries and copies of the static structures) moves with the builds; its slot logic ("this slot's last use") must not change.
- The saving may be smaller than the estimate because the G-buffer already keeps the GPU busy (an overlap only fills idle units): the experiment answers that, which is why E1 comes first and costs a day or less.

## Not in this plan
RT off (no builds to overlap), DLSS (a fixed 1.3 ms on the graphics queue), the alpha passes and the RT sun pass itself (other items of the GPU list), and any quality cut.

## Decision for Adam
E1 first (the early build with the late barrier, default off, measured back to back). E2 only if E1 pays.

## Result of E1 and the decision for E2 (2026-11-03)
E1 (build early, wait late, same queue) measured neutral: two laps each, alternating, on the defaults: GPU in the stills 11.60 ms off vs 11.69 on, the turn 11.85 vs 11.84. The GPU is 99% busy there, so there is nothing to overlap. Adam kept E1 (default off) and asked for E2 anyway: the gain is expected once the other items of the GPU list free the GPU, and the point of E2 is to have the structure ready then. **E2 is measured against E1 only** (same scripted laps).

## E2 design (LL_VK_RT_EARLY=3: E1's early point, the builds on the compute queue)
Why three graphics command buffers: a semaphore wait holds a whole submission, not a point inside a command buffer, so the frame's one command buffer is split at the two points where the queues meet.
```
graphics  A1 [uploads, flush_buffer_updates] --signal v1--> A2 [G-buffer, velocity, ...] --submit--> B [lighting ... present] waits v2 (+ acquire)
compute                        wait v1 --> [skin, BLAS builds/refits, compaction copies, TLAS] --signal v2
```
- One timeline semaphore (`Context::mAsyncSem`), values counted up by every signal.
- **E1's early point splits A1 from A2**: `Renderer::splitFrame()` ends the render pass, ends the command buffer, submits it (signal v1) and starts the next one (a fresh command buffer: the state flags `beginFrame` clears are cleared again). The compute work is recorded into the slot's own compute command buffer (`Renderer::recordExternal` records into it while `mComputeRedirect` is set; no GPU stamps from inside) and submitted to the compute queue (wait v1, signal v2).
- **E1's late point is the join**: `splitFrame()` again (A2 submitted), and the next submit (B, in `endFrame` or a mid-frame flush) waits v2.
- Every buffer the compute queue touches (arena blocks, scratch, tables) is created CONCURRENT between the two families when E2 is on (`Context::mAsyncConcurrent`); images are untouched by the builds.
- Hazards checked: frame N+1's uploads (A1) are after B(N) in the graphics queue, and B(N) started after compute(N) finished (the wait), and the semaphore wait scope covers later submissions of the queue; frame N+1's compute waits A1(N+1), which is after B(N) (the traces) in submission order; the frame fence is signalled by B, which waited for the compute work.
- Mid-frame flushes (`flushFrameAndWait`) while split: the flush submit includes the pending wait. Probes, hero probes and snapshots keep the old path (graphics queue, E0).
- Off by default; E1 (`=1`) and E0 stay as they are. If the device has no compute-only family, `=3` falls back to `=1`.
