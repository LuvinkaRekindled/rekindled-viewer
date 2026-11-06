# PLAN-avatars: avatars as rows, from load to skinning (2026-11-06)

Status: planned, not started. Decision: Adam, 2026-11-06 ("we might need to stop looking at avatar updates in pieces and think about how to redesign avatars completely, from load to skinning to animation"; hidden avatars may skip the pose and catch up: accepted). Work order: the clean-core plan (PLAN-clean-core.md, C5) goes on first; this plan is picked up after it.

## Why a plan and not another fix

Avatar cost has been attacked piece by piece for several sessions (the b0 skip of rigid attachments, the matrices-only update of hidden avatars, the parallel motion batch, the attachment loop as pool jobs, the far-update distance, skin cache, shadow caster LOD, rigged alpha LOD, the box override skip, the far avatar tier). Each found a few tenths of a millisecond and the cost moved to the next consumer. 2026-11-06 added three more negative or small results (below). The cost is spread over the model, not in a place: every consumer pulls its own copy of avatar state out of an object graph.

## What it costs today (RT off, a crowd of about 80 avatars, 28-31 k objects; zone laps of 2026-11-06, pool labels, attachment census)

Main thread, per frame:
- the avatar motion batch 0.80-1.2 ms: the blend wall 0.30-0.40 (29 jobs, 1.7-2.4 ms of CPU on all threads), the attachment jobs 0.06, the serial completion loop 0.49-0.54 (about 43 avatars, 11-12 us each: replay of the attachment ledger, post-motion matrices, voice, appearance, lip sync / wind, name tag, complexity every 20th frame)
- idleUpdate of the avatars 0.28, the attachment loop 0.14, the move chain of the attachments and avatars (drawable, bridge, partition moves) 0.2: about 230 bounds events a frame (bridge 113, avatar 75, animesh 23, particle 21), nearly every group of those partitions every frame
- the mark job's walked-partition part (37-160 drawables a cascade) and the walked partitions' cull in the main view and four cascades (about 0.4 ms of pool CPU)
- the worker: palettes 355 and joint matrices 10.6 k a frame (about 160 k palette floats, 0.8 MB in the ray scene naming), rigged packets (kind 21 prepare 1.1 ms of worker CPU)
- GPU: rigged triangles 10.7 M in the main view and 9.9 M in the cascades (cascade 1 more than half)

Measured and ruled out as single fixes: the skin cache across frames (4% of palettes and 12% of joint matrices unchanged), the retained set for the walked partitions (they are evented every frame), fusing the blend and attachment fork-joins (0.36 ms unfused against 0.41 ms fused: no gain, the attachment barrier is 0.06 ms), avatar update CPU as the limit in a GPU-limited crowd, back-face culling of rigged triangles (4-37% dropped).

Cuts that are quality-neutral and in: 22.9 of 36 looped avatars are skipped without the loop (98.7 attachments), static rigid attachments are not moved, the box override is skipped when unchanged, matrices-only updates for hidden avatars, alpha and opaque rigged LOD at half a pixel (looked at by Adam, default on), shadow caster LOD (2026-11-04).

## The model

One owner per fact, rows instead of an object graph, work proportional to what is seen.

