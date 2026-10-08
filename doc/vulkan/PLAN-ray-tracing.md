# Ray tracing and DLSS ray reconstruction: where the native renderer stands (2026-10-08)

Status: assessment, no code yet. Written from what the native-vk work has put in place; nothing here was measured on ray tracing hardware paths (the RTX 5060 Ti supports them).

## What the native path already gives us
- Vulkan only, no OpenGL state to emulate: ray tracing needs device addresses, acceleration structures and descriptor-heavy shaders, none of which fit a GL mirror.
- Bindless textures (4 tables x 8192 slots per frame, update after bind) and `VK_KHR_buffer_device_address` is already enabled for DLSS. A hit shader needs exactly this: any material texture by index.
- DLSS super resolution is integrated with the inputs ray reconstruction also needs: jittered projection, motion vectors (`generateMotionVectors`), depth, history reset rules (and the snapshot case: renders in one frame continue each other's history).
- Draw records carry texture slots and uniform deltas per draw, and shadow packets already turn static, rigged and alpha-masked casters into tables of (vertex address, index range, texture slot, model matrix): the same data a BLAS/TLAS build needs.
- Per-pass and per-zone GPU timing (`LL_VK_PHASES` + `FSGpuZoneTimers`) to judge what a ray traced pass costs.

## What is missing
1. **Stable device buffers.** Vertex and index data live in GL-shaped buffers that are renamed and copied constantly (about 58,000 renames in 5 s while loading). A BLAS needs a buffer that stays put and a known vertex format. This is the "buffer arena / vertex layout ids" item that was measured and left alone for rasterising; ray tracing is the reason to do it. It is the largest piece.
2. **Acceleration structures:** `VK_KHR_acceleration_structure` plus ray query (inline, no pipeline needed to start). BLAS per drawable (rebuild or refit on change; rigged avatars need a refit from the skinned positions each frame), one TLAS per frame from the object list. Start with static geometry and the avatar as one dynamic set.
3. **A material table.** Today materials are uniforms set per draw. A hit needs albedo, normal, metal/rough, emissive and alpha cutoff by instance/primitive id: a GPU buffer of material records (the draw-record plan's uniform ring is the first half of this).
4. **Guide buffers for ray reconstruction.** DLSS-RR wants diffuse albedo, specular albedo, normals, roughness, depth, motion vectors and the specular hit distance, and it replaces the denoiser. The G-buffer has albedo, normal, material and depth; specular albedo, hit distance and a clean roughness are to be added or derived. RR also needs the SDK's ray reconstruction feature (separate from the super resolution one we link; check the SDK version in `llvkdlss.cpp`).
5. **A ray traced lighting pass** to feed it: it must produce noisy radiance (sun shadows, glossy reflections, later diffuse GI from probes + rays). Water reflections are the obvious first customer (the screen-space trace `FSSSRWaterTrace` marks the spot and its limits: off-screen and occluded things).

## Suggested order
1. Buffer arena with stable vertex/index buffers (also pays back in rasteriser worker time; measured at about 0.45 ms vertex input + about 1% rename cost today, so the rasteriser gain alone is small; the reason is ray tracing).
2. Inline ray query shadows for the sun, replacing cascade lookups behind a setting: simplest correct test of BLAS/TLAS, compare with the cascade picture and cost.
3. Material table and ray traced water/glossy reflections, with DLSS-RR fed by the guide buffers.
4. Decide on GI after seeing 2 and 3 on screen and in the profile.

## Risks
- BLAS builds for a world that changes (prims edited, sculpts, rigged avatars, 75-avatar crowds): the build budget per frame is the real limit, not the trace.
- A stale buffer handle is a GPU fault: the arena needs the same discipline as the texture slot epoch.
- DLSS-RR support and API are the SDK's; confirm the SDK version and the hardware requirements before step 3. (DLSS 5 neural rendering is a separate wait item: see the project notes.)
