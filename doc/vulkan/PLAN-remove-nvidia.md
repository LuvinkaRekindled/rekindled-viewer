# Removing NVIDIA from the viewer (plan, 2026-11-14)

Adam, 2026-11-14: "let's get rid of nvidia, but leave NR only for local builds, not releases." Earlier goal (2026-11-10, `project-away-from-nvidia-proprietary`): no NVIDIA proprietary library in a release, so no NVIDIA licence duties (marks, notification text, end-user terms, the 4(e) LGPL question) and one vendor-neutral feature set. FSR 3.1 is the upscaler and AA ("on par with DLSS", Adam 2026-11-14); the AMD shadow denoiser is on by default with FSR; the reflection denoiser is parked (it never touches the planar mirror).

## End state

- A **release build** contains no NVIDIA SDK code, no `nvngx_*.dll`, no NVIDIA text in the licence file or the preferences, no DLSS / DLAA / ray reconstruction setting or code path.
- A **local build** can still switch on DLSS-NR (the neural renderer, `llvkdlssnr`) when the DLSS SDK is on this machine: a CMake option, off by default, forced off by the package script. Nothing else of NVIDIA's stays.
- The upscaler facade (`llvkupscale`) has one backend (FSR). `sDLSS` in `pipeline.cpp` is really "an upscaler pipeline is active" and gets a vendor-neutral name.

## State of the AMD side (what replaces it)

1. **FSR from source, no DLL** (this plan's step 0, 2026-11-14): `llvkfsr.cpp` uses the native FidelityFX API (`ffx_fsr3upscaler_x64.lib` + `ffx_backend_vk_x64.lib`, MIT, SDK 1.1.4, built by `<work>\ffx-build-fsr3.bat`), one backend interface and scratch memory per context (as the SDK's own ffx-api provider does), the three shared resources made through the backend. `amd_fidelityfx_vk.dll` is no longer loaded or needed. Built; needs a lap and Adam's eye (picture, fps against the DLL version, `LL_VK_UPSCALER=fsr`).
2. Shadow denoiser: done (on by default with FSR). Once DLSS is gone its `upscalerIsFsr()` condition is always true.

## Steps (each one a build, a lap, a commit)

1. **FSR static** (above). Then remove `amd_fidelityfx_vk.dll` from the Release folder copy list if any script copies it.
2. **One upscaler.** `FSVkUpscaler` and the "DLSS / DLAA" entries of the preference combo go; the quality ladder stays (FSR modes, 100 % = native AA). `LLVkUpscale` loses the DLSS backend choice. `LL_VK_UPSCALER=dlss` goes.
3. **Ray reconstruction out.** `sRR`, `mRR*` targets (50 sites in `pipeline.cpp`), `gRRGuidesProgram` / `gRRReflMotionProgram` and the native shaders `rr_guides.frag`, `rr_refl_motion.frag` (+ manifest lines), `rayReconstructionActive`, the "denoise first" order, the RR checkbox. Our own ray shadow accumulation (`mShadowAccum`) and the AMD shadow denoiser stay. The traced glossy reflection stays undenoised (the AMD reflection denoiser can come back later: parked, code kept).
4. **DLSS SR backend out.** `llvkdlss.cpp` (618 lines) and its `Params` use: `LLVkDlss::Params` is the shared parameter struct of both backends and moves to `llvkupscale.h`; NGX initialisation that NR also needs is split out first (see step 5). `LL_DLSS`, `DLSS_SDK_DIR`, `nvngx_dlss.dll`, `nvngx_dlssd.dll` leave the CMake files and the package script.
5. **NR local only.** NR (`llvkdlssnr.cpp`) needs the NGX loader and the `nvngx_dlssnr.dll`. Keep exactly that: a CMake option `REKINDLED_LOCAL_NR` (default OFF; the package script passes OFF and checks the exe has no `nvngx` string), compiled only with the DLSS SDK present. The setting `FSVkNeuralRenderMode` and its UI row exist only in a build with the option. A release has none of it.
6. **Text and packaging.** NVIDIA sections of `rekindled-licenses.txt`, the preference strings, `doc/vulkan/NVIDIA-EMAIL-DRAFT.md` and `NVIDIA-MARKS-SAMPLE.md` (kept as history, marked obsolete), the installer's file list.
7. **Other NVIDIA pieces to decide with Adam:** Aftermath (crash dump SDK, `<work>\aftermath-sdk`, a diagnostic for device lost: keep local-only like NR?), `NVAPI.cmake` (Firestorm's own, outside the Vulkan renderer), the "NB Aftermath" sky preset (just a name).
8. **A whole-tree check:** `grep -ri "nvidia\|dlss\|ngx\|nvngx"` over `indra` and the packaged exe's strings, a release-style build with the option off, a lap, Adam's live look.

## Not changing

Hardware floor and everything GPU-vendor-neutral (Vulkan 1.3, bindless, MDI), the NVIDIA driver itself (the viewer runs on it), `nvoglv64.dll` showing in crash stacks.
