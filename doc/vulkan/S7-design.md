# S7 part 1: Hi-Z occlusion and GPU visibility (design only, 2026-10-22)

Scope agreed with Adam: design here, build and test in a new session. Order: this (S7 part 1), then S6 (structure), then the rest of S7 (GPU LOD, OIT). H0-H3 (sections 9-11) are built and tested: world-camera occlusion is the worker's Hi-Z pass, the query code is gone.

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

## 9. H0 results: what the occlusion class costs today (2026-10-22, London midnight, no behaviour change)

Tools added (commit after 35e5e2d4): `LL_VK_OCC_CENSUS=1` (header `lloccensus.h`, one log line per camera every 600 world passes, tag `OccCensus`: query issue, readback, result ages, cull drops, probe queries, times by `rdtsc`), CPU zone `occlusion: doOcclusion`, worker op-stat line "draw of 8 indices (occlusion cubes, included in draw)". Runs: `h0a` (timing, no phases, OPSTATS + CPU zones + census; 47 windows of 600 frames, first 5 dropped for the averages below) and `h0b` (phases + `FSGpuZoneTimers` + UNICENSUS; phases inflate worker times, so only GPU and record counts are taken from it). Logs in `<work>un-logs\h0a.log`, `h0b.log`. The crowd differs from the x7a run (345 cube draws a frame in h0a, 440 in h0b, x7a 448).

| item | per frame |
|---|---|
| cube draws (worker count) | **345**: 200 group queries + **146 probe queries** (reflection/hero probes are 42 %, not "few") |
| occlusion list at `doOcclusion` | 776 groups, **627 already OCCLUDED** (topmost occluders are re-queried); a query is still pending for 492 of 691 calls (kept), 201 issued |
| main thread: `LLPipeline::doOcclusion` zone | **0.099 ms**: group loop 0.058 (incl. the ~1,300 records of the cubes), probes 0.038, `processOccluders` ~0 (0 calls with groups) |
| main thread: `checkOcclusion` | 5,207 calls; the pending branch (1,016 calls: `queryReady`/`queryResult`, state propagation) **0.117 ms**; the other ~4,200 calls are the cheap path (not timed, est. 0.02 ms) |
| main thread total of the class | **about 0.22-0.25 ms** (cull and sort loops that read the result are not in it) |
| worker | cube draws 345 x 313 ns = **0.107 ms**, `queryBegin` 0.107, `queryEnd` 0.034 -> **about 0.25 ms** of 12.7 ms in operations (the worker waited 0.6 ms in this run: it is not limiting) |
| GPU | zone `doOcclusion` **0.03 ms** (h0b); the cube draws are not a GPU cost |
| what it finds | 75 % of the results say OCCLUDED (152 of 200); the cull then drops **627 groups a frame** as OCCLUDED (the sort loops see 0.5), so occlusion is worth a lot, and a more conservative test could cost more in `updateCull`/`stateSort` than it saves |
| result age when read (frames after issue) | **3: 59, 4: 54, 5: 88**, never younger than 3; 0.05 forced reads a frame (the 4 frame timeout works on the call count of `checkOcclusion`, which is called 3-4 times a frame per group). So today's lag is 3-5 frames, not 1-4 as assumed in section 1 |
| probes | 559 calls, 147 reads (120 occluded), 400 still pending, 147 new cube draws, 0.038 ms |

**What this means for H1-H3.** The main-thread prize of A is about 0.2 ms (H2, world groups only: group loop 0.058 + pending branch 0.117 minus the new box record and ring read, so about 0.15; H3 adds the probes 0.038 and the cheap-path calls: about 0.2-0.25), the worker gets about 0.25 ms back, the GPU nothing measurable (0.03 ms, Hi-Z adds its own dispatches). That is at or below the 0.2 ms line the plan set, so this stage's value is mostly structural (no query pool/state machine, no blocking read, results 1-2 frames old instead of 3-5), plus the groundwork for B. Risk to weigh: Hi-Z is conservative, so it reports fewer occluded groups than the queries (627 groups dropped a frame today); if it keeps materially fewer, `updateCull`/`stateSort` work grows and eats the gain. H1's mode 2 measures exactly that before anything depends on it.

