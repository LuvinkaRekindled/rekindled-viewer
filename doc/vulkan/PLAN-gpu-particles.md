# Plan: particles on the GPU (2026-10-27) — decided by Adam; stages 1, 1b, 2 and the GPU evaluate-and-sort are built and on by default (status 2026-11-07 below)

## Status (2026-11-07, checked against the git history and the code; Adam asked whether this was still "not started")
**Built, committed and on by default:** stage 1 and 1b (rows for the pull shaders, `RK_VK_GPU_PARTICLES`), stage 2 step 1 (the closed form, compare lap), step 2.1 (records, `RK_VK_GPU_PARTICLE_SIM`, default on since 2026-11-02), step 3 (follow source, target linear, target position and bounce as record laws: the beach goes from 1,650-2,000 stepped particles a frame to 14), G0 and G1 of `PLAN-gpu-particles-gpu-sort.md` (the sort pass evaluates the records once and writes the sorted rows; `RK_VK_GPU_PARTICLE_SORT`, default on; the vertex-shader evaluation of G0 and `RK_VK_GPU_PARTICLE_EVAL` were deleted: "step 2.2" below is superseded by it) and C5 J2 (a group keeps runs of rows, `RKParticleRun`, instead of draw infos).
**Not on the GPU, by design (gpu-sort plan section 4):** wind particles, particles of callback sources (spiral, beam, chat patterns) and ribbons stay stepped on the CPU and are drawn as stage 1 rows: 14 a frame at the beach, a few hundred in the plaza where ribbons are 17% of the stepped particles. What would move them: ribbons as records that name their parent's record (the original stage 2 step 4), wind from a per-region wind field read by the evaluation, callbacks stay CPU (they run script-driven code). Not started; the plaza number is the one that says whether ribbons are worth it.
**Stale in the text below:** the title says "not started", the "Why not now" and the stage 1 paragraphs predate the built stages, and the last paragraph's "Next: step 2.2" is superseded.

Decision: particles move to the GPU. Reasons given: no real downside, sims full of particle spam or decor (torches, waterfalls, fountains, fire) cost the main thread much more than the plaza does, and a vertex-shader billboard from a GPU buffer removes the per-frame quad building and upload as well. Order of work to be picked when the main thread and alpha passes are done.

## What there is now (CPU)

- Simulation: `LLViewerPartSim` steps every source and every particle on the main thread (`idle: updateParticles` 0.19 ms in the plaza, 5-10 times that in a particle-heavy place, to be measured).
- Geometry: `LLVOPartGroup::getGeometry` writes camera-facing quads into vertex buffers when the particle partition rebuilds (about 3.8 groups a call, 23 us a call, 0.09 ms a frame in the plaza).
- Drawing: particle groups are not scene-table fragments; they leave as the transient entries of the alpha pool (`is_particle_or_hud_particle` in `renderAlpha`), 0.08 ms of GPU in the plaza.

## Stages

0. **Census** (optional, for ordering and for the club numbers): particles alive, sources, simulation ms, geometry ms every 600 frames under `RK_VK_ITEMCENSUS`; run once in a particle-heavy place.
1. **GPU quads, CPU simulation.** The simulation stays; its output (position, size, color, texture frame, orientation) goes into a GPU-resident per-group particle buffer (written as rows, delta only), and a vertex shader expands each particle into a quad (6 vertices from `gl_VertexIndex`). Removes quad building, vertex buffer writes and index data; the alpha entry draws N particles instead of a vertex range. Low risk: semantics stay on the CPU.
2. **GPU simulation of the common behaviours.** Source parameters (pattern, burst rate and radius, angles, velocity, acceleration, lifetime, color/scale start-end, glow, texture, flags) in a small table; a compute pass spawns and steps particles in a persistent pool: wind, gravity, interpolation of color and scale, texture animation frame, emissive, follow-velocity, "follow source" (source matrices from the CPU per frame). Ground bounce reads a coarse height texture of the region (or CPU keeps bounce particles).
3. **The rest of the semantics** (target tracking, ribbons, beams, particle sources attached to avatars and linksets, HUD particles), then retire the CPU path behind a switch.
4. **Sorting.** Particles are alpha: back-to-front against the other alpha runs. Options: per group sort on the GPU (bitonic by view depth) and keep the group's place in the alpha order as now, or weighted blended order-independent output for the soft, low-alpha kind (smoke, fire) with a switch. Decide with a picture, not before.

