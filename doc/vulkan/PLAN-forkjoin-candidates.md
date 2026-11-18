# Where else the compute/apply split and the fork-join pool pay

Written 2026-10-27 as a companion to `PLAN-statesort-offmain.md`. That plan moves the main view's stateSort to the shared `RKForkJoinPool` as a pure compute pass over chunks (workers write only item-owned fields, per-thread lists and POD delta records) followed by an apply pass on the main thread (queues, group state, rebuilds). This note asks where else in the frame the same shape fits, and ranks the places by return over work. Nothing here is built; numbers are from `MAINTHREAD-candidates.md` (club1/club2, the crowded plaza, laps of 2026-10-26/27) and are the serial main-thread time of each block today. Estimates are marked (est.).

## 1. What the pattern needs, and what it costs

A block fits when it is a loop over items where most of the work per item reads frozen world state and writes memory that only that item owns, and the writes to shared state (queues, octrees, group flags, GPU records) are few and can be recorded and replayed in order. It does not fit when the serial time *is* the shared mutation (octree moves, rebuild queue processing), when the output is an ordered GPU record (draw issue, alpha runs), or when the loop is a cache-miss walk whose result nobody needs in parallel.

The pool's rules shape the design: `shared()` has 7 workers plus the calling thread, phases never overlap, and one `run()` costs 0.05-0.1 ms of wake-up wall (the chunked render map job: 0.11 ms wall for 0.25 ms of work). So fewer, fatter fork-joins beat many small ones; a new compute pass should join an existing phase where its inputs allow, and a loop under about 0.3 ms never pays a phase of its own.

## 2. The candidates, ranked

| # | place | serial today | compute (workers) | apply (main) | return (est.) | status |
|---|---|---|---|---|---|---|
| 1 | avatar update: idle loop + deferred completion | 2.2-3.0 ms | attachment matrices, extents, face centres, tag anchor, impostor tests, per avatar | copy records; queue only the bridges that left their node | 1.3-1.8 ms | planned: R1 in `overview-brief/REPORT-fable-avatars.md` |
| 2 | stateSort of the main view (+ cull, + cascade merge) | 1.9-2.3 ms | distance, LOD, visibility per chunk | deltas, rebuilds, pools | 1.5-1.8 ms | planned: `PLAN-statesort-offmain.md` stages 1-3 |
| 3 | sun shadow cascade camera setup + point cloud | 0.5 ms | per cascade: frustum fit, octree walk | write the cascade's matrix slots | 0.2-0.3 ms | add to stateSort stage 3 |
| 4 | ray-scene naming of rigged draws | 0.5-0.6 ms | entries, palettes, matrices per avatar | merge blobs, one `shadowPackets` | 0.3-0.4 ms | self-contained |
| 5 | moved list (what remains after R1) | ~1.1 ms incl. | xform lerp, face centres, "still in node" test | partition moves, rebuild pushes | 0.3-0.5 ms | after R1 |
| 6 | the small idle loops, bundled | ~0.8 ms together | one job per loop in one fork-join | each loop's writes | 0.3-0.4 ms | last, if still main-thread bound |

### 2.1 Avatar update (R1): the same design, already planned

`LLViewerObjectList::update` (`llviewerobjectlist.cpp` 1213) runs the serial idle loop, then `run_avatar_motion_batch` (the pose blends in parallel), then the deferred completion per avatar on the main thread: `idleUpdateMisc` with the attachment loop (0.76-1.0 ms), root position and rotation, name tag, extents. R1 moves everything after the blend that reads frozen state and writes avatar-owned memory into the batch job, and leaves a POD record per avatar (root, tag anchor, flags, `attachments[] {drawable*, matrix, extents, node_left}`) for a tight apply loop. That is the stateSort delta record with an avatar as the item. Both plans want the same machinery: a per-frame record arena, a verify mode that diffs the new path against the old on the same frame, and a tripwire (thread-local "in job" flag asserted in `markMoved`, `markRebuild`, `LLSpatialPartition::move`). Whichever is built first should build those three generically.

### 2.2 Cascade camera setup: one job per cascade, inside stateSort stage 3

