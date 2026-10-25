# Expiry of what is built out of view: design (2026-10-25)

Status: DESIGN for Adam's review. No code beyond the measurement in section 8, stage 0. Read with `PLAN-texture-handles.md` sections 17.16 (the cost that stays after a turn), 17.14 and 17.12 (what was counted), `ATTACHMENTS-design.md` (the design that the numbers killed: the lesson on measuring first), and the texture lists of section 17.3-17.5 (the model this follows).

## 1. The problem, with the numbers we have

Adam: "once you have done a full 360 turn you never get those resources back"; sitting still in one spot for hours is fine. Five turn laps (`h6w`, `h6x`, `h6y`, `h6z`, `h6c`, plaza, same yaw before and after) agree:

- The loop interval after the turn is higher by 1.0-3.7 ms (12.4 -> 16.1, 12.8 -> 15.4, 10.4 -> 12.5, 11.5 -> 12.8, 11.1 -> 12.1) and it does **not** decay over the 120 s second still phase (the second phase is twice as long as the first on purpose: it is settled, not still loading).
- The same zones grow every time: the lighting passes (`renderDeferredLighting` +0.3 to +0.9, `post-deferred geometry` +0.3 to +0.5, `local lights` +0.15 to +0.23), the shadow prep (`generateSunShadow` +0.4 to +0.5, `renderShadow` +0.16 to +0.26), `gObjectList.update` (+0.3 to +0.4), `stateSort` / `updateCull` (+0.2 to +0.6), `updateGeom` / `updateMovedList` / `rebuildPriorityGroups` (+0.15 to +0.2 each).
- The **visible set did not grow**: 1,331-1,346 groups in the render map loop before the turn, 1,217-1,229 after, 99.8% the same frame to frame. So the growth is not the main view's own loops; it is what the viewer has built and keeps working on around the view.
- Memory goes the same way (Adam, earlier laps): vertex buffers about 1 GB, 46,000 ray structures at the turn falling to 15,000-20,000 after (the ray scene already expires by age), 8,500-13,000 textures in the list at the club against 1,300 at home. Video memory near its budget is also what the sudden device losses had in common (`PLAN-texture-handles.md` 17.15).

The first still phase is not an unfinished scene (the visible set is the same size, 40-60 s after login); the turn is what builds the rest, and nothing ever lets it go.

## 2. What the code already has (read, not assumed)

- **A visibility stamp per group and camera.** `LLViewerOctreeGroup::mVisible[camera]` (`llvieweroctree.h:271`) holds the frame in which that camera's cull last accepted the group (`setVisible`, `llvieweroctree.cpp:820`), and `mAnyVisible` the latest of the world and shadow cameras (below `CAMERA_WATER0`). Every cull pass already writes these: main view, the four sun cascades, reflection probes, mirrors, snapshots. **"Nothing has accepted this group for N frames" is already recorded**; no influence test has to be written.
- **A release call.** `LLSpatialGroup::destroyGLState` (`llspatialpartition.cpp:903`) drops the group's vertex buffer, buffer map, draw map (`clearDrawMap`, which also frees the scene table fragment so the worker's table follows, and bumps `mDrawMapSerial`), clears the faces' vertex buffers, marks the group `GEOM_DIRTY` and queues it for rebuild (`markRebuild` -> `mGroupQ1`, built by `rebuildPriorityGroups`). Today it is used for teleports and device changes. `mGeometryBytes` tracks the bytes a volume group holds.
- **A cheaper tier that exists and is unused.** `LLDrawable::unload` (`lldrawable.cpp:160`, state `FOR_UNLOAD`: "should be unloaded from memory") only sets the lowest LOD; nothing sets the state any more.
- **The model to copy.** The texture lists (H6): active and dormant lists with wake events, a worker that works out what each texture needs, a budget, and no polling of the dormant ones. Faces of a texture wake it (`wakeGroupTextures`). The ray scene expires structures by age and rebuilds them on demand with a budget (`LL_VK_RT_BUILD_MAX`, now 384 a call).
- **A measurement.** `LLPipeline::sceneStateCensus` (`LL_VK_SCENECENSUS=1`, lap mode `scenecensus`) logs every 600 frames: objects, active drawables, spatial groups of the objects, groups with built geometry, inside the frustum, built and outside, draw infos and triangles in and out, elements, lights, queue sizes. Built, verified, not yet run in a busy plaza.

