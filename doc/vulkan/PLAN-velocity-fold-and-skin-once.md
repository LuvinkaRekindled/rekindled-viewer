# PLAN: fold velocity into the G-buffer (idea 1), skin once and read everywhere (idea 2)

2026-11-14. Scope only: nothing here is built. Everything below was read from the code on this date; the list at the end says what was NOT read and decides the first steps.

## What is there today

### The velocity pass (pipeline.cpp, `beginAvatarVelocity` 21614, `renderAvatarVelocity` 21639, `generateMotionVectors` 21953)
- Runs after the G-buffer (llviewerdisplay.cpp 1427), before the lighting. Gated by `RenderAvatarMotionVectors` and an upscaler being on (`avatarVelocityWanted`).
- Target: `mAvatarVelocityMap`, RGBA16F at render size, cleared every frame. rg = pixels moved, a = 1 where something wrote.
- Three record kinds, all drawn with NO depth test of their own: the fragment shader (`velocity.frag`) reads the G-buffer depth and `discard`s whatever is not the G-buffer's surface (tolerance 0.004 relative + 0.005 m).
  - rigged parts: `velocity_skin.vert`, a 48 byte entry per draw, skins twice (this frame's palette and last frame's);
  - moving objects: `velocity_object.vert`, model matrices of this frame and last (CPU copied into `sModelMatrixHistory`);
  - the system avatar's body parts: `velocity_body.vert`, one draw per mesh with two uniform palettes.
- CPU side: `build_velocity_rigged` (a pool job started before the G-buffer) walks the rigged draws AGAIN (second collection of the same draws the G-buffer rows already name), runs the "did the palette move" test, the range test, and the moving-object test. The simplified-LOD lists the G-buffer uses are looked up again (`ll_rigged_lod_opaque`) so the velocity draw covers the same pixels.
- Consumer: ONLY `generateMotionVectors` (motionvectors.frag): where the avatar map's a > 0.5 its rg replaces the camera motion computed from depth. Result goes to `mMotionMap` (RG16F), which DLSS, FSR, the denoiser and DLSS-NR read. `visualizeMotion` (debug view 7) also reads the avatar map. Nothing else reads `mAvatarVelocityMap`.
- The settings comment says about 1 ms of GPU in a crowd (2026-10-12) and no measurable cost (2026-10-27). So the GPU win of idea 1 is bounded by about 1 ms in a crowd; the CPU job and the code removed are the larger part.

### The G-buffer
- `deferredScreen`: RGBA8 diffuse + three more attachments (ORM, normal, emissive) + depth. Rows come from the scene table: `gbuffer.vert` reads a 160 byte `Row` and a 176 byte `ModelBlock`; a rigged draw is the `HAS_SKIN` variant (`row_skin`: weights + palette by address; the palette has a tail entry holding the rig anchor). Current pose only, no previous pose anywhere in the row.
- Free room in the row: CORRECTED in the second pass below. Only `skinPad2` is free (`skinPad0` holds the Batch Size debug colour, `skinPad1` the vertex limit, `pad.x` the texture matrix slot, `pad.y` the PBR row).
- `ModelBlock` has no previous matrix. A previous model matrix for moving objects needs either 64 more bytes for every model (176 -> 240) or a second table indexed by the same model number and filled only for moving models.

### Where a vertex is skinned today (one avatar draw, one frame)
1. G-buffer vertex shader (`gbuffer.vert` HAS_SKIN; glTF `pbr_*` variants), once per vertex invocation, current pose.
2. Alpha (transparent) vertex shaders: `alpha.vert`, `alpha_pbr.vert` (`row_skin`).
3. Shadow vertex shaders: `shadow.vert`, `shadow_alpha.vert` (packets; palette by address).
4. Velocity pass: `velocity_skin.vert`, twice per vertex (current + previous), plus `velocity_body.vert`.
5. Ray scene: compute `kSkinShader` (llvkrt.cpp 1062), world position (w = texture index) and optional normal per vertex into a skinned buffer indexed by the absolute vertex number. Mode 0 = rigged mesh weights, mode 1 = the system avatar's own mesh (one weight, joints i and i+1).
6. Mirror and probe views: the views draw through 1-3 again.
The ray scene call runs by default (LL_VK_RT_EARLY=3) BEFORE the G-buffer on the compute queue, overlapping it.

## Idea 1: fold the velocity into the G-buffer

