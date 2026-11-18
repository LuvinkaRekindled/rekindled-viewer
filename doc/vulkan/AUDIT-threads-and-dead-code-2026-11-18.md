# Audit: threads, main-thread work, and code that runs unread (2026-11-18)

Asked by Adam: "an audit of our current setup to see if there's anything else we can get off the main thread, or a more efficient way to organize the threads we have", and "redundant or old code that's still doing work that nothing uses".

Tree: 9395f07b5d. Method: reading only. Nothing was edited, built, run or measured. Five read-only studies (the thread inventory, the main thread inside `display()`, the main thread outside it, the render worker, the dead code sweep); the claims marked **(C)** were checked again in the source after the studies came back, the others are a study's reading (V = the study read or grepped it, U = inference). Every cost is from an older zone lap in the docs or an estimate from the code. Paths are under `indra/`.

Builds on `PLAN-thread-organization.md` (T0 to T5) and `PLAN-visible-set-owner.md`.

## 1. Read this first: the main thread's 100 percent includes spinning

**(C)** `frameBegin` (`llrender/rkvkasync.cpp:1404-1420`) loops while the main thread is more than `RK_VK_ASYNC_DEPTH` (default 1) frames ahead of the render worker: 200 `YieldProcessor`, then `SwitchToThread` without end, and a `wake()` every turn. The same pattern is at ring full (`:248-254`, 64 MB ring) and `wait_drained` (`:3120`). The time is added to `gMainWaitTicks` and printed only under `RK_VK_PHASES` (`:1394-1401`).

So "main 100 to 101 percent in every phase" cannot tell work from waiting for the worker. If the wait is large in the turn and the second still, the render worker is the cap there and section 3 comes before section 4.

To do: read `gMainWaitTicks` in a no-zones lap (ms a frame, per phase). A `WaitOnAddress` on `gBeginsDone` after the short spin would free the core.

## 2. The threads as they are

Correction to earlier notes: the render worker is created at `llrender/rkvkasync.cpp:3219` (`workerMain`); `llrender/rkvkworker.cpp:11805` starts the derive thread; `newview/pipeline.cpp:14667` is the shadow LOD thread (only with `RK_VK_SHADOWLOD`); the thread at `llrender/llvertexbuffer.cpp:231` is compiled out (`ENABLE_GL_WORK_QUEUE 0`).

| Thread or pool | Created | Wake | Priority | Notes |
|---|---|---|---|---|
| Main | process | | normal | `RK_VK_PIN_MAIN` |
| Render worker | rkvkasync.cpp:3219 | 300 pause spins, `SwitchToThread`, `WaitOnAddress` 1 ms (`:2954`) | normal | `RK_VK_PIN_WORKER` |
| Derive thread | rkvkworker.cpp:11805, detached | condition variable | normal | no off switch; the comment about `RK_VK_DERIVE_THREAD=0` at `:11772` is stale (no getenv) |
| Fork-join pool | rkforkjoinpool.h:308-325, min(hw/2, 8) - 1 workers | 150 us spin, condition variable | normal | |
| Async pool, 2 | rkasyncpool.h:187 | condition variable | normal | bind job pipeline.cpp:13139, shadow skin and alpha :20130, :20137 |
| Pipeline build pool, 2 | rkvkrenderer.cpp:189-195 | condition variable | below normal | |
| Texture recs | rkviewertexturerecs.cpp:342 | wait 250 ms, sleep 33 ms a pass | normal | |
| RKImageTex, 1 | rkimagetex.cpp:2442 | work queue | normal | texture creation off main |
| ImageDecode, clamp(cores - 4, 2, 8) | llimageworker.cpp:75 | work queue | normal | |
| General, 3 | llappviewer.cpp:2814 | work queue | normal | audio decode, assets, glTF, plugins |
| Mesh repo, mesh LOD 2, physics decomp | llmeshrepository.cpp | signal | normal | |
| Texture cache, texture fetch, LFS | llappviewer.cpp:2861 | queue | normal | |
| HttpService | _httpservice.cpp:214 | 2 ms sleep poll while busy | normal | |
| Window | llwindowwin32.cpp:4735 | GetMessage | normal | the only thread with `SetThreadDescription` **(C)** |
| Watchdog, disk cache purge, plugin poll | | sleep | normal | |
| Stall watch, hitch and sample, fast timer log | | sleep | normal | diagnostics switches only |

