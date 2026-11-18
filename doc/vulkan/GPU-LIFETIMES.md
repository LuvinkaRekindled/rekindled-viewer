# Lifetimes: what may be handed to the GPU or to another thread, and for how long (revised)

Revised 2026-11-09 from `GPU-LIFETIMES.md` (first version, commit `933506c857`), after reading the code at `cdbe206729` (`indra/llrender/rkvk*.cpp`, `rkvkhandles.h`, `rkvkbridge.*`) and the incident notes since 2026-10-24. Nothing in the code was changed for this document. Every statement about the code is marked in section 4 as read, partly read or not read; section 5 lists what the reading found that looks wrong. None of the findings has been tested in a run.

## 0. What changed from the first version

1. **Scope.** The first version covers one family of bug: the CPU changes something a submitted frame still reads. Going by the notes, that family is 5 of the 19 incidents listed in section 6. Four more families are added here: stale names (section 3.2), GPU results used for the wrong state (3.3), CPU threads and ownership (3.4), and upkeep that only one mode runs (3.5).
2. **Enforcement.** The first version proposed to detect stale reads. Most incidents were bad writes. Section 7 aims at making the wrong write impossible to express, and at one shared answer to "has the GPU finished frame n" instead of the eight separate ones the code has now.
3. **Units.** "Frame" means a renderer frame (`Context::getFrameNumber()`), not a viewer frame. A viewer frame is usually two renderer frames (the presented one and the off-screen one after it). The palette depths (F + 2, F + 3) and the ray scene's "two frames earlier" are all renderer frames (`av_tag` is the frame number + 1).
4. **Corrections.** A poison mode exists already (`LL_VK_ARENA_POISON=1`), so does a loud stand-in (`LL_VK_STANDIN_MAGENTA`); the first version said poison was not proposed. Deferred destruction runs when the slot comes round the *second* time, not the first. `Context::frameFinished` is not always fence based (finding 5.1).
5. **The audit is wider but not finished.** The channels the first version listed as unread are in the table now; what is still unread is listed in section 9.

## 1. The families

Every incident in section 6 is one of these. A to E are what this document gives rules for.

- **A. Written or freed under a frame in flight.** The CPU rewrites, frees or reuses something a submitted frame (or a submitted part of one) still reads.
- **B. A name that no longer means what it was noted for.** A record, cache or pointer outlives the thing it names: the buffer was respecified, the entry is gone, the layout is another one, the cache key missed an input.
- **C. A result from the GPU used for another state.** A readback is read before the GPU wrote it, or is applied to a camera or scene that has moved on.
- **D. Two CPU threads, or one thread and a dangling pointer.** A pool job writes shared state, the main thread writes what a job reads, a record keeps a raw pointer past its owner.
- **E. Upkeep that only one mode runs.** An eviction, sweep or retire step sits inside a pass that some mode skips (RT on, split frames, snapshots, another frame count).
- **Outside** (no rule here will catch them): garbage in asset data, the order of updates within one thread's frame, plain picture logic in a shader.

## 2. The model of time

- **Frame slots.** `Context::frameCount()` slots (F, 3 by default, `LL_VK_FRAMES` 2..6). Frame n uses slot n % F. `beginFrame` of frame n waits for the fence of frame n - F, then reuses its slot.
- **In a frame and between frames.** While frame n is recorded, frames up to n - F are finished. Between `endFrame` of n - 1 and `beginFrame` of n the frame number is already n but nothing was waited for: only frames up to n - F - 1 are known finished. The worker does real work there (`after_frame`, buffer writes, renames, deletes). Every lifetime test must be right in both states; two of the tests in the code differ by exactly this one frame (finding 5.1).
- **Split frames** (`rtearly=3`, RT on). `Context::splitFrame` submits the first part of the frame while the rest is still recorded, and the ray scene work runs on the compute queue between timeline values. `frameSplit()` is true from then on. Anything the submitted part reads is in flight while the same frame is still being built. The frame's fence is signalled by its last part, which waits for the compute work.
- **Mid-frame flush** (`flushFrameAndWait`): submits what is recorded, waits, carries on in the same slot. After it nothing recorded earlier in the frame is in flight.
- **Deferred destruction.** A thing given up in frame n goes into the slot's list, moves to the slot's late list when the slot is next used (frame n + F), and is destroyed when the slot is used again (frame n + 2F): a copy or draw that names it can have been recorded into a later frame. A thing given up between frames goes with the next frame recorded.
- **Threads.** The main thread; the fork-join pool and the async pool (cull, shadow prep, palette jobs, builders); the render worker (plays the command queue, owns the renderer and every Vulkan call of a frame). The viewer hands the worker *copies* (queue records with their data, sub-streams spliced in order), with two exceptions that hand over memory itself: the palette arenas and the texture upload pixel hold.

## 3. The rules

### 3.1 Handing data to the GPU (family A)