The pass-1 loop of `generateSunShadow` (pipeline.cpp about 23700-24108) computes each cascade's camera: `getVisiblePointCloud` (23039: a read-only octree walk through `getVisibleExtents`, then frustum/box clipping), the least-squares fit of the shadow frustum, the view and projection, and writes `mShadowModelview[j]`, `mShadowProjection[j]`, `mSunShadowMatrix[j]`, `mShadowError.mV[j]`, `mShadowFOV.mV[j]` and the `ShadowCascadePrep` entry. Per cascade the writes are its own index: item-owned. The shared writes are `set_current_modelview` / `set_current_projection` / `set_last_*` (process globals) and the shadow-map reuse decision, both of which belong in the apply.

Stage 3 of the stateSort plan already hoists this loop before the main cull so that one fork-join can cull all views. Make it one job per cascade that computes the camera and then runs that cascade's partition culls, so no extra phase is added; the wall is the longest cascade. With the cascade rotation (cascade 0 every frame, 1-3 one in four) a frame computes about two cascades, so the saving is the smaller one's setup plus the point cloud walk: 0.2-0.3 ms (est.). Needs: `getVisibleExtents` audited for writes (it reads group bounds; `rebound()` is already called for the partitions before the parallel prep, 24126).

#**Status 2026-10-27 (from the stateSort session):** stage 3 ended with 3a (the main cull on the pool). The visible-extents walk of the point cloud was put on the pool and checked bit for bit, and measured at 0.031 ms a call serial against 0.024 ms on the pool, so the walk is not where the 0.17-0.22 ms a frame of the cascade setup goes, and it was dropped. The hoist described below was not done: it moves the point cloud before the main cull's occlusion clearing (extents no longer bit-identical) for 0.2-0.4 ms (est.) in a scene that is GPU-bound. A fork-join also costs no wake-up (spin test 150 vs 1500 us: no change), so merging runs as such does not pay. Re-open only if a measurement shows the cascade setup on the critical path.

## 2.3 Ray-scene naming of rigged draws: chunk by avatar

`rt_name_rigged_main_view` (pipeline.cpp 19760) walks the rigged render maps and the scene fragments, builds `ShadowSkinEntry` / `ShadowAlphaEntry` vectors with deduplicated matrices and palettes, and sends one blob per kind through `RKVkBridge::shadowPackets`. After `rt_name_avatar_due` (19737) it is 0.5-0.6 ms (1.0-1.2 before). Per entry it reads a draw info and writes local vectors: pure compute, except

- `shadow_palette` calls `updateSkinInfoMatrixPalette`, which may compute and cache a palette on the avatar: safe if one job owns one avatar;
- `rt_name_avatar_due` writes `mRtNameChecked` / `mRtNameDue` / `mRtNameLast` on the avatar: same rule;
- the alpha path's `texture_name` calls `gRender.getTexUnit(0)->prepareFast(tex)`: a GL-era bind. Must become a lookup of the texture's name without touching render state, or be hoisted into a serial pre-pass over the distinct textures;
- `rt_textures_ready` and `shadow_alpha_packet_ok`: read-only (audit).

Shape: group the draw infos by avatar (one pass on the main thread, or chunk the render maps and partition by avatar inside the job), one job per avatar or per group of small avatars, per-job entry/matrix/palette vectors, merge on the main thread with offset fix-ups into one blob. Return 0.3-0.4 ms (est.); the pose census and the own-avatar debug counters stay on the serial path.

### 2.4 Moved list: only what R1 leaves

`updateMovedList` (pipeline.cpp 2481) calls `LLDrawable::updateMove` (lldrawable.cpp 793) per drawable: `updateXform` (623: lerp/nlerp of position, rotation, scale; `mXform.updateMatrix()`), `moveUpdatePipeline` (face centres), then the partition move or a bridge move. Inclusive about 1.1 ms at the plaza, of which the octree work (`partition move` 0.34, `group updateInGroup` 0.28) is shared mutation and stays serial. The compute is the xform arithmetic and the face centres, plus a read-only "did the new box leave its octree node" test so that the apply moves only the drawables that need it (R1 does the same for attachments).

