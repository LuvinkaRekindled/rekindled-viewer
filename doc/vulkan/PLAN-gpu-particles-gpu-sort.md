# Plan: particles evaluated, culled and sorted on the GPU (design, 2026-11-02) — not started

Follows `PLAN-gpu-particles.md` (stages 1, 1b, 2, 3 built and looked at by Adam). Adam, 2026-11-02: having all particles on the GPU is "more efficient and simpler to deal with"; he asked for the design before any code.

## 1. Where the main thread is now, and why this step has to be the whole step

After step 3 the particles are records (free flight, follow source, target linear, target position, bounce; wind, callbacks and ribbons are still stepped, 14 a frame at the beach). The main thread per frame at the beach (3,700 records, 55 groups, 120 sources, lap `beach4-`): sources loop 198 kcycles (0.08 ms: spawning, stays), groups loop 141 (expiry sweep and per-group bookkeeping), `LLVOPartGroup::updateRows` 10 kcycles a call and `rebuildRows` 8 kcycles a group: about 0.2-0.3 ms in all, linear in the number of particles apart from the sources.

Evaluating colour, scale and glow in the vertex shader alone saves about 0.08 ms and leaves the movement laws in two places (C++ for the sort and cull, GLSL for the draw): not simpler. The cost that goes away only when **the GPU also sorts and culls**: then the CPU does nothing per particle per frame, and there is one copy of the laws. So the design is one piece: evaluate + cull + sort + draw on the GPU.

## 2. What the census says about the shape of a group (lap `beach5-`, 1,300-2,000 group rebuilds a window)

| per group | beach |
|---|---|
| draw states (blend pair + fullbright) | 1.06-1.10 |
| groups with more than one state | 6-11% |
| distinct textures | 1.06-1.16 |
| draw infos (runs of one texture and state in the back to front order) | 1.06-1.6 |
| particles | 34-96 |

So a group is almost always **one draw state and one texture**: one draw a group, no interleaving, and the order inside it is the only order that matters. The mixed groups (6-11%) are the exception to design for.

## 3. The design

