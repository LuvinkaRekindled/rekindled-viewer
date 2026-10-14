# Handoff 2026-10-14 evening: ray reconstruction done, traced mirror flicker hunt, hair is what is left

Repo `<work>\phoenix-firestorm`, branch `native-vk`, last commit `c03c0c26`. Read first: memory `project-dlss-rr-check-2026-10-13` (RR status block at its end) and `project-glossy-mirror-flicker-hunt` (the 2026-10-14 block).

## Done and committed

- **DLSS ray reconstruction (RR) works** (`FSVkRayReconstruction` checkbox under the DLSS options, hidden `FSVkRRPreset`, default 6 = F). RR runs on the noisy linear HDR picture (`mRT->screen`) before the tonemap; guides from `rrGuidesF.glsl` (16 bit diffuse albedo, specular albedo, world normal + roughness), the exposure map as exposure texture; the post chain (tonemap, CAS, glow, vignette) then runs at window size on RGBA16F targets (8 bit bands in dark gradients once nothing averages them: Adam saw it, 16 bit fixed it). RR returns no alpha: the glow amount in alpha is restored from the render size picture. Own RT shadow accumulation is off with RR. Depth of field is not applied with RR yet. fps with RR is fine (55).
- **Reflection guides for the traced mirror**: the soften pass leaves the hit distance in the output alpha (`rr_hit_distance` uniform, `rt_hit_dist_out`), `extractRRHitDistance` moves it to `mRRHitDist` and clears the glow alpha; `rrReflMotionF.glsl` projects the hit point with this frame's and last frame's camera into specular motion vectors (`mRRReflMotion`), given to NGX as `pInMotionVectorsReflections`. A hit distance + matrices variant exists (rr-debug mode 10). **Neither made a visible difference** to the flicker Adam sees (A/B with mode 7 = no reflection guides); keep them, they are the right data, but do not expect them to fix the flicker.
- **Avatar flash fixed** (the real cause of the camera-move flicker of the whole reflected avatar): with 128 or more `SkinDraw`s, a new draw reused a dead slot and `noteSkinned` wiped `mBuiltIdx`, so the scene call after it in the same frame left the avatar out ("no geometries" SKIP, white flash in debug view 9). Now only wiped when the slot is part of the built structure.
- **Hair body mesh steady in the structure**: a body mesh (mode 1/2) whose buffer only MOVED (address changed, epoch and size same) was dropped on every frame it was not named; now it takes the new place (`built_draws` steady at 53 instead of 52/53).
- Avatar log (`avlog=1`) now prints why draws are left out (`excluded dead/unnamed/old/...`, `slot_clears`, per draw `why`, `rec_calls`, `rec_bad`; the draws dump comes every 20 frames).

## Open: the hair still flickers in the traced mirror (Adam, 2026-10-14)

- Hair only, camera motion only (steady when the camera is still), with DLSS off, with RR on, with all reflection guides, with flat hit colours (hitq 0), 64 or 1024 masked candidates (glossyrt 22). In lit modes (hitq 1/2) the hair also pulses bright/dark.
- Ruled out by the logs: the avatar structure (steady), the alpha test records (`rec_bad` 0 in about 2000 scene calls), buffer moves (fixed).
- Working theory: the hair is thin alpha-cut cards; one ray per pixel hits or misses a strand depending on the sub-pixel position, and nothing can average it while the camera moves (RR has no hit position motion for it; the reflection vectors are right for static geometry but the coverage still flips per frame). Ideas: a softer coverage for masked hair hits (blurred alpha mip, like `rtShadowRaySoft`'s coverage), alpha to coverage style dithering that is stable over frames, more than one ray for hair pixels, or treating the hair as the "layer" (blended) path that already exists for the blended hair layer (`layer_alpha`).
- **Adam's second hair request**: shadows hit HARD on hair but SOFT on skin and clothes. Hair is drawn in the alpha pass, outside the soft ray traced sun shadow of the opaque lighting. Look at the same path: the hair shading in `rtLitHitColor` / the alpha pass lighting.

## Test tools made today (all in `<work>`)

- `rr-login-hold.ps1 -Initial "glossyrt=1;avlog=1" -Visual 9|-1 -More "--set;FSVkDLSS;1;--set;FSVkRayReconstruction;1;--set;FSVkRayTracing;1;--set;FSVkGlossyMirror;1"` starts the viewer and logs in (use `RT_PRESENT=mailbox` for another present mode). DLSS fully off: `FSVkDLSS;0`.
- `rr-debug.txt` (one number, read every ~60 frames): 0 normal, 1 no RR, 2/3/4 guide views, 5 plain DLSS, 6 RR + own accumulation, 7 no reflection guides, 8 reflection motion view, 10 hit distance + matrices instead of the vectors.
- `hotkey-ring.ps1`: keeps the last 3 s of the screen, Pause/Break saves them to `strobe\hot\N` (8-12 fps: too slow for single frame flicker, good for what changes in a few seconds). `rt-phases.ps1` (beeps per phase) for live A/B of `rt-debug.txt` keys.
- The avatar log analysis pattern: rows of `rt-avatars.log` per scene call (self avatar), SKIP rows, `rt-avatar-draws.log` dumps; see the python snippets in the session transcript (frame numbers repeat between sessions: move the old logs aside before a run).

## Streamline / DLSS 5 (Adam, 2026-10-14)

`kFeatureDLSS_NR = 1004` is in the public `sl_core_types.h`, but there is no `sl_dlss_nr.h` and no `sl.dlss_nr.dll` in the public repo. Adam: the DLL is not part of the SDK, a higher developer tier gets it on request (or it can be taken from a game that ships it: fine for local tests, not for shipping). Streamline would interface with it. A real hookup = adopting Streamline (manual hooking for Vulkan, `ProgrammingGuideManualHooking.md`), a separate step, only once Adam has the plugin and header.

## Rules and lessons of today (Adam)

- Close the viewer for every build (Adam does it); never kill a logged-in viewer. A "viewer is already running" from the launcher can be the previous one still closing: wait a few seconds.
- Judge flicker with Adam's eyes and live A/B (beeps), then confirm in the logs; stills at 8-12 fps cannot show a one-frame flicker.
- Commit after every kept change, then `powershell -NoProfile -ExecutionPolicy Bypass -File <work>\vulkan-port-backup\refresh-backup.ps1`.
