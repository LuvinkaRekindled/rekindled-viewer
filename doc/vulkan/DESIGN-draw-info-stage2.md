# Draw info removal, stage 2: design (2026-11-12, for Adam to review before any code)

Written after stage 1 (nothing outside the producers and the fragment builders reads a draw info or a draw map any more; see `PLAN-draw-info-removal.md`, steps 3a to 6c). Everything here is from reading the code, nothing is built.

## 1. What the code does today (verified)

- **Who writes draw infos** (the only writers): `LLVolumeGeometryManager::registerFace` (`llvovolume.cpp:6068`: the merge test and the batch fields), called from `fillBatch` into a per task map (`FillTask::mMap`); the particle groups (`llvopartgroup.cpp:1601-1692`, plus the empty entries kept at 1289 and 1445); grass (`llvograss.cpp:707-774`); water exclusion and the invisible types go through `registerFace` too. The terrain paint map (`llterrainpaintmap.cpp:137`) keeps its own local vector, not a group draw map.
- **Where they go:** the task maps are merged into `group->mDrawMap` on the MAIN thread (`llvovolume.cpp:7646` for one group, `7746` for a batched rebuild), and straight after that the scene fragment is built from the group's draw map on the main thread (`llspatialpartition.cpp:518`, and `llvovolume.cpp:7755`; a batched rebuild defers the build to the batch flush with `fragmentLater`). The fragment is the only thing every view reads since stage 1.
- **What reads `group->mDrawMap` after the merge** (the full list, outside `pipeline.cpp`): `LLSpatialGroup::clearDrawMap` (bumps `mDrawMapSerial`, clears the map and the fragment: a rebuild, a release or a death), `validateDrawMap` (paranoia check, off), and the three producers above. In `pipeline.cpp`: the fragment builders (`scene_geo_fill`, `scene_fragment_fill*`, `scene_alpha_plan_fill_set`, `scene_fragment_walk_summary`, `scene_geo_attach`, `scene_fragment_ensure`, `ll_scene_fragment_build`, `scene_fragment_behind`), `shadowPrepRenderMap` (only `mDrawMap.empty()`, for a dropped counter), `renderShadow` (a counter), and the reference checks (`check_references`).
- **What keeps a draw info alive after the fragment is built** (the persistent readers, all small): `LLFace::mDrawInfo` (set in `registerFace`, grass and particles; read in `llface.cpp:500` and `llspatialpartition.cpp:4615-4623`: "is the face's new texture still the one its batch was made with, else the group must rebuild"); `GeoItem::mInfo` (`scene_geo_fill`, the plan fill: used only to make the rigged source after the loop); the fragment's own draw info pointers (`mFallback`, `mGeoFallback`, `RtExtra::mDraw`); and the builder state flags written INTO draw infos (`mGeoServed`, `mAlphaInFragment`, `mAlphaFragTex`, `mRasterFrag`, `mRigAlphaSrc`): they are read by the walk summary inside the same build, nowhere later.

So after stage 1 a draw info is only needed from `registerFace` until the end of `ll_scene_fragment_build`. Nothing later reads one.

## 2. The design: the draw map becomes transient (recommended)

**The idea.** Stage 2 in the plan says "the producers write items". Items are not a per face thing: an item's row needs the group's matrix slots, the fragment's texture numbering, the served mask over all the group's draws of a type, the plan's runs and the rigged sources, all of which are decisions over the WHOLE group, made when the fragment is built. A producer that wrote items directly would have to run the builders inside `registerFace`, which only moves them. What can be removed is the persistence: the draws exist between the producers' merge and the end of the fragment build, and are dropped right after.

- `group->mDrawMap` stays as the hand over between the producers and the builders, but is emptied when `ll_scene_fragment_build` returns (`clearDrawMap` is then only about the fragment). No view ever sees a draw info; memory drops (about 17,000 draw infos in the plaza census, and their texture lists).
- Stage 3 then deletes what is left: `group->mDrawMap` becomes a map local to the rebuild (the `FillTask` maps already are), `LLDrawInfo` is renamed into a producer local batch struct with no refcount, no builder flags, no `LLFace` back pointer; `mDrawMapSerial` becomes the fragment's own build counter; the reference checks go.

