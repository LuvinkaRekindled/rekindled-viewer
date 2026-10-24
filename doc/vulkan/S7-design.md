# S7 part 1: Hi-Z occlusion and GPU visibility (design only, 2026-10-22)

Scope agreed with Adam: design here, build and test in a new session. Sections 12 (2026-10-23) is the plan for B, C, GPU LOD and OIT; the rest of this note is the census and part 1 (done). Order: this (S7 part 1), then S6 (structure), then the rest of S7 (GPU LOD, OIT). H0-H3 (sections 9-11) are built and tested: world-camera occlusion is the worker's Hi-Z pass, the query code is gone.

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

C is the only piece that cuts the large CPU costs, and it depends on B and on S6: until every drawn thing (terrain, avatar bodies, trees, UI, water, sky) has a table entry or its own list, the CPU still needs `stateSort` and `postSort` for them. So **C cannot be finished inside S7 part 1**; A and B can, and they are valuable on their own. (Update 2026-10-23: the typing of those classes is S6.6, S6-design.md section 9; B and C start after it.) (Status 2026-10-23: terrain, water, the exclusion mask, avatar bodies and the texture matrix draws are served; shiny, bump, glTF double sided, HUD, sky and the shadow fallbacks, about 40 draws, keep a CPU list until they are; the UI and the fullscreen steps stay on the draw record and need none.)

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

Tools added (commit after 35e5e2d4): `LL_VK_OCC_CENSUS=1` (header `lloccensus.h`, one log line per camera every 600 world passes, tag `OccCensus`: query issue, readback, result ages, cull drops, probe queries, times by `rdtsc`), CPU zone `occlusion: doOcclusion`, worker op-stat line "draw of 8 indices (occlusion cubes, included in draw)". Runs: `h0a` (timing, no phases, OPSTATS + CPU zones + census; 47 windows of 600 frames, first 5 dropped for the averages below) and `h0b` (phases + `FSGpuZoneTimers` + UNICENSUS; phases inflate worker times, so only GPU and record counts are taken from it). Logs in `<work>
un-logs\h0a.log`, `h0b.log`. The crowd differs from the x7a run (345 cube draws a frame in h0a, 440 in h0b, x7a 448).

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


## 12. B, C and the rest of S7 after S6 (design, 2026-10-23; no code yet)

Written for the session that builds it; the hand-off with the practical details is `S7-handoff.md`. This section replaces section 5 (outline) and the "after S6" row of section 6. Naming: "B" and "C" here are S7's (GPU fragment cull, CPU cull optional); S4 also had a B and C (persistent rigged fragments, moving matrices), both built and on by default.

### 12.1 What changed since sections 1-8

- A (Hi-Z occlusion, sections 9-11) is built and the query machinery is deleted. S6.6 typed every buffer-based draw class (S6-design.md section 9 and its status paragraphs): terrain, water, sky, haze, the avatar bodies and rigged layers, HUD, bump, shiny, the shadow casters of every kind, glTF. What stays on a generic draw record is the UI, the impostor quads and the fullscreen steps, which have no visibility question. The old paths that remain are fallbacks behind switches (`LL_VK_TERRAIN_PULL=0`, `LL_VK_PULL_TWINS=0`) and wait for the 6.7 test list; they do not block anything here.
- **Typed is not table-served.** "Typed" means the draw goes through a pull program and a record the worker understands. Which classes have a fragment in the scene table (their draws come from a GPU-side list, not from a CPU walk of draw infos) is a smaller set: the static opaque pass types (S3, 13 types), rigged opaque (S4), alpha runs (S5), moving matrices (S4). Terrain, water, sky, the avatar body meshes, bump, shiny, grass, trees and the shadow fallbacks are still chosen and ordered by the CPU pools (face pools, `renderGroups` loops) and only drawn through the new records. The first job of the build session is the exact list (census: for each pass type, the share of draws that come from the table against the pool loops).
- The measured frame (S6-census.md section 14, London midnight, 70 fps, **taken before S4-S6: re-measure first**): the main thread is the limiter at 14.2 ms (`display()` 9.7: stateSort early 2.7, updateCull 0.9, postSort 1.3, shadow prep waits 1.0; `idle()` 4.2: object update 3.4, avatar motion 2.0); the worker waits 4.0 ms of 14.2; the GPU is about 13 ms (alpha runs 4.0, ray scene builds 1.3, DLSS 1.3, deferred lighting 1.2, main lighting 1.7). The three are close.

### 12.2 What the CPU visibility chain does (the part that is not "a list")

Three different jobs sit inside `updateCull` / `stateSort` / `postSort` (pipeline.cpp 2749, 3427, 4126); only the first moves to the GPU easily.

| job | where | GPU-able? | note |
|---|---|---|---|
| (a) the decision: frustum on group bounds, the occlusion verdict (Hi-Z now, applied at the start of the world cull, 2-4 frames old) | `updateCull` octree walk (`LLOctreeCull`), `earlyFail`, `checkOcclusion`, `OCCLUDED` state with `STATE_MODE_DIFF` to children | yes: a flat table of group bounds, one test dispatch, verdict readback | the frustum part must be this frame's or conservative (12.4) |
| (b) state work on visible groups | `stateSort`: `group->setVisible()` stamp, `markVisible(drawable)` per element, `rebuildMesh()` (dirty geometry is built only for visible groups), `changeLOD()` / `updateDistance()` (distance LOD, alpha sort keys), the avatar render-distance and `isVisible()` filters, selection hiding | partly | CPU *state* that the update code and the builders read; it needs "visible" before the draw of the same frame. This, not the list pushes, is most of the early sort's 2.7 ms (to confirm in C0) |
| (c) list production | `postSort` render maps, `geo_list` for the table, pool enqueues (`addFace`), the sorted alpha lists | the table-served part, yes | alpha group order is by distance, fixed per frame on the CPU (S5 kept it) |

The consumers of the visible bit (section 1 table) split the same way: texture sizing, the texture list, flexi, particles, the object cache and avatar update tolerate a verdict 1-2 frames late; **the group state work (b) and the lists (c) do not**: a moving camera would pop geometry in at the screen edge.

### 12.3 B: GPU fragment cull for the table-served passes

- **Input:** the fragment rows of the scene table (`GeoGpuG`, 160 B, `llvkworker.cpp` ~6026) **have no bounds**; add them per item (6 floats in a side array, not in the row, so the row stays 160 B and aligned), written when the fragment is made; the pass frustum; the **previous frame's** Hi-Z pyramid (A builds one for its own tests: find where it is built and kept, reuse it).
- **Output:** compacted indirect draws per pass type; the pull shaders already read their rows by address, so a pass becomes an indirect-count draw over the compacted list. The S2 cull shader (shadow casters, `LL_VK_SCENE_RESIDENT`) is a base. Note `LL_VK_SCENE_GEO` (pipeline.cpp:5508) defaults to 2 = the S3-S4 table with its persistent rigged / moving parts, **not** a GPU cull mode (the S3 text planned one; check whether anything of it was built before assuming).
- **Honest expectation:** GPU only. The CPU still produces the visible group list, so B removes fragment-level overdraw inside visible groups (fragments of big groups that are off-screen or hidden). Section 3 guessed 0.5-1.5 ms of GPU in dense scenes; unmeasured. **Stage B0 measures before building:** a count-only cull pass reporting, per pass type, how many table draws and triangles are frustum-outside or Hi-Z-hidden inside the groups the CPU keeps.
- **Gate:** the GPU-visible set must contain every fragment the CPU would draw that is truly visible (verify: with occlusion off the sets are equal up to bounds rounding; with occlusion on, only Hi-Z lag differs); pictures equal; no new device lost over 3 runs.

### 12.4 C: making the CPU cull optional, staged by class

The idea is a **flat table of all spatial groups** (bounds, type flags, parent chain for `OCCLUDED` inheritance) tested on the GPU, with the verdict read back and applied to the CPU state one frame late; the octree walk goes away for classes that have fragments. The central risk is pop-in, so the verdict must be conservative about camera motion:

- The GPU tests last frame's pose against a frustum expanded by a bound on the camera motion over the latency (rotation and translation of recent frames plus a margin); groups in the expanded set get their state work (b) and their draws; B's draw-time test with this frame's exact planes removes the rest. Cost: a few percent more groups carry state work; benefit: no octree walk and no pop-in. Fallback if the margin proves too large in fast turns: keep a CPU frustum test over the flat table (a loop over a few thousand bounds, far cheaper than the walk) and send only occlusion to the GPU.
- Stages (each behind a switch, old path intact until its gate):
  - **C0 measure:** zones for `updateCull`, `stateSort` parts (a)(b)(c) and `postSort` now; a census of drawables by class and whether their group has table fragments; a census of every reader of `isVisible`, `setVisible`, `mVisible[camera]`, `OCCLUDED`, `mLastUpdateDistance` (section 1 lists sites; re-grep, the code moved).
  - **C1 shadow mode:** the flat group table, GPU test and readback, compared every frame with the CPU's visible set (missed = in the CPU set but not in the GPU set must be 0 with the expanded frustum; extra = the cost). No behaviour change.
  - **C2 lists from the GPU for table-served classes:** their `geo_list` and render-map entries stop coming from `postSort`; the groups of those classes get only the state work (b), from the late verdict.
  - **C3 consumers read the late bit:** texture sizing, flexi, particles, the object cache, avatar filters (one frame late is the status quo for occlusion already).
  - **C4 face-pool classes** (terrain, water, sky, avatar bodies, trees, grass, bump, shiny): fragments, or a tiny always-visible CPU list (terrain patches and water are few; avatar bodies are `LLVOAvatar` drawables, not octree groups), or the CPU path stays. This decides how much of the octree walk can go: if any class stays on the walk, the walk stays and C saves only the table-served share. Decide from C0's census, not before.
  - **C5 remove the walk** for the groups that are fully covered; keep it for shadow cascades, probes, mirror views until they have their own flat tests (the cascade cull job is the partition walk, S-retire-views-design.md).
- **Not covered by the flat table without extra work:** spatial bridges (avatar attachments and moving objects, `LLSpatialBridge`), HUD, the object cache partition, the particle and water partitions, and every secondary view (shadow cascades, probes, hero / mirror, snapshots).
- **Honest gain:** removing (a)+(b)+(c) for the covered share is up to about 3 ms of the 14.2 ms main thread (sections 2-3; re-measure in C0). But the GPU (13 ms) is within 1 ms of the main thread: a 3 ms main-thread cut gives roughly the GPU's frame interval, i.e. at most about 1-1.5 ms (7-10 %) in the London scene unless the GPU shrinks too (B, OIT / alpha, scene builds). Say this before building C.

### 12.5 GPU LOD and OIT (own notes, after C0)

