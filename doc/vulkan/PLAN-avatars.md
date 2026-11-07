# PLAN-avatars: avatars as rows, from load to skinning (2026-11-06)

Status: planned, picked up 2026-11-06 after C5 (J2, PBR shadow rows and INVISIBLE done; BUMP / SHINY shaders, the RT material cache and J1 follow this plan). Decision: Adam, 2026-11-06 ("we might need to stop looking at avatar updates in pieces and think about how to redesign avatars completely, from load to skinning to animation"; hidden avatars may skip the pose and catch up: accepted). Work order: the clean-core plan (PLAN-clean-core.md, C5) goes on first; this plan is picked up after it.

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

Zone lap pz1 (2026-11-06, the completion loop by piece, 40.8 avatars a frame, main thread): motion batch 0.727 ms = blend 0.250 + attachment jobs 0.050 + completions 0.425; of the completions idleUpdateMisc (attachment ledger replay, world matrices) 0.190, appearance animation 0.117 (2.9 us an avatar for a function that is one flag test unless the avatar is morphing: a cache miss on the avatar object, or avatars still animating their appearance), name tag 0.092, voice visualizer 0.010, lip sync / loading / water / wind 0.007 (14.8 avatars), updateCharacterPostMotion 0.020 (27.5 avatars). Every piece is small and per avatar: the shape the avatar table is for.

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

## Leftovers from the clean-core plan that this plan owns (2026-11-06, Adam: "combining the leftovers from here that are avatar owned")

Everything below is a rigged draw that still reaches the GPU through a per-draw path or a LLDrawInfo, so LLDrawInfo cannot be deleted (J1) until they are rows. They are stage B / C work (the skin and palette plumbing), not separate fixes:
- **Main view render map, rigged, per frame at the beach spot (census inv1-rmcensus1):** NORMSPEC_MASK_RIGGED 14, GLTF_GLOW_RIGGED 12, GLTF_PBR_ALPHA_MASK_RIGGED 12, FULLBRIGHT_ALPHA_MASK_RIGGED 4 (+ rigged SHINY / BUMP / INVISIBLE variants, which come with their table kinds). The rigged table types are served only when the skin pull programs and `ll_rigged_packets_enabled()` allow; these are the types where a rigged variant of the program does not exist or the draw is refused. Fix: the missing skin pull programs, then the draws are rows with a palette binding like the other rigged items.
- **Rigged glTF casters in the cascades:** `renderGLTFObjects(rigged)` / `pushRiggedGLTFBatches` still draw per draw (the static ones moved in the PBR shadow rows step); the skin shadow pull program has to take the glTF opaque / mask (base colour transform, cutoff) like the alpha records do now.
- **Rigged alpha casters (`shadow_alpha_packet_ok` rigged), skin LOD (`ll_rigged_lod_lookup`, `shadow_lod_*`, `rigged_lod_for`):** read the draw info's index and vertex data per frame; the avatar table holds the mesh's LOD data and the pixel-size decision instead.
- **Palettes:** `ll_frame_palette` copies a palette per (avatar, skin) per frame (355 palettes, 10.6 k joint matrices a frame); stage B writes them straight into the GPU palette buffer from the skeleton arrays.
- **The ray scene's rigged material / skin buffer** (`note_rt_skinned_alpha`, `rt_material_id` cached on the draw info): RT is not the baseline; it moves with the RT material cache step, after this plan.

## Stage 0 first steps (what to build before any of the model)

1. The consumer list: who reads which avatar field (isVisible / mDrawable->isVisible / mUpdatePeriod / mCulled / isImpostor / isTooSlow / isVisuallyMuted / far_avatar_tier / mNeedsImpostorUpdate / isFullyLoaded, counted by file: llvoavatar.cpp, pipeline.cpp, lldrawpoolavatar.cpp, llviewerpartsim.cpp, llworld.cpp, llflexibleobject.cpp, llcontrolavatar.cpp, lldrawable.cpp), into this document.
2. The importance class as a number computed next to the existing tests every frame (own / seen in the main view / cascade or probe only / nothing), with a mismatch counter against today's tests, log line every 600 frames. No behaviour change.
3. Cost by class of the completion loop pieces (zones exist: pz1) once the class is logged.
4. The load path timeline (first packet to first draw) by avatar, from the existing appearance and mesh log lines.

## Stage 0 result 1: the importance class (2026-11-07, lap avi1-avimp1, plaza, RT off, `LL_VK_AVIMP=1`, instrument in `updateCharacter`)

Per frame over 600 frames (the lap includes the turn): own 1.00 (full update). **Resident avatars seen in the main view 25.6**: 7.3 skipped by the period test (impostors: the test counts 6.3 impostors with period > 1), 18.4 full update. **Seen by nothing 13.9: all on the hidden update** (motions only, matrices in the batch: the cheap path that is already in). **Cascade only 0.02: the class is empty** (an avatar inside a sun cascade but outside the main view is practically never there in this scene, the turn phase included). Animesh / control avatars 13.0, all in the main view: 6.3 full update, 6.7 skipped (7 impostors). Today's tests inside the main-view class: impostor 6.3, sitting 2.5, over 40 m 3.0; inside the "nothing" class: impostor 2.3, culled 0.25, sitting 0.75, over 40 m 1.75; nothing was ever too slow or visually muted.
What it says: (1) the model's classes are three, not four: own, seen, not seen; cascade-only needs no separate path here. (2) 35% of the avatars (13.9 of 40) are already on the cheap path, so the remaining cost sits in the 18.4 + 6.3 full updates; hiding more is not where the gain is, doing the full update cheaper is (stage B). (3) The 6.3 animesh full updates are a quarter of the full updates: animesh is its own big item for the table (control avatars have no resident-avatar presentation: no name tag, voice, lip sync). (4) The existing tests agree with the class (the impostor, culled and period tests never contradict "seen by nothing" or "seen"): no mismatch to chase. Next: the cost of each piece by class (the zones exist: pz1), then the consumer list.
