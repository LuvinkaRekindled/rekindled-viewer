# FSR 3.1 upscaler as a second backend (plan, 2026-10-27)

Adam's decision: add an AMD upscaler beside DLSS so an AMD (or any non-RTX) card can run the renderer with the ray traced sun shadows. Nobody has an AMD card to test on yet; FSR 3.1 runs on any GPU, so most of it can be tested on the NVIDIA machine with a switch.

## What the SDK allows (checked 2026-10-27)
- AMD FSR SDK 2.3.0 ("Redstone": FSR 4.1.1 ML upscaling, Ray Regeneration 1.2.0, ML frame generation 4.0.1) is DirectX 12 only. `Kits/FidelityFX` has `backend/dx12` and nothing else; its known issues say "Vulkan is currently not supported in AMD FSR SDK 2.3". Ray Regeneration is RX 9000 only.
- FidelityFX SDK **v1.1.4** (tag, 2025-05-08) is the last release with Vulkan. FSR 3.1 upscaling works through the **ffx-api** (C API: `ffxCreateContext`, `ffxDispatch`, `ffxQuery`, `ffxDestroyContext`) with `amd_fidelityfx_vk.dll` (prebuilt, signed). Downloaded (minimal package, 449 MB) and unpacked to `<work>\FidelityFX-SDK-1.1.4` (not in the repo): `ffx-api/include/ffx_api/` (headers, `vk/ffx_api_vk.h`), `PrebuiltSignedDLL/amd_fidelityfx_vk.{dll,lib}`, `sdk/` (source of the backend), `samples/fsrapi/fsrapirendermodule.cpp` (the reference for create / dispatch).
- When AMD ships Vulkan in a later SDK the same ffx-api calls should carry over (2.x uses the same API style); keep the FSR code in one file.
- Out of scope: FSR frame generation (needs its swapchain replacement and frame pacing), Ray Regeneration and FSR 4 (no Vulkan). With FSR the RT shadows and reflections use our own temporal accumulation, as the DLSS-without-RR path already does.

