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
- **Recommendation (by fps alone):** neither B nor C1 first. If one of the two is wanted for its own sake, C1 (shadow mode: the flat group table tested on the GPU, compared with the CPU's visible set) is the cheaper and safer, and its value is structural; B has the weaker case. By these numbers the next GPU-side work is OIT or the rigged triangle count, not the visibility chain. If B is still wanted, first measure it with a moving camera: the 27 % is for a still camera.
- **Open from this session:** (1) UI and name tag flicker Adam reported (one pixel shift or bolder text for a frame; not investigated, noted in the side-session folder, to be looked at after S7); (2) the run-to-run spread of the plaza (64.0-68.4 fps with the same code, by crowd) is wider than the 0.3 ms the hand-off assumes: compare A/B pairs inside one run (`LL_VK_*_AB`), not runs; (3) a B0 run with a moving camera; (4) the "second dip" and `arenaNewBlock` timing from the hand-off, untouched.
