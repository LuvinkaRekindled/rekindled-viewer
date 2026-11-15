# Plan: the main view's sort and map made change-driven (2026-11-15)

Why: the main thread is the limiter in the RT-off scene (6.0 ms of work in a 6.22 ms loop, lap z-cyc1-def1, cycles counted: blocked 0.08 ms), and the GPU is 84-90 % busy. `stateSort (early)` is 0.68 ms and `updateCull` 0.27 ms of it (zone laps z-off1-def1, z-cyc1-def1, London, RT off, 5-6 reports each). `PLAN-gpu-scene.md` lists them as "gone: no per-frame lists". S7-design.md section 5 (C) is the GPU route; this plan is the CPU route that needs no GPU culling, and it takes only what the readers below allow.

Frame budget limit: the GPU takes about 5.3-5.5 ms in this scene. Main thread savings beyond about 0.7-0.9 ms only make the GPU the limit (about 180 fps). The pieces below add up to about 0.25-0.3 ms; the rest of the 1.4 ms (updateCull, cascade culls) is idea 4.

## 1. What changes per frame (census lap cs-def1, LL_VK_VISCENSUS=1, 600 frames; proportions only, the census adds its own time: its updateCull reads 0.74 ms against 0.27 in the zone lap)

| measure | per frame |
|---|---|
| visible groups (main view) | 1,869; 1,868 (100.0 %) were also visible the frame before; 0.8 left |
| groups whose draw map changed since the frame before | 22.9 (the 22 avatars), new draw infos 0.0 |
| `changeLOD` true | 81 of 1,869 calls; checkOcclusion: 2,029 calls, 0 occluded |
| drawables marked visible | 485: avatars 22, bridges 218, other 245 |
| drawables sorted (`stateSort(drawable)`) | 563: avatars 22, other active 98.7, static 442 |
| non-volume drawables enqueued for the pools | 293 (faces: TERRAIN 234, AVATAR 104, CONTROL_AV 16, WATER 11, SKY 8): about 250 are one-face terrain, water and sky drawables |
| rebuilds that reach `genDrawInfo` | 0.6 (zone lap); the 22 avatar groups are the real rebuilds (census comment in `rebuildPriorityGroups`: avatar bridge groups, about 0.5 ms steady before the fold) |

Correction of what I said earlier in this session: the "390 groups a frame through `rebuildGeom`" is mostly the zone timer itself. The census times the whole drawable-group rebuild sweep at 0.005 ms; the 0.08-0.11 ms of `rebuild: partition rebuildGeom` in the zone laps is three zones per group times 390 groups. The rebuild sweep is not a target.

The group-level work is already change-gated and cheap: `changeLOD` decides from the distance ratio against the last update (81 true), the occlusion check is 0.02 ms. What costs is per drawable and per group in lists that are rebuilt every frame from an unchanged set.

## 2. Readers (every one found by grep over indra/newview, 2026-11-15)

**`mDistanceWRTCamera`** (written by `LLDrawable::updateDistance`, by the apply pass for non-volume drawables as an `OBJECT_LOD` delta):
- volumes: `LLVOVolume::calcLOD` (llvovolume.cpp 2047, 2076, 2172), alpha distance factor (5189), mesh repo priority (llmeshrepository.cpp 604, 607), spatial partition (llspatialpartition.cpp 4479), flexible objects (llflexibleobject.cpp 303, 309, 708). These run through the group gate (`changeLOD` true, 81 groups a frame).
- grass (`LLVOGrass::updateLOD`, llvograss.cpp 373) and trees (`LLVOTree::idleUpdate`, llvotree.cpp 365): need it.
- avatars: alpha pool (lldrawpoolalpha.cpp 760, 1310), impostor and far tier decisions (llvoavatar.cpp 6855, 7705, 7844, 8588, 10827, 16055, 16082), pipeline.cpp 7652 (far avatar tier). Avatars and their attachments keep a per-frame update.
- terrain patch (`LLVOSurfacePatch::updateLOD` returns true and does nothing), water, void water, sky, particle groups: NO reader. `LLViewerObject::updateLOD` is `return false`. For these the per-frame `updateDistance` and its `OBJECT_LOD` delta write a number nobody reads.