**Record table (GPU).** One device-local buffer of fixed-size records (about 96 bytes), slots handed out by the CPU from a free list, written once at spawn (a small upload of the new records each frame, 8-12 a frame at the beach): position, spawn time, velocity, acceleration, max age, mode (free / follow / target linear / target position) and bounce bit, start and end colour (unorm16 x4), start and end scale (half x2), start and end glow, floor height, target at spawn, the **source number** (index into the source table), the **texture index** (0-3 into the group's texture list), flags (follow velocity, emissive). A slot is freed by the CPU when its dead-at time has passed (the sweep that exists: `expireRecords`); the GPU also culls a record whose age is past its max age, so a freed-late slot never draws.

**Source table (GPU), per frame.** Position and target of each source (about 120 at the beach, 32 bytes each): a small upload every frame. Follow source, target linear and target position read it.

**Compute pass, once a frame for all particle groups, before the alpha pass.** One workgroup per group chunk:
1. evaluate each record at the frame's clock (the laws of `LLViewerPartRecord::evaluate`, in GLSL: free flight, follow, target linear, target position, the bounce flights) -> position, distance along the camera's at axis, cull flag (dead, the 5 mm size cut, and the area guard that replaces the order dependent budget cut);
2. sort the group's live records by distance, back to front (bitonic sort in shared memory for groups up to 1024 records, which is every group in the census; a group above that is split when it is made, see section 6);
3. write the sorted record numbers to the group's order buffer and the visible count into an indirect draw command.

**Draw.** One transient alpha entry (kind 32) a group and state: `GEO_PARTICLE_GPU`, 6 vertices a visible record from an indirect command, the vertex shader reads `order[gl_VertexIndex / 6]`, then the record, evaluates it again (the compute pass could store the evaluated row instead: one write and one read of 48 bytes a particle against the evaluation done twice; decide by measurement) and expands the quad as the stage 1 shader does. The texture index picks the entry's texture slot as `position.w` does today. The group's place in the alpha order is unchanged (the loop of `renderAlpha` sorts the groups by distance as now).

**The CPU keeps:** the sources loop and the spawning, the slot allocator and the expiry sweep, the source table, the **group extents** (the group must still be culled by the frustum: extents from the path box of each record at spawn, an exact bound for the parabolas; a follow source record adds the source's position; kept as a union that is rebuilt from the live records every few seconds), and a C++ copy of the laws (`LLViewerPartRecord::evaluate`) used as the **oracle** for the compare mode and for picking a particle on a right click (evaluated on demand for the records of the group under the cursor, so the laws exist twice, with a readback test that keeps them equal, section 5).

## 4. What does not move

Wind particles, callback sources (spiral, beam, chat) and ribbons stay stepped and drawn as stage 1 rows (14 a frame at the beach, a few hundred in the plaza where ribbons are 17%): the stage 1 path stays for them, so this is **not** "one path": it is the records path and the stepped rows path. Ribbons as records (a parent slot number in the record, the shader reads both) would remove the plaza's share and leave only wind and callbacks.

## 5. Gates

1. **Parity:** a debug mode reads back the evaluated positions of one frame and compares them with `evaluate` for every record (the largest difference per law logged, as the compare mode does now): must stay at the numbers of step 3's compare lap (free 4.5 mm, follow 2 mm, bounce 2 mm rms).
2. **Order:** the same readback checks the sorted order is non-increasing in distance; plus Adam's look at overlapping particles (smoke over fire).
3. **Visual:** side by side with the stage 1 rows path (a switch, `LL_VK_GPU_PARTICLE_SORT=0`), at the beach and the plaza.
4. **Counts:** `sParticleCount` / `sRecordCount` unchanged; slots freed = slots allocated over a lap.
5. **Cost:** main thread (sources, groups loop, updateRows, rebuild) and GPU time of the compute pass and the draws.

## 6. Decisions for Adam

1. **Mixed groups (6-11% of rebuilds have more than one draw state).** (a) Split by state: one draw each, so inside a group the particles of one state are drawn together (fire all before smoke or the reverse, not interleaved by distance); simple, same code for all groups. (b) Keep mixed groups on the stage 1 rows path: exact order, but a second path stays. I would take (a) and look at it with a group that mixes additive and alpha.
2. **A group above 1024 records:** split when it is made (a new group for the same box), or a multi-pass sort. The census shows groups of 34-96 particles on average, with ribbon groups of about 700 in the plaza; I would split above 1024.
3. **More than 4 textures in a group:** a second entry for textures 5-8 with its own sorted list (the order between the two is lost, like mixed states). The census has 1.06-1.16 textures a group: rare.
4. **Picking** with the laws in C++ as well as GLSL (kept equal by the readback test), against dropping the pick of particles. I would keep the pick.

## 7. Steps

- **G0. Record table and shader evaluation with the CPU order**: the table, the source table, the evaluation in the vertex shader (the worker side: buffers, the upload of new records, the entry kind), the CPU still sorts from `evaluate`. Gate: parity readback, Adam's look. This is the first half of the pipeline, not a stopping point: its gain is small (0.08 ms) and it makes the laws exist twice.
- **G1. The compute pass: evaluate, cull, sort, indirect draw.** The CPU stops evaluating records; the extents come from the path boxes. Gate: all of section 5.
- **G2. Mixed states and the overflow cases** as decided in section 6.
- **G3. Ribbons as records** (optional), then remove what the stepped rows path no longer needs.

## 8. Expected result and risk

CPU: `updateRows` and `rebuildRows` for records (0.2-0.3 ms at the beach, 0.1 ms in the plaza, linear in particles) go to the spawn writes and the source table (about 0.02 ms, independent of the particle count). GPU: the compute pass for a few thousand records is small (an estimate of 0.05 ms, to measure) and the vertex data the draws read shrinks. Risks: the worker side is new code on a path that has had crashes (a new buffer kind, a compute pass in the frame, indirect draws); ordering changes for mixed groups; a parity bug between the C++ and GLSL laws (the readback gate is there for it). The work is several sessions, not one: G0 alone is a day.

## 9. G0 built (2026-11-02), switch `LL_VK_GPU_PARTICLE_EVAL=1` (default off), launcher `run-viewer-parteval.bat`, lap mode `parteval`

What is in: each `LLViewerPartGroup` keeps a **record store** (`mStore`, an `LLVertexBuffer` with `RECORD_DATA_MASK`: a record a slot, slot = the record's place in `mRecords`; `fs_store_write` packs it: positions = pos0 + t0 high, normals = vel0 + t0 low, tangents = accel + max age, weight4 = target at spawn + floor height, texcoord0 / 1 = start / end scale, texcoord2 = two raw words (start glow | end glow << 8 | mode << 16 | bounce << 18 | interp colour << 19 | interp scale << 20 | follow velocity << 21 | source number << 24, end colour), colour = start colour); a spawn writes its slot, an expiry's swap writes the moved record's slot (`mDirtySlots`; a whole store on growth). When the spatial group holds exactly one part group, all of whose particles are records (`LLVOPartGroup::mPure`), `rebuildRows` still sorts on the CPU but writes only the **order of the slots** to the store's index data (2 bytes a particle) and makes the draw infos over the store (`mParticleRecords`, `mPartSources`). The transient alpha entry carries `GEO_PARTICLE_RECORDS`, the vertex buffer names, and `mPbrRow` = the entry's first source in the batch's **source table** (a new section of the kind 32 record: a (now high, now low) row, then position + target of each source of the frame, `mParticleFloats`, scratch memory, `ll_packet_sources`). `alphaPullV.glsl` and `emissivePullV.glsl` look the slot up in the order, evaluate the record at the clock (`ll_record_corner`: free flight, follow source, target linear, the solved target position pull, the bounce flights; colour, scale, glow of the age) and make the quad (`ll_billboard`, shared with the rows).

Beach lap `g0a-parteval1` / `g0b-parteval1`: no shader or draw errors, no draw dropped, 121 fps, 836-1510 of 3,000 rebuilds drawn from the store (the others: 308-427 spatial groups with more than one part group, 200-1,013 with stepped particles or no store). The share by rebuilds is low because the groups that are rebuilt every frame include the few that hold stepped particles (13 a frame at the beach). Not looked at by Adam yet; the CPU evaluates the records for the sort as before (G1 moves that).

Limits to lift later: a spatial group with more than one part group (sort the part groups by their depth), stepped particles in a group (they could be a second draw from the rows buffer), ribbons.