Findings on the organization:

1. **T0 is not done (C).** One `SetThreadDescription` in the tree. `LLThread` names by the debugger exception, `LL::ThreadPool` names for the profiler only, no raw `std::thread` is named. The thread census is by tid.
2. **T1 is not done (V).** Kinds 21, 29 and 16 still run on the render worker; only the consumers' part of the main derive moved (`derive_publish`, rkvkworker.cpp:11403).
3. **T2 is not done (V).** Two table copies (`sCullShadow` :11069, `DeriveThread::mS` :11790). A delta row is copied four times: main table to ring blob, ring to the worker's table (:11937), to three new job vectors (:11950-11962), to the derive thread's table (:11836).
4. **There is no render to derive join today (V).** The derive thread publishes to three mailboxes the main thread reads (`gLodMutex`, `gConsMutex`, `gViewSetMutex`, rkvkbridge.cpp:1721-1800). The worker never waits for it. Requests are dropped over a queue of 400 (:11796). The join of section 8 of the thread plan does not exist yet.
5. **Streaming is not below the frame threads (C for the priorities).** The only `SetThreadPriority` calls below normal are the pipeline build pool and the shadow LOD thread. On 16 logical cores: main, worker, derive, 7 pool, 2 async, RKImageTex = 13 frame path threads, then 8 ImageDecode, 3 General, 2 mesh LOD, fetch, cache, LFS, HTTP, texture recs at normal. The pool budget (hw/2) does not count the other frame threads. U: the effect while textures load in the turn.
6. **Two pools on the frame path (V use, U merge).** The async pool runs 2 to 4 jobs a frame; the fork-join pool's `launch()` / `join()` has the same contract. Not checked: whether `quiesce()` or job lifetime blocks a merge.
7. **Two threads woken every frame for nothing (C).** `LLQueuedThread::updateQueue` (`llcommon/llqueuedthread.cpp:164`) posts a `threadedUpdate` lambda on every call from `llappviewer.cpp:2076`, `:2081`. Only `LLTextureFetch` overrides `threadedUpdate`; the texture cache and LFS threads wake and do nothing.
8. **Locks shared by main and the worker each frame (V), low.** `gPaceMutex` twice in `frameBegin` and at each present and idle wait; the gap vectors it guards are read only under `RK_VK_PHASES`. `gLodMutex` is held across a copy of table/32 words on both sides (rkvkbridge.cpp:1728-1742).
9. **`syncCall` (V), low unless something syncs every frame.** `CaptureStackBackTrace` and two clock reads on every call (rkvkasync.cpp:1121-1131). Not established which callers run per frame; `RK_VK_SYNC_LOG` says.

## 3. The render worker: work that is not recording

Op kinds with no producer, dead (V; the dispatch arm for 3, 25, 26 **(C)** at rkvkworker.cpp:12624): kinds 3, 25, 26; kinds 8 and 9 with `on_velocity_packets` and `on_object_velocity_packets` (:6011 to about :6317), and the kind 9 comment at rkvkbridge.h:272.

By estimated value:

