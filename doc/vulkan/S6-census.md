# S6 census: what is left in the GL-shaped stream, and what the worker spends (2026-10-22)

Question: is S6 ("render thread loop and removal of the GL-shaped stream", PLAN-gpu-scene.md) worth building, and what has to come before it? Facts first, then what they say, then the order I propose. Runs: London, ~99 avatars, build after the S5 crash hunt (commit 0446ef56) plus census counters. Logs `<work>\run-logs\s6a..s6e.log`. Switches: `LL_VK_OPSTATS=1` (stream and worker counters, every 600 frames), `LL_VK_UNICENSUS=1` with `FS_CPU_ZONES=1` (per-zone counts, section 2). One run each: numbers are good to about 10 %, not better.

## 1. Main thread: what writing the stream costs

Per frame (run s6a, no phases, loop 15.3 ms):

- 18.2k calls from the viewer become 9.0k records; **writing them costs 0.60 ms** (33 ns a call).
- The front-end entry points (what the viewer code spends before a record exists): shader bind 0.12, uniform writes 0.38 (20k calls), uniform matrices 0.11, texture unit binds 0.05 + 0.07, setBuffer 0.10 (1.8k), draw 0.34 (3.2k), the rest 0.1: **about 1.3 ms**.
- Together about 1.9 ms of a 15.3 ms loop. Before S5 (Oct 21 run) the same entry points were about 2.2 ms plus 0.85 ms of record writing: S5 took about 1.1 ms of this out.
- The viewer's own code that computes those values is not in these numbers; deleting the stream would not delete it.

## 2. Where the remaining records and draws come from (run s6b, innermost CPU zone, per frame)

Totals: 8,587 records, 3,124 draws, 1,702 setBuffer, 3,676 syncMatrices, 15.7k uniform writes (4.9k changed), 12.2k texture binds, 989 shader binds.

Records: post pool alpha (post water) **2,540** (the S5 alpha runs: no draws of their own, about 9 records per shader bind), renderGeomDeferred 1,308, bindDeferredShaderFast 925, local lights 325, terrain 328, UI panels ~400 together, projectors 248, bindDeferredShader 295.

Draws still written the GL way (about 3,100):

| group | draws | what |
|---|---|---|
| scene geometry in group buffers that no slice took | **~1,800** | renderGeomDeferred 434 (why not served: not yet checked), post-pool fullbright 359, terrain 316, alpha-mask pool 247, glow pool 225, fullbright alpha mask 89, glTF 59, avatar ~60, bump / simple ~30 |
| UI / HUD | ~600 | main panel 287, name tags 78, toolbars and small panels |
| lighting volumes | ~550 | projectors 238, local light boxes 313 |
| post, water, sky | small | |

`kGeoTypes` (pipeline.cpp) has 13 pass types (simple + the material variants). Fullbright, glow, bump, terrain, the alpha-mask and glTF pools are not among them. The plan says UI, terrain, water, sky, HUD, text stay on the old path "until they get their own entries"; terrain is the only one of those that is group-buffer geometry.

## 3. Worker: where its time goes (runs s6d / s6e, per presented frame, wall 15.3 to 15.9 ms)

- **Operations 12.6 to 15.1 ms of 15.3 to 15.9 ms.** Worker idle 0.6 to 1.9 ms a frame; the main thread never waits for the worker (0.0). The worker is nearly the limiting thread now, not the main thread.
- `shadowPackets` (every typed packet record) 5.2 to 6.3 ms; `rtScene` 2.4 to 2.9 ms in 2 calls (serial); `draw` (the GL-shaped draws) 2.5 to 2.6 ms for ~2,700 (0.96 us each); present 0.7 to 2.0; the rest under 0.3 each.
- Packet kinds (the per kind timer runs per command buffer submit, and there are **2 submits per presented frame**; multiplied up): alpha runs, kind 29, **about 3.7 ms for ~740 records (5 us each)**, prepare (kind 21) 0.8 ms (one 810 us call), G-buffer table draws (kind 22) 0.5 ms (26 calls), rigged note kinds 5 / 6 / 7 about 0.7 ms together, everything else under 0.1 each. Sum 6.0, equal to the dispatcher's figure (my first reading printed half of this: it divided by submits, see the note at the end).
- **`gMutex` is not a blocker:** 0.02 ms a frame of waiting before packet records.
- Reading the blob costs nothing (30 ticks); no record arrives without data.

