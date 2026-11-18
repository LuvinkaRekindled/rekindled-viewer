# Attachments that follow their joints without the per-object update chain: design (2026-10-25)

Status: DESIGN for Adam's review. No code. Read with `PLAN-texture-handles.md` section 17.13 (the measurements this rests on), `S4-design.md` (what the scene table already does with moving matrices: option C, on by default since 2026-10-21), `PLAN-native-renderer.md` N6 ("attachments follow joints by index in the shader"), `POST-F-REVIEW.md` (the avatar tail).

Naming: Adam asked for "N6 / S4 D". In `S4-design.md` option D is the *shared skinned output* (measured: skinning is at most 0.26 ms of GPU in the G-buffer, not built). What is meant here is the N6 line, attachments following joints. This note argues that the GPU is the last stage of that, not the first.

## 1. What the measurements say (plaza, RK_AVATAR_TIMERS, section 17.13)

Per frame in the still: 23-27 avatars run the attachment loop of `LLVOAvatar::idleUpdateMisc`, about 200 attachments (91 rigged, 108 rigid, all with a bridge).

| piece | ms | note |
|---|---|---|
| attachment loop (`att: attachment loop`) | 0.43-0.62 | the serial completion loop of the avatar batch is this loop |
| of which rigid attachments | 0.245 | 2.3 us each; drawable move 0.10 + bridge move 0.10 |
| of which rigged attachments | 0.10 | 1.1 us each, two thirds already skipped by the 8-frame stagger |
| of which stable check of the avatar box | 0.09 | |
| downstream of the marked moves: `updateMovedList` 0.29-0.37, `drawable: updateMove` 0.21 (360 calls), `bridge: updateMove` 0.19 (206), `octree: partition move` 0.21 (293), `idle: updateMove` 0.17 | 0.6-0.9 together | the share that belongs to attachments is **not measured yet** (stage 0) |

Do the joints move? For the rigid attachments with a bridge (about 140 a frame): 20% had a world matrix unchanged within 0.1 mm since the loop last saw them, 80% moved more than 0.1 mm, 45% more than 1 mm (idle animations keep joints swaying). So "skip what did not move" removes about 20% of the work; the work is real motion, and the cost is the *machinery* that carries the motion (below), not the arithmetic.

## 2. What the machinery does today, per rigid attachment per frame

1. `idleUpdateMisc` (main thread, serial, after the pose blend): `transformExtents` of the avatar box into the attachment's frame, then `updateMoveDampedAsync(drawable)` (marks the drawable moved), then `updateMoveNormalAsync(bridge)` (marks the bridge moved), `updateText`.
2. `LLPipeline::updateMove` / `updateMovedList` later in the frame: each marked drawable recomputes its xform from the joint chain (`LLDrawable::updateMove` -> `LLVOVolume::updateRelativeXform`), updates its extents and position group, `movePartition` re-seats it in the octree when its box left its node (`octree: partition move`), the bridge recomputes the matrix of its children's space and its own extents (`bridge: updateMove`, `updateSpatialExtents`).
3. Consumers then read the *result*: `LLVOAvatar::calculateSpatialExtents` stretches the avatar box by the extents of static (non-rigged) attachments; the cull reads the bridge's group box; `stateSort` calls `updateDistance`; the scene table copies `LLMatrix4` values of the moving slots with a `memcpy` every frame (`pipeline.cpp` near 9070: `sMovingMatrix[k]->mMatrix`, about 150-300 matrices of 64 bytes); the ray scene takes the group matrix of the bridge; texture priority (`RKTexRecs`, volatile faces keep the face loop) reads the positions.

The 91% rigged triangles already have no model matrix (`registerFace`: "rigged meshes ignore their model matrix") and are culled by the avatar's box (`override_bbox`); only the rigid prims depend on this chain. A rigid attachment's world matrix is a pure function of three things: the attachment point joint's world matrix (already computed inside the parallel blend, GAINS.md "skeleton world matrices inside the parallel blend"), the object's local transform relative to the joint (changes on server updates, edits), and for linksets the children's local transforms relative to the root. Nothing about it needs to be discovered object by object through `updateMove`.

