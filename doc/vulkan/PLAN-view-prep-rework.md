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

