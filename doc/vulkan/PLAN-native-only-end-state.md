# Native only: the end state, and the list that gets us there

Written 2026-11-13 after Adam's note: "the plan was started to convert all shaders to native and get rid of old/dead shaders, yet it doesn't seem to be set up to do either unless I keep pushing".
The cause: every plan so far (twin removal, draw info removal, shader families) was scoped to one mechanism, was called done when that mechanism was done, and left the rest as "next" items that
needed a new instruction. This document is the one list. It is finished when `python <work>\native-inventory.py` exits 0, and each step below is worked down in order without asking.
The only things that stop the work are a look decision that is Adam's (a program whose picture would change) and a failed check.

## The end state (what `native-inventory.py` counts)

- A: no program of `llviewershadermgr.cpp` loads a stock file (`mShaderFiles.push_back`): every program is a line of `native/manifest.txt`, or is deleted because nothing binds it.
- A2: the loader attaches no library file by name (`shaders.push_back(make_pair("x.glsl", ...))`, 34 files on 2026-11-13).
- B: the buffer-attribute draw paths of the old pools are gone (`pushFaceGeometry`, `renderIndexed`, `pushBufferVerts`, the `postSort` lists): every draw is a record of table entries.
- C: no stock shader file under `app_settings/shaders/class1..3` is left that no program names (the stock directories themselves are deleted when empty).

## State (the inventory script prints it; first count 2026-11-13: A 28, later in the day 13)

- A: stock programs left (13): Avatar Shader, Deferred Avatar Alpha Shader pull (stock files for the `rt` sessions), Deferred RT Reflection Shader, Deferred Sun Shader (stock vertex, the RT sun fragment), GLTF PBR Metallic
  Roughness, Impostor, No color alpha mask, Object Preview, Pathfinding x2, Preview Physics, Ray Scene Material Debug, Reflection Probe Display. Done today (step 2): CoF, DoFCombine, Deferred Post (DoF), Buffer
  Visualization, Avatar Velocity Debug, Vignette, Snapshot Frame, CAS x2 (cas.frag generated from the stock file by <work>\make_cas.py; the CPU half of that file is a C++ header now, `ffxcas.h`), Gamma x2,
  RLVa Sphere, DLSS-NR Rim Lift, RR Guides, RR Reflection Motion.
- A2: one stock file line with a computed name (llviewershadermgr.cpp: the sun program's fragment choice). The loops of the glow and pull programs had two dead stock-file parameters: gone.
- A3 DONE (2026-11-13): the 17 `rt` lines (lit alpha programs, the PBR alpha, the material blends, the avatar alpha) are `rtquery` lines now: a session with ray queries builds them with LL_RT_QUERY (the loader adds the
  extension and the draw binds set 3), and the sun shadow of a transparent surface is a ray (lib/shadow_alpha.glsl: `sun_shadow_alpha`, the stock text of lib/rt.glsl; the uniforms the alpha pass sets are the members of
  lib/rt_uniforms.inc in alpha_pass.inc). Their stock files and the dead old alpha programs (Deferred Alpha, Skinned, HUD, the impostor pair, the HUD PBR alpha: bound by no code) are deleted. Sizes of the RT variants:
  alpha lit 24,064 SPIR-V words, PBR alpha 33,398, avatar alpha 23,894 (non-RT: 13,812 as before). Checked: matrix 0 failures (with the ray query profile), RT lap and RT-off lap, no errors, ground 140 / 142. Not checked
  by eye: the hair and glass shadows in the traced sessions against the stock look (Adam).
- The sun lightmap and the material debug view DONE (same day): "Deferred Sun Shader" is one native only line (`sun_lightmap.frag`, option rtquery): a session with ray queries traces the sun ray (rt_sun_shadow) and writes the
  blocker distance to attachment 1 (frag_blocker, LL_RT_QUERY), a session without ambient occlusion (no global HAS_SSAO) builds it as LL_NO_AO; the traced mode (LL_RT_SHADOWS 1, 4 x-ray view, 5 swung sun) is a global define of
  the native programs of such a session and lib/rt.glsl's `rt_sun_shadow` has the two test views of the stock wrapper. "Ray Scene Material Debug" is `rt_material_debug.frag` (generated from the stock text by
  <work>\make_material_debug.py, the line defines LL_RT_QUERY). Checked: RT laps (s71 sun 16,683 words, s72 material debug 10,101 words: no errors, ground 141 / 142), RT-off laps s73 (another avatar stood in the view:
  a ground region differed by 10 levels, the tree shadows the same) and s74 (the same view as before: ground regions within 2 levels, means equal, 141.5). Not checked by eye: the traced sun shadow's penumbra against the stock.
