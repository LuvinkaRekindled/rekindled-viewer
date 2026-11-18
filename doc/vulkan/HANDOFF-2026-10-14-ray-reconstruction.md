# Handoff 2026-10-14: DLSS ray reconstruction (next session)

Repo `<work>\phoenix-firestorm`, branch `native-vk`, last commit `777fff0a` (traced mirror quality levels). Working directory of the sessions is `<work>`. Read first: memory `project-dlss-rr-check-2026-10-13` (the RR needs check), `project-glossy-mirror-quality-plan` (its STATUS blocks at the end), this file, `PLAN-ray-tracing.md` (section on ray reconstruction), `PLAN-glossy-mirror-quality.md` (the 2026-10-14 sections).

## Where things stand

The traced glossy mirror is done and committed (quality levels Low / Medium / High / Ultra in Preferences > Graphics > Vulkan, `FSVkMirrorQuality`; hair layer, smooth avatar normals, soft light shadows). What it still lacks, and what ray reconstruction (RR) is for: **a traced reflection has no history, motion vectors or denoising of its own.** One ray per pixel, noisy soft shadows (4 rays per light with per-frame rotated noise at Ultra), and the reflected avatar flickers a little when the camera moves (DLSS sees the mirror surface as static while the reflected avatar shifts by parallax). The agreed order (Adam, 2026-10-13): hit shading A+B (done) -> **RR guide buffers + RR evaluate, first on the mirror, then the water** -> GPU scene S1+.

## What the viewer feeds DLSS today (verified 2026-10-14)

- `rkvkdlss.cpp`: RR is only DETECTED (`gRayReconstruction`, line ~288, `rayReconstructionAvailable()`); `evaluate()` (line ~356) is super resolution only (`NGX_VULKAN_CREATE_DLSS_EXT1`, `NVSDK_NGX_VK_DLSS_Eval_Params`). SDK headers: `<work>\DLSS\DLSS\include\nvsdk_ngx_*_dlssd*.h` (`nvsdk_ngx_defs_dlssd.h`, `nvsdk_ngx_params_dlssd.h`, `nvsdk_ngx_helpers_dlssd_vk.h`); `nvngx_dlssd.dll` is already in `build-vc170-64\newview\Release`.
- The call site: `LLPipeline::applyUpscale(src, dst)` (`pipeline.cpp` ~10659) builds `LLVkMirror::UpscaleArgs` (color = `src` texture 0, depth = `mRT->deferredScreen.getDepth()`, motion = `mMotionMap`, output = `dst`, render and output sizes, jitter from `LLViewerCamera::sJitterPixels`, reset flag) and calls `LLVkMirror::upscale(a)`. It is called near the END of `LLPipeline::renderFinalize` (~11531), on the post-processed LDR image (after tonemap, CAS, glow, DoF, vignette).
- **RR wants noisy linear HDR radiance BEFORE tonemapping**, so RR means moving the upscale up to just after the alpha pass (`mRT->screen`, RGBA16F) and running tonemap / CAS / glow / DoF at output resolution after it. `Params.mHdr` already exists in the DLSS params.
- G-buffer (`deferredScreen`, 4 attachments): [0] diffuse albedo, [1] occlusion / roughness / metallic (ORM), [2] normal + flags (RGBA16 when HDR), [3] emissive. RR needs: diffuse albedo, **specular albedo** (not stored: derive F0 = mix(0.04, albedo, metal) with the environment BRDF in a small pass), normals, roughness, color, depth, motion vectors, optional specular hit distance, optional before / after transparency color guides (hair and other alpha-blended parts have no vectors).
- Own temporal filters (RT shadow accumulation `mShadowAccum`, `RenderRTShadowAccumulate`) must be off when RR is on (RR denoises; double filtering). The mirror's soft shadow noise (`rt_noise_frame`, interleaved gradient noise) is exactly what RR should resolve.
- Traced reflections need a **reflection motion vector + specular hit distance** (not produced yet): the likely real fix for the camera-move flicker of the reflected avatar. For a traced mirror pixel the hit point is known in `rtTraceHitEx` (`hit_pos`, `t`); the previous-frame position of the hit (moving avatar: previous skinned pose) is the harder half.

