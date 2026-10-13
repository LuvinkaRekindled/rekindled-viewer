# Next session: what the traced glossy mirror costs (fps A/B), then the loose ends

Repo `<work>\phoenix-firestorm`, branch `native-vk`. Last commit of 2026-10-13: `04621f65`. The login device lost is solved (`HANDOFF-2026-10-13-login-crash-and-glossy.md`, section 0): read that first only if something crashes again. Build with `cmd.exe /c <work>\build-run.bat` (PowerShell tool, `run_in_background`; the build reports by itself, start no monitor); never start a viewer while a build runs, and never run a hunt while Adam has a viewer open (the scripts refuse and exit 2).

## 1. The task: fps cost of the traced glossy mirror

`glossyrt` (live switch in `<work>\rt-debug.txt`, default 0) traces rays for glossy surfaces (gloss 0.9 and up) of the lighting pass; the mirror in the Larsson room is the test. 0 = the stock hero probe mirror ("legacy mirror"), 1 = traced. Everything Adam has measured so far (about 120 fps in front of the mirror with ray shadows on) was `glossyrt=0`. The traced mirror cost is not measured cleanly: the handoff estimates +0.3 to 0.4 ms GPU per frame with the mirror at about 15 % of the screen. Needed: the steady-state fps difference, and which of the two paths to make the default.

**Baseline (2026-10-13, lean config, `glossyrt=0`, 19 logins, readings every 10 s):** settled median 122 fps, mean 119 (first reading while the world still loads: 99). One login that failed to start read 38 and 62: drop it. Config of that baseline: `LL_VK_RT=1`, `LL_VK_RT_SHADOWS=1`, `LL_VK_RT_MATERIALS=1`, `LL_VK_PRESENT=immediate`, no `LL_VK_PHASES`, no draw stamps, 2558x1360 window, own avatar standing in the mirror room circle.

**How to run it (already set up):**

```
powershell -NoProfile -ExecutionPolicy Bypass -File <work>\hunt.ps1 -Name glossy-ab1 -Runs 6 -Materials -Lean -Settle 60 -Hold 45 -Configs "glossyrt=0|glossyrt=1|glossyrt=0|glossyrt=1|glossyrt=0|glossyrt=1"
python <work>\ab-fps-report.py glossy-ab1 --skip 1
```

- `hunt.ps1 -Lean` = no phase timing, no draw stamps, no opstats (so the GPU runs at its normal load: the diagnostic config leaves it at about 60 %). `-Configs` switches the live keys of `rt-debug.txt` between phases of ONE login, so the sun, the sim and the avatar are identical on both sides (comparing separate logins drifts). `-Hold` is the phase length in seconds, `-Settle` the wait after login before phase 1.
- The viewer's `display_stats : FPS` line comes every 10 s and is the average of the 10 s before it; `ab-fps-report.py` takes the readings that lie wholly inside a phase and prints per-phase means, per-setting means and the mean difference of each change. `--skip 1` drops the first reading of each phase (the first switch to 1 builds the glossy shader permutation, it hitches).
- `hunt.ps1` stops at the first non-zero exit or "device lost": a login error also stops it ("CRASH at run N" without "device lost" in the log is a login problem: free the account with `<work>\free-account.ps1`, it logs in through Radegast; the hunt does that itself when the last log shows no "Goodbye!").
- Adam wants steady-state numbers in the mirror room, ideally also standing so that the mirror fills more of the screen (a second set of runs with Adam in place; he can stand there with the toggle script instead: `<work>\run-viewer-clean-rtcheck-toggle.bat` + `rt-toggle-check.ps1` flip other keys the same way; write a copy for `glossyrt` if needed). GPU ms per pass (not just fps) needs the diagnostic config (`LL_VK_PHASES=1`, drop `-Lean`): the stats line "GPU time by render pass" then names the glossy lighting cost; it changes timing, so use it for the ms figure only, not for the fps.
- **Do not touch the viewer window during a session** (and do not give another window the focus): an unfocused viewer drops to single-digit fps for the readings it covers (a 9 fps reading in the smoke test was Adam typing elsewhere). `ab-fps-report.py` drops readings under `--min-fps` (default 30) for that reason, but the phase then has fewer readings.
- Read the **per-setting medians** from the report, not the means: a reading that overlaps a focus loss is only partly depressed (77 and 69 in the smoke test) and survives the 30 fps floor. Smoke test (one login, 3 phases per setting): median `glossyrt=0` 105.8, `glossyrt=1` 103.1, about 2.7 fps (2.5 %) behind, not conclusive yet.
- Do at least 6 logins (36 phase pairs is plenty), look at the spread between logins before trusting a 1 to 2 fps difference.

