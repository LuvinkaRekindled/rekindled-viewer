# The traced reflection as its own pass (plan, 2026-11-11)

Adam, 2026-11-11, waiting at the login screen for the first start of a session: "it's taking a while ... because of this shader loading. we need to work on that time. it's very long ... right now there's too much in that one shader."
This plan takes the ray traced reflection out of the lighting shader. It replaces the "RT compile can wait" note (`project-rt-compile-deferred`): Adam has asked for it.

## What is wrong (read from the code and the logs of 2026-11-11)

- **One pipeline holds the start-up.** The log of Adam's start: `login waits for 1 of 383 GPU pipelines to be built` (STATE_AGENTS_WAIT, `llstartup.cpp`), still waiting after 2.5 minutes. The one pipeline is the *Deferred Soften Shader* with `LL_RT_REFLECTIONS`: about 176 s cold (notes of 2026-11-07).
- **It is built whenever RT is configured, not when the mirror is on.** `rt_reflections_possible()` (`llviewershadermgr.cpp:1288`) is true when the environment has `LL_VK_RT_SHADOWS` and `LL_VK_RT_MATERIALS=1`; `loadShaders` then adds the permutation to the soften program (`:3651`) and to the water program (`:1330`). `FSVkGlossyMirror` only picks the runtime value of `rt_glossy_reflections`. So everyone who starts with RT materials pays the compile, mirror or not.
- **Cold = any change.** A shader edit, a GPU driver change or a different variant (the glossy mirror on, the quality level) is a new pipeline. The pipeline cache (117 MB, `Renderer::init`) is written at a normal exit only.
- **What is inside `softenLightF.glsl` (975 lines).**

  | lines | what |
  |---|---|
  | 1-108 | the lighting pass proper: G-buffer decode, atmospherics, probes, sun, SSAO |
  | 109-153 | declarations of the traced reflection: uniforms (`rt_glossy_reflections`, `rt_hit_quality`, 8 hit lights, a projector), the ray query helpers from `shadowUtil.glsl` |
  | 154-585 | **the hit shading, ~430 lines**: `rtLightOnHit`, `rtHitLocalLights` (up to 8 lights, each with `rtShadowRayArea` = ray queries in loops), the projector, `rtHitForwardLights`, `rtLitHitColor` (probes sampled AT the hit, glTF and legacy lighting), `rtReflectionLit` (trace with layers for blended surfaces, the quality modes, the fade) |
  | 587-653 | two test helpers (`rtHitLightsOnly`, `rtHitProjectorInternals`) |
  | 655-975 | `main`; the traced reflection is three call sites (PBR path 737-754, legacy path 799-814, a test at 875) and ~90 lines of bisect probes (`rt_glossy_reflections` 2-15) at 882-963 |

  Everything that costs compile time is in 109-653, and `main` uses it only as `radiance = mix(radiance, rt.rgb, rt.a)` for pixels whose gloss is at least 0.9 (0.0 in the test modes). It is already a function with a clean boundary: `vec4 rtReflectionLit(origin, dir, light_dir_n, sunlit, amblit, tc, fallback)`.
- Also in the soften pass: the alpha channel carries the traced hit distance for DLSS ray reconstruction (`rr_hit_distance`, `extractRRHitDistance`, `pipeline.cpp:23181`), so a separate pass must keep that.

## Goal

The lighting pass compiles in seconds with or without RT, and the start-up no longer waits for any pipeline that only the traced reflection needs. The traced reflection looks and costs the same as today, builds in the background, and until it is ready the glossy surfaces show the probe / hero probe result (a miss) instead of the world going dark.

## Design

