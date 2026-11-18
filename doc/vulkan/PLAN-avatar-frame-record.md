# Plan: the avatar frame record (R1 stages a and b of `overview-brief/REPORT-fable-avatars.md`), with the numbers as they are now (2026-10-26)

Status: **plan only, nothing built.** Numbers are from lap `avt2-` (heavy plaza crowd, after the cheap batch `1b24c599`; `RK_AVATAR_TIMERS` + `RK_VK_ATTCENSUS`) unless stated. Fable's own figures were estimates from the code shape; where our per-call census disagrees it is said.

## 1. What the serial avatar update costs now

Per frame, 69 avatars in `idleUpdate`, 42 with the deferred completion, 385 attachments (225 rigged, 160 rigid, all with a bridge), 35 avatars with the attachment loop:

| piece | ms | note |
|---|---|---|
| `idleUpdate` (69 avatars) | 0.667 | 9.7 us each |
| `updateCharacter` | 0.39 | of it root position+rotation 0.12 |
| own avatar inline (motion update 0.154, post-character 0.21 for 4 avatars) | 0.36 | `canDeferMotionUpdate` excludes the own avatar |
| **deferred completion (42 avatars)** | **1.38** | 33 us each |
| of it: **attachment loop** | **1.12** | rigged 0.26 (1.16 us each, 171 of 225 skipped by the 8-frame stagger), **rigid 0.64 (3.97 us each, 160)**, rest = glue, `override_bbox` 0.05 |
| downstream of the loop (moved list, partition moves, bridge moves) | 0.2-0.3 | `updateMovedList` 0.22, `octree: partition move` 0.27 (437 calls, not all attachments) |

The zones and `AvScope` timers inflate small items (about 0.3 us a reading), so the real rigid cost is perhaps 0.45 ms of the 0.64. **The part a frame record could take is the rigid attachment chain: about 0.45-0.64 ms in the loop plus 0.1-0.2 downstream. Fable's 1.3-1.8 ms for R1 as a whole is not supported by these numbers** (the stage 0 census, `PLAN-texture-handles.md` 17.14, found the chain at 0.14 ms in a thin crowd and about 0.6-0.9 ms in the heavy one before the cheap batch).

How the rigid attachments move (per frame, 160 of them): joint matrix unchanged since the last visit (within 0.1 mm) 45, moved more than 0.1 mm 115, more than 1 mm 72. So 88 of 160 (55%) move less than 1 mm between two visits.

## 2. Stage a (done, except a rest worth 0.01 ms)

Already in `1b24c599`: the clean skeleton walk of `updateCharacterPostMotion` is skipped for deferred avatars (0.078 -> 0.008 ms), `LLJoint::setRotation` equality restored (updateOrientation 0.150 -> 0.012 ms), root rotation not set for a turn under 0.03 degrees. What is left of stage a, moving `updateHeadOffset` into the job, is about 0.01-0.02 ms: not worth a change.

## 3. Stage b as the report describes it

**What:** the batch job (`run_avatar_motion_batch`) computes, after the blend and `updateWorldMatrixChildren`, for each plain rigid attachment: its world matrix (`joint world x local`, what `LLXformMatrix::update` does with the attachment point as parent), its world extents, its face centres (`LLFace::updateCenterAgent`), the bridge's extents and whether the bridge's box left its octree node. One POD record per attachment in a per-frame arena. The main thread applies them in a tight loop (copy the matrix into `mXform` / `mWorldMatrix` through a setter that skips `update()`, set the extents, `mGeneration++`, push only the bridges that left their node onto the move lists).

**What disappears:** `updateMoveDampedAsync` + `LLDrawable::updateXform` (damping lerp written for server driven objects, matrix rebuild from a quaternion, `markRebuild` checks) and `LLSpatialBridge::updateMove` (`balance()`, node test) for those attachments; 243 of 260 moved-list entries were "already moved this frame" in the census, so the list becomes mostly empty.

**What stays on the old path (23%):** attachments or linkset children with a light, flexible prims, particles, sound, a selection, animesh, HUDs. The apply pass runs the old loop for them.

