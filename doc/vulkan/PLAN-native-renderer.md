# Plan: a native renderer (no OpenGL, no GL mirror)

Status: PLAN ONLY. Written 2026-10-07. Numbers are measured in this tree unless marked "estimate".
(`PLAN-retained-records.md` is the smaller, incremental alternative on top of today's mirror; this document is the ground-up route.)

## Scope decisions to make first (they remove most of the cost)
- Windows only, Vulkan 1.3 minimum, NVIDIA RTX class as the target (bindless, buffer device address, EDS3 are required already; DLSS needs RTX). No OpenGL, macOS, Linux, or pre-RTX GPUs.
- Keep: the sim protocol, asset formats, the object model (`LLViewerObject` and friends), the UI toolkit (`llui`), the GLSL shaders as a starting corpus (231 files, 209 programs already compile to SPIR-V through `llvkshader`).
- Replace: everything that draws.

## What exists and what it is worth
| Part | Size | Fate |
|---|---|---|
| Native Vulkan code: context, renderer (passes, barriers, sampler/pipeline caches, bindless set), shader compiler, SPIR-V and pipeline caches, DLSS | 7.5k lines (`llvkcontext/renderer/shader/dlss/probe`) | **kept, becomes the RHI** |
| GL emulation: call interception, GL state tracking, the worker queue, verify tools | 10.2k lines (`llvkgl*`, `llvkasync*`, `llvkmirror*`, `llvkverify.h`) | **deleted** |
| GL-shaped classes the viewer calls: `LLRender`, `LLVertexBuffer`, `LLImageGL`, `LLRenderTarget`, `LLGLSLShader`, `LLGLState`, `LLTexUnit`, `LLGL*` | about 30k lines in `llrender` | **replaced, in two steps (below)** |
| Callers of those classes | `gGL.` 4,743 call sites in 145 files; `LLVertexBuffer` 83 files; `LLGLSLShader` 72; `LLTexUnit` 114; `LLGLState` 60; `LLImageGL` 44; `LLRenderTarget` 36 | **migrated progressively** |
| The renderer proper: draw pools (13 files, 6.8k lines), `pipeline.cpp` (14.4k lines), spatial groups/draw infos, the octree as a render structure | about 40k lines | **replaced by the new frame and scene code** |
| Object, network, avatar, UI logic | most of 900k lines in `newview` | kept |

## Stages
Each stage must keep the viewer playable and has a gate. Estimates are for focused work at the pace of this project; the real uncertainty is testing against real content.

**N0. Golden images and decisions (about 1 day).** Without OpenGL there is no reference to compare with, so capture it now: per-pass dumps and whole-frame screenshots of a set of scenes (crowd, water, interior, night, snapshots, HUDs) from today's build, plus the existing verify numbers. Every later stage is judged against them.

**N1. Native backend under the existing classes (1-2 weeks).** Re-implement the GL-shaped classes directly on `Renderer`: `LLVertexBuffer` on device-local buffer arenas, `LLImageGL` on `VkImage`, `LLRenderTarget` on images, `LLGLSLShader` on native programs with a real uniform struct instead of loose uniforms, `LLGLState` as dynamic state, `LLRender` immediate mode as batched native draws. Window creation without WGL. Delete the interception, the queue, GL state tracking.
Gate: same pictures as N0, fps not lower.
What it buys by itself (estimate): the interception push (5-8% of the main thread) and its lock traffic go away, no duplicate GL memory, no GL context, startup faster. Mostly it removes the translation layer so the later stages are possible.

**N2. Frame structure (about 1 week).** A frame graph (passes declare what they read and write; barriers and render-target lifetimes are derived), a job system, and a **pipelined frame**: the main thread simulates and publishes an immutable frame description, a render thread records and submits it while the main thread starts the next. This is the structural fix for "the main thread paces the frame". It needs the scene data below to be double-buffered, which is why it comes with N3.

**N3. GPU scene (about 2 weeks).** Persistent vertex/index arenas, a global instance buffer (transform, material id, flags), a global material table, one bindless texture table with stable slots and a feedback of which textures were used. Render proxies replace `LLDrawable`/`LLFace` as what the renderer sees: `LLVOVolume`, avatars, particles, terrain, water, sky each publish proxies and send deltas when something changes. Geometry generation (`LLVolume`, mesh LODs) stays on the CPU but writes into the arenas.
Gate: static scenes cost almost no CPU per frame.

**N4. GPU-driven passes (1-2 weeks).** Compute culling (frustum, Hi-Z occlusion), LOD selection, indirect draws for the shadow cascades, G-buffer, and reflection views, all from the one GPU scene. Draw counts drop from 6,000-17,000 to dozens.
This is where the measured costs go: shadow and G-buffer submission and building (about 3.5 ms), postSort/stateSort (1.7 ms), updateCull (0.6 ms), the octree as a render structure (about 1.2 ms of move/cull work).

**N5. Lighting, transparency, shadows (2-3 weeks).** Clustered lights; order-independent transparency (replaces the sorted alpha pass: 2,275 draws and 884 shader switches in the blues place); cached or virtual shadow maps; ray-traced water reflections and shadows on RTX with probes as the fallback; the SSR work becomes proper hierarchical tracing.

**N6. Avatars on the GPU (1-2 weeks).** Skinning in compute once per frame, shared by all views; attachments follow joints by index in the shader (the 1.5 ms attachment loop and its octree moves disappear); other avatars' animation evaluated on the GPU, the CPU keeps only state.

**N7. Upscaling and frame generation by design (about 3 days after N3).** Per-instance motion vectors in the G-buffer, built-in jitter, scene-referred HDR, UI as a separate layer. DLSS ghosting on moving objects goes away; DLSS frame generation becomes feasible; DLSS 5 can be attached when it is public.

**N8. Texture streaming from GPU feedback (1-2 weeks).** The GPU reports which mips it needed; the CPU virtual-size loops (0.5-1 ms) go away. Sparse/virtual textures after that if wanted.

**N9. UI (3-5 days).** Retained, cached panels, glyph atlas text, drawn into its own layer (about 1.2 ms today).

## What this is expected to give (estimates, not measurements)
- N1 alone: 5-10% faster main thread, much simpler code.
- After N4 the per-frame render CPU work for the shadow/G-buffer/cull/sort items drops by most of the roughly 8-9 ms they cost in the blues place; what remains on the main thread is the object update, avatar logic and network (about 4-5 ms), and with N2 that overlaps the render thread.
- Plausible result in a crowd like the blues place: GPU-bound at about 7-9 ms, i.e. 110-140 fps, against about 60 now (loop 16 ms), before DLSS.

## Risks (the honest list)
- No fallback and no reference renderer after N1; N0 is the only safety net. Odd user content (alpha tricks, huge meshes, broken materials) is the main source of surprises.
- Things that need GL today and must be redone natively: snapshot and picking readbacks (`readPixels`, the pick passes), media plugin textures (CEF shared textures need Vulkan external-memory interop), the reflection probe renderer, terrain painting, the material and glTF previews.
- The object model is tangled with the renderer (`LLDrawable` and `LLFace` carry rendering state); N3 is where that has to be cut cleanly.
- Third-party rendering code (CEF's GPU process, FMOD is audio only) is unaffected but needs checking.
- Time: I would say 2-4 months of focused work to parity plus the gains of N4-N6, but the testing against real content is the part I cannot size from here.

## Order and why
N0, N1 first (everything depends on removing the translation layer and nothing visible changes); N2+N3 together (they only make sense as a pair); N4 (largest measured gain); then N5, N6, N7 in either order; N8, N9 last. N1-N4 is the part that answers the main-thread limit; N5-N9 are quality and further savings.
