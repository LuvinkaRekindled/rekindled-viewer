# Retiring the per-view cull and render-map jobs (design note, 2026-10-21, before code)

Context: the agreed order after S4 is "retire the per-view cull / render-map jobs", then S5-S7 (PLAN-gpu-scene.md). S0-S4 are built; the scene table (`LL_VK_SCENE_GEO=2`) with B + C and rigged packets is on by default as of this note (`=0` turns each off). This note is facts first, then options in the order they would be done, then what is NOT reachable before S7. No code yet.

## 1. What runs per view today (read in `pipeline.cpp`, numbers from `pc-ab1` / `pc-ab2`, London, 77 avatars, ms per frame on the main thread)

**Main view.**
- `display: updateCull` 0.86 (`updateCull: partition cull` / `octree: partition cull (render)` 0.9): the octree walk that makes the cull result's visible groups and drawables.
- `display: stateSort (early)` 2.34, of which: `drawable groups (occlusion + rebuild)` 0.57 (occlusion state, `setVisible`, `markVisible` of every drawable, `rebuildMesh`), `visible groups` 0.52 (`stateSort(group)`), `visible bridges` 0.18, `visible drawables` 0.22, `postSort` 1.02 (rebuild of drawable geometry, `postSort: build render map` 0.38-0.45, the rest group rebuilds and priorities).
- The render map is what the pools read. Since S3/S4 the draws the table serves are left out of it per type (`mGeoElided`, `mGeoServedMask`); the loop over the visible groups' draw maps still runs.

**Sun cascades** (`FSParallelShadowPrep`, on in Adam's settings): `parallel sun cascade preparation 1.152 ms/frame on the main thread, 1.8 cascades/frame`. Phases: `cull` 0.498 (wait for the pool job `shadowPrepCull`: partition cull per cascade, `setVisible` / `markVisible` / `changeLOD` of what is inside), `render map` 0.564 (wait for `shadowPrepRenderMap`: every draw info of every visible group of the cascade into per-pass lists, alpha groups, and, in resident mode, `mSceneGroups` = the groups without a current resident fragment), plus the static / skin / alpha builders and `renderShadow` itself.
- In the ray-only shadow modes (1, 4, 5, Adam's default) the cascade maps are never read (`rt_sun_cascades_unused`): the passes only name casters to the ray scene, nothing is drawn.

**What already moved off these jobs.**
- S2: resident fragments are culled on the GPU for non-ray cascades, and in ray mode the ray scene's static membership comes from residency (`res_ray_membership`: 4,443 of 5,407 fragments in the scene in the London run), independent of any cascade.
- S3 / S4: the main view's deferred static, moving and rigged draws come from the table.

So the cascade jobs' remaining work in ray mode is whatever the ray scene still gets by cascade: the groups that have no resident fragment (about 18 % of fragments, bridges, avatars, attachments, alpha / masked and anything the fragments do not take), plus their pools.

## 2. What is NOT yet verified (so the first step is a census, not code)

I have not yet counted, per frame in ray mode: how many groups / draw infos the cascade jobs produce, how many of them end up as ray-scene notes that residency does not already give, and per pass type. I also have not listed every reader of the main view's render map after S3 / S4 (alpha pools, fullbright, glow, terrain, avatar pools, impostors, the velocity pass and the ray naming read it or the served-items helper `collect_main_view_draw_infos`). Without those two lists any "skip the job" change risks a missing caster that no verify mode would catch.

## 3. Options, in the order they would be done

**R0. Census (no behaviour change, `LL_VK_VIEWCENSUS=1`).** Per frame, in ray mode: cascade jobs' groups / draw infos by pass type; how many are in groups with a current resident fragment (redundant), how many reach the ray scene as notes; main view render map entries by pool and which pool reads them. Gate: the two lists exist and the redundant share is known. Cost: about a day of small code, log only.

**R1. One caster cull instead of four cascade cull + render-map jobs, for the groups residency does not hold (ray-only modes).** The ray scene needs a caster set, not four shadow maps: cull once against the union of the cascade frustums (or the ray scene's range sphere), skip every group with a current resident fragment, and name the rest. Keeps `setVisible` / `markVisible` / `changeLOD` for the drawables inside (that state is read elsewhere, see R3). Expected: most of the 1.15 ms (0.5 cull + 0.56 render map waits) minus a smaller single job, honest range 0.6-0.9 ms. Risk: a caster that only a particular cascade would have named (far cascades are reused / rate limited today: `CascadeReuse`, `rt_far_interval`). Gate: ray scene content (instances, groups, casters per frame) equal to the old path within noise, shadow pictures equal, verify mode 0 uncovered, `shadow prep` zones.

**R2. Main view render map without the served draws' work.** `postSort` builds the map by walking every visible group's draw map and skipping served types; make the served groups cost nothing (the group's served mask decides before the draw map is touched), and build the map only for the pools that still read it (from R0's list). Expected 0.2-0.3 ms. Gate: pictures equal, `postSort: build render map` zone, `LL_VK_SCENE_GEO_VERIFY` 0 mismatches.

**R3 and R4 stay where they are until S7:** `stateSort (early)` 2.3 ms and `updateCull` 0.86 ms are not render lists, they are visibility, occlusion, LOD and per-drawable state (`markVisible`, `changeLOD`, `rebuildMesh`, `setVisible`) that the animation and update code reads. Replacing them needs GPU group visibility, GPU LOD selection and Hi-Z occlusion (S7), plus a way to feed visible-drawable state back to the CPU. They cannot be retired by removing a list.

## 4. Honest expectation

R1 + R2 together are about 0.8-1.2 ms of the 11.8 ms `display()`, not the "-3 ms" the plan listed for S2 (that figure assumed the visibility work went with it). The rest (about 3.2 ms in `stateSort (early)` and `updateCull`) is S7's.

## 5. Switches and order

`LL_VK_VIEWJOBS` (default off until Adam has used it): bit 1 = R1, bit 2 = R2; `=0` always the old path. `LL_VK_VIEWJOBS_AB=<s>` alternates on / off for the usual `ab_zones.py` runs. Order R0 -> R1 -> R2, no reordering without a stated dependency.

## 6. Questions for Adam

1. R0 census first (my recommendation, it decides whether R1 is safe), or straight to R1?
2. Ray-only modes first (default), non-ray cascades later?
3. OK that R3 / R4 are S7 and this step is about 1 ms, not 3?
