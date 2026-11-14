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
