# Design: native programs without a stock twin (stage 4, written 2026-11-12, for Adam's review)

Nothing in this note is built yet. It is the design for removing the stock GLSL programs that stand behind the native programs ("stock carriers"), so that a native program is the program, not a replacement drawn in place of another one.

## 1. What a stock program with a native override does today

A program named in `native/manifest.txt` is still created from its stock GLSL files at shader load (glslang to SPIR-V, one program each). At every draw, `nativeOverride(Program*)` (`llvknative.cpp`) looks the program up by name and, if a manifest line fits, swaps in the native `Program` and copies the old program's uniform values into it (`Program::adoptFrom`, by name, plus the sampler units). The old program keeps these jobs:

1. **Uniform carrier.** The C++ callers set uniforms on the old `LLShaderProgram` through the reserved-uniform tables (`mUniform[]`, built by `mapUniforms()` from the stock program's reflection). `adoptFrom` copies those bytes into the native program on every draw (per-draw cost on the worker).
2. **Texture channel and attribute map.** `mTexture[]` / `mActiveTextureChannels` (what `enableTexture(DIFFUSE_MAP)` returns) and `mAttributeMask` come from the stock program's reflection.
3. **Variant key.** Manifest lines pick a native variant from the old program: `require <uniform>` (the old program must have that loose uniform) and `when <uniform>=<value>` (its value at the draw). 22 of the 115 lines use them.
4. **Definition source.** `make_pull_twins` copies a source program's `mShaderFiles`, `mFeatures`, `mDefines` into its pull twin (and needs `src.mProgramObject != 0`); `make_rigged_variant` and the glTF variants are made from a stock program the same way.
5. **Fallback.** Family switched off (`native_<family>=0`), native build failed or not ready, native output count different, or a ray traced variant (`mUsesRt`): the old program draws.
6. **Cold start.** Every one of them is compiled at load, whether or not it ever draws.

## 2. What was measured today (RT-off and RT-on laps, 301 program names)

- 115 manifest lines for 105 programs (8 programs have variant lines: Deferred Soften 4 by `ssrTraceMode`, the four alpha pull programs and the Fullbright Alpha Mask Pull by `classic_mode`, Deferred Avatar Shader pull, Reflection Mip). Families: gbuffer 65, post 25, alpha 18, resolve 4, copy 1, blur 1, sun 1. Options: `require` 27, `when` 12, `define` 240.
- Bound in a lap: 122 programs, 67 of them with an exact manifest line (83% of all binds: 4.12 of 4.96 million). 55 bound programs have no manifest line (not ported yet): the shadow programs (about 7 of the busiest by binds, 17% of binds together), the fullbright/shiny/glow pull programs, the light, water and post programs, the avatar velocity programs; ordered by binds in `INVENTORY-shaders.md`, section "port next".
- Fallbacks (`NativeCensus`, `LL_VK_PROGRAM_CENSUS=1`): none of family off, condition not fit, native not ready or output mismatch. The only fallback is the ray traced variant: 894,418 draws on 23 programs in the RT-on lap. So for every program the laps reached, the stock program is only the carrier (1, 2), the variant key (3), the definition source (4) and, in RT mode, the RT variant.
- Two programs already have no stock twin: Native Sun Lightmap Half and Native Light Upsample set `mNativeVertex` / `mNativeFragment` / `mNativeDefines` and no `mShaderFiles`; `LLShaderProgram::createShader` builds their `Program` from the native source (`compileNativeProgram`, `llshaderprogram.cpp`), `mapUniforms()` reflects it, and the callers drive them with the same API as the stock sun program (`pipeline.cpp`, `sun_shader.uniform...`). That is the mechanism to extend.

## 3. Design

**One rule: a program that has a native source is built from it; the stock GLSL is not compiled for it.**

3.1 **Built from the manifest, at load.** `LLShaderProgram::createShader` asks the native loader for the manifest line of `mName` (the first line whose conditions can be decided at load). If there is one, it takes the native path that Native Sun Half already uses (`compileNativeProgram` with the line's files and defines, plus the viewer's global defines that the loader adds today). The stock `mShaderFiles` in the manager's setup block are then not read; they are deleted block by block, with the stock files, as each family is converted. No hand edit of 105 setup blocks is needed to start: the loader makes the switch, the cleanup follows.

3.2 **Interface comes from the native program.** `mapUniforms()`, `mTexture[]`, `mActiveTextureChannels` and `mAttributeMask` are reflected from the native `Program`. Names a caller sets that the native program does not declare resolve to -1 and are ignored, which is what `adoptFrom` already does today (it copies only names the native program has). The sampler names are the `ll_ti_<name>` members, as for Native Sun Half.

3.3 **No copy for a program with one native line.** The 97 programs with a single line draw with their own `Program`; `adoptFrom` is not called for them. It stays only for the variant programs (3.4) until they are reworked, so the per-draw copy cost goes away for most draws.

3.4 **Variants.** The 13 extra lines are variants of the same program chosen by a uniform value at the draw (`classic_mode`, `ssrTraceMode`) or by uniform presence (`require`). Variants chosen by value stay as they are (the primary native program owns the interface, the other lines are siblings that read its values through `adoptFrom`); that is correct and small, and can be replaced later by a specialisation constant if the measured cost asks for it. `require` lines (27) existed to tell two stock programs of the same name apart; each must be read when its family is converted and replaced by an explicit name or define.

3.5 **Pull twins and rigged variants.** `make_pull_twins` copies the source program's definition; with a native source it copies `mNativeVertex/Fragment/Defines` and takes the twin's own manifest line (`X pull` already has one: 27 pull targets). The twin still needs its source to be loaded (`mProgramObject != 0`): that test becomes "the source is valid". Rigged variants and glTF variants of a converted program are converted in the same step; the manifest has one `Skinned` line only, so the others are still stock and are listed per family.

3.6 **Ray traced variants stay stock, compiled only when needed.** `mUsesRt` programs (23) are compiled from the stock GLSL because the RT track has no native port yet. They stay: but they are compiled only when ray tracing is on (the non-RT cold start no longer pays for them), and they are deleted with the RT track's port. Until then `nativeOverride` keeps returning the old program for them.

3.7 **Fallback.** No stock program means no fallback. A native compile failure at load is a warning and the program is invalid (its draws are skipped and logged once with the program name, the way the viewer treats a stock shader that fails to compile). The risk is covered before a family is converted by `native-check-all.py` (every line, plain and with the global defines) and in a lap by the `NativeCensus` line (`not_ready` must stay 0). The `native_<family>=0` debug switch ends for a converted family (one path after the move: a temporary switch, then delete it); comparison runs use the exe of the commit before.

3.8 **Hot reload and settings changes.** The loader already rebuilds a native program when its sources change or when the viewer's global defines change (SSR, probes, terrain). For a program built at load this is the shader reload that a graphics setting change already triggers (`setShaders`), plus the file watch for development (`LL_VK_NATIVE_DIR`).

## 4. Order of work (each step: build, `native-check-all.py`, both census laps with a zero `not_ready`, picture check on the family, commit; stock files of the family deleted in the same step)

0. **Interface diff tool** (no behaviour change). A script that, for every manifest program, compares the uniform and sampler names and attribute set of the stock program with the native one (the stock side from `LL_VK_PROGRAM_MAP`, the native side logged at build), and lists every name a caller sets that the native program does not have and every name the native program has that no caller sets. This is the check for the one thing 3.2 could change: a caller that branches on `mUniform[X] != -1`.
1. **Pilot: `copy` and `blur`** (2 programs, one line each): the loader switch (3.1), no `adoptFrom`, stock files deleted. Proves 3.1-3.3.
2. **`post`** (25 programs, full screen passes).
3. **`resolve`** (4 lines): the Soften variants by `ssrTraceMode` test 3.4.
4. **`alpha`** (18 lines): the `classic_mode` variants and the `require shadow_matrix` lines.
5. **`gbuffer`** (65 lines, pull twins, rigged and glTF variants): the largest step, split by pool.
6. **The 55 bound programs without a manifest line**: ported straight to native-only (no stock twin), busiest first (the shadow programs).
7. **RT variants**: with the RT track's port; then the last stock GLSL and the stock shader preprocessor can go.

## 5. What it removes

Per converted family: the stock GLSL files and the manager setup blocks (about 12 to 25 lines per program), the per-draw `adoptFrom` copy, the compile of the stock program at load (glslang runs once per program today; the load time to be measured before step 1 and after each step), the family switch. At the end: `nativeOverride`'s swap for single-line programs, `make_pull_twins`'s copy of stock definitions, and `LLShaderMgr::attachShaderFeatures` for converted programs.

## 6. Decisions for Adam

1. Is losing the stock fallback acceptable for a converted family (3.7)? The alternative, keeping stock compiled lazily only after a native failure, keeps all the stock files and most of the code.
2. Ray traced variants stay stock and compiled only with ray tracing on (3.6): fine until the RT track port?
3. Start with step 0 and the pilot, or port the busiest unported programs (step 6, the shadow programs) first? The note recommends 0 then 1: a converted family should not grow while the mechanism is still being proved.
4. Variants chosen by value (3.4) stay as `adoptFrom` siblings for now: fine?