**Alternative (what the plan wording says): the producers write items directly.** Rejected for now, for the reason above: it would put the group wide decisions inside `registerFace`, whose merge test needs the previous batch of the same pass and is the most fragile code in the producers. I recommend the transient map, which reaches the same end (no persistent draw info, no mDrawMap reader outside the producers and the builder) in smaller steps. If you want the other form, step 2.4 is where it would start.

## 3. Steps (each built, lapped RT off and ray mode, and committed on its own)

- **2.0 Verify the premises** (no change): that nothing between the merge and the fragment build reads a draw info for a view (the batched rebuild defers the build to the flush: the group must not be drawn in between, check `fragmentLater` against `postSort`), that the async builders of `LLBuilderGuard` read no draw info any more (the shadow builders read fragments since 3n to 3o), and that release and expiry (`clearDrawMap` from the cold mode) only need the fragment. A short report, before anything is edited.
- **2.1 Builders take the draws as a parameter.** `scene_geo_fill`, `scene_fragment_fill*`, `scene_alpha_plan_fill_set`, the walk summary and the others read `const draw_map_t& draws` instead of `group->mDrawMap`; `ll_scene_fragment_build` passes `group->mDrawMap`. No behaviour change; the compare is the build and a lap.
- **2.2 The face stops pointing at its batch.** `LLFace::mDrawInfo` becomes the texture the face was batched with (`mBatchedTexture`, set where `setDrawInfo` was called); `llface.cpp:500` and `llspatialpartition.cpp:4615-4623` read that. Checked by changing a texture on a batched face in the room (the group must rebuild exactly when it did).
- **2.3 The fragment holds no draw info pointer.** `GeoItem::mInfo`, `mFallback`, `mGeoFallback`, `RtExtra::mDraw` go (the rigged source is made inside the loop); the builder state flags move from the draw info into the build's local state.
- **2.4 The map is emptied after the build** (behind a switch for one lap, then no switch): `ll_scene_fragment_build` ends with `group->mDrawMap.clear()` and counts any later reader that finds an empty map for a group with a fragment (the counter must stay 0). The groups of the batched rebuild are cleared after the flush.
- **2.5 The other producers** (grass, particle groups) already write the same map before the build: nothing to change but the empty entries kept at `llvopartgroup.cpp:1289` and `1445` (`ll_group_has_alpha_key` reads the fragment's walk bit, made from them: the bit is made from the map when the fragment is built, so they stay until stage 3).
- **2.6 Stage 3** as in the plan: the member goes, the struct is renamed and slimmed, the serial moves, the reference checks go, the plan's last sweep of switches.

## 4. Risks and what I do not know yet

- **The batched rebuild** (`LLRebuildBatch`, `fragmentLater`): between the task merge and the flush the group has draws and no fresh fragment; today it works because the group is not drawn then. 2.0 must confirm it for every view (probe faces, the HUD, snapshots) because 2.4 turns a mistake there into a missing group, not a stale one.
- **Release and wake** (cold mode, `FS_RELEASED`): `clearDrawMap` already frees the draw map and the fragment together; a woken group is rebuilt. Not read yet in detail.
- **The texture change check** (2.2) is the only reader that lives outside a view; it decides a rebuild. Getting it wrong shows as a texture that does not change, or a group that rebuilds every frame.
- **Cost:** none expected (the builders run the same work; one clear of the map after it).
- **Size:** 2.1 and 2.3 are mechanical (about 15 functions), 2.2 and 2.4 are small, 2.0 is reading. No shader or Vulkan code is touched.

## 5. What I need from you

1. The transient map (section 2) or producers that write items directly? (My recommendation is the transient map.)
2. Is the mirror and the HUD alpha check still pending on your side before I touch the producers' lifetime (2.4)? Both use the paths changed in stage 1.
