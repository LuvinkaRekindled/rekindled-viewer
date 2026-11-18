# Handoff 2026-10-13: Larsson login device lost, glossy mirror, pending items

Repo `<work>\phoenix-firestorm`, branch `native-vk`. Commits of this session: `ce7010be` (BRDF lookup fix), `711e5e5d` (name table leak), `9d1b021a` (device lost report), `f5a01670` (crash hunt tools, switches). Uncommitted at the end of the session: the descriptor flush before submit (`Renderer::endFrame`, `flushAndRestart`, `LL_VK_NO_END_FLUSH`), the slot describer in the device lost report. Build with `cmd.exe /c <work>\build-run.bat` (PowerShell tool, run_in_background), never start the viewer while a build runs.

## 0. RESOLVED (end of 2026-10-13): the login device lost was stale uniform ring offsets

**Cause.** `Renderer::flushAndRestart` (a mid-frame flush: readbacks, `glGetTexImage`, query resolve; it happens in every login, 35 to 60 times, more in the mirror room) keeps the frame number but `beginFrame` restarts the uniform ring at offset 0, and only the CURRENT program was marked dirty. Every other program kept its cached `mRingOffset` (valid while `mRingFrame == frame number`), which pointed into ring memory that new draws then overwrote. The avatar draw (`Deferred Avatar Shader`, 4962 indices) read other programs' bytes as its bindless texture slot index (`rk_ti_*`, a uint into a table of 65536 descriptors) and as bone matrices; a huge index reads unmapped memory (faults at 0x1_1xxxxxxx and 0x29xxxxxx). Most stale reads were harmless one-frame glitches; the crash was the case where the bytes landed on the slot index.

**Fix.** `mRingEpoch`, bumped in `beginFrame`, is the cache key (`ring_key`) instead of the frame number. The vertex scratch ring (palettes and tables the ray scene reads by address later in the frame) is no longer reset by a mid-frame flush (`mRestarting`). `LL_VK_RING_STALE=1` restores both old behaviours (A/B).

**Evidence.** Fix on: 30 of 30 clean (diagnostic config), 36 of 36 clean lean (no phases, stamps, opstats: normal GPU load), 12 of 12 after the vertex ring change. Control (`LL_VK_RING_STALE=1`): crashed at login 2 with the same signature. Avatar programs with bindless off (`LL_VK_BINDLESS_SKIP`): 13 clean. The old rate was about 1 in 4 or 5 logins.

**What did not help (do not repeat):** `LL_VK_ROBUST2=1` (robust image/buffer access: still crashed, so not an out-of-range access), per-draw serialization alone, address log alone. `LL_VK_ADDR_LOG=1` together with `LL_VK_DRAW_SERIAL=1` hid the crash (20 of 20 clean): use diagnostics one at a time, and `hunt.ps1 -Lean` for verdict runs (PHASES and per-draw stamps leave the GPU at about 60 % instead of 90 %).

**Tools added (all env switches, off unless noted):** `LL_VK_ADDR_LOG=1` (VK_EXT_device_address_binding_report: every bind/unbind in a ring, the device lost report lists what touched the fault page, the image ranges of the stuck draw, the last binds and unbinds), `LL_VK_DRAW_SERIAL=1` (+ `_ONLY` / `_SKIP` by target width, `_LOG`; with `RT_DRAW_STAMPS=2` the last reached checkpoint is exactly the faulting draw), `LL_VK_ROBUST2=1`, avatar draw detail in the stamp text (pipeline, vertex bindings, draw / material uniform bytes, bone palette sanity), GL texture lookup for the stuck slot, mid-frame flush log (first 60), sun direction log (`ENVIRONMENT`: elevation, azimuth, sky name; at the first frame and whenever it moves half a degree), `hunt.ps1 -Lean`, `rt-toggle-check.ps1` + `run-viewer-clean-rtcheck-toggle.bat` (live toggle of the ray range check).

**Ray scene clean-up done on the way (all from GPU-assisted validation, now 0 of each):** range check `check=1 skipbad=1` on by default (1 to 2 microseconds a frame, stats line shows "range check CPU"): ranges with NaN or huge vertex positions or indices past the buffer (the real "index out of bounds": a few meshes address 1 to 64 vertices more than their buffer holds) stay out of the structures. `rtRayOk` guard in `shadowUtil.glsl` (`rtSunRay`, `rtReflection`): no ray with a NaN or infinite origin or direction. Dead inputs removed from `pbrglowF.glsl` and the HUD branch of `pbropaqueF.glsl` (SPIR-V interface errors). Left in the validation output: one `Skinned Deferred PBR Alpha Shader` variant with a vec3 input at location 3 and no vertex output (not found), `frag_color` written in shadow passes without a colour attachment (warnings), and a start-up-only `vkBeginCommandBuffer` on a command buffer in the executable state (twice, most likely the profiler context init; harmless).