## Things to preserve (what scripters rely on)

`llParticleSystem` flags: interpolate color/scale, emissive, follow source, follow velocity, wind, bounce, target pos / target linear, ribbons; pattern drop / explode / angle / angle cone / cone empty; start and end alpha, glow, burst rate, radius, speed min/max, angles, omega, texture; particle count limits (per source and `RenderMaxPartCount`); particles of avatars too far or hidden are not simulated (`LLViewerPartSim` priority and budget).

## Why not now

About 0.3 ms of main thread in the plaza, GPU draw negligible: the plaza alone does not pay for it. The club and spam sims do; the census there fixes the order of work.

**Update 2026-11-02:** measured again with the census (`mt3-`, `mt4-`, plan 8.35): the particle partition rebuild is about 0.6 ms a frame (about 16 different groups at 40 us, none rebuilt twice) and `updateParticles` 0.36 ms: about 1.0 ms of the main thread in the plaza, not 0.3 ms. Stage 1 alone removes the 0.6 ms.

**Stage 1 and 1b built 2026-11-02 (plan 8.36):** rows for the pull shaders, ribbons included, no face a particle, switch `RK_VK_GPU_PARTICLES`. A group costs 93 -> 20 kcycles on the main thread (about 0.45 ms in the plaza). Stage 2 (the simulation, `updateParticles` 0.36 ms) is next. Visuals not yet looked at by Adam.

## Stage 2 plan: the simulation (2026-11-02)

### What the census says (lap `pt8-zonesoff1`, plaza, RT off; counters in `LLViewerPartSim::updateSimulation`, log `Q1Census`, every 600 frames)

Per frame: sources loop 160 kcycles (75 sources), groups loop 620 kcycles (27 groups, 8 of them not visible and stepped every 8th frame), about 2,000 particles stepped (310 cycles each, 0.12 us), 12 spawned. That is about 0.3 ms of main thread, plus the stage 1 rows work (about 16 groups x 20 kcycles = 0.12 ms); at the 8,192 particle cap the stepping alone would be about 1 ms.

What the stepped particles are (steady windows; the first window, with a burst of target-position particles, was 26% analytic): **64% need nothing but their own start values** (no wind, target position, bounce or callback), 57% of the stepped need nothing from the source either; bounce 25%, follow velocity 38-41%, follow source 9-14%, target linear 6-11%, target position 8% (62% in the first window), wind 2%, callback 0%, ribbon 17% (all with a parent), interpolated colour and scale 80%+, emissive 91%; mean max age 3.6-4.0 s.

### The idea: a particle is a function of its age, not state to step

For a particle with no per-frame input, `LLViewerPartGroup::updateParticles` integrates `pos += dt*vel + 0.5*dt^2*accel; vel += accel*dt`: for a constant acceleration that is exactly `pos(t) = pos0 + vel0*age + 0.5*accel*age^2`, whatever the step sizes were (the visible-rate skipping included). Colour, scale and glow are linear in `age / maxAge`; death is `age > maxAge`. So the GPU needs the spawn record once (position, velocity, acceleration, spawn time, max age, start/end colour, start/end scale, start/end glow, flags, texture, blend) and the clock: **no per-frame CPU work and no per-frame upload for those particles**. The vertex shader of stage 1 (`rk_particle_corner`) evaluates the record at the frame's time instead of reading a position.

