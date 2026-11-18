# Plan: the legacy cascades' casters from the GPU, with a live CPU/GPU dial (2026-11-04)

Status: plan only, nothing built. Adam's idea (2026-11-04): RT off is CPU-bound with an idle GPU, the cascade caster work does not exist with RT on, so it is a clean first candidate for a path that can move between CPU and GPU while the viewer runs. "If it can be toggled without issues it could be one of our first load-balanced paths."

## 1. Why this candidate

- RT off, ground-sit pose, zone-timer laps (`noturn-ro-phasesoff1`): loop 15.9-16.5 ms, GPU 9.1-9.3 ms a frame (55-58 % of the loop), main thread 101 % of one core, the worker waits 7.2-7.6 ms a frame for work. RT on (`noturn-al-phaseson1`): loop 14.3 ms, GPU 9.6-10.0 ms, worker waits 4.1-4.7 ms. **The zone timers cost the main thread several ms (more with the RT-off zones), so these loop times are not the real fps gap: step 0 measures it without timers.**
- The legacy cascade passes cost the GPU about 2.0 ms (shadow casters about 1.65, soften shadow 0.35). The main thread pays 1.8-2.1 ms for `generateSunShadow` (renderShadow recording 0.75-0.96, cull and cloud waits 0.31-0.33, spot/projector 0.15-0.31); in a turn the recording alone is about 1.5 ms (PLAN-view-prep-rework 8.13, 8.16).
- Nothing of it runs with RT on, so the ray scene cannot be disturbed.

## 2. What exists already (read before designing)

- **The flat group table and its GPU frustum test** (S7-design sections 14-17, `LL_VK_CULL_SHADOW=1`, `rkcullshadow.h`): the groups keep their rows up to date through listener hooks, the viewer sends delta rows to a worker-side copy. **Measured cost of keeping it current: 0.012 ms (still) to 0.019 ms (turning) of main thread and 0.037 ms of worker a frame.** So a table that is always maintained costs nothing worth saving, which is what a live dial needs.
- **A live switch of this kind exists for the main view's cull:** `culluse=0|2` in rt-debug.txt (S7-design section 18, update 2026-10-29) swaps the octree walk and the GPU verdict while running. The cascades, probes, mirrors and snapshots are NOT covered by the table: they still walk.
- **The warning from C2 (S7-design 18.4):** the main view's cull from the GPU verdict did not pay on the main thread (flat pass 0.43-0.61 ms against the 0.33-0.9 ms walk it replaced). What the walk spent was the per-group pointer work (`checkOcclusion`, `markNotCulled`, `markOccluder`) for about 2,000 groups a frame, which C2 still had to do, plus extra groups from the motion margin (+29 % while turning, +0.5 ms of sort and render map). **A GPU verdict alone will not pay for the cascades either; the saving has to come from dropping the per-group work and the draw recording, not from the frustum test.**
- The shadow draw recording goes through render packets to the worker (kind 2 and related shadow packets, `LLVk::Rt::noteCaster` for the ray scene). How much of the recording could be replaced by indirect draws from resident caster rows is not known yet (step 1).

## 3. Steps, each with its gate

0. **The real gap.** Two laps without `FSGpuZoneTimers`, same pose, RT on then RT off: loop ms, worker wait, GPU ms a frame (balance-read). Gate: a number for how much GPU headroom RT off really has. If the loop is within about 1 ms of the GPU time, this plan has little to win on this card and stays as a low-end/hardware option only.
   **Step 0 result (2026-11-04, ground-sit face pose, no timers, `noturn-nt-phaseson1` / `noturn-nt-phasesoff1`):** RT on loop 12.5-12.7 ms (79-80 fps), GPU 12.4-12.6 ms (99 %), main waited for the worker 3.3-3.8 ms a frame. RT off loop 9.4-9.6 ms (104-106 fps), GPU 9.3-9.5 ms (99 %), main waited 1.5-1.8 ms, worker waited 0.00. **Both are GPU-bound at this pose; the gap is 0.1 ms, not 7 ms (the earlier 55 % GPU figure was the zone timers' main-thread cost).** Moving the casters to the GPU would add work to a saturated GPU here. Gate verdict: no win at this pose on this card; the dial only pays where the worker waits for work (a weaker CPU, a stronger GPU). **The pose was already the crowded one: London City, 77 avatars nearby in the People list (screenshot `noturn-nt-phasesoff1-p1.png`), so a crowd does not make this card CPU-bound.** Verdict: park on this card; revisit only with a weaker CPU or a faster GPU, or after the GPU side gets cheaper.
1. **Inventory of the cascade main-thread work, RT off.** From the existing laps (8.13, 8.16 of PLAN-view-prep-rework) plus one zone lap in a still and a turn: per cascade, the cull job, mark, render map, packet recording, and what part of each is per group (pointer work) and what is per draw. Output: a table of ms that a GPU-driven version could remove, and what it would have to keep. Gate: an honest ceiling (the notes say 0.5-0.7 ms in a turn).
2. **Design only if the ceiling is worth it.** Candidate shape: caster rows resident on the worker (as the ray scene's resident casters already are), one compute pass per cascade that tests the rows against the cascade's box and writes a compact draw list, indirect draws for the cascade pass. The CPU path stays as it is. Questions to settle then: caster state that the CPU cull sets (visible stamps, wake queue, map reuse in a turn, avatar extents), and how the near cascade's reuse works when the draw list is not on the CPU.
3. **The dial.** Same mechanism as `culluse`: a live key plus a setting; both paths must produce the same shadow maps (gate: a pixel comparison of the cascades per frame on a scripted turn, 0 differing casters). The first choice at start-up from the hardware (the GPU class and `mDiscrete`, `LLFeatureManager::loadGPUClass`: memory bandwidth measured at start, scaled by CPU speed), then adjusted from measured waits: GPU path while the main thread is the limit and the worker waits for work; CPU path when the main thread waits on the worker or GPU use is near 100 %. Hysteresis (seconds, not frames) so it does not flap. The log says which path ran and why, so laps record it.
4. **After it works:** the dial pattern is the template for other shared work (below). Keep one rule: a second path only where the hardware differs or a measured wait says so; the loser of any pair is deleted after a week unless it is the hardware fallback.

## 4. What it opens (only if steps 0-3 hold)

- The probes, mirrors and the exit snapshot could use the same table for their culls (S7-design 18.2 says they walk until they have their own tests).
- The Hi-Z group boxes already read the same table.
- A measured-wait dial for other shared work whose two paths already exist or are cheap to add; none is chosen here.

## 5. Not decided / risks

- The margin policy of the GPU verdict is a pop-in question (S7 15.3: factor 1 or 0.5); a cascade culls against a box, not a frustum, and its maps are reused in turns, so the margin has to be re-measured for cascades.
- A low-end GPU is GPU-bound: the dial must be able to stay on the CPU path there, and the start-up class is only a guess for the first seconds.
- The GPU-side cascade work competes with the 2.0 ms the cascades already cost the GPU; the dial moves only the casters' selection and recording, not the raster.
- Nothing here is measured for a weaker card; the matrix of low-spec tests is deferred (open items 2026-10-25).
