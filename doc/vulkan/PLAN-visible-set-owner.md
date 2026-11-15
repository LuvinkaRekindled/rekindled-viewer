# Plan: the worker owns the visible set (idea 4, researched 2026-11-15)

Goal (Adam): get rid of `stateSort` and the render map. Constraint (Adam, 2026-11-15): only ideas that keep with the tables, the events and the worker structure we have; researched so that no stage turns out to be impossible halfway. Method: every statement below is from reading the code (file and line given), not from a census. Sections 1-3 are what exists and what the sort produces, section 4 the design, section 5 the stages, section 6 what could still block and what answers it.

## 1. What exists (read in the code)

**The group table.** `LLCullShadow` (llcullshadow.h; pipeline.cpp 13063-13300): one `CullRow` per spatial group (llvkbridge.h 356: centre, half extents, parent, flags, extents, `mPad0` = the partition's drawable type, `mPad1` free; 64 bytes). A row is made by `cull_fill_row` (pipeline.cpp ~12440) from the group. The groups call `LLCullShadow::touch(group)` at every event that changes a row (llvieweroctree.cpp 554, 573, 610, 631, 649, 715, 773, 1283, 1311, 1371, 1400: bounds, parent, occlusion state, death); `cull_process` remakes the touched rows; `LLCullShadow::send` sends delta rows (kind 35); the worker keeps its own copy (`CullShadowState::mTable`, llvkworker.cpp 11399, a host vector on the worker thread). Flags today: 1 root, 2 skip frustum, 4 occluded, 8 sphere test, 16 empty, 32 dead.

**The verdict.** A compute pass tests every row against the camera planes and writes four bit sets per frame (llvkworker.cpp 11312-11390): variant 0 is the exact frustum (margin 0), variants 1-3 add a margin that grows with the distance of the row (`margin = P.v[8].x + dist * P.v[8].y`, times the variant factor). The sets go to a ring in host memory and are published when the frame is finished (`cull_poll`); the viewer reads them with `cullRead`. Occlusion is not in the verdict: the CPU's `OCCLUDED` state stays on top (row flag 4, the Hi-Z verdict applied by `checkOcclusion`). C2 (`LL_VK_CULL_USE`, default off) already builds the main view's visible group lists from variant bits; verified in laps, matched the walk within the margin and the stock far-limit quirk (S7-design.md 18.4).

**Coverage.** `cull_covered` (pipeline.cpp ~12420) leaves out the HUD, BRIDGE, AVATAR, CONTROL_AV, HUD_PARTICLE and VO_CACHE partitions. The VisCensus partition lines (London, RT off) show where the table draws come from: VOLUME 1,459 of 1,505 visible groups have a fragment, BRIDGE 74 of 77, AVATAR 233 of 262, CONTROL_AV 2 of 2, PARTICLE 0. So the table covers about 83 % of the groups that have handles; the other 17 % (the avatars and their attachments) are the partitions the walk still owns.

**The scene table and the record.** Fragments are `GeoFrag` on the worker, found by handle (`geo_find`). Each frame the viewer sends one record (kind 21, `geo_prepare`, llvkworker.cpp 9109): the handles of the listed fragments, the moving-slot matrices, the rigged bindings (item to palette id, joints, simplified list; persistent since P2, only changes are sent when `h.mFragment == 1`). The worker then loops over the listed fragments (rebuilds the commands of a dirty one, counts items by pass type, sets the rigged commands from the bindings) and draws.

## 2. What the sort and the render map produce, and who reads it (all grep-verified)

Per visible group, `sortLoops` + `buildRenderLists` make:
1. **The visible stamps** (`setVisible`, `markVisible`): read by `isVisible` / `isRecentlyVisible` (texture priority, `LLVOVolume::isVisible`, picking, avatar code). `isVisible` already falls back to the group's stamp; stamps can be derived from a set (C4.5 R2-a built that: `row.retainedSlot`, `LLRetainedStamps::sFrame[slot]`).
2. **The LOD decision** (`changeLOD()`, then `stateSort(drawable)` for the group's drawables): `changeLOD` is a function of `row().distance`, `lastUpdateDistance`, `radius`, the partition's `mSlopRatio` and the dirty bits (llspatialpartition.cpp 935); `distance` is `calcDistance` (llspatialpartition.cpp 834): the distance from the camera to `objectBounds[0]` (not the octree bounds), with a squared ramp below 16 m; for a group with an alpha key it also computes `depth` and, when the view angle moved by 0.64, sets `ALPHA_DIRTY` and queues a rebuild.
3. **The handle list** `mGeoHandles` (and the statistic): the group's `mGeoHandle` for every visible group that is not dead, not occluded, not over the auto-hide surface area limit, not in the far-avatar skip set. Readers: `geo_prepare` (blob, bump images, moving matrices, bindings, the rigged LOD choice), the palette kick (`ll_scene_geo_palette_kick`, which makes the palettes of the rigged items of the listed fragments on the pool), the rigged source lists per type (pipeline.cpp ~10690), the type-available test (`ll_scene_geo_type_available`, ~13499), the RT naming of rigged fragments (~23814), the statistics, and `mGeoElided` (lldrawpool.cpp 1449).
4. **The alpha group lists** with their distance update and order: read by 5 sites of the alpha pool (lldrawpoolalpha.cpp: `hasStaticAlpha`, the table range build, the transient draw, the two blend loops) and by `geo_prepare` (the moving matrices of the alpha groups) and the RT naming.

Not group work, already moved in stages 1-3: the rebuilds (geometry stage), the pool fill (`fillPools`), the beacons.

## 3. What each consumer really needs (the answers that decide the design)

| consumer | needs this frame's exact set? | needs | replaced by |
|---|---|---|---|
| the worker's draw list | yes, exact for the view | the rows, the view's planes, the filters | the worker derives it (section 4, D3) |
| bump image refresh (`refreshBumpImage` -> `addTextureStats`) | no: it is a texture priority signal | a superset a frame late | the late verdict list |
| moving matrices of listed fragments | a superset is enough (a matrix for a fragment the worker does not draw is unused) | the fragments' `mGeoMoving` slots | the late verdict list |
| bindings and palettes of rigged items | a superset covering the exact set (an item draws only with a palette of this frame) | avatar, skin, item per fragment | the late verdict list with a margin that covers the camera and the object motion |
| rigged LOD choice, rig source lists, type-available test, RT naming | a superset | the fragments | the late verdict list |
| alpha pass groups | a superset (extras are clipped on the GPU; the order of the visible ones is unchanged) | alpha groups, back to front | the late verdict list restricted to groups with an alpha key, sorted by `depth` |
| LOD, stamps | the camera moved: the decision needs this frame's distance, not the exact visibility | the row's object bounds, `lastUpdateDistance`, the slop | the worker computes the changeLOD bit per row (D6) |

Nothing in the table needs the CPU to know the exact visible set of the current frame. That is what makes the design possible: the exact set is only needed by the worker's draw, and the worker has the rows and the planes.

## 4. The design

**D1. Map rows (a second table, the same events).** A parallel array `GroupMapRow` next to `CullRow`, kept by the same `touch` events and sent in the same delta record (a second section of the blob). Fields: `mHandle` (the fragment handle, 0 = none), `mServedTris`, `mFlags` (all-served, alpha bits 1 and 2, has alpha key, has rigged alpha key, auto-hide, far-avatar skip, render-by-group, suppressed by the avatar: impostor / jelly-doll / hidden), `mObjBox` (centre and half of `objectBounds`, 6 floats), `mLastUpdateDistance`, `mSlop`, `mRadius`. About 56 bytes a row. Events that write it, all existing: fragment ensure and release (`scene_fragment_ensure`, the release path: handle, served bits); a group rebuild (tris, alpha bits, surface area: the auto-hide flag); the far-avatar tier decision (flag, when the decision changes); `updateDistance` / rebound (object bounds); the avatar owner (suppression flag, when an avatar's impostor or visibility state changes); `LLSpatialGroup::changeLOD`'s write-back of `lastUpdateDistance`.

**D2. Coverage of the walked partitions.** The BRIDGE, AVATAR and CONTROL_AV groups get rows by dropping them from `cull_covered`. The hooks already fire for them (the bounds events are the same code); what was refused was the cost of walking them, not the data. The data is small: about 340 groups, one row each, written only when an avatar moves or animates (the 22 changed draw maps a frame). Rigged attachment bridges already hold the avatar's box (C4b: `mConservative`), so their row bounds are the avatar box plus margin. The HUD and VO cache partitions stay out; the HUD view keeps its few groups on the CPU list.

**D3. The worker derives the listed set.** In `geo_prepare` the worker loops over its rows instead of reading the CPU's `handles`: a row is listed when it has a handle, is not dead, not occluded (flag 4 with the parent chain, as the shader does), not auto-hidden, not far-skipped, not suppressed, and its octree box passes the exact frustum of the view (new: the kind 21 record gains the plane block that `CullParams` has today in kind 35: 7 plane vectors, the origin, the plane count). The cost is about 4,000 rows times 6 planes on the worker thread (tens of microseconds). The worker keeps its per-fragment loop unchanged below that point. The same function serves every view whose record carries planes: the main view, the HUD view's own rows aside, probes, impostors, snapshots, the serial shadow path.

**D4. The CPU consumers read the late verdict.** Variant 2 (or 1) bits plus a map `slot -> LLSceneFragment*` kept by the hooks (an array, no group pointer chase). The consumers of section 3 iterate that list: bump refresh, moving matrices, bindings and palettes, rigged lists, type-available, RT naming. The margin must cover the camera's motion over the latency (already the variant factors) and an object's motion (new: a per-row pad for moving rows, bounded by speed times latency). A fragment that is new appears in the verdict one to three frames after its row (accepted: the same as a texture that arrives).

**D5. Alpha.** The alpha pass takes the groups of the late verdict whose row has an alpha key, calls the alpha branch of `calcDistance` for them (a few dozen groups: the `ALPHA_DIRTY` side effect stays where it is, in the pass) and sorts by `depth`. Delete `pushAlphaGroup`, the alpha part of the render map and the alpha sort in `buildRenderLists`.

**D6. LOD and stamps.** The worker adds a fifth bit set to the verdict: `lodChanged` per row, computed from `mObjBox`, the camera origin, `mLastUpdateDistance`, `mSlop`, `mRadius` and the row's dirty flags with the exact `calcDistance` and `changeLOD` formulas. The CPU takes only the set bits (about 81 a frame), runs the existing group and drawable LOD update, and writes `lastUpdateDistance` back through a row event. Stamps are derived from the verdict by slot (the R2-a mechanism, `getVisible(cam)` takes the maximum with the set's frame). `checkOcclusion` stays an event at the Hi-Z result.

**D7. Palette demand by avatar, not by fragment.** The palette kick now asks the listed fragments for their rigged items. The avatar owner already knows which avatars are visible (their rows are in the verdict): the kick takes the visible avatars' rigged items from the avatars' own item lists. For a probe or impostor view the avatars it sees are the avatars whose rows pass its planes (the worker's derived set, published back as a handle list on request), the same set, one view at a time.

**D8. Deletion.** `sortLoops`, `buildRenderLists` (the handle list, the alpha lists, the statistic, the far tier moves to D1's flag event), `stateSort`, `mGeoHandles`, `mGeoElided`, the check modes.

## 5. Stages (each leaves a working viewer; the old path stays behind its switch until the stage's gate and soak)

1. **Map rows, verify only.** D1: the second table and the hooks on the viewer side; a verify mode (`LL_VK_CULL_VERIFY`, the existing full walk) rebuilds every row from the groups and compares per field. The delta rows to the worker come with stage 3, where something reads them. Gate: 0 differences over London and a crowd; main thread cost of the rows below 0.05 ms. Nothing reads them yet. **DONE 2026-11-15** (viewer side): the three hook sites (the end of `scene_fragment_ensure`, `clearDrawMap`, the end of `LLSpatialGroup::rebuildGeom`; the destructor is covered by the slot free) hold the rows; lap mr1 with `LL_VK_CULL_SHADOW=1;LL_VK_CULL_VERIFY=1`: about 5,410 rows with a handle a walk, 10 walks a report, 0 rows differ in any of the 7 fields (handle, served tris, draw tris, alpha bits, flags, surface area, object box size). Not measured: the main thread cost of the rows (the lap ran the whole table and its comparison), a crowd lap. The group table is still behind `LL_VK_CULL_SHADOW=1`; it becomes the default when stage 3 passes.
2. **Coverage.** D2: rows for BRIDGE, AVATAR, CONTROL_AV with the same verify. Gate: 0 differences; the delta volume per frame measured.
3. **The worker's derived list, compare only.** D3: the worker derives the listed set and compares it with `handles` of the record every frame; reports missing and extra per partition type and per filter. Gate: 0 missing and 0 extra (extras are expected only from the bounds rounding of the exact test: bounded, logged) over the main view, the HUD view, probes, impostors, shadow serial, in London and a crowd. This is the stage that proves the whole idea before anything depends on it.
4. **The CPU consumers on the late verdict, compare only.** D4: every consumer of section 3 runs on the verdict list in compare mode: every fragment of the exact list must be in the verdict list (missed = 0, the C1 gate), the extras are counted (the cost of the margin). Gate: missed 0 with the motion pad; extras within the margins S7 measured.
5. **Switch the worker.** The record stops carrying `handles`; the worker uses its derived list; the CPU consumers use the verdict list. The render map's handle part, `mGeoHandles` and the palette kick's fragment walk are deleted. Gate: pictures equal, no device lost over three laps, the compare modes of stages 3 and 4 kept as verify for a soak.
6. **Alpha.** D5, delete the alpha lists and their sort. Gate: the alpha pass draws the same groups in the same order (compare, 0 differences), hair, glass, particles and rigged alpha looked at by Adam.
7. **LOD and stamps.** D6, delete `sortLoops`. Gate: the LOD decisions equal the old ones (compare per group, 0 differences) and the stamps equal.
8. **Delete `stateSort`, `buildRenderLists` and the scaffolding.**

## 6. What could still block, and the answer

| risk | answer found in the code |
|---|---|
| The avatars and attachments are not in the table | D2: rows by the same hooks; their bounds are already conservative boxes; the volume is about 340 rows |
| The CPU needs the current visible set for bindings and palettes | section 3: a superset is enough; the margin variants exist; an item with no palette draws nothing for a frame, as today for a new one |
| Moving objects can outrun a one-frame-late verdict | the row's motion pad; stage 4 counts misses; if it misses, widen the pad: no structural change |
| The worker's per-frame cost | the exact test over about 4,000 rows is tens of microseconds; the per-fragment loop already exists and is unchanged; stage 3 measures it |
| Secondary views (probes, impostors, snapshots, shadow serial, HUD) | D3 serves any view whose record carries planes; the cascades keep their own render map jobs (a separate stage, S-retire-views); the HUD keeps its short list; D7 gives each view the palettes of the avatars it sees |
| `calcDistance`'s alpha side effects (`ALPHA_DIRTY`) | they move with the alpha groups into the alpha pass (D5), run for a few dozen groups |
| The `lodChanged` formulas must equal the CPU's | stage 7's compare per group, with the dirty flags as row flags; the old function stays until the compare is 0 |
| A new fragment appears one to three frames late | accepted; the same as a texture; it is the cost of not walking |
| Hi-Z occlusion stays the CPU's state | unchanged: the occluded bit is already a row flag, kept by the existing hook; the worker's derived list excludes occluded rows exactly as the render map did |

## 7. Decisions taken here (Adam can overrule)

Margin: variant 2 for the CPU consumers (the S7 default), with the motion pad for moving rows. Alpha: CPU sort of the verdict's alpha groups (OIT is a picture change and stays a separate decision). Secondary views: the same derived list. The old and the new path run side by side only behind their switch and only until each stage's soak, then the old one is deleted (the rule).

## 8. What this does not promise

The numbers: the main thread pays about 1.4 ms for all of it today (`stateSort` early 0.6-0.7, `updateCull` 0.27, cascade culls about 0.5); the design removes the sort, the render map and the walk of the covered partitions, and adds the row events (0.012-0.019 ms measured for the cull table) and the verdict iteration. The cascades' culls and `updateCull` for the walked partitions remain until the cascades and the object cache have their own stage. The point is the deletion.