1. **Kind 16, the resident draw, scans and copies the whole scene each frame.** `res_refresh` (:6821) walks every fragment of `sRes.mFrags` and every buffer pair (**(C)** the loop). `res_draw` copies the master table to scratch (48 bytes a row plus 16 a row plus the matrices) when `mCopyFrame != frame_now`, so once every frame whatever `mVersion` says (**(C)** :8053-8066). `alpha_res_draw` the same at 96 bytes a row (:7560). U: this is most of kind 16's 0.27 to 0.42 ms. Change: a versioned copy that is made again only when `mVersion` moves; the pair check from buffer events (`res_buffer_gone`, `sResChanged` exist).
2. **The main view's verdict pass.** `cull_derive_main_view` (:11633, from :9113): three table sized assigns, a `cull_row_verdict` loop over the table, a copy of `vis_words` and a mutex push. Its inputs are in the kind 21 blob (pipeline.cpp:13527-13578) and the derive thread has the same table. Same frame dependency: `own_handles` before the listing loop (:9172) and the `mHandleStamp` / `mRecords` recency rule (:11690). Change: main posts the view and the kind 35 deltas to the derive thread; the worker joins on the handle list at kind 21. Measure the join wait first. T2 falls out of this.
3. **`geo_prepare` makes the static list from nothing each frame.** For each listed fragment a 160 byte `GeoGpuG` row and a command per command into write-combined scratch (:9300-9330), again for rigged (:9338-9410). It reads worker state (buffer addresses, pin slots), so it stays on the worker. Change: keep the copy per frame slot and patch the changed fragments (a hash of the listed set plus a row version).
4. **Kind 29 pass begin is sized by the largest slot ever.** `slots = max(mMaxSlot, mMaxSlotAll) + 1` (:9974), `mMaxSlotAll` never shrinks (:8470); each begin clears 176 bytes a slot, writes 64, and makes `geo_matrix` again for every sent slot that `geo_draw` made for the same view (:9541-9575). Two uncached `sGL.mBuffers.find` a pair (:9992-10002). A second pass over every command for `mZeroStatic` / `mZeroRigged`, read by an `RK_DIAG` line, not gated (:10116, :10157).
5. **Kind 15 is the same for every view and is sent every shadow pass** (pipeline.cpp:14342-14365; main also builds an `unordered_set` and a vector each pass).
6. **`geo_stale_check` is on by default (C)**: every 16th prepare over all listed fragments, pairs and textures (:9272, :9024; `RK_VK_SCENE_GEO_STALECHECK=0` turns it off). Behind `rk_diag_on()` if its counters read 0.
7. **Small, per record (V):** two rdtsc in `execute()` for the slow command log (rkvkasync.cpp:2899-2911); about ten `steady_clock::now()` a record in kinds 21 and 29 whether timed or not. The `RK_VK_OPSTATS` census leaves out kinds 40, 41 and 43 (`>= 40`, :12457), so the view requests never show in it.

Secondary command buffers (R3): kind 22 is the best candidate (commands, rows and matrices are in scratch by address after kind 21); the cascades (16, 37) next. Blocked today by one `mCmd` and one applied-state cache in `Renderer` (rkvkrenderer.cpp:2213-2300), `Program` uniform state changed per record, the bump allocators without a lock, `flush_buffer_updates` ending the pass mid stream (:1956), and the per frame statics (`sGeo`, `gPassState`).

Clean: the stats, phase timers and op stats are behind switches; the bindless writes are incremental; `handles_refresh`, `geo_reclaim`, `res_reclaim` follow changes; Hi-Z, the consumer list, the LOD bits and the view sets all have readers.

## 4. The main thread

### Inside display()

1. **The probe cube faces are the last full user of the old walk** (0.43 to 0.66 ms; plan line 159: face updateCull 0.07-0.11, face generateSunShadow 0.16-0.27, face stateSort 0.05-0.08). `main_kept` leaves out `gCubeSnapshot` (pipeline.cpp:3848), every partition is walked (:3870-3889), the pool is off for faces (:27448), each face's 2 cascades go through `renderShadow`'s own `updateCull` and `stateSort` (:25508, :25514), `getVisiblePointCloud` walks again (:27312). The worker set for faces was tried and dropped (stage 10a): not again. Change: the face's cascades through `run_shadow_prep_batch`, or the faces sample the main view's sun maps. U: the hard part is the `sUseOcclusion` / `sShadowRender` / `gCubeSnapshot` globals the jobs read.
2. **The materials pool makes 24 full `bindDeferredShader` calls a main view** (12 not rigged in every view): lldrawpoolmaterials.cpp:48-51, :99-156; pipeline.cpp:22597-22823 (about 10 `enableTexture`, 30 uniforms, 6 shadow matrices, `bindReflectionProbes`). The simple, alpha mask and grass pools use a plain `bind()`. `pushGeometryPackets` sends kind 22 whether the type has rows or not (lldrawpool.cpp:1426-1466). U: that the pull material programs read none of the deferred inputs. The zone "materials: bind pull program" has the cost.
3. **`far_avatar_tier` scans every visible group each frame** (pipeline.cpp:6845-6922, called :7519; 0.08 ms), a new `unordered_map` of vectors a frame. Readers: llvoavatar.cpp:15590-15597. The tier has hysteresis: a frame late, or every Nth frame, or summed when an attachment fragment is built.
4. **`sortLoopsParallel` makes three serial passes over all visible groups before it forks**: the pre-scan (:5623-5637), the `loopc_rest` filter (:5696-5704), `rk_lod_cand_collect` under `sCullMutex` (:11899-11935). U: the kept pass chunks (:12296-12343) visit each group already and could make all three; `rk_occluded_retest` runs after that pass, so the order needs care.
5. **The probe uniforms are filled and uploaded whole for every `renderGeomDeferred`** (pipeline.cpp:15668; llreflectionmapmanager.cpp:1089-1317; about 49 KB by the struct). `mHeroProbeManager.updateUniforms()` runs twice **(C)** (pipeline.cpp:15669 and llreflectionmapmanager.cpp:1298). `update()` sorts all probes each frame (:350). The second hero call can go now; the bucket and neighbour build depends on the probe list and the modelview only.
6. **The avatar pool's shadow passes are old path draws from main** (`renderGeomShadow`, pipeline.cpp:16020-16081, called :26409: each avatar pool, 3 passes, a bind and `renderSkinned`). U: the cost (zone "renderShadow: geom").
7. **Small leftovers:** the texture unbind loop (llviewerdisplay.cpp:1383-1411: `unbind()` then `clear()` a unit, three clock reads); the impostor bracket saves and restores matrices and sets the viewport twice when nothing is generated (:1028-1038); `addBeacons` still does `mSelectedFaces.clear()`, `getTextureChannelToEdit()`, `flushBuffers()` each sort (pipeline.cpp:7427-7483).

