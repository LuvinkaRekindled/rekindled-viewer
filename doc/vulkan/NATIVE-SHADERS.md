# Native shaders: how to add and test one (slice 0 of PLAN-shaders-rewrite.md, 2026-11-08)

Code: `indra/llrender/llvknative.h/.cpp` (loader, manifest, swap at the draw), `Program::adoptFrom` and the specialization-constant support in `llvkrenderer.cpp`, the draw hook in `draw_context_apply` (`llvkworker.cpp`). Shaders: `indra/newview/app_settings/shaders/native/`.

## Writing a program
- Plain Vulkan GLSL 460. The loader puts `#version 460`, the include extension, `LL_VERTEX_STAGE` / `LL_FRAGMENT_STAGE` and the manifest's defines in front of `native/ll_native.glsl` (the bindless tables and `LL_HM`), then your file. Do not write `#version`.
- Per-pass values: `layout(std140, set = 0, binding = 0) uniform LLLooseUniforms { ... };`. Per-draw values: `binding = 5` `LLLooseDraw` (bindings 6-8: LLLooseMaterial / LLLooseLight / LLLooseSkin). A member `uint ll_ti_<name>` in the draw block is the texture slot of a sampler `<name>` (`ll_ti_cube_<name>`: a cube map); read it with `texture(ll_tbl2d[LL_HM(ll_ti_name)], uv)`.
- Vertex inputs / fragment outputs need explicit `layout(location = n)` on the same line as `in` / `out` and the type (the loader reads them from the text); n = index in the viewer's reserved attribute list (position = 0). Vulkan depth (z in [0, w]), no GL remap.
- Keep programs small and share code through `#include "lib/x.glsl"` (paths from the native directory). The loader logs SPIR-V words and compile ms and warns above 20000 words / 150 ms.
- The names of the members must match the old program's uniforms (and sampler names) while the old C++ callers still set them: the swap copies values by name from the program being replaced. A name the old program does not have simply stays zero.

## Putting it in the picture
- `native/manifest.txt`: `override | <name of the viewer program> | <vertex file> | <fragment file> | <family> [| define NAME=VALUE] [| spec ID=VALUE]`.
- The family is the live key `native_<family>=1` in `<work>\rt-debug.txt` (0 or missing = the old program); `native_reload=0` stops the file watching.
- `LL_VK_NATIVE_DIR=<repo>/indra/newview/app_settings/shaders/native` makes the viewer read the source directory instead of the packaged copy: edit a shader, save, and it is rebuilt within a second (the old program draws until the new one compiles; a failed compile keeps the old one and is retried on the next save). Lap mode `nativeab` in `<work>\h6-fixed.ps1` sets it.

## Testing old against new
- `powershell -File <work>\native-ab.ps1 -Name <lap name> -Family <family> -Seconds 25 -Pairs 3` (start the lap `h6-fixed.ps1 -Mode nativeab -Prefix <p>-` in the background first): alternates the family 0 / 1 in 25 s windows and writes `<name>.keys.txt`; read GPU ms per window with the zone timers (`H6_EXTRA_ARGS='--set,FSGpuZoneTimers,1'`) as with `keycycle.ps1`.
- `python <work>\imgdiff.py a.png b.png [--crop x0,y0,x1,y1] [--out diff.png]`: mean error, max, PSNR, share of pixels off by more than 2/255. Needs a still camera; moving avatars, clouds and DLSS jitter set a noise floor (two windows of the SAME key differ by about 4/255 mean on the plaza), so compare against old-vs-old.
- Per-pass compare (run old and new into two targets in one frame and diff on the GPU): not built yet; it comes with the first family that writes a full target (the lighting resolve).

## Status (2026-11-08)
First program: `Copy Shader` (the exposure history copy, drawn every frame): matches the old one by eye and by frame time (6.1 ms both), 50 ms first compile, hot reload tested (edit and revert both rebuilt). Specialization constants are implemented (`spec ID=VALUE`) but not exercised yet. The per-frame data block (camera, sun, sky, exposure) is not built: slice 1 defines what the lighting resolve needs and builds it then.