- **GPU LOD:** the CPU `changeLOD` / `updateDistance` work and the mesh rebuild per LOD change need all LOD buffers resident and one fragment per LOD with a GPU distance select. Not design-ready: mesh LOD buffers are created lazily by the volume code, prim / sculpt LODs are rebuilt from `LLVolume`. Write the note after C0's census says how much of (b) is LOD.
- **OIT:** the alpha runs are 4.0 ms of GPU (30 %). A weighted-blended or per-pixel-list OIT replaces the CPU group-distance sort, which also removes the sorted alpha lists from (c). It changes the picture (different blend artefacts) and needs Adam's call on quality and a test list (hair, glass, particles, rigged alpha). The best GPU lever in the list and independent of C.

### 12.6 Decisions for Adam before the build session starts

1. Order: B0 (measure) -> C0 (measure) -> pick B or C1 by the numbers (recommended), or start B regardless of the numbers.
2. Pop-in policy for C: expanded frustum (recommended) or a CPU frustum over a flat table.
3. Whether OIT is wanted at all (visual change), and whether it comes before C (the GPU is the closer limiter).
4. The 6.7 test list (terrain / PBR terrain, shadow-map hill, snapshot, impostors) stays open; it needs no S7 code and can run in any session.


## 13. B0 and C0 results: what the visibility chain costs, who reads it, what a fragment cull would drop (2026-10-23, the plaza)

Measure only; nothing here changes what is drawn. Code: `llviscensus.h` and the hooks in `pipeline.cpp`, `llvovolume.cpp` (draw extents), `llvieweroctree.cpp` (`LLHiz::addItem`, the verdicts), `llhiz.h`, `llvkbridge.*`, `llvkworker.cpp` (the Hi-Z test shader gets `mode` / `first`, a second dispatch against the previous pyramid, results buffer 2 x 32768 slots). Taken from the saved branch `s7-census-wip` by hand (no checkout, no restore). **Switches, all default off:** `LL_VK_VISCENSUS=1` (laps and counts, log tag `VisCensus`, one report per 600 main-view post sorts), `LL_VK_VISCENSUS_BOXES=0` (leave the box test out). New `FS_CPU_ZONES` zones: one per partition cull, `postSort: rebuildPriorityGroups / delayed rebuildMesh / sort alpha groups`. No GPU zone timers anywhere. Logs: `<work>\run-logs\b0c0-*.log` (`.smi.csv` = nvidia-smi once a second); runs started by `<work>\b0c0-chain.ps1`.

### 13.1 Which diagnostics were on in each run (numbers are compared only within a row group)

| run | exe | switches | fps (10 s samples, median) | frame avg / p50 / p95 ms | main waited for worker / worker waited for work ms | worker operations / `shadowPackets` / `present` / `rtScene` ms | GPU util (nvidia-smi) |
|---|---|---|---|---|---|---|---|
| `fix1` (reference) | 10:23 | none | 68.3 | 14.6 / 14.4 / 18.6 | 0.6 / 0.0 | 14.56 / 6.41 / 4.80 / 1.82 | - |
| `b0c0-base1` | 10:23 (HEAD) | none | 64.6 | 15.4 / 15.3 / 18.9 | 0.1 / 0.7 | 14.69 / 6.74 / 4.36 / 2.00 | - |
| `b0c0-off` | 10:46 (this tree) | none (census code compiled in, switches off) | 64.0 | 15.6 / 15.2 / 20.3 | 0.8 / 0.1 | 15.53 / 6.88 / 4.90 / 2.07 | **99 %** (p10 96) |
| `b0c0-zones` | 10:46 | `FS_CPU_ZONES` | 60.0 | 16.7 / 16.6 / 19.0 | 0.0 / 6.2 | 10.38 / 6.49 / 0.62 / 1.95 | 88 % |
| `b0c0-cen` | 10:46 | `LL_VK_VISCENSUS=1 _BOXES=0` (laps + counts) | 64.3 | 15.6 / 15.6 / 17.7 | 0.0 / 5.0 | 10.59 / 6.36 / 0.85 / 1.97 | 98 % |
| `b0c0-box` | 10:46 | `LL_VK_VISCENSUS=1` (laps + counts + the box test) | 64.0 | 15.6 / 15.7 / 17.6 | 0.0 / 3.7 | 11.77 / 6.43 / 2.05 / 1.92 | 98 % |
| `b0c0-box2` | 10:46 | same as `box` | 64.0 | 15.6 / 15.4 / 18.9 | 0.2 / 1.2 | 14.08 / 6.47 / 4.17 / 1.93 | 99 % |
| `b0c0-box3` | 10:46 | same as `box` | **68.4** | 14.6 / 14.7 / 16.3 | 0.0 / 4.8 | 9.78 / 5.96 / 0.83 / 1.73 | 94 % |

- **The same configuration gives 64.0 to 68.4 fps from run to run, and the crowd explains it.** Joints in palettes at the end of the runs: `off` 587, `box` 414, `box2` 421, `box3` 378 (`fix1` 519, `base1` 510); triangles a frame in the box census: 14.94 M, 14.26 M, 13.72 M. The lightest crowd (`box3`) is the fastest run, with the GPU time histogram showing frames under 12 ms for the first time (3-13 of 600) and `rtScene` 1.73 ms. So the 68.3 of `fix1` and the 64 of today's plain runs are the same code on different crowds; differences under about 5 % between runs of this scene are not code. This is the "scene drift" the hand-off warns about, larger than it said (+-0.3 ms of `display()`).
- **Reading of the two waits (hand-off section 5), plain runs (`base1`, `off`):** the worker never waits for work (0.1-0.7 ms), the main thread waits 0.1-0.8 ms for the worker, `present` is 4.4-4.9 ms of the worker's 14.7-15.5 ms, GPU utilisation 99 % (nvidia-smi counts "any kernel running": no idle gaps, not how close to the limit). The worker's own CPU work is about 10.3 ms (`operations` minus `present`). That reads as GPU-limited.
- **In the diagnostic runs the reading flips with the crowd.** `cen`, `box`, `box2`: fps equal to the plain runs (64.0-64.3) although the worker waits for work 1.2-5.0 ms. `box3` (light crowd, 68.4 fps): the worker waits 4.8 ms for work, the main thread never waits for the worker, `present` is 0.8 ms and GPU utilisation 94 %: **there the main thread (with the census laps and the box building, a few tenths of a ms) sets the frame**. The zones run (zones cost about 1.4 ms on the main thread, loop 17.0 ms) is main-thread limited too: 60 fps, GPU 88 %. So the GPU and the main thread are within about 1 ms of each other, as 12.4 said, and which one limits depends on the crowd.

### 13.2 C0: where the main thread's visibility work goes (plaza; laps of the main view only, `b0c0-cen`, 18 windows of 600 frames; zone values from `b0c0-zones` include the zone cost)

| piece | ms / frame | what it is |
|---|---|---|
| `updateCull` | **0.85** (zone 0.92) | partition cull 0.87 (zone): VOLUME 0.63, AVATAR 0.105, the rest small; 134 partition culls a frame (main view + cascades); Hi-Z results applied 0.07 |
| `stateSort (early)`, main view, `postSort` inside it | **2.43** (zone 2.53) | |
| ... of which `postSort` | 1.15 | build render map 0.70 (2,218 visible groups), `rebuildPriorityGroups` about 0.39 (zone), delayed `rebuildMesh` 0.025, alpha group sort 0.02, `rebuildGeom` of drawable groups 0.005 |
| ... the rest of the sort | 1.28 | `markVisible` loop (883 drawables of 276 drawable groups) 0.53; per-drawable sort of 610 drawables 0.33 (`updateDistance` 0.22 of it); visible bridges (547) 0.17; `checkOcclusion` 2,510 calls 0.146; `changeLOD` 2,234 calls, 87 true, 0.023 |
| **main view total, cull + sort** | **3.3** | of a 15.5 ms frame (21 %) |

- `postSort` is nested inside `stateSort (early)`: the S6-census sum "updateCull 0.9 + stateSort 2.7 + postSort 1.3" counted it twice. The "up to about 3 ms" of sections 2-3 is the whole of 3.3 ms, and C could not remove all of it.
- Job split of section 12.2: **(a) the decision** = `updateCull` 0.85 + `checkOcclusion` 0.15 = about **1.0 ms**; **(b) state work** = `markVisible` 0.53 + drawable sort 0.33 + bridges 0.17 + `changeLOD` 0.02 + `rebuildPriorityGroups` 0.39 + the other rebuilds 0.03 = about **1.5 ms**; **(c) list production** = build render map 0.70 + alpha sort 0.02 = about **0.7 ms**. (b) is the largest part, as 12.2 expected, and it is the part that stays on the CPU unless its consumers move (C3).
- The cascade prep runs the same functions on the pool (5.1 `postSort` calls a frame in the zones); the main thread only waits 0.43 (cull job) + 0.38 (render map job) ms for them. Not in the table.
- 2,218 visible groups a frame; 0 are OCCLUDED by the time `stateSort` sees them (the cull already dropped those).

### 13.3 C0: table-served draws against pool-loop draws, per pass type (visible groups of the main view, draw infos a frame; `b0c0-cen`)

"Table" = draw infos the scene table serves (not listed in the render map); "pool" = listed in the render map and drawn through the typed pool records. Alpha is separate.

