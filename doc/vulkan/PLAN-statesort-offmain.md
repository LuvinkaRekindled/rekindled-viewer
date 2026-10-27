# Plan: stateSort off the main thread

Written 2026-10-27 from the code at 1b18976d and the measurements in `MAINTHREAD-candidates.md` and `S7-design.md` 13.2. Nothing here is built or measured yet; every number marked (est.) is an estimate.

## 1. What is being moved

`LLPipeline::stateSort(camera, result, defer_pools)` (pipeline.cpp 4071) for the main view, called once a frame as `display: stateSort (early)` right after `updateCull`, before the sun shadows. Club (lap mm1-), of a 12.2 ms main thread:

| piece | ms | what it does |
|---|---|---|
| drawable groups loop (4107) | 0.50 | `checkOcclusion`, `setVisible`, `markVisible` of every drawable of the render-by-drawable partitions (883 drawables of 276 groups: avatars, attachment bridges, terrain, water, sky, particles), `rebuildMesh` |
| visible bridges loop (4157) | 0.22 | 547 bridges: `changeLOD`, `LLSpatialBridge::updateDistance` (camera transform, the root drawable and its children: distance, `LLVOVolume::updateLOD`) |
| visible groups loop (4195) | 0.48 | 2,218 groups: `checkOcclusion`, `setVisible`, `changeLOD` (87 true a frame) and for those the per-drawable sort, `rebuildMesh` |
| visible drawables loop (4230) | 0.24 | 610 drawables: selection filter, avatar filters, `setVisible`, `updateDistance` for static drawables and avatars (`LLVOAvatar::updateLOD`), the pool enqueue for 283 non-volume drawables (deferred already) |
| `postSort` | 0.70 | `rebuildGeom` of drawable groups (~0), `rebuildPriorityGroups` 0.28, far avatar tier 0.08, render map 0.15 (chunked on the pool since 7c17829f), alpha sort 0.02 |
| **total** | **1.9-2.3** | plus `updateCull` 0.85 right before it, same structure |

The visible set is 99.8 % the same from frame to frame (VisCensus churn counters), 0 of the 2,218 groups are OCCLUDED by the time the sort sees them, and only 87 groups a frame pass `changeLOD`. The loops are re-stamping and re-deriving, not deciding.

## 2. What pins each piece to the main thread

The cascade preparation already runs the same loops on the fork-join pool: `shadowPrepMark` (5865) is the two group loops without `rebuildMesh` and without the world-camera block, with `sCull`, `sCurCameraID` and `sShadowCascadeCull` made per-thread by `ShadowPrepScope`. What the main view adds, and where it writes:

| write | where | shared with | fix |
|---|---|---|---|
| `markRebuild(drawable)` → `mBuildQ1` + `IN_REBUILD_Q`; `markRebuild(group)` → `mGroupQ1` (`llassert_always(!mGroupQ1Locked)`) | `LLVOVolume::updateLOD` 1946, `calcDistance` 713 | every queue reader | **delta record**, applied on the main thread |
| `markPartitionMove` → `mPartitionQ` | `LLVOVolume::updateLOD` 1955 | `processPartitionQ` | delta record |
| `setState(ALPHA_DIRTY)`, `mViewAngle`, `mLastUpdateViewAngle` | `LLSpatialPartition::calcDistance` 696-713, from `markNotCulled` → `group->updateDistance` (world camera only) | group `mState` is a plain U32 | delta record (group kind) |
| `clearOcclusionState(OCCLUDED, STATE_MODE_DIFF)`: a subtree walk | `checkOcclusion` 1383 (only when `sUseOcclusion >= 2`: the cascades run with 0) | the octree subtree's `mState` | one job per partition; a subtree belongs to one job |
| `mLOD`, `mLODDistance`, `mLODRadius`, `mAppAngle`, `mLODChanged` | `calcLOD` | the volume itself | written by the worker (one drawable, one chunk) |
| `mDistanceWRTCamera`, face `mDistance` for alpha faces | `LLDrawable::updateDistance` 933, 953, 967 | rigged children read `avatar->mDrawable->mDistanceWRTCamera` (1781) | written by the worker; today the attachments read the avatar's value **from the frame before** (the bridges loop runs before the avatars' `updateDistance` in the drawables loop), so the split keeps that order: avatars' distance is written in the apply pass |
| `LLVOAvatar::updateLOD` → `updateJointLODs` (mesh LOD over the joint tree, `sNumLODChangesThisFrame`), `updateMeshData` (vertex buffers), `updateVisibility` | `LLDrawable::updateDistance` 956 for avatars | avatar, GL buffers | apply pass, flagged per avatar (40-90 a frame) |
| `rebuildMesh`, `rebuildGeom`, `rebuildPriorityGroups` | the loops, `postSort` | `LLVertexBuffer` statics (`sMappedBuffers`), the mesh repo, `LLBuilderGuard::beforeMutation` (quiesces the async builders), scene fragment builds | main thread, as today (section 6) |
| pool face lists | `stateSort(drawable)` 4474 | the pools | already deferred (`mDeferredPoolFill`, `refillPools`) |
| drawable stamp `mEntry->mVisible` (one U32 for all cameras), group `mVisible[cam]` | `setVisible` | the cascade workers already write the drawable stamp concurrently, same value: a benign race in practice, UB on paper | make the stamp a relaxed atomic store; `mVisible[cam]` is per camera already |
| `wakeRequest` → `mWakeQ.push_back`, `FS_WAKING` | `markNotCulled` 3449 | **today's cascade cull jobs already push here from several threads** when a Released group is walked: an existing race, rare (Released groups only) | per-thread list, merged after the join (fix it in stage 1 regardless) |
| `markOccluder` → `ACTIVE_OCCLUSION` on the group and its parent | `earlyFail`, `markNotCulled` | `mState` of groups of one partition | one job per partition |
| census and debug: VisCensus counters, `LLOccCensus`, `setDebugText` in `calcLOD` (DebugObjectLODs, LOD_INFO debug mask), `LL_WARNS` | everywhere | statics | the serial path when any is on (the pattern `postSort` uses for the chunked map) |
| `LLSelectMgr::getInstance()->mHideSelectedObjects` | `stateSort(drawable)` 4388 | read only; it is also what disables the parallel prep today (22126) | read once a frame into a bool; selection cannot change during `run()` |

