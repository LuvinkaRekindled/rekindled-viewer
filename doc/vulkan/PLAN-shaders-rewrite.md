# PLAN-shaders-rewrite: the shader set from scratch (started 2026-11-08, branch `shaders-from-scratch`)

Adam's direction (2026-11-08): pretend the current shaders cannot be used at all. We know what the picture must be; build the shaders that make it from scratch, for the same quality or better at less cost. The old shaders are a REFERENCE only: how they worked around fringe cases, and how user settings and variables steer them. None of their code is reused (no copy, no port line by line).

## 0. The fork and the way back

- Branch `shaders-from-scratch`, cut from `native-vk` at c758140c3d (the shader pass research commit). `git switch native-vk` is the way back, always.
- Inside the branch, every new family sits next to the old one behind a runtime switch (rt-debug.txt key per family, default old until accepted), so one lap can A/B old against new in alternating windows (`<work>\keycycle.ps1`) and Adam can flip while looking. A family is accepted when the numbers and Adam's eye say so; only then is the old family deleted on this branch (one commit per family, easy to revert).
- Merge back to `native-vk` only when Adam says so.

## 1. What exists today (inventory, 2026-11-08)

250 `.glsl` files (class1 216, class2 10, class3 19), 134 programs registered in `llviewershadermgr.cpp`. By family (programs, GPU ms in the RT-off crowd lap `gz9`, 6.4 ms GPU frame):

| family | programs (examples) | GPU ms (crowd, RT off) |
|---|---|---|
| lighting resolve | sunLight, softenLight (41 KB frag), hazeF, blurLight, point/spot lights, SSAO inside | sun 0.31 + soften 0.29 + local lights 0.03 |
| sky / atmosphere | skyV/F, cloudsV/F, atmospherics*, sun/moon/stars | 0.58 (atmospherics 0.23, sky haze 0.17, clouds 0.12, haze 0.06) |
| alpha pass | alphaPullV (15 KB), alphaF (19 KB), pbrAlpha*, fringe | 0.78 (rigged 0.56, static 0.15, fringe 0.07) |
| G-buffer fill | diffusePull, materialPull, pbropaque, pbrterrain, terrain, avatar, tree, bump, emissive, fullbright, impostor | 0.59 + terrain 0.07 |
| shadows | shadowPull, shadowAlphaPull, skin, tree, avatar, gltf | 0.78 (see PLAN-shaders.md section 5) |
| velocity / motion | avatarVelocity*, objectVelocityPull, motionVectors | 0.19 + |
| post | exposure, luminance, glow, postDeferred*, dof, tonemap (6 variants), CAS (157 KB frag), gamma, vignette | renderFinalize 0.11, glow 0.03, cas 0.03, tonemap 0.03, rt mips 0.05, ... |
| external | DLSS | 1.29 (NVIDIA, not ours) |
| water | water*, underwater, waterHaze | water pass 0.08 + 0.04 |
| UI / debug / misc | interface (43 files), highlight, debug, rlv, previews, pathfinding | UI 0.07 |
| RT | rtShadowPrep/Filter, rr*, nr*, ray query lighting (own track, see section 6) | 0 in this lap |

Variants that multiply programs today: HUD copies (gHUD*), skinned copies, indexed/non-indexed, alpha mask / no color, 6 tonemap x gamma combinations, impostor, cube shadow. A from-scratch set must reach these through specialization constants and one contract, not by program copies.

## 2. What the new set is built on (principles)

1. One shader contract, Vulkan native: GLSL 450+, no GL-era uniform names, no `mirror_flag` / `clipPlane` / `clipSign` leftovers (a clip plane only where a pass really needs one, as `gl_ClipDistance`), no loose uniforms translated into a block behind our back. Per-frame data in one UBO (camera, previous camera, sun/moon/sky/atmosphere, exposure, render size, frame index), per-pass data in push constants, textures through the bindless table, per-draw data through the existing buffer-reference tables.
2. Few wide programs, specialization constants for settings (quality levels, features on/off, HUD, rigged, alpha mode): the driver compiles away what a setting turns off; no runtime branches on settings in a hot path, no program copies.
3. Fewer passes where the work allows: sky + haze + atmospherics can resolve in the lighting pass instead of three more full-screen passes; the post chain (exposure, glow, DoF, tonemap, sharpen, gamma) is a few compute dispatches reading and writing once; low-frequency terms (clouds, AO, haze) at the resolution they need, upsampled with a depth-aware filter that is checked against full resolution.
4. Precision and bandwidth are part of the design: packed G-buffer fields, 16 bit where the picture cannot show it, no re-derivation of what the G-buffer or the table already holds.
5. Same or better picture is a MEASURED claim: each family ships with an A/B compare (section 3) and is judged on the numbers and on Adam's eye in hard cases.
6. Written from the physical intent, not from the old code: the BRDF, the atmosphere model, the shadow filtering and the tone curve are specified in the family's note first (what the picture must show, which ranges, which edge cases), the shader follows the note. Old shaders are read afterwards, for the fringe cases and the settings they honour, and each found case is added to the note as a test.

