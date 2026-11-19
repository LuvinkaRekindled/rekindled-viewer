# Plan: 100 avatars, the avatar as one owned unit (2026-11-18)

Asked by Adam: "one more thing we need to look at is avatars. we've made big structural changes since we've dealt with them last. assume we have to deal with ~100 avatars fully loaded and animating. what would we do differently? it can be a rewrite, more threads, whatever your best solution is for efficiency and cost per avatar without losing quality."

Status: proposal. Nothing built, nothing measured for this note. Three read-only studies of the tree at 5a6425adb3 (the CPU path, the render path, the history and loading), the zone laps `j2-def1.log` (97.5 avatars in the region, 39 in the motion batch, loop 10.2 ms), `rf-def1.log` (69 to 77 avatars) and `zt-def1.log` (86 loaded, about 31 in the frustum, GPU 5.78 ms). V = read in the code, a log or a doc; U = inference. The targets in section 4 are estimates.

Builds on `PLAN-avatars.md` (stages A to C, live), `PLAN-avatar-frame-record.md` (never built as a record), `PLAN-crowd-worker-and-gpu.md`, `PLAN-velocity-fold-and-skin-once.md`, `PLAN-thread-organization.md`, `PLAN-hitchless-loading.md`. Paths under `indra/`; "voav" = `newview/llvoavatar.cpp`, "vol" = `newview/llviewerobjectlist.cpp`, "pipe" = `newview/pipeline.cpp`, "worker" = `llrender/rkvkworker.cpp`.

## 1. The answer in short

Stop treating an avatar as a set of generic scene objects (drawables, bridges, spatial groups, per-group vertex buffers, joint and motion object graphs). Make it one owned unit with three flat stores:

- a **shared mesh store**: one immutable buffer per rigged mesh asset, built off the main thread, used by every avatar that wears it;
- a **pose store**: the animation of an avatar as a compiled plan over flat arrays;
- one **frame record** per avatar per frame, written by one job, read by everything else.

No new thread. The pool does the work in one fork-join that overlaps the rest of the main thread's frame.

## 2. What an avatar costs today (V unless marked)

### CPU, per frame

| Stage | Where | Today (j2) | Per unit | At 100 visible, animating (U, linear) |
|---|---|---|---|---|
| Table, `idleUpdate` serial part | main; vol:1502, voav:3419 | 0.26 ms | 2.8 us an avatar on average, about 5 for a full one (U) | about 0.5 ms main |
| Pose blend and world pass | pool; vol:1237, voav:7923, `llcharacter/llmotioncontroller.cpp:1008`, voav:14045 | 1.68 ms CPU, 0.26 wall | 48 us a job on average; about 85 for a full pose (the doc measured 99) | 8.5 to 10 ms CPU, 1.1 to 1.3 ms wall |
| Attachment job | pool; vol:1259, voav:4743 | 0.19 ms CPU | 10.4 us an avatar | about 1.0 ms CPU |
| Completions and the ledger replay | main, serial; vol:1343, voav:7970, voav:4765 | 0.31 ms | 8 us a batch avatar, about 14 a seen one (U) | about 1.4 ms main |
| Palettes | pool; vol:1356, voav:14201 | 0.81 ms CPU, 0.1 wall | 1.9 us a palette, about 12 palettes a drawn avatar (U) | about 2.3 ms CPU |
| Owner listing | main; pipe:4657, 4759, 4790 | 0.11 ms | about 0.2 us an avatar or bridge a view (U) | 0.2 to 0.3 ms |
| Pixel area sort, far tier, name tags, moved list | main | about 0.4 ms | | 0.6 to 1 ms (U) |

In all today about 1.35 ms main and 2.8 ms of pool CPU. Scaled to 100 visible: **3 to 3.5 ms main, 12 to 14 ms of pool CPU**, in four separate fork-joins (blend, attachments, worn animated objects, palettes), all joined inside `run_avatar_motion_batch`. Worse than linear: `cullAvatarsByPixelArea` sorts a list up to 2.8 times a frame; the owner listing is (avatars + bridges) times views; the completions are serial.

Why the pose costs 85 us: about 14.8 motions are active on a seen avatar; `LLPose` is a `std::map<std::string, LLPointer<LLJointState>>` with a vector mirror (`llcharacter/llpose.h:51-60`); `LLJointState` is a heap object per (motion, joint); the blender has 216 slots of 6 `LLPointer` (`llcharacter/llpose.cpp:497`) and `addMotion` copies a pointer per joint state per frame (`:690`). Most of those motions lose every joint to a higher priority and are sampled and added all the same.