## 3. Where the post-turn cost can sit (hypotheses; the census tells which)

a. **Per-frame loops over everything built or active, whether visible or not**: active drawables and the moved list (objects that script-move or animate stay active after being seen once), animated textures, particles, flexible prims, hover text. Census: active drawables, queues, objects with text.
b. **The sun cascades and local lights take in what is now built behind the camera.** Before the turn those groups had no draw infos, so they cast nothing; after it they do. Some of that is real (a tree behind you shadows the street in front), some is not (a roof 80 m away whose shadow falls outside the view). Census: built groups outside the frustum and the draw infos and triangles in them; lights registered against visible.
c. **Lighting passes that loop over built groups of certain types** (post-deferred geometry: alpha, glow, fullbright, bump). The visible set is the same, so a loop that grew must be over something larger than it. To read from code once the census shows the counts.
d. **Rebuild queues** that stay long (`updateGeom`, `rebuildPriorityGroups`, mesh dirty groups).
e. **Memory pressure** (vertex buffers, scene table fragments, ray structures) making other things slower or less stable.

Rule from the attachments note: no design detail beyond the general model below until the census says which of these carry the milliseconds.

## 4. The model: one rule, no special cases

**Relevance** of a group is the latest frame in which any camera accepted it (`max over mVisible[]`, with `mAnyVisible` as the cheap read), or in which an event touched it (an object in it was selected or edited, a server update moved or changed an object, a script moved it, an avatar attached to it, it holds the light a visible object needs).

**States**, by age of relevance:

1. **Live**: accepted within T1.
2. **Cold** (age > T1, for example 10 s): nothing visits it any more (the cull already does not, because it is not accepted). Everything that still touches it per frame because it is *built or active* stops: its objects leave the active and moved lists with their state kept, their animated textures and particles are paused, lights keep contributing only if in range of the view (section 6), its texture faces go dormant (the existing wake events). Geometry is kept: waking from Cold costs nothing.
3. **Released** (age > T2, for example 60 s, or sooner under memory pressure): the geometry goes (`destroyGLState` path: vertex buffers, draw infos, scene table fragment; the ray structures follow by their own age rule). Objects, drawables and faces stay (the server owns object lifetime); only what is derived from them is dropped.

**Wake** is the existing path: when a cull accepts a Released group it is marked `GEOM_DIRTY` and goes through `mGroupQ1`; when it accepts a Cold one, its objects return to the active lists and its textures wake (events). The rebuild is **paced**: a millisecond budget per frame, priority by angular distance to the view direction, so the cost of coming back is spread instead of the 2.3 ms spike `rebuildPriorityGroups` shows at the start of a turn.

**Predictive wake** (against the reverse hitch): the cull margin grows with camera angular velocity (the C1 experiment already needed exactly this: factor 1 -> 0.001% misses while turning), so groups about to enter the view are woken a few frames before they are accepted.

