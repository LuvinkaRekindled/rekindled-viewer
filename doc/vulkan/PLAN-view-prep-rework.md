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

## 7. Open points for Adam

- Order after step 2: the overlap (step 3) or the world view through the same stages (step 5, the larger share)?
- Is a short period with both the old and the new path behind a setting acceptable, or should each step replace the old one at once (the rule "one path after a move")? The check mode needs the old path for the comparison.
