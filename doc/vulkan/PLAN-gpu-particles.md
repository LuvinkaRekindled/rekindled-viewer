# Plan: particles on the GPU (2026-10-27) — decided by Adam, not started

Decision: particles move to the GPU. Reasons given: no real downside, sims full of particle spam or decor (torches, waterfalls, fountains, fire) cost the main thread much more than the plaza does, and a vertex-shader billboard from a GPU buffer removes the per-frame quad building and upload as well. Order of work to be picked when the main thread and alpha passes are done.

## What there is now (CPU)

- Simulation: `LLViewerPartSim` steps every source and every particle on the main thread (`idle: updateParticles` 0.19 ms in the plaza, 5-10 times that in a particle-heavy place, to be measured).
- Geometry: `LLVOPartGroup::getGeometry` writes camera-facing quads into vertex buffers when the particle partition rebuilds (about 3.8 groups a call, 23 us a call, 0.09 ms a frame in the plaza).
- Drawing: particle groups are not scene-table fragments; they leave as the transient entries of the alpha pool (`is_particle_or_hud_particle` in `renderAlpha`), 0.08 ms of GPU in the plaza.

## Stages

0. **Census** (optional, for ordering and for the club numbers): particles alive, sources, simulation ms, geometry ms every 600 frames under `LL_VK_ITEMCENSUS`; run once in a particle-heavy place.
1. **GPU quads, CPU simulation.** The simulation stays; its output (position, size, color, texture frame, orientation) goes into a GPU-resident per-group particle buffer (written as rows, delta only), and a vertex shader expands each particle into a quad (6 vertices from `gl_VertexIndex`). Removes quad building, vertex buffer writes and index data; the alpha entry draws N particles instead of a vertex range. Low risk: semantics stay on the CPU.
2. **GPU simulation of the common behaviours.** Source parameters (pattern, burst rate and radius, angles, velocity, acceleration, lifetime, color/scale start-end, glow, texture, flags) in a small table; a compute pass spawns and steps particles in a persistent pool: wind, gravity, interpolation of color and scale, texture animation frame, emissive, follow-velocity, "follow source" (source matrices from the CPU per frame). Ground bounce reads a coarse height texture of the region (or CPU keeps bounce particles).
3. **The rest of the semantics** (target tracking, ribbons, beams, particle sources attached to avatars and linksets, HUD particles), then retire the CPU path behind a switch.
4. **Sorting.** Particles are alpha: back-to-front against the other alpha runs. Options: per group sort on the GPU (bitonic by view depth) and keep the group's place in the alpha order as now, or weighted blended order-independent output for the soft, low-alpha kind (smoke, fire) with a switch. Decide with a picture, not before.

## Things to preserve (what scripters rely on)

`llParticleSystem` flags: interpolate color/scale, emissive, follow source, follow velocity, wind, bounce, target pos / target linear, ribbons; pattern drop / explode / angle / angle cone / cone empty; start and end alpha, glow, burst rate, radius, speed min/max, angles, omega, texture; particle count limits (per source and `RenderMaxPartCount`); particles of avatars too far or hidden are not simulated (`LLViewerPartSim` priority and budget).

## Why not now

About 0.3 ms of main thread in the plaza, GPU draw negligible: the plaza alone does not pay for it. The club and spam sims do; the census there fixes the order of work.

**Update 2026-11-02:** measured again with the census (`mt3-`, `mt4-`, plan 8.35): the particle partition rebuild is about 0.6 ms a frame (about 16 different groups at 40 us, none rebuilt twice) and `updateParticles` 0.36 ms: about 1.0 ms of the main thread in the plaza, not 0.3 ms. Stage 1 alone removes the 0.6 ms.

**Stage 1 and 1b built 2026-11-02 (plan 8.36):** rows for the pull shaders, ribbons included, no face a particle, switch `LL_VK_GPU_PARTICLES`. A group costs 93 -> 20 kcycles on the main thread (about 0.45 ms in the plaza). Stage 2 (the simulation, `updateParticles` 0.36 ms) is next. Visuals not yet looked at by Adam.