## 4. What this says

1. **Deleting the GL translation is not where the worker's time is.** The GL-shaped `draw` is 2.6 ms; the largest single item is my S5 alpha run records (3.7 ms), then the ray scene (2.9 ms), which is not stream-related at all. S6 as written (delete the translation) would not remove either.
2. **Serving the rest of the scene geometry from the table (about 1,800 draws) pays on both threads:** main thread: draw / setBuffer / syncMatrices / the Fast binds per pass (guess 0.4 to 0.6 ms, not measured); worker: up to 1,800 x 0.96 us = 1.7 ms of `draw`, less what the table draws cost instead (S3 measured 0.68 -> 0.26 ms worker per frame for the types it took). It also removes most of the state that crosses pass boundaries.
3. **Multi-threading blockers seen so far:** the order-dependent stream state of the ~3,100 GL-shaped draws (UI, lighting, post, leftover geometry); single-owner tables (retire / pins / bindless sets written by the worker thread; kept single-writer on purpose); the ray scene call (serial, 2.9 ms); and per frame work that is one big record (prepare, 0.8 ms). Not a blocker: `gMutex`.
4. The second command buffer submit per frame (2 a presented frame) is from pass ends ("command buffer 2.0" in the pass census); worth knowing when passes become self-contained.

## 5. Proposed order for the rest of S (Adam's call; threading plan comes after)

1. **S3 extension: the remaining scene geometry pass types**, one type at a time with a check first (can the existing pull programs read it? per-draw values?): start with the 434 draws left in renderGeomDeferred (find out why they are not served), then fullbright, glow, alpha-mask pool, glTF, bump, terrain. Gate: draws a frame, main-thread zones, worker `draw`, pictures equal.
2. **Alpha run cost on the worker (S5 follow-up):** 740 records a frame at 5 us. Options: fewer records (merge fullbright and lit programs per the S5 design note, merge runs across groups), cheaper per record (pass tables made 16k times in a run, begin record work). Measure before choosing.
3. **S6 proper**, rescoped: passes become self-contained descriptions for every pass whose draws are typed by then; the GL-shaped stream stays only for UI, lighting volumes and post until they are typed (separate decision). Deleting translation code for what moved is cleanup, not a gain.
4. **S7** (Hi-Z, GPU LOD, group visibility, OIT) as planned.
5. Then the threading plan (recorders per pass or pass slice, single-owner tables, ray scene off the render thread).

## Note on the census counters

`packet kinds, worker` (llvkworker.cpp, `on_shadow_packets`) counts per command buffer submit; the viewer's frame counter there is the submit count, 2 a presented frame. `LLUniCensus` now also counts records, draws, setBuffer and syncMatrices per zone (llgpuzonetimer.cpp, lluniformcensus.h, llvkasync.cpp `noteRecord` / `frontStart`).

## 6. Alpha run cost on the worker: measured and cut (2026-10-22, runs s6f / s6g)

`LL_VK_SCENE_TIMES=1` now splits the alpha run record (kind 29) further (`us per record: ... (program check, ranges loop, stamp+flight, uniforms, prepare_draw+vertex input, bindless flush)`). Before (s6f): 4.53 us a record = **2.13 us "copy"** + draw call 1.09 + ranges loop 0.44 + stamp/flight 0.13 + uniforms 0.19 + prepare_draw 0.18 + program check 0.11 + rest.

