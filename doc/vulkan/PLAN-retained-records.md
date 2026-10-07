# Plan: retained draw records (a small GPU scene)

Status: PLAN ONLY, nothing built. Written 2026-10-07 after reading the code; every claim below names where it comes from.

## Why
Measured (blues place, 43 avatars, loop about 16 ms): the main thread builds the shadow and G-buffer records from scratch every frame:
sun shadows 2.4-3.0 ms (renderShadow 1.15 incl. the record builders), pool: materials 0.87, pool: simple 0.41, postSort / render map building about 0.4 per view set.
The deferred packets showed the lesson: a per-frame record builder costs the main thread about what the per-draw code did (worker -1.4 ms, frame unchanged).
Only records that survive from frame to frame remove the cost.

## What the code does today (verified)
- Draw infos (`LLDrawInfo`, llspatialpartition.h:80) live in `LLSpatialGroup::mDrawMap` (type -> vector). They are created in `LLVOVolume::registerFace` (llvovolume.cpp:5644-5860) while a group rebuilds (`LLSpatialPartition::rebuildGeom`, llspatialpartition.cpp:352: `clearDrawMap()`, then `getGeometry`). The only in-place mutation is a later face being merged into the previous info (count/end grow), also inside the rebuild. Grass (llvograss.cpp:706) and particles (llvopartgroup.cpp:822) append during their own rebuild.
- `mModelMatrix` is the region render matrix for static drawables (`getRegion()->mRenderMatrix`, llvovolume.cpp:5692: constant), `drawable->getRenderMatrix()` for active ones (changes per frame, pointer is stable), null for rigged.
- `mTextureMatrix` points at the face's matrix, which texture animation rewrites in place every frame (llvovolume.cpp:790-807).
- Per frame and per view `postSort` copies every draw info pointer of every visible group into the cull result (pipeline.cpp:3898); my builders then walk that list again and make the entries (pipeline.cpp:11580 render_shadow_packets, lldrawpool.cpp pushGeometryPackets).
- Bindless slots are assigned per frame per (view, sampler) pair and reset every frame, so records cannot hold them across frames today.

## Design
**Fragment** = the entries of one spatial group for one pass family, built once and kept:
- Entry content (static until the group rebuilds): vertex/index buffer GL names, index offset and count, flags, cutoff etc. (for shadows: `ShadowPacketEntry`; later `GeoEntry`).
- One matrix pointer per fragment (static: the region matrix; active/bridge groups: the drawable matrix). If the infos of a group/type disagree, the group is not retained and takes the old path.
- Validity key: `LLSpatialGroup::mDrawMapSerial`, a new counter bumped in `clearDrawMap()`. Everything that creates draw infos runs after that clear, so the serial seen at render time identifies the draw info set. A rebuild between two frames changes it, the fragment is rebuilt, the old one released.
- Lifetime: the fragment lives in the group (freed with it); the worker keeps a table id -> entries and gets RETAIN / RELEASE commands in the same ordered queue as the draws (so a release can never overtake a draw that uses it).

**Per view:** the main thread walks the visible groups (hundreds, not thousands of infos), takes each group's fragment (rebuilding stale ones), gathers the few distinct matrices, and sends one `retained` record: fragment ids + matrix blob. The worker concatenates the retained entries into the per-frame table (a memcpy, the worker has 4-6 ms of slack), then draws indirect exactly as now.

**Fallbacks stay:** anything not retainable (texture matrix, odd vertex layout, rigged, mismatched matrices) is collected per frame as today.

## Phases (each behind its own live setting, each A/B-able in one spot)
0. Counters, no behaviour change: group rebuild rate per frame, fraction of draw infos in groups unchanged since the last frame (the hit rate), draws per fragment. Gate: hit rate must be high (expected above 90% in static scenes). If it is not, stop.
1. Shadows, untextured static casters (kind 0). No textures involved, so no bindless problem. One fragment per group (all 13 shadow types). Expected 0.3-0.5 ms.
2. Persistent bindless table: stable slot per (texture object, sampler), freed when the GL texture goes away or is recreated; fragments store texture identity, not GL names; per-frame `setActive` bookkeeping per distinct texture (the texture-streaming priority depends on it, per-draw prepareFast today). This is the hard part and the prerequisite for everything textured.
3. Alpha-masked shadow casters and G-buffer (simple, materials) on top of 2; per-view matrices (mv, mvp, normal) stay per frame, per distinct matrix. Expected 0.9-1.3 ms.
4. Group-level render maps: stop copying every info pointer in postSort for retained types. Expected up to 0.3 ms per view set.

Total expectation: about 1.5-2.5 ms of a 16 ms main thread in busy scenes (10-15%), mostly from phase 3. Phase 1 alone is small; its value is proving the lifecycle.

## Invalidation (the risk list)
| Event | Handled by |
|---|---|
| Group geometry rebuild (edit, LOD change, load-in, face added/removed) | serial bump in clearDrawMap |
| Texture swap on a face | triggers a rebuild today; verify (phase 0 counter: fragment hit but texture differs) |
| GL texture recreated at another size (discard level change) | phase 2: slots keyed by texture object, generation check |
| Vertex buffer reallocated without a rebuild | check getGLBuffer() generation on fragment use (cheap, per fragment) |
| Drawable becomes active/static (matrix pointer changes) | it re-registers its faces, so the group rebuilds |
| Texture animation | infos with mTextureMatrix are not retained |
| Region shift (gShiftFrame) | region matrices change: matrices are gathered per frame, entries unaffected |
| Group destroyed | fragment freed with it; RELEASE queued |
| Cascade avatar cull, rigged alpha, impostors | rigged stays on the per-frame path |

## Not in this plan
Alpha pass (sorted, per draw), avatar and attachment updates, UI. Those need other fixes (order-independent transparency, GPU attachments).

## First step
Phase 0 only: the serial and the counters (about 40 lines), logged with the existing CpuZones report, so the hit rate and churn are known before any record code is written.