**Open after this:** flush sources (count per second in the mirror room, which readback or query resolve causes them: each is a CPU/GPU wait), glossy fps cost in steady state, the items in section 3, plus Adam's later notes (selection wireframe jitter while editing an object, alpha outline of hair in front of foliage: see memory `project-todo-revisit-list`). Sections 2 and 4 to 5 below describe the hunt that led here and are history.

## 1. Done and verified

- **Glossy mirror was black** because the BRDF lookup texture (`mPbrBrdfLut`) was never drawn: the native renderer builds pipelines in the background and drops the draws until ready, and the lookup is drawn once at start-up. `LLPipeline::generateBrdfLut()` now runs again at frames 200/400/800 of `renderDeferredLighting`. Tested: traced glossy mirror and the stock probe mirror both show the room. Cost of the traced glossy mirror: about +0.3-0.4 ms GPU per frame in the mirror room (mirror about 15 % of the screen); fps cost not measured cleanly. `glossyrt` is still off by default (live switch in `<work>\rt-debug.txt`), gloss cutoff 0.9, legacy shiny surfaces not traced, probe views still in `softenLightF.glsl`.
- **Frame time grew with time** (expire step 0.04 -> 1.7 ms/call): `sNameGroups` leak in the material mode, fixed (`711e5e5d`). Stats line now ends with table sizes.
- **Account freeing:** `<work>\free-account.ps1` (Radegast, real mouse click). `hunt.ps1` calls it only when the last log shows no "Goodbye!" or a device lost.

## 2. The login device lost (SOLVED, see section 0; the text below is the history of the hunt)

Larsson mirror room login, about 1 login in 4-5, 1-3 s after the own avatar is fully loaded. Evidence (all with the mirror on, 12 clean logins = a clean verdict, p about 3 %):

| Change | Result |
|---|---|
| `RenderMirrors=0` | 12 of 12 clean |
| `LL_VK_BINDLESS=0` | 12 of 12 clean |
| `LL_VK_ASYNC=0` (inline) and `LL_VK_ASYNC=lockstep` | 12 of 12 clean each |
| ray tracing off (`RT_OFF=1`) | still crashes (2 of 2) |
| packets off, `LL_VK_FRAMES=6`, `LL_VK_COARSE_BARRIERS`, `NO_DEVICE_BUFFERS`, `ARENA=0`, `NO_QUERIES`, `LL_VK_INDEXED_CHANNELS=1`, bone palette clamp, `UPDATE_UNUSED_WHILE_PENDING` bit, descriptor flush before submit | each still crashes |
| `LL_VK_BINDLESS_ONLY` bisect over 194 bindless programs | first 97 / 48 programs crash, first 24 clean, every subset of 25-48 clean: needs many bindless programs at once |

So it needs: mirror (hero probe) pass + bindless texture binding in many programs + overlap of the main thread and the worker. Not memory (2.7-4.4 of 12.8 GB), not ray tracing, not packets, not stale slots (no slot used after its image was destroyed).

Where the GPU is: by the checkpoint rule (markers are set BEFORE draws; the draw of the last COMPLETED marker is the oldest unfinished) it is 'Deferred Diffuse Alpha Mask Shader' (a small alpha mask mesh, e.g. 270 indices, indexed texture channels) in 11 of 15 crash logs, otherwise 'Deferred Avatar Shader indices 4962'. VK_EXT_device_fault reports an unmapped-page read (page level precision, a different page each run), constant instruction address per pipeline set. Do not trust exact addresses; the "in arena block" matching never found anything. Do not call vkGetDeviceFaultInfoEXT unless the device is lost (the driver crashes) and zero `vendorBinarySize` before the second call.

Open leads, none proven: (1) bindless descriptor writes in the middle of recording into one set per frame slot (update-after-bind); (2) a main-thread vs worker data race on vertex/index data (only visible with overlap); (3) the hero probe copies into a cube map array (`copyRegion`, `grow_levels` only handles 2D). A new report line (slot describer, uncommitted) prints age, descriptor writes, size, mips, layout of the slots of the oldest unfinished draw: read it on the next crash.

Next-session plan: read the slot describer output from a crash; compare the mirror pass vs main pass on bindless slot usage; if no cause, choose a workaround with a steady-state fps measurement (lockstep, bindless off, or mirror off for the room).

Tools (<work>): `hunt.ps1 -Name x -Runs 12 -Materials [-ExtraArgs "--set;K;V"] [-RtOff]` with env `RT_ENV="LL_VK_X=1;LL_VK_Y=2"`, `RT_FRAMES`, `RT_VALIDATE`, `RT_GPUAV`, `RT_SYNCVAL`, `RT_AV_LOG`; `bisect-bindless.ps1`; per-program switches `LL_VK_BINDLESS_ONLY` / `LL_VK_BINDLESS_SKIP` (exact names, `|` separated, names in the log as "program '<name>': bindless"). Logs of every hunt: `<work>\run-logs\hunt-*`.