## 3. How we know the picture is the same (or better)

- A compare mode per family: the pass runs old and new into separate targets for the same frame, a small compare shader writes max/mean error and PSNR to the log (and a false-colour debug view: stripes/split, as agreed for debug views), at fixed still-camera spots (the crowd spot, a sky-heavy spot, an indoor / dark spot, water, a night scene, a HUD scene).
- Gates before a family is accepted: (a) error numbers inside an agreed bound at every test spot (a family with a deliberate improvement lists it and is judged by eye instead), (b) zone-timer ms lower in the alternating-window lap, (c) no new warnings, (d) Adam's look at his own spots and settings.
- Settings matrix per family (all user settings that steer it, each tested on and off / at its levels): listed in the family note while reading the old shader for what it honours.

## 4. Order of attack

Slice 0, the contract and the tools (no picture change): the native shader directory and loader, the per-frame UBO, push-constant helpers, specialization constants plumbing, the runtime family switch, the A/B compare tool, shader hot-reload so a shader-only edit needs no C++ build, and one trivial program end to end (a copy / tonemap-free blit) to prove it.

Then family by family, biggest GPU prize and cleanest boundary first:

1. Lighting resolve + sky (sun, soften, haze, atmospherics, sky, clouds, local lights): about 1.2 ms, self-contained (reads the G-buffer, depth, shadow maps, probes; writes HDR colour). First, because it is the largest block with a clean input/output contract and the one where merging passes pays.
2. Alpha pass (0.78 ms): the second largest, own uber shaders today; rigged and static together.
3. G-buffer fill + velocity (0.8 ms): this is where the G-buffer layout could change (section 5, decision 2), so it comes after the lighting resolve defines what it needs to read.
4. Post chain (finalize, glow, DoF, tonemap, sharpen).
5. Shadow casters (small, last of the big ones; the structure is already table driven).
6. Water, terrain details, UI/debug: only if the numbers justify; interface shaders are cheap and stay until everything else is done.
7. RT shaders: their own track (section 6).

Each family: note (intent + fringe cases + settings) -> shader -> compare mode -> alternating-window lap -> Adam's look -> accept -> delete the old family.

## 5. Decisions for Adam (before slice 0 starts)

1. First family: lighting resolve + sky (recommended), or the alpha pass first?
2. G-buffer layout: keep today's layout until the G-buffer family comes up (recommended: the lighting resolve reads what exists), or design the new layout first and move everything together (cleaner, but nothing can be A/B'd until all consumers exist, and RT / DLSS inputs read it too)?
3. The legacy non-deferred path (class1 forward lighting shaders, ALM off): drop it in the new set? (Rekindled only runs deferred; nothing here needs the forward path.) Recommended: drop.
4. Mirrors / planar reflection / water refraction passes (they need clip planes today): keep supporting them with a proper clip-distance path in the contract?
5. Non-NVIDIA GPUs: the new set uses only portable Vulkan features (no NVIDIA-only paths), so AMD/Intel get the same shaders. Agreed?

## 6. RT track

The ray-query lighting pipeline compile time fix is deferred until just before release or the shader updates (memory note). The from-scratch work is the natural moment: the new lighting resolve is written with ray-query inputs in mind from the start (specialization constant on/off), and the RT shaders get their rewrite after the raster families, using the same contract.

## 7. Risks

- Size: 134 programs. Mitigated by the order above (cost-ranked) and by dropping the program copies, the forward path and the dead GL-era variants; the interface and debug shaders are not part of the cost story.
- Hidden settings and fringe cases: the old shaders hold years of workarounds. Mitigated by the "read after writing" rule and the settings matrix; each found case becomes a test spot.
- Two worlds in one branch while families are mixed: the switch per family keeps every build runnable and A/B-able.
- Build speed: shaders are text compiled at run time; hot-reload (slice 0) keeps iteration fast.