## 3. Design: three layers, each useful alone

**L1. A bulk matrix pass instead of the per-object chain.** After the blend of an avatar (same pool job, joints are warm in cache), compute for every *plain* rigid attachment of the avatar: `world = jointWorld * local` (4x4 SIMD, about 50-100 ns each, so 100 a frame is 0.01 ms on the pool), and the world-space extents from the object's local box (`LLVector4a` transform, the loop's `transformExtents` already does half of this). Results go into a per-avatar array of records `{attachment, matrix, extents, version}`. The main thread applies them in one tight pass (copy the matrix into the drawable's world matrix and bridge matrix, set extents, flag the drawable "driven") instead of walking `updateMove*` for each. What counts as *plain*: single prim or linkset root whose drawable is static-geometry (not flexi, not animesh/control avatar, not particle/light/sound-bearing for now), not selected, not being edited, no recent server position update, not a HUD attachment. Everything else keeps today's path (the loop stays, only for those).

**L2. Culling by the avatar's box, like the rigged attachments.** Rigid attachments get the same treatment `override_bbox` gives rigged ones: their octree box is the avatar's box (which `calculateSpatialExtents` already widens by the static attachments' extents; with L1 these extents come from the bulk pass in the same frame, not from the chain a frame earlier). Then the 293 partition moves a frame stop, as do the bridge extents updates. The cost: an attachment far from its avatar (a big wing, a floating pet 10 m away) makes the avatar box big, which only costs extra draws when the avatar is at the screen edge. The existing `max_attachment_span` clamp in `calculateSpatialExtents` stays.

**L3. The GPU computes the matrix (the N6 end state), only if L1/L2 leave it worth doing.** Per frame the viewer would send per avatar the attachment-point world matrices (55 points at most, 3x4 floats) once, and each rigid entry in the scene table would carry `(avatar slot, attachment point index, local matrix)`; the vertex or instance transform becomes `point[avatar][index] * local`. This removes the 64-byte `memcpy` per moving slot and the pool job. It touches the pull shaders (every pass type has a model matrix path), the shadow packets, the velocity pass (previous-frame matrices: needs last frame's attachment-point table too), and the ray scene instances (TLAS instance transforms are built on the CPU from group matrices; they would need a compute pass writing instance matrices). That is a large, risky change for a saving that C (`GeoSlotMatrix` rows by value, already on) mostly gives: the table does *not* walk the objects, it copies finished matrices. **Recommendation: build L1 and L2, measure, and only then decide about L3.**

## 4. Everything that reads an attachment's transform (the audit that gates stage C)

| consumer | how it is satisfied |
|---|---|
| G-buffer, shadow and velocity draws via the moving slots | read `LLMatrix4` of the drawable/bridge; L1 writes it before the table's per-frame send (`prepare` near `pipeline.cpp:9070`) |
| ray scene casters (matrix id, group `mModel`) | same source |
| octree cull, `stateSort` `updateDistance`, LOD | L2 (avatar box) for the cull; `updateDistance` reads the drawable position: written by L1 |
| avatar box | `calculateSpatialExtents` reads attachment drawable extents: written by L1 earlier in the same frame (better than today's one-frame-late value) |
| texture priority records | volatile faces keep the face loop; they read drawable positions: written by L1 |
| picking, hover, selection highlight, beacons, `getRenderPosition` | read the drawable lazily; need current matrices for avatars in view (L1 covers every avatar the loop covers) and a refresh on demand for avatars the loop skips (out of view / reduced-update): `LLDrawable` gets a "pose version" check, or those avatars keep the old path |
| point lights, sounds, particle emitters on attachments | not plain: old path in the first versions |
| hover text of attachments (`updateText`) | stays in the loop (0.004 ms) |
| flexi prims, animesh, control avatars, children with their own server motion | old path |
| the camera following the own avatar, the self avatar | old path (self is one avatar) |

## 5. Stages and gates

Everything behind `RK_VK_ATT_BULK` (0 off, 1 verify, 2 use); off by default until Adam has used it, as for S3/S4.

- **Stage 0, census (no behaviour change).** Attribute the downstream zones to attachments: split `updateMovedList`, `drawable: updateMove`, `bridge: updateMove`, `octree: partition move` by "belongs to an attachment of an avatar" (drawable flag `ATTACHMENT_ROOT`/parent chain) and by plain / not plain. Count the not-plain reasons (selected, lights, flexi, animesh, recent server update, hidden-update avatars). Gate: the attributable downstream cost plus the loop's 0.245 ms is at least 0.4 ms of the main thread, else stop here and take the tolerance skip (section 7).
- **Stage A, bulk pass in verify mode (mode 1).** Compute the L1 matrices and extents on the pool *beside* the old chain and compare with what the chain produced (matrix max abs difference over 16 floats, extents within 1 cm); count per attachment kind and reason, log the worst cases with object ids. Gate: 0 mismatches above 1e-4 for plain attachments over a full lap with the turn; every mismatch category explained (linkset child order, damping: `updateMoveDampedAsync` interpolates, check that the damped path equals the joint-driven value at rest; the first frame after attach).
- **Stage B, use mode for matrices (L1, mode 2).** Skip `updateMove*Async` for plain attachments and write the matrices/extents from the bulk records; the octree still moves them (L2 not yet). Gate: pictures identical on/off (the A/B alternation used for S3/S4: `ab_zones.py`, on/off shot differences no larger than same-state differences), shadows and ray shadows of attachments, motion vectors of moving attachments, main thread lower by about the measured loop share, `device lost` 0.
- **Stage C, L2.** Rigid attachments culled by the avatar box; no partition moves for plain ones. Gate: no popping at the screen edge in the turn (watch the attachments of avatars entering from the side), the cull counters unchanged except for the removed moves.
- **Stage D (decision), L3.** Only if stages B+C leave a main-thread cost that the `memcpy` of matrices and the pool job still explain (they should be under 0.05 ms).

## 6. Expected gain, honestly

The loop's rigid share (0.245 ms) and part of the downstream moves are removed, the bulk pass costs about 0.02 ms on the pool plus a tight apply loop of about 0.03 ms on the main thread. Estimate: **0.3-0.6 ms of the main thread (3-5% of a 12 ms frame)**, depending on how much of the 0.6-0.9 ms downstream belongs to attachments (stage 0 decides). The rigged stagger (0.19 ms ceiling for an event-driven design) is a separate, smaller item that would ride along: the stable-check and the 8-frame stagger become unnecessary when the avatar box is event-driven in the same record. Effort: stage 0 half a day, A about 2 days, B 2 days, C 2 days including the audit. Risk: medium-high (many consumers; the audit table above is the mitigation, and verify mode is what finds the rest); the failure mode is a visibly wrong or lagging attachment, which the verify mode and the A/B alternation catch before it is default.

## 7. The cheaper alternative, and how it relates

A tolerance skip in the existing loop: do not mark a rigid attachment moved when its attachment point's world matrix is within 1 mm / 0.06 degrees of the matrix last *applied* (not last seen, so slow drift still triggers). From the census about half of the rigid attachments would skip; gain about 0.12 ms in the loop and some of the downstream moves, a day of work, low risk, and a stale error of at most 1 mm. It does not conflict with the design above (stage B replaces it) but it removes the argument for the project by a third. If stage 0 shows the attributable downstream cost is small, this is all we do.

## 8. Decisions for Adam

1. Go for stage 0 (the census, one build, one lap) before any decision? Recommended.
2. Is the first target the plaza only (90+ avatars, 108 rigid attachments in view)? The smaller areas hide the gain.
3. Order after stage 0: the tolerance skip first (cheap, a day), or straight to stage A?
4. L3 (GPU compose) is parked unless B+C leave a measurable cost; agreed?