What changes:
- `deferredScreen` gets a fifth attachment, RG16F: the screen motion in pixels. "Written by an avatar or moving object" cannot be 0 (an avatar pixel with zero motion differs from the camera motion), so the clear value is a sentinel (a value no real motion has, e.g. 65504) and `motionvectors.frag` tests for it. (RGBA16F would double the bytes of a 5th target for a flag.)
- Static draws must not touch the attachment: per-attachment colour write mask 0 in their pipelines (attachment stays at the sentinel). Only rows that can move write it.
- Vertex shaders of the draws that can move (`gbuffer.vert` HAS_SKIN and its glTF counterpart, the moving-object variant, the body program `avatar.vert`) output `vary_cur`/`vary_prev` clip positions (the same two varyings `velocity_skin.vert` has); the fragment shaders (`gbuffer_diffuse`, `_mask`, `_material`, `_bump`, `pbr_opaque`, `avatar.frag`) compute `mv` and write it. A masked fragment that discards writes nothing, so the attribution is exact and the depth tolerance test disappears.
- Rows: previous palette address (the two free uints), a flag bit "writes velocity" that carries the range test and the "palette did not move" test (a row of a still avatar does not write: its pixels keep the sentinel and take the camera vector, as today). Moving objects: a previous matrix source (see above).
- The "did not move" and range decisions have to be made where the rows are written (the worker's row build) instead of in a separate job; the data it needs (current and previous palette of each skin) is the palette cache the G-buffer rows already reference.
- Removed: `renderAvatarVelocity`, `beginAvatarVelocity`, `build_velocity_rigged` and its job and the `LL_VK_VEL_JOB` modes, `ll_rigged_lod_opaque` use in the velocity pass, `sModelMatrixHistory` (becomes the previous-matrix source), `mAvatarVelocityMap`, `gAvatarVelocityPullProgram`, `gObjectVelocityPullProgram`, `gAvatarBodyVelocityProgram`, `velocity.frag`, `velocity_*.vert`, `velocity_pass.inc`, the four settings that tune the pass (`RenderAvatarMotionVectors*`, the first would become "on whenever an upscaler is on").
- What it costs: every G-buffer pipeline in the world pass has one more colour attachment in its blend state (write mask 0 for most); the clear grows by 4 bytes per pixel. The rigged vertex work per moving draw becomes cur + prev inside one invocation (the velocity pass did cur + prev AND the G-buffer did cur: three skins become two).
- Behaviour difference to watch: hair cards and other masked rigged parts now attribute per fragment instead of by depth tolerance; layered parts that were previously "within tolerance" of each other now each get their own part's vector (more correct, but a visible change in a crowd test).

## Idea 2: skin once

Aim: one compute pass per frame skins every drawn avatar (current AND previous pose in the same invocation from the two palettes); the G-buffer, alpha, shadow and mirror vertex shaders read the skinned positions and normals like a static draw; the ray scene builds from the same buffer. The rigged `HAS_SKIN` variants of the vertex shaders disappear, the palettes leave the rows' hot path, and a previous pose costs one more read, not another skin.

What reading the code says is in the way:
1. Timing. The skin runs inside the ray scene call, which by default runs on the compute queue concurrently with the G-buffer. A G-buffer that reads its output has to wait for it. The skin has to be split from the acceleration structure build: skin first (before the world pass, on the graphics queue, with a barrier to vertex shader reads), the bottom/top level builds stay on the async queue.
2. Cadence and coverage. The ray scene skins an avatar only every `rtSkinPeriod(distance)` frames (1 within 8 m, up to 4 beyond 28 m; `rt_name_avatar_due`) and only while ray shadows are on. The G-buffer needs EVERY drawn avatar skinned EVERY frame, RT or not. The period then applies only to the refit of the acceleration structure; far avatars cost more compute than today (the cost of that is the one number to measure before committing).
3. Tangents. The compute writes position and normal; a normal mapped rigged draw also needs the skinned tangent (the G-buffer's `vary_tangent`). Either a third output or the rotation read again for tangents only.
4. Row addressing. The skinned buffer is indexed by the absolute vertex number; the row's `positions` would point at it (base adjusted by the first vertex). The rows are written by the worker; the arena address of each draw's skinned output has to exist when the row is written (a per-frame bump arena sized from the entries the skin pass is given, known after the naming/collection step that now happens in the lighting or the early scene call).
5. The system avatar's body parts use mode 1 (and `avatar.vert` with uniform palettes, not rows), a different path again; they can join the same pass as mode-1 jobs.
6. Memory: current + previous output per skinned vertex (2 x 16 bytes position, 16 normal) if previous is stored; or recompute previous from the previous palette in the same invocation (no storage, a second palette read and blend per vertex).
7. Mirror, reflection and probe views get the same skinned vertices for free (the skin is camera independent), which is the part of idea 2 that removes work that exists today.

## Order

1. Idea 1 first, self contained: the velocity pass goes, the G-buffer rows get previous palette and velocity outputs. It is what idea 2's G-buffer-reads-skin step would later simplify (the cur/prev vertex shader pair collapses to two buffer reads).
2. Idea 2 in stages: (a) split the skin from the scene build and make it per frame for all drawn avatars with both poses; (b) G-buffer reads it; (c) alpha, shadow, mirror; (d) delete the `HAS_SKIN` variants. Each stage ends with a lap and a ground mean/std check.

## Second pass (2026-11-14, Adam: the cost of the current pass is already known, about 1 ms in a crowd; the bar for idea 1 is "efficient and not more expensive", not a new measurement)

Read and decided:
- Attachments (llvkrenderer.cpp 923-951): the pipeline's colour attachment count and formats come from the render target (`key.mColorCount`, `key.mColor`), so a fifth attachment on `deferredScreen` is picked up by every G-buffer pipeline without a per-program edit. Every pipeline then writes it unless masked.
- Masks: blend/write mask is dynamic state (`vkCmdSetColorWriteMaskEXT`, 3982) but the renderer applies ONE mask (`mBlend.mWriteMask`) to all attachments. Idea 1 needs a per-attachment mask: a small renderer state addition (a flag "the velocity attachment (index 4) is off" that sets `masks[4] = 0`, set by the draw paths of static rows). Without it every static pixel would write the sentinel: extra bandwidth for the whole screen, which would make the fold cost MORE than today. So this is a requirement of the idea, not an option.
- Setting: `RenderAvatarMotionVectors` default is 1 (on), `...Objects` 1, `...Packets` 1.
- Row space: the row (`GeoGpuG`, 160 bytes) has ONE free uint (`skinPad2`). The previous palette is not an offset from the current one: the viewer names palettes by id; the worker resolves an id to an address per frame (`palette_id_address`), and the previous frame's palette lives in the previous frame's arena (`palette_id_address_prev`, 0 = no history, then previous = current). Choices, in order of preference: (a) rigged rows store the palette ids, the VS reads the two addresses from a small per-frame GPU table indexed by id (one extra 16 byte read per vertex, cached); (b) move the Batch Size debug colour out of `skinPad0` (it is a debug view) so two uints are free for the previous address; (c) grow the row to 176 bytes (more bandwidth on every row read, rejected). (b) is the cheapest at runtime and is the first try; it needs the debug colour to move (e.g. computed from the row index, or into `pad2`).
- Previous model matrix for moving objects: models are written per frame into a 176 byte block (`allocVertexScratch(176 * slots)`, 9914); a second scratch array of previous mvp matrices (64 bytes each) only for moving models avoids growing every block. Source: `sModelMatrixHistory` today (CPU map keyed by address), moved to where the model blocks are written.
- Which programs carry a rigged or moving variant: the G-buffer family in `gbuffer.vert` (HAS_SKIN, 6 uses) and the glTF vertex shader `gltf_scene.vert` (HAS_SKIN, 2 uses) with `gltf_scene.frag`/`pbr_opaque.frag`; the body parts use `avatar.vert` with uniform palettes. Terrain, trees, impostors, sky are static and stay masked off.
- Efficiency, reasoned (not measured): the pass removed is one raster of every moving draw plus a clear and a resolve of an 8 byte per pixel target plus the CPU job; added is one 4 byte per pixel attachment (cleared, written only where something moves), two varyings and a second palette blend in the VS of moving rows. The skin work goes from three blends per vertex of a moving draw (G-buffer, velocity cur, velocity prev) to two. A still avatar costs nothing extra (it does not write, its row is not flagged). Net: not more expensive in any case the reading shows; the risk is only the unmasked static pipelines, which the mask requirement removes.
- Order inside idea 1: (1) per-attachment mask + fifth attachment + sentinel clear + motionvectors.frag reading it, with the old pass still running and the two results compared (a debug view of the difference); (2) rigged rows (previous palette, flag, VS/FS outputs); (3) moving objects; (4) body parts; (5) delete the old pass and its settings.

Still not read (idea 2 only; idea 1 does not need them):
- Cost of the skin compute at every-frame cadence for far avatars (idea 2, point 2).
- The worker's handling of body-part draws (mode 1) in the G-buffer, for stage (a) of idea 2.

## (older list, answered above) NOT read yet
- How the renderer builds G-buffer pipelines: is the number of colour attachments and their write masks derived from the render pass target (then a 5th attachment is one change), or fixed per program? (llvkrenderer.cpp pipeline creation; `TexelFormat` and `addColorAttachment` path.)
- The worker's writer of rows (`GeoGpu`, llvkworker.cpp): where the rigged row's palette address is written, and whether the two free uints can be made adjacent, and where per-model matrices are filled.
- The glTF and terrain/tree G-buffer programs: which of them can have a rigged or moving variant (glTF rigged yes).
- The default and the saved values of `RenderAvatarMotionVectors*` (the settings file was not read for values, only the comments).
- Real GPU cost of the current velocity pass in London (a Nsight or Tracy zone of "avatar velocity"), so the saving of idea 1 is a number and not a guess.
- Cost of the skin compute at every-frame cadence for far avatars (idea 2, point 2).
