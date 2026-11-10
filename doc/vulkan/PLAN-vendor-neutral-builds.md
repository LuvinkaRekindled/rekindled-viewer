# Plan: vendor-neutral builds (kept together, decision later) - 2026-11-10

Adam's goal: move away from NVIDIA proprietary libraries while keeping equivalent features, and keep the option of shipping with them. Decision on what the first beta ships is **open**; this note holds the plan so the choice is a switch, not a rewrite. See also `PLAN-amd-fsr-sdk2.md` stage E (denoiser possibilities), `NVIDIA-EMAIL-DRAFT.md`, `NVIDIA-MARKS-SAMPLE.md`, `BETA-FEATURES.md`.

## What depends on the NVIDIA SDK today (read from the code, 2026-11-10)

- `indra/llrender/CMakeLists.txt` (line ~173): `DLSS_SDK_DIR` (default `<work>/DLSS/DLSS`); when `include/nvsdk_ngx_vk.h` and `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib` exist, it adds the include path, defines `LL_DLSS=1` and **links `nvsdk_ngx_d.lib` statically into the viewer**. Without them `LL_DLSS` is undefined and `llvkdlss.cpp` / `llvkdlssnr.cpp` compile to stubs that report "not available".
- DLSS super resolution, DLAA and ray reconstruction use `nvngx_dlss.dll` and `nvngx_dlssd.dll` (loaded at run time through the NGX library, not shipped by any packaging step yet).
- **DLSS-NR is inside the same `#if defined(LL_DLSS)`** and includes the NGX headers (`llvkdlssnr.cpp`). So NR is not independent of the SDK: no SDK at build time means no NR code at all.
- FSR 3.1 (`llvkfsr.cpp`, `LL_FSR`) needs only AMD's ffx-api headers; `amd_fidelityfx_vk.dll` is loaded at run time, nothing linked. Vulkan device extensions with NVIDIA names (`VK_NV_ray_tracing_invocation_reorder`, cluster acceleration, diagnostics) are optional driver features in `llvkcontext.cpp`, not libraries.

## Replacements (equivalent features)

| NVIDIA feature | Replacement | State |
| --- | --- | --- |
| DLSS Super Resolution | FSR 3.1 (MIT SDK) | in, tested on the RTX card |
| DLAA | FSR at a 1.0 ratio (FSR 3.1's API has a native-AA mode; our `llvkfsr` accepts any render and output size) | not checked that the "DLAA (100%)" entry of the mode combo reaches FSR |
| Ray reconstruction | a vendor-neutral denoiser: the classic AMD FidelityFX Denoiser 1.3 shadow part first, or our own SVGF (`PLAN-amd-fsr-sdk2.md` stage E) | not started; unmeasured |
| DLSS-NR | none; stays an optional, never-shipped extra | see variant B |
| Frame generation | left out | - |

## Build variants (switches, decide later)

- **A. No NVIDIA SDK in the build** (`LL_DLSS` undefined). FSR 3.1 + our own RT shadow accumulation. No NVIDIA code in the executable, no NVIDIA DLLs, no NVIDIA licence duties; no DLSS, no RR, **no NR code**. Needs only the configure step without `DLSS_SDK_DIR` content (or an explicit option, below). Not built or tested yet.
- **B. SDK linked, no NVIDIA DLLs shipped.** `LL_DLSS` defined, `nvsdk_ngx_d.lib` in the executable; the installer ships none of `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssnr.dll`. Users who drop in their own DLLs get DLSS and NR (NR stays grayed out until its DLL is beside the viewer, already coded). **This does not remove the licence duties:** the SDK's static library is object code inside the executable, so section 1(c)/2 of the licence still applies (credit, the marks in the About box, end-user terms, the notification, the open-source clause 4(e)). It only avoids redistributing the DLLs. Reading of the licence, not legal advice.
- **C. SDK linked and the DLLs shipped** (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, never the NR one). Full DLSS out of the box; all the NVIDIA steps in `NVIDIA-EMAIL-DRAFT.md` apply.

## The switches (small; not built yet)

1. CMake: an explicit `USE_NVIDIA_NGX` option (ON/OFF) instead of "the directory happens to exist", so a build says what it is; OFF skips the SDK lookup and never defines `LL_DLSS`. Keep `DLSS_SDK_DIR` for ON and document that source builders download the SDK from NVIDIA's public repository (the repository must never contain it).
2. Packaging: `viewer_manifest.py` ships `nvngx_dlss.dll` and `nvngx_dlssd.dll` (the SDK's `rel` folder, unmodified) only for variant C, and never `nvngx_dlssnr.dll`.
3. About / Licenses: the NVIDIA credit and user terms (`rekindled-licenses.txt`) are present for B and C, left out for A.
4. UI: with `LL_DLSS` undefined the DLSS entries and the ray reconstruction box must be hidden or grayed and the neural rendering row absent (check: the stubs report "not available"; the Vulkan panel's labels are static XML today).
5. DLAA through FSR: make the "100%" mode work with the FSR backend (render size = output size) so FSR users have the equivalent.

## Ray tracing must not need an NVIDIA card (Adam, 2026-11-10: remove the RTX requirement)

Facts from the code: ray tracing is our own Vulkan ray query on the cross-vendor extensions (`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, buffer device address; `rt.mUsable` in `llvkcontext.cpp:865`). The NVIDIA ray tracing extensions (invocation reorder, cluster acceleration structures, opacity micromaps) are queried but forced off at device creation (`llvkcontext.cpp:932`) and used nowhere else. The vendor id (`llvkbridge.cpp:302-317`) only picks the strings of the GL-style vendor and version report. No code gates ray tracing on the vendor. It has only run on one GeForce RTX card.

To do (not done):
- Reword the preference (`panel_preferences_graphics1.xml:2049, 2053`): the label "Use ray tracing (RTX graphics cards, experimental)" and the tooltip "Needs an NVIDIA RTX graphics card" become "needs a graphics card with Vulkan ray tracing support" (any vendor).
- Reword the other text that implies it: the DLSS upscaler tooltip (line 1922) is correct as it stands (it says DLSS is for RTX cards and FSR for any), the neural rendering one stays NVIDIA-specific; the About page on the site and any release text say "ray tracing hardware", not "RTX".
- Check, and let an AMD or Intel tester check (stage C of `PLAN-amd-fsr-sdk2.md`): the viewer must enable the setting on any device with `rt.mUsable`, whatever the vendor; look at the code that decides when the checkbox is enabled and at the device-creation path for the three vendor structs.
- Keep it off the NVIDIA-only list in `BETA-FEATURES.md`: say "cards with ray tracing hardware" there (already the wording).

## Checks before deciding

- Build variant A once and run it: FSR on, RT shadows on and off, snapshots, no NVIDIA strings in the log, the Vulkan panel readable.
- Confirm DLAA-through-FSR looks right and costs what is expected.
- Measure a denoiser candidate (stage E) against RR on the shadows before calling it equivalent.
- Review `llvkdlssnr.*`, `NOTES-dlss-nr.md` and `PLAN-dlss-nr.md` before the repository goes public (they describe a non-public NVIDIA interface).
- Ask a lawyer to read the licence for variant B (static library but no DLLs) if it is the one chosen.
