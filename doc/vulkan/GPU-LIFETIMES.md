# GPU lifetimes: what the CPU may hand to the GPU, and for how long

Written 2026-11-09 (first version, for Adam to read before the code changes it proposes). Scope: the Vulkan renderer (`indra/llrender/llvk*.cpp`, the viewer's producers in `newview`).

## 1. Why this document exists

Every device lost, flash, stray shadow, blink and spike we fixed since 2026-10-24 was the same mistake in a different place: **the CPU wrote something a submitted frame (or a part of one) still reads, or freed or reused something such a frame still reads.** The guards we added at destroy time never fired in the worst case, because nothing referenced the object at that moment: the stale read came through a table entry that was changed after the part was submitted (the 2026-11-09 handle map, `0762a0fe0a`).

The rule is simple. This document writes it down per channel, so that a new write can be checked against a table instead of against someone's memory of the last bug.

## 2. The model of time

- **Frame slots.** `Context::frameCount()` slots are in flight (3 by default, `LL_VK_FRAMES` 2..6; `FRAMES_IN_FLIGHT = 6` is only the array bound). A slot is reused only after the fence of its previous use is signalled (`Context::frameFinished(frame)`).
- **Two slots per viewer frame.** A viewer frame uses the frame slot and the off-screen slot after its present, so the thread that records can be about 1.5 viewer frames ahead of the GPU with 3 slots.
- **Split frames.** With async ray tracing the first part of a frame is submitted early (`Context::splitFrame`) and the rest is recorded afterwards. From that moment the early part is on the GPU while the CPU still builds the same frame. `Context::frameSplit()` is true then. **Anything the early part reads is "in flight" while the frame is still being built.**
- **Threads.** The main thread (viewer), pool jobs (palettes, shadow builders, cull), the render worker (records commands, owns the renderer). A channel names who writes it.
- **"In flight"** for a buffer range or a table: from the moment a command that reads it is recorded until the fence of that frame is signalled. For a buffer the worker approximates it by `mLastUseFrame + frameCount() + 1` (see `write_buffer` in `llvkworker.cpp`: a write to a buffer that is in flight by that test goes to a new range).

## 3. The four allowed ways to hand data to the GPU

Everything in section 4 is one of these. A write that is none of them is a bug until shown otherwise.

- **P1. Own slot, write once.** The producer writes into scratch that belongs to this frame slot (rings: uniforms, vertex/scratch, palette ring, staging), and only this frame's commands read it. Nothing else reads it, nothing rewrites it. It is reused when the slot's fence has passed.
- **P2. Master and per-frame copy.** The CPU keeps a master table that the GPU never reads. Each frame copies the master into its own scratch (P1) at the moment it builds its commands. Used for the scene table rows (`sGeo.mTCpu` -> `mSTable` / `mRTable` / `alpha_rows`), the resident shadow table, the handle map (log replayed per slot).
- **P3. Rename on write.** A buffer that a frame in flight may read is never written in place: the write goes to a new range, the old range is given back by frame (`destroyLater` / retirement), the buffer's address changes for the frames after.
- **P4. Immutable until retired.** An index (descriptor, handle, row number, id) is never reused while any frame in flight may still hold it. It goes to a retired list stamped with the frame, and returns to the free list when `frameFinished(frame)`. Reads of a retired entry see the placeholder.

**Never:** change anything a submitted frame part reads (including a map entry "for early visibility" after `splitFrame`); reuse an index before retirement; hold a GPU address in a record that outlives the ring depth of its channel; read write-combined upload memory on the CPU (uncached, 20 fps lost once); free or reuse on the strength of "nothing references it now" (the reference can arrive through a table).

## 4. The channels

Ring depths in frames: F = `frameCount()` (3 default).

| Channel | Carries | Memory and writer | Rule (pattern) | Retired / reused when |
|---|---|---|---|---|
| Uniform ring `mRing[slot]` (`allocUniform`) | loose uniform blocks of each draw | host, render worker | P1; the ring never wraps: when it is full the draw is dropped (counted in `mRingExhausted`), because wrapping would overwrite data the GPU has yet to read | the slot's fence (the cursor starts again at frame begin) |
| Vertex / scratch ring `mVertexRing[slot]` (+ `mVertexOverflow`) (`allocVertexScratch`) | the per-frame scene table row copies, command lists, model blocks, texture matrices, immediate-mode vertices | host, render worker | P1, and P2 for the table copies | the slot's fence; overflow chunks are reused when the slot comes round, released after a long idle |
| Palette ring `mPaletteRing` (`allocPaletteScratch`) | joint palettes of rigged draws | host, render worker | P1, **deeper**: F + 2 slots, because the ray scene may skin with a palette noted up to two frames earlier (2026-11-04 spikes) | reused only by the frame F + 2 later |
| Palette arenas `mPaletteArena` (`ll_frame_palette*`, kinds 39/40) | the same palettes, written straight by the viewer's palette jobs; each palette carries a tail entry (the avatar rebase anchor) | host, pool jobs (main thread prepares the list) | P1 per viewer frame; named by id, ids are P4 (generation) | freed F + 3 renderer frames after the last record that named them |
| Handle map `mHandleMap[slot]` + `HandleTable` | (table, handle) -> descriptor index, for every bindless texture read | host copy per slot, render worker | P2 (log replayed per slot at frame begin) + P4 | log entries by frame; **no patch into the current slot's map once `frameSplit()`** (`0762a0fe0a`) |
| Bindless descriptors / `mPendingRetire` | the descriptor behind an index | device, render worker | P4: an index back on the free list only after `frameFinished` | `HandleTable::retire` per frame |
| Image views (`mViewUsers`, `releaseViewHandles`) | which handles and descriptors show a view | CPU bookkeeping | P4: a view is destroyed after its handles were pointed at the placeholder and its descriptors retired | `destroyLater` (after every submitted frame) |
| Arena blocks (`Context::arenaAlloc`, kinds Device, Shared, Host, Accel) | mesh vertex and index buffers, acceleration structures, host-written tables | VMA virtual allocations in 64 MB blocks | P3 for host-written buffers (`write_buffer`: in flight -> new range); ranges returned after the frames in flight | frame based; empty blocks trimmed after a while (`arenaTrimPool`); a new host block is filled once before use (`LL_VK_HOST_BLOCK_GATE`) |
| Resident tables (shadow resident table, alpha resident rows) | per-draw rows that stay between frames | CPU master, copied per frame | P2 (`b7cf8adc6b`: the master used to be rewritten under frames in flight) | the frame's scratch (P1) |
| Scene table rows (`sGeo.mTCpu`) | the rows of every draw the frame names | CPU vector, never read by the GPU | P2: `mSTable`, `mRTable`, `alpha_rows` are the frame's copies (`LL_VK_GEO_ROWCOPY` and `RENAME_ONCE` are gone since `c0ac19bb60`) | the slot's fence |
| Staging (`allocStaging`) | texture and buffer uploads | host, per slot | P1: "lives until this frame's GPU work completes" | the slot's fence; a copy may name only a staging block of some slot |
| Deferred destruction (`Context::destroyLater`) | everything that is destroyed: images, buffers, pipelines, shader modules | CPU | runs after the GPU finished every frame submitted so far; work given up while no frame is being recorded waits for the next one (`mDeferredNext`) | by definition |
| Ray scene (`llvkrt.cpp`): BLAS/TLAS, skinned vertex output, the skin kernel's jobs | acceleration structures and the skinned geometry they were built from | Accel arena, render worker | P3 for structures; skin jobs read the palette ring (F + 2) | the frame that last built or traced them; snapshots build once per snapshot |
| Readbacks (`mFbRead[slot]`, query pools, mip feedback) | GPU results the CPU reads | host cached, per slot | the CPU may read slot s only after its fence | the slot's next use |

### Channels found by name whose rules I have not read yet

These need an audit line each before the table can be called complete (the next step): the GPU light table (`ll_light_table`), the GPU particle rows and sort (`kind 36`), the Hi-Z boxes (`kind 34`) and cull rows (`kind 35`), the per-record texture matrix lists, the HUD and impostor record lists, `mFbRead` consumers other than the mip feedback.

## 5. The incidents, as rule violations

| Date | Symptom | Channel | What was written or freed under whom | Fix |
|---|---|---|---|---|
| 2026-10-24 | textures flash a wrong colour at first load and at every resolution change | scene table rows vs per-frame descriptor sets | rows (shared by all frames) rewritten to a new slot, older frames' sets lack it (P4/P2 broken) | texture handle rework H0-H6 |
| 2026-11-02 | stray or missing one-frame sun shadows | resident shadow table | the master rewritten in place while frames in flight read it | `b7cf8adc6b`: master + per-frame copy (P2) |
| 2026-11-04 | skinned vertices flew to wherever the new bytes said (spikes from avatars named only by a far cascade) | vertex ring used for palettes | the ray scene read a palette up to two frames old; the slot had been reused | palette ring of F + 2 slots (P1, deeper) |
| 2026-10-25 .. 31 | sudden device lost, fault in the first MB of a host-written arena block made 1-3 s before | arena host blocks | not found; the block is filled by the CPU right after creation | `LL_VK_HOST_BLOCK_GATE` (first fill by the GPU), still watched |
| 2026-11-09 | device lost in a crowded zone with RT on | handle map | `handleSet` patched the current slot's map after `splitFrame`; the early part read an entry whose descriptor index was a recycled one still holding a destroyed view | `0762a0fe0a`: no patch when `frameSplit()` |

## 6. Checklist for a new CPU -> GPU channel

1. Which of P1..P4 is it? If none, stop and ask.
2. Who writes it (thread), who reads it (which pass, which frame part)? Can the reader be a part of a frame that is already submitted (`frameSplit()`)?
3. What stamps the frame it belongs to, and what returns it (a fence, `frameFinished`, `destroyLater`)?
4. What is the depth: does anything read it later than the frame that wrote it (the ray scene, the previous palette, history buffers)? If so the ring is deeper by that many frames.
5. What does the reader see if it arrives late or early (a placeholder, not an old object)?
6. Add the channel to section 4 and its first failure mode to section 5.

## 7. What will enforce this in code (proposed, not done)

1. **A stamping accessor.** Every place that takes a GPU address or a mapped pointer for a channel goes through one accessor that records the frame it was taken for and the channel; a read of an address stamped for another frame (past the channel's depth) is a log line and a counter, in every build.
2. **`frameSplit()` assertions** at the writers of P2 copies and tables: a write into a slot's table or map after the split asserts (debug) and counts (release), instead of relying on a comment at one call site.
3. Poison mode (fill retired memory with a pattern) is not proposed: it hides timing bugs under its own cost, and the 2026-11-02 notes say timing bugs vanish under validation.

Order agreed with Adam: this document first; the accessor and the assertions after he has read it.