**Drawable visible stamp** (`LLDrawable::setVisible` = `LLViewerOctreeEntryData::setVisible`, one U32 for all cameras): read by `LLDrawable::isVisible` (it already falls back to `group->isVisible()`, lldrawable.cpp 1387-1397), `isRecentlyVisible` (falls back to the group's, llvieweroctree.cpp 436; callers llviewertexture.cpp 4152, 4168 and llvocache.cpp 907), `LLVOVolume::isVisible` (llvovolume.cpp 1182-1215), llspatialpartition.cpp 3343 and 5238, lldrawable.cpp 798 and 1414. The only raw `getVisible()` reads of a drawable are lldrawable.cpp 1414 (recent, with the group fallback) and 1850 (avatars). The group stamps (`row().visible[cam]`) have their own readers (texture list 1146, 1371; pipeline.cpp 2602-3344, 5151) and are not touched here.

**The visible drawable and bridge lists** (`sCull->beginVisibleList` / `beginVisibleBridge`): in a release build the consumers are the stateSort loops themselves (bridge `updateDistance` gated by `changeLOD || fov_changed`; drawable `updateDistance`, pool enqueue), `fs_blink_detect` (a diagnostic that scans every avatar in the main view's `postSort`; it returns at once unless `LL_VK_BLINK` is set, so it costs nothing by default and is a pre-release removal), render debug, and debug-only reference checks (pipeline.cpp 3739, 3771, 15920).

**The render map** (`postSort: build render map`, main view, chunks on the pool; 1,869 groups a frame): it makes `sCull->mGeoHandles` (the list the worker draws from), the alpha and rigged alpha group lists with their distance update, and the triangles-drawn statistic. Per group it reads `isDead`, the occlusion state, `mSurfaceArea` against `RenderAutoHideSurfaceAreaLimit`, the far-avatar skip set, `mGeoHandle`, `mGeoServedTris`, `mGeoAllServed`, `mGeoAlphaBits`. In this scene alpha groups are 0, and the served groups are all of them (VOLUME 1,459 of 1,505, BRIDGE 74 of 77, AVATAR 233 of 262: "all draws served").

## 3. The changes, in order

**T1. Stop the writes nobody reads.** A virtual `LLViewerObject::usesDistance()` (default true; false for surface patch, water, void water, sky, particle group). `LLDrawable::updateDistance` returns before the position math and before pushing the `OBJECT_LOD` delta when the object says false; the pool enqueue of the same drawable stays (the terrain, water and sky pools read the face lists). About 250 of the 293 non-volume drawables a frame stop producing a delta. Where it lands: the apply pass (`stateSort: apply deltas` 0.083 ms a frame, a single zone call so it carries little timer overhead) and the compute chunks. Expected 0.06-0.1 ms. Risk: low; the readers above are complete for these classes. Check mode: a frame counter of deltas applied for classes with `usesDistance()` false must be 0, and one lap with the old path run side by side comparing every `mDistanceWRTCamera` that a reader asked for (assert in a debug env var, not in the build default).

**T2. A persistent visible handle list for the main view.** The cull result is still made every frame (`updateCull` stays; that is idea 4). The render map stops being rebuilt from 1,869 groups: the previous frame's list is kept and compared to the new visible group list by pointer (one pass over 1,869 pointers, no group is touched for the groups that did not change). A group's contribution to the list (`mGeoHandle`, served tris, alpha bits, far skip, auto-hide, occluded, dead) is recomputed only when the group enters the list, or its epoch changed. The epoch is a U32 on the group bumped at the sites that write those inputs (fragment build sets `mGeoHandle`/`mGeoServed*`, the occlusion state set in `checkOcclusion`/`markOccluder`, the far tier set, the group's death); the far tier and the auto-hide limit are compared as part of the frame's key (the limit setting and the far skip set's version). Expected: the render map chunks, join and wait (0.15 ms in the zone lap) shrink to the compare plus about 24 recomputed groups. Risk: medium. A missed epoch bump is a stale group in the list (a draw missing or extra). Check mode (`LL_VK_SORT_CHANGE=2`): build both lists, compare handle sets and the triangle statistic every frame, log differences with the group's partition and state bits; the gate is 0 differences over a London lap and a crowd lap.

**T3. Per-drawable stamps from the group (optional, measure first).** `markVisible` and `stateSort(drawable)` set the drawable stamp for the 485 and 563 drawables. `isVisible()` and `isRecentlyVisible()` already fall back to the group's stamp; the group is stamped in the same loop. Drop the per-drawable stamp for drawables that have a group, keep it for avatars (lldrawable.cpp 1850 reads it) and for drawables without a group. Expected at most 0.03-0.05 ms (a store of a U32 and the iteration); do it only if a lap without zone timers shows it. Risk: low, but the gain is below what a lap can measure, so it ships only as part of T1's commit if at all.

**Not changed:** `checkOcclusion` and `changeLOD` loops (0.02 ms each, already gated); the bridge `updateDistance` for attachments (real work: 22 avatars move, their groups are dirty; the far tier already budgets it); `rebuildGeom`, `rebuildPriorityGroups`, `rebuildMesh` (real work: the 22 avatar groups; the rest is timer overhead); pool face enqueue for terrain (the pools read it; `refill pools` is 0.01 ms); the cascade culls; `updateCull`.

## 4. Gates, in the runbook's terms

Each change is behind `LL_VK_SORT_CHANGE` (bit 1 T1, bit 2 T2, bit 4 T3; default off until the gate). Gate per change: build 0 error C and 0 error LNK; native-check-all unchanged; check mode difference count 0 in London (RT off) and in the crowd; then the three-condition alternating laps without zone timers (off, T1, T1+T2), two laps each, reading fps, GPU utilisation (util.py) and the still window's `GPU time` lines; the picture compared with a crop of the same frame. The measurement rule from the texture work applies: zone timers inflate small zones (the 390-call rebuild zone), so the gain is judged from laps without them, and from `stateSort (early)` and `postSort` as single zones with `FS_CPU_ZONES_CPU=1`.

## 5. Honest size

T1 + T2 + T3 together about 0.25-0.3 ms of a 6.2 ms main-thread loop, 4-5 %. It is structure (no per-frame restating of an unchanged set, no writes nobody reads) more than speed, and it is the part of "stateSort and postSort should not exist" that the CPU can retire without GPU visibility. What is left of them after this is the real work (22 avatars, about 99 moving objects, the 81 LOD decisions, the avatar rebuilds), plus `updateCull` itself, which only idea 4 removes.