- **P1. Own slot, write once.** Scratch of this frame slot (uniform ring, vertex ring, staging), written once, read only by this frame's commands, reused when the slot's fence has passed. If something reads it in a later frame, the ring is deeper by that many frames, and the reader is named where the depth is defined.
- **P2. Master and per-frame copy.** The CPU keeps a master the GPU never sees (it has no GPU address). Each frame copies what it needs into its own scratch when it builds its commands.
- **P3. Rename on write.** A buffer a frame in flight may read is never written in place: the write goes to a new range, the old one is given back by frame.
- **P4. An index is not reused until retired.** A descriptor index, handle, row or range goes on a retired list stamped with the frame and returns to the free list when that frame is finished. A reader that arrives late sees the placeholder, never another object. Writing a *fresh or retired* index while frames are in flight is allowed (nothing can reach it); writing a reachable one is not.
- **P5. Written by the GPU, in command order.** Device buffers get their data by a staged copy recorded in the frame, and compute passes write their outputs (skinned vertices, sorted particle rows, cull bits) in the frame. The order of the commands is the lifetime rule, so the only hazard is a second queue (the split) and a CPU that also writes the same buffer.
- **P6. Memory the GPU reads only by address is shown to the GPU first.** A new host block is filled once by the GPU before anything is placed in it (`LL_VK_HOST_BLOCK_GATE`). This is the rule the device losts of 2026-10-25 to 31 led to; its cause on the driver side was never proven.

**Never:** change anything a submitted frame part reads, including a table entry "for early visibility" after `splitFrame`; reuse an index before retirement; keep a GPU address in a record that lives longer than the depth of its channel; read write-combined upload memory on the CPU (the one counted exception is the rename copy without a system memory copy); free or reuse on the strength of "nothing references it now"; fall back to a weaker rule without a counter and a log line.

### 3.2 Names and caches (family B)

- **G1. Generation.** Anything that remembers a buffer, entry or object by name or pointer across frames records the generation it was noted under (epoch, moves, cache generation, serial) and checks it where it is used, with a counter and a log line when the check fails (`AUDIT-buffer-generations.md` has the list of such memories; this is its rule).
- **G2. Kind.** A name also carries what the data *is*. A reader that takes a buffer by address must check that the buffer holds what it expects (positions, not particle rows). Whoever adds a new kind of content lists every reader of that channel first (the ray scene, the shadow casters, the velocity pass).
- **G3. Cache keys.** A cache key holds every input that changes the result (the shader cache key missed one and served stale SPIR-V).
- **G4. A weak handle needs an owner.** A handle that owns nothing (`RKVBRef`) is only good while something else owns the object; a standalone record must own it itself.

### 3.3 Results that come back from the GPU (family C)

- **R1. Published by fence.** The CPU reads a result only after the frame that wrote it is finished: by slot (read when the slot is reused) or by a publish step that asks whether the frame is finished.
- **R2. Stamped with what it was computed for.** A result carries the camera, the table version or the serial it was made for, and is used only for that. A verdict for a camera that has moved on is not a verdict.

### 3.4 CPU threads and ownership (family D)

These are the rules `PLAN-forkjoin-candidates.md` and `PLAN-render-threads.md` already state; they are here because the same incidents list needs them.

- **T1. Compute, then apply.** A pool job writes only memory its item owns, per-thread lists and plain delta records. Shared state (queues, group flags, octrees, the rebuild lists, GPU records) is written by the main thread from the records afterwards.
- **T2. Frozen inputs.** What a phase's jobs read is not written by anyone until the phase has joined. A main-thread step that writes such data runs before the fork or after the join, never beside it.
- **T3. Records own what they name.** A record that outlives the statement it was made in (a deferred op, a queue entry, a fragment item) holds a counted pointer or a handle with a generation, never a raw pointer.
- **T4. A threading primitive is stress tested alone.** The pool, a lock-free ring, a claim word: a standalone test (random counts, every index once, nothing in flight after the join) before theories. That test found the fork-join double claim in seconds after three wrong readings of the code.

### 3.5 Upkeep and modes (family E)

- **M1. Upkeep does not live inside a pass a mode can skip.** Eviction, sweeps and retire steps run from a place every mode reaches once a frame (a pump), not from the cascade path, the lighting path or the RT path.
- **M2. Each mode that changes the frame model is in the test list.** RT on with the split, RT off, a snapshot (a second render inside the frame), a teleport, `LL_VK_FRAMES` 2 and 6.

## 4. The channels

F = `frameCount()`. "Read": **yes** = the code was read for this document; **partly**; **no** = taken from the first version or from comments.

