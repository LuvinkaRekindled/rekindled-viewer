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

## Step two run 2026-10-28 (create and evaluate behind LL_VK_DLSS_NR=2, shown behind 3)
- **Create** (`NVSDK_NGX_VULKAN_CreateFeature1(device, cmd, 18, params, &handle)` with `DLSSNR.Width/Height` = 2558x1341, `PerfQualityValue` 1, `DLSSNR.Hint.Render.Preset` 0, `DLSSNR.DepthInverted` 0): Success, **179 ms** the first time, 133 ms in the next session (no multi-second stall; the 254 MB snippet was already mapped by the probe). No `VK_ERROR_DEVICE_LOST`. Gate G2 passes.
- **Evaluate** (`DLSSNR.Color` RGBA8 window size, `.Output` RGBA16F window size, `.Depth` D32 and `.MVec` RG16F at the render size 1713x898 with their subrects, `.MVecScaleX/Y` 1, `.Reset`, the six look floats): Success every frame, two sessions, zero failures. **Render-size depth and motion with an output-size colour are accepted**, as are the mixed formats.
- **Picture** (flag 3, Adam by eye at his spot): a real picture, "everything looks the same mostly but shadows are touched up, some shading". Not black, not tiled, not shifted; the UI draws over it. Gate G3 passes.
- **Cost, not measured yet:** the fps counter read 44-46 with the GPU at 99 % while NR ran (the frame was about 12.8 ms = 78 fps at his yaw before); a same-run A/B needs a live skip switch (next). Process memory 2.9 GB, GPU memory 6.0 of 16 GB with the world loaded.
- **Snapshots** do not include NR: `applyNeuralRender` is skipped while `gSnapshot` is set (the plan's skip rule); a saved snapshot shows the normal picture.
- Live controls: `<work>/nr-debug.txt` (`intensity style gtone ltone lstruct skin quality preset automask mvscale`), read once a second; what intensity 0 does is not reported yet.

## The controls, from the bundle's own config (`OptiScaler.ini` `[DlssNr]`, read from the zip 2026-10-28; nothing run)
- `DLSSNR.Style`: 0 standard, 1 natural, 2 cinematic. Baked into the feature with the preset: a change recreates it (now part of the feature key; the log names the style at creation). Adam: the styles are "mainly lighting and contrast changes".
- `DLSSNR.Hint.Render.Preset`: 0 automatic (= 2 on 310.9+), 1 the original model ("Honest Tench": the snippet's embedded `CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04` weights), 2 the smaller FP8 model ("Crazy Cuckoo Small FP8", `CC_SMALL_FP8`).
- Intensity, local structure, local tone: 0..2, default 1.0 (0 = the input picture, 1 = the model's, above 1 past it). Skin structure: -1..2, default -1 = follow local structure. Auto mask: default on. Global tone is not exposed by the bundle; we pass 1.
- The bundle prefers NR before the upscaler at the render size (cheaper), and has a "working scale" of its own (the model's work done small and enlarged; cost falls with the square). Options for the cost step.
- `<work>/nr-debug.txt` is now `intensity style gtone ltone lstruct skin quality preset automask mvscale run`, defaults `1 0 1 1 1 -1 1 0 1 1 1`; run 0 skips the evaluate (the live off state).

## Crash 2026-10-28 09:33:35 while switching styles (not NR's)
Two style recreates worked (style 1 at 09:32:54, 58 ms; style 0 at 09:33:12, 64 ms), the viewer crashed 23 s later. Minidump `<user-profile>\AppData\Local\CrashDumps\firestorm-bin.exe.24884.dmp` (copied with the log to `<work>\crash-nr-2026-10-28\`). Stack (cdb, the build's pdb): a fork-join worker in `LLPipeline::generateSunShadow` lambda -> `shadowPrepMark` -> `markVisible` -> `LLCullResult::pushDrawable` -> `vector::push_back` reallocation -> `memcpy` reading address 0 (NULL_POINTER_READ): a cull result's vector with a null buffer, the signature of two workers pushing into one `LLCullResult`. The NR path (the Vulkan worker thread, `renderFinalize` on the main thread) is not on the stack and shares nothing with the shadow prep; the launcher runs with `FSParallelShadowPrepCheck 0` like the other launchers.

## Cost, measured 2026-10-28 (Adam's spot, flag 3, the viewer's 10 s fps lines, the debug file stepped every 32 s)
| setting | fps | frame |
|---|---|---|
| run 0 (the evaluate skipped) | 114.7 | 8.7 ms |
| run 1, ratio not set | 47.0 | 21.3 ms |
| `DLSSNR.ScalingRatio` 0.67 | 47.0 | 21.3 ms |
| `DLSSNR.ScalingRatio` 0.5 | 47.4 | 21.1 ms |
| `DLSSNR.ScalingRatio` 1.0 | 46.8 | 21.4 ms |

**NR costs about 12.6 ms a frame at 2558x1341**, more than the rest of the frame. The scaling ratio callback works with the `DLSSOptimalSettingsCallback` convention: performance mode 2 (quality) returns Success and ratio 1.0, modes 0 and 1 return UnsupportedParameter (the snippet supports the quality mode only). Setting `DLSSNR.ScalingRatio` at creation is accepted (the feature is recreated, 58 ms) but **does not change the cost**: whatever the snippet's "network = output x scale" is, this parameter does not drive it from outside (NGX logging, `ngx_log_verbose.reg`, would show the "CreateFeature begin requested resolution (network %ux%u)" line and settle whether the network size moved at all). The bundle's own "working scale" is therefore its own downscale of the colour it hands the feature, not this parameter.

What is left for the cost: **run NR before the upscaler at the render size** (1713x898, 45 % of the pixels; the bundle's recommended placement), expected about 6 ms if the cost follows the pixels; the model then sees the jittered, pre-tonemap render and DLSS upscales its output. A different position in the frame (before `applyUpscale` in `renderFinalize`, on `mRT->screen` with the HDR question of the plan's section 2), about a day.

## Position A/B 2026-10-28 (`nr-debug.txt` pos, the thirteenth number; same session, the file stepped every 35 s)
| position | fps | frame | NR cost |
|---|---|---|---|
| off (run 0) | 114.6 | 8.7 ms | |
| pos 0: after the upscale, 2558x1341 | 47 | 21.3 ms | 12.6 ms |
| pos 1: before the upscale, 1713x898 (the feature recreated at the render size, DLSS upscales its output) | 62 | 16.1 ms | 7.4 ms |

The cost follows the pixels (45 % of them, 59 % of the cost: a little fixed overhead). Adam at pos 1: "the outlines were more pronounced but it worked": the dark rims the model draws along edges are drawn in render pixels and DLSS enlarges them 1.5x; the look otherwise holds, and the jittered input did not show as flicker. Pos 1 is not used with ray reconstruction (the pre-upscale picture is the noisy radiance then; pos 0 runs instead).

Where it stands: NR is a 7.4 ms (render size) or 12.6 ms (window size) opt-in on this card for a look change Adam judges as touched-up shadows, lighting, skin texture and eyes, with dark edge rims as the artifact to live with (the bundle has a halo suppression pass of its own for them). Everything stays behind `LL_VK_DLSS_NR`; nothing ships.

## Preference, rim lift, mask fix (2026-10-28, later)
- **Checkbox** "Neural rendering (DLSS 5, experimental, local test)" on the Vulkan tab (`FSVkNeuralRender`, restart). Enabled only when `FSVkDLSS` is on and the device reported the feature usable (`LLDeviceCaps::mHasNeuralRender`, from `LLVkDlssNr::queryAvailability`, which runs at every start: `DLSSNR.Available` and the requirements of id 18, both no without `nvngx_dlssnr.dll` next to the viewer). The setting becomes `LL_VK_DLSS_NR=3` in `llappviewer.cpp` unless a launcher set the variable. Verified: the checkbox enabled, ticked, restarted with no flag (`run-viewer-dlss-nr.bat pref`): the log shows the probe at `LL_VK_DLSS_NR=3`. Adam's intent: a just-for-fun option and for group snapshots; a "snapshots only" mode (live picture untouched) is the natural next value of the setting.
- **Auto mask**: `DLSSNR.UseAutoMask` is now written every frame (0 or 1); before, a 0 in the file left the parameter at its last value, so the mask had never been off. The pink "haze" of skin structure 25 was with the mask on: the mask does not keep the skin response off walls and floor.
- **Rim lift** (`nrRimLiftF.glsl`, `applyNeuralRenderRimLift`, debug numbers 14-16: rim width 2, strength, depth edge 0.02): the first version lifted edge pixels toward the untouched input and made a light band wherever one body part crosses another (Adam: "worse"). Reworked: an edge pixel is lifted only toward the darkening ratio the model applied to its own neighbourhood two and three widths out on its side of the edge, never past it. Not yet judged; strength 0 in the file until it is.
- Adam on the picture at position 1 (snapshot 05:41): fabric weave and folds, skin translucency, consistent lighting; the hair stays flat (alpha cards give the model nothing). "If it wasn't for the hair not setting flat you wouldn't know that wasn't a real person between the skin and the cloth."
- **Rim lift verdict** (reworked version, same day): "it still fights and makes an inverse version that competes over it, making it flicker in spots". A per-frame decision (the depth edge band moves with the jitter, the neighbourhood ratio with the model's history) on top of a temporal model cannot be steady. Default strength 0; the pass stays for experiments only. The model's dark edge rims are the artifact to live with, or the bundle's approach (its own temporal residual) if it is ever worth a week.

## Live three-way mode (2026-10-28, evening)
`FSVkNeuralRenderMode` (S32, a combo on the Vulkan tab): 0 off, 1 on, 2 snapshots only. Applies at once: the pipeline reads it live (`LLPipeline::neuralRenderMode`), a change reallocates the screen buffers (the NR targets come and go, the same listener as ray reconstruction), the feature is created on the first frame that asks and retired when the mode goes off (`UpscaleArgs::mNeuralRender` 2 -> `LLVkDlssNr::release`, sent for a few frames so the retired feature is released once its frame has completed). Feature 18's device extensions are added whenever DLSS is on (they are DLSS's own plus push descriptors), so no restart is needed. The launcher's `LL_VK_DLSS_NR` flag overrides the preference (the test modes). Verified by Adam ("works great") and by the log: On created the 1713x898 feature, a snapshot in snapshots-only mode created and retired features at the snapshot's sizes (2744x1459 and the 1147x601 preview), Off retired the live one; no evaluate failures.
- **RR + NR** (Adam, 2026-10-28, a mid-sized region, taking snapshots): 65 fps with NR and RT on, 44 with ray reconstruction stacked on top; it runs and the picture holds. With RR the pre-upscale picture is the noisy radiance, so NR takes the window-size position there (12.6 ms rather than 7.4): the stack costs RR's own time plus the dearer NR position, 15.4 -> 22.7 ms.

## Denoise first (plan section 8), built and measured 2026-10-28/29 (the hub, Adam's account, environment fixed to morning, RT on, 2558x1341 window, render 1713x898)

`rr-debug.txt` fifth number: 0 today's order (RR denoises and upscales to the window in one feature, NR at the window size), 1 denoise first (RR at a one-to-one ratio into `mRRDenoised`, the plain path from there: post chain at the render size, NR at its render-size position, DLSS super resolution to the window with the jitter as today), 2 the same with the super resolution told a jitter of zero, 3 automatic (2 when NR runs in the frame, 0 otherwise; the default from 2026-10-29, also with no fifth number).

Log, order 1 and 2: `ray reconstruction feature created: 1713x898 -> 1713x898 quality 5` (the DLAA value) `preset 6`, `DLSS-NR feature created: 1713x898`, `DLSS feature created: 1713x898 -> 2558x1341 quality 2`; the two DLSS-family features park and swap every frame (PARKED_MAX 3), no warnings. The reset bookkeeping in `applyUpscale` is per kind (RR / SR), since both run in one frame.

| features on | order | fps (10 s lines, still) |
|---|---|---|
| DLSS only (RR off, NR off) | - | 107-110 |
| RR + NR | 0 today's | 37.5 |
| RR + NR | 1 denoise first, SR jittered | 47.7 |
| RR + NR | 2 denoise first, SR zero jitter | 48.8 |
| RR alone | 2 | 67 |
| RR alone | 0 today's | 76.5 |

Gates: G1 (RR at 1:1 creates and evaluates, the picture clean) passed from the log and by eye. G2 (SR with zero jitter on RR's output: no shimmer, ghosting or softness): Adam, standing and turning, "everything looks great". G3 (NR at the render size in the new order): the same verdict; the look is the plain path's. G4 (cost): a third of the stack's cost gone (37.5 -> 48.8); the cost of order 1 and 2 is the same, as expected from one number. G5 (snapshots): a 2558x1360 snapshot created RR 1713x911 -> 1713x911, NR 1713x911 and SR 1713x911 -> 2558x1360, no warnings; the snapshot had the depth of field in it. Depth of field is back with RR in the new order (the post chain runs at the render size, as the plain path).

RR alone: the second feature costs about 1.8 ms (76.5 -> 67 fps) and only buys the depth of field back, so the automatic default keeps today's order without NR. Someone who wants DoF with RR and no NR can force 2 in the file; a preference for that is not planned until asked for.

Not measured: RR at 1:1 at a hi-res snapshot size (4100x2178); the GPU zones `dlss rr` / `dlss` / `dlss nr` split (the fps lines were enough for the decision).

## The scaling ratio again (2026-11-15, Adam, render-size position 1, FSR upscaling after NR, 1713x898 render, 2558x1341 window)

Measured by Adam in the viewer (the 12th number of `nr-debug.txt`; the feature is made again when it changes): **at 1.0 about 85 fps (11.8 ms a frame); at any other value 55 to 57 fps (about 18 ms), the same for every value he tried; the picture looks better the higher the value, and it caps at 2.** Without NR the frame is about 5 ms, so NR costs about 6.8 ms at 1.0 and about 13 ms at any other ratio.

This corrects the 2026-10-28 table above (ratio 0.67 / 0.5 / 1.0 all 47 fps at the window-size position, "the ratio does nothing"): at position 1 there is a step at exactly 1.0. Reading (not proven): a ratio other than 1.0 turns on the DLL's second path (the guided upsample with the original colour snapshot and the previous output, the strings of PLAN-dlss-nr.md section 1.2), which costs about 6 ms of its own and does not depend on the value; within that path a larger value is a better picture (up to 2) at no extra cost. So there are two useful settings: **1.0 for the cheap one, 2.0 for the best looking one**; nothing in between is worth using. The cost with the ratio above 1 is the full network plus the upsample pass, not a smaller network: the value does not make NR cheaper.

Adam, same session: "1 seems to have more contrast, 2 is more subtle". It fits the reading above: at 1.0 the network's output is the picture; with the guided upsample (any other ratio) the output is blended back with the original colour as the guide, which pulls it toward the input and takes some of the contrast and restyling away. To get the contrast back at 2.0, the look controls (`intensity`, the first number of `nr-debug.txt`, 0 to 2, and the tone and structure numbers) are the knobs to try; not tried.
