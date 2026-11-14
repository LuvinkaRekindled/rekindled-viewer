# Hitchless loading (plan, 2026-11-14)

Roadmap stop 2a. Adam, 2026-11-14: "i'm looking more for a solution that just doesn't hold up whatever is hitching instead of pacing it per frame", with a placeholder for what is not ready: "a placeholder either wouldn't even draw or if it has geometry, maybe white texture".

## The rule

**A frame never waits for loading work.** Anything that is not ready when the frame needs it is skipped or drawn as a placeholder, and the finished piece is swapped in at a frame boundary. No per frame budgets or pacing as the main tool (a budget moves the hitch into a lower frame rate and still holds frames up when the work is one big item).

Placeholders:
1. **Geometry not ready: the object is not drawn.** It appears when its geometry is complete (never half built).
2. **Geometry ready, a texture not: the face is drawn with a plain white texture** (Adam's pick; the stock viewer draws mid grey 0x7f, `sDefaultImagep`; our debug stand-in `LL_VK_STANDIN_MAGENTA` is a lifetime test aid, not the placeholder). The real texture replaces it when its first mip is resident; the texture system already has "resident handles" (PLAN-texture-handles.md), so the swap is a handle update.
3. **A rebuilt object keeps its old geometry until the new one is complete** (no flash of nothing on an edit or a LOD change).
4. **Things a frame reads from the GPU or the disk are never waited for:** use the previous result or a default and fix it a frame or two later.

## What stalls the main thread today (measured 2026-11-14, lap hl-rt1)

One login lap, warm cache, RT on, traced mirror, the hitch watcher at 80 ms (`LL_VK_HITCHWATCH`; stacks of every thread when the main loop or the worker's presents are late; `<work>\hitch-summary.py <log>` groups them). 16 stalls of 80 to 104 ms in the 70 s of the log, the first 20 s of loading:

| Stall (main thread, top of the stack) | ms | Kind |
|---|---|---|
| `LLVolumeFace::createSide` / `LLVolume::createVolumeFaces` / `resizeVertices` (volume creation for new objects) | 83 to 98 (x4) | CPU work for an object that is not drawn yet: async candidate |
| `LLVolumeGeometryManager::rebuildGeom` / `fillBatch` / `LLRebuildBatch::flush` / `LLFace::getGeometryVolume` (group geometry rebuild) | 83 to 86 (x3) | CPU work on a group: async candidate (old geometry stays) |
| `LLVOCacheEntry::LLVOCacheEntry` -> `apr_file_read` (the object cache file read) | 98 | disk read on the main thread: prefetch off thread |
| `LLTexLayer::renderMorphMasks` -> `LLRender::readPixels` -> `LLVkAsync::syncCall` (avatar bake: reads pixels back from the GPU) | 90 | the main thread waits for the worker AND the GPU: placeholder rule 4 |
| `LLVkBridge::beginFrame` waiting (login, and 88 ms at frame 275) | 88 to 102 | the main thread waits for the worker (what is it waiting for: to find out) |
| `LLVOAvatar::cullAvatarsByPixelArea` / `isFullyBaked` / `isTextureDefined` (texture queries) | 104 | CPU, avatar texture bookkeeping |
| `LLFace::calcPixelArea` / `LLSkinningUtil::updateRiggingInfo` | 94 to 101 | CPU |
| `LLMeshRepository::notifyLoadedMeshes` (a regex `shared_ptr` copy) | 84 | CPU |
| particles (`LLViewerPartSim`), `LLVkAsync::mainReserve` (the command ring full), `LLProcessListener::tick` | 87 to 92 | small |

Nothing in this lap is long (the worst is 104 ms): a warm login is nearly smooth. The known long ones are not in it: about 1 s at the wake peak after a turn (the worker busy in `tex_mipmap` / `geo_prepare` while the main thread waits in `beginFrame`, lap n16), the 322 ms creation of the cached object volumes at login, the 2 to 3 s animation asset file open (a Defender exclusion fixed that one). **To measure next:** a teleport lap and a cold cache login (the hitch watcher at 50 ms), and the per source totals, so the order below is by cost and not by guess.

## The mechanisms, by source

**A. Async object geometry (volumes and group rebuilds).** The object's volume is built by a worker job (`LLForkJoinPool` exists: avatar motion and the rebuild batches already run on it, `LL_VK_REBUILD_PARALLEL`); the drawable stays out of the render lists (rule 1) until the job publishes the finished `LLVolume` at a frame boundary; a rebuild builds into new buffers and swaps (rule 3). Open question: the stock object update path (`LLVOVolume::setVolume` -> `LLVolumeMgr::refVolume`) uses the volume at once for bounds and physics; what may run before it is ready needs reading before any code (an object with a placeholder box for its bounds is the likely answer).

**B. Disk and cache reads off the main thread.** The object cache entry read and the animation asset reads: read ahead on an I/O thread, the main thread takes the result when it is there; an object whose cache entry is not read yet simply is not created yet (rule 1).

**C. The main thread never waits for the worker.** Find every `syncCall`, `beginFrame` wait and `mainReserve` (the command ring full): for each, the wait is replaced by use the last result or skip this frame. `readPixels` of the avatar bake (rule 4): the morph masks render on the CPU or the read is asynchronous with a frame or two of delay and the avatar shows its previous or a default skin meanwhile.

**D. Texture growth on the worker** (mip upload, `tex_mipmap`): already off the main thread; its cost shows as a worker late present, not a main thread stall. The fix is the same rule: a frame that is late because the worker is busy uploading is the case to remove, by letting the uploads run on the transfer queue and be picked up whenever they finish (rule 2: white until the first mip is resident).

## Order

1. Measure: teleport lap and cold cache login with the watcher at 50 ms; totals per source (the table above, over the three laps).
2. C first if the waits turn out to be long (they hold the frame for work the frame does not need); it is the smallest change per site and the rule is unambiguous.
3. A (volume creation, then rebuilds), the biggest and the only one that touches the object model: read the object update path first, then a plan of its own.
4. B and D as the measurements say.

## What this plan does not do

Per frame budgets for wake pacing, mip work or rebuilds (the older idea; Adam: not what he wants). The expiry design and draw info work stay as they are (they only point here for hitches).

## Measurements, Citadel (2026-11-14 23:48 to 23:51, `run-viewer-hitch50.bat`, watcher at 50 ms, Adam's saved settings)

Login into the last location (London City, 05:48:31Z), a teleport to Citadel (all the paint cans for sale: Adam's pick for object loading, 05:49:02Z), then two more teleports (away and back, 05:50:43Z and 05:50:56Z). 26 episodes ("the frame came back" lines).

- **Login: 20 episodes of 80 to 390 ms in 8 s** (2.2 s in all). By the main thread's stack at the first capture (53 stalls under 2 s, 4.0 s in all, login and use together): volume and geometry build (`createVolumeFaces`, `refVolume`, `setVolume`, `sculpt`, `rebuildGeom`, `genDrawInfo`, `createGLBuffer`, `LLFace::init`) **15 stalls, 1.3 s**; other 12 / 0.8 s; render CPU (shadow render, lighting, pull batches) 6 / 0.5 s; **readback `syncCall`s (`LLImageTex::readBackRaw` from `LLVOVolume::sculpt`, the avatar bake's `readPixels`) 4 stalls, 0.35 s**; UI and inventory (`LLVisSet::update`, folder view filter) 5 / 0.3 s; texture bookkeeping 5 / 0.3 s; `beginFrame` waits 4 / 0.26 s; object cache and message handling 2 / 0.17 s.
- **Teleports: three "frames" of 2.57, 2.75 and 2.50 s**, each from the teleport request to the line "arrival_fraction is 1" (the main loop's frame number does not advance while the teleport runs, and the stacks differ at every capture: UI layout, shadow render, pull batches, a sculpt readback, the inventory filter: it is several seconds of ordinary work, not one stuck call). The arrival itself afterwards is clean: the largest stall after the Citadel arrival is 154 ms (`updateDrawableGeom`). Whether the teleport screen froze for Adam is not known; about a dozen J2C decode threads were running flat out during it.
- The second and third teleports (away and back) cost the same 2.5 s each, so it is not a cold versus warm cache effect.

Tools: `<work>\hitch-summary.py <log>` (stalls and the main thread's stacks), `<work>\run-viewer-hitch50.bat`.

## Design for the big ticket: object geometry (volumes and group rebuilds), 2026-11-14

Adam: "these little things are going to give it a better feel when people try the viewer for the first time. it's already a big improvement, but if we can get rid of the little hiccups too it would be near perfectly smooth." So the target is every stall above the watcher's floor in a first login and a first teleport, not only the big ones.

### What the Citadel data says about this group (15 of 53 stalls, 1.3 s of 4.0 s)

1. The stalls are **batches**, not single objects: `LLViewerRegion::idleUpdate` -> `addNewObject` -> `processUpdateMessage` -> `LLVOVolume::setVolume` -> `LLPrimitive::setVolume` -> `LLVolumeMgr::refVolume` -> `LLVolumeLODGroup::refLOD` -> `new LLVolume(params, detail)` -> `createVolumeFaces` (new objects from the region cache and from the sim), and `updateGeometry` -> `lodOrSculptChanged` -> `setVolume` (a LOD change). Each volume is cheap, a frame builds tens of them. The stock viewer budgets some of this by time already (the region's `idleUpdate`); the stalls show where the budget overshoots or does not exist. We do not want to tune budgets, we want the work off the main thread.
2. **The volume cache is keyed by shape**: `LLVolumeMgr` keeps an `LLVolumeLODGroup` per set of volume parameters, with 4 LODs each created on first reference (`refLOD`: `mVolumeLODs[lod] = new LLVolume(...)`, under the manager's mutex). A plaza of identical prims builds each shape once. `LLVolume` is already built off the main thread elsewhere (the mesh repository's thread), so construction is thread safe in the cases that matter (to be confirmed for plain prims: static tables in `LLPath` / `LLProfile`).
3. **The group rebuild fills already run on the pool** (`LL_VK_REBUILD_PARALLEL=2`, the default since 2026-10-31, PLAN-view-prep-rework.md 8.10 / 8.11). What stays on the main thread in the stalls: `genDrawInfo` and `LLVertexBuffer::createGLBuffer` (148 ms once), `updateTextureVirtualSize` -> `LLFace::calcPixelArea` and rigging info (92 and 101 ms), `LLFace::getGeometryVolume` / `fillBatch` when a fill is not offloaded (83 to 86 ms), `LLFace::init` for new faces.

### A1. Progressive volumes (the volume builds)

The viewer already has a mechanism for "this object now needs another level of detail": `mLODChanged` -> `updateGeometry` -> `lodOrSculptChanged` -> `setVolume(params, lod)` -> the drawable's volume is replaced and `REBUILD_VOLUME` marks the geometry for a rebuild. The design reuses it: **a first creation is a LOD upgrade from the cheapest LOD.**

- `LLVolumeLODGroup::refLOD(lod)` returns the best LOD that exists (creating LOD 0, the cheapest, synchronously when none exists) and, when the wanted LOD does not exist, posts a job for it to the pool and records the object as a waiter. `LLVOVolume::mLOD` keeps the LOD in use, the wanted one is kept next to it.
- The job builds `new LLVolume(params, detail)` off the main thread into a private pointer; when it is done a frame-boundary step (main thread, a cheap pointer store under the manager's mutex) fills `mVolumeLODs[lod]` and sets `mLODChanged` on the waiters, which the existing path then swaps in. **The visible effect: a prim first appears as its coarse shape and refines a few frames later** (placeholder rule 1 in its better form: geometry exists, so it is drawn; Adam's call: coarse shape or not drawn until ready).
- Applies to plain prims. Not to: meshes (the repository thread already loads and builds them; the main thread's part is `notifyLoadedMeshes`), sculpts (need their map: the readback fix and the placeholder shape exist), flexi and other `isVolumeUnique` volumes (private, mutated per frame), rigged objects (their volume is the mesh).
- Open questions: what `setVolume` does with the volume at once (`getVolume()->calcSurfaceArea()` "NaCl" protection, bounds, `updateSpatialExtents`): all must work on the coarse volume, which they do today for a low LOD object; the physics shape (`LLVolume` user in the sim, not the viewer); the ray traced scene and the shadow passes see the coarse volume until the swap (consistent); thread safety of `LLVolume` construction for plain prims.
- **Measurement gate before any code:** counters for the volume builds: per LOD, the number built per frame, their time (`LL_VK_VOLSTATS`), at a login and at Citadel. If the time is mostly LOD 2 and 3 (the high detail volumes) progressive volumes remove it; if it is LOD 0 (many distinct shapes) the pool alone (build the LOD 0 volumes of a burst in parallel and wait for them at the frame boundary) is the only fix and the placeholder is not needed.

### A2. Rebuild work that is left on the main thread

- `calcPixelArea` / `updateTextureVirtualSize` for faces of a group being rebuilt: the value is recomputed every frame anyway by the texture priority pass, so a rebuild may use the last value (rule 4: use the last result) and only a face that never had one computes it.
- `createGLBuffer` and `updateNumVerts` (vertex buffer allocation in `genDrawInfo`): a pool of released buffers by size class, or creation on the pool thread (the Vulkan mirror's buffer creation is a queued command: the main thread's part should be a name and a push, to be checked why it took 148 ms once).
- The fills that are not offloaded (animated children, small groups): count them in the same stall census before deciding.
- A rebuild that is late keeps the old geometry (rule 3): already the case for groups (the old buffers stay until `genDrawInfo` replaces them); to be confirmed for the first rebuild of a new group (nothing to keep: the group is not drawn, which is rule 1).

### A3. The rest of the "small hiccups" (the other 38 stalls)

The Citadel table above, by group, with the fix in the plan's mechanisms B and C: the object cache read off the main thread, the sculpt readback (done 2026-11-14, `mSculptDeferStamp`), the avatar bake's `readPixels` (one per avatar: the morph mask has to be read back; an asynchronous read with the mask applied a frame or two later), UI and inventory updates (`LLVisSet::update`, the folder view filter: budget them or move the inventory filter to idle time), texture bookkeeping (`isFullyBaked` / `isTextureDefined` per avatar per frame: cache the answer until a texture changes), and the 12 stalls filed under "other" (to be looked at one by one with a CPU zone run).

### Order

1. Volume stats counters (A1's gate) and one CPU zone run of a Citadel login, so "other" and the budgets are known. 2. A2's `calcPixelArea` reuse and the avatar texture bookkeeping cache (small, local). 3. A1, if the gate says LOD 2 and 3 dominate. 4. The rest as the new stall table says. Each step ends with the same Citadel login and teleport (`run-viewer-hitch50.bat`, `hitch-summary.py`), compared with this table.
