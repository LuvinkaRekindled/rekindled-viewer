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

6. **Per-object motion vectors** (added 2026-10-11). Today `LLPipeline::generateMotionVectors` rebuilds each pixel's world position from the depth buffer and projects it with this frame's and last frame's CAMERA matrices: the vectors describe camera motion only, as if the world were still. Anything that moves relative to the world (avatars and their attachments, animated and flexible objects, vehicles) gets a vector that says "static", and DLSS smears or ghosts it. Ray reconstruction does not fix that: it is also temporal and needs the same vectors (the claim that it has "no ghosting" holds only where its vectors are right). Plan:
   - **Avatars first** (the largest visible win in this viewer). The ray scene's compute skinning pass already writes the current skinned world positions of every avatar vertex to an arena range; keep LAST frame's range too (two ranges per avatar, swapped) so the previous position of each skinned vertex is known. The avatar G-buffer vertex shaders (`avatarSkinV`, `objectSkinV`) read it by vertex index (buffer address) and write the velocity (current minus previous, in screen space) into a velocity attachment. Without the compute pass the alternative is to keep the previous frame's joint matrix palette per skin and evaluate the skinning twice in the vertex shader.
   - **Moving prims and vehicles second:** the previous model matrix per drawable (only drawables that moved need one; a static drawable writes zero object velocity). The vertex shader computes the previous position from it.
   - **Combine in the existing pass:** final motion = camera motion (the current reconstruction, kept for pixels marked static) + the stored object velocity.
   - **Reset rules:** no previous position (a new avatar, rez, teleport, LOD or impostor switch, the first frame after the skin changed) means previous = current; alpha-blended surfaces have no vectors (a DLSS limit).
   - **Order and test:** before step 3 below, because it also fixes DLSS super resolution ghosting on moving avatars today. Test: orbit around an avatar in a looping animation with DLSS on, compare the trailing before and after, and add a debug view of the motion map.

### Design notes for item 6 (2026-10-12, from reading the code; nothing built, Adam: no work until a new session)
Decision (Adam): build option A below first; option B only if A costs too much.

**What the code does today (read, not measured)**
- `LLPipeline::generateMotionVectors` (pipeline.cpp, about line 9647) is one full-screen pass over the G-buffer depth; the output `mMotionMap` is RG16F (allocated about line 1052, handed to DLSS in `applyUpscale`, called from about line 10569). Every pixel gets camera motion only.
- Avatar body: `LLDrawPoolAvatar::renderAvatars` -> `avatarp->renderSkinned()` -> `LLViewerJointMesh::drawShape`, program `gDeferredAvatarProgram` (avatarV + avatarSkinV: one float weight, 45-vec4 `matrixPalette`).
- Rigged attachments and animesh: draw infos in the render map (the rigged variant of a pass type is the type number + 1), drawn by the pool batch code (`lldrawpool.cpp` around lines 430-560, the materials/bump/alpha pools); the palette is `uploadMatrixPalette(avatar, skinInfo)` -> `LLVOAvatar::updateSkinInfoMatrixPalette` (llvoavatar.cpp about line 11641) -> `MatrixPaletteCache::mGLMp` (12 floats per joint, mat3x4); shaders use `getObjectSkinnedTransform()` (objectSkinV.glsl, weight4) and then `modelview_matrix * skin`.
- `build_shadow_skin` (pipeline.cpp about line 4720) already walks those rigged draw infos and collects vertex buffer, index range, palette and model matrix per draw: the velocity pass can reuse its enumeration.
- Rigid attachments (eyes via `renderRigid`, prims worn on a joint) are NOT skinned: they move through their model matrix. They belong to step 1b (moving prims), not to this step.
- Rejected: taking the previous and current vertices from the ray scene's compute skinning output. Far avatars are only refreshed every 2-4 frames (`skinperiod`), the body is a separate record kind, and it would tie DLSS to the ray scene's scheduling.

