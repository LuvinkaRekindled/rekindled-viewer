# Render threads: using the CPU (the "worker fix", full redesign)

Status: roadmap, 2026-10-10. Decided with Adam: go for the full redesign (not the quick pool-only fix). Thread budget: a shared pool of `max(1, hardware threads / 2 - 1)` threads (15 on his 32), a setting, so other programs keep the rest.
Builds on `PLAN-native-renderer.md` (N2 frame structure, N3 GPU scene, N4 GPU-driven passes) and `PLAN-draw-records.md`. This file says what to do first, in measured ms.

## Where the time is (measured 2026-10-10, ray shadows mode 1, midday, 2558x1360 DLSS, about 7-8k draws/frame)
Worker thread (one serial thread, 20.4 ms/frame in the first run, wall 20.65; the main thread waited for it 3.9 ms/frame, the worker never waited for work):

| Part | ms/frame | Notes |
|---|---|---|
| `draw` operations | 13.4 | 7.4k draws at about 1.8 us: G-buffer pass 7.9 ms (3.9k draws), forward/alpha pass 6.4 ms (3.2k draws), lighting 1.3, UI/final 1.0, probes 0.6 |
| of which renderer apply+record | 9.0 | uniforms 2.1, texture layouts 0.7, vkCmdDraw 0.6, vertex input 0.4+0.8, pipeline 0.4, textures 1.1, about 3.9 not attributed |
| `rtScene` | 2.2 | 1.4 ms/call after stage II of the CPU work, 1.5 calls/frame |
| `shadowPackets` | 1.8-2.1 | 52-60 records/frame, 34 us each |
| `arrayRun`, `glDrawRangeElements`, present | 1.0, 0.65, 0.47 | |

Why more threads do not simply speed up the draw path: the worker replays the main thread's stream of GL-shaped calls (bound program, uniforms, textures, blend, targets), and each draw's Vulkan commands depend on that accumulated state. Only the final vkCmd recording (about 2-4 ms) is separable without changing the model; the translation (what to record) is serial by construction. The ground-up answer is draws that carry everything they need (no state to accumulate), recorded by any thread: that is what the stages below build.

## Stage R1: draws that carry their state, starting with the biggest passes (measured, cheap, already half built)
1. **Deferred geometry packets on by default** (`FSDeferredPackets`, built 2026-10-06, off since the main thread was the limit then). A/B 2026-10-10, same sun (61.5 degrees), same spot: G-buffer pass 6.4 ms / 3257 draws -> 4.3 ms / 1636 draws, worker 17.4 -> 15.9 ms/frame, frame average 17.6 -> 16.1 ms (-9%), main thread wait 2.5 -> 1.1 ms/frame. Needs Adam's look (materials, glTF, rigged, normal/spec maps) before it becomes the default.
2. **Alpha/forward pass as ordered records.** 3.1k draws, 6.3 ms, now the largest piece. Alpha cannot be reordered, but consecutive draws with the same program can be one record that draws its entries in order (same idea as the shadow alpha packets). Gate: draws and worker ms of the `c1 d` pass.
3. **Remaining plain passes** (lighting full-screen quads, UI, probes) are small; leave.

## Stage R2: a job pool and independent work off the serial worker
Pool of the size above (`LLVk::Jobs`, std threads, no viewer dependencies). First jobs, each judged by worker ms:
- ray scene preparation (candidate scan, instance and table assembly, expiry), started when the frame's shadow packets are in and finished before `rtScene` records: about 1.5-2 ms/frame;
- shadow packet table building, chunked per record (about 1-1.5 ms);
- compute-skin job tables and avatar scheduling for the ray scene;
- pipeline pre-warm and SPIR-V work already runs in the background; fold it into the pool.
Rule: jobs only read data that is immutable until they finish (the frame's notes) and write into their own staging memory; the serial worker does the cheap final copy/record.

## Stage R3: two-phase draw path (resolve serially, record in parallel)
For the passes that stay as individual draws (alpha, UI, odd materials): the worker walks the stream and resolves state serially but cheaply into compact draw records (pipeline handle, uniform ring offset, texture slot set, vertex bindings, draw args); pool threads record chunks of records into **secondary command buffers** (dynamic state set at the start of each chunk); the primary executes them in order inside the pass. Barriers and image layout changes stay serial (resolve phase, before the pass). Expected 3-4 ms; real risks: bindless slot allocation, uniform ring allocation per chunk, pass begin/end around secondaries. Do only if the profile after R1/R2 still shows the draw path as the limit.

## Stage R4: the structural fix (N2 + N3 + N4)
Pipelined frame (main thread publishes an immutable frame description, a render thread records it while the main thread starts the next), a frame graph (barriers derived), the GPU scene (instance buffer, material table, stable bindless slots) and indirect/GPU-driven passes. The ray scene already is a retained, incremental, world-space scene with persistent instances and stable buffer addresses (arena), so it is the proof of concept for the scene side. This is the multi-week part; R1-R3 pay for themselves meanwhile and every record format they introduce is the one the GPU scene needs.

## Order and gates
R1.1 (A/B done, needs Adam's visual check) -> R1.2 -> R2 (pool + ray scene prep) -> re-profile -> R3 or R4 by the numbers. Each step: worker ms/frame by operation, frame pacing (avg, p95, main thread waited), draws/frame, the regression checklist and a before/after snapshot. The main thread becomes the limit when the worker drops below about 15 ms (it waited 1.1 ms/frame in the packets run), so from there main-thread work (object update, avatar update, culling) is what R4 and the earlier N-stages address.
