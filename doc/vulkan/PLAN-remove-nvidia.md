# Removing NVIDIA from the viewer (plan, 2026-11-14)

Adam, 2026-11-14: "let's get rid of nvidia, but leave NR only for local builds, not releases." Earlier goal (2026-11-10, `project-away-from-nvidia-proprietary`): no NVIDIA proprietary library in a release, so no NVIDIA licence duties (marks, notification text, end-user terms, the 4(e) LGPL question) and one vendor-neutral feature set. FSR 3.1 is the upscaler and AA ("on par with DLSS", Adam 2026-11-14); the AMD shadow denoiser is on by default with FSR; the reflection denoiser is parked (it never touches the planar mirror).

## End state (Adam, 2026-11-14: "if people want to build with it they can, but i won't release with any nvidia active. that should avoid their licensing")

- A **release** has no NVIDIA code or DLL active: no DLSS, no ray reconstruction, no NGX, no `nvngx_*.dll`, no NVIDIA diagnostics, no NVIDIA text in the licence file or the preferences. The source tree contains no NVIDIA SDK files either (the DLSS SDK lives outside it).
- **One opt-in for builders:** the CMake option `REKINDLED_LOCAL_NVIDIA` (default OFF, forced OFF by the package script). With it ON and the DLSS SDK at `DLSS_SDK_DIR` the build links NGX for **DLSS-NR** (the neural renderer; `llvkngx.cpp`, `llvkdlssnr.cpp`; `LL_NGX_NR`) and asks the driver for `VK_NV_device_diagnostics_config` (the Aftermath decode of a device lost; `LL_LOCAL_NV_DIAG` in `llvkcontext.cpp`). Whoever builds it supplies the SDK and takes NVIDIA's terms on themselves. Adam's own local build dir has it ON (`<work>\cmake-regen.bat` passes it).
- DLSS super resolution and ray reconstruction are gone for everyone (local builds too).
- The upscaler facade (`llvkupscale`) has one backend (FSR). `sDLSS` in `pipeline.cpp` is really "an upscaler pipeline is active" and gets a vendor-neutral name at the end.

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
7. **Aftermath: local only** (Adam, 2026-11-14), done together with step 5 under `REKINDLED_LOCAL_NVIDIA`: the viewer only asks the driver for the diagnostics extension, no Aftermath SDK is linked (the decode tool is outside). **NVAPI** (`indra/cmake/NVAPI.cmake`, `llappviewerwin32.cpp`: stock Firestorm's OpenGL driver profile) stays: Adam, 2026-11-14, "if it's stock firestorm, leave it". The "NB Aftermath" sky preset is only a name.
8. **A whole-tree check:** `grep -ri "nvidia\|dlss\|ngx\|nvngx"` over `indra` and the packaged exe's strings, a release-style build with the option off, a lap, Adam's live look.

## Not changing

Hardware floor and everything GPU-vendor-neutral (Vulkan 1.3, bindless, MDI), the NVIDIA driver itself (the viewer runs on it), `nvoglv64.dll` showing in crash stacks.