## 10. H1 + H2: Hi-Z occlusion built (2026-10-22, commit 96d8bc98; London midnight)

**What was built.** `LLPipeline::doOcclusion` (same point of the G-buffer pass as the cubes) sends one record, **kind 34**, with the boxes of the occlusion groups (+ the view-projection). The worker (`on_hiz_boxes`, llvkworker.cpp) ends the pass with `recordExternal` (depth to read-only; the pass split costs nothing extra: the cubes' read-only depth draws already split the pass there, and the next draw starts a new one), builds a max-depth pyramid of the bound depth target (compute, one dispatch a level: first level half resolution, floor sizes like Vulkan's mip chain, the odd last row/column folded into the last texel; 10 levels at 1713x898, 11 dispatches), and tests every box with one thread: nearest depth of its 8 corners against the largest depth of the pyramid texels under its screen rectangle, at the level where the rectangle spans up to about 9 x 9 texels (`LL_VK_HIZ_FINE`, default 5 = five levels finer than "2 texels"). Depth is NOT reversed-Z: 0..1 near to far, so the test is `nearest > max depth under the rectangle`. Three words a box into a ring of 8 slots of host-visible **cached** memory: result bits (occluded, level, special cases), nearest box depth, largest pyramid depth. The viewer reads a slot when the frame's fence has signalled (`hiz_poll` publishes the serial after `vmaInvalidateAllocation`; `LLVkBridge::hizRead`, never blocking; a serial that never publishes is dropped after 60 frames). `LL_VK_HIZ` 0 / 1 / 2 existed for H1/H2 (queries / Hi-Z drives / both, queries drive, Hi-Z compared with the query issued with the same box); H3 removed the switch.

**Pass structure answer (section 7, risk 4):** the depth CAN be sampled at the query point without an extra split (see above); the fallback of section 7 was not needed. Depth convention checked: the shader wrapper maps GL clip z to (z+w)/2, no y flip anywhere, framebuffer row = (ndc.y*0.5+0.5)*H; the test uses the same matrix the cubes used (`gRender.getProjectionMatrix() * gRenderModelView`).

**Mode 2 compare (the H1 gate, 195-250 boxes a frame, queries issued with the same boxes):**

| pyramid level read | Hi-Z OCCLUDED where the query saw samples (pop-in risk) | Hi-Z visible where the query said occluded (lost saving) |
|---|---|---|
| 1 finer than "2 texels" (first build) | 5.1 boxes = 2.1 % | 114 = 46.7 % |
| 3 finer | **0** | 45 = 23.1 % |
| 5 finer (default) | 0.4 = 0.2 % (gaps of 1e-4..1e-3 in depth) | 29 = 15.0 % |