Classes:
- **A analytic** (64%): GPU from the record alone.
- **B source driven** (follow source 9-14%, target linear 6-11%): analytic given the source's position (and target) this frame: one vec3 or two a source a frame in a small table; `pos(t) = source(t) + offset(t)` with the offset ballistic in the source's frame; target linear `pos = source + frac*(target - source)`.
- **C stateful** (bounce 25%, target position 8%, wind 2%, callbacks 0%): stay on the CPU, built as stage 1 rows. Bounce is the question (below).
- **Ribbons** (17%): the record names its parent's record; the shader evaluates both.

### What the CPU still does (and what goes)

- Sources keep spawning (the RNG, source transforms, patterns, the throttles by `sParticleCount`, burst rate and adaptive rate): about 12 spawns a frame, each a record write instead of a `new LLViewerPart`.
- **Counts:** `sParticleCount` and the throttles need to know when a particle dies: death is deterministic (spawn time + max age), so each group keeps an expiry queue; `removeParticlesByID` marks records dead (a small delta write).
- **Groups:** the groups are spatial boxes particles move between (`posInGroup`, `put`). With a closed form the CPU can compute each particle's path box at spawn (a parabola: exact) and keep the group's extents as the union of its live path boxes: conservative, no transfers, no per-frame extents.
- **Order:** back to front within a group still needs distances. First step: the CPU evaluates the closed form for the order only (a few multiplies a particle, no object access, a sort of keys) and writes the index buffer; later (stage 4) a GPU sort.
- **Picking / right-click on a particle / blacklist:** `lineSegmentIntersect` evaluates the closed form on demand (only on a pick).
- **Region shift** (`shift(offset)`): a group offset in the table, not a write to every particle.
- **The size and budget cuts** of `updateGeometry` (5 mm at a distance, `tot_area > max_area`): the tiny-particle cut becomes a degenerate quad in the shader; the budget cut (order dependent, drops the farthest of a sorted-by-list-order prefix) needs a decision.

### Steps, each with a gate

0. Census: done (numbers above).
1. **Compare mode:** the record and the closed form on the CPU, run next to the stepping for class A particles; a log of the largest position, colour, scale and glow difference a frame. Gate: differences below a visible size (1 mm, 1/255) over a lap.
2. **GPU evaluation** for class A (the CPU order from the closed form), switch `RK_VK_GPU_PARTICLE_SIM` (default off at first, A/B launcher): the group rebuild writes only the index/order, spawn writes records. Gate: `updateParticles` + group cost, Adam's look at the same places as for stage 1.
3. Class B (source table, follow source, target linear).
4. Ribbons (parent record index).
5. Group extents from path boxes, expiry queues, no group transfers; the old stepping of class A is deleted (one path after a move).
6. Bounce and target position: decide (below).

### Decisions for Adam