What is left on the old walk: the main view walks the HUD partitions and the VO cache only; the cascades and spots take the covered partitions from the worker's set when the planes match and walk the rest; the impostors do not walk; the probe faces walk everything.

Correction to `PLAN-thread-organization.md`: "textures: fetch dispatch" (0.22 to 0.26 ms) runs inside `display()` (llviewerdisplay.cpp:639), not in `idle()`.

### Outside display()

Zone figures from `<work>\run-logs\j2-def1.log` line 25092 (loop 9.60 ms: display 6.26, idle 2.64, probes and snapshots 0.59).

1. **The animated texture poll: about 0.20 ms for almost no writes.** `llviewertextureanim.cpp:135` walks all 453 instances each frame; with `RKTexAnimOnGPU` `animateTexturesGpu` (llvovolume.cpp:1022) makes the parameters of 385 faces again and compares them; the log shows 1 to 15 writes per 600 frames. Change: by event (the animation message, a texture entry change, a face realloc, becoming visible); only `setClock` and the CPU path animations need the frame.
2. **Two world picks with the same arguments a frame (C).** `LLToolPie::handleHover`: `pickImmediate(x, y, false, pick_rigged)` at lltoolpie.cpp:872 and again at :923, every frame the mouse is over the world. The comment at :922 says the second is to detect transparent objects, so first find out whether the second was meant to pass `true`; if not, reuse `mHoverPick`. `updateUI` zone 0.29 ms; the picks' share is not measured.
3. **Particle sim (0.11 ms).** The census at llviewerpartsim.cpp:1556-1562 loops over every record of every group each frame **(C)**, and :862-879 adds about 17 counters a stepped particle; the only reader is the `RK_DIAG("Q1Census")` report (:1603). Behind `rk_diag_on()`. U: the group stepping could go to the pool (`markRebuild`, `mVPCallback`, the shared count and group changes are the hard parts).
4. **`cullAvatarsByPixelArea` at the end of every `updateApparentAngles`** (llvoavatar.cpp:15196): a list sort of all avatars and two walks; its rank changes with visibility. Throttle. (The visit of the objects itself was measured and its skip tried and removed: not proposed again.)
5. **The region probe grid every frame.** `LLSurface::idleUpdate` (llsurface.cpp:860) always calls `updateReflectionProbes` **(C)** (llviewerregion.cpp:1373): 64 `resolveHeightRegion` a region a frame; the values change with terrain edits or an origin shift.
6. **`LLPerfStats::updateClass`: three walks over all avatars a frame** (llperfstats.cpp:304-306). Readers: the autotuner and the performance floater. `mGPURenderTime` changes only in `readProfileQuery`, a GL era timer query; not checked whether it yields anything now.
7. **Per frame `instance_snapshot()`**: `LLEventTimer::updateClass`, `LLToast`, `LLLayoutStack` (most return at `!mNeedsLayout`), `LLConsole`.
8. **Idle callbacks (0.10 ms zone):** `LLInventoryPanel::idle` for every panel including hidden ones; `LLViewerMedia::onIdle` sorts all media impls; `LLVivoxVoiceClient::idle` is an empty function called every frame (llvoicevivox.cpp:732).
9. **Stats and string lookups, all small:** `LLTrace` `nextPeriod` (llappviewer.cpp:1809), `getPeriodMeanPerSec` over 200 periods twice a frame, `stat_type_t::getInstance("Frame")`, about 8 `LLFloaterReg::instanceVisible("...")` a frame.
10. **Sleeps on by default:** `FSLimitFramerate` with `FramePerSecondLimit` 60 (llappviewer.cpp:2123; the laps run above 60, so the lap setup overrides it); `BackgroundYieldTime` 40 ms a frame when unfocused (:2049).

