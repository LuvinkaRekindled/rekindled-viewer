# Handoff 2026-10-13: Larsson login device lost, glossy mirror, pending items

Repo `<work>\phoenix-firestorm`, branch `native-vk`. Commits of this session: `ce7010be` (BRDF lookup fix), `711e5e5d` (name table leak), `9d1b021a` (device lost report), `f5a01670` (crash hunt tools, switches). Uncommitted at the end of the session: the descriptor flush before submit (`Renderer::endFrame`, `flushAndRestart`, `LL_VK_NO_END_FLUSH`), the slot describer in the device lost report. Build with `cmd.exe /c <work>\build-run.bat` (PowerShell tool, run_in_background), never start the viewer while a build runs.

## 1. Done and verified

- **Glossy mirror was black** because the BRDF lookup texture (`mPbrBrdfLut`) was never drawn: the native renderer builds pipelines in the background and drops the draws until ready, and the lookup is drawn once at start-up. `LLPipeline::generateBrdfLut()` now runs again at frames 200/400/800 of `renderDeferredLighting`. Tested: traced glossy mirror and the stock probe mirror both show the room. Cost of the traced glossy mirror: about +0.3-0.4 ms GPU per frame in the mirror room (mirror about 15 % of the screen); fps cost not measured cleanly. `glossyrt` is still off by default (live switch in `<work>\rt-debug.txt`), gloss cutoff 0.9, legacy shiny surfaces not traced, probe views still in `softenLightF.glsl`.
- **Frame time grew with time** (expire step 0.04 -> 1.7 ms/call): `sNameGroups` leak in the material mode, fixed (`711e5e5d`). Stats line now ends with table sizes.
- **Account freeing:** `<work>\free-account.ps1` (Radegast, real mouse click). `hunt.ps1` calls it only when the last log shows no "Goodbye!" or a device lost.

## 2. The login device lost (open, the main problem)

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
