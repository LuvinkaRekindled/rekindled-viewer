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
4. **Rebuild** (main thread, as now: the vertex and index buffers are filled through the legacy buffer API, face and group state is updated, the priority queue; no OpenGL calls are left in that path).
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
- `postSort`: rebuild drawable groups, `rebuildPriorityGroups` (geometry builds: filling vertex buffers and draw infos, updating face and group state), the far avatar tier, the render map (`main_map_chunk` chunks on the pool when a dozen conditions hold, else the serial loop), the alpha group sort.
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

## 8. Direction (Adam, 2026-10-31): as much off the main thread as possible, not limited by legacy

The goal is to take everything off the main thread that can leave it, and to question each piece that stays: is it a real constraint or only the old object model? Before W4 starts, write a **main-thread inventory** here: every piece of per-frame view work that still runs on the main thread (cull serial parts, delta apply, wake queue, the group geometry rebuild, the render map join, the far avatar tier, the alpha group sort, the occlusion and Hi-Z bookkeeping, the pool fills), each with its cost and why it is there: a real ordering or hardware constraint, or data that the legacy object model (faces, drawables, volume LOD, shared draw info lists) does not protect. The legacy-owned ones are candidates for new ownership rules (per-group ownership of faces and draw infos, per-thread queues merged at the join), not for leaving as they are. W3 (jobs work for their own view, not the globals) is the first piece of that; the join and apply as pool jobs (W4) is the second; the geometry rebuild by group is the third and the largest.

### 8.1 Main-thread inventory (measured 2026-10-31, lap `inv1-poolnewz1`, first still, default placement, 548-640 nearby lights; zone timers on, so every figure is about 15% high; ms a frame on the main thread)

The loop in that lap is 12.3 ms (display() 7.9, idle() 4.0). Blocked time (the main thread waiting, not working) is small everywhere: the pool joins block 0.24 ms in all (shadow prep cull wait 0.14, point cloud wait 0.07, the rest under 0.03). **The main thread is not waiting for the pool; it is doing work, and it runs a share of the pool's own jobs.** So overlap (W4) can win back at most that 0.24 ms of waiting; the larger gains are in work that the main thread does itself and the pool does not.

| Piece | ms | Kind | Why it is on the main thread, and the question to ask |
|---|---|---|---|
| `gObjectList.update` (avatar motion batch 1.57, avatar idleUpdate 0.76, attachment loop 0.79, flexible objects 0.18) | 2.71 | legacy | Avatars' skeleton updates mutate shared avatar and drawable state; the blend already runs on the pool but main works a share and the rest is serial. Largest single piece, and not view preparation. Per-avatar ownership of its skeleton, attachments and drawable updates is the candidate. |
| `generateSunShadow` (renderShadow 0.96 for 4.4 passes, cull and cloud waits 0.31, spot/projector 0.31, rest cascade plan, mark and render map joins) | 2.09 | mixed | Recording the draws of a pass touches the command stream (a real ordering constraint: one recorder per pass, but the passes could be recorded on several threads into sub-streams: the F3 sub-stream test exists). The plan and the joins are the W3/W4 work. |
| `render_ui` (widgets draw 0.81, world panel 0.63, nearby panel 0.60, floaters 0.51, radar 0.30) | 1.12 | legacy | The widget tree is drawn on the main thread because widgets read and write shared UI state; most of it is unchanged frame to frame (the UI cache idea, `LL_VK_UI_CACHE`). |
| `stateSort (early)` (postSort 0.62: priority groups 0.24, render map 0.14; apply deltas 0.19; compute waits 0.14) | 1.12 | legacy | The deltas and the group rebuild mutate groups and faces the legacy model shares. W4 (apply by owner), then group-owned geometry rebuild. |
| `renderDeferredLighting` + post-deferred geometry + local lights | 1.05 | real, partly | Command recording for the main view's lighting and alpha passes: ordering is real; the alpha and lighting lists could be built off main. |
| `avatar velocity` (rigged draws: collect, palettes, motion test) | 0.68 | legacy | Walks every rigged draw; per-avatar data, a candidate for the same per-avatar ownership. |
| `updateCull` (partition cull 0.18, jobs wait 0.14) | 0.44 | mostly done | On the pool; the serial parts are the top levels of split partitions and the bridge/sky handling. |
| Object movement bookkeeping (`partition move` 0.23, `updateInGroup` 0.18, `movePartition` 0.13, `drawable updateMove` 0.14, `bridge updateMove` 0.16, `updateMoveDampedAsync` 0.15, `updateMovedList` 0.14) | ~1.1 | legacy | Moves drawables between octree groups; the octree is mutated by one thread. A per-partition owner or a ledger applied by partition is the candidate. |
| Textures (`updateImages` 0.46, fetch dispatch 0.44, decode priority 0.31) | 1.2 | legacy | Priority and fetch decisions per texture; independent per texture, a pool candidate. |
| `updateGeom` / `pipeline: updateGeom` | 0.32 | legacy | Part of the geometry rebuild above. |

What this changes in the plan: W4 (launch early / join late) is worth at most about 0.24 ms in the still phase. The big pieces are owner-based: avatars (2.7 + 0.7), the UI (1.1), the octree movement (1.1), textures (1.2), the group geometry rebuild (0.3 to 0.5), the shadow recording (1.0). Each needs an ownership rule, not a faster pool. One cheap dial first: the pool is capped at 8 threads (7 workers) on this 32-thread machine, and the main thread works a share of every batch; a larger pool shortens every parallel stage that has enough jobs (`LL_VK_POOL_WORKERS` to try).

### 8.2 The avatar update (lap `av1-att1`, FS_AVATAR_TIMERS=1, about 100 avatars, ms a frame on the main thread; design target 150 avatars, Adam: 100-150 is the most a sim has)

| Part | ms | Notes |
|---|---|---|
| idle loop, `updateCharacter` (67 avatars a frame) | 0.39 | root position and rotation 0.09 (35 avatars), the agent's own avatar inline 0.14-0.16 (its motion stops go to the network), hidden avatars 0.06 |
| deferred motion blend, parallel (34 avatars a batch) | 0.37-0.42 wall | skeleton blend and world matrices; every avatar owns its joints, already a pool job per avatar |
| **completion loop, serial** (34 avatars, about 27 us each) | **0.87-0.93** | `updateCharacterPostMotion` (head offset, footstep sounds, matrices) and `idleUpdatePostCharacter` |
| `idleUpdateMisc` inside it (67 avatars, 12 us each) | 0.8 | mostly the attachment loop (0.75): per attachment the rigged box override, drawable move and bridge move |
| appearance animation | 0.2 | the avatar's own visual params |
| name tags | 0.16 | HUD name tag objects |

Why more workers do not help the batch (15 against 7 workers: 1.32 against 1.39 ms): 70% of it is the serial completion loop, which runs after the parallel blend on the calling thread.

What the completion loop touches that is not the avatar's own: the sound system (footsteps), the voice client (visualizer, lip sync), the HUD name tag objects, the selection manager (is an attachment selected), and the octree through each attachment's drawable move and bridge move (partition moves, the moved list). What is the avatar's own: its joints and matrices, head offset, visual parameters and appearance animation, the box of its drawable.

Direction (two phases per avatar, the shape the sort already has): **A, parallel, one job per avatar:** blend, matrices, head offset, appearance animation, and the attachment loop's computing (the boxes, the moves it would make) written into a ledger of effects. **B, serial, in avatar order:** apply the ledger to the octree and the moved list, footsteps, name tags, voice. The attachment loop already has the "skip when nothing changed" logic that keeps most of the effects away. Estimate (soft, not tried): the serial part falls from 1.7 ms (completion loop plus attachment moves) to about 0.7, and the extra workers then matter because phase A has 34 to 100 equal jobs. The agent's own avatar stays inline (network).

### 8.3 The avatar phase A/B split: what the loop writes to shared state, and how it is checked (read 2026-10-31, before any code)

Measured first (probe, short zone laps): the attachment loop is 0.67 ms for about 330 attachments a frame; skipping 148 rigged visits on skip frames recovers only 0.10 ms, so a visit cache is not the lever. The moves themselves are about 0.16 ms. The avatar update is a hundred small pieces (blend 0.4, completion loop 0.9, idle updates 0.8, name tags 0.16, appearance animation 0.2, attachments 0.67): the gain is in running all of them as one job per avatar (phase A) and applying the shared writes in avatar order afterwards (phase B). Direction (Adam): structural, not the size of one step; see feedback-structure-over-single-gains.

**The shared writes of the attachment loop (what phase A must record, phase B must replay, in the order the serial loop makes them):**

1. `LLPipeline::mMovedList` / `mMovedBridge` push_back and the drawable's `ON_MOVE_LIST` bit (`updateMoveDampedAsync`, `updateMoveNormalAsync`, `markMoved`). The bit is read by the next call for the same drawable, so it is set at record time (the drawable's own state) and the push is replayed.
2. `LLDrawable::makeActive()`: state `ACTIVE` is the drawable's own, but it also marks the faces' texture records volatile (`LLTexRecs::faceChanged`, a shared table), makes the parent active first (and sets `ACTIVE_CHILD` on it) and every child object active: a hierarchy walk with shared writes, not one queue entry. Only a drawable that is not yet active reaches it (the moving attachments of a frame are mostly active already), so the replay can be a single recorded `makeActive(drawable)` call.
3. The octree: `LLDrawable::movePartition()` -> `LLSpatialPartition::move` -> `updateInGroup` / `remove` + `put` (`override_bbox`, `updateMove`, the bridge move). `updateSpatialExtents` is the drawable's own and stays in phase A.
4. `markRebuild(drawable / group)`: already recorded by the sort ledger (`sSortSink`), reused.
5. The render order written on the attachment's group (`group->mAvatarp`, `group->mRenderOrder`): groups are shared between avatars, last writer wins by avatar order, so it is replayed in order.
6. `updateText()` on the attached object (the HUD text of the object).