- **The 2.13 us was a diagnostic reading the draw commands back from write-combined scratch memory** (`a.mZeroStatic += out[k].vertexCount == 0`, 3 reads a record at about 0.7 us each: the same gotcha as the resident tables, see the S2 notes). Fixed: counted from the source copy. Both loops (static and rigged).
- **Uniforms by name**: `hasUniform` x2-4 and `setInts` x3-4 per record, hashing strings and building temporaries (two of the names are longer than the small string size). Now resolved once per program (`Program::packetUniforms()`, reset in `create`) and written with `setFloatsAt`.
- **G-buffer draw records (kind 22) made the whole matrix table again for every record**: 26 records a frame x 7.7 us. Now one table per view / projection / prepare, shared by the records of a frame (`geo_draw`, `shared`).

Result (s6g, same scene, 100 s): alpha record **4.53 -> 2.24 us**, kind 29 **3.7 -> 1.5 ms a frame**, kind 22 0.54 -> 0.19 ms a frame, `shadowPackets` 5.2-6.3 -> 3.4 ms a frame, frame pacing 15.3-15.9 -> 13.3 ms (the worker's operations 12.6-15.1 -> 13.2 of 13.26 ms: it is now the limiting thread outright; the main thread waited 0.8 ms a frame, `present` takes 4.5 ms). 0 slot-check findings, 0 stale, 0 refused, 0 device lost. Pictures not compared (the changes are CPU side only; Adam's eyes on the next run).

Left in the alpha record (2.24 us): the draw call itself 1.07 (`drawPackets`), the ranges loop 0.41 (fragment lookup, safety once a fragment a frame, rebuild checks, slot check), stamp + flight 0.20 (crash diagnostics, kept), prepare_draw + vertex input 0.18, bindless flush 0.04.

**Open, found on the way:** `geo_check_slots` (the crash hunt's safety check) costs **0.35 ms a frame** of the worker's `prepare` listing (about 6,400 slot looks a frame, 55 ns each, cache misses). It found 0 problems in 40M checks since the stand-in fill. Cheaper without losing the protection: do the freed-slot test only when a slot lifecycle event happened (a generation counter bumped at free / sweep), and leave "not in this frame's set" to the flush that the draw records already do. Not done: it is safety code, decide with Adam.

## 7. Remaining pass types: type table generalised, fullbright served (X0 / X1, 2026-10-22)

**The 434 draws left in `renderGeomDeferred` are occlusion query cubes** (`doOcclusion`: one cube draw per occlusion group, 377 queries a frame): not geometry, S7's (Hi-Z replaces them). Not served.

**X0:** the table's pass type count is now one shared constant (`LLVkBridge::kGeoPassTypes`, llvkbridge.h; the viewer's `kGeoTypes` list is static_asserted to it), the alpha items' type numbers follow it (`2 * count`, `+ 1` rigged), the served masks are 64 bit. Adding a type = one entry in `kGeoTypes`, one pull program, the pool's call.

**X1: opaque fullbright** (`LLDrawPoolFullbright::renderPostDeferred`): `gDeferredFullbrightPullProgram` / `...SkinProgram` = `alphaPullV.glsl` (IS_FULLBRIGHT_PULL, same rows and matrices as the G-buffer table) + `fullbrightF.glsl` without IS_ALPHA. HUDs stay old (the HUD cull is not prepared; `geo_list` is false while `sRenderingHUDs`). A pass type is served only when the pool has its pull program (`geo_type_available`). The fullbright vertex buffers have **no normal array** (5,868 refusals, reason 7 in the new census `fallback draws by type and reason`), so the normal array is optional for these passes (`ll_pass_needs_normals`). London: post pool fullbright old-path draws 354 -> 29 a frame (the left ones have a texture matrix), 367 table commands a frame, picture the same.