1. **Avatar table.** One row per avatar: world transform, importance class (own / seen in the main view / seen only by a cascade or probe / seen by nothing), the per-view visibility stamps (from the same cull that serves the scene), distance, animation clock, flags. Name tags, voice, lip sync, wind, loading effect, complexity, impostor / jelly-doll decisions, attachment loop and the rigged naming read the row instead of asking the object. The importance class replaces the scattered isVisible() / mDrawable->isVisible() / mUpdatePeriod / far-update tests.
2. **Skeleton as flat arrays.** Per avatar: local TRS, parent index, world matrix, inverse-bind products. One batched vector pass over all avatars of the batch computes world matrices and writes the skinning palettes straight into the GPU palette buffer. No LLJoint tree walk, no palette copy into packets for each pass.
3. **Animation as data.** The motion controller (states, weights, priorities, transitions, the "walk" and "head turn" logic) stays on the main thread and only produces per-motion weights and times. Sampling the keyframes and blending is one batched job over all avatars. An avatar of class "seen by nothing" is not posed at all: it catches up from the animation clock when it is seen (keyframe motions are time functions, so the catch-up is exact; history-dependent motions (head turn, walk smoothing) settle over a few frames: accepted by Adam). Anything seen is exact: no visual cut.
4. **Attachments as joint-parented rows.** An attachment's world matrix is its joint's world matrix times its local matrix, produced by the same batched pass and written to the instance row (the instance table the ray scene and the scene table already use). No per-frame octree move, no bridge, no moved list. Culling of the movers is the GPU's, against a conservative avatar bound. Rigged attachments are skinned with the avatar's palette (already how they are drawn).
5. **Skin once per frame.** Skin each avatar mesh once into a vertex buffer that the main view, the four cascades and the ray scene read (today each pass skins in its vertex shader, the ray scene has its own skin buffer). This is the one stage whose gain is not established: the earlier "skin cache ruled out" result was about reuse across frames, not sharing within a frame (vertex runs were measured small, about 13 M a frame). Stage D starts with a measurement.
6. **Presentation as importance-gated jobs.** Name tags, voice visualizer, lip sync, wind, loading effect, complexity: per-avatar jobs run for the classes that can show them. The serial completion loop disappears as a loop of object calls.
7. **Load.** Appearance / bake / mesh fetch and decode / rigged mesh parse (bind matrices scrubbed at parse, the writer-bug fix) are priority-ordered by the importance class, and a mesh's skin data is stored in the form stage B reads (joint indices, inverse bind matrices, weights), so no per-frame conversion is left. Stage 0 inventories the load path (it is the least known part).

## Stages (each measurable on its own; census first, as always)

- **0. Census and inventory.** Per-frame cost by class (own / seen / cascade-only / hidden) of every avatar consumer, the load path timeline of an avatar from first packet to first draw, who reads which avatar field (the consumer list the table has to serve), the importance class as a logged number next to the existing tests (a mismatch counter between the classes and today's tests).
- **A. Avatar table and importance class.** The row, filled once a frame, read by the existing consumers; the old tests removed one by one behind check frames (the row against the object). Gain here is mostly structural (the completion loop starts to shrink: classes that cannot show a name tag or lip sync skip them).
- **B. Skeleton arrays and the batched pose job.** Includes the palette written to the GPU buffer, the motion controller reduced to weights and times, hidden avatars not posed. Expected: the blend wall and most of the completion loop (together about 0.9 ms of the main thread, 1.7-2.4 ms of pool CPU), the palette copies in the packets.
- **C. Attachments on the instance rows.** Removes the move chain (0.2 ms main, 230 bounds events, the walked-partition cull and mark work), the attachment loop (0.14), the moved list and the bridges. The walked partitions then hold only what is not an avatar or attachment (particles, HUD).
- **D. Skin once.** After a measurement of the shared-skin gain in the cascades and the ray scene.

## Open questions

- The own avatar and animesh (control avatars) are special in the motion controller and the loading path: stage 0 lists what they need from the table.
- Flexible-prim attachments (461.9 children a frame in the crowd) read parent positions every frame: they move with their joint, but their own physics needs a per-frame step: the row form has to keep them (stage C).
- Sitting avatars (they follow their seat), avatars on moving vehicles, and the "update root after the pose" ordering of updateCharacterPostMotion.
- RLV / attachment locks, hover offset (a bug of 2026-11-04 was a read in the middle of a deferred update), LSL animation overrides: all read the pose or the skeleton and must read it through the table's accessors.
- Where the importance class comes from for a view that has no GPU cull yet (cascades, probes): the CPU cull's stamps until the GPU cull serves them.

## Relation to the other plans

PLAN-clean-core.md (C4.2 for the walked partitions turned out the wrong form: they are evented every frame; stage C of this plan is their right form). The ray scene's instance table (PLAN-retained-records / S-series) is the table stage C writes to. "Plasmastorm" (a from-scratch viewer) would start from this model; nothing here depends on it.