| Channel | Carries | Writer, memory | Rule | Given back | Read |
|---|---|---|---|---|---|
| Uniform ring `mRing[slot]` | loose uniform blocks | worker, host | P1; full: the draw is dropped and counted (`mRingExhausted`) | slot's fence | yes |
| Vertex / scratch ring `mVertexRing[slot]` + overflow chunks | the per-frame copies (scene rows, resident tables, cull table, light rows, Hi-Z boxes, particle sort jobs), command lists, immediate vertices; 56 call sites in the worker | worker, host | P1 (and the copies of P2) | slot's fence | yes |
| Palette ring `mPaletteRing` | joint palettes of rigged draws | worker, host | P1, two frames deep: the ray scene skins with a palette noted up to two frames earlier (`mNotedTag + 2 >= av_tag`); F + 2 slots | by the frame F + 2 later | yes |
| Palette arenas (`paletteArena*`, kinds 39/40) | the same palettes, written by the viewer's jobs straight into mapped GPU memory | pool jobs, host; one arena per viewer frame | P1 by viewer frame with a busy flag; ids for last frame's palette (two id tables) | renderer frame >= last record + F + 3 | yes (bridge), partly (readers) |
| Handle table and maps (`HandleTable`, `mHandleMap[slot]`) | handle -> descriptor index | worker; one map per slot | P2 (log replayed per slot) + P4, all inside one class; no patch of the current map once `frameSplit()` | by `frameFinished` | yes |
| Bindless descriptor sets (one per slot) | the descriptors | worker, at `bindlessFlush`, into every slot's set | P4: only fresh or retired indices are written while frames are in flight; one exception, the release guard, writes the placeholder into a reachable index | never (indices are) | yes |
| View release guard (`mViewUsers`) | which handles and descriptors showed a view | worker | bookkeeping for the guard | at destroy | yes |
| Host-written vertex / index buffers (arena Host ranges) | mesh data the CPU writes | worker (`write_buffer`) | P3; "in flight" = stamped by a draw within the last F + 1 frames, or listed by a scene fragment | old range by deferred destruction | yes |
| Device-path buffers | mesh data in video memory | worker; staged copy in the frame | P5 (`updateBuffer`, flushed before a draw that reads it); writes outside a frame wait for the next one | deferred destruction | partly |
| Arena blocks and ranges | the storage of the two rows above and of acceleration structures | VMA virtual blocks | free is deferred; P6 for new host blocks | two rounds of the slot; empty blocks trimmed later | yes |
| Scene table rows (`sGeo.mTCpu`) | rows of every draw | CPU vector only | P2 (`c0ac19bb60`) | slot's fence | no |
| Resident shadow table | per-draw rows kept between frames | CPU master | P2 (`b7cf8adc6b`) | slot's fence | no |
| Resident alpha rows and index pool | rows; a persistent GPU index pool | worker | rows P2 (copy per frame and version); pool ranges and slots P4 (`frame >= retired + F + 1`) | by that test | partly |
| Staging (`allocStaging`) | uploads | worker, per slot | P1 | slot's fence | yes |
| Deferred destruction (`destroyLater`) | everything destroyed | worker | two rounds of the slot (see section 2) | by definition | yes |
| Ray scene (`rkvkrt.cpp`) | structures, skinned output, group ranges | worker; Accel arena | structures P3 + deferred destroy; skin jobs stamp their source buffers (`markBufferUsed`); ranges G1 (epochs) | deferred | partly |
| Compute queue work (split frames) | ray scene skin, builds, top level | worker, second queue | P5 across two queues: ordered by timeline values (hazard list in `PLAN-async-compute.md`) | frame's fence | no |
| Hi-Z results ring (kind 34) | verdict words per box | GPU writes slot serial % 8 of cached host memory | R1 (`hiz_poll` publishes when the frame is finished), R2 (`1f35993aae`); at most 6 serials unread | when the main thread marks the serial done | yes |
| Cull results ring (kind 35) | bit sets | GPU; table master on the worker, copied into scratch per record | R1, P2 | as Hi-Z | yes |
| Mip feedback (`mFb`, `mFbRead[slot]`) | smallest texel area per handle | GPU; copied to the slot's buffer at frame end | R1 by slot | slot's next use | yes |
| Particle sort (kind 36) | sorted rows | GPU compute writes the row buffer; jobs in scratch | P5 + P1; both buffers stamped | with the buffers | yes |
| Light volume rows (kind 33), texture matrices in records | per-record rows | worker, scratch | P1 | slot's fence | yes (lights), no (texture matrices) |
| Texture upload pixel hold | decoded pixels by pointer | main thread memory, read by the worker | the caller keeps them alive and unchanged while the hold is set | caller | no |
| Upscaler resources (DLSS, FSR, NR) | images given up on a resize | worker | own retired lists, `frameFinished` | by that test | no |
| Occlusion query results, uniform block sets per slot, HUD and impostor record lists | | | | | no |

## 5. What the reading found (not tested, except 5.1)