- The material preview's PBR alpha program DONE (alpha_pbr_preview.vert + alpha_pbr.frag, PBR_PREVIEW) and the RT Reflection Shader DONE: `native/rt_reflection.frag` is the stock text of class3 rtReflectionF and its fourteen library
  files (the stock loader's attach order) as one unit, made by `<work>\freeze_stock.py rt_reflection` (the `#if`s of the text stay, the native defines decide; the declarations part, the loose blocks and the sampler macros, is
  the stock translator's own output for the program, dumped with the temporary switch LL_VK_FREEZE_DIR; the uniform / in / out declarations of the files are removed; duplicate constants and the probes block are fixed up), the
  vertex stage is fullscreen.vert, the manifest line defines HAS_SUN_SHADOW, LL_RT_REFLECTIONS, LL_RT_QUERY. The native programs now also get the LL_RT_* tuning defines and SPOT_SHADOW of the environment (what the stock
  library files saw) and RT_REFL_DEBUG (LL_VK_RTREFL_DEBUG). 58,063 SPIR-V words, its pipeline pre-warms in about 12 s. Checked: all 18 setting variants compile offline, RT lap s78 (no errors, ground 142). Not checked by eye:
  the traced glossy reflection itself (Adam: FSVkMirrorQuality, a glossy surface).
- The glTF scene programs DONE (2026-11-13): `native/gltf_scene.vert` and `gltf_scene.frag` (generated by `<work>\freeze_gltf.py` from the stock text of class1/gltf/pbrmetallicroughnessV / F and the stock library files, the `#if`s
  stay) are the 16 variants of "GLTF PBR Metallic Roughness Shader" (manifest lines named by variant: `blend`, `rigged`, `unlit`, `multiuv`, defines ALPHA_BLEND, HAS_SKIN, UNLIT, MULTI_UV; the blended ones are rtquery lines);
  the sizes of their blocks (MAX_NODES_PER_GLTF_OBJECT, MAX_MATERIALS_PER_GLTF_OBJECT, MAX_UBO_VEC4S) are global defines of the native programs from the device. Checked: all 16 variants compile offline in a plain, an RT and a
  no-shadow session (and in the matrix), laps s80 (RT, GLTFEnabled forced: 16 programs, the blended ones 42,856 SPIR-V words) and s81 (RT off: 33,132): no errors, ground 142 / 143, ground regions within 2 levels of the
  baseline. Not checked: a glTF scene drawn (needs a region with GLTFEnabled).
- The old stock loader is GONE (2026-11-13, the end of A): `LLShaderProgram::createShader` / `link` build native programs only (a program without a native source is a visible warning and a failed program: the stock path, `mShaderFiles`,
  `attachObject` and friends are deleted); llshadermgr.cpp lost `attachShaderFeatures`, the pull rewriting and `loadShaderFile` (1,100 lines) and the dead GL shader binary cache (`initShaderCache`, the settings
  RenderShaderCacheEnabled / Version and the menu toggle); the stock translator llvkshader.cpp (GL GLSL to Vulkan GLSL, 1,670 lines) is deleted, llvkshader.h keeps the program info types and `debugShadersEnabled`;
  `loadBasicShaders` loads no library files; the directories class1, class2, class3 and the error fallbacks errorF / errorV are deleted (the frozen units, `freeze_stock.py` / `freeze_gltf.py`, were made from them: the generated
  files are the programs now). Three groups of stock programs the inventory had MISSED (it parsed `x.mName` and `x->mName`, not array elements or computed names) were found by the first lap after the sweep (the viewer
  stopped loading shaders at the first failed program and crashed in LLDrawPoolSimple::renderDeferred, ASSERT mProgramObject != 0): the old non-pull Material Shader programs (dead: the materials pool bound them only where
  the records were not available; the pool does not any more, `gDeferredMaterialProgram` is gone), SMAA (12 programs: `smaa_edge / smaa_weights / smaa_blend` with `lib/smaa.glsl`, the stock library text, one manifest line
  for each pass and preset) and the PBR terrain paint bake (`terrain_bake.vert / .frag`, one line for each bit depth of the paint map: the loader names it by the octal of the depth, `Terrain Bake Shader RGB10` is 8 bits).
  Checked: build read, RT lap s84 (GLTFEnabled forced: 216 programs built, the lap before 204 + the 12 SMAA ones), RT-off lap s85 (196): no errors; the ground was in the darker, plaza-lights-off state in both (Adam: no glare
  on the ground = lights off; settled, do not chase). Not checked by eye: SMAA (RenderFSAAType 2), the terrain bake (LocalTerrainPaintEnabled), the materials passes of non-default views.
- B: DONE (4c62282013): `pushFaceGeometry` deleted; `renderIndexed`, `pushBufferVerts` and `postSort` are live primitives of non-scene draws, not an old path. 102 no-op stock permutation statements removed from the mgr.
- C: DONE (errorF / errorV deleted with the loader).
- State 2026-11-13: `native-inventory.py` exits 0. Laps s86: 216 programs (RT + GLTF), 196 (RT off), no errors. Open by choice: the LLDrawInfo rename (Adam, DESIGN-draw-info 6.9), stale comments that mention the stock loader, the three non-literal `addPermutation` / `mDefines` copies in the mgr.

## Order

1. Commit the water work (RT water in the native pull programs, the stock Water Shader / Underwater Shader / Water Haze Shader removed, the haze under water as a table entry, the water pool's dead walk).
2. The screen passes (17 programs, one family `post`, `fullscreen.vert`, one fragment file each, close translation of the stock text): each gets its manifest line, a picture check where it can be run (CAS, DoF, gamma, vignette,
   snapshot frame are in the views driver), and its stock files deleted in the same commit. Never-bound ones are deleted, not ported (the census says which).
3. Avatar Shader, Impostor, GLTF PBR Metallic Roughness, No color alpha mask, Deferred Sun Shader, RT Reflection, Avatar Alpha pull (its stock files: the loader still reads them for the `rt` sessions).
4. The interface programs: Pathfinding x2, Object Preview, Preview Physics, Reflection Probe Display (debug / preview views: converted, never dropped).
5. B: `renderIndexed` and `pushFaceGeometry` callers (wlsky, terrain, face), `pushBufferVerts` (the debug view), `postSort`; then draw info Stage 3 (the rename is Adam's decision, section 6.9 of DESIGN-draw-info).
6. A2 and C: the library files the loader attaches, the stock directories.

## Checks

After each step: `python <work>\native-inventory.py` (the numbers must go down), `native-check-all.py --matrix` (0 failures), a build with the result read, a lap (RT off and RT on) with the ground check, the census of
unbound programs. A step that fails one of these is fixed before the next starts. Each step is its own commit.

## Not on this list (separate plans)

GPU lifetimes, the repo move, the beta; the GPU particles; the OpenAL work.

## Palette copies (2026-11-13, dfda24a1c7)
The own-palette mode of the rigged shadow records, the rigged alpha shadow records and the ray scene notes is deleted (palettes are ids in the frame's list only; `LL_VK_PALETTE_SHARED` and `ll_palette_shared` are gone). Laps s88: 216 / 196 programs, no errors. Then also deleted: the rigged G-buffer handler's `own_palettes` branch (8e96dcc8a2), and the avatar velocity record's copy (kind 8 names this frame's and last frame's palettes by id at the worker) with the switches LL_VK_VELIDS and LL_VK_PALID (palette ids are the only mode). Laps s89, s90: 216 / 196 programs, no errors. Left with its own palette on purpose: the RT body mesh record (kind 4, `llviewerjointmesh.cpp`: the base mesh's 45 joint palette is not a skin palette). Left as switches: LL_VK_GEOPERSIST=0 (every binding every frame), LL_VK_PALARENA=0 / 2, LL_VK_RIGREBASE=0 / 2, LL_VK_VELJOB=2 compare.
