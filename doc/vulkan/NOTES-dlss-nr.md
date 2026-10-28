# DLSS-NR (DLSS 5 neural rendering): what is known (2026-10-27)

Local testing only: nothing from this is shipped (Adam). The public Streamline repo (NVIDIA-RTX/Streamline 2.14.16) has `kFeatureDLSS_NR = 1004` in `sl_core_types.h` but no `sl_dlss_nr.h` and no plugin DLL; the public NGX SDK headers (`<work>/DLSS`, feature enum ends at `RayReconstruction = 13`) have no NR feature either.

## The zip
`F:\Documents\Desktop\dlss-unlocked-standalone-NR-v0.9.80.zip` (629 MB) is an OptiScaler based game mod bundle (proxy `dxgi.dll`, `REPatch/dinput8.dll`, `dlss-enabler-headless.dll`, XeSS / FSR / frame generation shims, `nvfp4/` weights). Not an NVIDIA package: do not run its hook DLLs. No header or documentation for the NR API is in it. Only the NVIDIA files were extracted, to `<work>\dlss-nr-test` (outside the repo, nothing run):

| file | Authenticode |
|---|---|
| `nvngx_dlssnr.dll` (from `OptiScaler/streamline/`, 254 MB, "NVIDIA DLSSNR" 310.9.16) | Valid, NVIDIA Corporation |
| `sl.dlss_nr.dll` (Streamline plugin 2.13.0, 0.4 MB) | Valid, NVIDIA Corporation |
| `sl.interposer.dll`, `sl.common.dll` (2.14.16) | Valid, NVIDIA Corporation |
| `root-nvngx_dlssnr.dll` (zip root, 263 MB) | NotSigned: a patched copy ("unlocked"): do not use |
| `root-nvngx.dll_dlssnr.dll` (zip root, 0.1 MB) | NotSigned: a shim: do not use |

## What the plugin tells us (strings of `sl.dlss_nr.dll`, read only)
It wraps an NGX feature (`DLSSNRBackendNGX`, "Created DLSS-NR NGX feature %ux%u preset=%u performanceMode=%u"; "DLSS-NR feature is not supported. Please check if you have a valid nvngx_dlssnr.dll or your driver supports DLSS-NR"; it reads the GPU architecture, and the zip carries `nvfp4` weights: expect Blackwell only). The NGX parameter names it sets: `DLSSNR.Color`, `.Depth` (+ `DepthInverted`), `.MVec` (+ `MVecScaleX/Y`), `.Output`, `.UI`, `.UIAlpha`, `.UICorrection`, `.Backbuffer`, `.ControlMask`, `.UseAutoMask`, `.BidirectionalDistortionField`, `.Reset`, `.Width`, `.Height`, `PerfQualityValue`, `DLSSNR.Hint.Render.Preset`, each image with `...SubrectBaseX/Y/Width/Height`, and the look controls `DLSSNR.Style`, `.Intensity`, `.GlobalToneStrength`, `.LocalToneStrength`, `.LocalStructureStrength`, `.SkinStructureStrength`. Plus the Streamline buffer tags (`kBufferType...`: colour before/after fog, particles, transparency, depth of field, AO, diffuse hit distance, ...). So it looks like a neural upscaler / image enhancer with tone, structure and skin controls, not a denoiser.

## Not known
The NGX feature id of NR (the SL id 1004 is not the NGX one; RR is 13), the parameter types (resource / int / float) and the create and evaluate calls, the hardware and driver requirements, the licence terms for the DLL. A Streamline hookup is blocked on the missing `sl_dlss_nr.h`. An NGX level probe (feature requirements query with candidate ids, then the parameter names above) is possible in a test build only; the NVIDIA signed `streamline/` files are the ones to use.

## Update 2026-10-28
Most of the "not known" list is answered in `PLAN-dlss-nr.md` section 1.2 from the installed driver's NGX core (`_nvngx.dll`, 617.42): the core has `DLSSNR.Available` and friends, and its feature table puts DLSSNR at NGX feature id **18** (`NVSDK_NGX_Feature_Reserved18`; 14-17 are TrueHDR, Latewarp, VSR, FGX). The snippet itself adds `DLSSNR.Enabled`, `.ScalingRatio`, `.MaskBlendMode` and the callbacks `DLSSNRComputeScalingRatioCallback` / `DLSSNRGetStatsCallback`, needs at least Blackwell2, and wants Color + Output (+ MVec, Depth) as a minimum. The plan has the probe build and the gates.

## Probe run 2026-10-28 (step one of the plan, `run-viewer-dlss-nr.bat`, LL_VK_DLSS_NR=1, driver 617.42, RTX 5060 Ti)
What the NGX core answered at device creation (`#VkDlssNr#` lines in Firestorm.log; nothing created, the viewer ran on to the login screen as before):

| question | answer |
|---|---|
| `DLSSNR.Available` / `.NeedsUpdatedDriver` | 1 / 0 |
| `DLSSNR.MinDriverVersionMajor.Minor` | 615.0 |
| `DLSSNR.FeatureInitResult` | 1 (= `NVSDK_NGX_Result_Success`) |
| `DLSSNR.Enabled`, `.Width`, `.Height` on the capability object | not set (UnsupportedParameter): per-feature parameters, not capabilities |
| `DLSSNRComputeScalingRatioCallback`, `DLSSNRGetStatsCallback` | both non-null (the snippet is loaded and hands them out like `DLSSOptimalSettingsCallback`) |
| `GetFeatureRequirements(18)` | **Supported**, min hardware architecture 0x1b0, min OS 10.0.0 (so GB206 passes the Blackwell2 check, and the custom project id is not denied) |
| `GetFeatureRequirements(14..17)` | the query fails with an unnamed result for TrueHDR, Latewarp, VSR, FGX (no snippet for them here): consistent with the table reading, 18 is the one that answers |
| feature 18 instance extensions | `VK_KHR_get_physical_device_properties2` |
| feature 18 device extensions | `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_buffer_device_address` (DLSS's set) + `VK_KHR_push_descriptor` (already on the device: no "enabling" line) |
| `GetScratchBufferSize(18)` | PlatformError, 0 bytes (the SDK documents the scratch size as unused for the DLSS features; same here) |
| for comparison | `SuperSampling.Available` 1, `SuperSamplingDenoising.Available` 1, `FrameGeneration.Available` 0 (no `nvngx_dlssg.dll` next to the exe) |

Not run: the scaling ratio callback (`LL_VK_DLSS_NR_RATIO=1`, a guessed convention). Gate G1 of the plan passes; G2/G3 (create, evaluate) are step two.
