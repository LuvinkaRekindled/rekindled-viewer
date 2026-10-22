# S7 part 1: Hi-Z occlusion and GPU visibility (design only, 2026-10-22)

Scope agreed with Adam: design here, build and test in a new session. Order: this (S7 part 1), then S6 (structure), then the rest of S7 (GPU LOD, OIT). Nothing in this note is built.

## 1. What "visible" means today (census)

Visibility is decided on the CPU in `LLPipeline::updateCull` (octree walk, `LLOctreeCull`):

1. `earlyFail`: `group->checkOcclusion()` (reads the result of an earlier occlusion query), and if the group is `OCCLUDED` it is dropped (`markOccluder` keeps the topmost one as an occlusion group so a query is still issued).
2. `frustumCheck` on the group bounds.
3. `processGroup` -> `markNotCulled`: `group->setVisible()` (stamp `mVisible[camera] = sCurVisible`), `updateDistance` (world camera only), push to the drawable-group or visible-group list, and push to the occlusion list if the group needs an update or was not visible last frame.

"Visible" is therefore **frustum AND not-occluded-by-a-query-that-is-1-to-4-frames-old**. The occlusion part is already stale by design (`RenderOcclusionTimeout` = 4 frames, a pending query is only read when ready or after the timeout). `LLDrawable::isVisible()` is true if the drawable's own stamp is current or its group is visible this frame.

### Occlusion queries today

- Issued mid-frame in `renderGeomDeferred`, at the first pool of type >= `POOL_GRASS` (after the big opaque occluders drew into the depth buffer): `LLPipeline::doOcclusion` walks `sCull->beginOcclusionGroups()`, per group `doOcclusion` draws a box (8-vertex fan, `mCubeVB`) inside a `QUERY_ANY_SAMPLES` begin/end. Plus reflection probe and hero probe occlusion, plus `LLVOCachePartition::processOccluders`.
- Measured (London, per frame): **448 cube draws, 377 queries**. Groups with `earlyFail` (camera inside the box etc.) are marked non-occluded without a query. Occlusion is not done for shadow, cube snapshot or reflection renders.
- Feedback: `checkOcclusion()` at the next cull reads the query (`queryReady`/`queryResult`; blocking read after the timeout), sets/clears `OCCLUDED` with `STATE_MODE_DIFF` (propagates to children); a parent that is `OCCLUDED` implies the child is.

### Who consumes the visible bit on the CPU