| group | table draw infos (tris) | pool loops (tris) |
|---|---|---|
| static opaque / mask passes | 3,496 (1.17 M) | 171 (20 k) = 4.7 % of the draws, 1.7 % of the tris |
| rigged opaque / mask passes | 856 (11.7 M) | 64 (190 k) = 7 % of the draws, 1.6 % of the tris |
| alpha, static | 1,094 of 1,126 items in the table | 32 |
| alpha, rigged runs | 464 | about 0 (426 rigged alpha draw infos in the map are the table's runs) |

Left to the pool loops, by pass type: FULLBRIGHT 30 (of 450), FULLBRIGHT_SHINY 17.5, INVISIBLE 15, BUMP 9 + BUMP_RIGGED 2 + POST_BUMP 8, NORMMAP_MASK 7, GLOW 27 + GLOW_RIGGED 26, FULLBRIGHT_ALPHA_MASK 38 + RIGGED 28, ALPHA_MASK 6, SIMPLE 13, GLTF_PBR_RIGGED 2 and GLTF_PBR_ALPHA_MASK_RIGGED 4. Everything else (SIMPLE, MATERIAL, SPECMAP, NORMMAP, NORMSPEC and their mask / rigged variants, FULLBRIGHT, GLOW, GLTF_PBR) is 100 % table.

Not draw infos at all (drawables the pools take by face, enqueued by `stateSort (drawable)`): **283 drawables a frame, 449 faces**: TERRAIN 211 faces, AVATAR 161, CONTROL_AV 60, WATER 9, SKY 8. They have no table fragments; C4 would need fragments or a small always-visible list for them.

By partition (visible groups a frame; with a fragment / every draw served): VOLUME 1,323 (1,283 / 764), AVATAR 843 (621 / 420), BRIDGE 39.5 (36.5 / 22), CONTROL_AV 11.6 (7.6 / 4), PARTICLE 17 (0 / 0). Triangles: AVATAR partition 14.0 M, VOLUME 1.07 M; **91 % of the frame's triangles are rigged** (13.6 M of 14.9 M).

Draw calls: the table replaces 3,421 commands in 40 table draw records a frame; the pool loops draw about 60 more in the main view (deferred 29.9: sky 4, bump 11, gltf pbr 4, avatar 7, animesh 3; post-deferred 31.2: fullbright 3, bump 25, fullbright alpha mask 2, water 1).

### 13.4 C0: readers of the visibility state (grep of the tree at 767b771f)

| state | writers | readers (what they do) | tolerates a verdict 1-2 frames late? |
|---|---|---|---|
| group `mVisible[cam]`: `setVisible()` / `isVisible()` / `isRecentlyVisible()` | `markNotCulled` / `LLOctreeMarkNotCulled` (the cull), `stateSort` (`group->setVisible()` in the drawable-group and visible-group loops), `LLVOCachePartition::processOccluders` (llvocache 901-924), resets in the group constructor | `LLDrawable::isVisible` (lldrawable 1138: own stamp, else the group's), `LLSpatialGroup::getUpdateUrgency` (llspatialpartition 756), render loops (llspatialpartition 1288, 1847), `LLVOPartGroup` (llvopartgroup 245), `LLViewerPartSim` (817), `LLViewerTextureAnim` (88, `getVisible(id)`), object cache (llvocache 592, 907) | yes, except the render loops and the update urgency |
| drawable stamp `LLViewerOctreeEntry::mVisible`: `LLDrawable::setVisible`, `markVisible` | `stateSort` via `markVisible`, `LLDrawable::setVisible`, `LLViewerRegion` (1287, 1925-1950), pipeline.cpp 4736-4752 and 11202 (lights), 20450-20523 (the own avatar's attachments, HUD) | `LLVOAvatar::isVisible` (llvoavatar 9755; about 25 uses: animation, motion, LOD, texture requests, attachments 6309 / 6389 / 12327), `LLVOVolume::isVisible` (916-949: texture sizing), `LLFlexibleObject` (346, 434), `llviewertexture.cpp` 3815 / 3831 (`isRecentlyVisible`: texture boost / decay), `llviewertextureanim` 105, lldrawable.cpp 650 (damped move), 1547-1559 (avatars), pipeline.cpp 3733 (avatar filter in `stateSort`), lldrawable.h 236 (sort order) | yes (a frame late shows as a one-frame pose / LOD freeze at screen entry for avatars) |
| `OCCLUDED` (`mOcclusionState[cam]`, `STATE_MODE_DIFF` to children) | the Hi-Z verdict (`hizResult`, llvieweroctree 1221-1286); parents propagate | the cull's `earlyFail`, both `stateSort` group loops, `postSort` render map (pipeline 4146, 4185 of the old numbering), bridges, shadow / probe loops, llvocache 597 / 847, llspatialpartition 1108 / 1199 / 1278 (render loops) and 3602 (inherited mask) | the verdict is 2-4 frames old by design; the readers want the current stamp |
| `mLastUpdateDistance` | `stateSort (bridge)` and `stateSort (group)` after `changeLOD()`, llvovolume 6013, llvopartgroup 689, llspatialpartition 385 | only `LLSpatialGroup::changeLOD()` (llspatialpartition 779: the distance slop that decides LOD / rebuild) | it is bookkeeping of the group state work (b); it moves with it |

Debug-only readers (`renderNormals`, llselectmgr 1767, llviewertexturelist 919, the llviewerdisplay VisDip code) do not matter. **For C:** the readers that need the current frame are the render loops and the group state work in `stateSort`; everything about avatars, textures, flexi, particles and the object cache tolerates one frame. That matches 12.2 and shows no new blocker. The `OCCLUDED` readers are about 25 sites in 7 files: removing the octree walk means keeping `OCCLUDED` correct for each of them (or a flat replacement) in every secondary view.

### 13.5 B0: what a fragment cull over the table items would drop (`b0c0-box`, plaza, 18 windows)

The test: every table item of the visible, not-occluded groups gets a box (draw extents for static items, the model matrix applied for active drawables; for rigged items the owning avatar's animated extents padded 0.25 m) and goes in the Hi-Z batch of the frame. The worker tests it twice: against this frame's view-projection and pyramid (built at the occlusion point of the pass) and against the **previous frame's** pyramid with the previous view-projection. Count only; nothing is dropped. **Caveat: the scripted run stands still** (camera still, avatars idle, only the crowd moves), so the previous pyramid is nearly the same as this frame's: the "hidden, this frame" and "hidden, previous frame" counts agree to the second decimal. A moving camera will make the previous-pyramid drop smaller, or need a margin. Numbers for a moving camera need a run with the camera in motion.

Per frame, 5,960 table items, 14.94 M triangles (items / tris of the type; dropped = frustum outside or hidden by the previous pyramid):

| pass type (items; tris) | no box | frustum outside (GPU) | hidden by previous pyramid | dropped |
|---|---|---|---|---|
| SIMPLE (1,406; 567 k) | 4.8 | 21.6 (7.6 k) | 301.8 (38.6 k) | 323 = 23 % of items, 8.2 % of tris |
| MATERIAL (102; 4.4 k) | 0 | 2 | 52.5 | 54.5 = 53 % |
| MATERIAL_ALPHA_MASK (273; 90 k) | 0 | 1 | 138.3 | 139 = 51 % items, 12.9 % tris |
| SPECMAP (197; 145 k) | 0 | 1 | 80.4 | 81 = 41 % items, 2.7 % tris |
| NORMMAP (79; 35 k) | 0 | 10 | 21.4 | 31 = 40 % items, 22 % tris |
| NORMSPEC (381; 176 k) | 0 | 5.9 | 161.7 | 168 = 44 % items, 3.0 % tris |
| NORMSPEC_MASK (69; 4.9 k) | 0 | 0 | 26.8 | 27 = 39 % |
| FULLBRIGHT (428; 73 k) | 2.5 | 15 | 93.5 | 108 = 25 % items, 12 % tris |
| GLOW (204; 57 k) | 0.8 | 3 | 67.1 | 70 = 34 % items, 12.8 % tris |
| ALPHA_MASK (264; 12 k) | 1 | 5 | 81.5 | 86.5 = 33 % items, 21 % tris |
| ALPHA runs (1,137; 142 k) | 8 | 12 | 495 | 507 = 45 % items, 4.8 % tris |
| **all rigged items** (about 1,330 items including 464 rigged alpha runs; **13.6 M tris**) | 0 | **0** | **0** | **0** |
| **all items** | | 79.6 (18.4 k tris) | 1,538 (85 k tris) | **1,618 items = 27.2 %; 103.7 k tris = 0.69 %** |

- **The cull would drop 27 % of the table items and 0.7 % of the triangles.** Three box runs with three different crowds: 27.2 % / 0.69 % (`box`), 27.2 % / 0.68 % (`box2`), 27.0 % / 0.72 % (`box3`). The first session's census (26 % / 0.59 %, the row-copy-bug build) agrees in both shares.
- **No rigged item is ever dropped**: an avatar's extents box is always on screen and never hidden (31.5 distinct avatars boxed a frame). A per-draw bound for rigged items would need the skinned bounds of each draw, which the viewer does not have. Rigged items are **91 % of the triangles**, so a cull of this kind cannot touch the bulk of the GPU work.
- Frustum: the CPU culls whole groups, not items, so the 79.6 outside items sit in groups it kept (the CPU's own item test says 104.5 outside; the GPU misses 25 of them because its screen rectangle test is conservative, and it never calls an item outside that the CPU calls inside: 0 for every type). The frustum part of B is small (18 k tris).
- The dropped items are small: 85 k tris in 1,538 hidden items is 55 tris each. Savings are draw commands and the vertex stage, not fill. In proportion to triangles (0.7 % of 14.9 M) the gain is about 0.1 ms of GPU; adding the roughly 1,600 of 3,421 commands dropped, **my estimate of B's GPU gain in the plaza is well under 0.3 ms** (an estimate: nothing was dropped to measure it).
- Cost of the measurement: the box test adds about 6,000 boxes to the Hi-Z batch; worker `operations` is 11.8 against 10.6 ms (`b0c0-box` against `b0c0-cen`), absorbed by the `present` wait; fps unchanged.

### 13.6 Is the census code what crashed the earlier exe? (the saved code tested alone)

The hand-off's open item: three default-config runs of the 09:48 exe crashed with a device lost within 1-3 minutes at 10.4-11.2 of 12.84 GB. This tree has the same Hi-Z census changes written again by hand (results buffer 2 x 32768 slots = 6.3 MB, the second dispatch, `mode` / `first` push constants) and nothing else of that tree.

| run | switches | VRAM used (nvidia-smi, whole GPU) median / max, MiB (total 16,311; viewer budget 12,840) | result |
|---|---|---|---|
| `b0c0-off` | none | 10,974 / 11,138 | clean 200 s, no device lost |
| `b0c0-zones` | `FS_CPU_ZONES` | 10,778 / 10,959 | clean |
| `b0c0-cen` | census, no boxes | 10,724 / 10,923 | clean |
| `b0c0-box` | census + boxes | 10,457 / 10,620 | clean |
| `b0c0-box2` | census + boxes | 10,096 / 10,296 | clean |
| `b0c0-box3` | census + boxes | 9,582 / 9,795 | clean |

Six runs, none with a device lost. Each was about 3.5 minutes in the world (login, 40 s settle, 200 s), longer than the 1-3 minutes of the earlier crashes; the first four sat at the VRAM level of the crashed runs (10.4-11.1 GB), the last two a little below it (10.1 and 9.6 GB, the crowd was lighter). **So the census pieces re-applied here, on a tree that has the row-mirror fix (`3c142c98`), did not cause a device lost.** The crashed 09:48 exe was built before that fix. Adam's reading, which fits: the crashes were a combination of the Hi-Z census changes and the table bug fixed shortly before this session. It cannot be dismissed that the table bug only exposed something in the Hi-Z path (the bigger results buffer, the second dispatch, the barriers around it), so a clean run with the fix in does not clear the Hi-Z code. What else differed in the crashed exe is unknown (it had other uncommitted edits; `s7-census-wip` is a snapshot of that tree and I re-applied the Hi-Z, census and box hunks by hand). Not proven gone: a fault that shows once in N runs would not show in this few. If B or C1 builds on the Hi-Z batch, run the validation layers once with the census switches on (`RT_VALIDATE=1 RT_SYNCVAL=1`).

### 13.7 What this means, and the question for Adam

- **GPU and main thread are within about 1 ms of each other** (13.1): in the plain runs of the heavier crowds the GPU sets the frame (99 % utilised, the worker waits in `present`, never for work), in the lightest crowd with diagnostics on the main thread did. So **C's gain is bounded by that gap, about 1 ms (6-7 %), and only in lighter crowds**; in a heavy crowd it is nothing unless the GPU shrinks first. Its whole prize is at most 3.3 ms of main thread, of which about 1.0 ms is the decision, 0.7 the lists and 1.5 the state work that mostly stays; realistically C2 (lists) and the walk are 1.5 ms at best. C also pays on a weaker CPU.
- **B cuts items, not triangles**: 27 % of the draw items, 0.7 % of the triangles, nothing of the rigged bulk. GPU gain estimated well under 0.3 ms with a still camera. Cost: a bounds side array, a compute pass, indirect draws for 20+ pass types.
- **What fills the GPU** (S6-census section 14, older; to re-measure with phases): alpha runs 4.0 ms, ray scene builds 1.3, DLSS 1.3, deferred lighting 2.9, and the rigged geometry (13.6 M triangles a frame, 91 % of all). The levers there are **OIT** (the alpha runs: 1,137 static + 464 rigged items) and the **rigged triangle count** (per-avatar GPU LOD select, section 12.5), neither of which is B or C.
- **Adam's point (2026-10-23): C is groundwork, not an fps item.** The plaza sits on the edge of CPU / GPU balance, so one side alone moves little, but C is what makes multi-threading and balancing the two possible. Read that way, the order is C1 first (shadow mode: the flat group table and a conservative late verdict, compared with the CPU's visible set, no behaviour change): it moves (a), the decision (about 1.0 ms), off the walk. (c), the lists (about 0.7 ms), follows once (a) is a late verdict. (b), the state work (about 1.5 ms), is the real blocker for threading, because its readers (13.4) need the current frame; C3 is where it gets decided. B stays the weak case.
- **Direction from Adam (2026-10-23): balance by moving tasks between CPU and GPU.** Split work so that tasks of similar cost on either side can run on whichever has room, with table copies on both (the CPU copy already exists: `sGeo.mTCpu`, the GPU table is write-combined and unreadable). Notes: (1) the signal exists (`main thread waited for the worker` and `worker waited for work`, 13.1); (2) C1's every-frame comparison is the test harness for two implementations of one decision; (3) good swap candidates cost about the same either way (rigged palettes, alpha sort, LOD select, list production); the visibility decision itself is 1.0 ms on the CPU against about 0.05 ms on the GPU, so it should simply move; (4) the verdict latency differs (GPU 2-4 frames old, CPU current), so the pop-in margin must hold for either source, and the switch needs hysteresis; (5) it keeps both paths as a permanent pair, an exception to the no-old-path rule that needs Adam's explicit OK when it comes to that.
- **Recommendation (by fps alone):** neither B nor C1 first. If one of the two is wanted for its own sake, C1 (shadow mode: the flat group table tested on the GPU, compared with the CPU's visible set) is the cheaper and safer, and its value is structural; B has the weaker case. By these numbers the next GPU-side work is OIT or the rigged triangle count, not the visibility chain. If B is still wanted, first measure it with a moving camera: the 27 % is for a still camera.
- **Open from this session:** (1) UI and name tag flicker Adam reported (one pixel shift or bolder text for a frame; not investigated, noted in the side-session folder, to be looked at after S7); (2) the run-to-run spread of the plaza (64.0-68.4 fps with the same code, by crowd) is wider than the 0.3 ms the hand-off assumes: compare A/B pairs inside one run (`LL_VK_*_AB`), not runs; (3) a B0 run with a moving camera; (4) the "second dip" and `arenaNewBlock` timing from the hand-off, untouched.


## 14. C1 design: the flat group table and a shadow-mode GPU frustum test (2026-10-23; design only, no code; for Adam's OK)

Purpose (13.7): C1 is the first step of making the CPU cull replaceable, and the test bench for any task that either side may run. **C1 changes nothing that is drawn or culled.** The GPU computes a visible set from a flat table of groups; the CPU's own set is recorded; a log compares them every frame. Off by default (`LL_VK_CULL_SHADOW=1`).

### 14.1 What the CPU decides today, and what C1 must reproduce

`LLViewerOctreeCull::traverse` (llvieweroctree.cpp 1477), per node from the partition's root: `earlyFail` (`checkOcclusion`, then drop when the group is `OCCLUDED` and not the root; this also calls `markOccluder`); then, if the parent was fully inside (`mRes == 2`) or the group has `SKIP_FRUSTUM_CHECK`, take everything below without testing; else `frustumCheck` = `AABBInFrustumNoFarClip` on the group bounds `mBounds` and (if not outside) `AABBSphereIntersect` of `mExtents` with the sphere of radius `mFrustumCornerDist` around the camera, the `min` of the two results (0 outside, 1 partial, 2 inside); partial or inside descends. A group that reaches `processGroup` = `markNotCulled` is pushed to the drawable-group or visible-group list (by `mRenderByGroup`). **The CPU set of a frame = the groups of `sCull->beginDrawableGroups()` and `beginVisibleGroups()`** (about 2,500 a frame in the plaza: 287 drawable + 2,218 visible, 13.2). Culler variants: `LLOctreeCullNoFarClip` (no sphere test; used when `mInfiniteFarClip` or no far clip) and the shadow / probe cullers. C1 covers only the main view's `LLOctreeCull` / `NoFarClip` (the world camera, not `sShadowRender`, not `gCubeSnapshot`, not HUD).

### 14.2 The table (one row per group of the covered partitions)

Row (SoA, 64 bytes): bounds center + half (`mBounds`), extents min + max (`mExtents`, for the sphere test), parent row (the octree parent; -1 for a root), flags (root, `SKIP_FRUSTUM_CHECK`, dead, `OCCLUDED` as the CPU holds it), partition id. The row number is the group's id. Covered first: the agent-space partitions that go through `LLSpatialPartition::cull(LLCamera&, bool)` with the world camera: VOLUME (1,323 visible groups a frame), TERRAIN, WATER, TREE, GRASS, PARTICLE. **Not covered:** the BRIDGE and AVATAR partitions (attachments are in bridge-local space, a transformed camera), HUD, the VO cache partition (region-local bounds, `AABBInRegion...`), all secondary views.
- **Building it in C1 (shadow mode): a full walk after `updateCull`** (children by pointer; the cost of a walk, measured and logged; accepted because C1 saves nothing). Row order = parent before child. Uploaded whole each frame through the worker (about 8k rows x 64 B = 0.5 MB; the first log line prints the real group count).
- **Maintained incrementally = C1b**, before any CPU saving: octree listener hooks (`handleInsertion`, `handleRemoval`, `handleChildAddition` / `Removal`, `handleDestruction`) and `rebound()` / `unbound()` mark rows dirty; a free list for rows; delta upload. Not part of C1. The hooks are where the risk is (hot path; the shadow prep threads call `cull` on the pool, but only the main thread may write the table).
- The bounds are recomputed lazily (`rebound()` starts every `cull`), so the build runs after the traversal, not before.

### 14.3 The GPU pass

One compute record (a new worker kind next to kind 34, sharing its ring slots and readback code). Inputs: the table; the frustum of the frame (six planes in agent space, the origin, `mFrustumCornerDist`, the far-clip flag as the CPU culler uses it); the pose of the frame; **three margin factors** (14.4). Per row, one thread computes `frustum(row)` exactly as the CPU does it (plane test on center +- half, then the sphere test), the bounds inflated by the margin, and `pass = frustum && no ancestor-or-self OCCLUDED` (root excepted). The ancestor test: the octree is 6-8 levels deep, so each thread walks its parent chain (8 reads of the flag byte) instead of waiting for the parent's result. The CPU's "fully inside parent takes all below" is only a shortcut: an inside parent implies inside children, so testing every row alone gives the same set.
Output: one bit per row per margin factor (3 x rows / 32 words, about 750 words) in the readback ring, read once the frame's fence has signalled, like `hizRead` (never blocking). Expected cost: 8k rows of a few dozen flops, well under 0.05 ms of GPU (Hi-Z's test of 6,000 boxes plus the pyramid is 0.03-0.05 ms).

### 14.4 The margins (the pop-in policy, tested, not assumed)

A verdict made at the pose of frame N is meant to be applied at frame N + k (k = 2-4, the same latency as Hi-Z). So the GPU must pass every group that can become visible in that time. Per-group inflation of the bounds: `m = t + r * a`, `t` = a bound on camera translation over k frames, `a` = a bound on camera rotation (radians) over k frames, `r` = the group's distance from the camera (known to the shader). `t` and `a` come from the recent camera history (the maximum per-frame translation and rotation over the last 30 frames times (k + 1)) times a safety factor. **Three factors are computed at once** (for example 1.0, 1.5, 2.5), so one run shows which is enough. A margin of 0 gives the exact test (for a still camera it must equal the CPU set).

### 14.5 The comparison and the log

At the end of `updateCull` the CPU records its set as a bitset over rows (groups not in the table are counted separately), with the frame number, in a ring of 8 keyed by the record's serial. When a verdict is read (frame N + k) it is compared with the CPU set of the frame **it would be applied to**, N + k, not N: that is the pop-in test. Every 600 frames, per margin factor: GPU set size, CPU set size, **missed** (in the CPU set of N + k, not in the GPU set of N) split by cause (a group created after N: no row yet; a group whose bounds changed since N; the rest = camera motion), **extra** (in the GPU set, not in the CPU set of N + k; this is the cost: each extra group carries state work, about 1 microsecond a group, from 13.2: (b) + (c) = 2.2 ms for 2,218 groups), the actual latency k (min / median / max), table rows, build ms, upload bytes, the camera speed bounds used, and the worker ms of the record (`LL_VK_OPSTATS`; no GPU zone timers).

### 14.6 Gate, and what is not decided

- Gate for C1 (switch on, compared with a plain run of the same crowd): **missed = 0 at the chosen margin over 5 minutes with the camera turning and walking** (`<work>\rt-flash-scan.ps1` already holds the left arrow for a scripted turn through `keybd_event`; walking forward and a fast turn need the same trick); for a still camera the exact set equals the CPU set; **extra** at the chosen margin a small share (aim under 10 % of the CPU set, about +0.2 ms of state work if applied); build + upload + record under 0.3 ms of main thread in total (it is a full walk, so more than the incremental C1b will cost); no device lost over 3 runs; validation layers clean once (`RT_VALIDATE=1 RT_SYNCVAL=1`).
- Not decided here: whether `OCCLUDED` stays an input flag (C1) or is computed on the GPU from the Hi-Z results of the same batch (a later step; the rows' boxes are the Hi-Z group boxes); how bridges and avatars join the table (C4); and the policy for groups the GPU does not know (created since the verdict): the real design must treat them as visible until the next upload.
- Size of the work: table build and the CPU set record in pipeline.cpp (about 150 lines), a new worker kind with one shader (about 200 lines, modeled on `on_hiz_boxes`), the log. About a day; no behaviour change.


## 15. C1 results: the flat group table and the shadow-mode GPU frustum test (2026-10-23)

Built as designed in section 14, measure only, off by default. **Switches:** `LL_VK_CULL_SHADOW=1` (table, GPU test, comparison log, tag `CullShadow`), `LL_VK_CULL_FACTORS=a,b,c` (the three margin factors, default 1, 1.5, 2.5), `LL_VK_POSE_LOG=1` (one log line a second with the camera's yaw, for the run scripts). **Code:** `llcullshadow.h`; `LLCullShadow::build / send / poseLog` and the comparison in `pipeline.cpp` (hooks: end of the main view's `updateCull`, and next to `LLHiz::send` in `doOcclusion`); the group fields `mCullId / mCullFirstSerial / mCullCpuStamp` in `LLViewerOctreeGroup`; worker kind 35 (`on_cull_rows`, compute shader `kCullTestShader`, 32 threads a group, 4 bit sets per record) and the ring functions `cullSend / cullRead / cullDone` in `llvkbridge.*`; `LLCamera::getPlaneCount()`. Runs: `<work>\c1-chain.ps1` (names `still*`, `move*`, `*plain*` = no diagnostic, `*settle*` = 300 s with the last 80 s still), `<work>\rt-run-move.ps1` (arrow keys: turn left 4 s, right 4 s, walk forward 3 s, back 3 s, taps; at the end it turns back to the yaw it started with, which the viewer logs).

### 15.1 What the table and the test are

Covered partitions: every agent-space partition of every region except HUD, bridge, avatar, control avatar, HUD particle and VO cache; one row (64 bytes) per octree node, parent before child. Facing the crowd (`still4`): **7,771 rows, of which 1,901 are empty nodes** (the cull never keeps an empty group: `markNotCulled` returns); the CPU keeps 1,445 groups; **3,246 non-empty groups are inside the frustum** (the GPU test and the CPU's own exact test agree on that set to 0.6 groups of 3,246). So the CPU's occlusion state removes 56 % of the frustum set (Hi-Z is the larger cull; the frustum is the smaller part).
Per record: the table is built by a full walk after `updateCull` (0.90 ms for 7.8k rows, 2.5 ms for 16.8k rows: about 0.11-0.15 microseconds a row), copied (0.46-1.1 MB, 0.06-0.34 ms) and tested by one compute dispatch; the worker spends **0.036 ms a frame** on the record (36 microseconds, op stats). **Latency between the record and its comparison depends on the queue depth**: `still4` (GPU-limited, 99 % utilisation) 4 frames in 49 % of the records and 5 in 35 %, 2-3 in 16 %; `movesettle1` (main-thread-limited, GPU 53-65 %) 2 frames in 67 %, 3 in 29 %, 4-5 in 4.5 %. The margin bound (5 frames of the largest per-frame motion of the last 30) covers both; a margin that used the measured latency would be tighter in the main-thread-limited case, and the still run needs no margin at all. The comparison (0.5-1.1 ms) is diagnostic only and is not part of the cost of the design.

### 15.2 Runs (all with `LL_VK_CULL_SHADOW=1` unless noted; fps are NOT comparable between rows)

| run | what | notes |
|---|---|---|
| `c1-still1`, `c1-move1`, `c1-still2`, `c1-move2` | first builds (empties not excluded, no frustum-only metric) | gave the first findings (14.x bugs below); not used for numbers |
| `c1-still3`, `c1-move3` | **not controls**: the avatar was left facing away from the crowd by the moving run before (Adam's note); 96 fps, 1,100 groups kept | cull comparison valid, fps and group counts not representative |
| `c1-valid1` | validation + sync validation (`RT_VALIDATE=1 RT_SYNCVAL=1`), 150 s | no message names kind 35 or the cull dispatch; the 24 messages are the old ones (UI shader copy hazard, shadow pull writes, avatar alpha pipeline) |
| `c1-still4` | still camera, facing the crowd (set by Adam) | the numbers below |
| `c1-moveplainsettle1` | moving, **no cull diagnostic**, `LL_VK_POSE_LOG=1` only, last 80 s still | engine reference for the moving case |
| `c1-movesettle1` | moving, diagnostic on, last 80 s still | the moving numbers below |

**No device lost in any of the 10 C1 runs** (the Hi-Z census runs of section 13 had 6 more). Yaw return worked (start -0.88, end -0.84; start -0.92, end -0.89).

### 15.3 Result: the GPU test against the CPU

Frustum only (what the GPU would supply; the CPU keeps applying its own `OCCLUDED` state on top), against the CPU's exact frustum test of every group at the pose of the frame the verdict is compared with (2-3 frames after the record):

| case | margin factor | GPU set | CPU frustum set | MISSED a frame | EXTRA a frame |
|---|---|---|---|---|---|
| still (`still4`, 18 windows) | any | 3,246 | 3,246 | 0.60 (0.018 %) | 1 (0.0 %) |
| moving (`movesettle1`, 14 windows, camera bounds over 5 frames: 0.59 m, 0.142 rad) | exact (0) | 3,131 | 3,130 | **68.5 (2.2 %)** | 69 |
| | 0.25 | 3,454 | 3,130 | 6.2 (0.20 %) | 330 (10.5 %) |
| | 0.5 | 3,749 | 3,130 | 0.55 (0.018 %) | 620 (19.8 %) |
| | 1.0 | 4,267 | 3,130 | **0.03 (0.001 %)** | 1,137 (36 %) |
| settled after the motion (4 windows) | 0.25 / 0.5 / 1.0 | 3,269 / 3,272 / 3,285 | 3,265 | 0.05 | 5 / 7 / 21 (0.1-0.6 %) |

- **Without a margin an old verdict would miss 2.2 % of the groups while turning** (68 groups a frame at the screen edge): the margin is needed. At factor 1 the miss is one group every 30 frames in the fastest turn, zero when still; at factor 0.5 it is one group a frame in the fastest turn windows (0.018 %), the fastest window of `move3` had 5 groups a frame at factor 0.5 and 0.2 at factor 1.
- **The price is extra groups only while the camera turns**: +20 % (factor 0.5) to +36 % (factor 1) of the frustum set during a turn at about 0.14 rad over 5 frames (a fast turn), +0.1-0.6 % once it stops. The margin is `t + distance * a` per group, so most of the extra is far groups (a 0.14 rad bound is 28 m at 200 m).
- **Against the CPU's final kept set** (after its occlusion state and its own quirks) at factor 0.5 in the moving windows: 2.98 missed, of which 2.65 are the far-limit quirk below, 0.28 camera motion, 0.04 occlusion flag changes; at factor 1: 2.47, of which 2.42 the quirk and 0.01 real.

### 15.4 Findings (bugs, waste, room to improve) found by the comparison

1. **The stock cull keeps groups beyond the far limit.** `AABBSphereIntersectR2` (llvieweroctree.cpp 192) returns "2, fully inside" when only the minimum and maximum corners of the extents are within the radius, and `LLViewerOctreeCull::traverse` then takes everything below without testing. Groups at 256.2 m with a far limit of 256 m (the view distance) stay in the CPU set: **about 23 groups a frame (1.6 % of the kept set) in the crowd facing run, 7 while turning**, which the far plane clips anyway. A fix (test all eight corners, or the farthest corner) is small and cuts the cull, state work and render map by those groups. Not done (it changes the CPU cull; Adam's call). The GPU test does not have the quirk.
2. **A quarter of the octree nodes are empty** (1,901 of 7,771; 4,165 of 16,840). They are visited and tested every frame and the cull throws them away in `markNotCulled`. The flat table needs them as parents only.
3. **The table size moves a lot between runs** (7.1k to 17.5k rows in the same sim, 17k in both moving runs) while the CPU kept set stays 1,100-1,450: not explained (neighbouring region groups? object churn?). The cost of the full walk follows the row count. C1b (incremental table) removes it.
4. **Occlusion state flips cost misses in the CPU's own set**: while turning, 1-40 groups a frame were held OCCLUDED at the record and are kept by the CPU 2-3 frames later (the Hi-Z verdict lag). Not a defect of the GPU test; the reason the design supplies only the frustum.
5. **Texture priorities and the discard run before the frame's cull** (display(): `updateImages` before `updateCull`; idle(): object texture sizing): they read the previous frame's visibility stamps. See the notes in the side-session folder (`texture-discard-at-ram-limit.md`) for the cumulative slowdown, the thrash at the VRAM budget and the proposed test; not a C1 item.
6. **GPU utilisation after a 360 degree turn** (nvidia-smi, 1 Hz): still 99 %, VRAM 10.1 GB. Plain moving run: 62-73 % while turning with VRAM at 12.5-12.7 GB (the 12,840 MB budget), **87-88 % in the 80 s still tail**, VRAM unchanged at 12.4-12.6 GB. With the cull diagnostic: 53-59 % moving, 62-65 % in the tail (the diagnostic costs the main thread 2.5 + 1.1 ms at 17k rows). fps (median, 10 s samples): still 66.9, plain moving 58.5 (31-74), diagnostic moving 48.2. Adam saw hair and clothes textures lose, gain and lose their texture again at the budget edge, and many things in view still without textures after 80 s.

### 15.5 Gate of section 14.6, and what is left

- Pop-in: **not exactly zero** at any factor in the fastest turns (0.03 groups a frame at factor 1, one group 2-3 frames late at the screen edge every 30 frames); zero when the camera is still. A group whose verdict is late shows one frame late at the edge. The decision for Adam: factor 1 (almost no misses, +36 % groups while turning fast) or 0.5 (one late group a frame in a fast turn, +20 %), or a margin made of the camera's actual speed this frame instead of the maximum of 30 frames.
- Extra under 10 % of the set: yes when still (0.0-0.6 %); no while turning fast (20-36 % at the factors that avoid misses).
- Cost: the record is cheap (0.036 ms of worker a frame, a few tens of microseconds of GPU); the **table build (full walk) is not**: 0.9-2.5 ms. **C1b, the incremental table (listener hooks, free list, delta upload), is the next step before anything is saved**; without it C1 only costs.
- Validation: clean for the cull code. Device lost: none in 10 runs.
- Decisions for Adam: (1) the margin (factor and the speed source), (2) fix the stock far-limit quirk in the CPU cull (about 1.6 % of the kept groups, low risk), (3) go on with C1b, or look at the texture / VRAM budget thrash first since it costs 25-40 points of GPU utilisation in the moving case (15.4 item 6).


## 16. C1b design: the group table kept up to date by the groups themselves (2026-10-23)

Why: section 15 measured the full walk that builds the table each frame at 0.9 ms (7.8k rows) to 2.5 ms (16.8k rows); until it is gone C1 only costs. C1b keeps one CPU mirror of the rows, a stable slot per group, and updates only the rows that changed. The GPU still receives the whole mirror each frame in this step (0.5-1 MB, 0.06-0.34 ms measured in 15.1); a resident table with delta records is a later step (C1c) once the mirror is proven.

### 16.1 What changes a row, and where it is hooked (all in llvieweroctree.cpp, behind `LL_VK_CULL_SHADOW`)

| row field | changes in | hook |
|---|---|---|
| bounds, extents | only `LLViewerOctreeGroup::rebound()`, which runs after `unbound()` marked the node and every ancestor `DIRTY` | `unbound()` touches the node and each ancestor it marks; a row is refreshed only when its group is no longer `DIRTY` (otherwise it stays in the list) |
| `SKIP_FRUSTUM_CHECK` | the parent's `rebound()` (3 sites: set on a single child, cleared on children) | touch the child when the flag actually changes |
| empty | element insert / remove (`handleInsertion`, `handleRemoval`, `removeFromGroup`) | all call `unbound()` |
| parent link | node creation and removal (`handleChildAddition` / `Removal`, both call `unbound()`) | the new group is registered in its constructor (`added`), its row is built at the next processing |
| `OCCLUDED` (world camera) | `setOcclusionState` / `clearOcclusionState` (single and all-cameras modes; the branch modes call them per group) | touch when the world camera's `OCCLUDED` bit toggles |
| dead | `handleDestruction` (flag `DEAD`), the destructor | the destructor frees the slot (generation + 1, row marked dead, slot on the free list) |

`mCullId` becomes slot + 1 (a stable id, no map). The touch list is drained once per frame at the end of `updateCull` (main thread; touches may come from the shadow prep pool, so list and registry are behind one mutex). Only the partitions of 14.2 are registered (`dynamic_cast` to `LLSpatialGroup` and a check of the partition type at the first processing; the VO cache groups are not).
Dead and free rows carry flags 33 (dead + root) and are ignored by the shader and the comparison. The comparison now names a group by slot and generation, so a reused slot is not compared with the wrong group.

### 16.2 The test of the hooks: `LL_VK_CULL_VERIFY=1`

Every 60 frames the old full walk runs next to the mirror and the two are compared group by group: groups the walk finds without a row, rows without a group in the walk, and differences in bounds, extents, parent, and each flag bit. The log line `CullVerify` gives the counts over the window. Gate: all counts 0 over a moving run (a hook that is missing shows up here before it shows up as a miss in the GPU comparison). `LL_VK_CULL_FULL=1` keeps the full walk as the table source (the C1 behaviour) for A/B timing.

### 16.3 Gate

Verify counts 0 in the still run and the moving run; build time per frame (the touch list, no walk) well under 0.3 ms at 17k rows; the GPU comparison of 15.3 unchanged (missed / extra within noise of the full-walk numbers); no device lost; validation clean once.

### 16.4 Results (2026-10-23, runs `c1-stillverify1/2`, `c1-moveverify1/2`; the plaza facing the crowd, moving = the arrow-key script of 15.2)

**Built as designed** (switches: `LL_VK_CULL_SHADOW=1`, `LL_VK_CULL_VERIFY=1`; the C1 full-walk table source was removed, the walk now exists only as the verify check). Files: `llcullshadow.h`; the hooks in `llvieweroctree.cpp` (constructor, destructor, `unbound`, `rebound` skip flag sites, `handleDestruction`, `setOcclusionState` / `clearOcclusionState`, and the new `handleParentChange`); `lloctree.h` (new listener callback, below); the mirror, slot registry, `cull_process`, `cull_verify` in `pipeline.cpp`; the shader ignores dead rows (flag 32). No device lost in these 4 runs.

**The test found a hook that did not exist.** The first verify runs (`...verify1`) matched on bounds, extents, every flag, row presence and orphans (all 0 over 20 full walks), and differed only in the parent link: 3-5 rows per walk in some windows. Cause: `LLOctreeRoot::balance` (a root with one empty branch child takes that child's children, `addChild(child, true)`, silent) and the root's expansion in `LLOctreeRoot::insert` (`newnode->addChild(child)` before the new node has listeners) change a node's parent without telling any listener, and `setParent` has no hook. Fix: `LLOctreeListener::handleParentChange(node)` (default empty) called from `addChild` for the child's listeners; `LLViewerOctreeGroup` touches its row. After it: **0 differences in every field over 44 report windows (about 440 full-walk comparisons) in the still and the moving runs** (`...verify2`: 24 and 20 windows, 8.4k and 17.4k rows).

| | full walk (C1, 15.1) | kept table (C1b) |
|---|---|---|
| rows refreshed a frame | all (7.8k-17k) | **38 still, 124 turning** (touches 38-125: moving objects, avatars' groups, occlusion flips, rebounds) |
| table build (the part that was the walk) | 0.90 ms (7.8k rows), 2.5 ms (16.8k rows) | **0.003 ms still, 0.009 ms turning** (whole `build` incl. the mirror copy, the CPU set stamps and the camera bounds: 0.11 / 0.18 ms) |
| `send` (copy of the whole table into the record) | 0.06-0.34 ms | 0.10 ms still, 0.37 ms turning (unchanged: the table is still sent whole) |
| rows put back for a pending rebound | - | 0 |
| groups added / freed a frame | - | 0.25 still, 1-2 turning |

- GPU comparison unchanged (frustum-only, against the CPU's exact test): still 3,250 vs 3,250 (missed 0.06 a frame, 0.002 %, down from 0.6 in 15.3: the old walk saw a changed parent only by luck); turning (0.44 m, 0.109 rad over 5 frames): exact misses 63 (2.07 %), factor 0.25: 8.3 (0.27 %) +288 extra (9.4 %), factor 0.5: **0.5 (0.016 %)** +533 (17.4 %), factor 1: **0.06 (0.002 %)** +982 (32 %). Same as 15.3 within the spread between runs.
- The verify walk itself costs 1.3 ms (8k rows) to 3.9 ms (17k rows): only in verify runs, every 60 frames.
- What is left of the cost: the two copies of the whole table (into the record's row list for the comparison, and into the queue blob by `cullSend`), 0.1-0.37 ms. fps in these runs are not comparable with section 15 (the crowd differs; `stillverify2` 73.5 fps, 13.3 ms, `moveverify2` 62.5 fps).

### 16.5 Next, and what it opens

- **C1c: resident table with delta records.** The worker keeps the table in a GPU buffer; the viewer sends only the refreshed rows (38-124 rows, 2.4-8 KB) and a few new-slot / dead-slot rows; the comparison keeps its own copy only in verify mode. That takes `send` from 0.1-0.37 ms to a few microseconds and the whole C1 cost to about the 0.04 ms of the dispatch and the 0.003-0.009 ms of the touched rows. The same table is what the Hi-Z group boxes use (`hizCollect` builds its box batch from the occlusion list each frame), so it can feed that too.
- **C2: use the verdict.** The GPU frustum set (with factor 0.5-1) replaces the CPU octree walk for the covered partitions; the CPU applies its own `OCCLUDED` flags on top (the table already carries them) and runs state work only for the groups left. The fastest CPU saving in 13.2 is the decision (a) at about 1.0 ms and the part of (c) fed by the groups it drops.
- Decisions for Adam: margin factor (0.5 or 1, or the camera's speed this frame), fix of the stock far-limit quirk (15.4), and whether C1c goes first or C2 directly on the whole-table send (0.1-0.37 ms) as an interim.
- Scene drift to know about (Adam, 2026-10-23): the plaza got brighter during the day (mean screenshot luminance 44 in the morning, about 90 by 13:00) although the viewer's environment is fixed midnight: **lights in the sim run on the sim's own clock** and turn on over time. The nearby light count in the log went from 580 (morning) to 670-740. So the lighting cost, and fps, drift with sim time on top of the crowd (13.1); compare runs only inside one session or the same hour, and do not read screenshot brightness as a rendering regression.


## 17. C1c: delta rows to a worker-side table (2026-10-23)

**Built, shadow mode, default off** (`LL_VK_CULL_SHADOW=1`; `LL_VK_CULL_COMPARE=0` leaves the comparison with the CPU set out, which is what a use of the verdict would cost). One change from the plan of 15.5: the worker keeps the table in **its own memory** and uploads all of it into the frame's scratch for the test, instead of a GPU-resident buffer with deltas. A resident buffer is read by frames in flight and every touched row would have to be versioned against them (hand-off trap, section 4); the worker copy gives the main thread what was needed, and the whole-table copy stays on the worker, which is not the limiter (0.037 ms a frame, below).

**Record (kind 35):** `CullParams`, then `slots[n]` (padded to 16 bytes), then `rows[n]`; `mMatrices` = rows of the whole table after the delta; `mPaletteFloats` = 1 starts the worker's copy afresh (the first record, and after any record `cullSend` refused: ring full, no worker). The viewer collects the changed slots (every refreshed row, every freed slot) in `sSendSlots`, sorts and uniques them at the send and puts the newest row of each. The worker applies the delta to its copy before anything else, so a record whose test cannot run (no scratch, too many rows) still leaves the copy right for the next one. Files: `llvkbridge.*` (`cullSend(params, slots, rows, n, table_size, reset)`, `cullRead(serial, table_size, out)`), `on_cull_rows` in `llvkworker.cpp`, `LLCullShadow::send` and `cull_drain` in `pipeline.cpp`.

**Results** (runs `c1-stillverify3`, `c1-moveverify3` with the comparison; `c1-stillnocmp1`, `c1-movenocmp1` without; same plaza, the arrow-key script of 15.2; no device lost in the 4 runs; the verify walk of 16.2 was on in the first two):

| main thread, per frame | C1 full walk (15.1) | C1b whole table sent (16.4) | **C1c, comparison off** | C1c, comparison on |
|---|---|---|---|---|
| table build (touched rows made again, stamps, camera bounds) | 0.90 ms (7.8k rows), 2.5 ms (16.8k) | 0.11 ms still, 0.18 ms turning | **0.008 ms still, 0.013 ms turning** (touched rows 0.004 / 0.009) | 0.011 / 0.017 |
| send | 0.06-0.34 ms | 0.10 / 0.37 ms | **0.004 ms still, 0.006 ms turning** | 0.03 / 0.17 (it copies the table for the comparison) |
| bytes to the worker a record | 0.46-1.1 MB | 0.46-1.1 MB | **3-4 KB still, 8.7 KB turning** (36-60 and 126 delta rows) | same |
| comparison (diagnostic) | 0.5-1.1 ms | same | 0 | 0.28 / 0.59 ms |
| **total for the verdict's input** | 1.0-2.9 ms | 0.2-0.55 ms | **0.012 ms still, 0.019 ms turning** | |

- Worker: kind 35 costs **0.037 ms a frame** (37 microseconds a record: the table copy into scratch and the dispatch), unchanged. One reset in 15,600 records (the first), 0 bad slots, the worker's table has 7.3k rows.
- GPU comparison unchanged by the move to delta rows (frustum-only against the CPU's exact test): still 3,130 vs 3,130 (exact: 0.56 missed a frame, 0.018 %; with margin 0.25, 0.5 or 1: 0.00 missed, +7 / +15 / +26 extra); turning (17.5k rows): exact misses 56 (1.8 %), factor 0.25: 7.1 (0.23 %), factor 0.5: **0.51 (0.017 %)**, factor 1: **0.04 (0.001 %)**, extra 245 / 449 / 839. This is also the check of the worker's copy: if a delta had been lost or applied to the wrong slot the misses would grow while the camera stands still.
- **The cost of the whole C1 chain (table, test input) is now about 0.01-0.02 ms of main thread and 0.04 ms of worker, against 1.0-2.9 ms of main thread for the full walk it started from.**

**Next: C2, using the verdict** (switch default off, the old walk stays; `LL_VK_CULL_VERIFY`-style comparison stays as the gate). Decisions open: the margin factor (0.5: one late group a frame in the fastest turn; 1: almost none, 839 extra groups of 3,108 in that turn), the stock far-limit quirk (15.4 item 1), and which consumers read the GPU set first (render lists of the VOLUME partition, as 12.4 C2 says; the state work (b) of 13.2 stays on the CPU for the groups the set names).


## 18. C2 design: the main view's cull from the GPU verdict (2026-10-23)

Adam's decision (2026-10-23): C2 is structural: it is what moves the visibility decision off the octree walk and prepares the balancing of CPU and GPU. Expected main-thread gain 0.4-0.6 ms at best (about 3-4 %), fps only where the main thread limits (13.7, the answer of this session). Default off, the old walk stays as fallback and as the gate.

### 18.1 What the walk does per node, and what the flat version must keep

`LLViewerOctreeCull::traverse` / `LLOctreeCull`: per visited node `earlyFail` = `checkOcclusion()` (two side effects: a group under an `OCCLUDED` parent forgets its awaited Hi-Z verdict; a group that asked to be discarded while its verdict was awaited, `mHizStale`, clears `OCCLUDED` for its branch) then, when `OCCLUDED` and not the root, `markOccluder(group)` (the topmost occluded group goes to the occlusion list: **this is what makes Hi-Z ask again**) and the subtree is skipped; else the frustum test, descent on partial / inside; every group that passes gets `markNotCulled` (empty groups return at once; `setVisible`, `updateDistance`, push to the drawable-group or visible-group list, and to the occlusion list when it needs an update or was not visible last frame). Before the walk `LLSpatialPartition::cull` calls `rebound()` on the root.

### 18.2 The flat version (`LL_VK_CULL_USE`, needs `LL_VK_CULL_SHADOW=1`)

`LL_VK_CULL_USE=1` **derive**: the walk runs as now and is what the pipeline uses; the flat version runs after it and its set is compared with the walk's (the gate). `LL_VK_CULL_USE=2` **use**: the covered partitions are not walked; the flat version produces their lists. `LL_VK_CULL_USE_VARIANT=1..3` picks the margin variant of the verdict (default 2).
Input: the newest verdict read (a bit set per variant over slots, its slot generations) and the kept table (`sMirror`, whose flags are the CPU's own `OCCLUDED`, skip, empty, dead state as of the last hook). A row's verdict bit is **unknown** (counts as passed) when the group did not exist in that record (slot beyond its size or another generation).
One pass over the rows with a memo, parent state first:
- state of a row, given its parent's state (the root's parent counts as passed): parent not passed -> FAIL; row `OCCLUDED` and not the root -> **TOP-OCCLUDED**; else verdict bit set -> PASS, clear -> FAIL. A row whose parent is TOP-OCCLUDED or deeper is skipped (the walk never visits it).
- **kept** = PASS, not empty, not dead, and its partition's render type is enabled (`hasRenderType`, as the walk's partition loop does: the row carries the partition's drawable type). For kept groups: `checkOcclusion()` (the state may flip), re-read `OCCLUDED`, then `markNotCulled`.
- **TOP-OCCLUDED** rows: `checkOcclusion()` (a stale verdict clears the branch: then the group counts as kept after all), else `markOccluder(group)`.
- Pointer access only for the kept and top-occluded groups (about 1,400 + a few hundred of the 8k rows): the rest is the flat table.
Roots: `rebound()` of each covered partition's root stays (it is what `LLSpatialPartition::cull` did first); the touched rows are refreshed (`cull_process`) before the pass.
Not covered, still walked: bridge, avatar, control avatar, HUD, HUD particle, VO cache; every cull of a secondary view (cascades, probes, snapshots, mirrors).

### 18.3 Gate and measurements

Derive mode: the flat kept set against the walk's kept set, per frame, with the causes (a group beyond the far limit: the stock sphere quirk; bounds changed; occlusion state changed; the rest = motion): missed must be ~0 at the chosen variant (like 15.3), extra within the margin's cost. Use mode: no device lost, the Hi-Z numbers (groups occluded a frame, probes) unchanged against derive runs, pictures (scripted turns) without missing groups, `updateCull` lap and zone times against a run with the walk, fps A/B only in the same session (13.1). Valid only for the main view's world pass; the walk comes back by itself while no verdict has been read yet (first frames, a refused record).

### 18.4 Results (2026-10-23, runs `c1-stillderive1`, `c1-movederive1` (derive), `c1-stilllapcuse1`, `c1-movelapcuse1` (use, comparison on), `c1-stilllapplain2`, `c1-movelapplain2` (plain walk, laps), `c1-stilllapcusenocmp1`, `c1-movelapcusenocmp1` (use, comparison off, laps); margin variant 3 = factor 1; no device lost in the 8 runs)

**Derive mode** (the flat set next to the walk, margin factor 1): still: the flat pass keeps 1,526 groups and 636 topmost occluded ones in 0.165 ms; the walk keeps 1,507; both 1,495, **missed 12 (11 of them beyond the far limit: the stock sphere quirk of 15.4), extra 30** (the margin, as in 15.3); turning: kept 1.4k-2.4k, missed 1-5 (mostly far limit), extra 240-930 (the margin), 0.11-0.23 ms.

**Use mode against the plain walk, same session, laps (`LL_VK_VISCENSUS=1`), comparison off**, main thread, ms a frame:

| | still, walk | still, C2 | turning, walk | turning, C2 |
|---|---|---|---|---|
| `updateCull` | 0.644 | 0.649 | 0.905 | 1.051 |
| `stateSort` (includes `postSort`) | 1.541 | 1.764 | 1.840 | 2.325 |
| `postSort` (render map) | 0.781 (0.558) | 0.886 (0.622) | 1.122 (0.549) | 1.505 (0.740) |
| visible groups a frame | 1,807 | 1,869 (+3 %) | 1,447 | 1,866 (+29 %) |
| the flat pass itself (`cull_flat`) | - | 0.43 | - | 0.61 |
| fps median (10 s samples) | 72.4 | 72.2 | 61.4 | 56.8 |

(The first use runs, with the comparison on, showed `updateCull` 0.85 / 1.67 ms: the end-of-frame build and the comparison with the CPU set sit inside that lap.)

**Conclusion: C2 as built does not pay on the main thread in the plaza.** `updateCull` does not shrink: the flat pass (0.43-0.61 ms) costs about what the walk it replaces cost, because what the walk spent was mostly per-group work that C2 still has to do for about 2,000 groups a frame (1,300-1,460 kept + 595-670 topmost occluded: `checkOcclusion`, `markNotCulled`, `markOccluder`, each a pointer access), and not the traversal itself; the estimate of 0.4-0.6 ms in section 18 and in the answer to Adam was too high. **The verdict's margin costs state work while turning**: +29 % groups kept at factor 1 give +0.5 ms in `stateSort` and `postSort`. Correctness is not the problem: derive matched the walk within the margin and the far-limit quirk, use mode ran clean 4 times.
What would still cut it, if it is ever wanted: skip the pointer access for topmost occluded groups that are already `ACTIVE_OCCLUSION` (a row flag set by the hooks), run the classification (0.17 ms of the 0.43) on a worker thread, a tighter margin (factor 0.5: +17 % groups). Realistic result: 0.1-0.2 ms, not worth the extra moving parts now.
Status: `LL_VK_CULL_USE=1|2` stays in the tree, default off, verified; the C1 chain (table, delta rows, GPU test, comparison) stays as groundwork for the balancing direction of 13.7. Recommendation: do not enable C2; the bigger lever found on the way is the texture / VRAM budget thrash after turns (15.4 item 6, the side-session notes).


## 19. Texture downscale without white (unfocus downgrade)

**Problem.** After the unfocus downgrade (`TextureDiscardBackgroundedTime`), textures turned white. Found by A/B: `LL_VK_NO_SCALEDOWN=1` (skip `LLViewerLODTexture::scaleDown`) gave no white; with it on, about 2.8% of the 3D view went white for about 10 s after refocus (`tex-unfocus2`), and the same textures stay white after a dump recovery. The copies themselves ran and none were dropped (`VkTexCopy` counters), so the white is in what `LLImageTex::scaleDown` produces: it draws the texture small into a render target and copies that back (`copySubImage`) before `generateMipmap`.

**Fix.** A texture that holds its whole mip chain gives up its top k levels by copying levels k.. into a new, smaller image (`OPK_DROP_LEVELS`, `LLVkTex::dropLevels`, worker `drop_levels`, `Renderer::copyMipLevels`). No draw, no render target. The worker refuses (leaves the image as it was, counted as 'drops refused') when the texture is not plain 2D, is compressed or depth, or does not hold its full chain; the viewer does not fall back then, so the texture just keeps its larger image (no white, no memory freed). None were refused in the runs. On by default; `LL_VK_SCALEDOWN_LEVELS=0` restores the old path. `LLImageTex::scaleDown` otherwise keeps its behavior (alloc, options dirty, `mCurrentDiscardLevel`).

**Result (unfocus test, 60 s away, same scene).**

| run | white pixels unfocused | after refocus | note |
|---|---|---|---|
| `tex-unfocus2` old path | 2.8% | 0.04% after 10 s | |
| `tex-unfocus3` no scaledown | 0.01% | 0.01% | |
| `tex-unfocus5` level drop (env) | 0.01% | 0.01% | 4923 drops, 0 refused |
| `tex-unfocus6` level drop (default) | 0.28% | 0.28% | a ground light pool that switched on, not a texture |

VRAM still falls about 1 GB on unfocus (9.1 to 8.1 GB) and returns after refocus. Not tested: changing the max texture resolution in Preferences (it uses the same function), and a turn run.

**Blink after load (follow-up to section 19).** Counters in `packet_texture_slot` (logged with `VkTexCopy`) count draw samples of a texture that was drawn with its picture before and now has a deleted name, no image, or an image with no level yet. Turn run `tex-moveb1` (4 min, login included): 16 samples of a deleted name, 0 without an image, 0 blank, none in the first-load window. So the one-frame white/transparent blink did not come from these cases; Adam saw no blink in turns or dumps with the level-drop fix. During turns the budget moves textures a lot: 43,483 level drops in the run (about 180/s), with no visible cost: A/B against the old path (`tex-moveb2`, `LL_VK_SCALEDOWN_LEVELS=0`): fps median 57.6 vs 61.1 (ranges 43-68 vs 45-81, crowd noise), worker ops 0.25 vs 0.71 ms/frame, shadowPackets 4.1 vs 4.4 ms. Not chased: why the budget ping-pongs that much while turning.

## 20. Texture budget over time and the GPU that stays at 70-80% after a turn

Run `tex-settleb1` (login, about 160 s of turning and moving, 80 s standing still), level-drop on, `LL_VK_BUDGET_LOG=1` (new: `VkBudget` log line every 2 s: used / budget / target / over / bias / texture bytes). Same run's `nvidia-smi` and `VkPhases` lines.

**The budget does not oscillate; the bias climbs and sticks.** Budget 12840 MB, target 11556 MB (90%). `used` (what VK_EXT_memory_budget says the process holds) rises from 2 GB at login to 11.3-11.5 GB and then sits 0.6-5% under the target. Only 9 of 143 samples were over the target (largest +2.3%). Each crossing slams the bias to 1.5 and ramps it; it ended at 1.92 and stayed there through the whole settle, because the bias only comes down when `used` is more than 10% under the target (11.4 GB is 1-2% under). So after the first crossing the texture quality stays about one discard level lower than the memory needs, for as long as the scene needs that much memory. `used` is not textures: the viewer's own count of texture bytes was 6.4 GB of it; the rest is the ray-tracing structures (about 1 GB at a time, 255 arena blocks created in the run), buffers and targets.

**The per-texture churn is not the budget either.** The `VkTexCopy` counters show 20-200 level drops and grows per 5 s while standing still, with bursts of thousands when the camera turns (the desired discard of each texture follows the camera and the bias), not a loop between "over" and "under".

**The 70-80% GPU is the worker thread, not the GPU.** In the settle phase: frame 17.4 ms (57 fps), worker operations 14-16 ms/frame, worker waited for work about 3 ms, GPU time per frame 12-16.7 ms (so the GPU is busy about 75% of 17 ms). Early in the run (looking away from the crowd): 12-13.6 ms, 73-79 fps, GPU 99%. What grew: draws per frame 6,000-6,700 to 9,000, `shadowPackets` 3.4 to 8 ms, ray scene CPU 0.76 to 1.19 ms/call, skin jobs 123 to 354/frame, avatars drawn 4 to 7/frame, casters named 1243 to 3885/frame. Cost per draw stays about 2 us (it is 2.5-3.3 us during movement), so the extra time follows how much is in view and animating, not a slower worker. The run ends facing the crowd again, the first minute faced away from it; the still runs facing the crowd (`tex-unfocus5/6`) show 6,300-6,850 draws, 13-14.5 ms, 99% GPU. So this run cannot say whether the extra work is the crowd moving or a state left behind by the turn.

Not done: a controlled pair (same view, same time, with and without a turn) to separate crowd from leftover state; the bias hysteresis (counting only textures, or an exit threshold nearer the target); the ray-structure churn.

**Controlled pair (section 20 addendum): 60 s still, 60 s turning in place, 120 s still, facing the crowd all the time** (`run-pts.ps1`, runs `tex-pts1/2/3`; pts2 has the main thread zones, pts3 per-thread CPU time).

| | still before | still after the turn |
|---|---|---|
| fps (pts1 / pts3) | 72 / 75 | 62 / 65 |
| draws per frame | 8,700-9,000 | 8,800-9,100 (the same) |
| GPU time per frame (sum of passes) | 12.7 ms | 13.0 ms (same, by pass too) |
| worker operations (ms/frame) | 11.9 | 12.7 |
| frame interval on the main thread | 14.0-14.5 ms | 16.1-17.0 ms |
| worker waited for work | 2.0 ms | 3.3 ms |
| used video memory / bias | 8.5 GB / 1.0 | 11.3 GB / 1.9-2.1 |
| rt scene instances | 11.8k | 13.9k (+18%) |

So the GPU is not slower; the main thread is: it is at 95-96% CPU in both windows and takes 2.5 ms longer per frame after the turn. Per zone (call counts the same): `display()` +1.7-1.9 ms, `idle()` +0.7-0.9 ms; named: texture list update +0.4 (decode priority calls per frame 188 to 352), `gObjectList.update` +0.3-0.45, `generateSunShadow` +0.25, deferred lighting +0.3, post-deferred geometry +0.3, local lights +0.2, `updateMovedList` +0.2, `stateSort` +0.2. Everything got 10-18% slower, nothing dominates. No thread's CPU time grew (per-thread, same 45 s and 90 s windows: total 281% before, 251% after; the one thread at 34% before, probably decode and fetch, went to 0). So not competition from other threads. Likely reading: after the turn the whole 360 degrees around the avatar holds built geometry, a 18% larger ray scene, more drawables to walk in the object, shadow and lighting passes. This does not show a leak. Not tested: the teleport case Adam described (the slowdown stays after teleporting out of the busy sim), which would separate "more content built" from "state that never goes away".

**Texture list update cost (the +0.4 ms).** `updateImagesFetchTextures` refreshes 1/`FSTextureUpdateDivisor` of the entries each frame (this fork's default is already 80, with divisor 20 for 3 s after a camera change or a new region; stock is 20). The entries are all the textures the viewer has ever asked for: 11,300 at the first still window, 21,000 after the turn (20,400 of them with a GL texture), so the calls per frame follow (188 to 352 at about 2.2 us each); it is the size of the list that doubled, not the rate. A test with the divisor at 40 (`tex-pts4`) made it worse (286 to 535 calls per frame, 0.63 to 1.08 ms; fps 65 to 55), as expected for a lower divisor; the existing default is the throttled one. Raising it above 80 trades the cost against how long a new texture waits for its first request (a full circle at 1/80 is already about 1.3 s). No code change made. The levers left are a cheaper per-entry check (skip entries with no faces and nothing changed) or a larger divisor as a user setting.

## 21. Texture priority stamp (`LL_VK_TEXSTAMP`, default off)

The face loop of `LLViewerTextureList::updateImageDecodePriority` is 85-90% of the texture list update cost (zones, `tex-pts5`: 0.74 of 0.86 ms after a turn). Its result is kept per texture while nothing it reads has changed: the faces of the texture (`LLViewerTexture::mFaceStamp`, bumped by `addFace/removeFace` and by `LLFace` where a face is set up, moved or re-sized: `setTEOffset`, `setDrawable`, `setViewerObject`, `updateCenterAgent`, the region shift in `LLDrawable`, grass and tree extents), the camera (`LLViewerTextureList::sCameraEpoch`: a hash of position in 10 cm cells, directions in 0.01 cells, field of view, aspect, window size), the bias (in buckets of 0.05) and the age (8 s, `LL_VK_TEXSTAMP_AGE`). Not kept: a texture with a rigged, media or particle face or a face of a moving (active) drawable, one with more than 1024 faces, and one with faces but none on screen (a stale first reading). `LL_VK_TEXSTAMP=1` uses the kept result; `=2` runs the loop anyway and counts the cases where it finds something else (`VkTexStamp` log every 10 s).

Verify runs (`tex-pts6/7/8`, still 60 s, turn 60 s, still 120 s): about 0.15-0.4% of the kept results differ by more than a quarter (1.5-5x either way, `on screen` the same), always on faces whose drawable is not flagged active; never older than the 8 s limit, so a texture's priority can be that late on such a face. Not hunted down yet. First version of the check (`on screen` flipping false to true on a first reading, 0.1%) is fixed by not keeping an evaluation with faces and none on screen.

Use (`tex-pts10`, stamp on, zones on) against `tex-pts5` (stamp off, zones on): after the turn the face loop 0.38 against 0.74 ms, the texture list update 0.63 against 0.98 ms, the main thread's loop interval +1.2 against +3.2 ms over its first still window (different crowds, so the second figure is not a clean pair). Kept 134,000 of 160,000 loops per 30 s in the still window, 17,000 of them re-run at the age limit.

Note about the last runs: `tex-pts7/8/9` (9,000-11,000 draws, GPU 15-17 ms per frame) stayed at 98% GPU before and after the turn, also with the stamp off (`tex-pts9`), so in that heavy scene the main thread's growth after a turn did not take frames. In the lighter scenes of `pts1-5` (7,000-9,000 draws) it did.

## 22. White flashes while textures reload (grass, signs, couch, ground decals)

What Adam saw after the level-drop (section 19): short white flashes on a few places on each spin and while the textures recover (the grass outside the store, signs, the floor under the couch, then the London sign on the ground and the chalk outline); avatars not.

**Counters** (`VkTexCopy` line, new): draw samples of a texture whose name was deleted (6 of 54 five-second windows, 1-2 each), of a texture without an image (0), of an image with no level yet (0); bindless table at most 32,419 of 65,536 slots, 0 refused; downscales by the old draw and copy: 0 (every downscale went through the level drop, 0 refused). So the lookups are not where the white comes from.

**Slots given the stand-in** (`tex-pts12`): `Context::destroyImage` and the view re-creation in `llvkworker.cpp` call `Renderer::bindlessForgetView` at once, which gave every bindless slot naming the old view the white stand-in in the descriptor set of the frame being recorded (update-after-bind), while the draws of that frame that had named the slot earlier still had to run. Counted when it happened (a slot used in the same frame): 1,100-1,300 per 5 s window during the spin and the recovery, 3,400 right after the turn starts, 5,500 at the login load. The image memory is destroyed several frames later in any case, so the swap now waits for the next `beginFrame` (`bindlessFlushForgets`; `LL_VK_FORGET_NOW=1` swaps at once as before). `tex-pts13`: grass, signs and the couch spot stopped flashing; the London sign and the chalk outline still did.

**Alpha classification** (stock `LLImageTex::analyzeAlpha`): the mask or not decision is made again from every uploaded level. A smaller upload no longer overrides the decision made from a bigger level (`mAnalyzedPixels`; same size or bigger decides again; `LL_VK_ALPHA_KEEP=0` is the old behaviour; reset by `forceToInvalidateGLTexture` and by `setNeedsAlphaAndPickMask`). `tex-pts14`: the London sign and the chalk outline stopped flashing (Adam). The counters do not show this as clearly: mask flag changes 2,310 against 2,248 over the run (most are the first classification of a texture, 0 to 1), analyses 13,925 against 15,401, so the attribution to this change rests on the observation, and `LL_VK_ALPHA_KEEP=0` is the A/B if it comes back.

Recovery after the turn (Adam): much better than before even at 14 GB of video memory, GPU peaks of 80-95% while turning once settled.