Why the loss never reaches zero: the old cube draw is an 8 vertex triangle FAN from the box's nearest corner over its silhouette, i.e. a surface INSIDE the box, deeper than the box's front faces, so the query was more aggressive than any conservative box test. Hi-Z reports about 80 % of what the queries found and culls what is visible only through thin gaps. Result age when applied: 2-4 frames (mostly 4) against 3-5 for the queries (0.05 forced reads a frame: the viewer's timeout never mattered).

**Mode 1 (Hi-Z drives OCCLUDED) against mode 0, same exe, London (h2a/h2c mode 0, h2b/h2e/h2f mode 1; the crowd differs a bit between runs, so differences under about 0.1 ms are noise):**

| per frame | queries (mode 0) | Hi-Z (mode 1) |
|---|---|---|
| main thread: `doOcclusion` zone | 0.089-0.106 ms | 0.063-0.067 (collect 0.014, send 0.001) |
| main thread: query read (`checkOcclusion` pending branch) | 0.12 | 0 |
| main thread: apply results (inside updateCull) | - | 0.03-0.05 |
| **main thread net** | | **about -0.15 ms** |
| worker: `queryBegin` + `queryEnd` | 0.12 + 0.03 | 0 |
| worker: cube draws (group + probe + object cache) | 290 a frame (0.09 ms) | 120 (only probes) |
| worker: kind 34 record | - | 29 us |
| **worker net** | | **about -0.14 ms** |
| GPU | zone `doOcclusion` 0.03 ms | pyramid + test 0.05 ms, zone 0.01: **+0.03 ms** |

Sort/cull side: groups dropped as OCCLUDED by the cull 608-631 against 580-602 (Hi-Z), groups reaching `stateSort` 2030-2060 against 2165-2180 (about 6 % more, the cost of the conservative test; included in the zones above; the frame interval did not move outside the noise: 14.9-15.0 against 14.2-14.9 ms). Pictures: the static scene is equal to the query run (the difference image of two adjacent runs shows only the moving avatars and edge noise); runs minutes apart cannot be diffed (the night environment keeps moving). Validation + sync validation (RT_VALIDATE=1 RT_SYNCVAL=1) found my pyramid asked for 11 mips where the image allows 10 (fixed); no Hi-Z message since (the remaining messages are old ones: buffer copy hazard in the BRDF / copy shaders, fragment output without attachment in the shadow programs). Adam asked about GPU % in the phases run (h2d): the logs show the same GPU time a frame with and without Hi-Z (13.0 / 13.4 ms) and a main-thread limited frame in both phases runs.

## 11. H3: probes and the object cache on the same path, query machinery deleted (2026-10-22)

End state, no old path: `LLHizTarget` / `LLHiz` (llhiz.h, llvieweroctree.cpp) collect the boxes of everything that wants a verdict in ONE batch per frame: occlusion groups (`LLOcclusionCullingGroup::hizCollect`), reflection and hero probes (`LLReflectionMap::hizCollect`, hizResult sets `mOccluded`), the object cache's groups (`LLVOCachePartition::processOccluders` with the region shift). `LLHiz::send` once, `LLHiz::apply` at the start of every world cull. Deleted: `mOcclusionQuery[]`, `mOcclusionCheckCount[]`, `mOcclusionIssued[]`, the query name pool and `sPendingQueries`, `getNewOcclusionQueryObjectName`, the `QUERY_PENDING` / `DISCARD_QUERY` states (now `mHizSerial` != 0 = a verdict awaited, `hizInvalidate()` = the group changed: the behaviour of the discard flag), `checkOcclusion`'s query branch and the timeout (setting `RenderOcclusionTimeout` removed), the group's `doOcclusion`, the probe query pool and `cleanupQueryPool`, the cube drawing and `gOcclusionCubeProgram` with its shader file, the "Queries pending" debug line. The scene monitor keeps its own query object (generated directly). The shadow-cascade occlusion pass (`doOcclusion(shadow_cam)`) is **gone**: it needed the cascade camera's own queries; ray shadows (the default) never used it; in cascade mode the casters are no longer occlusion culled (a perf difference, not a picture difference). Without the Vulkan worker nothing is occluded.

Found by the first H3 run: probes dropped from 82 % occluded (queries) to 11 % because a box with all corners behind the camera or outside the view was reported visible, while a clipped cube has no samples. Fixed in the shader (bit 19 = wholly behind the camera, bit 17 = outside the view: both OCCLUDED; a box cut by the near plane stays visible): probes 64-65 % occluded (90-130 of 140-200 boxes a frame). London (h3c/h3d, a heavier crowd than the H2 runs: 350-500 boxes a frame of which 140-190 probes): worker draws 862-950 a frame (was 1150-1270 with queries), no cube draw, no query record; `LL_VK_OCC_CENSUS=1` lines "Hi-Z window" and "camera 0" show boxes, results, ages, probes and the main thread ms of each piece. Three clean runs (h3b, h3c, h3d), no device lost, clean quits.

Open / not done: the Hi-Z pyramid can also serve B (fragment cull, S7 later) and SSR; the `mCubeVB` still draws the (non pull) lighting volumes when `LL_VK_LIGHT_PULL=0`; cascade shadow occlusion could come back with a pyramid of the cascade depth if the non-ray mode matters.