## What carries over (the hard part is done)
The renderer already makes everything a temporal upscaler wants: render-size colour, depth, motion vectors (surface, avatars, reflections), a jittered projection with a Halton sequence of 8*(window/render)^2 phases (`LLPipeline::updateJitter`, the same count FSR recommends), a render scale (`getDLSSScale`: 0.5 / 0.58 / 0.67 / 1.0 = FSR's 2.0 / 1.7 / 1.5 / 1.0 ratios), texture LOD bias, a window-size output the tonemap/CAS/UI run on, and the RT shadow history that expects an upscaler to average the rest. The RT code only needs KHR extensions; the NVIDIA ones (`VK_NV_ray_tracing_invocation_reorder`, cluster AS, checkpoints) are probed and optional (`rkvkcontext.cpp`).

## What is NVIDIA specific and has to change
- `rkvkdlss.cpp` (NGX): the only place that calls DLSS. All callers go through `RKVkDlss::` (`rkvkworker.cpp` 20 sites, `rkvkcontext.cpp` 8, `rkvkbridge.cpp` 5, `rkvkrenderer.cpp` 2): `requested / addInstanceExtensions / addDeviceExtensions / init / ready / shutdown / evaluate / inputLayout / outputLayout / lodBias / setLodBias / rayReconstructionAvailable`, with one `Params` struct (`rkvkdlss.h`).
- Ray reconstruction (guides pass, hit distance, `rayReconstructionActive`) stays DLSS-only; with FSR `rayReconstructionAvailable()` is false and the existing non-RR path runs.
- Names say DLSS (`sDLSS`, `RKVkDLSS*`, `getDLSSScale`, 18 uses of `sDLSS` in newview). Keep the names for now: `sDLSS` means "an upscaler is active".

## Design
1. `rkvkupscale.{h,cpp}`: a facade with the same functions as `RKVkDlss` (and the same `Params`), choosing the backend at start-up: `RKVkUpscaler` setting (0 auto: DLSS on an RTX card that has it, else FSR; 1 DLSS; 2 FSR) and `RK_VK_UPSCALER=fsr|dlss` for testing FSR on the NVIDIA machine. The callers outside `rkvkdlss.cpp` change from `RKVkDlss::` to `RKVkUpscale::`; `rkvkdlss.cpp` stays as it is.
2. `rkvkfsr.{h,cpp}`: the FSR backend. `amd_fidelityfx_vk.dll` is loaded with `LoadLibrary` (no link-time dependency: the viewer starts without it and reports "FSR not available"); entry points from the ffx-api loader. Backend context = `ffxCreateBackendVKDesc{vkDevice, vkPhysicalDevice, vkDeviceProcAddr}` chained to `ffxCreateContextDescUpscale{flags, maxRenderSize, maxUpscaleSize}` (flags: HDR when `mHdr`, DEPTH_INVERTED if our depth is, MOTION_VECTORS_JITTER_CANCELLATION as needed; no AUTO_EXPOSURE, we pass `preExposure`). Context made on first use and when the sizes change (parked like the DLSS features). `evaluate` fills `ffxDispatchDescUpscale` from `Params` (colour, depth, motion, output; jitter in pixels; `motionVectorScale`; render/upscale size; reset; camera near/far/fov from the caller; frame time) and calls `ffxDispatch` with the command buffer.
3. `Params` grows what FSR needs and DLSS ignores: camera near/far/vertical fov, frame time (ms), pre-exposure. Filled at the one site that builds `Params` (`rkvkworker.cpp` ~10787).
4. Resource states: ffx takes the state each resource is in (`FFX_API_RESOURCE_STATE_*`) and records its own barriers. `inputLayout / outputLayout` must report the layouts that match the states we declare (read the Vulkan backend's state to layout table in `sdk/src/backends/vk/ffx_vk.cpp` before settling).
5. Device requirements: the backend looks for optional features (subgroup size control, shaderFloat16, synchronization2, descriptor indexing, ...) among the enabled device extensions. Enable the ones the device has (`addDeviceExtensions`), as long as ours do not already.
6. LOD bias: FSR recommends `log2(render/display) - 1`; DLSS uses `log2(render/display)`. The backend supplies its own (`lodBias`).
7. Packaging: `amd_fidelityfx_vk.dll` is copied beside the exe (MIT licence, notice in the licence list); `viewer_manifest.py` copies it when present; the `<work>` build copies it for local runs.

## Steps
1. Facade + FSR backend compiled, `RK_VK_UPSCALER=fsr` on the NVIDIA machine, plain upscaling working (picture, motion, jitter, depth, HDR flag, reset on camera cut).
2. Quality modes, texture LOD bias, resize / window changes (context re-made), snapshots (the extra renders), the overlays that read the scene depth after the upscale.
3. Compare FSR against DLSS on the same scene (same-lap runs, `h6-fixed.ps1`), RT shadows on, and write down what the AMD tester should check.
4. Packaging and the setting in the preferences floater, then a beta build for an AMD tester.

## Risks
- ffx VK backend wants features the Vulkan device here does not enable: found at step 1 from the log.
- Layout / state mismatches show up as validation errors or a black picture: the validation layer run is part of step 1.
- On a non-NVIDIA card the first time anything of the RT path runs is the beta: Adam's call whether the first AMD build gets RT on by default.

## Status 2026-10-27
- Step 1 done and run on the NVIDIA machine (`RK_VK_UPSCALER=fsr`, `<work>\run-viewer-fsr.bat`): FSR context created (1713x898 -> 2558x1341, formats colour RGBA8 / depth D32 / motion RG16F), fps as plain DLSS (about 110 fps), and Adam sees LESS ghosting than plain DLSS with RR off (the fair upscaler against upscaler comparison; by eye, no same-lap capture). RR is not active with FSR: `rayReconstructionAvailable()` is false, the RR guides are not allocated, our own RT shadow accumulation runs). First run crashed in `ffxCreateContext`: the backend calls `vkGetBufferMemoryRequirements2KHR` unchecked, so `VK_KHR_get_memory_requirements2` / `VK_KHR_dedicated_allocation` (and `VK_AMD_buffer_marker` where present) are enabled on the device when FSR is a possible backend (`RKVkFsr::addDeviceExtensions`).
- Not measured: FSR against plain DLSS (RR off) on the same lap (fps, ghosting captures); the ghosting verdict so far is Adam's eye.
- `RKVkUpscaler` (0 automatic, 1 DLSS, 2 FSR; restart) with a combo in the graphics preferences and the `RK_VK_UPSCALER` translation in `llappviewer.cpp`: written, built and tested next.
- The upscaler DLLs (`nvngx_dlss*.dll`, `amd_fidelityfx_vk.dll`) are copied by hand into Release; neither the manifest nor CMake packages them. Packaging is open for both.