**Then decide:** default of `glossyrt`, the gloss cutoff (0.9), legacy shiny surfaces (not traced), and the probe views still in `softenLightF.glsl` (can go once the traced path is the default). Also the item "DLSS ray reconstruction guide data" is next on the plan after glossy.

## 1b. Two things Adam saw in the traced mirror (2026-10-13, after the crash fix)

- **Avatar flicker with `glossyrt=1`**: a fast flicker that makes the avatar in the mirror look half transparent. The own avatar is never left out of the ray scene (`avlog=1` in `rt-debug.txt` writes `<work>t-avatars.log`; no SKIP lines in 90 s), its pose is 0 to 2 frames old on about a third of the calls. What is regular: two frames of three have 3 scene calls and one has only call 0 (9924 : 4962 frames in the log), a 3-frame cycle (about 33 Hz): the extra calls are the hero probe / reflection probe faces (the comment at the call counter in `llvkrt.cpp`: "a second scene call in a frame (a reflection probe, a snapshot) has its own camera"). All calls of a frame share one TLAS and one geometry table per frame slot, so the lighting pass of the main view may trace a scene that a probe call rebuilt with its own camera and culling. Test in progress: the live switch `single=1` (only the first scene call of a frame builds) against `single=0` with `glossyrt=1`: `hunt.ps1 -Name x -Runs 1 -Materials -Lean -Settle 40 -Hold 50 -Configs "glossyrt=1;single=0|glossyrt=1;single=1|..."` while Adam watches (result: see the end of this section if filled in, else ask him). If `single=1` removes it: give each call its own TLAS (or make the lighting pass keep the main scene) instead of the shared one.
- **The traced mirror looks like the diffuse buffer (RenderBufferVisualization 0), not like the lit scene**: not a setting. `rtReflection` (`shadowUtil.glsl`) shades a hit as `(ambient + sun * N.L * shadow) * albedo + emissive`: diffuse only, no specular, no normal map, no roughness, no point lights, no probe ambient. The quality gap to the stock mirror is this shading. Improve the hit shading (the material table already has normal and metallic-roughness textures and factors) before choosing the default.

## 2. Open after the crash fix (none of them blocks the A/B)

- **Selection wireframe jitter** while editing an object (the grid vibrates like name tags and hover text used to): a position computed against another frame's camera than the one drawn with. Look at how the selection outline is drawn in the native path.
- **Alpha outline** around hair and other blended avatar parts in front of foliage or other alpha: a legacy bug, still there. Check first whether it shows in the stock GL path of this build.
- Validation leftovers (harmless): `frag_color` written by the depth-only shadow shaders (a warning; it is a GL leftover, dropped by the hardware), one `Skinned Deferred PBR Alpha Shader` variant with a vec3 fragment input at location 3 and no vertex output (it did not show in the last runs; pipeline build order varies, find it with `RT_VALIDATE=1` runs: the message now names the program).
- The 33 mid-frame flushes at world entry (31 `glGetTexImage`): one short stall at login, none afterwards; trace the callers only if a world-entry hitch matters.
- Revisit list and memory: `project-todo-revisit-list` (flexi vs camera, per-object motion vectors, BC7 textures, startup, stalls, avatar slow load-in after a sim crossing).

## 3. Switches and tools worth knowing (all in the commits of 2026-10-13)

| What | How |
|---|---|
| Lean verdict runs | `hunt.ps1 -Lean` (RT_NO_PHASES=1, draw stamps 0) |
| Ray scene range check (on by default) | `check=` / `skipbad=` in `<work>\rt-debug.txt`, `LL_VK_RT_CHECK`; cost line "range check CPU" in the `rt scene` stats |
| Crash diagnosis if a device lost returns | `RT_DRAW_STAMPS=2` (hunt does it without `-Lean`) + `LL_VK_DRAW_SERIAL=1` (exact faulting draw); `LL_VK_ADDR_LOG=1` (what held the fault page); use them one at a time, together they hid the crash once |
| Old behaviours for A/B | `LL_VK_RING_STALE=1` (the crash bug comes back within a few logins) |
| Validation | `RT_VALIDATE=1` (+ `RT_GPUAV=1` for GPU-assisted: slow, `-Settle 70`); `rtcheck`: `hunt.ps1 -Name x -Runs 2 -Materials` with those env vars set in the same PowerShell call |
| Flushes | VkPhases stats line "mid-frame flushes (each waits for the GPU)" by cause; first 60 also logged one by one |
| Sun | log line `sun direction ... elevation, azimuth, sky '<name>'` (first frame and whenever it moves half a degree) |