**The device lost this exposed (x1c, x1d: 2 of the 2 valid runs):** a table draw (type 13 fullbright, then an alpha run) sampled the memory of an image destroyed seconds before (fault page last held by a destroyed 2048x2048 texture; the report said "the GPU is inside [scene table draw, type 13]"). The same class as the 2026-10-21 snapshot device lost. Causes found in the code:
1. `rebuild_sampling_view` (swizzle change) destroyed the old view without putting its id in the destroyed list and zeroed `mViewId`: the slot keyed by the old id was never retired, every frame's set kept the dead view, and the image destroyed later was noted under another id.
2. Retire wrote the stand-in only for pinned slots; the other frames' sets kept the dead view of an unpinned slot, and a slot given back (sweep) kept its last descriptor in every set.

**Fix, structural, not per table:** the slot lifetime follows the VIEW. `Renderer::bindlessForgetView(view)` runs at the point an image or its view is given up (`Context::destroyImage`, `rebuild_sampling_view`; hook `Context::setViewForgetHook`): the slots whose info names that view handle (index `mViewSlots`, kept where a slot takes a view) get the stand-in in every frame's set (pin log), whatever the key or id says. Also: every retired slot goes to the log, a slot given back holds the stand-in in all sets. Census: `[the view of slot N ... the id list does not know it: repaired by handle]` counts the cases the old id rule would have missed.

**Status:** 3 clean runs of about 3 minutes each (x2a, x2b, x2c) after, 2 crashes of 2 before. The census saw 0 repairs in x2c: the by-handle rule did not have to fire in that run, so it is not shown to be what fixed it (the all-sets retire and the stand-in on given back slots went in at the same time). Open: more runs; a closest avatar's shirt and skirt (a rigged outfit) disappear for a frame while it walks (Adam, 2026-10-22, not seen on a still avatar): counters for failed palette requests (`[palette] request failed`, `palette requests that failed` in the RiggedPackets line) are in to tell whether a missing palette is the cause. Next types: glow, alpha mask, fullbright alpha mask, glTF, bump, terrain.

## 8. Glow served from the table (X2, 2026-10-22)

Opaque glow (`LLDrawPoolGlow::renderPostDeferred`) uses the emissive pull programs the alpha glow already had (`gDeferredEmissivePullProgram` / `...SkinProgram`, same rows: the row's emissive offset is now written for every item, 0 when the draw has no emissive array). `ll_pass_arrays(type)` says which arrays a pass type needs (fullbright: none, glow: the emissive array, the others: normals). London: post pool glow old-path draws 225 -> 19 a frame (texture matrix draws), 153 table commands a frame, the Soho neon and the lamp glow draw as before. 6 clean runs of about 3 minutes (x3a-x3f), none lost the device. HUD, glTF glow (`PASS_GLTF_GLOW`) and passes with a texture matrix stay old.

**Open, looked at with census counters (all in the log):**
- Crowd avatars' clothing and wings stay gray ("same avatars every run", nearest ~32 m, 7th nearest, all of their clothes): the viewer's own texture list has 59-276 images "with no data yet, 0 being fetched" in every run, also in logs from before S5 (65-105 in vj-ab1, alpha-census1, pc-ab2). Their faces have a pixel area of 0.4-2.6 (rigged mesh attachments among them) and a desired discard level of 5-6: the viewer believes they cover nothing and asks for the smallest level. Not caused by the impostor limit (RenderAvatarMaxNonImpostors 25; with 0 the stuck count rose to 276), not by FSEarlySort / FSAsyncBuilders (both 0: 167 stuck). Not found yet why those faces get a tiny area.
- A close walking avatar's whole mesh outfit disappears for a frame (seen in three runs): 0 failed palette requests out of 7.4M and 9.1M, so not a missing palette; also seen with impostors off.

## 9. Alpha masked passes served from the table (X3, 2026-10-22)

`PASS_ALPHA_MASK` (the diffuse mask G-buffer pass, `LLDrawPoolAlphaMask`) and `PASS_FULLBRIGHT_ALPHA_MASK` (`LLDrawPoolFullbrightAlphaMask`), static and rigged. The old passes set `minimum_alpha` per draw as a uniform; the table programs read the cutoff from the draw's row (`pk.cutoff`, now written for every entry) through a flat varying (`LL_MASK_CUTOFF` in diffusePullV / alphaPullV, and in diffuseAlphaMaskIndexedF / fullbrightF). Programs: `gDeferredDiffuseAlphaMaskPull(Skin)Program` (diffusePullV + diffuseAlphaMaskIndexedF), `gDeferredFullbrightAlphaMaskPull(Skin)Program` (alphaPullV IS_FULLBRIGHT_PULL + fullbrightF HAS_ALPHA_MASK); the pools fall back to the old draw (with the draw's own cutoff) for what the table cannot carry. London: pool alpha mask old-path draws 247 -> 6 a frame, 186 table commands; fullbright alpha mask 89 -> 38 (texture matrix draws), 54 table commands. One clean run so far (x4a), picture the same (foliage, signs). Type count 17.