**No special cases in the rule.** Partitions that must not expire (the avatar and its attachments, HUD, terrain, water, sky) are not exceptions in the logic: they are never Cold or Released because they are always relevant by the same rule (an avatar's bridge group is accepted every frame it is in view; the own avatar is always touched by the camera). If a case turns out to need a branch, the model is wrong and gets fixed, not extended. Mismatches are found by the verify mode below.

## 5. Which consumers see a dormant group

| consumer | Cold | Released |
|---|---|---|
| main cull, stateSort, render map | not accepted, not visited | not accepted; accepted -> wake |
| sun cascades, reflection probes, mirrors | the same stamps: a cascade that accepts a group wakes it | same |
| scene table / fragments | kept | freed by `clearDrawMap` (event to the worker), re-registered on rebuild |
| ray scene | structures expire by age as today | gone; named again when drawn |
| local lights | the light entry stays while its range reaches the view (distance test against the view, not the group's stamp) | same: lights are not geometry |
| texture lists | faces dormant (existing wake events) | same |
| active / moved lists, animated textures, particles | removed, state kept | same |
| picking, hover, selection | on demand: an object pick wakes its group first (a cursor ray only meets what is in view anyway) | same |
| server updates to objects in the group | applied to the object, group marked dirty, rebuild deferred until accepted | same |

## 6. Memory pressure

T2 is a function of pressure: with video memory over a threshold (the same budget the texture lists use: used 11.5 of 12.8 GB at the club crash) the oldest Cold groups are Released first, like the texture budget picks victims. This ties the expiry to the stability problem of 17.15 without a separate mechanism.

## 7. What can go wrong

- **Reverse hitch**: turning back pays the first turn's rebuild cost. Mitigations: T2 long enough, rebuild budget, predictive wake, LRU so a back-and-forth keeps its set. The measure of success is the 2.3 ms spike staying below one frame's budget.
- **Code that assumes built groups exist** (shadow passes, impostors, the minimap, beacons, anything dereferencing `group->mVertexBuffer` of a group nobody accepted). The verify mode counts these as "late wakes".
- **Hi-Z occlusion** keeps per-group query state (`hizInvalidate`); Released groups must keep it consistent (`destroyGLState(keep_occlusion)` already has the flag).
- **Objects that script-move while their group is Cold or Released** must catch up on wake from their current state, not their history.

## 8. Stages and gates

Everything behind `LL_VK_EXPIRY` (0 off, 1 count only, 2 Cold, 3 Released), off by default.

- **Stage 0, census (built).** Run the `scenecensus` turn lap in a busy plaza and in a dense place (Adam: London crowded, building and avatars all round): numbers before, after the turn, and at the end of the second still. Gate: it names which of a-e carry the milliseconds, and how many groups are built and outside the view (if it is a few percent, stop).
- **Stage 1, age histogram (count only, mode 1).** Per frame: groups by age of relevance (buckets 1, 5, 10, 30, 60, 300 s), built geometry bytes and draw infos by bucket, objects active per bucket. Gate: T1 and T2 values chosen from data, not guessed.
- **Stage 2, Cold (mode 2).** Only the participation that the census named. Verify: every Cold group that is accepted again counts as a wake and must be indistinguishable from a Live one (pictures, shadows). Gate: the loop interval after the turn falls by the named zones' share; late wakes counted; no picture difference.
- **Stage 3, Released (mode 3).** `destroyGLState` path with the paced rebuild. Gate: after-turn loop interval within 10% of before-turn; the first turn back pays no frame above 25 ms from rebuilds; vertex buffer memory and the scene table fragments fall; ray structures still follow; no device lost over 10 teleports.
- **Stage 4, predictive wake** and **stage 5, pressure coupling** only if the gates above leave a hitch or the crashes of 17.15 point at memory.

Every stage needs the same A/B: the lap with the switch on and off (`h6-fixed.ps1`, `zonediff.py`), the turn-twice test (look away, look back, look away again), and pictures.

## 9. Expected gain, honestly

Bounded by the census. Up to the +1.0 to +3.7 ms of the five laps, of which part is real (shadows and lights that fall into the view from behind) and cannot be recovered; the recoverable part is not known. Memory: up to the roughly 1 GB of vertex buffers and the scene fragments for what is behind. The reverse hitch is the price and must be paid in small frames.

## 10. Decisions (Adam, 2026-10-25: yes to all four)

1. The stage 0 census lap runs when London is crowded, and once in a place dense in every direction.
2. The VOLUME partition (prims and meshes) first; avatar, terrain, water, sky and HUD stay out of the first stages by their own relevance, not by a branch.
3. Time-based T1/T2 first, pressure coupling afterwards.
4. T1 = 10 s, T2 = 60 s as the starting point, replaced by the data of the stage 1 age histogram.

Stage 1 (the age histogram, count only) is part of the same census and is built in the same exe: `LL_VK_SCENECENSUS=1` now also logs, for all groups and for the VOLUME partition alone, how many groups, built groups, draw infos, triangles, geometry bytes, elements and active drawables sit in each age bucket of relevance (< 1 s, 1-5, 5-10, 10-30, 30-60, 60-300, over 300 s, never accepted).