## UI decision (Adam, 2026-10-13)

RR is ONE extra checkbox under the DLSS options (needs DLSS + ray tracing on); the DLSS quality modes / DLAA stay; with RR on it runs INSTEAD of super resolution. SDK presets: D transformer, E latest transformer, F "default model RR2" (A-C removed): try F first, as a hidden setting.

## Suggested order of work

1. Read the SDK headers (`..._dlssd_vk.h`: create / evaluate parameter structs, required resource states and formats) and decide the pipeline change: where RR runs (before tonemap), what moves to output resolution. This is the big structural step; check what `renderFinalize` does between the lighting and the upscale call and measure what each stage costs at output resolution.
2. Guide buffers: first the ones that exist (albedo, normal, roughness, depth, motion), then the derived specular albedo pass; hit distance and reflection motion vectors for traced pixels last.
3. NGX: create the DLSSD feature, evaluate, reset conditions (same jitter sequence as now), resize.
4. Checkbox + hidden preset setting; fall back to super resolution when RR is unavailable.
5. Test first on the mirror at Ultra (noisiest), then the water; then decide what to retire (own shadow accumulation).

## Test routine that works (Adam only has to close the viewer)

- Build with the PowerShell tool: `cmd.exe /c "<work>\build-run.bat"` (`run_in_background: true`, no monitors; the bash tool strips backslashes of Windows paths). 0 `error C` lines and a new `firestorm-bin.exe` time = success (the llpackage FileNotFound line is noise). The viewer must be closed for every build (never kill a logged-in viewer; Adam closes it).
- Start + login: `powershell -NoProfile -ExecutionPolicy Bypass -File <work>\rt-login-hold.ps1 -Initial "glossyrt=1"` (lean) or `rt-login-zones.ps1` (diagnostic, GPU zone timers). The saved login is Adam's right account. Ground sit with `vk-session.ps1 key "^%s"` about 40 to 60 s after login. rt-debug.txt keys `hitq` / `hitrays` / `glossyrt` override the preferences: delete them from `<work>\rt-debug.txt` before judging UI behaviour.
- Pictures: `rt-hitq-ab.ps1` (phases, 1287 px PrintWindow), `vk-session.ps1 shot x` (native 2574 px; the mirror avatar sits at about x 2080-2400, y 560-960). **Do not poll screenshots every few seconds while Adam watches** (it glitches the window). GPU cost: `gpu-phases.ps1` + `gpu-phases.py` (stock-interleaved phases; the sun drifts the baseline), zone `atmospherics` = the soften pass.
- Crash dumps: `<user-profile>\AppData\Local\CrashDumps`, read with `cdb.exe -z <dump> -y <work>\phoenix-firestorm\build-vc170-64\newview\Release -c ".lines -e; .ecxr; kn 12; q"` (`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`).

## Adam's rules and lessons of this session

- If the viewer crashes, it is something I changed (60+ clean mirror runs before): look at the dump, do not blame the environment.
- Look at the evidence before claiming (I said the mirror avatar was not low poly without checking; the cause was flat normals): judge artifacts at native resolution, not at small zooms.
- When removing test variants, check what each variant changed besides the thing under test (`glossyrt=29` also skipped the normal map; removing it brought the roughness back).
- Commit after every kept change, then `powershell -NoProfile -ExecutionPolicy Bypass -File <work>\vulkan-port-backup\refresh-backup.ps1` (look for "backup refreshed and pushed").
- Open and not blocking: black mirror for a minute after login in some sessions (not reproducible since the probe-list and eviction fixes), an intermittent "rock glitch" (likely tearing with `LL_VK_PRESENT=immediate`, goes away when focus changes), outside window / globe dimmer than the stock mirror, one projector per mirror view. Adam: solve them when they come up.