**5.1 was confirmed and fixed the same evening.** `Context` keeps `mCompletedBelow` (every frame below it has been waited for, set in `beginFrame` after the slot's fence) and `frameFinished` answers from it or from the frame's own fence. A counter logs each time the old test would have answered "finished" for a frame still on the GPU: in Adam's first run on the fix (a busy spot, about 125 fps) it counted 6000 in 133 seconds, about one presented frame in three, every one with the frame number exactly F ahead as predicted. So before the fix about a third of the Hi-Z results were published before the GPU had written them. The worker's Hi-Z numbers are unchanged by the fix (22,200 records, 2 pending, nothing refused). What it does for the blinks is Adam's eye to say.

1. **`Context::frameFinished(n)` answers "finished" one frame too early between frames.** It returns true when `mFrameNumber - n >= F` "because the slot has been reused", without looking at the fence. In a frame that is right. Between frames the frame number is already the next one and the slot has not been reused yet, so frame n = number - F is called finished while it may still be on the GPU. The one caller I found in that state is `hiz_poll()` in `after_frame()` (after a presented frame's `endFrame`): it can publish Hi-Z verdicts of a frame the GPU has not run yet, and the main thread then copies whatever the slot holds at that moment (the verdicts of the record eight serials earlier), while it believes they belong to the camera of the new serial, so the camera check of `1f35993aae` does not see it. The Hi-Z record is sent in the presented frame n; the exposed call is the `after_frame()` of presented frame n + 2. When the GPU is the limit the worker blocks in the next `beginFrame` on that very fence, so frame n should usually be unfinished at that call; this is reasoned from the code, not measured. Whether it is behind any of the object or avatar blinks is not known. The handle retire and the view guard call it in a frame (or right after the wait) and are right. A naive fix (check the fence instead) is wrong inside `beginFrame`, which has reset that fence by then; the fix is a "completed through frame n" number set where the wait happens (step 1 of section 7).
2. **Eight separate answers to "is frame n finished".** `HandleTable::retire` (asks `frameFinished`), `write_buffer` (`frame < last + F + 1`), `geo_in_flight_impl` (listed + F + 1), the resident alpha ranges and slots (`frame >= retired + F + 1`, three places), the palette ring (frame % (F + 2)), the palette arenas (record + F + 3), deferred destruction (two rounds of a slot), Hi-Z / cull / upscalers (`frameFinished`). Each was written at a different time; finding 1 is what that costs.
3. **"In flight" for a buffer is a stamp set by hand.** 25 places in `rkvkworker.cpp` set `mLastUseFrame = frame_now` after taking a buffer's address for a draw, and the ray scene calls `markBufferUsed`. A new record kind that forgets the stamp makes `write_buffer` write in place under the GPU, with no error.
4. **`write_buffer` writes in place when it cannot get a fresh range.** If the buffer is in flight and `arenaAlloc` (or `createBuffer`) fails, nothing is renamed and the `memcpy` below still runs on the old range. No counter, no log line.
5. **The palette ring's fallback is the vertex ring.** When the palette ring is full the palette goes to the vertex ring, whose depth is zero, so the ray scene can read it after the slot was reused (the 2026-11-04 spikes again). It is counted and warned; the palette-ring notes say 0 fallbacks in the proof runs.
6. **The view release guard forgets.** `mViewUsers` is cleared whole when it passes 40,000 entries; views noted before that are not guarded any more. Not counted.
7. **Palette arena takeover.** An arena held by an earlier viewer frame that no record has named yet is taken over once it is `count` viewer frames old ("an aborted frame"). That holds only while the worker is never that many viewer frames behind the viewer. Not checked whether the queue's back-pressure guarantees it.
8. **`destroyLater` inside a frame pushes into the slot's list without a lock** (the lock is only on the between-frames list). Safe if every caller in a frame is the worker; not checked for every caller.

## 6. The incidents, by family

Dates and causes are from the notes of those days. "Now" says what stops a repeat: **built** = a mechanism in the code stops it or reports it; **rule** = only a rule in this document; **open** = cause not known well enough.

| # | Date | Symptom | Family, rule broken | Fix | Now |
|---|---|---|---|---|---|
| 1 | 10-24 | textures flash a wrong colour at load and at every resolution change | A: rows shared by all frames against per-frame descriptor sets (P2, P4) | texture handles H0-H6 | built (`HandleTable`) |
| 2 | 10-25..31 | device lost, fault in the first MB of a host block made 1-3 s before | A (P6), driver side unproven | block gate `42d1730a` | open, watched |
| 3 | 11-02 | stray or missing one-frame sun shadows | A: master table rewritten in place (P2) | `b7cf8adc6b` | built for that table; rule for the next one |
| 4 | 11-04 | spikes from skinned avatars (case 2 of that hunt) | A: scratch read two frames later than its depth (P1) | palette ring, `abcf3d94ec` | built; fallback is finding 5.5 |
| 5 | 11-09 | device lost in a crowd, RT on | A: map entry written after the split (P2) | `0762a0fe0a` | built at that one call site |
| 6 | 11-01 | login crash guard, ray scene "bad range" warnings | B: pointer and ranges kept across a buffer's respecify (G1) | `c1ecf39b`, epochs (plan 8.33) | built (epoch checks, counters) |
| 7 | 11-03 | spike shadows, a tree as a block | B: particle row buffers named as alpha casters (G2) | particles excluded as casters | rule |
| 8 | 10-24 | device lost after a shader change | B: cache key missed an input (G3) | key extended | rule |
| 9 | 11-08 | crash in the glTF preview | B: weak buffer handle with no owner (G4) | the model owns its buffer | rule; the retire log counts such reads |
| 10 | 11-02 | objects blink while moving through foliage | C: verdicts used for a camera that moved on (R2) | `1f35993aae` | built; finding 5.1 is the same channel (R1) |
| 11 | 10-29 | crash on closing a floater, a hang, a crash in a lap | D: fork-join claim word and count read apart (T4) | `762537ad` | built; the stress test is not in the repo |
| 12 | 10-30 | crashes at the turn with the kill stage on | D: pool workers pushed into the wake queue with no lock (T1) | lock in `wakeRequest` | rule |
| 13 | 11-01 | crash in the moved list pass | D: raw pointer across a call that unlinks; deferred avatar ops with raw pointers (T3) | `c1ecf39b`, `e7ff11f744` | partly built (live checks); route still unnamed |
| 14 | 11-04 | animesh joints torn (suspected) | D: main thread wrote joint matrices beside the palette jobs (T2) | the change was reverted | rule |
| 15 | 11-04 | memory grew to 18 GB with RT on | E: the cache's only eviction sat on the cascade path (M1) | `63a4812b9b` | rule |
| 16 | 10-29 | device lost and long freezes at the exit snapshot | E: a second render in the frame (M2); cause partly unknown | `e0920243` | open |
| 17 | 11-04 | spikes from one avatar (case 1) | outside: garbage bind matrices in the asset | scrub in `llmodel.cpp` | n/a |
| 18 | 11-04 | animesh attachment blinks by its host's hover offset | outside: value read before a later step of the same frame | `matchVolumeTransform` | n/a |
| 19 | 11-09 | flecks on shadowed skin | outside: the half lightmap blur tested the wrong pixel | `cdbe206729` | n/a |

**Reading of the table.** The first version's scope was rows 1-5; the two checks it proposed would have reported rows 4 and 5. With the rules of section 3, 16 of the 19 are covered by a rule. Of those 16: 7 are stopped or reported by something already built (1, 3, 4, 5, 6, 10, 11), 2 more would be by step 4 of section 7 (12, 14), 5 depend on someone following a rule or a checklist (7, 8, 9, 13, 15), and 2 have no cause known well enough to write a mechanism for (2, 16). So "most of what we ran into" is a fair claim for the rules, and a weaker one for what the code enforces by itself.

## 7. What would enforce this (proposed, in order; nothing here is done)

Each step is one build and a lap unless it says otherwise. Saving and cost numbers are estimates.

1. **One answer to "is frame n finished".** `Context` keeps the highest frame number known finished, set where `beginFrame` has waited and raised by a fence look when asked between frames. `frameFinished` answers from it (fixes finding 5.1), and the arithmetic of finding 5.2 is replaced by it one site at a time. A side effect: a buffer whose last use is already finished is no longer renamed just because fewer than F + 1 frames have passed, so fewer renames when the GPU keeps up (not measured). **State 2026-11-09:** the number exists and `frameFinished` uses it (finding 5.1); the other sites of finding 5.2 still do their own arithmetic. **State 2026-11-13:** `Context::frameKnownFinished(n)` (the cheap form, no fence look) exists and `write_buffer` asks it instead of `frame < last + F + 1` (the same answer between frames, one frame tighter inside a frame); the handle retire already asked `frameFinished`. **State 2026-11-13, later:** the resident shadow slots (`res_reclaim`), the resident alpha ranges and slots (`alpha_res_reclaim`), the scene geometry table slots and PBR rows (`geo_reclaim`) and `geo_in_flight_impl` ask `frameKnownFinished` too (a thing given up in frame r comes back when frame r is finished: one frame earlier inside a frame than the old `frame >= r + F + 1`, the same between frames). Left on purpose, as depths for late readers and not a finished test: the palette ring (`frame % (F + 2)`) and the palette arenas (held until `F + 3` renderer frames after the last record): they stay until step 7 (depth by the reader's stamp) is taken.
2. **Counters where a rule is weakened.** Findings 5.4 and 5.6 get a counter and a log line (first 20, then every Nth), as 5.5 has. Healthy = 0 in a lap. **Done 2026-11-13:** `write_buffer: ... written in place` (5.4) and `view release guard: ... cleared` (5.6); laps s91 (RT, RT off) and the stress lap read 0 for both.
3. **Taking an address is the stamp.** One accessor on the worker returns a buffer's address for a draw or record and stamps the buffer in the same call; the 25 hand stamps and `markBufferUsed` go through it. A new record kind cannot forget. **Done in part 2026-11-13 (1345481f2a):** `packet_buffer_used(name, frame)` in `rkvkworker.cpp` finds a record entry's buffer and stamps it in the same call; the seven record handlers (alpha shadow, geometry, rigged shadow, avatar velocity, object velocity, particle sort, the first shadow packet record) use it and 14 hand stamps are gone. Left as hand stamps, of other kinds: the classic attribute and uniform paths (`mark_used`, the uniform binding), `markBufferUsed` (the ray scene, 6 calls in `rkvkrt.cpp`), the RT probe jobs, the retained fragments' draws (`p.mPvb`).
4. **Thread phase checks.** A flag says a parallel phase is running. The shared writers that the incidents hit (`wakeRequest`, `markRebuild`, the queue pushes, drawable link and unlink, the joint matrix writers) check "main thread, or no phase running" and count a violation in every build. Row 12 would have shown on its first frame. The fork-join stress test of row 11 goes into the repo so it can be run again after a change to the pool. **Done in part 2026-11-13:** `indra/llcommon/rkphasecheck.h`: `RKPhaseCheck::sPhaseDepth` (the fork-join pool counts a batch from the slot taken in `launch` to the end of the join) and `rk_phase_check("name")`, which counts and logs (tag PhaseCheck, never stops the call) a call from a thread that is not the main thread, or from the main thread while a batch runs. It sits in the direct writes of `unlinkDrawable`, `markMoved` (the moved list and the moved bridge list), `markPartitionMove`, `markMeshDirty` and `markRebuild` (the group queue); the totals by writer are one line `thread phase check totals: N (healthy = 0)` in the periodic fork-join report. Not checked: `wakeRequest` (its own lock, deliberate), the joint arrays (a pose job owns them), the other queue pushes in `pipeline.cpp` (2376 / 2379 are the apply step, 2469 / 2508 are main thread code), the async pool's own phases (only "not the main thread" applies there). `LL_VK_POOL_SELFTEST=<seconds>` runs the stress test once at start (`RKForkJoinPool::selfTest`: random batches of 1 to 1000 jobs, every index exactly once, nothing running after the join; 10 s = 263,127 batches, 131.7 M jobs, 0 wrong) and tries the phase check on purpose (4 calls: 3 from workers, 1 on the main thread inside a batch, counted in the totals). Observed and not explained: of those 4 only one event line reached the log, so read the totals, not the event lines. Laps s93 / s94: totals 0.
5. **The split guard as one function.** Every write into a per-slot table that persists across the frame (today: the handle map; the audit of section 9 says whether there are more) goes through one function that refuses and counts when `frameSplit()`. The descriptor writes check "fresh or retired index" the same way, except the guard path. **Done in part 2026-11-13:** `Renderer::handleMapSet(table, handle, d)` (`rkvkrenderer.cpp`) is the one write into the current frame's handle map that is not the apply at the start of the frame: in a split frame it refuses a handle whose entry shows something (counted: "entries not written into the map of a split frame", the same 4000 per lap as before the change), and takes a placeholder handle's entry at once after flushing its descriptor. `handlesBeginFrame` refuses and counts ("the apply of the log was asked in a split frame", healthy = 0) if the apply is ever asked in a split frame; `Context::beginFrame` resets `mCmdIndex` before it, so it is 0 in every lap (s95, lt-sp). Not done: the audit of other per-slot tables that persist across a frame (section 9), and a "fresh or retired index" check on the descriptor writes (`flushPendingWrites` writes into every slot's set; the index comes from `HandleTable::setContent`, which gives a fresh one by construction, and the view release guard writes the placeholder into a reachable index on purpose).
6. **A lifetime stress lap in `REGRESSION-CHECKLIST.md`.** Not always on. One lap each with `LL_VK_FRAMES=6` and `=2`, `LL_VK_ARENA_POISON=1`, `LL_VK_STANDIN_MAGENTA=1`, `LL_VK_ARENA_BLOCK_MB=16`, RT on with `rtearly=3`, a snapshot and a teleport. Pass = every guard and fallback counter at 0 and Adam's eye. The frame count is the proven lever: the flash of row 1 lasted longer at 6, the shadow blink of row 3 showed at 6 and not at 2. A symptom that changes with `LL_VK_FRAMES` is family A; one that does not is something else. This also answers the first version's worry about poison: validation hides timing bugs because it is ten times slower, a fill of retired ranges is not. **Done 2026-11-13** (the recipe is in `REGRESSION-CHECKLIST.md`): first runs found that poison needs `LL_VK_RT_EARLY=1`: it fills in graphics queue command order, and with the ray scene on the compute queue nothing orders a structure build against the fill (a device lost inside an acceleration structure build, and avatar shadow flicker, with poison at the default `RT_EARLY 3`; neither with `RT_EARLY=1`; six slots alone clean). A proper fix would poison at the real (deferred) free, which needs the free delayed one round after the fill for host ranges (the CPU rewrites them at once); not done, the viewer warns instead.
7. **Depth by the reader's stamp (bigger, optional).** Instead of the constants F + 2 and F + 3, a late reader (the ray scene, the velocity pass) stamps what it reads with its own frame, and a ring slot or arena is reused when that frame is finished. A new late reader then keeps its data alive by reading it, and no constant has to be found and raised. Needs a small pool of ring buffers instead of the fixed modulo.

Order: 1 and 2 first (they are fixes), then the rest of the audit (section 9), then 3, 4, 5, 6. Step 7 only if another depth bug appears.

## 8. Checklists

**A new CPU -> GPU channel**
1. Which of P1..P6? If none, stop and ask.
2. Who writes (thread), who reads (pass, frame part)? Can the reader be a part that is already submitted?
3. Does anything read it in a later frame (ray scene, velocity, history)? Then name the reader where the depth is defined.
4. What does a late or early reader see: the placeholder, or an old object?
5. What happens when its memory is full? The answer is "the draw is dropped and counted", never "a weaker channel".
6. Add the row to section 4 and the first failure mode to section 6.

**A new kind of content in an existing channel** (row 7): list every reader of that buffer or record kind by address, and what each assumes about the layout.

**A new cache or remembered name** (G1-G4): what generation does it record, where is it checked, what counts a miss, who owns the object.

**A new result read back from the GPU**: published by which fence; stamped with what; what does the reader do with a result that is late or lost.

**A new pool job**: what does it read that someone could write during the phase; what does it write that is not its own; which records does it leave for the apply step; do those records own what they name.

**A new mode or path** (RT variant, a second render, another queue): which upkeep steps does it skip; run the stress lap of step 6 with it on.

## 9. The audit of the channels that were not read (2026-11-13)

Read from the code and checked in laps s96 / s97 (RT on). Verdicts: **ok** = a rule holds by construction or by a check in the code; **counted** = a counter was added; **open** = not read or not decided.

| Channel | What was read | Verdict |
|---|---|---|
| `destroyLater` callers (finding 5.8) | 12 call sites, all in `rkvk*.cpp` (the worker's code); the in-frame list has no lock, the between-frames list has one. `Context::destroyLater` now compares the calling thread with the thread that last began a frame | **counted**: 0 calls from another thread in the laps ("destroyLater called from a thread that is not the frame thread", healthy = 0). A shutdown from the main thread would show |
| Palette arena takeover (finding 5.7) | `paletteArenaAcquire`: an arena held by an earlier viewer frame that was never freed is taken over once the frame is 10 (`FRAMES_IN_FLIGHT + 4`) viewer frames old. Right for an aborted frame; wrong if the worker is 10 viewer frames behind with that frame's record still queued | **counted**: 3 takeovers in a lap, arenas 1 to 3 at start-up (viewer frames 1 to 3 sent no record, taken over at frames 11 to 13), none while playing. The queue's back-pressure (a ring that waits when full) bounds bytes, not viewer frames: the guarantee is not proven by code, only by the counter staying at the start-up three ("palette arena N taken over", logged for the first 5 and at every power of two) |
| Occlusion and timestamp queries (`harvestQueries`) | read when the slot is begun again (the fence of its last frame was waited for: R1); results carry the request's serial (R2); a query whose draw never happened is reported as "nothing passed" | **ok**; the channel is idle in the shipping setup (the period line says 0 queries with samples, 0 with none) |
| Uniform block sets per slot (`mUboSets`, `mUboPool`) | the pool of a slot is reset and its map cleared when the slot begins; a set is allocated and written once for a buffer combination and only used by later draws; the key holds VkBuffer handles (not reusable within a frame: destroys wait two rounds); `mSet0[slot]` is written once at start-up (the ring is bound by dynamic offset) | **ok** (P1); a split frame allocates new sets after the first part, it never rewrites one the first part uses |
| Upscalers (DLSS, DLSS-NR, FSR) retired images | each keeps `gRetired` with the frame it was retired in and frees on `Context::frameFinished` (the corrected test) | **ok** |
| Texture upload pixel hold | `LLImageBase::mPixelHolds` counts holds, every exclusive data lock of the image first waits (`waitForPixelHolds`), the hold is released by the worker once the pixels are in staging | **ok** (enforced, not a rule to follow) |
| Compute queue during a split | `PLAN-async-compute.md` E2: the compute work waits v1 (the uploads A1 of the same frame), the lighting waits v2 (its end); frame N + 1's compute follows B(N) in submission order; every buffer it touches is CONCURRENT between the two families; `buildScene` opens with an ALL_COMMANDS barrier before any build or refit | **ok** by the documented hazard analysis; not tested by a deliberate break |
| Ray scene refit in place | the refit writes the structure's own memory; ordered after the last frame's traces by that barrier (same queue) or by the timeline waits (split); a BLAS is replaced (not refitted) when the vertex or index buffer's epoch, version or move count changed, after 64 refits, when compacted, or when a geometry list changed; old structures go through `destroyLater` | **ok** for the ordering; the epoch checks are rule G1 (counters exist: the "stale check ... made again" warnings, about 120 a lap, all handled by building again) |
| Ray scene compaction slots | one query pool per frame slot, its sizes read (`vkGetQueryPoolResults`) when the slot is next used (R1 by slot); the BLAS is looked up again by key before a compacted copy replaces it | **ok** |
| Ray scene descriptor sets (F + 1) | one more set than slots, for the frame that is between two frames | **open**: only the pool size was read (`frameCount() + 1` sets and descriptors); how each set is rewritten, and when, was not read |
| Scene table, resident shadow table, record lists (HUD, impostor, texture matrices) | the records are copies in the command stream (P2): the viewer hands the worker the bytes, the worker copies a table into its frame scratch per record (`res_frame_copy`); texture matrix rows travel inside the record's blob | **ok** by construction; not re-read line by line |
| Viewer side vertex buffer retire list (`RKVBRef`) | `RKRetireList::pump`: "parked N, destroyed N, count rose again 0" in every lap, including the stress laps | **ok** (counted by the existing line) |

**What remains open after the audit:** a deliberate-break test of the compute queue ordering (not read as a risk, never tested), the unlocked `destroyLater` in-frame list is only as safe as the counter staying at 0, the takeover guarantee above, and findings 5.2 (the arithmetic sites still not unified: palette ring, arenas, resident alpha, `geo_in_flight_impl`), 5.3 (partly done by step 3), 5.5 (the palette ring's fallback to the vertex ring is counted, 0 in the laps).

## 10. The audit, second part (2026-11-15): the ray scene sets, the paths added since, and what is left

**Ray scene descriptor sets (the open row of section 9): ok.** Read in `rkvkrt.cpp`: `frameCount() + 1` sets are allocated once at start-up (`sPool`, `maxSets = F + 1`); `make_tlas` is called only there (once for each slot's TLAS, once for the null TLAS in the extra set) and writes the set once: the acceleration structure handle and the slot's table buffer (dynamic offset: the region of a scene call). Nothing rewrites a set while it is in flight; each frame's TLAS is rebuilt in place under the same handle, ordered by the barrier / timeline waits of the compute queue row. `scene_current(slot)` picks the slot's set when that slot's TLAS was built this frame, else the null set (an empty structure with a header table): the draw before the first scene call traces against a valid empty scene.

**Paths added since the audit, and what holds them:**

| Channel | What was read | Verdict |
|---|---|---|
| Deferred texture uploads (`Renderer::mDeferredUploads`, 45ed106234) | an upload made with no frame active is filled at once into a host visible staging block and recorded into the next frame's command buffer at `beginFrame` before anything draws (one barrier pair a texture, the regions of one block in one copy). Hazards: the block (written now, read by a copy in a later frame): kept in `mDeferredBlocks` until recorded, then retired (below); the image (named by a raw `Image*` and the `VkImage`): `Context::destroyImage` calls the image destroy hook first, which drops its pending uploads, and the flush checks `image->mImage == handle` again; anything else that submits on its own or reads an image back (`withCommandBuffer`, the three readbacks) flushes first | **ok** by construction; counters: the `VkUpload` lines (uploads kept, recorded), no new failure mode logged yet |
| Staging block pool (`mRetiredBlocks`, 7676fd679a) | a flushed block is stamped with the frame number being recorded and taken again only when `Context::frameFinished(stamp)`: the GPU is done with the frame that copies from it. At most 6 blocks of 40 MB or less; a block idle for 1800 frames is destroyed (`destroyBuffer`, itself deferred) | **ok** (P1, the same test the upscalers use) |
| Asynchronous readback (`RKVkAsync::postCall`, `RKReadbackTicket`, 47d63e5fe2) | the worker runs a function behind everything queued and writes into a ticket's vector that it owns through a `shared_ptr`; the main thread reads only after `mDone` (release / acquire); the pending list of the avatar bake drops a destroyed layer's entries | **ok** (D: no shared writable state without the flag) |
| Static FSR and per-kind denoiser contexts (b574fa2e03, e5d4e02899) | the FSR contexts keep their retire list stamped with frames and free on `frameFinished`; each denoiser kind has its own context and helper images, rebuilt only on a size change after `Context::waitIdle()` | **ok** |

**What is left of the lifetimes list, and the decisions:**
1. **Poison at the real free: not needed.** The poison fill (`LL_VK_ARENA_POISON`) is recorded when a range is given up and the range stays poison until new data is written into it. A late reader sees poison until the range is reused; after the reuse it sees another object's data, which a poison fill at the real free cannot show (the data is no longer poison then). Closed as "covered".
2. **A deliberate break of the compute queue ordering: built, to be run.** `LL_VK_BREAK_ORDER=1` removes the compute work's wait for the uploads of its own frame, `=2` removes the frame's last submit's wait for the compute work (`rkvkcontext.cpp`, debug only, default off). A lap with RT on (`LL_VK_RT_EARLY` 3, the default) and each setting must show something (a missing or wrong ray traced shadow or reflection, flicker, a device lost): if it shows nothing the hazard is not observable in a lap and the stress lap cannot be said to cover that ordering; if it shows, the ordering is real and the unbroken stress lap is evidence that it holds.
3. **Step 7 (depth by the reader's stamp): stays optional** (only if another depth bug appears).
4. **The stress laps** (`REGRESSION-CHECKLIST.md`) on all of the above: the unbroken laps first (`LL_VK_FRAMES` 6 and 2, poison, magenta, with the deferred uploads on), then the two broken ones.

**Stress laps on all of the above (2026-11-15, Sirens Beach, the last location of the harness; Adam watching):**

| Lap | Settings | Result |
|---|---|---|
| lt6 | `LL_VK_FRAMES=6`, poison, magenta stand-in, 16 MB arena blocks, `LL_VK_RT_EARLY=1`, RT materials | clean: no device lost, no magenta or flicker seen, "written in place" and "view release guard" not logged (0), retire list "count rose again 0", thread phase totals 0, 45 stale checks (rule G1, handled), 9 late frame episodes (worst 152 ms at start-up). The palette arena takeover counter reached 256, all within the first 2 s after arriving at Sirens (the viewer frames that sent no rigged record before the avatars loaded, taken over 10 frames later: the designed fallback; the plaza lap had 3 because it loaded faster) |
| lt6b | `LL_VK_FRAMES=6`, magenta stand-in, 16 MB blocks, the default `LL_VK_RT_EARLY` 3 (split frames), no poison | clean: no device lost, thread phase totals 0. Magenta on the beach waves while loading: the known split-frame placeholder (a texture created in a split frame has no entry in that frame's handle map, the counter read 1000 "entries not written into the map of a split frame"; the stand-in is only magenta with this switch) |
| lt2 | the same with `LL_VK_FRAMES=2` | clean: no device lost, thread phase totals 0, the placeholder counter not logged, no magenta except behind the loading screen (Adam) |
| brk1 | the lt6b settings with `LL_VK_BREAK_ORDER=1` (the compute work does not wait for its frame's uploads) | **froze**: a worker command waited 10 s and then 30 s on a fence, the watchdog said "viewer is hung" and the harness had to end the viewer. The break is observable: with the ordering intact a clean lap is evidence that it holds |
| brk2 | the same with `LL_VK_BREAK_ORDER=2` (the frame's last submit does not wait for the compute work) | **device lost** (11 lines) within about 25 s of the world loading, the viewer gone before the early picture. The second half of the ordering is observable too: both waits are real, and a clean unbroken lap is evidence that both hold |
