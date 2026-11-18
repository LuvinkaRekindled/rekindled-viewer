# The RT Reflection pass: native, and split if it still compiles slowly (2026-11-12)

Adam, 2026-11-12: "for the 30 s shader that's still GLSL, let's make it native and do any optimizations that you think would be good without hurting quality. we might want to break that one down more into separate shaders if the load is still ~30 s".
The pass is `class3/deferred/rtReflectionF.glsl` (program "Deferred RT Reflection Shader", `gDeferredRtReflectionProgram`, llviewershadermgr.cpp), carved out of the soften shader on 2026-11-11 (PLAN-rt-reflection-pass.md). It is stock GLSL, linked with the stock libraries its `mFeatures` pull in.

## What the pass is made of (read 2026-11-12)
- `rtReflectionF.glsl` itself (about 770 lines): the lights at a hit (`rtHitLocalLights`, `rtLightOnHit`, `rtHitForwardLights`), the hit lighting `rtLitHitColor`, `rtReflectionLit` (trace, up to two blended layers, composite, fade), `main`.
- `class1/deferred/shadowUtil.glsl` (1500 lines, shared with the sun shadow and alpha programs): the ray query side. `rtAlphaHit` / `rtAlphaCover` (masked candidates), `rtShadowRay*` (five `rayQueryEXT` sites: sun, local lights soft/area), `rtShadeSurfaceEx` (decode a hit: record, indices, uv, textures, normal map, material), `rtTraceHitEx` (the reflection ray with blended layers), `rtReflection` (the flat level 0).
- the lighting libraries (probes at the hit `sampleProbes` / `sampleProbeAmbient`, `pbrBaseLight`, atmosphere, G-buffer unpack, `tapHeroProbe`).

## Where the native path stands (read in rkvknative.cpp / rkvkshader.cpp)
- Stock stages that mention `rk_rt_tlas` get the ray query extension and the set 3 declarations (scene, geometry records `RKRtGeom`, materials `RKRtMaterial`, pins, `rk_rt_tex`) from the loader (rkvkshader.cpp ~1066 and ~1098) and `ProgramInfo::mUsesRt` (the draw binds set 3, rkvkrenderer.cpp ~4071).
- Native programs (compileNativeProgram) get neither: no ray query extension, no set 3 text, `mUsesRt` stays false; and `nativeOverride` returns the old program for any program with `mUsesRt` ("the ray traced variants are the RT track", rkvknative.cpp ~705). So there is no native ray query shader yet: this pass is the first one, and its library (`native/lib/rt.glsl`) will also serve a native RT sun shadow later.
- A native pass stands in by manifest line; the old program's uniform values are copied by name into the native one, so the C++ callers do not change.

## Order of work
0. **Quick win first (done, uncommitted):** the test views of the hit (hitq 3, 4, 6, 7-15, 17, 18, 22-24) each called the hit function again: five inlined copies of the biggest function in the pass. They are compiled only with `RT_REFL_DEBUG` (environment `RK_VK_RTREFL_DEBUG=1`, permutation added in llviewershadermgr.cpp). The pass the viewer draws with has one call site. Measure the cold compile (PIPEDIAG) against the 29-35 s before.
1. Loader: native programs that mention `rk_rt_tlas` get the ray query extension, `mUsesRt`, and `nativeOverride` lets a flagged manifest entry through (`rt` marker).
2. `native/lib/rt.glsl`: the set 3 declarations as text (same layouts as the loader's), `rtAlphaHit`, `rtShadowRay`, `rtShadeSurfaceEx`, `rtTraceHitEx` translated line for line (explicit `textureLod`, the rule of feedback-native-shaders-explicit-lod).
3. `native/rt_reflection.frag` (+ `fullscreen.vert`), manifest family `rtrefl`, pass block members named like the stock uniforms.
4. Compare old and new (same frame, pixels that differ), compile time (PIPEDIAG), GPU time.
5. If the compile is still long: split. Candidate cut (to be checked against the numbers of step 0/4): trace (ray query loops and the hit descriptor) | decode and light (material fetch, probes, lights with their shadow rays); the intermediate target must be small (a few uint per glossy pixel, and the pass must not clear a full size target for the few pixels that trace).

## Rules in force
Line for line first (Adam: no changes), optimizations only where the picture does not change; `python <work>\native-check.py` before every native copy; the hot reload dir; check the exact shader variant that runs (RT-on uses native resolve for the lighting, this pass for the trace).

## Measured 2026-11-12
- Gating the hit test views (step 0): the pass's pipeline cold 28.9 s -> 12.4 s (PIPEDIAG, same machine, the next cold run).
- Cold start, driver cache bypassed with `RK_VK_COLD_SALT=<n>` (raises the SPIR-V id bound by n: the module's bytes change, its behaviour does not; NVIDIA's on disk cache sits outside the program's directories and `__GL_SHADER_DISK_CACHE=0` does not switch it off for Vulkan here; renaming its folders was refused by Windows: in use): only four pipelines over 1 s: RT Reflection 12.0-13.9 s, the two Alpha Uber Material Pull shaders 1.6-2.2 s, Water 1.3-1.6 s.
- Fully cold (also `RK_VK_SPIRV_CACHE=0`, `RK_VK_PIPELINE_CACHE=0`): login screen 17 s (9 s with the SPIR-V cache), world started 36 s, the RT Reflection pass ready 50 s after launch (it starts at the first draw when the SPIR-V is compiled late). The login does not wait for it (`is_optional_program`). Before the carve-out the lighting pipeline with ray tracing took 157-176 s cold and the login waited for it.
- Native conversion is therefore not needed for compile time; it stays the plan for the traced mirror's own rewrite (steps 1-5 above) and for a native RT sun shadow.
