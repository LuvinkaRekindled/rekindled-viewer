# Linux build in WSL (2026-11-02)

Adam: "we should be able to do linux builds with the windows linux layer right?" Yes: WSL2 Ubuntu 26.04 (gcc-14 like the CI, cmake, autobuild in a venv). The Windows tree is copied into WSL and built there (building on /mnt/d is very slow).

## Environment (in `<work>`, not in the repo)
- `wsl-sync-src.sh` copies the Windows working tree to `~/rekindled` (no `.git`, no build dirs); run it before every Linux build.
- `wsl-autobuild.sh <args>` runs autobuild with the venv and the Firestorm build variables; `wsl-cmake-config.sh [-D...]` re-runs cmake with autobuild on the PATH (a bare `cmake .` loses `AUTOBUILD_EXECUTABLE`); `wsl-cmake-build.sh <targets>` builds (`KEEP_GOING=1` for make -k, `JOBS=<n>`, default 10).
- Use `JOBS=10` or less: 32 parallel compiles of the huge renderer sources crashed the WSL VM (out of memory) and left empty `.o` files; after a crash delete them (`find build-linux-x86_64 -name '*.o' -size -1k -delete`).
- First configure: `autobuild configure -c ReleaseFS_open -A 64`, then `cmake . -DUSE_VULKAN:BOOL=ON` (the native renderer needs it).

## What the port changed
- `rkvkplatform.h` (llrender): Windows includes `<windows.h>` / `<intrin.h>`; Linux gets the calls the renderer uses with the same names (performance counter, Sleep, thread ids, futex based WaitOnAddress, CreateThread over pthreads, LoadLibrary / GetProcAddress, VirtualAlloc, CaptureStackBackTrace, the MSVC bit intrinsics, the TEB thread id).
- Windows-only diagnostics are `#if LL_WINDOWS` with Linux stubs: SEH crash filter and DbgHelp stacks (`rkvkasync.cpp`, `rkvkcontext.cpp`), the process dump, the vectored exception handler, the hitch watcher / sampler / growth probe (`rkgpuzonetimer.cpp`), the SEH read in `lldrawable.cpp`.
- Vulkan on Linux: `llrender/CMakeLists.txt` takes the system headers (libvulkan-dev) and static glslang / SPIR-V (glslang-dev, spirv-tools); the surface and its instance extensions come from SDL (`SDL_Vulkan_*`), `LLVk::windowClientSize` replaces `GetClientRect`.
- `llwindowsdl2.cpp` / `.h`: a Vulkan window (no GL attributes, no GL context), the main window handed to `LLVkBridge`, no shared contexts, vsync and swap are the swapchain's (as in the Windows window); Xlib's `None` macro is undefined after SDL's headers (`LL_X11_NONE`), because the renderer enums use `None`.
- GCC strictness fixes (-Werror): memcpy / memset to non-trivial types, unused results, `std::move` on return, a few more; `LLGpuZoneHooks` is defined for every profiler configuration; one stray carriage return inside a comment of `llappviewer.cpp` (a "\r" written as a character) turned the next text into code for GCC.
- `viewer_manifest.py` Linux: the executable is `rekindled-bin` (installed as `do-not-directly-run-firestorm-bin`, the name the wrapper and desktop files use).

## State
Everything compiles and links, `copy_l_viewer_manifest` stages `newview/packaged` (3.9 GB) in `~/rekindled/build-linux-x86_64`. **Nothing has been run on Linux.** The SDL Vulkan window and surface, the futex wait, the pthread worker are new code. No DLSS or FSR path on Linux (DLSS has a Linux SDK, FSR loads a Windows DLL), no VLC plugin target (the stream device message is ignored), the crash and hitch diagnostics do nothing, Mac is untouched.

## Next
Run it under WSLg (`packaged/bin/do-not-directly-run-firestorm-bin` through the wrapper) and read the log: loader and surface creation, device pick, first frame. Then DLSS / FSR on Linux, a Linux crash handler, and the Mac manifest (`firestorm` naming in `viewer_manifest.py`).

## Saved for later (Adam, 2026-11-03): a release build, then run it through WSL
- **Release build:** the Linux build above is the ReleaseFS_open configuration with `-g`, so the executable is 3.2 GB (2.9 GB of it DWARF; the code is about 95 MB). A release build goes through `--package` (viewer_manifest strips the binaries) or `strip -S` after splitting the debug info with `objcopy --only-keep-debug` into a symbol file; expect about 100 MB for the executable and a few hundred MB for the tree.
- **Running it in WSL:** WSLg works here (`DISPLAY=:0`, Wayland, a Pulse bridge for OpenAL). The only Vulkan device is lavapipe (`libvulkan_lvp.so`, software, CPU): there is no Vulkan driver over the D3D12 passthrough and NVIDIA ships none for WSL. Expect a few frames a second at best; the point is the startup log: loader, SDL surface creation, device pick (does lavapipe offer what the renderer insists on: Vulkan 1.3, descriptor indexing, timeline semaphores, buffer device address ...), shader compile, first frames. Launch `packaged/bin/do-not-directly-run-firestorm-bin` through the wrapper, with `LL_VK_VALIDATION=1` when a validation layer is installed (`vulkan-validationlayers`); read `~/.firestorm_x64/logs`-style log of the Rekindled settings directory.
- A real GPU run needs a Linux machine or a dual boot (real performance, DLSS / FSR on Linux).