## 8. G-buffer alternatives and the overshading measurement (2026-11-08)

Today: albedo RGBA8, ORM/specular RGBA8, normal RGBA16 (octahedral + env intensity + flag), emissive RGBA16F only with RenderEnableEmissiveBuffer (default off), depth shared with the HDR screen. About 20-24 B a pixel written once and read by lighting.

Alternatives: (A) slim packing: normal RGB10A2, flags folded in, 12-16 B a pixel; (B) visibility buffer: the geometry pass writes instance + triangle id + barycentrics (about 8 B), one compute resolve shades each pixel once from the row tables, buffer-reference vertices, bindless textures (the architecture we already have); (C) fullbright / emissive-only surfaces write HDR colour directly (skip the G-buffer write and the lighting read; "post pool fullbright" already exists); (D) material id instead of ORM (only with B); (E) depth pre-pass then G-buffer at equal depth (stopgap, doubles vertex work); (F) forward/clustered (ruled out: Second Life overdraw, no cheap SSAO/SSR/DLSS inputs).

Measurement (LL_VK_FRAGSTATS, lap os1/os2 `phasesoffstats`, RT off, crowd spot, still camera; fragment shader invocations by zone, per pass occurrence, divided by the 1713x898 = 1.54 Mpx target): the opaque G-buffer segments together run about **3.2 fragment invocations per screen pixel** (scene table type 0 1.14, type 2 0.38, type 17 0.59 + rigged 0.59, terrain 0.40, the small rigged types about 0.1). That counts hidden-surface fragments (the unsorted opaque draws do not get all the early-Z help) and the helper lanes of partly covered 2x2 quads; the split between the two is not measured, and screen coverage by opaque surface (sky pixels excluded) is not either, so the ratio per covered pixel is probably somewhat higher still.
Other numbers from the same lap: the rigged opaque types draw hundreds of thousands of triangles for tens of thousands of fragments (type 10 rigged: 234k triangles, 14k fragments a frame): the distant crowd is vertex bound, not pixel bound; the cascade shadow depth passes run 1.2-1.5 M vertices each with 0 fragments (simple) and the static alpha caster passes shade 1.0-6.5 M fragments.

Reading: the material shading in the G-buffer runs about 3x as often as a single-shading scheme would. What that is worth in ms: the opaque G-buffer fill is 0.59 ms + terrain 0.07 ms in the zone lap, and only the pixel part of it is exposed to the 3x, so a visibility buffer or a depth pre-pass can win at best a few tenths of a ms (3-6% of the 6.4 ms GPU frame), at the price of the largest rewrite in the plan, and it adds the resolve's own cost. The crowd itself (rigged, sub-pixel triangles) is a vertex-side problem that a V-buffer does not change. So: not a first-slice item. Keep today's layout for the lighting resolve and alpha families, take C (fullbright straight to colour) and A (slim packing) when the G-buffer family comes up, and decide B with a pre-pass experiment then (draw the opaque types depth-only first, re-measure invocations and ms).

## 9. Keep list: what the picture must not lose (Adam, 2026-11-08)

Adam (new to this territory, so every step is explained plainly): "as long as we don't lose any of the current things, like in the plaza the ground has a shine so all lights show pretty brightly on it". This is a standing rule for every family: before a family is rewritten, its keep list is written from the current picture, each item has a test spot, and the A/B compare covers it.

Keep list so far (grows while reading the old shaders and while Adam plays):
1. Plaza ground shine: the shiny floor shows local lights (and the sun) brightly in its specular highlights; the highlights' strength and spread under point and spot lights match today. Test spot: the plaza, night and day, lights on.
2. Reflections on shiny surfaces (probe, SSR and RT sources) keep their HDR brightness: bright things reflect bright.
3. Fullbright and glow surfaces look as they do now (haze applied, SSR/probe/RT see them in reflections); emissive stronger than 1.0 may only get BETTER, as an approved change.
4. (to be filled per family while reading: water, hair/alpha fringes, avatar skin, atmospherics at dusk, clouds, HUD, mirrors)

Reading of the G-buffer question (this turn): option A (slimmer normal packing) and C (fullbright straight to colour, optional and last) do not touch how floors and reflections get their HDR; that comes from the lighting resolve (probes, SSR, RT radiance into the 16 bit float screen). The G-buffer's only HDR attachment is emissive RGBA16F, off by default (RenderEnableEmissiveBuffer): with it off glTF emissive is added to the 8 bit albedo and clamps at 1.0. A cheap HDR emissive field (R11G11B10F / RGB9E5) is a quality candidate for the G-buffer family, to be approved by Adam as a visible improvement.

