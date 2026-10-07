# N1: the native backend (big-bang), design

Branch: `native-vk` (from render-clean 9b270eb4). Reference viewer: `<work>\viewer-reference-mirror` (frozen, `run-viewer-reference.bat`).
Decided with Adam 2026-10-07: big-bang, Windows + Vulkan 1.3 + RTX only, not bound to the OpenGL look (cheaper equivalents welcome), goals: native Vulkan, GPU-focused, multi-threaded.

## What gets replaced (measured)
Raw GL calls left in the tree: about 440 in 8 files plus the Win32 context code.

| File | GL calls | Becomes |
|---|---|---|
| llglslshader.cpp | 94 | native program: compile through llvkshader (exists), uniforms by index into the program's block, no GL locations |
| llrender.cpp | 85 | state in a render context struct; immediate mode on native vertex scratch |
| llgl.cpp | 80 | caps from the Vulkan device, error/debug helpers, LLGLState as state writes |
| llimagegl.cpp | 72 | native images, uploads, mips, readback |
| llrendertarget.cpp | 42 | render target = images + attachments, no FBOs |
| llvertexbuffer.cpp | 40 | device-local arenas, CPU-visible staging, no client arrays |
| llshadermgr.cpp | 32 | text assembly stays, GL link/compile calls go |
| llpostprocess.cpp, cubemaps, misc | about 30 | follow the above |
| llwindowwin32.cpp | 25 | window without WGL |
Deleted at the end: `llvkgl*`, `llvkasync*`, the GL parts of `llvkmirror*`, `gen_llvkgl.py`, GL headers and the loader (about 10.2k lines).
Kept: `llvkcontext`, `llvkrenderer` (revised), `llvkshader`, `llvkdlss`, VMA.

## The one architectural rule: the main thread writes small typed records
Measured in this project: inline recording on the main thread gave 25 fps, the same work on a worker gave 60-78 fps. So the native layer is three parts:

1. **Front end (main thread): `RenderContext`.** What `gGL`, `LLGLState`, `LLTexUnit`, `LLVertexBuffer::setBuffer()` and `LLGLSLShader::uniform*()` write into. It holds the current state as plain values (program, blend/depth/raster as small state structs, texture bindings, uniform values, vertex buffer, render target). A draw call hashes what changed into **state objects with ids** (pipeline state, texture set, uniform block) and appends one POD `DrawCmd {programId, stateId, vertexBuf+offset, indexBuf+offset, first/count, textureSetId, uniformBlockId}` to the stream. No Vulkan calls, no GL names, no locks on the hot path.
2. **Command stream:** typed records (draw, clear, set target, copy, upload, query, present). Resource creation and destruction are commands too, so ordering with draws is by construction.
3. **Executor (render thread):** owns every Vulkan object and the `Renderer`. It resolves ids, builds descriptor sets and pipelines (cached), records, submits. Later this is where secondary command buffers and several recorders fit: the context struct has no globals, so each producer thread can have its own.

Handles: classes own their native objects. Where old code passes `U32` GL names around (`getTexture()`, `getGLBuffer()`, 114 files use texture units), the names become opaque ids into a handle table (index + generation) allocated by the native layer.

## Class by class
- `LLVertexBuffer`: CPU shadow memory behind the strider API as today (so 83 files do not change), upload on `unmapBuffer()` as a staging copy command into a device-local arena (suballocated, no per-buffer VkBuffer for small ones). `setBuffer()` records the binding in the context; draws append commands. Index type is 16 or 32 bit per buffer.
- `LLImageGL` / `LLTexUnit`: `LLImageGL` owns an image handle; format enums keep their GL numeric values as constants (GL_RGBA16F etc. appear in 36 files) and map to VkFormat in one table. Uploads are staging copy commands; mip generation by blits (exists). `LLTexUnit::bind` writes the context's texture table only.
- `LLRenderTarget`: images plus an attachment list; `bindTarget`/`flush` write the target stack in the context; clear is a command; no FBOs.
- `LLGLSLShader`: keeps the text assembly (`LLShaderMgr`), hands the final sources to `llvkshader`; `mUniform[]` holds an index into the program's uniform block; `uniform*()` writes the context's value block (compare-before-write as now); a draw references a deduplicated uniform block id.
- State (`LLGLState`, `LLGLEnable`, blend/depth/cull/scissor/colour mask, polygon mode, depth clamp, stencil): writes into the context state structs; the pipeline state object is derived at draw time.
- `LLRender` immediate mode (UI, debug drawing): batches into per-frame vertex scratch and appends draws through the same path.
- Queries, fences, readbacks (`readPixels`, snapshot, picking, texture save): occlusion/timestamp queries on query pools as now (results arrive frames later; the viewer already polls); readbacks are commands that return through a completion the caller waits on only where it must (snapshot, pick).
- Window/present: Win32 window without WGL, swapchain on the main HWND (the mirror already does this), resize handled in the executor.

## Bring-up order (big-bang, but with checkpoints)
M1 compiles and links with no GL headers in llrender; viewer starts, black window, log clean.
M2 login screen renders (UI shader, fonts, textures, render target stack for the UI).
M3 the world renders in the simplest path (sky, terrain, objects, avatars, water) with shadows off.
M4 every pass of the deferred pipeline: shadows, probes, lights, alpha, post, DLSS/DLAA. Compare against the reference viewer in the same spots.
M5 delete the mirror and GL leftovers; snapshot, pick, media, previews.
M6 profile; hand over to N2-N4 (pipelined frame, GPU scene, GPU-driven passes).
Each checkpoint is committed; the reference viewer is the comparison for M2-M5.

## Open to change (not bound to the GL look)
Candidates once M4 works, each judged by eye and by cost: temporal AA (DLAA/TAA) instead of FXAA/SMAA, simpler or cheaper post chain, lower-precision HDR formats (R11G11B10), cheaper SSAO or GTAO, fewer shadow samples with temporal filtering, drop legacy fixed-function remnants, one shader permutation set instead of the 209 programs where passes can be merged.

## Risks
- A broken viewer for the whole of M1-M3 (accepted: big-bang). The reference viewer keeps working.
- Thread-safety of the resource/handle tables (design above: ids allocated on the producer with atomics, objects created on the executor).
- Readback paths (snapshot, pick, texture save, previews) need real completions instead of GL's implicit sync.
- Media plugin textures (CEF) need external-memory interop: leave media off until M5.