Already covered by a plan, not listed again: the avatar motion batch, completions and attachment replay (`PLAN-avatar-frame-record.md`), `updateMove`, audio, the UI draw (`PLAN-ui-record-cache.md`).

## 5. Code that runs and nothing reads, and old code

Runs by default:

1. **`LLPipeline::mNumVisibleFaces` (C)**: an atomic `fetch_add` per visible drawable from every pool thread (pipeline.cpp:6570), reset at :2293, no reader: one cache line contended for nothing. **`mLightMovingMask` (C)**: written at :17991, :18017, no reader.
2. **`LLSpatialGroup::mBufferMap` (C)**: built on every volume rebuild (llvovolume.cpp:8007-8010, a map of maps of pointer vectors), read only by the debug `pushVerts` (llspatialpartition.cpp:1762). U: the faces hold their own buffer references. Needs a look at the octree debug views after removal.
3. **Three position scans, default on.** `RK_VK_GEOMCHECK` (**(C)** default on, llface.cpp:1458; `geom_check_positions` :1504, `geom_check_indices` :1801) loops over every source vertex and written slot per face fill; `RK_VK_VBCHECK` (llvertexbuffer.cpp:1441) scans every flush again with three clock calls and a 10 s log line, and carries `RK_VK_VBFIX`, so its scan does work; `RK_VK_RT_CHECK` (rkvkrt.cpp:363) scans again at the structure build. GEOMCHECK is the redundant one (about 100 lines; only the owner's name in the warning is lost).
4. **The drawable destroy ring (C)**: `RK_VK_DRAWABLE_RING` default on, `CaptureStackBackTrace(2, 18)` for every drawable freed (lldrawable.cpp:202-216), read only by a crash report. A burst on teleport or region unload. Marked "while the crash of 2026-11-01 is open": Adam's call. `RK_VK_DRAWABLE_TRACE` is also default on.
5. **Logs and timers not behind diagnostics:** `SortSplit` (pipeline.cpp:6270-6360: clock reads per sort, a line every 600); `VkLodCand` prints every 600 sorts with counters that fill only with diagnostics on, so it prints zeros (:11979-11994); the `PhaseCheck` summary every 600 frames (:12103-12107); `PcRetain` (:27089); the `VkRt` sun line (:23554); the 10 s `VkTexIdx` and `VkTexRec` lines (llviewertexturelist.cpp:1015-1034); `VkCullDerive` (rkvkworker.cpp:11763); the early sort A/B clock every frame (llviewerdisplay.cpp:522-539); `note_pool_draws` counts always and reports under `RK_DIAG` (pipeline.cpp:15548-15595). `CullPose` every 60 builds is read by the lap scripts: an env switch, not a deletion.
6. **The group row's `viewAngle`**: 16 of the row's 288 bytes; its one write stores the same value as `lastUpdateViewAngle` on the next line (llspatialpartition.cpp:869) and its reads copy it back (llspatialpartition.cpp:548, llvopartgroup.cpp:1191, 1379, 1406, llvovolume.cpp:6389).
7. **Fields with a diagnostic reader only, or none:** `LLSpatialGroup::mGeometryBytes` (read by `sceneStateCensus` behind a switch), `mMapPlan` (declaration only), `LLFacePool::mMoveFace` (declaration only), `LLSpatialGroup::mBuilt` (debug render).
8. **`RK_VK_DRAW_RING` default on** (rkvkrenderer.cpp:4333): a record per draw when the device has checkpoints, read after a device loss. Kept on purpose, listed because it is per draw.

Dead behind a switch (the default never takes it):

- The sort checks: `sortSplitCheck` (pipeline.cpp:5472, about 90 lines), `sortDryRun` and `sortParallelCheck` (:5960-6113, about 155). `sortLoops` stays: it is the fallback when the pool cannot take the sort.
- `shadowPrepCompare` (:15339-15447, about 110 lines) and the serial reference pass (:27499-27527, :25497-25501).
- The switches of the plan's line 307 are still there with their old paths; not sized here beyond the inventory.

Compiled, never called (V):

- `llrender/llrendernavprim.cpp/.h` (59 lines).
- `llrender/llpostprocess.cpp` (388 lines): only `initClass`, cleanup and `invalidate()` are called; nothing applies it.
- `llvertexbuffer.cpp:84-256`: `GLWorkQueue` and `RKRenderWorkerThread` (about 190 lines) under `ENABLE_GL_WORK_QUEUE 0`.
- `LLPipeline::replayMember`, `replayAdopted` (pipeline.cpp:4034-4060), `clearRebuildDrawables` (:4348), `bindScreenToTexture` (:20171). `replayMemberShadow` is live.
- The render worker's dead kinds of section 3.
- Stale header: `rkcullshadow.h` says "Measurement only... Off unless RK_VK_CULL_SHADOW=1"; `RKCullShadow::build` runs at the end of every world cull (pipeline.cpp:3852).

Switches: 292 `getenv("RK_VK_` / `"FS_` sites (pipeline.cpp 67, rkvkworker.cpp 30, llviewershadermgr.cpp 24, rkvkrenderer.cpp 23, llvoavatar.cpp 18), plus 43 `GetEnvironmentVariableA` and about 22 `env_u64`. No uncached per frame read was found; no compare mode is on by default; not all 292 were opened.

The octrees: all 13 partition types are still created. The seven covered types are not walked by the main view but are still kept up to date on the main thread on every object move (`markPartitionMove`, `processPartitionQ`, llviewerdisplay.cpp:946) for their other readers: the cascades' cull, every other view (probe faces, impostors, mirrors, snapshots, HUD), picking, rectangle select, and the group to slot table.

A study's claim that was wrong, dropped: `LLDrawable::mGeneration` is read (`getGeneration() == -1` in llhudeffectbeam.cpp:277, llhudeffectlookat.cpp:930, llhudeffectpointat.cpp:458).

## 6. Order I would take

1. `gMainWaitTicks` in a no-zones lap, and T0 (the thread names). They say whether the next strip comes off the main thread or the render worker. No behaviour change.
2. The deletions that need no eye: `mNumVisibleFaces`, `mLightMovingMask`, the worker's dead kinds, the never-called functions, the particle census and the alpha pass counters behind diagnostics, the logs of item 5, the empty Vivox idle, the second hero probe update, the no-op thread wakes.
3. The render worker: the resident copy by version (kind 16), then the verdict pass to the derive thread with T2.
4. The main thread: the animated texture poll by event, the hover pick, `far_avatar_tier`, the probe faces' cascades.
5. Priorities (T5's streaming below normal) as a lap test.

## 7. Not covered

`rkvkrt.cpp` (`buildScene`, `res_ray_membership`), the generic `CMD_DRAW` path, texture upload, `probe_run`, `rkvknative.cpp`; `RKHiz::apply`, `ownersCollect`, `shadowPrepMark` / `Rebuild` / `RenderMap`, `rk_scene_geo_frame`, the alpha pool's draw body, the terrain, water, sky and tree pools, the planar mirror view, `renderFinalize`, `LLViewerWindow::draw` and the name tags, the spot block of `generateSunShadow`; `updateCharacter` internals and the baked texture bookkeeping, voice and media internals, `gatherInput`; the shader program uniform maps, `llrendertarget`, the `LLTexUnit` shadowing; whether the CPU particle sim still steps particles the GPU owns; the twin and copy-vs-native status (the audit doc is from 2026-10-07). About 0.13 ms of `idle()` has no zone. `MAINTHREAD-candidates.md` was read only as far as its sorted table by the `display()` study, so items 2, 6, 7 of section 4 (display) were not checked against its later sections.
