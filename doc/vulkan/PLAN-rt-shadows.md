# Step 2 of PLAN-ray-tracing.md: inline ray query sun shadows (started 2026-10-10)

Goal: `sampleDirectionalShadow` (shadowUtil.glsl, used by sunLightF, the alpha and material shaders, water, forward PBR) can be answered by a ray query against a TLAS instead of the four cascade lookups, behind a switch, with a way to compare the two pictures and the cost.

## Where the instances come from
The shadow pass packet lambdas in `pipeline.cpp` (`render_shadow_packets`, `render_alpha_packets`, `render_skin_packets`) already name, for every caster draw of every cascade: vertex buffer, index buffer, index range and model matrix. That is the caster list of the sun cascades, including casters outside the camera frustum, so the coverage is exactly the coverage of the cascades today (no new culling, no new viewer walk). The worker handlers (`on_shadow_packets`) note every entry into `LLVk::Rt`. Entries get a stable matrix id (the address of the `LLDrawInfo::mModelMatrix`), so a moving linkset keeps its structure and only its instance transform changes.

## Structures
- **BLAS key = (vertex buffer name, index buffer name, matrix id, kind)**; kind 0 = opaque casters, kind 1 = alpha masked/blended casters (own structure so the instance mask separates them; stage A traces only kind 0, stage B alpha-tests kind 1). One geometry per distinct drawn index range (union since the last rewrite of the buffer), `ALLOW_UPDATE`, refit when the buffer's version moves, rebuilt when the range set grows or the epoch changes. Positions are the vec4 array at offset 0 (stride 16), 16 bit indices.
- **Storage** from a new arena kind (`ArenaKind::Accel`, large device blocks with `ACCELERATION_STRUCTURE_STORAGE` usage): no `VkBuffer` per structure.
- **Build scheduler**: per scene call, structures that are new or stale are sorted by distance of their instance from the camera and built until a triangle budget and a count budget are used up (`LL_VK_RT_BUILD_TRIS`, `LL_VK_RT_BUILD_MAX`); the rest wait for the next frame (shadows of far things appear a few frames late while loading).
- **TLAS per frame slot**, built in eye space (instance transform = view * model) right before the deferred lighting pass, so shaders trace in the space they already work in (`pos`, `sun_dir`). An empty TLAS is bound when nothing was built.
- **Descriptor**: set 3, binding 0 = acceleration structure; only programs whose source mentions `ll_rt_tlas` declare and use it (`ProgramInfo::mUsesRt`).

## Switch and comparison
`LL_VK_RT_SHADOWS=1` (replace the cascades), `2` (split screen: left half cascades, right half ray query), `3` (difference: red = ray says lit and cascades say shadow, blue = the opposite, otherwise the cascade result). Compile-time define `LL_RT_SHADOWS` in the shader programs, so a program without it carries no ray query code. Needs `LL_VK_RT=1` (device features) which the launcher sets. A viewer setting follows once the picture is accepted.

## Stages
- **A (this commit series)**: opaque casters, TLAS, ray query shadows, compare modes, log line with counts/ms. Known and expected differences: no shadows from avatars/rigged attachments, none from alpha masked casters (trees, fences, hair cards) in modes 1-3.
- **B**: alpha test in the query loop (candidate triangles of kind 1: interpolate texcoord, sample the bindless table, confirm) via a per-geometry record table (address of vertices/indices/texcoords, texture slot, cutoff), then the material table needed by step 3.
- **C**: compute skinning pass for rigged avatars and attachments (skinned positions to an arena range per instance every frame, BLAS refit), also feeds motion vectors later.
- **D**: soft shadows (sun disc cone jitter + DLSS accumulation), cost and picture review against the cascades, build block placement, compaction of static structures.

## Gates
A: the three modes run without validation messages; mode 3 shows differences only where expected (avatars, alpha masked); cost of the TLAS/BLAS block and of the lighting pass reported (GPU zones).
