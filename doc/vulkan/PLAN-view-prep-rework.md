# View preparation rework: one "view" for the world, the cascades, the probes and the mirrors (design note, 2026-10-31, before code)

Adam, 2026-10-31: the code we started from was stacked on top of for years; we know what the end result is, so start over where a better system exists. Visibility and view preparation are the root of most of the other work, so start there, with multi-threading and balancing as the design rule, not an afterthought. A rework is fine as long as it is an improvement and easier to work with now and later.

## 1. Why here (numbers)

Main thread, ms per frame (London, 77 avatars; `S-retire-views-design.md` section 1 for the 21 October numbers, `ShadowPrep` phase log for today):

| Work | ms |
|---|---|
| `display: updateCull` (world octree cull) | 0.5-0.9 |
| `display: stateSort (early)` (visibility, occlusion, LOD, per-drawable state, group rebuilds, render map) | 1.4-2.3 |
| Sun cascade preparation (`parallel sun cascade preparation`) | 0.58-0.70 clean, of which point cloud 0.39, cull and mark waits 0.41, render map 0.19, pass 1 0.12 (the phases overlap in the log) |

That is about 3-4 ms of an 11 ms frame, and the same work runs again for every other view (cascades, reflection probe snapshots, mirrors, HUD). Everything else reads its result: the pools, the packets, the ray scene, the avatars' state.

At today's pose the main thread is the limit (it never waits for the worker, the worker idles 0.3 to 1.3 ms a frame), so main-thread savings up to about 1 ms reach the frame time.

### Correction, same day, after reading the world path (`updateCull`, `stateSort`, `postSort`)

The "world view 3 ms" above is the 21 October number. The world view is already staged on the pool by the earlier stages (`cull_partitions_parallel`, default mode 3; the sort split `sortLoopsParallel`, default mode 3; the render map in chunks, `main_map_chunk`). What is left on the main thread, from the latest zone lap (`zU-blocked1`, zone timers on, so everything is a little inflated):

| Piece | ms |
|---|---|
| `updateCull` | 0.55 (waiting for the pool 0.20, VO cache cull 0.07, the rest the serial parts) |
| `stateSort (early)` | 1.67 |
| of which `postSort` | 0.96: `rebuildPriorityGroups` 0.49 (geometry builds, main thread), render map 0.16, far avatar tier 0.08, rebuildGeom 0.05 |
| of which apply deltas | 0.35 |
| of which waits for the sort compute | 0.28 |

So the world view is not an unstaged 3 ms. About 0.5 ms are waits for the pool (idle time that an early launch could fill), 0.35 is the delta apply, and 0.5 is the group geometry rebuild. The rework still fits (a view context, jobs launched early, results joined late), but the gain is in the overlap and the apply/rebuild, not in moving the walks to the pool.

## 2. What it is today (read in `pipeline.cpp`)