**Reads in phase A that expect an earlier write of phase B (the audit):** `getSpatialBridge()` after `override_bbox` (movePartition can change the partition), `getSpatialGroup()` before the render order is written, `isState(ON_MOVE_LIST / EARLY_MOVE)`. Each is recorded with the value phase A saw, and phase B compares it with what it finds when the op is replayed: a **stale-read counter** (0 expected). That is the check: it needs no snapshot of the octree, and it counts exactly the cases where deferring a write would have changed what a later step saw.

Steps: A1 the ledger and the replay with the counter, run serially (record every op in the loop, replay in order at the end of each avatar; the frame must come out the same, the counter must stay 0); A2 phase A as one job per avatar with a ledger each, replayed in avatar order (an avatar keeps an inline path for the agent's own avatar); A3 the avatar-local pieces (appearance animation, head offset, matrices) in the same jobs. Each behind a setting, the old path kept for the check until the soak.

### 8.4 The inventory again, with the avatar jobs on (lap `inv2-poolnewz1`, short, zone timers on, 76 avatars, 701 nearby lights, 2026-10-31)

Loop 10.93 ms (12.3 in the first inventory; the scenes differ). The main thread blocks 0.2 ms in all in the pool joins, as before.

| Piece | ms | Before | Notes |
|---|---|---|---|
| `gObjectList.update` | 1.78 | 2.71 | avatar motion batch 0.77 (1.57), idleUpdate (avatars) 0.65, attachment loop on main 0.13 (0.79), flexible objects 0.15 |
| `generateSunShadow` | 1.81 | 2.09 | renderShadow recording 0.75 (0.96), cull and cloud waits 0.33, spot/projector 0.15 |
| `stateSort (early)` | 1.19 | 1.12 | postSort 0.67 (priority groups 0.32, render map 0.13), apply deltas 0.22, compute waits 0.13 |
| `renderDeferredLighting` + post-deferred geometry | 1.17 + 0.95 | 1.05 + 0.85 | command recording |
| `render_ui` | 1.11 | 1.12 | widgets 0.80, name tags 0.13 |
| `avatar velocity` (rigged draws: collect, palettes, motion test 0.53; collect draw infos 0.15) | 0.63 | 0.68 | walks every rigged draw: per-avatar data |
| `updateCull` | 0.45 | 0.44 | |
| textures (`updateImages` 0.32, fetch dispatch 0.30, decode priority 0.22 + 0.17) | ~1.0 | 1.2 | |
| octree movement: `partition move` 0.22 (295 calls, includes `updateInGroup` 0.17), `updateMovedList` 0.17, `idle: updateMove` 0.10 | **~0.4** | "~1.1" | **the first inventory added inclusive zones that nest in each other** (partition move contains updateInGroup contains movePartition); the cost of the octree movement bookkeeping is about 0.4 ms, not 1.1 |

Corrections: (1) the octree movement is about 0.4 ms in all, and the avatar jobs have taken the attachment share of it out of the completion loop but their octree moves are still made on the main thread when the ledgers are replayed; (2) the avatar update has come down about 0.9 ms between the two inventories (some of it scene); the pieces left are the idle updates (0.65), the motion batch residue (blend 0.4 plus the join and replay) and the avatar velocity pass (0.63), which walks every rigged draw and is per-avatar data.

### 8.5 Octree movement (read 2026-10-31; Adam chose it for being structural, knowing the cost is about 0.4 ms)

Two parts, and they are not the same problem:

1. **The moved list** (`updateMovedList(mMovedList)` in `updateMove`, `updateMovedList(mMovedBridge)` in `updateGeom`; 0.17 ms in the inventory scene, much more in a scene with many moving prims or vehicles). Each entry gets `updateMove()` (transform math in `updateXform`, `makeActive`, `updateText`, `movePartition`, rebuild marks), then the `done` handling (`makeStatic`, `ON_MOVE_LIST`, the animated-child rebuild, the erase). The shared writes are all recorded already by the avatar ledger (moved lists, the octree moves, `makeActiveShared`, `updateText`, the rebuild marks through `sSortSink`), plus `dirtySpatialGroup` (a group flag and a group rebuild mark) which still needs a recorded form. The ordering rule is the hierarchy: a child reads its parent's transform and the list holds parents first, so the unit of a job is a **linkset** (all entries of one root, in list order). Plan: a serial pre-pass (the cold-mode skip, the EARLY_MOVE test, grouping the entries by root), one pool job per linkset with a ledger range for each entry, then a serial post-pass in list order that replays each entry's ledger range and does its `done` handling and the erase exactly as the loop does now. Behind `LL_VK_MOVED_PARALLEL`, with the stale-read counter, the old loop kept for the soak.
2. **The apply side** (`partition move` 0.22 ms in 295 calls, of which `updateInGroup` 0.17): the replay of the recorded octree moves is still serial. Most of the calls are the cheap case (the drawable stays in its group: `updateSpatialExtents`, `unbound()` up the parent chain, `OBJECT_DIRTY`). Parallel apply would need ownership of the group and its ancestors (a lock per node, or applying by subtree), for at most 0.1-0.2 ms. Not started: it is the part that changes the octree itself, and the data does not yet say it is worth it. If the cull walk and the group rebuild are reworked later (0.45 and 0.32 ms), the shape of the tree gets decided there.

Estimated gain of part 1 in the inventory scene: under 0.1 ms; the point is the scenes with many movers, and having one path for everything that moves.

### 8.6 The group rebuild: the audit (read 2026-10-31; no code; to come back to)

`LLSpatialGroup::rebuildGeom` -> `LLSpatialPartition::rebuildGeom` (the simple partitions: clear the draw map, count, allocate the group's vertex buffer, `getGeometry`) or `LLVolumeGeometryManager::rebuildGeom` (the volume, bridge and avatar partitions, about 560 lines; `genDrawInfo` another 640). 0.32 ms in `postSort: rebuildPriorityGroups` (4 groups a frame), more in a scene that changes; the avatar bridge groups were three quarters flexible prims' rebuilds before FSFlexibleStayInGroup.

**It is not the same audit as the attachment loop.** The attachment loop's shared writes were queue pushes and octree moves; the volume rebuild reaches into many systems before it makes any geometry:

1. **Static scratch.** The faces are sorted into `sSimpleFaces[i]`, `sFullbrightFaces[i]`, `sAlphaFaces[i]`, `sBumpFaces`, `sNormFaces`, `sSpecFaces`, `sNormSpecFaces`, `sPbrFaces` (static arrays, `allocateFaces`): two groups cannot rebuild at once until these are per job.
2. **Before the face loop, per drawable:** `vobj->updateTEMaterialTextures` and `updateTextureVirtualSize(true)` (texture stats and fetch priority on textures shared between groups), `gGLTFMaterialList.applyQueuedOverrides` (the global material list), `vobj->updateControlAvatar()` (can create or destroy an animesh control avatar), `avatar->addAttachmentOverridesForObject` (skeleton overrides of the avatar), `gMeshRepo.meshRezEnabled`, `LLPipeline::sVolumeSAFrame` (a global accumulator), `vobj->preRebuild()`.
3. **In the face loop:** `facep->getPool()` / `setPoolType` and `gPipeline.getPoolTypeFromTE` (the draw pools' face lists are shared by every group), the texture of each face (`facep->getTexture()`), the rigged-state change of a drawable on first rigging.
4. **`genDrawInfo`** (per mask: simple, fullbright, alpha, bump, norm, spec, normspec, pbr): creates the draw infos, new `LLVertexBuffer`s (the arena allocator takes a mutex and the render worker's command queue is written), `getGeometry` fills the group's own vertex data from each face's volume (the actual math), `mGeometryBytes`.

**What this says about the shape of a parallel rebuild:** two phases. **Prepare (serial, the cross-system effects)** = items 2 and 3 plus the sorting into scratch; **generate (parallel, one job per group, scratch per job)** = `genDrawInfo` and `getGeometry`, which write the group's own buffers, draw infos and faces. The generate half is where the time probably is (it fills vertex data for every face of the group) and it is group-owned data, so it is the right unit. Open: the split of the 0.32 ms between the two halves is not measured (the zones inside rebuildGeom do not exist at CPU-zone level); three zones (face list and prepare, `genDrawInfo`, `getGeometry`) would say, and are the first thing to add when this is picked up. Also open: `LLVertexBuffer` allocation from a pool thread (the arena mutex is there; the render worker's queue write is the main thread's), and whether `getGeometry` reads anything a concurrent job writes (a face's texture entry, a volume's LOD data shared by several drawables).

**Verdict:** tractable as prepare/generate, not as the plain record-and-replay of the avatar loop; the first step is the measurement, the second the per-job scratch arrays (a mechanical change with a check: the output vertex data and draw infos of a serial rebuild compared with the split one, which is a stronger check than the stale-read counter because the result is a buffer, not a queue).

### 8.7 The group rebuild measured (laps `rb1-rebz1`, `rb2-rebz1`, full laps with the turn, zone timers on; zones added in llvovolume.cpp and LLSpatialGroup::rebuildGeom, kept)

| Zone (ms a frame) | still before the turn | the minute of the turn | still after |
|---|---|---|---|
| `rebuildPriorityGroups` (4 calls) | 0.26-0.28 | 0.65-0.72 | 0.30-0.32 |
| `genDrawInfo` (2-3 calls; 12-14 in the turn) | 0.05-0.06 | 0.29-0.34 | 0.07 |
| of it, `getGeometryVolume` (the vertex math; 25 calls, 109-124 in the turn) | 0.03 | 0.15-0.18 | 0.03 |
| prepare (the face list into the scratch) | 0.02-0.03 | 0.09-0.11 | 0.02 |
| `LLSpatialGroup::rebuildGeom` -> partition `rebuildGeom` | 0.31 (457 calls) | 0.79 (835 calls) | 0.34 (484 calls) |
| the scene geometry fragment build after it | 0.03 (457 calls) | 0.07 (835) | 0.03 |

What it says: the real rebuild work is small. In the stills the generate and prepare halves together are about 0.09 ms; the rest of what `rebuildPriorityGroups` shows is **457 to 835 calls a frame to the group rebuild, most of them early-outs** (the group is not `GEOM_DIRTY`), and in a zone lap each call carries about 0.5 us of zone overhead, so the 0.31 and the 0.26 are mostly the timer: the same artefact as the visit cost of the attachment loop. After a turn the work is 0.45 ms (generate 0.34, prepare 0.11) and lasts the seconds it takes the new geometry to be built. The scene fragment build is 0.07 us a call: not a cost.

**Verdict:** the parallel generate half (plan 8.6) would gain about 0.05 ms in a still scene and 0.2 ms for the seconds after a turn, against the per-job scratch arrays, the pool allocation and the output comparison it needs. Not worth building now. Two things worth keeping: (1) the 457 visits a frame (`postSort: rebuildGeom (drawable groups)` and the priority queue walk the groups that are not dirty) are a candidate for a dirty list instead of a scan; (2) the after-turn spike is where the generate half would matter if that spike is ever what is being fought. The zones stay in.

### 8.8 R2: what genDrawInfo writes outside its group (read 2026-10-31, no code)

`genDrawInfo` (640 lines) sorts the group's faces of one kind by what breaks a batch, makes one `LLVertexBuffer` per batch, copies each face's geometry into it (`getGeometryVolume`), and registers each face in a pass (`registerFace`, which builds the `LLDrawInfo` in `group->mDrawMap`). What it touches:

- **The group's own, safe to give to a job:** the faces and their vertex buffer, index and texture index fields (a drawable is in exactly one group, so its faces are too), `group->mDrawMap`, `group->mBufferMap` (through a local map), `mGeometryBytes`, the sort of the scratch array (per thread since R1).
- **Reads of shared state, safe:** `LLShaderProgram::sIndexedTextureChannels`, `LLPipeline::sBakeSunlight`, `sTextureBindTest`, the selection flag of an object, RLV state, the region's render matrix, texture entries.
- **Non-atomic reference counts on shared assets, NOT safe.** `LLMaterial` derives from `LLRefCount` (plain `S32`), and so does `LLTexture` (virtual base of every viewer texture). `te->getMaterialParams()` returns a smart pointer **by value**, so every call copies it: an increment and a decrement of the count of a material that other groups' faces share; `registerFace` does it several times a face (`getMaterialParams().get()`, `->getHash()`), and a draw info holds a pointer to its texture and its material. Two groups rebuilt at once that share a material or a texture would race on those counts (lost updates: a leak or a premature free).
- **Vertex buffer creation:** `new LLVertexBuffer` and `allocateBuffer` go to the arena (mutex) and the worker's command queue; not examined further.

**What this means.** The vertex math (`getGeometryVolume`, `allocateBuffer`) writes only the batch's own buffer and could run in a job. The draw info registration cannot, as long as it copies counted pointers to shared assets. The part that can go parallel is about half of `genDrawInfo`: 0.15-0.18 ms of the 0.29-0.34 ms in the minute of a turn, 0.03 ms in the stills. That is not worth the change by itself.

**The structural finding is the reference counts.** Anything that touches draw infos, faces or materials from more than one thread is stopped by non-atomic counts on shared textures and materials; making `LLRefCount` atomic everywhere would add a locked instruction to every smart pointer copy in the viewer (hundreds of thousands a frame). The way out that fits the rest of the design: draw infos and faces refer to textures and materials **by handle** (the texture handle table, plan 17.x) and the objects are kept alive by their owners, so the rebuild copies no counted pointers. That is a larger change than the rebuild, and it is what would let the whole render-side data model be built and read by several threads.

**Verdict:** do not parallelise the group rebuild now (gain 0.05 ms still, 0.15 ms in a turn); the shadow prep and the post-sort rebuild stay serial. R1 (per-thread scratch) is harmless and stays. The shadow pass in motion (plan, section on the turn: +2.1 ms) is the next piece.

### 8.9 R3a done, R3b stopped before it started (2026-10-31)

R3a (genDrawInfo in plan, allocate and fill passes; c5187b4c) and the reference count work (levels 1 and 2; 26fa75f2) are in and cost nothing. Reading `registerFace` for R3b shows why the parallel fill does not pay: it **merges each face into the previous draw info of the same pass** (`info->mCount += facep->getIndicesCount(); info->mEnd += ...`, `draw_vec[idx]` is the last draw info of `group->mDrawMap[passType]`), and several masks register into the same pass vector (the simple and the fullbright faces can both land in PASS_SIMPLE), so the registration depends on the order of the faces and on the other masks of the group. It has to stay serial and in order. The part that is order-free is the vertex data (`getGeometryVolume` into the batch's own buffer): 0.15-0.18 ms of the minute of a turn, 0.03 ms in the stills, in batches of one to three per call. Parallelising only that would gain 0.1 ms in a turn at best, with the mapped-buffer guard and the face-list handoff to pay for it. **Not built.** If the vertex data is ever the cost to fight, the fill pass of R3a is where it goes (the buffers are already allocated before it).

### 8.10 R3b2 done (switch `LL_VK_REBUILD_PARALLEL`, default off) and measured: no gain at this grain; the rest of rebuildGeom is early-out visits (2026-10-31)

R3b2 splits the fill of a group into one task per render batch (`FillTask`, `prepareBatch`, `fillBatch`, `runFillTasks`) and runs the batches of one group on the pool. What it needed: `LLGLTFMaterial` and `LLMeshSkinInfo` on `LLAssetRefCount` (and `LLThreadSafeRefCount::ref/unref` const, so `LLPointer<const X>` works), a mutex on the mapped-buffer list, and **a main-thread prepare step**: `LLFace::setVertexBuffer` writes the sculpt size table (a global multi-index: the first parallel lap crashed in `LLSculptIDSize::inc`), `setGeomIndex`/`setIndicesIndex`/`setSize` free the face's old buffer, and `stampTextures` raises a counter on textures shared between faces and tells the texture record worker (main thread only). Batches with an animated child, a selected object or face, or a rigged face (its avatar is held by the draw info, and an avatar's count is not atomic) stay on the main thread.

Laps `r3b2d-rebz1` / `r3b2d-rebparz1` (full, zones, same pose, avatars 82/78, lights 636/590): in the minute of the turn the main thread runs 66 % fewer `getGeometryVolume` calls (141.7 -> 47.6 a frame, 0.172 -> 0.093 ms) but `rebuild: fill tasks` costs the same per call (15.3 us / 15.0 us): the launch and join of the pool for 12 faces a group cancel what the move saves. **No gain.**

**What is in the rest of `rebuildGeom` (the 0.8 ms I called unexplained):** it is visits, as 8.7 said. The group rebuild is called for 825-957 groups a frame (`stateSort` and `postSort` call `rebuildGeom` on every drawable group, the shadow prep on every visible group with `NEW_DRAWINFO`), and all but 13-15 of them return at once because the group is not dirty; each call carries the zone timer, so the zone time is mostly timer. The work that is real, in the turn: `genDrawInfo` 0.39 ms (fill 0.23), prepare 0.13 ms, plus the planning and allocation. About 0.5-0.6 ms in the seconds after a turn, 0.1 ms in the stills.

**Direction.** The batch across groups (R3b3: one launch for all the groups a frame rebuilds) would pay the pool launch once instead of 13-15 times a frame, for at most 0.2-0.3 ms in a turn. The cheaper-work items (material hash cache, stamp only on a change, bind shape matrix per skin, texture coordinate fast path) lower the same 0.23 ms. Both are small next to the shadow pass in motion (+2.1 ms) and textures in motion (+2.0 ms): those go first. R3b2 stays in behind its switch (the split and `prepareBatch` are what R3b3 would build on); delete it with R3b3 if that is never built.

### 8.11 R3b3: the fills of all the groups of a loop in one launch (`LL_VK_REBUILD_PARALLEL=2`, default off; 2026-10-31)

`LLRebuildBatch` (llspatialpartition.h, implemented in llvovolume.cpp): while a `LLRebuildBatch::Scope` is open on the main thread, `LLVolumeGeometryManager::rebuildGeom` plans and allocates as before, then hands its fill tasks to the batch (the faces are copied out of the thread's scratch, the main thread's part of the fill, `prepareBatch`, runs at once, the animated / selected / rigged batches stay serial). `LLSpatialGroup::rebuildGeom` builds the scene fragment of a deferred group at the flush. One `flush()` runs every group's fills in one pool launch, then merges the draw maps in group order and builds the fragments. A pending group that is rebuilt again or has `rebuildMesh` called is flushed first. Scopes: `rebuildPriorityGroups`, the postSort drawable-group loop together with the priority groups, `shadowPrepRebuild` (one scope for the whole function; flushed before the mesh-dirty pack). Not batched: the stateSort loops and the render-map rebuild loop (code after each rebuild reads the group).

Laps `r3b3d-rebz1` / `r3b3d-rebbatchz1` (full, zones; avatars 83/86, lights 568/562; clean, no crash, picture normal in the short lap): main thread, in the minute of the turn, `partition rebuildGeom` 0.963 -> 0.717 ms, `genDrawInfo` 0.428 -> 0.190 ms (the fill moved out), the new `batch: flush` 0.178 ms (it holds the fills, the merge and the deferred fragments; 0.8 flushes a frame); net about 0.07-0.15 ms less main thread time in a turn, and about 0.1 ms in the stills (445 against 498 visits, so not exactly the same scene). A small gain, as 8.10 said; the point is that the launch cost is paid once a loop and the pool is now the place where a group's fill happens, which the shadow prep and the avatar groups can share. Frame times of the laps do not show it (loop 17.5 / 17.5 ms in the turn): do not read fps from zone laps.

Next here: the cheaper-work items on the fill (hash cache, stamp on change, bind shape matrix, tcoord fast path), the stateSort loops, and then the shadow pass in motion.

### 8.12 Cheaper fill (`LL_VK_CHEAP_FILL`, default on, 0 = old paths, 2 = verify; Adam's rule: make the work cheaper as well as moving it; 2026-10-31)

Three of the four items of the list, done because Adam asked for them; the measured cost said each is about 3 % of the 0.26 ms fill (1.6 us a face, almost all vertex math), so the total is under 0.05 ms and was not measured on its own (the zone laps cannot show it).
1. **Material hash**: `registerFace` hashed the material (xxh128 over the object) for every face; the faces of a batch are sorted by material, so the hash is kept for the run of faces of one `fillBatch` (thread-local memo, on only inside `fillBatch`, cleared on entry and exit, so it cannot outlive a fill).
2. **Bind shape normal matrix** (transpose of the inverse, per rigged face): the last one per thread is kept with the matrix it came from, compared by value.
3. **Texture coordinates**: a channel that is not planar, has no texture matrix and no transform is a `memcpy` of the volume face's coordinates instead of the per-vertex copy loop.
4. **Texture stamp: not done.** `stampTextures` is already deduped (`mark_dirty` and `note_stamp` skip what is already queued) and costs a few ns; a signature of the texture entry to skip it would cost as much as the stamp.
Check: `cheapverify` lap (mode 2 compares every cached hash and matrix with the computed one and logs `CheapFill` on a difference): no warning, no crash, picture normal.

### 8.13 The shadow pass in motion: where the time is, and the pipelined render map that did not pay (2026-10-31)

Lap `r3b3d-rebbatchz1` (zones, same pose): `generateSunShadow` 1.92 ms in the still, 4.07 ms in the turn (+2.15). The far cascades reuse their maps in the still (cascade 0 renders every frame, 1-3 every 4th: `FSShadowFarCascadeInterval` 4, reused only while the camera moved under 0.05 m and turned under 3 degrees since that cascade was last rendered), so a turn re-renders all four every frame. Growth by piece: `renderShadow` recording +0.95 (4.4 -> 6.5 passes: 4.0 cascades in the turn against 1.75, the spot/projector maps (at most 2, cheap, 0.16 ms) and about 0.4 reflection probe face renders a frame, each with 4 cascade passes, which the cascade counters exclude), waits for the pool jobs +0.73 (the main thread is idle for 0.23 ms of it in the still, 0.45 in the turn; the rest is it running its share of the jobs), shadow alpha packets +0.29, rebuild +0.27, static fragments +0.14, spot/projector +0.15. The four cascades are one shadow map each per distance slice: needed with ray tracing off; fewer is a quality cut.

**Experiment: overlap the render map of the cascades with the recording of the earlier ones** (one launch per cascade, then join / merge / submit builders / record in order; `LL_VK_PREP_PIPELINE`). Laps `pipez-pipeoffz1` / `pipez-pipez1` (zones, full, avatars 96/94): `generateSunShadow` 2.05 -> 2.14 ms in the still and 4.01 -> 4.54 ms in the turn, loop 18.6 -> 19.2: **worse**. The render map wait did go (0.34 -> under 0.1), but `packets: wait for the alpha builder` went from 0.04 to 0.58 ms: the old order submitted every cascade's builders before the first recording, so the builders of cascades 1-3 ran behind the recording of cascade 0; submitting each cascade's builders just before it is recorded leaves the recording waiting for them. A lag of one cascade would win some of it back, best case about 0.1 ms. First reverted, then restored (Adam: structural work stays; the overlap of the prep with other work is the structure W4 needs): behind `LL_VK_PREP_PIPELINE`, **default off**; the open item is to submit each cascade's builders one cascade ahead of its recording (a lag of one), which should win the builder wait back.

What is left of the prep overlap: the cascade cull and point cloud with the world cull (needs the cascade plan split out of `generateSunShadow` and ordered against the ledger apply; the idle to win is 0.23 ms in the still, 0.45 in a turn), and recording the cascades on pool threads (about 1.5 ms of main thread in a turn; the sub-stream test exists). The cascade reuse in a turn is the quality question (a rotation-invariant fit).

### 8.14 Census of what the cascade culls give the CPU (lap `scen-shadowcensus1`, full, no zones, `LL_VK_SHADOWCENSUS=1`, per frame averages; 2026-10-31)

Question (for GPU-driven shadow casters): besides a draw list, what does the CPU cull of a cascade produce? "New" = not stamped visible by the world camera this frame. Groups found / new, in the still, then the minute of the turn (cascade 0 / 1 / 2 / 3; frames where a far cascade reuses its map do no cull, so the still counts are a quarter of a real cull's):

| | still | turn |
|---|---|---|
| visible groups found (new) | 39 (19) / 265 (129) / 822 (584) / 900 (682) | 39 (15) / 1266 (764) / 3777 (2681) / 4645 (3547) |
| drawables stamped visible (new) | 109 (9) / 224 (118) / 271 (138) / 140 (77) | 82 (30) / 547 (322) / 800 (400) / 650 (206) |
| groups LOD-checked (new), drawables in them | 1 / 8 / 26 / 28, drawables 5 / 22 / 89 / 95 | 1 / 40 / 119 / 146, drawables 5 / 104 / 381 / 491 |
| rebuilds in the shadow prep, groups only the cascades see | mesh 0.03, geometry 0, new draw info 0 | mesh 1.38, geometry 0, new draw info 0 |

Reading: (1) **Most of what a far cascade finds is not seen by the world camera** (60-76 % of cascades 2 and 3). (2) **The cull is not only a draw list**: it stamps about 340 drawables a frame in the still and 560 in the turn as visible that the world camera does not see (what reads that stamp: avatar animation gating, texture priority and so pre-fetch for what is behind the camera, damped motion), checks the LOD of 60-300 groups and rebuilds 1.4 meshes a frame in the turn. (3) **A turn costs more because the far cascades are culled every frame instead of every fourth**, not because a cull finds more: one real cull of cascade 2 is about 3.3k groups in the still (822 x 4) and 3.8k in the turn. (4) Render map of the cascades: 125 of 185 groups a job have a current resident fragment (68 %); 264 static draw infos, 123 alpha, 61 other, 19 rigged static a job.

What this means for GPU-driven casters: the draw lists (render map 0.34 ms and the static fragment recording 0.2 ms in the turn) could go, but the walk, the stamps and the LOD checks have consumers and stay (C2 found the flat pass costs what the walk does): about 0.5-0.7 ms in a turn, about 0.25 in the still, not the 1.5 ms I estimated. The larger lever is how often the far cascades are re-rendered in a turn (frequency x4), which is the quality question (cascade reuse while the camera turns).

### 8.15 Cascade reuse by coverage (`LL_VK_CASCADE_COVER`, default on, 0 = the old rules; laps `cov-coveroffz1` / `cov-coverz1`, full, zones; 2026-10-31)

What it does (pipeline.cpp, generateSunShadow): a far cascade (from `FSShadowFarCascadeFirst`) reuses its map while every receiver point of the current slice (the point cloud the new fit is made from) lies inside the old map's view and projection (1 % slack), with the sun angle, the field of view and the age cap as before; the camera-move and camera-turn limits and the split-distance match are gone. While the camera moves, the far cascades are fitted wider across the light by the angle turned and the distance travelled in a reuse interval (as shares of the field of view and of the slice width, capped by `FSShadowFarCascadeMaxMargin` 0.12), zero when the camera is still, so a still camera gets the same fit and density as before.

Result (the turn, left arrow held, 60 s): far cascades render 186 / 199 / 203 of 300 frames (300 each before), 312 reuses per 300 frames (0 before; the still 675 as before); `renderShadow` passes 6.6 -> 5.5 a frame, `generateSunShadow` 4.33 -> 3.75 ms (-0.58), render map wait 0.39 -> 0.31, rebuild 0.38 -> 0.29. The still is unchanged (renders per 300 frames 300 / 75 / 75 / 75, as before). Reuse in the turn is a third of what is possible: the 12 % cap and the new receivers that come into view ahead of the turn are what end it. Adam, watching the lap: one or two shadows popped back in; he put one down to an avatar whose idle animation had a bigger walking radius (the receiver bounds the coverage test uses are the group bounds, which lag an avatar's animated extents; a cheap fix if it matters: add the avatars' extents to the test).

### 8.16 The passes of renderShadow by kind (lap `kinds-shadowcensus1`, full, no zones, `LL_VK_SHADOWCENSUS=1`; 2026-10-31)

| kind | still passes a frame | still ms a frame (us a pass) | turn passes | turn ms (us a pass) |
|---|---|---|---|---|
| main view sun cascades | 1.75 | 0.73 (417) | 2.90 | 1.30 (446) |
| reflection probe faces | 0.67 | 0.015 (23) | 0.66 | 0.017 (25) |
| spot / projector maps | 2.00 | 0.106 (53) | 2.00 | 0.258 (129) |

Corrections to 8.13: the probe face passes are 0.67 a frame, not 1.6, and cost next to nothing (23 us a pass); the spot and projector maps are always two a frame. The 4.4 passes of the still are 1.75 + 0.67 + 2.0, and the cost is the sun cascades (417-446 us a pass, with the coverage reuse 2.9 passes in the turn instead of 4.0). The spot maps cost 0.1 ms in the still and 0.26 ms in the turn (the serial updateCull and stateSort of each spot frustum grow with the camera's motion). The unexplained part of 8.13 is not timers (about 140 zone entries a frame at about 0.2 us in `renderShadow`; `shadowPrepFinish` 0.02 ms in the still, 0.08 in the turn): it is about 0.1 ms of fixed work a cascade pass with no zone of its own.

What is left on the shadow side, each small or a trade: a higher margin cap (more reuse in fast turns, a few percent less far shadow density while turning; the turn is at 2.9 of 4.0 passes now, 1.75 in the still); avatar extents in the coverage test (the pop-backs); reuse of the spot maps when nothing in their frustum moved (0.1-0.26 ms, casters such as avatars make the test hard); GPU-driven casters (0.5-0.7 ms in a turn).

### 8.17 Textures in the turn (lap `cov-coverz1`, full, zones; the logs of `VkTexStamp` and the texture records; 2026-10-31)

Zones, the still against the minute of the turn (ms a frame on the main thread): `gTextureList.updateImages` 0.53 -> 1.28 (+0.75; of it the per-texture decode priority 0.32 -> 0.85, calls 165 -> 798 a frame, and inside it the face loop 0.27 -> 0.61, calls 44 -> 143), objectlist animated textures 0.12 -> 0.20. So the texture growth in a turn is about +0.85 ms, not the +2.0 of the first inventory (that figure came from nested zones and a different scene).

What drives it: the slice of the texture list refreshed each frame is max(32, N / divisor); the divisor halves while the view is steady and returns to the fast rate (a burst, 3 s after the camera turns more than about 16 degrees or moves 2 m): in a turn about 800 textures a frame are looked at against 165. The kept face loop results (`LL_VK_TEXSTAMP`) are never valid in a turn (the camera epoch changes every frame: kept 0 against 2.5-3.6k per 10 s in the stills), so every look goes to the worker's record (`LL_VK_TEXREC=2`): per 10 s, in the turn 348k sized from the worker's result, 85k from the main thread's face loop (68k and 33k in the still). **Of the main thread's face loops, 84 % in the turn and 95 % in the still are for volatile textures** (moving, rigged, particle or media faces: 31k a 10 s in the still, 71k in the turn), which the worker's record cannot answer because its faces change every frame. A look at a texture whose record the worker can answer costs about 0.3 us; a face loop 4.3 us.

Levers: (1) a volatile texture keeps the steady rate during a burst (the burst exists so that newly visible textures are requested quickly; a volatile one is an avatar's or a moving object's texture): saves about 0.3-0.4 ms in a turn, costs up to 0.35 s more before an avatar that was off screen asks for its textures again; (2) a cheaper face loop for rigged faces (the avatar's extents once a frame for all its faces; the rigged calcPixelArea already refreshes at 4 Hz); (3) the worker answering volatile textures (the records of moving faces are dirty every frame: 1.4M faceChanged calls a 10 s); (4) the looks on the pool (texture state is shared: a retrofit).

### 8.18 O1, the moved list as per-linkset jobs: built and measured (kept, behind `LL_VK_MOVED_PARALLEL`, default off) (laps `mvz-movedoffz1` / `mvz-movedz1`, full, zones; 2026-10-31)

Built as in 8.5 part 1 (`LL_VK_MOVED_PARALLEL`): a serial pre-pass (the cold skip, null and early-move entries, grouping by linkset root), one pool job per linkset of plain volume prims (no avatar, attachment, flexible, rigged or animated object), each recording its writes to shared state (the avatar ledger kinds, the sort deltas, and two new forms: `DIRTY_GROUP` for `dirtySpatialGroup`, and a texture stamp list for `LLFace::updateCenterAgent`), and a serial replay in list order that made each entry's writes and did its done handling or ran the entry the old way. Clean laps, no crash.

Result: about 260 entries a call, 93 of them run as jobs in 55 linksets (a microsecond each), 39 ledger writes and 260 texture stamps replayed a call. Main thread time in `updateMovedList`: still 0.109 ms serial against 0.118 ms parallel, the minute of the turn 0.305 against 0.292 ms. **No gain**: the replay and the launch cost what the jobs save, and the bulk of the list (avatars, attachments, early-move entries) and the bridge list stay serial. **Kept** (I reverted it at first and Adam pointed out that 8.5 gave under 0.1 ms and called the point structure: one path for everything that moves, on the ledgers the avatar work made; restored from the saved diff): behind `LL_VK_MOVED_PARALLEL` (default off), the old loop is the fallback and the check, as every step here. What it is for: the ledger forms for a generic move (`DIRTY_GROUP`, the texture stamp list, the range replay), the linkset grouping, and a path that gains where many plain prims move (vehicles, physical objects), which these laps do not have.

Left over from it: the replay stamps the textures of about 260 faces a call, all faces of active drawables whose textures the priority loop already treats as volatile; the stamp also wakes dormant textures, so it was not removed. The octree movement is not a lever at this scale (0.1-0.3 ms in all).

### 8.19 Texture priority: the face loops as pool jobs (`LL_VK_TEX_PRE`, default OFF since the avatar spikes (below), 1 = on, 2 = verify; laps `tp2-texpreverify1`, `tpz-texpreoffz1`, `tpz8-texprez1`; 2026-10-31)

What it does (llviewertexturelist.cpp): before the per-texture pass of a slice (`updateImagesFetchTextures`), pool jobs (chunks of 8 textures, `LL_VK_TEX_PRE_CHUNK`) compute the face loop of `updateImageDecodePriority` for every texture the kept result (`LL_VK_TEXSTAMP`) and the worker's record (`LL_VK_TEXREC=2`) cannot answer; the function then takes the precomputed values where it would have run the loop (`FaceLoopPre`, a thread-local pointer set around the call). The apply side is unchanged and stays on this thread. A job claims a face that is due a pixel area refresh (an atomic compare-and-swap on `mLastTextureUpdate`, 0xFFFFFFFF while it is worked out) and runs `calcPixelArea` for it; a texture whose face another job holds, or a rigged face whose rig table needs an update (a write to a volume face that meshes share), is left to the main thread. (First version left every due face to the main thread: 7 % of the loops were computed by jobs, because a texture's faces are nearly always due when it is visited; `calcPixelArea`'s random number generator is thread-local, so it is safe in jobs.)

Check (`LL_VK_TEX_PRE=2`: the loop runs here and is compared field by field, the size bit for bit; the first run's 419 "differences" were NaN sizes): 1,301,218 compared, 0 differed, 98 % of the textures that needed a loop computed by jobs (48.3 of 49.5 a frame). No crash.

Result (main thread, ms a frame, zone laps; serial / pool at chunks of 32 / at chunks of 8): `gTextureList.updateImages` still 0.279 / 0.255 / 0.267, the minute of the turn 0.894 / 0.754 / 0.669; the face loops are off the main thread (0.35 ms in the turn); what is left is the pool stage's wall (0.098 still, 0.164 turn) and the per-texture checks (800 a frame in the turn). About -0.22 ms in a turn, -0.01 ms in the stills. Small, as the plan expected; structural: the texture priority now has a compute phase that reads and an apply phase that writes, which is the shape the worker records and the stamp cache needed.

Left: the checks of 800 textures a frame run twice (the job's predicate and the function's own); the volatile textures' steady rate (lever 1 of 8.17) and a cheaper rigged loop stay as options.

**The avatar spikes and blink-outs (the same evening), and what changed:** with the loops on, Adam saw avatars (his own too) blink out and spikes come out of avatars for a frame or two; bisected with `run-viewer-flicker-A..D.bat` (the day's four new defaults off = clean; D, only these loops on = spikes). Cause: a job for a rigged face ran `calcPixelArea`, which reads the joints' world matrices through `LLJoint::getWorldMatrix()`, which recomputes a dirty joint and writes it: two jobs for faces of one avatar tore a joint matrix (a spike for a low joint, a blink-out for one near the root, until the avatar next updates). A due rigged face is now left to the main thread. Result with that: the jobs compute about half the loops (30 of 58 a frame; 0 differ in 800k compared except denormal sizes, 1e-42 on the pool's threads against 0 on this one, which flushes them: the comparison now treats them as equal) and **the gain is gone** (turn `updateImages` 0.901 against 0.894 ms serial, still 0.350 against 0.279). The rigged faces were the bulk of the loop cost, and the part that gained -0.22 ms was the part that raced.

To get the gain back safely: the avatars' joints have to be clean (their world matrices up to date) before the jobs run, so that `getWorldMatrix()` only reads: a serial pass on this thread over the avatars that own a rigged face in the slice, or the avatar update leaving its joints clean at its end (the per-avatar job). Then rigged faces can go back to the jobs. Not done; the switch stays off until then.

### 8.20 The avatar velocity pass: the frame record, step 1 (laps `tpz-texpreoffz1` before, `vel2-texpreoffz1` after; 2026-10-31)

Corrections to 8.x first: the matrix palettes are already cached once a frame per avatar and skin (`updateSkinInfoMatrixPalette`, under a mutex; the lookups cost 0.02-0.05 ms), so there is no palette work repeated between the shadow, G-buffer and velocity consumers; and the pass is 0.32-0.43 ms in this crowd (the 0.75 ms of the earlier lap had zone overhead of about 850 entries a frame in it and a heavier crowd). Per frame: about 470 rigged draws looked at (190 of them dropped as "the avatar did not move" by the 0.3 px test, 96-190 in the record), 95 moving non-rigged draws.

Step 1 (kept): the answers the pass made in per-frame hash maps now live on the avatar (`mVelocityFrame`, `mVelocityOk`, `mVelocityDistance`) and on its palette cache entry (`mMotionFrame`, `mMotionStill`, `mOffsetFrame`, `mOffset`), stamped with the run of the pass (a counter; a second run in a frame starts over). Result: **neutral** (`avatar velocity` still 0.419 -> 0.431 ms, turn 0.326 -> 0.321): the maps were not where the time went. Kept as the start of the per-avatar frame record: the answers are on the avatar, where the per-avatar update job can fill them and the shadow packet builders can read them.

What the pass costs, in the still: collecting the draw infos of 19 pass types 0.11, the motion tests 0.05, the palette lookups 0.02, about 0.15 for the loop over the draws, the moving-object walk 0.07. Next, in order of structure: (1) the motion test per avatar in its update job (the avatar knows its joints moved; the pass would read the answer); (2) a retained list of an avatar's rigged draw infos (built where the render map is, so the pass does not collect 19 types and loop all draws); (3) the velocity written by the G-buffer's own rigged draws (previous palette in, motion vector out), which removes the separate walk and the extra draws: shader changes in about 20 rigged variants and a render target in that pass.

### 8.21 Stage d: the own avatar's blend in the batch (`LL_VK_AV_SELF`, default on, 0 = inline; laps `vel2-texpreoffz1` before, `std2-texpreoffz1` after; 2026-10-31)

The own avatar was kept out of the motion batch because the motion controller asks it to stop a motion (`requestStopMotion`), which for the own avatar is a message to the server and a change of the agent's state (`LLAgent::onAnimStop`). It runs the same blend code as every other avatar, so that was the one blocker. Now, while the own avatar's blend runs as a pool job (`LLVOAvatar::sBlendJobAvatar`, thread-local), `LLVOAvatarSelf::requestStopMotion` keeps the motion ids (`mDeferredStopIds`) and `completeDeferredMotionUpdate` makes the requests on the main thread (`flushDeferredStopRequests`, `LLAgent::requestStopMotionId`) before the post-motion work. Its attachment loop and post-motion stay serial in the completion loop, as the others' do.

Result (main thread, ms a frame, zone laps): `idleUpdate (avatars)` 0.631 -> 0.266 (still), 0.734 -> 0.316 (turn); `avatar motion batch` 0.684 -> 0.713 and 0.544 -> 0.726; `gObjectList.update` 1.680 -> 1.178 and 1.729 -> 1.410; `idle()` 2.769 -> 2.197 and 3.527 -> 3.058. About -0.35 to -0.4 ms of the inline update (0.15 by the avatar timers, which understate it). Clean short lap, no crash. Hands-on check by Adam 2026-10-31 with the defaults launcher: everything seemed good. (Checked: walking, sitting and standing, animations and an AO, gestures, AFK/away ; what could go wrong was a stop request that is not made or made late: an animation that does not stop, an away state that does not clear.)

### 8.22 What is left of the per-avatar chain on the main thread (reading, 2026-10-31; nothing changed)

After stage d `objectlist: idleUpdate (avatars)` is 0.266 ms still / 0.316 turn over 75 calls (3.5 us each), so the fixed chain I planned to skip cannot be worth more than a part of that. The last lap with the avatar piece timers (`tr3-avtotal1`, 2026-10-27, before the attachment and blend moves) gives the shape: the part before `updateCharacter` 1.2 us each (0.056 ms/frame: `updateTooSlow`, `checkTextureLoading`, `setPixelAreaAndAngle`, the extent rule, `updateMoveDampedAsync`; each about 0.1-0.3 us), `updateCharacter` 8.4 us each of which the root position and rotation is 2.8 us each (0.125 ms/frame: `resolveHeightGlobal`, `updateOrientation`, the root matrix). `idleUpdateMisc` was 29 us each then; it held the attachment loop that A1/A2 moved (`att: attachment loop` is 0.09 ms now).

So: skipping the pre-chain pieces is worth at most ~0.05 ms and moves nothing off the thread (not structural). The root position and rotation is 2.9 us each, but the old piece timers put `resolveHeightGlobal` at 0.2 us, the orientation matrix and euler angles at 0.1 us and `updateOrientation` at 0.3 us each: the rest (about 2 us each, ~0.09 ms/frame) is the position change (`mRoot->touch()` marks the whole skeleton dirty, then `setWorldPosition`) for avatars that moved, which has to happen before the blend anyway. Moving it into the job needs the `mInAir` / `mSpeed` values that `beginNormalMotionUpdate` reads (set by the root step), so the order would change by a frame for the animation state decisions. Not worth it: nothing structural, ~0.09 ms. Left as it is; the chain is finished for now.

Arrival (warm cache), from the per-second zone windows of `std2-texpreoffz1`: the world shows at about -43 s before `still1` (a 8 ms frame, 0.35 ms of it the nearby people panel), one 16 ms window at -33 s (stateSort 3.3 ms: `rebuildPriorityGroups` 2.2 of it, `partition rebuildGeom` 1.9, `updateGeom` 1.4), and the steady 8.3 ms from -28 s. So with the object cache warm the arrival is about 10 s of heavier rebuild frames, which R3b2/R3b3 already work on. The ingest path (cache reads, `processObjectUpdate`, volume and sculpt setup) only shows with a cold object cache: that needs a cold login lap with zones (`run-viewer-cold.bat` / mode `cold`), which means moving the object cache aside first.

### 8.23 Cold login: what the hitch watcher showed (2026-10-31, `run-viewer-coldzones.bat`, caches in an empty `<work>\cold-cache`)

The zones were not on in this run (the launcher lacked `FS_CPU_ZONES=1`, fixed), so this is the hitch watcher only (80 ms; 11 stalls, log `Firestorm.log`):
- **1.6 s freeze at 03:41:47-48 (7 captures), the main thread in `LLFile::fopen` <- `LLFileSystem::write` <- `LLViewerAssetStorage::assetRequestCoro`.** The asset cache file is created and renamed inside the coroutine, which runs on the main thread. The same signature as the 2.4/2.8 s freezes of 2026-10-26 (a file open held up by the OS or Defender; the exclusion covers `F:\Games\SL`, not the cold-cache folder, which is why it shows here). It can hit anybody without such an exclusion, so the write is the thing to move: `assetRequestCoro` now writes and renames the file on the general queue and waits for it with `WorkQueue::waitForResult` (the callbacks still run after the file is in place). `LL_VK_ASSET_WRITE_MAIN=1` writes on the main thread as before (temporary switch).
- Arrival frames of 80-170 ms, one each: `LLPolySkeletalDistortion::apply` from `process_avatar_appearance` (172 ms, `LLVOAvatar::applyParsedAppearanceMessage`: the own avatar's first appearance), `LLCubeMapArray` -> `LLVkAsync::syncCall` in `initReflectionMaps` (105), `checkOcclusion` in `sortLoops` (135), `LLRebuildBatch::flush` -> `fillBatch` (96), `notifyLoadedMeshes` in `rebuildPriorityGroups` (81), `display_startup` -> `beginFrame` (102, 92).
- At quit: `LLWebRTCVoiceClient::drainConnections` (2.6 s, in `LLVoiceClient::terminate`), the final snapshot (`calcNearbyLights`, PNG encode 0.6 s). Not arrival; noted.
Still to see with zones on: the cost of object ingest per frame (cache reads, `processObjectUpdate`, volume and sculpt setup); needs another cold run (empty the folder first).

Second cold run (same launcher with `FS_CPU_ZONES=1`, caches emptied, the Defender exclusion for the cold-cache folder added by Adam, the cache write on the general queue; 44 zone windows, 125 s after the first):
- The `fopen` freeze did not come back: 11 captures, the longest arrival one 99 ms. With the exclusion in place this does not show the move did anything (the freeze may simply be gone with the exclusion); the move stays because the write has no reason to be on the main thread and not everybody has the exclusion. No harm seen (no failed writes in the log).
- **Cold arrival is not an ingest problem on the main thread.** From the world showing (window at 15 s) the heavy part is two windows (12.6 and 14.7 ms loops): `stateSort (early)` 1.2 and 3.8 ms (`rebuildPriorityGroups` 1.9, `partition rebuildGeom` 1.7, 269 calls), `gObjectList.update` 0.8 and 2.1 ms, `idleNetwork` 0.5 ms averaged over the two windows (the object update messages are in there). By 28 s the loop is 7.4-8 ms and it creeps to 9-10 ms as the scene fills (textures, avatars; `gObjectList.update` 1.4 -> 1.65, `generateSunShadow` 1.0 -> 1.6). Same shape as the warm arrival, only a little longer. Nothing in the cold path is a measured main-thread cost worth moving: the cache reads and the object updates are not visible as zones (they sit inside `idleNetwork` and `gObjectList.update`), and the biggest piece is the group rebuild, which R3b2/R3b3 already work on.
- Hitches left (all under 100 ms in the arrival): a binary LLSD parse (`fullread`, 97 ms, at 11 s: a cache or inventory file read on the main thread), `initReflectionMaps` -> `syncCall` (85), one `renderGeomPostDeferred` frame (99). At quit the voice shutdown (`drainConnections`, a deliberate 3 s drain of the WebRTC connections that polls on the main thread; 2.6 s, in the code from Linden) and the final snapshot (PNG encode, 0.6 s): not ours, not arrival.
- Not done: the `fullread` LLSD parse (which file?) and `initReflectionMaps` are 85-97 ms once at login each.

### 8.24 UI: what is left after the record cache (reading, laps `std2-texpreoffz1` and the cold run; nothing changed; 2026-10-31)

The record cache (`FSUIRecordCache` 1, mode 2, 30 Hz, N 3) is running in the laps (`UiCache` log: 153 units, 32 with a saved segment, 659 KB; live draws 2.25 a frame = 0.24 ms, replays 5.75 a frame = 0.014 ms, recording overhead 0.014 ms; the units' own estimate of the saving 0.56 ms a frame). What the main thread still spends on the UI (ms a frame, turn phase):
- `display: render_ui` 0.83: widgets 0.55 (units 0.27 of it: the people floater 0.36 ms a live draw, 0.30 live frames out of 1; the bottom toolbar, status bar, IM box, camera and move floaters 0.03-0.1 each; the other 0.28 is the tree above the units, views under 25 us, menus (blacklisted), big views), HUD attachments 0.14, name tags 0.09.
- `idle: updateUI` 0.19; the other `idle()` pieces that are not objects: `updateApparentAngles` 0.22 (that is the texture priority pass of the objects: parked with the texture pool loops), `updateRegions` 0.21, `updateParticles` 0.19, `idle callbacks` 0.17, `idleNetwork` 0.16, `updateMove` 0.15.
- Rejected recordings: the people floater 21 of 182 (draw ops 1027 and 1024 that a recording cannot hold); that costs it one live draw each.

What follows: the widget tree draws on the main thread because the widgets are the state; the cache already removed the share that does not change. What is left is the live draws at 30 Hz (0.24), the tree above the units (0.28) and small pieces. The next structural step would be a retained picture of the UI (the unit records composited as textures at a lower rate, redraw on a change signal): it saves the live draws too but costs a full-screen composite on the GPU and a signal from every widget; a design question for Adam, not started. Not worth a code change by itself: no single UI piece above 0.25 ms. The idle pieces of 0.15-0.2 ms each (`updateRegions`, `updateParticles` (GPU particles are planned), `idle callbacks`, `updateMove`) are the next place for a look; `updateApparentAngles` goes with the texture pool loops.

### 8.25 The avatar velocity pass: the rigged draws on the pool (`LL_VK_VEL_JOB`, default 1; laps `vj3-veljobz1` job, `vj3-veloffz1` inline; compare lap `vj2-veljobchk1`; 2026-10-31)

`build_velocity_rigged` (collect the 19 rigged draw types, the avatar's answers, the palette lookups, the "did not move" test, the palette record) now runs as a one-job batch on the fork-join pool: launched in `beginAvatarVelocity` (the main view's render map is complete there), joined in `renderAvatarVelocity`, which sends the record and does the moving-object walk as before. It reads the render map, the avatars and their palette caches (under the palette mutex) and writes the avatar frame record of 8.20. `LL_VK_VEL_JOB=0` builds it inline as before, 2 runs the job and then the same build again on the main thread and compares (counts and the bytes of the entries and palettes, logged every 600 frames).

The first compare lap (`vj-veljobchk1`) found: the job built 0 entries every pass (167 on the main thread). `sCull`, the cull result `beginRenderMap` reads, is thread-local in pipeline.cpp: a pool thread has none, so the render map looked empty. The work now carries the launching thread's cull result and the job installs it for its duration. Second compare lap: **0 of 27,600 passes differed** (entries, palette floats, still and skipped counts, bytes), no crash.

Result (main thread, ms a frame, zone laps): `avatar velocity` still1 0.383 -> 0.100, turn 0.270 -> 0.075, still2 0.417 -> 0.096: **about -0.3 ms**; the wait for the job does not show as a zone (the job is done long before the lighting, as the G-buffer is recorded in between). What is left is the moving-object walk (0.05-0.07) and building and sending the record (0.01-0.02). The frame intervals of the two zone laps differ by scene (14.98 vs 14.16 ms in the turn): not a fps read.

Next in this piece: the moving-object walk (the other 0.07) can go in the same job (it keeps a per-matrix history; only the pass touches it); the G-buffer-written velocity (8.20 step 3) would remove the pass altogether and stays a shader/target change. Not done.

The moving-object walk (step 1b) went into the same job (laps `vj4-veljobchk1` compare, `vj4-veljobz1` zones): `sModelMatrixHistory` is the pass's own, so the job updates it; the main thread only sends the record (`moving objects (send)`, under 0.001 ms). The compare now also checks the object entries, matrices and the unfit count: **0 of 30,000 passes differed**, no crash. `avatar velocity` on the main thread: still1 0.100 -> 0.029, turn 0.075 -> 0.022, still2 0.096 -> 0.027 ms (inline before 8.25: 0.38 / 0.27 / 0.42). What is left is the record send (0.012-0.019) and the join. The pass is finished as a main-thread cost; the G-buffer-written velocity (8.20 step 3) would remove it and the extra draws on the GPU, a separate shader step.

### 8.26 Texture pool loops: the rigged faces back in the jobs (`LL_VK_TEX_PRE`, default 1 since 2026-11-01; `LL_VK_TEX_PRE_RIGGED=0` keeps them on the main thread; laps `tr1-` .. `tr3-`; 2026-11-01)

8.19 left the rigged faces to the main thread because `calcPixelArea` of a rigged face reads the joints' world matrices through `LLJoint::getWorldMatrix`, which recomputes and writes a dirty joint. Now a job that meets a due rigged face first makes that avatar's joints clean (`LLVOAvatar::cleanJointMatricesOnce(frame)`: the first job to ask does it, the others for that avatar wait; it cleans the skeleton below the root, the collision volumes and the attachment points), then reads. A rigged face whose rig table needs an update (`mJointRiggingInfoTab.needsUpdate()`, a write to a volume face meshes share) stays with the main thread. The jobs count the dirty joints they meet while computing a rigged face (`gFSTexPreDirtyReads`, logged in the `TexPre` line; 0 is the expectation).

How it got there, and what the counter caught:
1. First version, a clean of every avatar's skeleton below the root before the jobs: **16,368 dirty joints met in 9,000 frames** and Adam saw spikes (the collision volumes and attachment points do not update with the tree walk, so they stay dirty and a job recomputed them). Stopped by Adam.
2. Clean covering the collision volumes and the attachment points too: **0 dirty joints in 30,000 frames**, compare 1.58M faces, 0 differed, 95 % of the loops computed by jobs (52.8 of 55.6 a frame). But the clean of every avatar every frame cost the main thread 0.8-0.96 ms (`clean the skeletons`), so `updateImages` was 1.1 / 1.7 / 1.1 ms against 0.34 / 1.12 / 0.44 with the jobs off: **worse**. (Lap `tr2-`.)
3. The clean on demand, per avatar, once a frame, by the first job that needs it (only the avatars that own a due rigged face are cleaned): verify lap `tr3-tprverify1` **0 dirty joints in 27,000 frames**, 1.46M compared, 0 differed, no crash. Zone laps `tr3-tprz1` (jobs) against `tr3-tproffz1` (off), `gTextureList.updateImages` still1 / turn / still2: **0.341 / 0.906 / 0.369 against 0.400 / 1.092 / 0.473 ms**, i.e. -0.06 / -0.19 / -0.10 ms; the per-texture decode priority 0.064 / 0.270 / 0.077 against 0.229 / 0.705 / 0.247; the face loops that are left on the main thread 8-13 calls a frame against 40 / 135 / 41. 52.4 of the 58.7 loops a frame come from the jobs. Zone laps differ by scene: not an fps read.

Small, as the plan expected (the texture priority is not a big cost), and it makes the texture priority a compute phase on the pool and an apply phase on the main thread for rigged faces too. Not yet the default: Adam to watch avatars (spikes, blink-outs) with the jobs on in normal play.

### 8.27 Inventory after today's moves, and what is left of the avatar pieces (lap `av3-avtotal1`, still1, zones and the avatar timers on, about 54 avatars, 28.5 in the motion batch; the timers switch the attachment jobs off, so the attachment loop runs inline in the completion loop here; 2026-11-01)

Main thread, ms a frame (loop 12.7 ms with the timers; not a fps read): `display` 8.3, `idle` 4.05. The pieces: `generateSunShadow` 2.13 (`renderShadow` recording 0.85, waits for pool jobs 0.74 of it: point cloud 0.25, cull 0.25, mark 0.15, render map 0.10; spot and projector 0.18, rebuild 0.14), `renderDeferredLighting` + post-deferred 1.51 (post-deferred geometry 1.0, local lights 0.22, rt name rigged 0.22), `stateSort (early)` 1.49 (`postSort` 0.93: priority groups 0.45, render map 0.14), `render_ui` 0.85, `updateCull` 0.5, textures `updateImages` 0.29, octree `partition move` 0.23, `updateMovedList` 0.18, `idle: updateMove` 0.10, `gObjectList.update` 2.47 (inflated by the timers: the attachment loop inline; without them about 1.2).

The avatar timers (per frame, 54 avatars): blend wall 0.30 ms, the serial part of `idleUpdate` 0.42 (4.7 us each), the completion loop's own pieces: name tag 0.117 (2.2 us each), appearance animation 0.14 (2.6 us each: the morph of avatars that have just changed, an arrival cost), lipsync, loading effect and wind 0.02, voice visualizer 0.014, head offset, footsteps, root orientation each under 0.01. So the avatar pieces that are left are many items of 0.01-0.14 ms; none is a structural move and the name tag and the morph write shared state (the HUD object list, the mesh dirty flags). Left as they are: the avatar work is at the point where each further step is under 0.15 ms.

The big pieces now are the shadow pass (2.1 ms: record 0.85, pool waits 0.74, spot/projector 0.18), the deferred lighting recording (1.5), `stateSort` (1.5) and the UI (0.85). The pool waits inside the shadow prep (0.74) are the main thread working through the jobs itself or waiting for the slowest: the first thing to look at is how long the longest job of each stage is.

### 8.28 The shadow prep's pool waits: not idle time, and the wake-up that did not matter (`LL_VK_POOL_WAKE`, default 1, 0 = `notify_all` as before; laps `wk1-wakez1` / `wk1-wakeoffz1`, zones, full; 2026-11-01)

The question was the 0.74 ms of "wait for the pool" in `generateSunShadow` (point cloud 0.25, cull 0.25, mark 0.15, render map 0.10 in lap `av3-avtotal1`). Two readings of the zones settle it: (1) the **blocked** column (the main thread really idle in the join, the rest is it running jobs of the batch itself) is only 0.04-0.23 ms of them: in `wk1-wakeoffz1` the cull wait is 0.24 / 0.35 ms of which 0.12 / 0.23 blocked, the point cloud 0.23 / 0.23 of which 0.08 / 0.09; (2) the older job census (`abA-rebuildq1`: cull wall 0.374 ms for a sum of jobs 0.222 and a longest 0.195, 1.75 jobs a run) says the wall of a stage is its longest job plus a start-up delay: a worker that sleeps (the stages of the shadow prep are further apart than the 150 us a worker spins) needs tens of microseconds to be running, and a pool of 15 workers instead of 7 made no stage faster (8.1).

Hypothesis tried: `launch` woke every sleeping worker (`notify_all`) for a batch of one or two jobs. Now it wakes only as many sleepers as the batch has jobs for besides the launching thread (`mSleeping` counted under the mutex; a batch with more jobs than sleepers still `notify_all`; the claim logic untouched). Result: the wake costs **6.4 us a launch against 7.8 us** (the call is not where the time goes), 4.9 workers asleep and 25 jobs per launch on average; `generateSunShadow` 1.96 / 2.35 ms against 2.07 / 2.33 (still1 / still2, within the noise of ±0.1); the cull wait's blocked time 0.04 / 0.10 against 0.12 / 0.23 ms. No `ForkJoin` warnings in either lap. Kept (default on): it is cheap, it cuts the idle time in the cull wait a little, and it removes the thundering herd for small batches; not a gain I would call a measured one.

So the waits are mostly the main thread's own share of the work. What is left in this stage is real work, not waiting: the cascades' point cloud (an octree walk of every partition for each of the cascades: 0.25 ms of main-thread time), the cull (0.2-0.3), the mark (0.14), the render map (0.1) and the rebuild (0.13). The cheaper-before-moved step would be one point cloud walk for all the cascades (their frusta are slices of one frustum) instead of four. Not started.

### 8.29 The cascades' point cloud by partition and subtree (`LL_VK_PC_SPLIT`, default 1, 0 = one pool job a cascade as before; laps `pc1-pcchk1` check, `pc2-pcsplitz1` / `pc2-pcoffz1` zones, full; 2026-11-01)

The octree walk that finds the visible extents of a cascade's frustum (`getVisibleExtents`, the first half of `getVisiblePointCloud`) was one pool job a cascade, so the stage was as long as the walk of the widest cascade (cascade 3). It now runs as jobs by partition and cascade, and a partition whose walk for that cascade cost over 60 us last time is walked in pieces the way the main cull is (`traverseTop` for its top levels on the main thread, the subtrees that cost over 40 us as jobs of their own: `LLSpatialPartition::visibleExtentsWhole / Split / Task`, costs smoothed per partition and cascade). The extents are minima and maxima, so the pieces merge in any order, and the second half became `LLPipeline::pointCloudFromExtents`.

Check (`LL_VK_PC_CHECK=1`, every cascade compared with the serial walk, exact): **0 of 102,600 cascades differed**, no crash. Zones (still1 / still2, ms a frame on the main thread): the point cloud stage 0.200 / 0.211 (one job a cascade) -> **0.110 / 0.121** (`point cloud split`: the pool wait 0.07 and 0.04-0.05 for the top levels and the unit list on this thread), -0.09 ms. `generateSunShadow` 2.00 / 2.38 -> 2.03 / 2.48: within the noise of these laps (the second lap's crowd was heavier: loop 12.4 against 11.4 ms), and the cull wait after it read 0.28 / 0.41 against 0.25 / 0.36 (blocked 0.13 / 0.24 against 0.05 / 0.08), which I put down to the scene.

Kept (default on): the stage is shorter by the measured 0.09 ms, the result is exact, and it is one more walk cut the way the cull is. The same cut is what the cascade cull (the biggest wait left in the shadow prep: 0.25-0.4 ms, a job a cascade, 0.04 ms for cascade 0 and 0.3 for cascades 2 and 3) needs, but that walk fills a result (groups, drawables, ledgers), so it would use the main cull's chunk-and-ledger machinery. Not started.

### 8.30 The cascade cull by partition and subtree (`LL_VK_SC_SPLIT`, default 1, 0 = one pool job a cascade as before, 2 = the split, then the serial cull of each cascade, compared list by list and in order; laps `sc2-scchk21`, `sc4-scchk21` check, `sc3-` and `sc4-scsplitz1` / `sc3-scoffz1` zones, full; 2026-11-01)

The same cut as 8.29, for the walk that fills each cascade's result (`shadowPrepCull`: `part->cull(camera)` with the shadow culler over every partition). `shadowPrepCullSplit`: for every pending cascade, the units in the order of the serial walk (the partitions in list order; a partition whose walk for that cascade cost over 60 us last time has its top levels walked on the main thread, between the segments of its result, and its subtrees that cost over 40 us as jobs of their own: `LLSpatialPartition::cullShadowWhole / Split / Task`), each unit with a result of its own (`LLCullResult::clearLists`, the lists a cull fills only), appended to the cascade's result in that order afterwards; the hull filter (`shadowPrepHull`, split out of `shadowPrepCull`) runs after the merge. The thread-local state of a cascade's jobs (`ShadowPrepScope`: camera id, job view, the avatar cull of the slice) is installed per unit.

Check: the old parallel check (`FSParallelShadowPrepCheck`) is stale: with the split OFF it reports 57,600 of 57,600 cascades lacking render map entries (3,595 missing from render map 23 of cascade 2): the scene table serves draws that the render map no longer holds. It compared the group and drawable lists as sets and found them equal in both runs. So `LL_VK_SC_SPLIT=2`: **0 of 70,200 cascades differ** from the serial cull in the lists and their order (first version), **0 of 66,600** after the cheaper chunk clear; no crash. (The stale check wants replacing: not done.)

Result (main thread, ms a frame, zones, still1 / still2): the cull stage 0.274 / 0.429 (one job a cascade; blocked 0.12 / 0.26) -> **0.170 / 0.255** (the pool wait 0.080 / 0.105, the top levels of the split partitions and the unit list 0.061 / 0.107, the merge 0.020 / 0.031; blocked 0): **-0.10 / -0.17 ms**. The first version, with `LLCullResult::clear` on every chunk result, cost 0.24 / 0.41 (-0.03 / -0.02): the clear of the render maps and scene fields that a cull never touches was most of the setup. `generateSunShadow` 2.11 / 2.57 -> 2.05 / 2.44 (-0.06 / -0.13, within the noise of laps whose loops differ by up to 1 ms).

Kept, default on. The shadow prep's pool waits are now: point cloud 0.12, cull 0.17, mark 0.12-0.16, render map 0.09-0.12 (8.28's list), of which the top levels on this thread (0.06-0.11) are the next piece if anything is: the node-cost rule (40 us) and the level limit (8) are the dials.


### 8.31 Two login crashes of the lap harness, found and fixed (laps `rp2-` to `rp4-`, `gd1-` crash, `gd2-` to `gd6-` clean; 2026-11-01)

Laps with `H6_PREPCHECK=1` and `LL_VK_SC_SPLIT=1` crashed about 33 s after the login in 3 of 7 runs (the first at 10:55 in the minidump of the first crash, then `rp4-scchk1` and `gd1-scchk1`; `rp1-` to `rp3-` were clean). Two sites, both a pointer to something freed:

- **The moved list pass** (`updateMovedListSerial`, main thread, every other thread idle in the dump): `rp4-` stopped at `isRoot()` of entry 0 of 89 and `gd1-` at line 2698, after `updateMove()` and a `clearState` on the same drawable. The pass worked on a raw pointer taken from the list; the list's counted pointer was the last reference, and `updateMove` can unlink the drawable (`unlinkDrawable` erases its entry), which freed it under the pass (the dump of `gd1-` stopped on the freed memory, reused for text). Fix: the pass holds a counted reference for each entry. The old iterator crash (8.17) was the other half of the same loop.
- **The resident ray scene** (`res_pair_ready` in the render worker, the first crash): a pair kept a pointer to a buffer table entry that went (the table's `res_buffer_gone` drops the pointers of the pairs registered under the name, a pair that missed it kept the dead one; `res_pair_check` tested the generation, this path did not). Fix: `res_pair_buffers` tests the pair's generation first and looks the entries up again, in both places that used the cached pointers. It warns (`VkResident`, the first 8 and every 600th) when the test drops a pointer that is not what the table holds now, so a lap shows whether it ever did anything: none of the 5 clean laps logged one (so the worker crash is not reproduced; the guard is by reading, not by a trigger seen).

Probes (`LL_VK_DRAWABLE_TRACE`, default on, 0 = off): `DrawableTrace` logs the stack when a drawable is freed while the moved list holds it, and when the pass's current drawable is unlinked from inside its own `updateMove`. None fired in the 5 clean laps, so the condition that crashed `gd1-` was not seen again either; the evidence for the fix is the dump, and 5 of 5 clean laps against 3 crashes in 7 before (about 13% if the rate had not changed). Frame times of the clean laps equal the earlier ones (12.7-13.5 ms loop, GPU bound with RT on).