**Option A (chosen): a velocity pass for avatars, after the G-buffer and before `generateMotionVectors`**
1. *Previous palettes.* `MatrixPaletteCache` keeps last frame's `mGLMp` next to this frame's (rotate when the cache entry is first computed in a new frame; a new entry has previous = current). The body palette (from the joint world matrices in `LLViewerJointMesh`) gets the same treatment. Palette space must be checked: it is agent/region space, so a region crossing or origin shift must reset previous = current.
2. *Programs.* Two small vertex shaders: rigged (weight4, mat3x4 palettes, current and previous) and body (float weight, 45-vec4 palettes, current and previous), plus one fragment shader. They write the TOTAL screen motion of the vertex, not an addition to the camera motion: `mv = (prevClip.xy/prevClip.w - curClip.xy/curClip.w) * 0.5 * render_res`, with `curClip = unjitteredViewProj_cur * skin_cur * v` and `prevClip = viewProj_prev * skin_prev * v` (`mViewProjCur` / `mViewProjPrev`, the unjittered ones `generateMotionVectors` already keeps). So the avatar's pixels REPLACE the camera vector. (This corrects the wording "camera motion + object velocity" in item 6.)
3. *Target.* Own RGBA16F target at render size, no depth attachment, cleared to 0; alpha = 1 where an avatar wrote (the flag the combine pass needs). The fragment shader reads the G-buffer depth with `texelFetch` and keeps the pixel only when its own depth matches within a relative tolerance (about 0.1-0.5 %); otherwise it discards. Depth EQUAL cannot be used because the vertex arithmetic differs from the G-buffer shaders; a LEQUAL test would wrongly mark pixels where an alpha-masked hair cut-out was discarded. The tolerance test handles occlusion by walls and other avatars, hair cut-outs and thin geometry together.
4. *Draws.* Same draw infos as the G-buffer pass (rigged lists via the `build_shadow_skin` style walk, body via the avatar loop), only for avatars that are rendered as real geometry this frame: skip impostors, avatars beyond a distance limit (DLSS ghosting is not visible on them), and skip entirely for impostor renders, reflection probe renders and snapshots. Alpha-blended parts get no vectors (DLSS has no vectors for blended surfaces anyway).
5. *Combine.* `motionVectorsF.glsl` samples the velocity target: flag set -> use its vector, else the camera vector from depth as today.
6. *Switch and debug view.* A live setting (name to be chosen, default on once it passes) and a debug view that shows the motion map (magnitude as colour).
7. *Measure on the lean build:* extra worker ms per frame and extra draws per frame, GPU ms of the pass, DLSS trailing before and after (orbit an avatar in a looping animation, then a crowd).

**Option B (fallback if A costs too much):** a fifth G-buffer output written by the avatar programs only. No extra draws, but every other G-buffer program would have to write a zero velocity (or the attachment write must be masked per program, and then a wall drawn after an avatar leaves stale avatar velocity unless the combine pass also checks a stored depth). It touches many shaders and has more risk of visual regressions.

**Open points to read first in the new session**
- Where exactly `LLViewerJointMesh::drawShape` gets its body palette and how to keep its previous values; whether the body palette is in the same space as the rigged one.
- The exact pass lists that hold rigged opaque and masked draws for the G-buffer (simple, fullbright, material, glTF/PBR, bump variants), and which of them have a normal-mapped variant that needs nothing extra (the velocity pass ignores textures except for alpha masks: hair cut-outs need the alpha test, so masked draws need their diffuse texture bound).
- How a pass binds the G-buffer depth as a sampler in the native renderer (the motion pass already does it: `bindTexture(DEFERRED_DEPTH, &mRT->deferredScreen, true)`).
- Own avatar in mouselook and the HUD (no velocity needed there), animesh control avatars, jelly dolls.
- Step 1b (moving prims and rigid attachments, previous model matrix per drawable) comes after A is measured.

