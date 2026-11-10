# Plan: removing LLDrawInfo (the old draw-object layer) - draft, 2026-11-10

Goal (Adam): own the object model, avatars and G-buffer through the scene table; no legacy `LLDrawInfo` path stays. This is the last big piece of K6 in `PLAN-clean-core.md`. Before this plan: groups A to D and the step 1 cleanup are done (`project-old-path-census` notes; commits up to `01605ce20f`). Read from the code on 2026-11-10; nothing here has been built or run.

## Where things stand (facts)

- `LLDrawInfo` (`llspatialpartition.h:126`) is still the producers' output. `LLVolumeGeometryManager::registerFace` (`llvovolume.cpp`, around 6108-6279, plus the merges at 7646 and 7746), the particle groups (`llvopartgroup.cpp`, 1601-1684), grass (`llvograss.cpp`, 707-768), the terrain paint map (`llterrainpaintmap.cpp:137`) and water exclusion make draw infos and put them in `group->mDrawMap[pass]`.
- The scene table is BUILT FROM those draw infos: the collector and builders (`scene_geo_fill`, `scene_fragment_fill`, `scene_fragment_fill_alpha`, `scene_alpha_plan_fill_set`, `scene_fragment_ensure`, `ll_scene_fragment_build`, `collect_main_view_draw_infos`, `pipeline.cpp` 8544-8626 `sCull->mGeoOwner[info]`) walk each group's `mDrawMap`. Deleting `LLDrawInfo` therefore means the producers write table items directly; nothing downstream can be removed first without that.
- Conversion helpers to draw-info-free structs already exist: `ll_geo_range(const LLDrawInfo*)` and `ll_geo_rig_source(const LLDrawInfo*)` (`llspatialpartition.h:277, 344`), `RtMatSource` / `rt_mat_source` (`pipeline.cpp`, the ray scene's material copy, with its own id cache), `GeoEntry::mDebugColor` (the debug view colour no longer needs the draw info).
- Water draws through the classic path (its pull twins are skipped by default, `LL_VK_PULL_TWIN_SKIP` default `"0,1,2"`); `LLFace::renderIndexed` stays for water, the sky's sun and moon and the parcel overlay (under a pull program `LLVertexBuffer::pullDraw` makes each of those one entry). Water is Adam's later rewrite; this plan does not touch it.
- About 300 references in 20 files. In `pipeline.cpp` they sit in about 60 functions.

## Readers by group (what has to stop reading a draw info)

1. **Producers:** `registerFace` (it also MERGES neighbouring faces into one draw: batching test on texture, material, bump, fullbright, matrices), particle groups, grass, terrain paint, water exclusion.
2. **Table builders and collectors** (`pipeline.cpp`): fill items from draw infos; `collect_main_view_draw_infos` for the passes the table does not draw (avatar motion vectors, the ray scene's rigged naming).
3. **Shadow, ray scene and velocity builders:** `build_shadow_static`, `build_shadow_skin`, `build_shadow_alpha`, `shadow_*_kind` helpers, `shadowPrepRenderMap` (15 references), `LLPipeline::renderShadow` (8), `build_velocity_rigged`, `rt_name_rigged_main_view` (12), and the small ray scene adapters `rt_mat_view(LLDrawInfo*)`, `rt_mat_source`, `rt_textures_ready`, `rt_material_id/pin`.
4. **Render maps and `postSort`** (18 references): the per-pass lists of draw infos.
5. **Old alpha loops** in `lldrawpoolalpha.cpp` (walks of `group->mDrawMap[PASS_ALPHA...]`, the transient and glow lists) and the entry builders and old push functions in `lldrawpool.cpp` (`geometryEntry(const LLDrawInfo&)`, `pbrAlphaEntry`, `geometry_packet_ok`, `pushBatch`, `pushGLTFBatch`, ... most of the push functions are the group C step 3b leftovers).
6. **Checks and census scaffolding** that disappears with the class: `ll_scene_geo_verify` (13), `verify_shadow_static`, `scene_verify_view`, `check_references`/`checkReferences`, `checkRenderMapLive`, `sceneStateCensus`, `vis_census*`, `scene_stats_group`, `shadow_pass_census`, `r2_left_over`, `far_avatar_tier`'s draw walk.

## Field by field: who reads it, and where it goes

Reader counts are accesses (`.` or `->`) outside comments in the 13 files that hold draw info code; names shared with other classes (`mStart`, `mEnd`, `mCount`, `mOffset`) are not counted. Destinations are proposals.

| Field | Readers (top files) | Proposed destination |
| --- | --- | --- |
| `mVertexBuffer`, `mStart`, `mEnd`, `mCount`, `mOffset` | mVertexBuffer 116 (pipeline 54, lldrawpool 21, llvopartgroup 15) | the item's `GeoRange` (exists) |
| `mTexture`, `mTextureList`, `mSpecularMap`, `mNormalMap` | 81 / 56 / 19 / 19 (pipeline 59 / 26 / 11 / 11) | the item's texture handles + `RtMatSource` (exists) |
| `mTextureMatrix`, `mModelMatrix` | 58 (llvovolume 27) / 67 (pipeline 55) | the item's matrix slots (rows already carry them) |
| `mSpecularMapMatrix`, `mNormalMapMatrix` | 0 | delete now (no reader found) |
| `mAvatar`, `mSkinInfo` | 91 (pipeline 69) / 29 | `GeoRigSource` (exists) |
| `mGLTFMaterial`, `mSpecColor`, `mEnvIntensity`, `mAlphaMaskCutoff`, `mDiffuseAlphaMode` | 44 / 5 / 6 / 12 / 3 | `RtMatSource` fields (exist) |
| `mFullbright`, `mBump`, `mShiny`, `mHasGlow` | 23 (pipeline 11, llvopartgroup 9) / 11 / 2 / 2 | the pass type (already the key) or the item's flags |
| `mBlendFuncSrc`, `mBlendFuncDst` | 3 / 3 (particles) | the particle run / alpha item (kind 29 runs carry a blend pair id) |
| `mHasMaterial`, `mMaterialID`, `mShaderMask` | 3 / 3 / 3 (llvovolume: the batching test) | producer state in `registerFace` (merge key), not table data |
| `mAlphaFragTex`, `mAlphaInFragment`, `mRigAlphaSrc`, `mGeoServed` | 5 / 6 / 6 / 6, all in pipeline.cpp | vanish: they exist only because infos and items coexist |
| `mExtents`, `mHasExtents` | 13 / 3 (llvovolume 8, llface 3) | the census only (`LL_VK_VISCENSUS`): delete with the census, or keep on the item if the census is wanted |
| `mRtMaterial` | 1 (pipeline) | `RtMatSource::mCache` (exists) |
| debug colour | via `getDebugColor()` | `GeoEntry::mDebugColor` (exists) |

## Stages (each behind a check against the old result, one path after a move)

**Stage 0, groundwork (small).** Delete the two fields with no readers; settle the open questions below; add a per-type counter of how many draw infos exist per frame so progress is visible.

**Stage 1, consumers stop reading draw infos.** In this order, each compared against the old result with the existing verify code before the old walk goes: (a) the ray scene adapters (`rt_*`: build `RtMatSource` where they take a draw info; cache moves); (b) velocity (`build_velocity_rigged`, `avatarVelocityWanted`); (c) the shadow builders and `shadowPrepRenderMap` (read fragment items only); (d) `collect_main_view_draw_infos` and `rt_name_rigged_main_view`; (e) the old alpha loops in `lldrawpoolalpha.cpp`; (f) the entry builders in `lldrawpool.cpp` take an item, not an info. Done when no consumer outside the producers and the table builder reads an info.

**Stage 2, producers write items.** `registerFace` builds the group's items directly (keeping the merge logic as producer state), one pass type at a time: opaque simple types first, then bump / shiny / glTF / rigged types, then alpha, then particles (`mParticleRuns` already carries them), grass, terrain paint, water exclusion. After each type the group's `mDrawMap` entry for it stays empty and the check says so. The fragment's serial logic (`mDrawMapSerial`, `scene_fragment_behind`) moves to the new build counter.

**Stage 3, delete.** `LLDrawInfo`, `mDrawMap`, `mDrawMapSerial`, the render maps and the `postSort` lists, the old push functions, the verify / census / reference-check scaffolding of group 6, `pushBufferVerts(LLSpatialGroup*)` (the one debug view that still reads a draw info is converted first, per the rule that debug views are converted, never dropped), and the highlight's old-walk fallback.

## Open questions (answer before Stage 2)

- Where does the merge logic run, and on which thread? `registerFace` is called from the group rebuild; the table's builders run on pool threads. Items built on the main thread need a hand-over (the row/fragment write path exists for rows; check whether it takes a draw built mid-rebuild).
- How do alpha groups sort without infos? Today the alpha lists sort draw infos by distance; the alpha plan (`LLAlphaPlanView`, kind 29 runs) already carries sort keys: confirm it needs nothing else from the info.
- Lifetime: infos hold handles (`LLVBRef`, `LLTexRef`, ...) that keep the frames-in-flight safe. The item must hold the same handles (the GPU lifetimes plan, `GPU-LIFETIMES-revised.md`, comes after this and assumes the table owns them).
- Do the verify codes (`ll_scene_geo_verify`, the census) have a replacement check once there is no old result to compare against? Proposal: keep a cheap per-type item count against the face count as the standing check.

## Not in this plan

Water (later rewrite), the selection and wireframe views (converted to the table later, per the rule), the physics and raycast debug draws (CPU meshes, no draw infos), avatars' stage D (skin once, hidden avatar posing, load priority): separate plan items.
