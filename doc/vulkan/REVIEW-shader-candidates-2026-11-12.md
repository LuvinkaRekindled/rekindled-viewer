# Shader candidates: the code review (stage 4 part 2, 2026-11-12)

Input: the 113 programs that no lap bound and that `inventory-shaders.py` found a use site for (`INVENTORY-shaders.md`), plus the 85 loop-built names. This note says what each use site is. Method: for each program's C++ global, every reference was sorted into setup (names, files, `createShader`, `add_common_permutations`, `make_rigged_variant`), registration (`mShaderList.push_back`, the list that gets the environment uniforms), the pull twin source list (`make_pull_twins`), and real use in other files; real uses were then followed to their function and its callers.

## What the first look got wrong
- A program can be drawn through its pull twin: the draw code calls `ll_pull_or(&gX)`, which binds `X pull` when the twin is ready. Then `X` itself is never bound, yet it is live: it is the source the twin copies its files and features from (`make_pull_twins` needs `src.mProgramObject != 0`). 12 of the 113 are in this state (their "pull" twin was bound in a lap): Avatar Velocity (body), Deferred Avatar Shader, Deferred Avatar Alpha Shader, Deferred Avatar Shadow Shader, Deferred Avatar Alpha Shadow Shader, Deferred Avatar Alpha Mask Shadow Shader, Deferred Diffuse Non-Indexed Alpha Mask No Color Shader, Deferred Star Program, Deferred Windlight Cloud / Moon / Sky Program, Draw Color Shader. Keep.
- Vignette Post and Snapshot Frame Post (Firestorm features) were first listed as dead by a filter bug; they stay.

## Group A: nothing in the code can bind them (13 programs, 22 names with their skinned variants)
Only setup, `mShaderList` registration and the rigged variant helper name them. Not drawn through a twin (no twin entry), not named in any other file. Candidates for deletion, each with its skinned variant:
Deferred Avatar Eyes Shader; Deferred Diffuse Alpha Mask Shader (+ Skinned); Deferred Diffuse Non-Indexed Alpha Mask Shader; Deferred Diffuse Shader (+ Skinned); Deferred Emissive Shader (+ Skinned); Deferred Fullbright Alpha Masking Alpha Shader (+ Skinned); Deferred Fullbright Alpha Masking Shader (+ Skinned); Deferred Fullbright Shader (+ Skinned); Deferred GLTF Shadow Alpha Mask Shader (+ Skinned); HUD Fullbright Alpha Masking Alpha Shader; HUD PBR Opaque Shader; Hero Radiance Gen Shader; Skinned PBR Glow Shader (the unskinned PBR Glow shader is a separate global and stays).
Before deleting: for each, which stock files only it uses (a file shared with a live program stays), the `mShaderList` lines, the skinned variant globals, and a check that no pull program is created from its files (the dedicated pull programs name their own files).

## Group B: only twin sources (7)
Deferred Bump, Deferred GLTF Shadow Alpha Blend, Deferred PBR Opaque, Deferred Shadow Alpha Mask, Deferred Shadow Fullbright Alpha Mask, HUD Fullbright Alpha Masking, HUD Fullbright. Their twins were never bound in a lap, but the twin exists only if the source loads. Part of the stock carrier work, not of this deletion.

## Group C: real use sites (64): live code that no lap exercises, or reached through the pull twin
- Post processing and settings in `pipeline.cpp` (29 incl. the ray reconstruction and debug passes): CAS and legacy gamma, depth of field (CoF, DoFCombine, Post), tonemap family, gamma, buffer visualisation, deferred light and spot light, avatar velocity debug, DLSS-NR rim lift, RR guides and reflection motion, ray material debug, snapshot frame, vignette, pathfinding, reflection probe display, highlights. Live features behind settings.
- Pools (`lldrawpool*.cpp`): reached by `LLPipeline::renderGeomDeferred`, which the main view still calls (`llviewerdisplay.cpp:1380`, `:1635`) and the impostor and preview renders call (`pipeline.cpp:30448`). The avatar, WL sky, tree, terrain, bump, water pools bind their programs through `ll_pull_or`, so the stock program stays unbound while its twin draws. Not dead.
- Other files: Benchmark (`llglsandbox.cpp`), RLVa sphere (`rlveffects.cpp`), object and physics preview (`llmodelpreview.cpp`, `llfloaterimagepreview.cpp`), the glTF preview (`llgltfmaterialpreviewmgr.cpp`), Debug / Skinned Debug (48 sites in many files). Live.
- Open: the old-path pool functions that nothing calls once the table draws (`beginBump(true)` and `beginFullbrightShiny(true)` in `lldrawpoolbump.cpp` run only when the table is not available; `LLDrawPoolTerrain::renderFullShader`, `hilightParcelOwners`, `LLDrawPoolAvatar::beginSkinned` / `beginRigid` / `beginImpostor` on the non-deferred render path). Whether those paths can still run needs the pool code read, not the shaders: a separate step after the carrier work, because deleting them is deleting draw code.

## Loop-built families (85 names)
Chosen at run time by a selector: material variants by flag index (Material Shader 0-31, Material Pull Shader / Pull Skin Shader), multilight counts, FXAA / SMAA quality levels, terrain, alpha / pull / skin twins. A member that no scene ever selects is only found by reading the selector, not by a census; none is touched yet.

## Result
- Delete now (needs Adam's yes): group A.
- Keep for the carrier work: group B and the 12 twin sources.
- Keep: group C, with the old-path pool functions listed as a later look.
