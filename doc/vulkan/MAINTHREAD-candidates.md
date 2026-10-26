# The main thread, sorted: what can move to a worker, what can be skipped, what stays

Written 2026-10-26 after the lasting-cost work of `PLAN-texture-handles.md` section 17.22. The viewer runs on about 2.3 cores of 16; the main thread is the one full core and the limit (loop interval 15.9 ms at the crowded plaza after a turn, worker waiting 5.5 ms a frame for work). This is a reading of the CPU zones, not a design: nothing here is built.

Numbers: ms per frame at the second settle of `crowd1`, `crowd3` and `cold1` (75-90 avatars, 48k objects), averaged; zones are inclusive and nested ones overlap, the cycle readings themselves cost about 0.2 ms. The UI contents are those of the lap harness (people floater open), not necessarily a normal session.

## The frame, top level

| block | ms | what is in it |
|---|---|---|
| `display()` | 11.0 (0.8 blocked) | shadows 2.75, deferred lighting 2.67, world-camera sort 1.83, UI 1.19, world cull 0.82, geometry deferred 0.53, updateGeom 0.47, texture list 0.45 |
| `idle()` | 5.5 | object update 3.27, UI update 0.78, texture fetch 0.41, particles 0.25, apparent angles 0.25, moves 0.22, network 0.17 |

## Sorted

**A. Already parallel, but the main thread waits for it: MOVE finer (about 0.4-0.9 ms).**
- Shadow cascade prep (cull job + render map job) runs on the shared fork-join pool (`llforkjoinpool.h`: `min(hardware threads / 2, 8) - 1` = 7 workers here, the calling thread works too). One job per cascade, and the cascades rotate: cascade 0 (about 100 groups) every frame, cascades 1-3 (1,100 / 3,300 / 3,300 groups) one in four frames. So a frame usually has two jobs, one tiny and one with thousands of groups, and the main thread waits for the big one: `shadow prep: cull job (wait for the pool)` 0.45 ms (0.35 blocked) and `render map job` 0.46 ms (0.37 blocked). Seven workers idle. Cut: jobs by cascade x partition (or x octree subtree) instead of by cascade; the shadow prep code already culls a camera as a job (`shadowPrepCull`), so this is finer jobs, not new machinery.
- `packets: wait for the alpha builder` 0.20 ms, `shadow static (fragments)` 0.19: the builders could start before the main thread needs them (they are submitted after the render map jobs).

**B. World-camera visibility, serial on the main thread: MOVE or OVERLAP (about 0.5-1.2 ms, the biggest single block, 3.7 ms in all).**
- `updateCull` 0.82 (133 partition culls a frame), `stateSort (early)` 1.83 (postSort 0.75, visible groups 0.43, drawable groups 0.41, build render map 0.28, rebuildPriorityGroups 0.31), `updateGeom` 0.47, `renderGeomDeferred` 0.53.
- The cascades already do their cull and render map as pool jobs (A); the world camera does the same work serially, and the two do not overlap. Candidates: the world camera's cull as one more job in the same fork-join (it needs the answer first, so it is the job the main thread works on while the workers do the cascades); the render map job for the world camera as for the cascades. Needs checking: whether the cascade cull uses anything the world cull produces (`LLCullShadow::useVerdict`, the C2 flat cull verdicts).

**C. Object and avatar update: part MOVE, part SKIP (about 3.3 ms, win unknown).**
- `objectlist: avatar motion batch` 1.72 (the pose blends already run in parallel; this is the wall time of the batch and its completion loop), `idleUpdate (avatars)` 1.0 (86 avatars, about 12 us each), attachment loop 0.89 inside (the zone overstates it, 0.14 ms measured without zone overhead, 17.13), animated textures 0.36 (`LLViewerTextureAnim::updateClass`; Cold takes most of it for objects nobody sees).
- Candidates: skip the serial update of avatars nobody sees or draws beyond a distance (what impostors do for drawing, not for updating); overlap the motion batch with other main-thread work (measured earlier: hides only 0.34 ms, because the batch is the long pole at the crowded plaza).

**D. Moved list and partition moves: SKIP (about 1.1 ms inclusive, Cold took 0.2-0.3).**
- `updateMovedList` 0.39, `partition move` 0.34 (420 calls), `group updateInGroup` 0.28, `drawable updateMove` 0.28, `bridge updateMove` 0.29, `updateMoveDamped/Normal` 0.2 + 0.13. What is left after Cold: avatar parts, attachments, children of linksets, objects moving inside the view. Not parallel (shared octree and drawable state).