Readers that must keep working on the main thread, unchanged: everything downstream of the cull result (shadow prep via `LLBuilderGuard`, impostors, `refillPools`, `renderGeomDeferred`, the alpha pass, picking, name tags). They read the result after the join, so they see the same lists in the same order (chunk order is list order, as in the cascade marking).

## 3. The shape: a pure pass on the pool, an apply pass on the main thread

The boundary: workers **read** the octree, groups, drawables, objects, cameras and settings; they **write** only fields owned by the item they hold (LOD fields, distances, stamps), per-thread cull lists, and a per-chunk vector of delta records. The main thread **owns** the queues, group state bits, avatar LOD, rebuilds and pools, and applies the deltas in chunk order.

```
struct SortDelta {            // ~24 B, POD, one per item that needs the main thread
    LLDrawable*     mDrawable;  // or LLSpatialGroup* for group kinds
    F32             mDistance;  // the avatar's mDistanceWRTCamera, written in apply
    U8              mKind;      // REBUILD_VOLUME | PARTITION_MOVE | GROUP_ALPHA_DIRTY | AVATAR_LOD | WAKE
};
```

Expected a frame (est. from the census): 87 groups with `changeLOD`, well under 100 drawables with a LOD change or bin radius change, 40-90 avatars, 0-2 wakes. The apply pass is a few hundred records: about 0.1 ms (est.).

One fork-join, chunks over the drawable groups and the visible groups (the lists `updateCull` made): a chunk that marks a bridge visible also computes its distance and LODs right there (today a separate loop over `beginVisibleBridge`), and a chunk whose group passes `changeLOD` computes its drawables right there (today the drawables loop). Order inside a chunk is today's order; chunks are joined in order. So the two dependent loops collapse into the group loops and no second fork-join is needed.

## 4. Stages

Each stage is behind a switch with the old path intact, and each has a gate in the zones that exist (`FS_CPU_ZONES`, the VisCensus laps) plus the picture check (`LL_VK_SCENE_GEO_VERIFY` 0 mismatches, screenshot, logs clean).

**Stage 1: the split, still serial (the de-risking step; about two days).**
- `LLVOVolume::computeLOD(camera) const -> {distance, detail, flags}` and `applyLOD(const SortDelta&)`; `LLDrawable::computeDistance(camera, deltas)` writes its own fields and appends deltas instead of calling `markRebuild` / `markPartitionMove`; `LLSpatialGroup::updateDistance` likewise for `ALPHA_DIRTY`; `wakeRequest` goes through a per-thread list.
- `stateSort` becomes `sortCompute(chunk_begin, chunk_end, local_result, deltas)` + `sortApply(deltas)` + the rebuild calls, run serially over one chunk. The bridges and drawables loops fold into the group loops as in section 3.
- Check mode `FSSortSplitCheck`: run the old path into a reference result, the new path into the real one, diff the four lists, the LOD of every drawable touched and the queues' contents (the `FSParallelShadowPrepCheck` pattern, 22724).
- Gate: 0 differences over a lap at the club and the plaza; `stateSort` zone within 0.05 ms of before (no gain expected, no loss allowed). Also fixes the `mWakeQ` race in the cascade jobs.