1. **Bounce (25% of the plaza's particles).** It reflects at the source's height each time a stepped position goes below it (`z += -2 dz; vz *= -0.75`): frame-rate dependent as written. A closed form (reflect when the parabola crosses the plane) is cleaner and could be exact, but it is not bit-compatible with the old stepping; or bounce stays CPU-stepped (25% of the cost stays).
2. **Frame-rate independence:** analytic particles behave the same at any frame rate; the old ones differ slightly with the step size. Acceptable?
3. **The budget cut** (`RenderMaxPartCount` area budget): keep as an estimate from path boxes, or drop in favour of the count limits that already exist.

### Expectation

Plaza: the groups loop (0.24 ms) and the stage 1 rows work (0.12 ms) shrink by the analytic share (64%): about 0.2-0.25 ms. A particle-heavy sim (thousands of particles, mostly analytic): 1 ms and up, no per-frame per-particle cost left except what the GPU does. The plan stays honest about the plaza being the small case: stage 1 was the structural move, stage 2 is what the club and the spam sims pay for.

### Decisions (Adam, 2026-11-02: "i agree")

1. **Bounce stays CPU-stepped** (class C, stage 1 rows), not a closed form: the old stepping rests with small hops and follows a moving source's floor; a closed form would differ, for about 0.06 ms in the plaza. Revisit with data from a place with many bouncing particles.
2. **Frame-rate independence: proceed.** The analytic particles run from the same clamped simulation clock; the compare mode (step 1) shows the differences.
3. **Budget cut: dropped** in the order-dependent form; the shader collapses a single quad above the same area threshold (the guard against a screen-filling puff stays, the arbitrary loss of later particles goes).

### Step 1 (compare mode), built 2026-11-02

`LLViewerPart` keeps its spawn state (`mSpawnPos`, `mSpawnVel`, set in `LLViewerPartSim::addPart`, moved by `LLViewerPartGroup::shift`). With `RK_VK_PARTSIM_COMPARE=1` every particle with no per-frame input (no wind, target, bounce, follow source, callback) is compared after each step with `pos0 + v0*age + 0.5*a*age^2`, the velocity, and the colour, scale and glow at `age / maxAge`; the largest differences of each 600 frames go to the `Q1Census` log. Launch mode `partcmp` in `h6-fixed.ps1`.

**Step 1 result (lap `pt9-partcmp1`, 2026-11-02; 3.8 million particle steps compared over the lap, about 650,000 per 600 frames):** colour 0, scale 0, glow 0 of 255 (exactly equal), velocity 1e-6 m/s. Position: the largest difference grows with age, 1.5 mm at age 2.5 s, 4 mm at 7.9 s, 15 mm at 30 s (ages up to 30 s exist in the plaza), and 4-7% of the steps in the later windows are over 1 mm. That is the old stepping's rounding, not the closed form's: it sums `dt*vel` in 32 bit floats, thousands of times, at coordinates around 100 m (a float step there is 8 um, a random walk over 1,800 steps is a few mm), while the closed form rounds once. A centimetre of a particle that is itself decimetres wide at 30 s of age is not visible. The gate I wrote (1 mm) is too tight for ages over 3 s; the gate that matters is met: no difference that shows, none that grows apart from the age. The closed form is the more accurate of the two.

### Step 2.1 built (2026-11-02, switch `RK_VK_GPU_PARTICLE_SIM=1`, default off; launcher `run-viewer-partsim.bat`, lap mode `partrec`)

Split of step 2: 2.1 keeps the particles with no per-frame input as **records** on the CPU and evaluates them at the frame's clock (the draw path stays stage 1's rows), 2.2 would evaluate them in the vertex shader.

What 2.1 is: `LLViewerPartSim::addPart` turns a particle without wind, target, bounce, follow source, ribbon, callback, HUD flag or parcel media into an `RKViewerPartRecord` in the group chosen by the same rules (`LLViewerPartGroup::addRecord`); the `LLViewerPart` is deleted. No stepping: `expireRecords` (a sweep of an array of dead-at times) removes them; the group's death, count and `sParticleCount` / `sRecordCount` bookkeeping follow (`checkParticleCount` compares `sParticleCount2 + sRecordCount`). The simulation has a clock (`sSimTime`, the sum of the frames' dt). `LLVOPartGroup::updateRows` evaluates stepped particles and records into `RKPartEval` rows (the cuts of 1b, the closed form from `rk_part_color_at` / `rk_part_scale_at`, which are bit-equal to the stepping: compare lap `pta-partcmp1`, colour, scale, glow 0 difference); `rebuildRows` reads those rows only. A record is not moved between groups when it leaves its box: the group's extents take the box of the drawn particles. Picking walks the drawn rows. A group with records never takes the quads path (a parcel media particle in it shows its plain texture).

Lap `pta-partrec1` (plaza, RT off): stepped 750-1000 a frame and 1300 records (before: 2,000 stepped); groups loop 290-390 kcycles (before 620); `updateGeometry` 9.1 kcycles a call, rebuild 8.4-9.1 (before 11.3); no count mismatch lines; loop 9.15 ms (109 fps, GPU bound, the world differs from earlier laps). The main thread's particle total is about 0.3-0.35 ms now (sources 0.07, groups loop 0.11-0.15, updateGeometry and rebuild 0.11): the 1.0 ms + 0.36 ms of the start of stage 1, less than a quarter. The rest is per group (29 groups: `markRebuild`, count checks, the camera lookups) and the 750 stepped particles. 2.2 (records in the vertex shader) would take about 0.1 ms more in the plaza; whether it is worth its risk is for the census of a particle-heavy sim to say.

