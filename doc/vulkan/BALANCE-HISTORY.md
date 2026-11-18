# Balance history: where the time goes between the main thread, the render worker, the pool and the GPU

A dated record, newest at the bottom. Each entry is two plain laps of the same scene, RT off and RT on, with no zone timers (zone laps inflate the main thread, see PLAN-view-prep-rework.md for per-zone numbers). Not a fps target: a record to compare the next entries with.

How it is taken: `<work>\h6-fixed.ps1 -Mode phasesoff` / `-Mode phaseson` (`RK_VK_PHASES=1`, and `RK_VK_RT=0;RK_VK_RT_SHADOWS=0` or `=1` set explicitly: the viewer turns RT on by itself when the saved `RKVkRayTracing` is on and neither variable is set, so a lap named "base" or "off" is **not** RT off unless the variables say so; found 2026-11-01), read with `python <work>\balance-read.py <run name>`. The lap logs in at the last location, stands still 60 s, turns left 60 s, returns, stands still 120 s; the table has the three phases (the first seconds of each are skipped). **Every entry must show its RT state from the log**: RT off means no `rt scene (mode` lines and no `rt scene` passes in the GPU time list.

Reading the columns:
- **loop**: ms between frame starts on the main thread (equal to the worker's present interval here, so the pipeline is in step).
- **main waited for worker**: ms a frame the main thread was blocked on the worker (idle headroom of the main thread). **worker waited for work**: ms a frame the worker had nothing to do (idle headroom of the worker). A side with 0.0 is saturated.
- **GPU ms**: GPU time per viewer frame from the timestamps (a little above an unprofiled run); **GPU % of loop** is that over the loop. 100% is GPU bound.
- **GPU util**: nvidia-smi, once a second.
- Thread processor time is not a busy measure: the main thread and the worker read about 100% because they spin instead of sleeping, and the pool's seven workers spin for 150 us before they sleep (17-36% there is mostly spinning). Use the waits.

## 2026-11-01, commit 4b69094c

Scene: the plaza, last-location login, camera at the lap's start pose; DLSS on (render 1713x898, output 2576x1360); the avatar count was not taken in these laps (54 were counted in the avatar timers lap earlier that day, and Adam saw 90+ in the plaza). Settings that matter: texture jobs on (rigged faces in), velocity job on, own avatar's blend in the batch, UI record cache on, far cascade reuse interval 4 from cascade 2, point cloud and cascade cull split on, AO noise moving with DLSS, lightmap blur on with the cascade shadows (RT off) and off with RT sun shadows (RT on), avatar shadow cone 0.008 m / 8 rays / strength 1, hair and other defaults of the build.

| set | phase | loop ms (fps) | main waited for worker | worker waited for work | GPU ms (% of loop) | GPU util | limit |
|---|---|---|---|---|---|---|---|
| RT off (`bal1-phasesoff1`) | still 1 | 10.56 (95) | 1.01 | 0.00 | 9.48 (90%) | 90% | GPU, with ~1 ms of main thread headroom |
| | turn | 15.50 (65) | 2.12 | 0.07 | 9.07 (59%) | 62% | the worker: it never waits for work and the GPU is only 62% busy; the loop is the worker's CPU time (the four cascades re-render every frame in a turn) |
| | still 2 | 10.76 (93) | 0.21 | 0.06 | 9.70 (90%) | 91% | GPU and the CPU side about equal |
| RT on (`bal1-phases1`) | still 1 | 12.52 (80) | 1.87 | 0.00 | 12.25 (98%) | 98% | GPU; the main thread has ~1.9 ms to spare |
| | turn | 12.90 (78) | 1.06 | 0.51 | 10.82 (84%) | 85% | between: GPU 85% busy and both CPU sides have a little slack |
| | still 2 | 12.65 (79) | 1.75 | 0.02 | 12.42 (98%) | 98% | GPU; the main thread has ~1.8 ms to spare |

GPU passes (the last report of each lap): RT off: DLSS 1.30 ms, scene table alpha run 1.12, the 2576x1360 post pass 1.07, the sun lightmap/AO pass (`1713x898 c1 #0000`) 1.05, the G-buffer 0.89, alpha run rigged 0.49. RT on: the sun pass (RT) 2.49 ms, alpha run 2.08, DLSS 1.28, rt scene builds 0.96, alpha run rigged 0.74, G-buffer 0.72.

GPU clocks 2745-2765 MHz in every lap, power 104-143 W (RT off turn 104 W, the GPU is not the limit there).

What it says:
- With RT on the GPU is the limit in the stills (98% busy; 12.3 ms of a 12.5 ms loop), and the main thread has about 1.8 ms of headroom. RT costs about 2.7 ms of GPU a frame (the sun pass 1.05 -> 2.49, the alpha passes +1.0 for the hair and other blended avatar pixels, the ray scene builds 0.96) and about 2 ms of loop.
- With RT off the stills are close to GPU bound too (90%, 9.5 ms of 10.6), with the main thread nearly at its limit (it waits for the worker 0.2-1.0 ms): the CPU side and the GPU are about balanced.
- In a turn, RT off is the worker's CPU time: 15.5 ms with the GPU 59% busy. RT on in the same turn is 12.9 ms: its cascades are not drawn (the ray scene takes the casters), so the worker has far less to do. A turn is where a CPU win would show with RT off.
- Not measured here: the main thread's own busy time by piece (the zone laps in the plan), which stay the place for that.

Earlier laps of the day were not labelled with their RT state: those from the cone tests onward (the avatar timers lap, the wake, point cloud and cascade cull laps) ran with RT on because the saved setting was on; each pair is valid within itself.