Shared writes inside `updateXform` to record as deltas: `markRebuild` (REBUILD_POSITION / REBUILD_ALL / REBUILD_VOLUME), `markMoved(mSpatialBridge)`, `movePartition`, `setState(ANIMATED_CHILD)` + `dirtySpatialGroup`, `shrinkWrap`, `LLControlAvatar::matchVolumeTransform`, `updateText`, `makeActive` / `makeStatic`. Ordering: a child's `mXform` matrix depends on the parent's, so a chunk must hold a whole linkset (roots first, children after, inside one job).

Why after R1: today 243 of the 260 rigid entries a frame are attachments already moved by the avatar loop, and R1 takes those out of the moved list altogether. What remains is linkset children, vehicles and objects moving in view, 0.3-0.5 ms (est.) of which the compute is perhaps half. Measure the list after R1 before building.

### 2.5 The small idle loops: one bundled phase

Each of these is 0.15-0.4 ms and none pays a fork-join alone; together they are about 0.8 ms and could share one phase with one job per loop (or chunked where a loop is long), with the writes applied after:

- `updateApparentAngles` (llviewerobjectlist.cpp 944): 0.2 ms, a cache-miss walk over about 360 objects (`setPixelAreaAndAngle`, `updateTextures`); the writes are face pixel areas and texture stats (`addTextureStats`: shared per-texture accumulators, need per-thread accumulation or atomics).
- `calcNearbyLights` (pipeline.cpp 14804): 0.17 ms after the in-place rework; the distance pass over `mLights` is the compute, the `NEARBY_LIGHT` state bits and the sorted vector the apply.
- `updateImageDecodePriority` (llviewertexturelist.cpp 1563): 0.2 ms over about 235 textures a frame; the face loop is compute (the TEXREC worker already does the face sizes for the static ones), the desired discard / priority / wake-list changes the apply.
- `LLViewerTextureAnim::updateClass` (llviewertextureanim.cpp 96) and `LLVolumeImplFlexible::updateClass` (llflexibleobject.cpp 93): per-object state advances that end in `markRebuild`; the flexible update writes vertex sections (item-owned) and queues a rebuild.

Return 0.3-0.4 ms (est.) for one phase; each loop still needs its own compute/apply audit, so the work is the sum of five small splits. Do it last, and only if the main thread is still the limit after 1-4; at the plaza the frame is GPU-bound after the stateSort stages (`project-gpu-bound-after-stages`), so this may never be worth it.

## 3. Not a fit

- **Alpha pool runs** (`run_begin` 0.36 ms) and the **lighting / geometry draw issue**: order-dependent GPU recording; the cost is the number of program and blend changes, not per-item work.
- **UI** (0.9-1.0 ms): hundreds of views at 1-2 us; the lever is a retained layer (`RKUIRecordCache` is that), not threads.
- **Rebuilds** (`rebuildGeom` / `rebuildPriorityGroups` / `rebuildMesh`, 0.3-0.4 ms): mapped-buffer lists, mesh repo notification and draw-info allocation are shared; `PLAN-statesort-offmain.md` section 6 defers them to their own plan.
- **Texture fetch dispatch** (0.4 ms) and **mesh repo notifications**: queue bookkeeping, event-driven changes at best.
- **Particles** (0.25 ms): would fit (per-source motion compute, group placement and `markRebuild` as apply), but `PLAN-gpu-particles.md` removes the CPU simulation.
- **Octree balance, name tags, the still-object skip**: measured dead.

## 4. Order

1. Avatar R1 (the largest; builds the record arena, verify mode and tripwire for everything after).
2. stateSort stages 1-3, with the cascade camera setup as jobs in stage 3.
3. The rigged naming chunked by avatar.
4. The moved list, after measuring what R1 leaves.
5. The bundled idle phase, only if the main thread is still the limit.

Expected together: about 3.5-4.5 ms of main thread in the club crowd (est.), most of it from the first two. After the first two the plaza frame is GPU-bound at Adam's yaw, so 3-5 lower CPU load and headroom in worse scenes rather than frame rate there.