## 10. glTF passes served from the table (X4, 2026-10-22)

`PASS_GLTF_PBR`, `PASS_GLTF_PBR_ALPHA_MASK` (the opaque and mask G-buffer passes, `LLDrawPoolGLTFPBR::renderDeferred`) and `PASS_GLTF_GLOW` (the glow in `renderPostDeferred`), static and rigged. The entries are the glTF blend items' (`pbrAlphaEntry`, the four textures, the 40 PBR floats incl. the minimum alpha as `LLFetchedGLTFMaterial::bind` sets it: the material's cutoff for MASK, -1 else), kept in `LLSceneFragment::mGeoPbr` and sent in the retain record's PBR section (the worker's `mPbrOfItem` is by wire item, so it needed no change; the row's `pad.y` is now written for every item that has one, and `geo_draw` sets `ll_packet_pbr`). Programs: `gDeferredPBROpaquePull(Skin)Program` = `pbrAlphaPullV.glsl` + `pbropaqueF.glsl` with the existing LL_BINDLESS_PBR rewrite (its patterns matched without a change); the glow reuses `gPBRGlowPull(Skin)Program`. A glTF draw with a double sided material (the culling changes per draw) or a texture matrix stays on the old path (`pushGLTFBatch` / `pushRiggedGLTFBatch` for the fallback). The `GLTFSceneManager` scene objects are drawn as before. London: pool glTF old-path draws 59 -> 0 a frame, 43 table commands a frame; 1 clean run (x5a). **Not yet seen with real PBR content** (London has few glTF materials): a PBR store check by Adam would settle the look of metal / roughness / emissive. Type count 20.

## 11. Where the GL-shaped stream stands after X1-X4 (run x5a, London, per frame)

Main thread: 7,145 records (was 8,587), 2,297 draws (was 3,124), 824 setBuffer (was 1,702). Worker (different crowd, so only a hint): `draw` 1,914 calls 1.54 ms (was 2,469 / 2.37), operations 9.3 ms of a 15.0 ms frame (s6g: 13.2 of 13.3).

What still draws through the old stream (innermost zone, draws a frame): **lighting volumes** 624 (local light boxes 395, projectors 229), **occlusion cubes** 448 (S7), **UI** about 450 (main panel 270, name tags 56, toolbars), **terrain** 308 (face based: `LLDrawPoolTerrain` draws the faces of its patches, not draw infos, so there are no fragment items to serve), shadows 74, avatar body pools 56, post-deferred geometry 39, fullbright alpha mask 37 / fullbright 27 / glow 19 (texture matrix draws), HUD and the rest small.

**Decided not to serve:** bump and post bump (26 draws): the bump image of a draw is resolved at draw time from its bump code and texture (`bindBumpMap`: `gBumpImageList` creates the images on demand and counts texture stats), so an entry made at group rebuild could name an image that does not exist yet or changes later; legacy feature, small. Stays old until a cheap way to name the image exists.

**Not draw-info passes (not reachable with the fragment table as it is):** terrain, avatar bodies, trees and grass (face based pools), water, sky, UI, HUD, lighting volumes, post processing. They need typed pass records of their own (or stay as the stream's users) in the threading plan.