## 10. Decisions taken (Adam, 2026-11-08)

1. First family: lighting resolve + sky (then the alpha pass, G-buffer, post, shadow casters, RT).
2. Today's G-buffer layout stays until the G-buffer family; options A (slimmer packing), HDR emissive field and C (fullbright to colour, optional, last) are decided then; the visibility buffer only after a pre-pass experiment.
3. The legacy non-deferred forward shaders (class1/lighting, RenderDeferred is already forced true) are dropped. The alpha pass stays forward-lit by nature and is rewritten, not dropped.
4. Mirrors / water refraction keep working through a proper clip-distance path in the contract.
5. Portable Vulkan only (AMD / Intel get the same shaders).
6. New shared piece: one clustered light list per frame (lights binned into screen/depth cells) read by BOTH the lighting resolve and the alpha pass, so opaque and transparent surfaces are lit the same way (plaza lights on the shiny ground stay as bright as today: keep list item 1).
7. Keep-list rule (section 9) applies to every family.

8. G-buffer family, emissive (approved in principle): a compact HDR emissive field (R11G11B10F, 4 B a pixel, always on) replaces the optional RGBA16F attachment. A strength setting 0..1 (default 0 = exactly today's look: emissive clamps at white; 1 = full HDR) and an optional soft shoulder above 1.0; at 0 the old/new compare must show zero error on emissive surfaces. Emissive stays out of the auto-exposure metering (or is capped) so a few bright signs cannot dim the scene, and prim glow (the bloom feed) keeps behaving exactly as today. Adam approves the shipped default after a before/after look.

## 11. Scope rule: speed first, the look stays (Adam, 2026-11-08, at the go for slice 0)

"Everyone has spent a lot of years building worlds in the old viewers and we'll eventually do some fancier shaders, but for now we'll try to keep the changes as minimal as possible unless we find some really cheap quality improvements. Main goal is speed and we can try some new things later on."
So: the new shaders reproduce today's picture (creators' worlds look as they were built); the only exceptions are really cheap quality improvements, each shown to Adam first. This overrules section 10 item 8: the HDR emissive field and its strength control are NOT part of the first pass; they go on the "later, fancier shaders" list. Speed is the measure of every family.

## 12. The list is not 1:1, and programs stay small (Adam, 2026-11-08, during slice 0)

"The shader list doesn't have to be 1:1 either. If we find better ways to split them up and re-use along with others, or go the opposite way and combine them where we can, that's great as well. Ideally we'll keep the sizes small so we don't have a giant shadow shader like we do now that takes a long time to compile."
Rules that follow:
- Programs are cut by what is cheap and clear, not by the old file names: split one old program into several small ones when its parts run in different passes or at different costs, merge several old ones (or whole passes) into one when they read and write the same pixels.
- Shared logic lives in small include modules under `native/lib/` (the BRDF terms, the atmosphere terms, the shadow lookup, the light list reads, octahedral packing), each kept short; a program is a short main plus the modules it needs. No program grows into an uber shader again.
- Switches that change the work a lot (hair/alpha modes, rigged or not, shadow quality levels) are separate small programs chosen by the C++ side, not branches inside one; cheap toggles may use specialization constants.
- The loader logs the compile time and the SPIR-V size of every native program and warns above a budget (first budget: 20 k SPIR-V words, 150 ms compile); the budget is part of a family's acceptance. The SPIR-V cache and the background pipeline build stay on.

## 13. Slice 0 result (2026-11-08)

Built and verified: the native loader (glslang, own includer, reflection into the renderer's ProgramInfo), the manifest and the per-family live key, the swap at the draw with the old program's uniform values and sampler units copied by name (callers unchanged), specialization constants (implemented, not exercised), hot reload from the source directory (tested: edit and revert both rebuilt in about 1 ms after the first compile), size/compile-time budget logging, screenshot compare tool and the old/new lap driver, first program (`Copy Shader`) matching the old one by eye and frame time. How to use it: NATIVE-SHADERS.md.
Changed from the plan: the per-frame data block is NOT in slice 0 (it needs main-thread values through the bridge; slice 1 says what the lighting resolve needs and builds it then), and the in-frame GPU compare of a full target comes with the first family that writes one.