**Records on by default (2026-11-02):** Adam looked at `run-viewer-partsim.bat` ("they looked fine") and said default on. `RK_VK_GPU_PARTICLE_SIM=0` (launcher `run-viewer-partsstep.bat`) steps them again; `RK_VK_GPU_PARTICLES=0` is the quads. Sanity lap with the defaults (`ptb-zonesoff1`): 1,100 records and 1,100 stepped, no count mismatch lines, no crash, 109 fps (GPU bound). The estimate of the whole stage 1 + 1b + 2.1 gain is about 1.1 ms of main thread in the plaza (1.4 -> 0.3 ms; the 1.4 adds the 0.4 ms of `updateGeometry` that the earlier 1.0 ms did not contain); stitched from separate laps with different particle populations, not one A/B. Adam: the approximation is fine, the trade is good (the GPU will be faster anyway). Step 2.2 (records in the vertex shader) waits for a census of a particle-heavy sim.

### Step 3 built (2026-11-02): every law that is not wind, a callback or a ribbon is a record

Adam: "let's go for all of them. even bounce to see how that looks", "the visuals can't be too far off", having all of it on the GPU is "more efficient and simpler to deal with". `RKViewerPartRecord` has a mode (`FREE`, `FOLLOW`, `TARGET_LINEAR`, `TARGET_POS`) and a bounce flag, and `evaluate(now, pos, vel)` gives the position and velocity of each:
- **follow source**: the source's position now + the free flight from zero (the stepping puts the particle at its source plus its offset every step, so the offset is the free flight): 2 mm from the stepping.
- **target linear**: the source's position + (age / max age) of the way to its target.
- **target position**: the stepping's pull towards the target (`vel` moves towards the speed that gets there in the time left, rate 5 / time left) solved: with u = x / r and w = u - v, `dw/dt = -4 w / r - a`, so `w = (w0 + a r0/3) (r/r0)^4 - a r/3` (checked against the stepping offline: the difference is the stepping's own step size, 3 cm at 120 fps, halving with the step). The particle keeps its place relative to a target that moves.
- **bounce**: the exact sequence of flights between the bounces (z mirrored, vertical speed * -0.75), ends in rest on the floor when the bounce is below 2 cm/s (the stepping hops about on it); the floor is the source's height at the spawn (follow source: the source's).

Beach compare lap (`beach3-partcmp1`, stepped particles against the closed form of each law): free flight 4.5 mm largest, follow source 2 mm, bounce 4.5 cm largest / 2 mm rms (a first version that did not bounce a particle starting on the floor going down was 2.4 m off; found by an offline check of the stepping against the closed form), target position 1-6 m largest / 4-19 cm rms (a moving target: the old law chases the target's place of each step, the record keeps its offset from the target; the same end point), colour, scale, glow exact. Lap with everything as records (`beach4-zonesoff1`): 14 particles stepped a frame (wind, callbacks, ribbons), 3,670 records (free 1,370, follow 340, target position 1,190, bounce 770), groups loop 141 kcycles (650-775 before), 129 fps, GPU bound.

(Step 2.2, the records evaluated in the vertex shader, was done as G0 and then replaced by the sort pass that evaluates them once, G1: see the status at the top.)

**GPU evaluation, cull and sort: design written (2026-11-02), see `PLAN-gpu-particles-gpu-sort.md`.** Shader evaluation alone would save about 0.08 ms and leave the laws in two places; the gain is in doing evaluate + cull + sort on the GPU together. The census (`beach5-`): a group has 1.06-1.10 draw states and 1.06-1.16 textures, 6-11% of rebuilds more than one state. Four decisions for Adam in section 6 of that document. No code.