Why the completions are on the main thread: the attachment job records its writes in a ledger (`AvOp`, `newview/pipeline.h:371`): moved list pushes, bridge moves, partition moves, render order, texture stamps; and the completion pieces write main-thread objects: name tag position, voice visualizer, footstep sounds, lip sync, impostor flags, motion stop requests.

### Render side, per frame (zt-def1, about 31 avatars in the frustum)

- A rigged mesh is an item of a `GeoFrag`, one fragment per spatial group (an octree node of the avatar's attachments, not one attachment; pipe:6770): a 160 byte row and one indexed indirect command. About 9 opaque items, 18 alpha lookups, 8 shadow entries and 6 palettes an avatar.
- Drawn per run (one fragment and one buffer pair): an index buffer bind and one indexed indirect draw (worker:9572-9585). No GPU cull for rigged commands; the cascades' rigged casters are a record rebuilt by a CPU job per pass (kind 2, pipe:14375).
- Skinned in the vertex shader per pass (`shaders/native/lib/rows.glsl:200-222`): 2 to 4 times a vertex a frame.
- One palette per (avatar, skin hash) (voav:13864): `invBind[j] x jointWorld` per joint per skin per frame on the pool (`newview/llskinningutil.cpp:207-210`), 96 bytes a joint with the previous pose, written into a 4 MB arena. Only 4 percent of palettes are unchanged frame to frame.
- GPU: rigged G-buffer 0.36 ms, rigged alpha and fringe 0.32 ms: about 0.022 ms an avatar in view, plus the cascades (0.15 to 0.38 ms with the static casters). The alpha pass runs the full lit path a pixel (cascade shadows, probes, a loop of up to 64 lights; `shaders/native/alpha_pass.inc:106-121`).

The GPU is not what limits a crowd. The CPU and the loading are.

### Arrival and rebuilds

- Mesh unpack and skin parse are on the repo thread, but `rebuildGeom` and `genDrawInfo` are on the main thread and the fills of rigged faces are serial there (`mSerialOnly` when `mAvatar` is set, `newview/llvovolume.cpp:7995`).
- Rigged batches sort by avatar, then skin hash, and the buffers are per group (`llvovolume.cpp:7089-7095`): **the same mesh on two avatars is two vertex buffers, each built on the main thread**.
- Rebuilds after loading: a LOD change (`llvovolume.cpp:2216`), a mesh or skin arrival, a texture or material change (`:837-842`), the hover pick (`REBUILD_RIGGED`, 9,600 a window in `PLAN-hitchless-loading.md:148`).
- Measured stalls (`PLAN-hitchless-loading.md`): 20 episodes of 80 to 390 ms at a login; `updateRiggingInfo` (a loop over every vertex, `newview/llskinningutil.cpp:351`) in stalls of 94 to 101 ms.
- The rigged LOD follows the avatar's box, and mesh makers upload the same triangles for every level; what cuts triangles is our own lists (half a pixel of error, the cascade lists, the far tier impostor).

## 3. The design

### D1. The pose: a plan compiled when the motion set changes

Per avatar, when a motion starts, stops or changes priority (an event, not a frame), build a plan: for each joint and each channel (position, rotation, scale) the motions that can contribute, highest priority first, cut at the first one whose weight is 1. A frame then samples only those curves (the flat curves of B2 exist) and blends two or three where a motion is easing in or out. The procedural motions (look at, eyes, breathing, hands, the walk and foot adjusters, the targets) stay as evaluators that run after the sampled part, in today's order, and are plan entries of their own.

State: flat arrays per avatar (locals as position, rotation, scale; the plan; the motions' clocks and weights). `LLJointState`, `LLPose`'s map and the blender's pointer slots are not touched per frame.

The result must be bit-equal to today's blend. Gate: a compare mode over a crowd lap, 0 differences (the method of B0 to B2c).

Not established, to read before building: the exact rule by which a lower priority fills the weight a higher one leaves, per channel, and the additive motions; the plan follows that rule, whatever it is.

### D2. One job, one record

One pool job per avatar does, in order: the pose (D1), the world matrices (the flat pass exists), the attachment placements (rigid: the joint's matrix times the attachment's offset, into the matrix slot the row already has; rigged: nothing, they are skinned), the animated extents, and the events (footstep, a motion that stopped). It writes the avatar's **frame record**: the joint world matrices of this frame, the attachment matrices, the bounds, the event list.

One fork-join replaces four. It is launched where its inputs are final (after the network and the agent's update) and joined where the first reader needs it (the publish of the frame), so the main thread's UI and object work overlap it. The own avatar's job runs first, or inline, where the camera needs it.

Readers take the record, not the joints: name tags and the voice visualizer read a position when they draw (only the tags in the frustum); the moved list, the bridge moves and the partition moves go, because an attachment's placement is a matrix slot and its visibility is the avatar's (D5); footsteps and stop requests are the event list, drained by the main thread (few a frame). The ledger and its replay go.

Pose and attachments come from one record of one frame by construction. That is the answer to the six one-frame-late bugs of the history (the hover offset written after the pass, a joint cleaned during the pass, the stale matrix copy, the host pose of a worn animated object, the kept set from old rows, the stamp read without a group): each was a second source or a write after the job. Rule: after the job is launched nothing writes that avatar's pose until the join; a late write (the hover offset, an appearance change) is an input of the next frame's job.

Worn animated objects: the host's job places them and runs their pose in the same job (they need the host's final pose; today a second batch phase on the main thread).

Hidden avatars keep today's reduced rate (every 2nd or 4th frame, the clock only when not visible). Anything seen is exact every frame.

### D3. Palettes are made on the GPU

The CPU sends the record's joint matrices once per avatar (about 133 bones of 48 bytes: 6.4 KB) and, once per mesh asset, its inverse binds and joint numbers (static). A small compute pass makes every palette of the frame: `palette[skin][j] = world[avatar][joint(j)] * invBind[skin][j]`: at 100 avatars about 36,000 matrix products. Last frame's palette buffer is the previous pose (two buffers, swapped); no second write.

Gone: the palette jobs (about 2.3 ms of pool CPU at 100), the arena's per-palette writes, the lazy path with its global mutex, the staleness rule (`sTouchSerial`), the palette ids and their fallback copy. "Two palettes of one avatar in one frame" (the walking shadow blink) cannot happen: there is one joint record a frame.

The vertex shader is unchanged (it reads a palette by address). Gate: the palettes of the compute pass compared with the CPU's for a lap (a read back in a diagnostic run only; never from write-combined memory).

### D4. The shared mesh store

One immutable vertex buffer and index buffer per (mesh asset, level of detail, skin), built on the repo thread or the pool when the mesh arrives, with its simplified index lists, its inverse binds, its per-joint rigged extents. Every avatar that wears the mesh uses it.

An avatar's rigged geometry is then a list of **instance rows**: (asset, face, material slots and factors, colour, texture matrix, palette). What the face build bakes per instance today moves to the row: the texture offsets and repeats (the texture matrix table exists), the colour. The bind shape is the asset's.

What this removes: the rigged `rebuildGeom`, `genDrawInfo` and fills on the main thread; the per-avatar copy of every worn mesh; the rebuild on a LOD change (the level is an index range chosen per frame), on a texture change (a row write), on the hover pick; `updateRiggingInfo` per avatar (per asset, once). An arrival or an outfit change is inserting and removing rows.

The row count grows (a face is a row; today's batches merge faces of one material into about 9 items an avatar): expect 20 to 40 rows an avatar, 2,000 to 4,000 at 100. D5 makes that free.

Rigid attachments keep their geometry path (they are ordinary prims); only their placement (D2) and visibility (D5) change.

Not established: how many avatars of a crowd wear the same mesh (step 0 counts it; it decides the memory saved, not the rest); whether any other per-instance value is baked into rigged vertices (to read in `LLFace::getGeometryVolume` before building).

### D5. Rigged draws like the static ones

The instance rows are a persistent table on the worker. Per view (the main view, each cascade, a spot, a probe face) one compute pass tests each avatar's bounds once, picks each row's index list by the projected error (the half pixel rule that exists; the cascade rule for cascades), and writes compacted indexed indirect commands; one multi draw per pass type draws them. All shared buffers live in one arena, so there is no index buffer bind per run.

Gone: the per-run binds and draws, the kind 2 record and its CPU job per shadow pass, the rigged part of the per-frame row copies in `geo_prepare`, the owner loops over all avatars and bridges per view on the main thread (the avatar's bounds are rows of a table the derive thread and the GPU read, like the group rows).

Cull unit: the avatar, with a Hi-Z verdict per avatar (exists). Rigid attachments are listed with their avatar.

The alpha order must stay what it is today. Not established: how rigged alpha items are ordered inside one avatar. If the order is fixed per avatar (by attachment), the plan is avatars back to front and the fixed order inside; if it is by distance per item, the GPU pass writes sort keys and the existing sort is used. To read before step 5.

The far tier (a whole heavy avatar as an impostor beyond 20 m or 40 m) and the impostors stay; with D4 and D5 a heavy avatar costs triangles only, so the tier's thresholds can be judged again by eye afterwards.

### D6. The alpha pass: lights per avatar

A rigged alpha pixel loops over up to 64 lights. Per avatar per frame, choose the lights whose reach meets the avatar's bounds (a 64 bit mask in the instance row, made in the job or on the derive thread); the pixel loops over the mask's bits. The picture is the same (a light that does not reach the bounds adds nothing). The gain grows with the alpha overdraw, the largest GPU zone.

## 4. What it should cost (estimates)

| | Today, scaled to 100 | Target |
|---|---|---|
| Main thread | 3 to 3.5 ms | under 0.5 ms (the table, the event drain, the join's tail) |
| Pool | 12 to 14 ms CPU, four joins | about 3 ms CPU (about 30 us an avatar: pose 15, world and attachments 8, the record 5), one join, overlapped: about 0.4 to 0.5 ms wall |
| Render worker | the rigged row copies, kind 2, the runs | near nothing per avatar (D5) |
| GPU | about 2.2 ms for 100 in view, plus cascades | the same; less in the alpha pass (D6); a compute pass of tens of microseconds for D3 and D5 |
| Arrival | 80 to 390 ms stalls, a copy of each mesh per avatar | row inserts; one build per asset off the main thread |

## 5. What not to do, and why

- **Skin once in a compute pass**: measured 2026-11-15, 0 to 0.2 ms; the vertex shader skins only the vertices the index list names, the compute pass all of them, and it writes and reads 48 bytes a vertex.
- **A held pose for far avatars**: Adam, an avatar at 23 m "was like a slideshow". Seen avatars are exact every frame.
- **Dropping groups by a triangle budget**: avatars at 14 to 40 m lost body parts.
- **A palette cache across frames**: 4 percent unchanged.
- **A dedicated avatar thread, one frame late**: it buys nothing the overlapped join does not, and costs a frame on the own avatar and the camera.
- **A bigger pool**: the work per avatar falls by a factor of four or more; 7 workers are enough.
- **Reading back GPU-visible memory** (the palette arena did 20 fps).

Bug classes this must not bring back, with the check for each: two palettes in a frame (D3 makes it impossible); a cascade drawn without the avatar (the pose-driven extents and the 2 m pad stay; D5's bounds are the same ones); far cascade reuse on cascade 1; stale simplified lists after a buffer rewrite (D4's buffers are immutable); bad inverse binds (validated once per asset at build); duplicate masked and opaque ray tracing draws; HUD attachments snap, never glide; name tags by frustum, not occlusion.

## 6. Steps and gates

0. **Census and a crowd lap.** No behaviour change. Per lap: distinct (mesh asset, level, skin) against instances; faces, rows and rigged buffer bytes an avatar; the split of the pose job (sampling, blend, world pass); palettes an avatar; motions an avatar and how many win a joint. A repeatable lap with about 100 avatars in view (London at a busy hour with the crowd printed). Gate: the numbers of section 2 confirmed or corrected; this plan is revised to them.
1. **D1, the plan.** Behind a compare first: 0 differences over a crowd lap. Then the switch and the deletion of the per-frame use of `LLJointState` and the blender. Gate: the pose job at most 30 us for a seen avatar; Adam's eye on animations, ease in and out, hands, faces.
2. **D2, one job and the record.** The readers move one at a time (name tags, the visualizer, footsteps, placements), each with its compare; then the ledger and three of the four joins are deleted. Gate: the completions' zone gone; main's avatar time under 0.5 ms a frame at the lap's crowd, scaled; Adam's eye on attachments (none a frame off), worn animated objects, the own avatar.
3. **D3, palettes on the GPU.** Compare in a diagnostic run, then the switch and the deletion of the palette jobs, the arena path and the ids. Gate: 0 differences beyond float rounding; shadows of walking avatars (the blink).
4. **D4, the mesh store.** The largest step. First the store and the instance rows next to the old path for one mesh class (opaque rigged), compared by picture; then alpha, then the deletion of the rigged group path. Gate: no rigged fill on the main thread; an arrival without a stall over 20 ms from avatar geometry; buffer bytes down by the census's sharing ratio; Adam's eye on outfits, texture changes, LOD.
5. **D5, the rigged draws and the cascades from the table.** Gate: kind 2 and the per-run draws gone from the worker's census; the same picture; the alpha order unchanged.
6. **D6, the light mask.** Gate: the same picture by image compare on a still camera; the alpha GPU zone down.

Order by value at 100 avatars: 1, 2, 3 are the CPU cap (the turn and the recovery are CPU bound); 4 is loading and memory; 5 and 6 follow from 4.

## 7. Not known, and left out

- The sharing ratio of worn meshes in a crowd (step 0).
- The alpha order inside one avatar (before step 5).
- The classic system body (`LLViewerJointMesh`: one draw per joint mesh, a 15 joint palette as a uniform, `shaders/native/avatar.vert`) has a path of its own; whether it still draws under mesh bodies was not established. It can move onto D3 to D5 as one more asset later; left out here.
- Memory per avatar (CPU and GPU): no figure in any doc or log.
- Whether probe faces draw rigged attachments.
- The cost of the table's begin and end and of `ownersFinish` (no zones of their own).
- Ray tracing: the avatar structures (refit on a period by distance, a compute skin of their own) are not changed by this plan; D4's shared buffers are their natural source later.
- The zone laps carry timer cost (scale by about 0.7); the per-avatar figures are from laps of 31 to 39 active avatars scaled linearly, not from a lap with 100.

## 8. D2 measured, and a reduced D2 (2026-11-18, same day, after reading the code and one zones lap)

Adam: "start D2". Before changing code, one zones lap `d2z-def1` (the clean exe 17:45:29, mode def, RT off, `-Zones`, short; 72 to 84 avatars, 20.0k to 27.7k objects; main loop interval 7.83 ms; zones carry timer cost) and a read of `run_avatar_motion_batch` (`newview/llviewerobjectlist.cpp:1186`), `completeDeferredMotionUpdate` (`llvoavatar.cpp:7970`), `runAttachmentJob` (`:4743`) and the pool (`rkforkjoinpool.h`).

### What the main thread spends on avatars (V, one lap, 84 avatars)

| Zone | ms a frame | Note |
|---|---|---|
| `objectlist: avatar motion batch` | 0.70 | everything below up to `animated objects` |
| of it `completions (main thread)` | 0.29 | of it `idleUpdateMisc (attachment replay, matrices)` 0.16 (56.8 calls); post motion 0.02; name tag position, voice, appearance, lip sync, name tag each 0.03 or less |
| of it `blend (launch to join)` | 0.26 | the main thread works as a pool member here: not pure waiting |
| of it `palette jobs (launch to join)` | 0.10 | |
| of it `attachment jobs (launch to join)` | 0.03 | |
| of it `blend of animated objects worn` | 0.04 | |
| `objectlist: idleUpdate (avatars)` | 0.24 | 84 calls, the serial part |
| owner collect, pixel area sort, far tier | 0.05, 0.05, 0.03 | |
| `ui: LLHUDObject::renderAll (name tags)` | 0.10 | draw, not update |

All of it is about 1.2 ms of a 7.83 ms loop (display 5.06, idle 2.25). That is 15 percent. At 100 avatars, linear, about 1.5 ms.

### What is already done (the plan text of section 2 predates part of it)

- The ledger carries 0.81 writes per avatar loop now (150,000 loops in the lap): moved list 0.35, bridge moves 0.36, partition moves 0.005, moved bridge 0.002, render orders 0.002. Of the eight kinds only two are left in numbers. Bridges and avatars left the octrees (the visible set owner plan, 3.3 and the avatars step), which took the partition moves away.
- `LLPose` already walks a vector (B2b); the flat world pass and the joint views exist (stages B and C of `PLAN-avatars.md`).
- `RKForkJoinPool` has `launch` and `join` already (`rkforkjoinpool.h`: "Several batches can be in flight at once"). The overlap of D2 needs no new pool code. The batch uses only `runLabeled` (launch and join in one call) today.

### What D2 can win on this machine

The whole batch 0.70 plus the serial update 0.24, minus what stays (the table, the event drain, the join's tail, about 0.3 to 0.4 in the plan's own target): **0.5 to 0.8 ms of 7.8 ms, 6 to 10 percent of the main loop**, about 1 ms at 100 avatars. The pool is not the limit (nine workers at 17 to 19 percent each in `rkturn-def1`). It is a real but modest win; the case for the rest of D2 is structure (one source of truth for the pose and the attachments, the six one-frame-late bugs), not frames.

### The reduced D2, in order, each with a compare and a gate

- **D2a: ledger ops out.** MOVED_LIST and BRIDGE_MOVE are 0.71 of the 0.81 writes a loop. First read what their replay still does now that bridges are not in octrees (`lldrawable.cpp:2043`, `pipeline.cpp:2321`): if a replay is bookkeeping nobody reads, delete it; if a reader needs it, give the reader the record. Compare: the lists the replay builds equal what the new path gives, over a crowd lap, 0 differences. Gate: the `idleUpdateMisc (attachment replay)` zone down from 0.16, picture the same, Adam's eye on attachments.
- **D2b: the blend overlapped.** `pool->launch` of the phase 1 blend and attachment jobs before the main thread's other work of `LLViewerObjectList::update` (flexible objects, animated textures, the fetches, the stats) and `join` where the first reader needs the skeleton. Must be read first: every reader between launch and join (flexible prims worn read the attachment's matrix, `updateApparentAngles` reads drawable positions the attachment job writes). The plan's rule: after launch nothing writes that avatar's pose until the join. Gate: `blend (launch to join)` plus the join zones from 0.39 to under 0.1 where the work is independent; no avatar one frame off (hover offset, ground sit, worn animated objects).
- **D2c: late writes become inputs of the next frame.** The hover offset and the ground sit changes of `updateCharacterPostMotion` (written after the pass today, the cause of past bugs) move into the job's inputs. One frame of lag on a ground sitting avatar's hover; Adam's eye. Only then can the palette join (0.10) fold into the avatar's single job.
- **D2d: readers of the record.** Name tag position, voice visualizer, footsteps, lip sync read the avatar's record instead of the joints (each 0.03 ms or less: small, structure only; last).

Not decided: whether D2a to D2d are worth their risk against 0.5 to 0.8 ms. The lap shows the main thread's remaining time is mostly outside the avatars (`generateSunShadow` 1.19, `stateSort (early)` 1.01, `gObjectList.update` 1.17 (the batch 0.70 is most of it), `renderDeferredLighting` 0.79, `render_ui` 0.59). That is Adam's call.

### D2a read, 2026-11-18: not worth doing as written

What the two remaining op kinds do when the completion replays them (`LLPipeline::applyAvOpsRange`, `pipeline.cpp:2327`):
- MOVED_LIST is `mMovedList.push_back(drawable)`: a vector push. Nothing to remove; the list is how `updateMove` later clears the drawable's move bits.
- BRIDGE_MOVE is `LLSpatialBridge::moveInPartition()` = `updateSpatialExtents()` (`lldrawable.cpp:1574`): the bridge's own box, radius and position, then `RKCullShadow::touchBridge(this)`, a write to shared cull rows. That shared write is why it waits for the main thread.

Counts: 0.81 ops a loop at about 57 loops a frame is about 46 ops a frame, of which about 21 are bridge moves. At well under a microsecond each that is **about 0.03 to 0.05 ms a frame**, not the 0.16 ms of the zone `av post: idleUpdateMisc (attachment replay, matrices)`. The rest of that zone is the sort deltas (`applySortDeltas`: marks, LOD updates, rebuilds), the texture stamps (`stampTextures` per moved face), the impostor checks of the same function and the zone's own cost (about 0.3 us a call over 57 calls). Estimated from counts, not measured: one zone around `replayAttachmentJob` would split it.

So D2a as written removes at most about 0.05 ms. It stays a structural clean-up (the ledger concept), not a frame-time step. The frame time in D2 is in the overlap (D2b): the palette jobs launched after the completions and joined where the render side first reads the palettes (hides most of the 0.10 ms join; the render read comes after the rest of the frame's update), and the blend launched in chunks while the serial idle loop (0.24 ms) goes on, joined before the completions (up to about 0.24 ms). Together about 0.35 ms, 4 to 5 percent of the main loop. D2b needs the readers between launch and join read first, as said above.

### D2 re-evaluated for structure, not frames (2026-11-18)

Adam's rule: structure is not skipped for being small; a new correct path must be neutral or faster. The yardstick of the notes above (milliseconds against the loop) was the wrong one. Read again for structure, one stream at a time. The "ledger" is three streams, not one:

1. **The AvOps** (`pipeline.h:371`): MOVED_LIST, BRIDGE_MOVE and traces of the rest.
   - BRIDGE_MOVE replays `updateSpatialExtents()` and `RKCullShadow::touchBridge`. `touch` takes `sCullMutex` already (`pipeline.cpp:12419`), so it is safe from a pool thread, and the non-job path of `LLSpatialBridge::updateMove` already runs `updateSpatialExtents()` inline (`lldrawable.cpp:2043`). The deferral looks like a leftover from when the bridge was in an octree. **Can go inline in the job.** Compare: the extents the replay computes equal what the inline call computed, 0 differences over a crowd lap (the replay recomputes from the same drawables; any difference means the inputs changed between the job and the replay, which is the thing to know).
   - MOVED_LIST: for an attachment the job already ran `updateMove` and set EARLY_MOVE; the moved list pass then only clears EARLY_MOVE and MOVE_UNDAMPED, clears ON_MOVE_LIST, handles ANIMATED_CHILD (`markRebuild` plus `dirtySpatialGroup`) and erases the entry (`pipeline.cpp:2750-2790`). All of it is state of the drawable the avatar owns. **Can become an avatar-owned list** processed at the same point of the frame. Compare: the bits and rebuild marks after the pass equal today's.
2. **The sort deltas** (`applySortDeltasRange`, `pipeline.cpp:5349`): REBUILD_DRAWABLE, PARTITION_MOVE, REBUILD_GROUP, OBJECT_LOD, REBUILD_MESH and others. Their targets are the pipeline's global rebuild lists and group marks, owned by the main thread. Taking them out needs a thread-safe owner for those lists. **Stays until D4 and D5** give rigged and attachment draws a table to write instead of a list to mark.
3. **The stamps** (`stampTextures` per moved face): the texture records are the main thread's. Same answer as 2.

So the ledger does not go whole in D2; two of its op kinds go, and the rest waits for the owners D4 and D5 create. The plan's sentence "the ledger and its replay go" is true only after D5.

**The bug class is the bigger structural prize, and it is D2c.** Today the completion changes the pose after the job (`updateCharacterPostMotion`, `llvoavatar.cpp:7998`): for an avatar sitting on the ground it adds the hover offset to the root's world position, `touch()`es the root, and `walkWorldAfterPass(true)` recomputes the children when the touch serial moved. That is the second source of the pose behind the six one-frame-late bugs, and it is why `canRunAttachmentJob` refuses ground-sitting avatars (`llvoavatar.cpp:4737`). The offset needs no frame of lag: `getHoverOffset()` and the sit state are known when the job starts (`mDeferredSitGroundConstrained` is already captured). It can be applied inside the job, to the root before the world pass. Then the pose is final at the job's end for every avatar, the rewalk goes, ground-sitting avatars take the job path, and a rule "nothing writes an avatar's joints between the job's start and the next frame" can be checked by a counter. Float rounding can differ from add-after-pass in the last bit, so the gate is a tolerance (1e-5 of a metre) over a lap, not bit equality, for those avatars only.

Not yet established, to count before building: how often the completion finds a dirtied joint at all (`clean_serial != LLJoint::sTouchSerial`), by cause, for avatars that are not on the ground. If it is never, the late-write class is already empty for them and D2c is only the ground sit. One `RK_DIAG` counter answers it.

### The structural order of D2

1. **Count the late writers** (one counter, diagnostic lap): which causes dirty a joint after the job.
2. **D2c: the hover offset into the job**, the rewalk out, ground-sitting avatars on the job path. Gate: joint world matrices after the completion equal today's within 1e-5 m over a crowd lap with a ground-sitting avatar (Adam's own avatar sits on the ground in every lap); Adam's eye on a sitting avatar and its attachments.
3. **D2a-bridge:** BRIDGE_MOVE inline in the job, the op kind deleted.
4. **D2a-moved:** MOVED_LIST as the avatar's own list, the op kind deleted.
5. **D2b:** the overlap (the pose is frozen from launch to join now, by construction).
6. **D2d:** readers of the record.
The sort delta and stamp streams stay as they are; each step is neutral or faster by the lap.

### D2 step 1 result: the late writers counted (2026-11-18, lap `late-def1`)

The counter (`AvLate`, `RK_VK_DIAG` only, in `buildPalettesInJob` and the hover block of `updateCharacterPostMotion`, `llvoavatar.cpp`) over 102,000 palette job runs (72 to 84 avatars, a short lap, mode def, exe 18:08:45):

- **A joint dirty after the pose job: 97 times (0.1 percent), all in one burst** (between the 42,000th and 48,000th run, none after), none on the own avatar, none on an animesh, none on an avatar sitting on the ground. The rule "nothing writes an avatar's joints between the pose job and the palettes" already holds in steady state, apart from that one burst (cause not found: an outfit or animation change of a few avatars).
- **Hover offset writes in the completion: 4,308 (4.2 percent of the runs, about 1.7 avatars a frame).** The only systematic late writer, as the plan assumed. They do not show as "a joint dirty" because `updateCharacterPostMotion` rewalks the children right after the write; the array the job filled is then marked stale instead (the palette job refills from the joints).
- **The flat array not filled by the job: 7,343 runs (7.2 percent)**, the same avatars as "of another frame" (7,341). It includes the hover ones; the rest are avatars whose pose job did not fill it (matrices only, no job). Not a late write; it is the cost of the refill path.

So D2c has exactly one thing to move: the hover offset of an avatar on the ground. Design (to build): the main thread captures the hover offset where it already captures `mDeferredSitGroundConstrained` (at the deferral, in `updateCharacter`), the pose job applies it to the root before the world pass, the completion's hover block does nothing for a deferred avatar, the rewalk is not needed, the array stays valid, and `canRunAttachmentJob` no longer needs to refuse ground-sitting avatars. Gate: the joint world matrices after the completion equal today's within 1e-5 m for those avatars over a lap (the counter gives the proof for the rest: late dirty stays 0 apart from the burst).

### Note for D4 from Adam (2026-11-18): the sharing ratio is probably high

Adam: the mesh cache seems useful because there are only 3 to 4 different meshes that people usually wear (the common body meshes), times 2 for men and women. So the bodies, which are the largest rigged meshes of an avatar, are very likely the same asset on most avatars of a crowd. This lowers the plan's "Not established: how many avatars of a crowd wear the same mesh": expect a high ratio for bodies and heads, a low one for hair and clothes. It makes D4 (one immutable buffer per mesh asset, level and skin) worth more than the memory argument alone (buffer bytes and the rebuild-per-avatar cost fall by about the number of avatars sharing a body), and it means a first D4 for the body class alone could carry most of the gain. Step 0's census should still count it, split by what is worn (body, head, other), before the design is fixed. The thing to read before building stays: whether any per-instance value is baked into rigged vertices (`LLFace::getGeometryVolume`).

### D2c built (2026-11-18, exe 18:17:24, lap `hov-def1`)

The ground sit hover offset is added at the end of the pose job (`LLVOAvatar::applyGroundSitHoverInJob`, `RK_VK_HOVERJOB=0` = as before): the same write in the same order (after the world pass, then the children follow), moved from the completion to before the join. `hasPendingGroundSitHover()` is false once the job added it (the animated objects worn by a ground sitting avatar no longer add it again). The attachment loop still refuses ground sitting avatars (`canRunAttachmentJob`): lifting that is the next, separate change.

Counters over 90,000 palette job runs (73 to 83 avatars, short lap, mode def): hover offset writes in the completion **0** (was 4,308 of 102,000), added in the pool job 3,908, the completion found another value or state **0**, a joint dirty after the job **0** (the burst of the earlier lap did not repeat), the flat array not filled by the job 730 (0.8 percent; was 7,343, 7.2 percent). Device lost 0, no errors, fps 132 to 134 in the still phase as before.

Not verified: the picture. The gate left is Adam's eye on avatars sitting on the ground (the avatar, its hair and attachments, animated objects worn by it: no blink, no one hover offset too high or too low) and an A/B with `RK_VK_HOVERJOB=0`.

### D4 step 0 result: the sharing of rigged meshes is low in the London crowd (2026-11-18, lap `skins-def1`)

The census (`LLVOAvatar::skinSharingCensus`, `AvSkins` line, `RK_VK_DIAG` only, every 600 batches) counts the palette entries of the avatars in the motion batch (one entry per skin an avatar wears, built when a draw asked for it) and the distinct mesh assets among them. A short lap, mode def, exe 18:40:14, 43 avatars with entries (of 73 to 83 in the region), 10 lines, the last ones steady:

- **241 skin entries, 206 distinct mesh assets: 1.17 entries to an asset.** A shared store would hold 15 percent fewer rigged meshes than there are instances, not a third or a half.
- **Heavy skins (50 or more joints, where bodies and heads rank, but so does fully weighted clothing): 89 entries on 15 avatars, 62 distinct assets (1.4 to an asset).** About 5.6 skins an avatar overall.
- Most worn assets: 8, 7, 5, 3, 3, 3 avatars (the first three have 70 joints). Three assets account for 20 entries.

So the hunch that 3 to 4 body meshes per gender cover most avatars does not show in this crowd: the heavy skins are mostly distinct assets (bodies are split in parts and sizes, and a body is only a few of an avatar's heavy skins). Caveats: 43 of about 80 avatars have entries (the others are impostors, hidden or not yet drawn), the count is of skins, not of triangles (a body weighs far more than a hat, so the byte saving could still be larger than 15 percent), one lap in one place.

What it changes for D4: the memory argument is weak (about 15 percent of instances, bytes not measured). The case for D4 rests on the other things it does: no rigged fill on the main thread (the serial `mSerialOnly` for `mAvatar` faces, `llvovolume.cpp:7995`), no per-avatar rebuild on a LOD, texture or hover change, immutable buffers. The cheap first question is why rigged fills are serial at all and whether they can run on the pool as the other faces do (the loading stalls of `PLAN-hitchless-loading.md`), before any shared store.