- `display()`: `updateCull` (world) -> `stateSort` (early) -> `generateSunShadow` -> the passes. The cube snapshot path and the hero/mirror path run their own `updateCull` / `generateSunShadow` / `stateSort`.
- `generateSunShadow` (about 900 lines): plans the cascades (clip planes, shadow cameras, visible extents by an octree walk per cascade), prepares them (cull, mark, rebuild, render map: `shadowPrepCull`, `shadowPrepMark`, `shadowPrepRebuild`, `shadowPrepRenderMap`) as pool jobs, then renders each.
- Shared mutable state that the jobs need to fake: `LLViewerCamera::sCurCameraID`, `sCull`, `sShadowCascadeCull`, `sParallelPrep` (thread-local, set by `ShadowPrepScope`), the render-type mask, `sUseOcclusion`, `sShadowRender`, the current matrices, the root bounds (`rebound()` writes), the wake queue (a race here crashed the viewer three times on 2026-10-30).
- Results are merged serially on the main thread (a push per group and draw info into each cascade's `LLCullResult`).

## 3. Target

A **view** is data: camera, cull settings (frustum slice, margin, occlusion on/off), the render types it wants, and a result (`ViewResult`: visible groups, drawables, bridges, alpha groups, render map). Nothing about a view lives in a thread-local or a global.

Stages, each a function of view data that can run on any thread:

1. **Plan** (cheap, main thread): clip planes, cascade cameras, the cull settings of each view. Output: a `ShadowPlan` of plain data.
2. **Extents** (pool, one job a cascade): the visible-extents walk. Done as step 0 (section 6).
3. **Cull and mark** (pool, chunks): visible groups and drawables of a view, marked per camera slot.
4. **Rebuild** (main thread, as now: GL buffers, the priority queue).
5. **Render map** (pool, chunks): the draw infos of the visible groups.
6. **Render** (main thread): draw each prepared view.

Rules for threading and balancing:

- Work is chunks of about equal cost (cascade prep already uses 128 and 192 groups a chunk); no stage is one job per view.
- The pool takes chunks dynamically and the main thread takes chunks while it waits (`LLForkJoinPool` does this).
- A stage declares its inputs and outputs; independent stages overlap (a cascade's cull does not depend on the world cull: launch before it, join after).
- Chunk size and the number of jobs get a dial driven by the waits we already measure (main waits for the worker, worker waits for work), never a per-scene CPU/GPU balancer.
- No merge on the main thread: a view's chunks write to disjoint ranges of its result, or the merge is a pool job per view.
- No hidden state: a job gets its view context, not a thread-local.

## 4. Steps (each behind a setting, checked against the old result, old path deleted after a clean soak)

0. Point cloud walks as pool jobs (built 2026-10-31, `FSParallelShadowPointCloud`, `LL_VK_PC_CHECK=1` compared 43,200 cascades with the serial walk: 0 differ). Short laps from the same pose, on against off: pass 1 plus point cloud 0.24 ms against 0.52 ms (a 0.29 ms saving), cull 0.30 against 0.31, render map 0.12 against 0.13; the loop was 11.13 against 11.90 ms, more than the phases explain, so single-lap noise is in it. Also in this step: the mark and render-map chunk results are reused between frames (0.09 ms, within noise) and zones around the two merges.
1. Extract the plan: `generateSunShadow` computes a `ShadowPlan` first, then prepares from it; no behaviour change.
2. A `ViewContext` replaces `sCurCameraID`, `sCull`, `sShadowCascadeCull`, `sParallelPrep` for the cascade jobs (the world and probe paths follow).
3. Launch the cascade cull and mark before the world `updateCull`, join after the early sort (the overlap; needs the render-type mask and `sUseOcclusion` out of globals first).
4. Merge as pool jobs or disjoint ranges.
5. The world view through the same stages, then probes and mirrors.
6. Delete the old per-cascade serial paths and the check mode scaffolding after a soak.

## 5. Checks

`FSParallelShadowPrepCheck` (old serial path versus the new result, per cascade), `LL_VK_PC_CHECK`, the phase log, clean laps from a hand-set pose (loop and waits), and the usual soak for crashes. Every step lands only when its check reports 0 differences and the loop does not rise.

## 6. Honest expectation

Shadows alone: the cull and mark waits (about 0.4 ms) can hide behind the world cull, the point cloud walks drop from 0.39 ms toward the longest cascade, the merge is 0.1-0.2 ms. About 0.5-0.8 ms in total on the main thread. The larger gain is structural: the same stages serve the world view (3 ms), the probes and the mirrors, and S7 (GPU visibility) has a place to plug in.

## 6b. The world view (Adam, 2026-10-31: simplify it; the work involved is not a concern)

What it is today (read in `pipeline.cpp`):

- `updateCull(camera, result)`: a serial loop over the partitions (mode 0) or `cull_partitions_parallel` (default, `LL_VK_CULL_SPLIT` 3; 4 = pool run checked against the serial one). The pool path cuts the partitions into runs of equal measured cost (a partition over 80 us is walked in pieces: top levels on the caller, subtrees as jobs), each job writes into its own `LLCullResult` and its own **effects ledger** (`SortDelta`: rebuild a drawable, move a partition, rebuild a group, set an object's LOD, rebuild a mesh, mark a bridge dead, wake a group), and the caller concatenates both in unit order and applies the ledger (`applySortDeltas`). Then the object cache cull, the sky.
- `stateSort(camera, result, defer_pools)`: `sort_split_mode` picks 0 (loops as before), 1 (compute into a ledger, then apply), 2 (check), 3 (compute on the pool: `sortLoopsParallel`), 4 (check of the parallel one); then `postSort`.
- `postSort`: rebuild drawable groups, `rebuildPriorityGroups` (geometry builds, GL work), the far avatar tier, the render map (`main_map_chunk` chunks on the pool when a dozen conditions hold, else the serial loop), the alpha group sort.
- Gates that keep each path off: the visibility, occlusion and cull-shadow censuses (`LLVisCensus`, `LLOccCensus`, `LLCullShadow`), the GPU zone timers, debug LOD views, the camera not being the world camera, shadow/reflection/HUD/cube snapshot flags. Shared state the paths read or fake: thread-locals `sCull`, `sSortSink`, `sSortTee`, `sSortPoolSink`, `sParallelPrep`, `sShadowCascadeCull`, `LLViewerCamera::sCurCameraID`, and the globals `sUseOcclusion`, `sShadowRender`, `sReflectionRender`, `sRenderingHUDs`, `gCubeSnapshot`, the render-type mask.

What is already right and stays: the shape "jobs read, write into a result of their own plus a ledger of effects, one apply in a fixed order". It is the same shape as the cascades. The rework makes it the only shape and gives it a name:

- A **View** is data (camera, kind: world / cascade / probe / mirror / HUD, cull settings, render types, occlusion on or off). The gates above become fields of the view, not tests of globals, so the census and timer modes no longer switch paths.
- A **StageResult** is lists plus a ledger. Every stage (cull, sort compute, render map) returns one; the join concatenates and applies in order. Cascades and the world use the same code.
- The ledger apply (0.35 ms) is the part to attack once the shape is single: effects have owners (a drawable, a group), so applying can be chunked by owner on the pool where the effect touches only that owner (LOD set, mark rebuild), and stays on the main thread where it touches shared queues or GL (wake queue, geometry builds).
- Launch early, join late: the cull jobs of the world and the cascades are submitted before either is joined, so the pool is busy while the main thread does the serial parts (the overlap).

Steps (the old path stays behind its setting while the check compares, then is deleted after a soak):

W1. `View` and `StageResult` types and the world cull expressed through them; `cull_partitions_parallel` takes a View instead of reading `sCurCameraID`, `sShadowRender` and friends. Check: `LL_VK_CULL_SPLIT=4` style (the new result against the old serial walk, 0 differ).
W2. The sort compute and the render map take a View and return a StageResult; the mode table (0-4) goes away, the check becomes a comparison against the old path. Check: `sortParallelCheck`, `checkRenderMapLive`.
W3. Cascades built on the same View (the plan extraction of step 1 above), so there is one cull and one render map implementation.
W4. Launch early / join late (the overlap), the apply chunked by owner.
W5. Probes and mirrors through the same stages.
W6. Delete the old paths and the check scaffolding after a soak.

Expected gain (honest): 0.5 to 1 ms of the 2.2 ms the world view costs on the main thread today (pool waits ~0.5, apply 0.35, geometry rebuild 0.5), the overlap with the cascades included; the larger result is one path to maintain and a place for S7 to plug in.

## 7. Open points for Adam

- Decided 2026-10-31: the world view first (section 6b, steps W1-W6; the overlap comes with W4).
- Decided 2026-10-31: the old and the new path both stay behind settings for the checks, the old one is deleted after a clean soak.