**Stage 2: the compute pass in chunks on the pool (about a day once stage 1 holds).**
- Chunks of 128 groups per kind (the `LL_VK_PREP_CHUNK` size) over the main view's two group lists, `pool->run`, the main thread working inside; join the local lists in chunk order, apply the deltas, then `rebuildMesh` for the flagged groups, then `postSort` as today.
- The stamp becomes a relaxed atomic store; the serial path stays for the census, the debug LOD text and the GPU zone timers.
- Gate: `stateSort (early)` 1.9 → about 1.0-1.2 ms (est.: 1.44 ms of loop work over 7 workers + the main thread is about 0.25 ms of wall, 0.1 join, 0.1 apply, `postSort` 0.7 unchanged). Watch the fork-join wall in a zone (`stateSort: compute (wait for the pool)`); if a chunk kind dominates, split it.

**Stage 3: one preparation for all views (the main view joins the cascade fork-joins; several days).**
- Today: `updateCull` (0.85, serial) → `stateSort` → `generateSunShadow` { cascade cull jobs (main waits 0.43) → mark jobs → rebuilds → render map jobs (waits 0.38) → the renders }. The two preparations do the same work and never overlap.
- Proposed: hoist the cascade camera setup (the pass-1 loop at 22560-22690: split distances, `getVisiblePointCloud`, which walks the octree and does not need the main cull) to just before the main cull; then `prepareViews()`: fork-join A = the main view's partition culls as jobs (one per partition; VOLUME 0.63 ms is the long pole, split by region or by the root's children if it stays long) together with the cascade culls; fork-join B = the compute chunks of all views (stage 2's for the main view, `shadowPrepMark` for the cascades); main thread: apply, then one rebuild pass for all views (`shadowPrepRebuild` already does the cascades); fork-join C = the render maps of all views (both exist).
- What it needs: `sUseOcclusion` and `sShadowRender` per job (the cascades run with 0 / true, the main view with 2 / false: today they are statics flipped around the whole shadow block; the job scope sets and restores them thread-locally, and the renderers keep reading the main thread's value); `LLHiz::apply` before fork-join A (it is, at the top of the main cull); the C2 `useVerdict` path stays serial (it is off).
- Side effect worth having: the rebuilds of every view happen before any render map is built, so the shadow rebuilds can no longer invalidate the main view's map and the early-sort re-sort path (`gEarlySortResorts`, llviewerdisplay.cpp 1098-1113) can go.
- Gate: `updateCull` 0.85 + `stateSort` 1.9 + the two prep waits 0.8 = about 3.5 ms → about 1.8-2.0 ms (est.); the ShadowPrep log's phases per frame; pictures equal in all three modes (serial, stage 2, stage 3).

**Stage 4 (only if stage 3 leaves the main thread waiting on the compute wall): shrink the compute itself.** The cull knows which groups entered the visible set this frame (`getVisible(cam) < frame - 1`, 3471). Groups that stayed visible with `changeLOD` false need no per-drawable work at all except the lists; keeping the lists across frames (a persistent visible set with swap-remove on leave) would make the compute O(changes). It changes the stamp semantics (`isRecentlyVisible`, texture decay) and the list order, so it is a separate design, not a stage of this one.

## 5. Honest expectation

Stages 1-3 together: about 1.5-1.8 ms of the main thread at the club (est.), from the 3.5 ms of `updateCull` + `stateSort` + prep waits down to about 1.8-2.0 ms. What stays on the main thread after them: the apply pass (0.1), the lists join (0.1), the rebuilds (`rebuildPriorityGroups` 0.28-0.39, `rebuildMesh`, `rebuildGeom`: section 6), the far tier (0.08), the alpha sort, `refillPools`, and the compute wall when the pool is slow to wake (the chunked map job showed 0.11 ms of wall for about 0.25 ms of work: the wake-up is a fixed cost, which is why stage 3 merges the fork-joins instead of adding one).

## 6. Not in this plan: the rebuilds

`rebuildGeom` / `rebuildPriorityGroups` / `rebuildMesh` (about 0.3-0.4 ms) pack face vertices into mapped vertex buffers and build draw infos: CPU work, but through `LLVertexBuffer`'s static mapped-buffer list and counters, the mesh repo (`notifyLoadedMeshes`), texture priority stats, the rebuild queues and `LLBuilderGuard`. Chunking them needs a mapped-buffer list per chunk merged on the main thread and the uploads queued there, the mesh repo notification hoisted out, and the draw-info allocation made per-thread. A plan of its own, after stage 3; `MAINTHREAD-candidates.md` (the mm1- section) lists the same blockers.

## 7. Risks

- **Reads of avatar state from workers** (`isImpostor`, `getOverallAppearance`, `needsImpostorUpdate`, `isVisible`, `isFullyLoaded`, `getLastAnimExtents`): safe only because nothing else runs during `run()`; the main thread must not touch avatars between fork and join (it does not today: it works inside the pool).
- **`calcLOD` is not pure today**: the debug text, `gPipeline.hasRenderDebugMask`, `LLSculptIDSize::instance().isUnloaded` (an `std::set` lookup; read only), `isSelected`. Stage 1's compute function must be `const` and audited line by line; the debug branches fall back to the serial path.
- **Rigged attachments read the avatar's distance from the frame before** (section 2). Keeping that is the conservative choice; fixing it (compute avatars first) changes LOD timing by a frame and belongs to a later step, measured separately.
- **Stage 3's thread-local `sUseOcclusion` / `sShadowRender`** touches statics read in many render paths; the change is mechanical but wide. If it proves too wide, stage 3 can keep the main view's cull serial and merge only fork-joins B and C (losing the 0.85 ms overlap, keeping the wait merge).
- **The pool is shared and never overlaps**: the chunked render map, the avatar motion batch and the shadow prep already use it in sequence. Stage 3 adds no new overlap, only fewer, larger fork-joins.

## Status 2026-10-27: stage 1 built (serial split with a check), off by default
`LL_VK_SORT_SPLIT=0|1|2` (test file `<work>/sort-split.txt` under `LL_VK_FARTIER_FILE`; mode 0 is the old loops). Built differently from section 3 in one way: instead of folding the bridge and drawable loops into the group loops and writing a pure `computeLOD`, the effects that touch shared state are **intercepted where they are made** and recorded in an ordered ledger (`LLPipeline::SortDelta`, `sSortSink`): `markRebuild(drawable)`, `markRebuild(group)`, `markPartitionMove`, the `updateLOD()` call (and the distance write) of every drawable that is not a volume (`LLDrawable::updateDistance`: avatars, grass, terrain patches) and the `rebuildMesh` calls of the group loops. The loops themselves (now `LLPipeline::sortLoops(camera, read)`, reading the cull result they are given) run unchanged; `applySortDeltas` makes the recorded calls in the order they were recorded, which is the order the loops made them, so rebuildMesh of an early group still happens before the LOD effects of the later loops, as before. Volumes update their own LOD fields in place (one volume, one chunk). Debug LOD text, the census, `DebugObjectLODs` and non-main views keep the old path.

**The check (`=2`, early sort only):** the loops run deferred into a scratch copy of the cull lists (nothing applied, the fields they write put back from a journal: the volumes' LOD fields, the groups' `mLastUpdateDistance`), then as before with every call teed into a second ledger; the two ledgers are compared entry by entry (kind, flag, drawable, group, distance). First version mismatched every sort (the deferred run read the real lists that its own first loop had not filled yet: fixed by giving it a copy). **Result: 11,400 sorts in the quiet region, 0 different lists of effects, no warnings.** Per sort in the crowd (about 70 avatars): 2,630 effects, of them about 2,000 rebuildMesh calls that find the group clean, about 320 object LOD updates, 1 drawable rebuild, 4 partition moves.

**Cost (crowd, 70 avatars, same lap, own timers around the loops, `ss2-`):** split off 1.24-1.30 ms per early sort; split on 1.27-1.32 ms (compute 1.14-1.17 + apply 0.14-0.15). Gate of the plan (within 0.05 ms of before) met: +0.03 ms. The compute part (the part stage 2 puts on the pool) is 1.14 ms; the apply pass is 0.14 ms, mostly the 2,000 clean rebuildMesh calls, which stage 2 can filter by `MESH_DIRTY` at record time (kept unfiltered in stage 1 so that the order of every call stays what it was).

**Not done yet (stage 2 needs them):** the volumes' in-place fields and `setVisible` stamps are written by the loops (one writer per item once chunked: the stamp has to become a relaxed atomic store); the bridge loop's `mLastUpdateDistance` and `markDead` writes on groups and bridges; `wakeRequest` from the cull jobs (existing race); grass `updateLOD` (now deferred as an OBJECT_LOD entry, its writes happen in the apply pass: fine).