## 3. Still pending (order of Adam)

- Fix the ray scene BLAS index range bug (indices past `maxVertex`, found by GPU-assisted validation) and the NaN ray origins in a mirror/reflection shader.
- Glossy: decide the default of `glossyrt`, gloss threshold, legacy shiny, remove the probe views, measure the fps cost in steady state in a PBR-heavy spot.
- DLSS ray reconstruction guide data (the plan item after glossy); avatar slow load-in after sim crossing; smooth normals for avatars (see memory `project-todo-revisit-list`).
- Oddity not explained: validation reports "vkBeginCommandBuffer: command buffer recorded and now in the executable state" about twice per run (not from the frame restart code: it resets the pool first).

## 4. State at the very end of the session

- The last hunt (descriptor flush before submit, mirror on, ray tracing on, materials on) ran 6 clean logins and crashed on the 7th: oldest unfinished draw 'Deferred Diffuse Alpha Mask Shader, indices 270 from 0, texture slots 4108 4435 1 1' (draw 435), started draw 450 'Deferred Avatar Shader, indices 4962' (slot 5088, a 2048x2048 image with 12 mips, alive). That crash had no slot details: the slot describer was in the wrong branch of `reportCheckpoints` (per-draw stamps are not serial checkpoints). Fixed afterwards (the stamped text is parsed in the loop that prints each stage), compiled but NOT yet run: the next crash will print `its slot N: ... given to its image X frames ago, N descriptor writes, last used ..., image WxH format mips layers layout` for each texture slot of the started and completed draws.
- The descriptor flush before submit (`Renderer::endFrame`, `flushAndRestart`; off switch `LL_VK_NO_END_FLUSH`) did not prevent the crash; it is harmless and kept.
- First thing next session: the build in `build-vc170-64/newview/Release` is current; run `powershell -File <work>/hunt.ps1 -Name slotdesc2 -Runs 12 -Materials` (it frees the account itself), then read the "its slot" lines in `<work>/run-logs/hunt-slotdesc2/runN.log` of the crashed run. Look for slots given to their image only a few frames ago and with few descriptor writes (a slot used before its descriptor was ever written into the frame's set).
- **Result of the first hunt with the committed build (e8347ba3: descriptor flush before submit + all earlier changes): 12 of 12 clean** (`<work>/run-logs/hunt-slotdesc2`). Not significant alone (the default-config crash rate seen today is about 1 in 6: 12 clean has about an 11 % chance by luck); a second batch of 12 (`hunt-slotdesc3`) was started right after. Earlier builds crashed at login 2 / 7 / 9 in such batches. If 24 in a row stay clean the flush fix (or the combination of today's small fixes: flush before submit, update-unused-while-pending bit, palette index sanitize) is probably the cause; to find which, A/B with `LL_VK_NO_END_FLUSH=1` (flush off) and `LL_VK_BINDLESS_NOUNUSED=1` (flag off) via `RT_ENV`.

## 5. First crash with slot details (hunt-slotdesc3, login 3 after 2 clean; build e8347ba3)

- Video memory 3979 of 12840 MB. Fault: read of an unmapped page (`0x11a187000`, 4 KB precision).
- Started draw 448 'Deferred Avatar Shader, indices 4962': slot 4995 = 2048x2048 RGBA8, 12 mips, given 18 frames ago, 3 descriptor writes (all 3 frame sets), pinned once, last used 2 frames ago, image alive, layout 5 (shader read only).
- Oldest unfinished draw 445 'Deferred Diffuse Alpha Mask Shader, indices 270 from 0': slots 4263 (256x128, 9 mips, 196 frames old), 3994 (256x256, 9 mips, 218 frames old), both 3 writes, alive, pinned, plus slot 1 (white 32x32) twice.
- **So the texture slots of the draws at the GPU's position are all healthy and written into every frame set: the slot / descriptor / stale texture theories are dead.** What reads the unmapped page is something else in flight at that moment (the mirror pass work, an async compute / transfer queue job, or the hero probe copy into the cube map array); the checkpoints only show the position of the main queue's front end.
- Running count with the committed build (descriptor flush + flags): 12 clean, 2 clean, crash = 1 crash in 15 logins (the same config crashed about 1 in 6 before): not proven better.
- Ideas not yet tried: (a) stamp the mirror/hero probe passes and the async compute / transfer submissions as well (the checkpoints only exist on the graphics queue), (b) test `LL_VK_ASYNC` modes only for the hero pass (e.g. serialize just the hero probe update: wait for the worker after it), (c) hero probe settings: `RenderHeroProbeUpdateRate` 6 (one face per frame) and `RenderHeroProbeResolution` smaller, (d) look at the transfer-only and async compute queues' use (BC7 / texture upload jobs) around avatar load.
