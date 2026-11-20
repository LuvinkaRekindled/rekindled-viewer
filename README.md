<img align="left" width="110" height="110" src="branding/rekindled/rekindled-icon.svg" alt="Rekindled Viewer icon"/>

# Rekindled Viewer

**Rekindled** is a viewer for Second Life and OpenSim worlds, built on **[Firestorm](https://www.firestormviewer.org)** ([Firestorm source](https://github.com/FirestormViewer/phoenix-firestorm)). Everything Firestorm gives you stays: its interface, RLVa, the radar, and the developer and debug tools. What changes is the engine underneath: a new renderer that talks to the graphics card through Vulkan only.

> **Status:** private development, not released. It has been built and tested on one Windows 11 machine with one graphics card (a GeForce RTX 5060 Ti). Other cards and platforms are untested. Please do not report problems with this viewer to the Firestorm project.

<br clear="left"/>

## What is different from Firestorm

- **Vulkan only, no OpenGL.** The old OpenGL drawing code is gone. A graphics card needs Vulkan 1.3 with descriptor indexing (bindless sampled images, at least 1024 slots), multi-draw indirect, indirect first instance and buffer device addresses. A card without them gets a plain message at start-up; there is no fallback path.
- **Draws come from scene tables, not draw lists.** Geometry, materials and textures live in persistent GPU tables addressed by bindless handles. The render worker draws from those rows with indirect draws and GPU occlusion culling, instead of the per-pass draw lists the main thread used to build every frame.
- **New shaders.** The stock shader set is replaced by native GLSL 460 programs, compiled to SPIR-V at start-up and cached on disk (see `doc/vulkan/NATIVE-SHADERS.md`).
- **Built for many cores.** A render thread records for the GPU, a shared worker pool does visibility, shadow preparation, avatar animation, palettes and texture work, and audio has its own thread.
- **Avatars in crowds.** Each avatar's pose is a job on the pool and overlaps with the rest of the update; hidden or far avatars update less often; rigged meshes that bake to the same data share one buffer; rigged draws are merged by memory block.
- **Optional ray tracing** on cards with ray tracing hardware: ray traced sun shadows with soft edges, and ray traced reflections. It is off by default.
- **Upscaling and anti-aliasing:** an upscaler that runs on any card (AMD FidelityFX FSR 3.1) and SMAA anti-aliasing.
- **Audio:** OpenAL Soft replaces FMOD, with optional HRTF.
- **Faster loading:** shader and pipeline caches, and a decoded texture disk cache with BC7 compression.

Measured numbers are in `doc/vulkan/GAINS.md`. They come from one machine, different scenes and different days, so they cannot be added up into one figure. The design notes and plans are the other files in `doc/vulkan/`; `doc/vulkan/PLAN-avatars-100.md` is the most recent.

## Versions

Rekindled has its own version, written like `0.99.01` (major.minor.patch, the minor and the patch with two digits), followed by a build number (the number of commits), and it shows the Firestorm version it is based on in parentheses: `0.99.01.1663 (Firestorm 7.2.5)`. The version is in `indra/newview/VIEWER_VERSION_REKINDLED.txt`; the Firestorm version it is based on stays in `indra/newview/VIEWER_VERSION_FS.txt`.

## Building

Rekindled builds like Firestorm does, with a few extra pieces. Start with Firestorm's own instructions for your platform:

- [Windows](doc/building_windows.md)
- [Mac](doc/building_macos.md)
- [Linux](doc/building_linux.md)

Windows is the only platform it is tested on.

Configure with the open-source configuration, then turn the Vulkan renderer on:

```
autobuild configure -c ReleaseFS_open -A 64
cmake . -DUSE_VULKAN:BOOL=ON -DVULKAN_SDK_DIR=<Vulkan SDK root>
autobuild build -A 64 -c ReleaseFS_open --no-configure
```

### What you need besides Firestorm's usual toolchain

| What | Needed for | Where from | How the build finds it |
|---|---|---|---|
| **Vulkan SDK** (LunarG), including the static glslang and SPIRV-Tools libraries | the renderer (required) | [vulkan.lunarg.com](https://vulkan.lunarg.com) | `-DVULKAN_SDK_DIR=<root>` or the `VULKAN_SDK` environment variable |
| **OpenAL Soft 1.24.3**, as an autobuild package | audio (required) | build it yourself from [OpenAL Soft](https://github.com/kcat/openal-soft) as a Windows x64 package | `autobuild.xml` names a local `file://` package (`openal-1.24.3-windows64-rk1.tar.zst`) that is not published: build your own and point that entry at it |
| **AMD FidelityFX SDK v1.1.4** (MIT), built to static libraries | the FSR upscaler (optional) | [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | Windows: `-DFFX_SDK_DIR=<root>`; the exact build flags are in `indra/llrender/CMakeLists.txt` and `doc/vulkan/PLAN-amd-denoiser.md`. Linux: the SDK's own build files are Windows only, so `indra/llrender/ffx-linux/` builds the libraries with GCC (its `CMakeLists.txt` explains it; `patch_sdk.py` changes the copy of the SDK it compiles) from a Windows build of the SDK's shaders, and `-DFFX_LINUX_DIR=<folder>` tells the viewer where they are. Without it the build still works, without FSR |
| **NVIDIA DLSS SDK** | the neural rendering option (local builds only, optional) | [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) | `-DREKINDLED_LOCAL_NVIDIA=ON -DDLSS_SDK_DIR=<root>`. Never part of a release build; you take NVIDIA's terms on yourself |

The defaults of `DLSS_SDK_DIR` and `FFX_SDK_DIR` in `indra/llrender/CMakeLists.txt`, and the OpenAL package URL in `autobuild.xml`, are folders on the original build machine: set them for yours. A few developer-only diagnostics in the code (live debug keys, crash and trace logs, dumps) also read or write fixed paths on that machine; they are not needed to build or run the viewer, and the notes under `doc/` name them as `<work>`.

This repository has been built from a working tree with local copies of the SDKs above. A from-scratch build on another machine has not been tried, so expect to find gaps in these notes.

## Running

The viewer always draws with Vulkan. The `RK_VK_*` environment variables and the `RK*` settings are developer switches for diagnostics and measuring; the defaults are what the viewer is meant to run with.

## License

Firestorm and the Second Life viewer it derives from are available under the GNU Lesser General Public License, version 2.1 (see `LICENSE`). Third-party components keep their own licenses. Rekindled is an independent derivative and is not an official Firestorm or Linden Lab product. See the [Third-Party Viewer Policy](https://secondlife.com/corporate/third-party-viewers) for the rules that apply to viewers for Second Life.
