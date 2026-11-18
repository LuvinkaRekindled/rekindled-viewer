# F5: what is left of OpenGL (measured and done, 2026-10-20)

`PLAN-gpu-scene.md` planned F5 as "delete `rkvkgl*`, `rkvkasync*`, the GL parts of `rkvkmirror*`, `gen_rkvkgl.py`, the GL stubs (about 10k lines)". That described the state of 2026-10-10 (a GL call hook layer replayed on the render thread). Measured after F4: **those files are not GL any more**. `gen_rkvkgl.py` and the hook layer went with F2.1; `rkvkgl.cpp` (6k lines) is the render thread's resource tables and handlers for the typed command records (textures, buffers, targets, queries, packets: `RKVkGL::tex_*`, `read_pixels` ...), `rkvkasync.cpp` is the typed command stream, `rkvkmirror.cpp` the front-door state of the renderer. Their GL tokens and calls were 0 before F5; only the *names* (`RKVkGL`, `RKVkMirror`, `sGL`, the file names) come from the design they replaced. They cannot be deleted: they are the executor.

## What F5 did (this commit)
- **`llglheaders.h` is no longer a GL header.** It was 1,098 lines: the system `gl.h` / `glext.h` / `glu.h`, ~900 lines of extension function pointers and `PFN*PROC` externs (WGL), the macOS / Mesa variants, EXT fallbacks: all unused since F3.6g. It is now 20 lines: `llwin32headers.h` on Windows (what the GL headers used to bring in transitively) and `rkvkgl.h` (the render thread's shared header, which every includer got through it).
- The dead extension pointer declarations in `llviewerjointmesh.cpp` (`glWeightPointerARB` ...).
- The F3.6e/f **migration table check** (`RKVkFormat::verifyTables`: every GL number the old code used against the typed value; 112 GL tokens): its job was done by the baseline-vs-migrated runs, the decoders and `noteFormat / noteKey` (the `RK_VK_FORMAT_VERIFY` site log) stay.
- **Media plugins**: they got `GL_RGB / GL_BGRA / GL_UNSIGNED_BYTE ...` for the texture format messages from `llgl.h`. The protocol values are now named in `indra/llplugin/rkpluginmediaformat.h` (`WIRE_RGB` ... the same numbers on the wire) and the plugins (`cef`, `libvlc`, `example`, `gstreamer010`, `gstreamer10`) include that and no GL / render header; the viewer still decodes them in `llviewermedia.cpp` (`texelFormatFromGL` ...).
- **GLOD** (`llmodelpreview.cpp`): the mesh simplification library names GL types in its interface; the file includes the system `<GL/gl.h>` itself before `glod/glod.h`: the one place that includes a GL header.
- `glReady` (a window thread "window created" signal in `llwindowwin32.cpp`, nothing GL) became `windowReady`.

## Gate (`<work>\f36g-gate.py` extended to the whole tree: `GL_*`, `GL*` types, `LLGL*` types, `gl*()` calls, `wgl*`, `PFN*PROC`; comments and string literals stripped; test directories left out)
Whole `indra/`: **45 hits: GLOD 9 (`llmodelpreview.cpp`), the SDL / SDL2 / Mesa-headless / macOS window code 36 (platform code of operating systems this port does not build; they need their own GL context code and are left alone), the Windows window 0.** Windows code path: GLOD only.
Beach runs `f5a1-beach` and `f5a2-beach` (builds without any GL header; the second with the `windowReady` rename, identical results): the `FormatVerify` set is the F3.6f baseline set, bindTarget 37.1, clear 16.0, worker passes 54.1, 0 PassCheck / StateGuard / MatrixGuard warnings, no `unknown command`, same warning categories; picture judged by eye (`f5a1-beach-b.png`: complete). Not run: the media plugin processes (CEF loads the login page: the login screen picture `f5a2-beach-login.png` is the page, which shows that the plugin's BGRA frames still arrive; judged by eye: complete).

## GL-named layers that remain (a naming decision, not GL): sizes measured
| Name | Uses | What it is now |
|---|---|---|
| `gGL` (`LLRender`) | 3,750 uses | the render front end: immediate mode, matrix / state / texture unit helpers |
| `LLImageGL`, `LLGLTexture` | 294 / more | the viewer's texture object (a `TextureId` and its settings) |
| `LLGLManager` / `gGLManager` | many | device capabilities (filled from Vulkan since `initGLNoGL`) |
| `LLGLSLShader`, `LLGLSLShader::mProgramObject` | many | the viewer's shader program object (GLSL source compiled to SPIR-V) |
| `RKVkGL` namespace, `rkvkgl.cpp / .h`, `RKVkMirror`, `sGL`, `RKVkGL::gMutex` | 167 / 484 | the render thread's tables and handlers (see above) |
| `LLGLUpdate`, `LLGLSquashToFarClip`, `LLGLState`-era files `llglstates.h`, `llgl.h` (`init_glstates`, `parse_gl_version`) | | helpers, no GL |
Renaming them is mechanical (thousands of sites in 145+ files, one script per name) and does not change behaviour; it is **not done here**: it needs Adam's decision on the names and a build / hand test cycle per rename.