**E. UI, about 2.2-2.5 ms: STAY on the main thread, SKIP or cache what does not change.**
- `render_ui` 1.19 (widgets 0.81, name tags 0.18), `updateUI` 0.78, `main_view` 0.80 / `menu_stack` 0.71 / `Floater View` 0.54 / `floater_people` 0.37 (nested). The UI is retained-mode widgets drawn every frame. Candidates: skip updating hidden or unchanged floaters, cache the draw of static panels in a texture. Depends on the user's layout; measure in a normal session before counting on it. It also grew 7-25% after a turn without any reason in the UI (17.22), which no change here would explain.

**F. Lighting and non-packetized pools: MOVE the recording (about 0.6-1.0 ms).**
- `renderDeferredLighting` 2.67: post-deferred geometry 1.23 (alpha pool 0.51), local lights (boxes) 0.43. The shadow, geometry and rigged passes already go as records built by jobs (render packets); these pools still issue draws one by one from the main thread.

**G. Textures: SKIP / event-driven (about 0.8 ms).**
- `fetch dispatch` 0.41, `updateImages` 0.45, `decode priority (per texture)` 0.25 over 243 calls, `face loop` 0.17. Per-texture polling each frame; the priority could be computed on a worker from the face lists (read-only) or only for textures whose faces moved or changed (events).

**H. Small, stays: particles 0.25, apparent angles 0.25 (a loop over all objects computing apparent size: could be incremental), network 0.17, probes and snapshots 0.30 (the harness).**

## Reading

Nothing here is large alone; the frame is a sum of many 0.2-1 ms blocks, and the list of what is serial is long. In order of (gain / work): A (finer shadow jobs, the machinery exists, 7 workers idle), then B (world cull and render map as pool jobs, the biggest block), then F (packets for the remaining pools), G (event-driven texture priority), C and E need measuring first. A realistic total for A+B+F+G is 2-3 ms of 15.9, i.e. the size of the lasting cost after a turn; none of them touches the growth after a turn by itself (that growth is spread over most of these zones).

## Status after the first pass (2026-10-26, evening)

- **A (finer shadow prep jobs): done as far as it pays.** The marking of the found groups runs in chunks of 128 over the pool (`LL_VK_PREP_CHUNK`): 0.04-0.06 ms. The wall of each fork-join is the longest single job (cull 0.2 ms of partition walks, render map 0.33-0.42 ms for the one big cascade); what is left is the render map job, at most 0.25 ms, not built. The wait on the pool was 0.9 ms of which job time was most; the 0.4-0.9 ms estimate above was too high.
- **B (world camera cull and sort): not built, deferred (Adam) to the Fable overview as a candidate for a deeper rework.** The world cull and the early stateSort do not split cheaply (one partition dominates the cull; the sort feeds GL and pool state in order). The achievable piece is overlap: start the cascade prep before the world cull and join after it, about 0.5-0.7 ms; it needs `LLPipeline::sUseOcclusion` and `sShadowRender` thread-local (the camera id already is) and `generateSunShadow` (pipeline.cpp ~21305, about 900 lines) split into launch / join / render. The cascade prep reads nothing from the world cull's results; only the spot shadows (0.18 ms) do.
- **F (remaining pools as packet records): already converted by the S5 series.** The alpha pass is "the scene table is the only alpha path" (lldrawpoolalpha.cpp `renderAlpha`, S5g): one record per run or transient entry. What is left of the 0.5 ms is the main thread building those runs and entries (the matrix of each transient key, including an inverse for the normal matrix, palette flushes). Not a conversion any more; a micro-optimization at best.
- **G (texture priority): already done by `LL_VK_TEXSTAMP` (default 1) and `LL_VK_TEXREC=2`** (the face loop is kept while the faces are static and unchanged; the worker computes the face sizes). About 24,000 textures stay out of the cache because they have moving, rigged, particle or media faces (avatars, attachments): their loops legitimately rerun. About 0.4 ms is left in the per-texture loop plus 0.4 ms of fetch dispatch bookkeeping.
- **C/D (objects, avatars, moved list): Cold (0.2-0.3 ms) and Released (0.4 ms) are built, off by default, no picture check yet** (`PLAN-texture-handles.md` 17.22).
- **Remaining sum on the CPU: tenths of a millisecond each.** The big steps left are structural (the overlap of B; the UI at 2.2-2.5 ms; the per-frame cost that scales with the scene after a turn), not another pass of the same kind.
