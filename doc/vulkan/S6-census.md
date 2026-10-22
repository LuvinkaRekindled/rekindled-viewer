# S6 census: what is left in the GL-shaped stream, and what the worker spends (2026-10-22)

Question: is S6 ("render thread loop and removal of the GL-shaped stream", PLAN-gpu-scene.md) worth building, and what has to come before it? Facts first, then what they say, then the order I propose. Runs: London, ~99 avatars, build after the S5 crash hunt (commit 0446ef56) plus census counters. Logs `<work>\run-logs\s6a..s6e.log`. Switches: `LL_VK_OPSTATS=1` (stream and worker counters, every 600 frames), `LL_VK_UNICENSUS=1` with `FS_CPU_ZONES=1` (per-zone counts, section 2). One run each: numbers are good to about 10 %, not better.

## 1. Main thread: what writing the stream costs

Per frame (run s6a, no phases, loop 15.3 ms):

- 18.2k calls from the viewer become 9.0k records; **writing them costs 0.60 ms** (33 ns a call).
- The front-end entry points (what the viewer code spends before a record exists): shader bind 0.12, uniform writes 0.38 (20k calls), uniform matrices 0.11, texture unit binds 0.05 + 0.07, setBuffer 0.10 (1.8k), draw 0.34 (3.2k), the rest 0.1: **about 1.3 ms**.
- Together about 1.9 ms of a 15.3 ms loop. Before S5 (Oct 21 run) the same entry points were about 2.2 ms plus 0.85 ms of record writing: S5 took about 1.1 ms of this out.
- The viewer's own code that computes those values is not in these numbers; deleting the stream would not delete it.

## 2. Where the remaining records and draws come from (run s6b, innermost CPU zone, per frame)

Totals: 8,587 records, 3,124 draws, 1,702 setBuffer, 3,676 syncMatrices, 15.7k uniform writes (4.9k changed), 12.2k texture binds, 989 shader binds.

Records: post pool alpha (post water) **2,540** (the S5 alpha runs: no draws of their own, about 9 records per shader bind), renderGeomDeferred 1,308, bindDeferredShaderFast 925, local lights 325, terrain 328, UI panels ~400 together, projectors 248, bindDeferredShader 295.

Draws still written the GL way (about 3,100):

| group | draws | what |
|---|---|---|
| scene geometry in group buffers that no slice took | **~1,800** | renderGeomDeferred 434 (why not served: not yet checked), post-pool fullbright 359, terrain 316, alpha-mask pool 247, glow pool 225, fullbright alpha mask 89, glTF 59, avatar ~60, bump / simple ~30 |
| UI / HUD | ~600 | main panel 287, name tags 78, toolbars and small panels |
| lighting volumes | ~550 | projectors 238, local light boxes 313 |
| post, water, sky | small | |

`kGeoTypes` (pipeline.cpp) has 13 pass types (simple + the material variants). Fullbright, glow, bump, terrain, the alpha-mask and glTF pools are not among them. The plan says UI, terrain, water, sky, HUD, text stay on the old path "until they get their own entries"; terrain is the only one of those that is group-buffer geometry.

## 3. Worker: where its time goes (runs s6d / s6e, per presented frame, wall 15.3 to 15.9 ms)

- **Operations 12.6 to 15.1 ms of 15.3 to 15.9 ms.** Worker idle 0.6 to 1.9 ms a frame; the main thread never waits for the worker (0.0). The worker is nearly the limiting thread now, not the main thread.
- `shadowPackets` (every typed packet record) 5.2 to 6.3 ms; `rtScene` 2.4 to 2.9 ms in 2 calls (serial); `draw` (the GL-shaped draws) 2.5 to 2.6 ms for ~2,700 (0.96 us each); present 0.7 to 2.0; the rest under 0.3 each.
- Packet kinds (the per kind timer runs per command buffer submit, and there are **2 submits per presented frame**; multiplied up): alpha runs, kind 29, **about 3.7 ms for ~740 records (5 us each)**, prepare (kind 21) 0.8 ms (one 810 us call), G-buffer table draws (kind 22) 0.5 ms (26 calls), rigged note kinds 5 / 6 / 7 about 0.7 ms together, everything else under 0.1 each. Sum 6.0, equal to the dispatcher's figure (my first reading printed half of this: it divided by submits, see the note at the end).
- **`gMutex` is not a blocker:** 0.02 ms a frame of waiting before packet records.
- Reading the blob costs nothing (30 ticks); no record arrives without data.

## 4. What this says

1. **Deleting the GL translation is not where the worker's time is.** The GL-shaped `draw` is 2.6 ms; the largest single item is my S5 alpha run records (3.7 ms), then the ray scene (2.9 ms), which is not stream-related at all. S6 as written (delete the translation) would not remove either.
2. **Serving the rest of the scene geometry from the table (about 1,800 draws) pays on both threads:** main thread: draw / setBuffer / syncMatrices / the Fast binds per pass (guess 0.4 to 0.6 ms, not measured); worker: up to 1,800 x 0.96 us = 1.7 ms of `draw`, less what the table draws cost instead (S3 measured 0.68 -> 0.26 ms worker per frame for the types it took). It also removes most of the state that crosses pass boundaries.
3. **Multi-threading blockers seen so far:** the order-dependent stream state of the ~3,100 GL-shaped draws (UI, lighting, post, leftover geometry); single-owner tables (retire / pins / bindless sets written by the worker thread; kept single-writer on purpose); the ray scene call (serial, 2.9 ms); and per frame work that is one big record (prepare, 0.8 ms). Not a blocker: `gMutex`.
4. The second command buffer submit per frame (2 a presented frame) is from pass ends ("command buffer 2.0" in the pass census); worth knowing when passes become self-contained.

## 5. Proposed order for the rest of S (Adam's call; threading plan comes after)

1. **S3 extension: the remaining scene geometry pass types**, one type at a time with a check first (can the existing pull programs read it? per-draw values?): start with the 434 draws left in renderGeomDeferred (find out why they are not served), then fullbright, glow, alpha-mask pool, glTF, bump, terrain. Gate: draws a frame, main-thread zones, worker `draw`, pictures equal.
2. **Alpha run cost on the worker (S5 follow-up):** 740 records a frame at 5 us. Options: fewer records (merge fullbright and lit programs per the S5 design note, merge runs across groups), cheaper per record (pass tables made 16k times in a run, begin record work). Measure before choosing.
3. **S6 proper**, rescoped: passes become self-contained descriptions for every pass whose draws are typed by then; the GL-shaped stream stays only for UI, lighting volumes and post until they are typed (separate decision). Deleting translation code for what moved is cleanup, not a gain.
4. **S7** (Hi-Z, GPU LOD, group visibility, OIT) as planned.
5. Then the threading plan (recorders per pass or pass slice, single-owner tables, ray scene off the render thread).

## Note on the census counters

`packet kinds, worker` (llvkworker.cpp, `on_shadow_packets`) counts per command buffer submit; the viewer's frame counter there is the submit count, 2 a presented frame. `LLUniCensus` now also counts records, draws, setBuffer and syncMatrices per zone (llgpuzonetimer.cpp, lluniformcensus.h, llvkasync.cpp `noteRecord` / `frontStart`).