**Boundary and what must be true:** the job reads the camera, joints, node bounds and the attachments' `LLViewerObject` transforms (written by the idle loop, which runs before the batch). It writes only the record. It must not copy an `LLPointer` (`LLRefCount::mRef` is a plain `S32`). Tripwire: a thread-local `gInAvatarJob` asserted false in `LLPipeline::markMoved/markRebuild`, `LLSpatialPartition::move`, `LLRefCount::ref/unref`, `LLSelectMgr::getSelection`.

**Risks:** (1) a consumer that reads the attachment's drawable between the old and the new write point: picking, `updateDistance` in `stateSort`, the scene table's `sMovingMatrix` copy (`pipeline.cpp` ~9602); all run after `idle()`, which the apply pass precedes. (2) The damping semantics: `updateXform` damps when the drawable is visible and `dist_squared` is in a range; a job version must reproduce or drop it (for attachments the damping target is the joint, so it is dropped in the stage 0 reading, to be checked). (3) The 23% on the old path doubles the code to keep right. (4) Attachment extents feed `override_bbox` of rigged ones.

**Expected gain (heavy crowd): about 0.4-0.5 ms net** (rigid loop 0.64 -> about 0.15 for the apply copy, downstream 0.2 -> about 0.05, the job's wall grows 0.1-0.2 ms on the pool). Effort: large (a job record, an apply pass, verify mode that runs both paths and compares matrices and extents for a few laps, the tripwire, the old path kept for the 23%); the failure mode is silent (an attachment a frame off, a stale bridge box).

## 4. A smaller way to most of the same: skip the chain for rigid attachments whose joint did not move (stage b0)

The attachment's joint world matrix is known when the loop runs. Keep the last *applied* matrix per attachment (64 bytes on the `LLViewerJointAttachment` / object); when the new one differs by less than 1 mm in position and 0.06 degrees in rotation, do nothing for that attachment this frame (no drawable move, no bridge move, no `updateText`). The rigged attachments already do the equivalent (8-frame stagger), so the visual idea is accepted. Anything that selects or edits an attachment, a non-plain attachment, a first visit, or a changed attachment list brings the full update back.

- Gain from the census: 88 of 160 rigid attachments are under 1 mm: **about 0.3-0.35 ms in the loop and 0.1 downstream, 0.4 ms in all** (heavy crowd), a standing crowd more, a dancing one less.
- Effort: small (about 40 lines in `idleUpdateMisc`, a setting `RKSkipStaticRigidAttachmentUpdates`, counters), no threading, no new write points. Failure mode: a visible lag of up to 1 mm, rotation up to 0.06 degrees (1 mm at the end of a 1 m prim).
- What it leaves: the moving 72 rigid attachments (0.3 ms) and the old chain for them: stage b would take those later, for about 0.2 ms more instead of 0.4-0.5.

## 5. The rest of the report, with the numbers now

- **Stage d, own avatar into the batch:** 0.154 + (part of) 0.21 = about 0.15-0.25 ms; needs `requestStopMotion` network messages queued into the record and sent in the apply pass. Medium effort, medium risk (the camera reads the own avatar).
- **R2 (contiguous skeleton):** after the cheap batch `updateCharacter` is 8.4 us per avatar (0.39 ms), `updateOrientation` 0.012: a layout change would gain a fraction of 0.3 ms for a medium-large change. Not recommended now.
- **R3 (hide the job behind the rest of idle):** the zone `objectlist: avatar motion batch` shows 0.02 ms blocked of 1.6 ms; the pool job is already finished when the loop needs it. Nothing to hide.
- **Predicates and polling-shaped pieces (name tag text, appearance animation, `isVisible` repeats):** 0.4-0.5 ms in Fable's estimate, never measured as a whole; the per-piece numbers we have are small (name tag 0.097-0.19, appearance 0.13).

## 6. Recommendation

1. **Build b0 first** (small, no threading, about 0.4 ms), with a counter of attachments skipped, and check the look of attached prims on a walking and dancing avatar.
2. **Then decide on b with the leftover:** after b0 the full stage b is worth about 0.2 ms more; I would not build it unless the main thread is still the limit and nothing cheaper is left.
3. Stage d later if the own avatar's 0.15-0.25 ms matters.

Total reachable from this list: about 0.4 (b0) + 0.2 (b) + 0.2 (d) = 0.8 ms of the 13 ms main thread; b0 alone is half of it.