| consumer | site | what it does with it | needs same-frame? |
|---|---|---|---|
| texture sizing | `LLVOVolume::updateTextureVirtualSize` (`isVisible()` at llvovolume.cpp:918/931 via `LLVOVolume::isVisible`, attachments follow the avatar's drawable) | invisible: pixel area and virtual size of every face set to 0, so textures are not requested and decay | no (a frame or two late only delays loads) |
| texture list | `llviewertexture.cpp:3815/3831` `isRecentlyVisible()`; `llviewertextureanim.cpp:105` | boost/decay, animation skip | no |
| avatars | `llvoavatar.cpp:6309, 6389, 9760, 12327`, `lldrawable.cpp:1547` | update visibility, attachment visibility, skip work when not visible | lag of 1 frame shows as a one-frame pose/LOD freeze on screen entry |
| flexi | `llflexibleobject.cpp:346, 434` | simulate only when visible | no |
| particles | `llvopartgroup.cpp:245`, `llviewerpartsim.cpp:817` | skip update for invisible groups | no |
| object cache | `llvocache.cpp:906` | which cache groups need a query | no |
| group work in `stateSort` | `checkOcclusionAndRebuildMesh`, `rebuildMesh`, `changeLOD`, `updateDistance`, drawable `markVisible`/stamp, face-pool enqueue | rebuild dirty geometry only for visible groups; LOD from distance; sorted lists for alpha | LOD and mesh rebuild want it before the draw of the same frame |
| render lists | `postSort` render maps, `geo_list` for the table | what the CPU asks the renderer to draw | yes (this frame's camera) |
| debug | `renderNormals`, `llselectmgr.cpp:1767`, `llviewertexturelist.cpp:919` | none that matter | - |

Two things follow:

- Everything except the last two groups tolerates a visible bit that is 1-2 frames late (they already live with query lag).
- The render lists and the group work in `stateSort` need the frustum part **this frame**. A moving camera makes a one-frame-late frustum test visibly pop in at the screen edge, so the frustum part must stay current, on the CPU or in the same frame's GPU cull.

## 2. What the GPU can take over, and what it removes from the CPU

| piece | CPU cost it removes | GPU cost | feedback to the CPU |
|---|---|---|---|
| **A. Occlusion by Hi-Z compute instead of queries** | the 448 cube draws and 377 query begin/end in the main stream (records, setBuffer, uniform writes), the worker draws for them, the blocking query reads, the query object pool | one depth downsample chain (about 6-8 mips, one dispatch each or one single-pass) plus one test dispatch over about 400 boxes: well under 0.1 ms (queries cost the GPU about the same as 448 small draws with state changes) | a per-group occluded bit, read back one frame late: same format as the query result |
| **B. Frustum + Hi-Z cull of table fragments on the GPU** (first for the G-buffer view) | none yet (the CPU still walks the octree and builds `geo_list`); later the table-served part of `postSort` | the cull compute pass over about 7-8k fragments, then indirect draws | none needed while the CPU cull stays |
| **C. Make the CPU cull optional** (visible bit read back from B for the consumers above) | `updateCull` 0.9 ms, a part of `stateSort` (2.7 ms early + 1.3 ms postSort) | none | the visible bit, one frame late, for the table above |

C is the only piece that cuts the large CPU costs, and it depends on B and on S6: until every drawn thing (terrain, avatar bodies, trees, UI, water, sky) has a table entry or its own list, the CPU still needs `stateSort` and `postSort` for them. So **C cannot be finished inside S7 part 1**; A and B can, and they are valuable on their own.

## 3. Honest expected gains (from the measured frame, S6-census section 14)

- Main thread is the limiter at 14.2 ms (display 9.7, idle 4.2); worker busy 10.2 of 14.2; GPU about 13.
- A: the 448 draws + 377 queries are about 6% of the remaining GL-shaped draws. Expected main-thread gain 0.3-0.6 ms (to be measured with the existing `doOcclusion` CPU zone and `LL_VK_UNICENSUS` per-zone numbers: **step H0 below measures it first, before any build**), worker draws about -0.15 ms, GPU about -0.1 ms. It also removes the stall risk (`queryResult` blocking read after the timeout) and the occlusion state machine (`QUERY_PENDING`, `DISCARD_QUERY`, timeouts, `getNewOcclusionQueryObjectName` pool).
- B: GPU only at first (fewer vertices/draws for occluded or off-screen fragments inside visible groups: groups are coarse, fragments are fine). Plausible GPU gain 0.5-1.5 ms in dense scenes, none on the CPU. This is not what limits the frame today, so B is deferred behind A and S6 unless H0 numbers say otherwise.
- C (later): up to about 3 ms of main thread. That is the real prize and it is gated on S6.

I do not claim a frame-rate change from A alone; it is a step that removes a GL-shaped class from the stream and an old state machine.

## 4. Design of A: Hi-Z occlusion with the same CPU feedback

### Inputs the CPU sends (one record kind, frame-local, no table)
At the point where `doOcclusion` runs today (same place in the frame, so the same depth is tested) the pipeline sends one `kind 34` record: a list of boxes `{center xyz, half-size xyz, group handle}` for the occlusion groups, with the same fudge (`SG_OCCLUSION_FUDGE`, water uses 1.0 in z and depth clamp) already applied, and the same `earlyFail` groups excluded (they are marked non-occluded on the CPU without a record, exactly as now). Probe and hero-probe occlusion and `processOccluders` stay on the old path in the first version (they are few; a second step moves them).

### Worker/GPU side
1. At the record's position in the stream (after the occluders drew) build the Hi-Z pyramid from the current G-buffer depth (max reduction, reversed-Z aware, one mip per dispatch or one single-pass downsample; the depth image is already sampled by lighting). The pyramid persists as a target, resized with the render target set, and is the same one DLSS/SSR hierarchy can later reuse.
2. Test dispatch: one thread per box, project the 8 corners with the view-projection of that record (matrices travel in the record, as for light volumes), conservative screen rect, pick the mip where the rect covers about 2x2 texels, compare the nearest box depth against that texel max. Camera inside the box or any corner behind the near plane: visible. Water groups: depth clamp equivalent = compare against far.
3. Output: one 32-bit word per box (bit set = occluded) into a host-visible ring slot belonging to this frame (3-slot ring like the bindless sets). A fence per slot.

### Feedback (replaces `queryReady`/`queryResult`)
- `LLOcclusionCullingGroup` stores `mHizSlot`/`mHizIndex` instead of `mOcclusionQuery[]`.
- `checkOcclusion()` becomes: if the slot's fence has signalled (non-blocking check, a flag the worker sets), read the bit, set/clear `OCCLUDED` with `STATE_MODE_DIFF`, clear `QUERY_PENDING`. If not signalled yet, keep the previous state and keep `QUERY_PENDING` (no timeout, no blocking read ever). The parent-implies-child rule and the topmost-occluder rule in `markOccluder` stay unchanged.
- The results are naturally one frame late (the worker finishes frame N while the main thread culls N+1): the same lag as a query that is ready after one frame, which is the common case today; the 4-frame worst case disappears.

### Differences from queries (decide before building)
1. A query counts samples that pass the depth test against **what was drawn so far in that frame**; the Hi-Z test is conservative (a box is occluded only if its nearest depth is behind the max depth of every texel it covers). Hi-Z therefore reports fewer occluded groups than the query for thin gaps, never more. Pop-in cannot get worse; the saving can be a bit smaller. Verify mode measures that difference.
2. Queries see transparent or alpha-tested pixels only as the passes before the query point drew them; same for Hi-Z, since it uses the same depth at the same point.
3. DLSS render resolution: use the render-resolution depth (what the G-buffer wrote), not the display-resolution one.
4. Reversed-Z: the pyramid reduces with min or max accordingly (to confirm from the existing depth convention in the native renderer; one line in the shader).

### Switch and verify
- `LL_VK_HIZ=0/1/2`: 0 = queries (today), 1 = Hi-Z drives `OCCLUDED`, 2 = both run, queries drive, and the log reports per frame: groups where Hi-Z says occluded and the query says visible (pop-in risk, must be about 0 when the query is current), and groups where the query says occluded and Hi-Z says visible (lost saving; expect a few percent).
- Gate: mode 2 shows no systematic "Hi-Z occluded, query visible" groups over a London run and a crowd run; mode 1 pictures identical to mode 0 on the baseline night shots; no new device lost; `[hiz]` counters in the log (boxes tested, occluded, results late, ring slot reuse).

## 5. Design of B and C in outline (not part of the first build)

- **B:** a cull dispatch over the scene table's fragment bounds (already in the 160-byte rows or the fragment records: to check whether bounds are present, otherwise add 6 floats), for the G-buffer view: frustum test, then Hi-Z against the pyramid **of the previous frame** (reprojected with the previous view-projection, two-phase as in the standard "draw last frame's visible, rebuild pyramid, test the rest" scheme), output compacted indirect draws per pass type. The pull shaders already read their rows by address, so a draw becomes `vkCmdDrawIndexedIndirectCount` over a list. Requires the worker to own the indirect buffers per pass type (needs S6's pass descriptions to be self-contained: this is why S6 comes right after A).
- **C:** the CPU stops producing `geo_list` for table-served pass types. The visible bit for the consumers (table in section 1) comes from a per-group/per-drawable visible bitmask the cull pass writes, read back one frame late; the CPU frustum walk stays for the partitions not yet table-served and for the sim consumers until their feedback is proven. Blockers: face-based pools (terrain, avatar bodies, trees, grass), water, sky, UI, HUD must each have a table entry or a separate list; `changeLOD`/`updateDistance` and `rebuildMesh` need the group visible flag before the draw of the same frame (keep the CPU frustum for those or move LOD to the GPU = rest of S7).

## 6. Stages (each ends in something measurable, behind a switch)

| stage | what | gate |
|---|---|---|
| H0 | **measure only**: CPU and worker cost of the occlusion class today (`doOcclusion` zone, 448 draws, 377 queries, `checkOcclusion` and `queryResult` time per frame, GPU time of the query draws via the phases run) from existing counters; also log how many of the 448 end up occluded and how old each result is when read | numbers written to this document; a go/no-go for H1 from Adam |
| H1 | Hi-Z pyramid + test dispatch + ring + `kind 34`, mode 2 (both run, queries drive) with the comparison counters | zero systematic Hi-Z-occluded/query-visible groups; compute cost measured |
| H2 | mode 1: Hi-Z drives `OCCLUDED`; query code skipped (no cube draws, no begin/end) | pictures identical, main thread and worker deltas from H0, no new device lost over 3 runs |
| H3 | probes and `processOccluders` onto the same path; delete the query machinery for the world camera (`mOcclusionQuery[]`, query pool, timeout, `DISCARD_QUERY`, `RenderOcclusionTimeout`) | no old path (standing rule: end state has no old path) |
| after S6 | B (fragment cull for table-served passes), then C | their own design notes |

## 7. Risks and unknowns

- Occlusion state is shared with `LLVOCachePartition`, reflection probes and `LLSpatialBridge`; each reads `OCCLUDED`. H1/H2 keep the state names, so they should be unaffected, but the census must grep every `isOcclusionState` reader before H2.
- Group handle lifetime: a group destroyed between record and readback must not receive a result (use the same generation-checked handle scheme as the bindless id ring; the group table is `LLSpatialGroup*`, so the record carries an index into a per-frame array and the CPU resolves it with a dead-check, as `doOcclusion` does now with `group->isDead()`).
- Readback memory must be host-visible cached (not write-combined; standing rule from the descriptor work: never read write-combined memory on the CPU).
- The Hi-Z pyramid is built from the depth that exists at the query point; if the G-buffer depth target is rendered in a pass that has not ended (render pass in progress), the downsample needs the depth in a sampled layout: this may force a pass split at that point (to check in the native renderer's pass structure before H1; if costly, build the pyramid at the end of the G-buffer pass and use it for the next frame's tests, which doubles the lag to two frames but costs no split).
- Open bugs unrelated to this (gray crowd clothing, outfit flicker, absent foliage) touch visibility consumers (`updateTextureVirtualSize` zeroes pixel area for non-visible drawables). If stage C is ever done, that function is exactly where late visibility would bite; keep this note when revisiting those bugs.

## 8. Decision for Adam

Go for H0 (measure) in the new session, then H1-H3. B and C wait for S6.