## Suggested order
1. Buffer arena with stable vertex/index buffers (also pays back in rasteriser worker time; measured at about 0.45 ms vertex input + about 1% rename cost today, so the rasteriser gain alone is small; the reason is ray tracing).
2. Inline ray query shadows for the sun, replacing cascade lookups behind a setting: simplest correct test of BLAS/TLAS, compare with the cascade picture and cost.
2b. Per-object motion vectors for avatars, then moving prims (item 6 of the list above): fixes DLSS ghosting on its own and is needed by step 3.
3. Material table and ray traced water/glossy reflections, with DLSS-RR fed by the guide buffers.
4. Decide on GI after seeing 2 and 3 on screen and in the profile.

## Current order (decided with Adam 2026-10-12)
1. **Per-object motion vectors**, avatars first and then moving prims (item 6 above, step 2b).
2. **Finish ray traced shadows and reflections** (step 2 gaps, then step 3): the shadow gaps are body hair and eyelash alpha parts, Linden trees, skinned vertex ranges that are whole spans (reduce), avatar distance/vertex tuning (C4), soft shadows and compaction (stage D); then the material table, ray traced water and glossy reflections, and DLSS ray reconstruction fed by the guide buffers (needs 1).
3. **The GPU scene** (`PLAN-gpu-scene.md`, S1 onward), after its scope is confirmed.
Flexi vs camera is NOT in this order: it is under "Later investigation" at the end.

## Risks
- BLAS builds for a world that changes (prims edited, sculpts, rigged avatars, 75-avatar crowds): the build budget per frame is the real limit, not the trace.
- A stale buffer handle is a GPU fault: the arena needs the same discipline as the texture slot epoch.
- DLSS-RR support and API are the SDK's; confirm the SDK version and the hardware requirements before step 3. (DLSS 5 neural rendering is a separate wait item: see the project notes.)

## To revisit (added 2026-10-11, after the device-lost hunt; none of these is started)
- **Ray scene CPU per call.** At about 10,000 instances the instances step was about 3 ms per call and there are about two scene calls a frame; measured with the timers on, which cost a lot on their own (the lean build ran 150 to 170 fps against 60 with the timers and diagnostics on). Re-measure on the lean build first. Candidate: skip rebuilding the instance list on a second call when nothing changed.
- **Texture memory.** Textures are stored as RGBA8 in the Vulkan mirror (the "lossy compression" setting does nothing here). Block compression (BC7, BC1/BC3 where quality allows) would shrink them 4 to 8 times; needs an encoder and a quality check. The viewer's texture budget now uses the OS's real video memory numbers.
- **Startup.** About 70 s from launch to the login screen: the disk texture cache check (a 13.9 GB cache with thousands of bad-size entries).
- **Main thread stalls found by the stall logger** (`LL_VK_DIAG=1`): a lock in `LLTextureCache::update` at login (about 2.4 s); FMOD's update calling the Windows audio service (RPC to AUDIOSES) on the main thread (about 0.8 s). See the project notes on replacing FMOD.
- **Diagnostics for device loss.** Everything added during the hunt is behind `LL_VK_DIAG=1` and `run-viewer-clean-rtshadows-diag.bat`; the lean launcher is `run-viewer-clean-rtshadows-lean.bat`. If a device lost returns, run the diag launcher and read the "STUCK IN THE DRAW" and flight recorder lines first.

## Later investigation (not in the order above)
- **Things that still depend on the camera and should not.** The ray traced sun shadows and the ray scene are already camera independent (no cascade fitting, instances in world space). Culling and LOD staying camera based is expected. Open: **flexible prims move or flap when the camera moves or zooms** (test spot: the flexi flags in the **Lost Outlaws scan area**; one flaps when you zoom, so stand there and zoom in and out while watching it). Find what ties the flexi update to the camera (visibility / LOD changes, the idle update in the flexible volume implementation) and whether it should run on world time instead. The ray scene's `bbox=1` debug switch (`<work>/rt-debug.txt`) can log the geometry extents of the moving casters while testing. Related: item 6 above, because a flexi prim also needs a correct object velocity.