A new fullscreen pass, **RT Reflection**, before the lighting pass:
- **Native, written for Vulkan** (native dir, own manifest family `rtrefl`), line for line from `rtReflectionLit` and what it calls (the "translate, no changes" rule); no old GLSL variant is kept (one path).
- **Inputs** (all exist at that point of the frame): depth, normal, specular / ORM of the G-buffer, the lightmap (sun shadow), the ray scene and its material table, the probe maps (for the hit lighting and for the fallback), the environment uniforms, the hit light arrays and the projector texture that `pipeline.cpp:26592-26700` fills today.
- **Output**: `RGBA16F` reflection (rgb = colour, a = how much replaces the probe result) plus the hit distance (a second attachment, replacing the alpha trick and `extractRRHitDistance`).
- The pass computes **its own fallback** with the probe library (the pixel's `radiance` / `glossenv` that `main` passes in as `fallback`): it only runs for the glossy pixels, so the probe lookup is paid there only, and the result is exactly what the inline code produces (the fade to the fallback at 250-1200 m and the "blended surface only" mix both use it).
- **The lighting pass** (`resolve.frag`, the native resolve that already stands in for the non-RT variants) gets two texture reads where it has the call sites. The old RT variants of the soften program (the manifest says they "stay old") and the permutation `LL_RT_REFLECTIONS` in `llviewershadermgr.cpp` are deleted.
- While the pass's pipeline is not built the pass is skipped (a draw whose pipeline is missing is dropped today) and the reflection texture is cleared to a = 0: the surfaces keep the probe result. **The login hold for that pipeline goes**; the progress text stays for any pipeline the world really needs.
- Water keeps its own flat `rtReflection` variant for now (it has the same permutation: step 6).

## Steps (each one verifiable; laps with the screenshot, the glass grab and the faces sheet)

0. **Baseline.** Log the compile time of every pipeline the pre-warm builds (the `pre-warm pipeline built` log exists for slow ones) and of the soften variants; confirm 176 s is the soften variant alone and nothing else is above a few seconds; record the cold start time (a changed built copy of the shader, as in `project-startup-pipeline-wait`). Read the mirror at the new quiet spot for the reference picture (the shell, the hair layers, the room).
1. **Carve out, nothing changes.** Move 109-653 and the three call sites into the new native pass, same code, same uniforms; `main` keeps the call sites as texture reads. Compare mode `LL_VK_RTREFL=2`: both run, count the pixels that differ (the old inline path stays only for this step), expect 0. Hit distance for RR from the new attachment. Laps: RT on, glossy mirror 1-4 qualities, RR on and off, the debug views of the hit quality (3, 4, 6-9, 11-18) as views of the new pass.
2. **Delete the old path.** Soften loses `SOFTEN_RT_REFLECTION`, the permutation and the old RT variants go; the native resolve covers every variant; `extractRRHitDistance` goes. Cold start measured again: expect the lighting pipelines in seconds.
3. **No login hold for the pass.** The reflection pass is optional: skipped until built, probe fallback meanwhile (a screenshot of a start with the pass held back by a switch). `LL_VK_STARTUP_PIPELINE_WAIT` stays for pipelines the world needs.
4. **The debug probes.** The plumbing probes (`rt_glossy_reflections` 2, 5, 6, 9-15) were bisect scaffolding: delete them; keep the hit views (3, 4, 7, 8) and the hit quality views as debug views of the new pass (the rule: our debug features are converted, not dropped).
5. **Smaller compile, cheaper frame.** (a) Split trace and shade if the new pass is still slow to compile: one pass traces and writes the hit record (position, normal, material id, layer), a second shades it; no shader then holds more than one ray query loop. (b) Only the glossy pixels: a classification pass writes a compact list (gloss at or above the threshold) and the trace runs over the list (a compute shader, workgroups of 64), so the cost follows the glossy area, not the screen. (c) Run it on the async compute path when `PLAN-async-compute.md` E1 is in (the pass has no raster dependency).
6. **Water.** The same carve for the water program's flat `rtReflection` permutation (`:1330`).
7. **This is also the traced mirror rewrite.** The new native pass is where the traced mirror is written from scratch (Adam: "write new native shaders for legacy and RT mirrors from scratch"): blended layers (`layer_alpha`: hair, and the pink translucent shell in the mirror), the quality levels, the hit lights. `PLAN-glossy-mirror-quality.md` steps A and B are what moves; its open points (hair layers in reflections, flat normals of avatars) are checked on the new pass.

## Risks and things to keep

- **DLSS ray reconstruction** takes the hit distance (alpha of the soften output today). The new attachment must reach `rrHitDistF` / the RR guides the same way; check with RR on.
- **Cube snapshots** (hero and reflection probe faces) do not trace (`rt_hit_quality >= 2 && !gCubeSnapshot` in `pipeline.cpp`): the faces stay as they are.
- **classic_mode**, the sun / moon light direction, the forward alpha pass's own RT lighting: unchanged inputs, checked in the compare lap.
- **The ray scene is not ready** in some frames (first frames, a teleport): the pass must clear its target, not leave stale values.
- **Cost**: a second fullscreen pass reads the G-buffer again: expect +0.1 ms for the extra reads, minus whatever 5(b) saves; judge by GPU ms at the quiet spot (not fps).
- **A cold start still exists** for the new pass, off the critical path. The pipeline cache must be written at exit; a crash at start-up loses it (as on 2026-11-11: two crashes, one reset of the graphics settings).

## Not in this plan

The start-up wait itself (it stays for what the world needs), the pre-warm list (`vk_pipeline_variants_default.txt`: regenerate after step 2 with `LL_VK_VARIANTS_EXPORT`, it needs the CMake list and `viewer_manifest.py`), and the other ray query programs (sun lightmap, alpha pull RAYALPHA): they are separate pipelines; step 0 says whether any of them matters.

## Status 2026-11-11 evening (uncommitted)

**Step 0 (baseline), measured.** A start with nothing changed has no pipeline wait: 66-72 s for a whole lap, no pipeline over 1 s, the driver cache unchanged (NVIDIA keeps its own on-disk shader cache next to ours: with `LL_VK_PIPELINE_CACHE=0` and `__GL_SHADER_DISK_CACHE=0` the start was just as fast). The slow compile happens only when the shader code really changes (any file the soften shader includes, e.g. `reflectionProbeF.glsl`): then the Soften RT variant took 157-176 s. `LL_VK_PIPEDIAG=1` (llvkrenderer.cpp `createPipelineObject`) logs every pipeline over 1 s with hashes of the SPIR-V, spec constants and key and the driver cache size before and after.

**Step 1 (carve out) done in stock GLSL, not native (decision: the compile time was the goal; the native rewrite of the traced mirror stays step 7).** `class3/deferred/rtReflectionF.glsl` is lines 1-653 of the old soften shader plus a new `main` (G-buffer decode, the probe fallback, `rtReflectionLit` for the glossy pixels, the same conditions); program `Deferred RT Reflection Shader` (gDeferredRtReflectionProgram, same libraries as soften, created when `rt_reflections_possible()`). Output target (pipeline.cpp `rtReflectionTarget`): RGBA16F rgb = reflection, a = weight (-1 traced and missed, 0 not traced) + R16F hit distance (replaces mRRHitDist, `extractRRHitDistance` and the Ray Reconstruction Hit Distance program / `rrHitDistF.glsl`). The pass runs before the lighting pass in renderDeferredLighting (hit lights and projector uniforms moved with it); soften reads `rtReflMap` (reserved uniform RT_REFL_MAP) when `rt_glossy_reflections != 0`. Dropped: the hitq=10 eye ray test.
Measured cold: the lighting pass compiles in under 1 s; the new pass 69 s; the start-up did not wait for it (it is requested at its first draw, not pre-warmed: the login hold is gone as a side effect; until it is built the surfaces show the probe / planar result).
Not done: native resolve covering the RT variants (step 2: add `ll_ti_rtReflMap` to resolve_pass.inc, delete `LL_RT_REFLECTIONS` from soften), the plumbing probes (step 4), split trace / shade (5a) to bring the 69 s down, the RR hit distance check with RR on, a same-pose picture comparison with the old inline path (the code moved verbatim; hair / shell look as before, see below).

**Hair / shell in the traced mirror (Adam: "no tinting").** hitq=11 (hit albedo) shows pale hair with a pink streak, no pink shell: the pink tint of the real avatar is the translucent rigged glTF blend shell, which the ray scene does not treat as a blended layer (`rtRecordBlended` only takes legacy blends: `mf & 2 == 0`) and the own avatar's big kind 0 draws enter the scene as opaque or not at all. That is step 7 of this plan, not something the carve changed.

**Step 2 done (same evening, uncommitted).** The old Soften program has no ray query code any more: its `LL_RT_REFLECTIONS` permutation is gone, the bisect probes (rt_glossy_reflections 2, 5, 6, 9-15) are gone (views 3, 4, 7, 8 stay, converted: green hit / red miss / colour found / surface colour), and `mFeatures.hasShadows = false` for it (shadowUtil.glsl, which mentions `ll_rt_tlas`, made `mUsesRt` true and the native stand-in refused the program: the cause that kept RT-on runs on the old lighting program; check `mUsesRt` = "the preprocessed source mentions ll_rt_tlas", llvkshader.cpp:719). The native resolve (`resolve.frag` / `resolve_pass.inc`) reads `ll_ti_rtReflMap` and the loose uniform `rt_glossy_reflections` and now stands in for RT-on runs too (compiled in 9 ms, drew every frame in the lap). Laps: RT on + glossy mirror: traced mirror as before, no pipeline over 1 s (the pass was cached); RT off: planar mirror with the pink hair, 194 fps in the grab. Not done: the pre-warm variants list (`vk_pipeline_variants_default.txt`) was not regenerated; RR with the hit distance not checked; same-pose numeric compare not made.
