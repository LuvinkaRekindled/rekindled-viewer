# Nsight Graphics GPU Trace: how we capture, where the data is, how to name shaders

Written 2026-11-08 so the commands and the lessons are not lost. Tool: `<work>\nsight-shaders.py`.

## Capture

Launchers (all in `<work>`, CRLF): `run-viewer-gputrace-shaders-rtoff.bat` (RT off; edit `LL_VK_RT=0; LL_VK_RT_SHADOWS=0` to `1` in its `--env` for RT on), older `run-viewer-gputrace.bat`, `run-viewer-gputrace-sun.bat`.
They start the viewer THROUGH `ngfx.exe` (Nsight Graphics 2026.3.1, `C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe`):

1. Check no `rekindled-bin` is running, then run the launcher (PowerShell: `cmd /c "<work>\run-viewer-gputrace-shaders-rtoff.bat"`, in the background).
2. Log in by hand, stand at the crowd spot at your own yaw, let it settle (about a minute).
3. Press F11 ONCE. 3 frames (`--limit-to-frames 3`, 1000 ms max) are traced. ngfx ends the viewer itself afterwards; do not quit it by hand, do not kill it.
4. Flags that matter: `--activity "GPU Trace Profiler" --start-after-hotkey --limit-to-frames 3 --auto-export --multi-pass-metrics --real-time-shader-profiler --set-gpu-clocks unaltered --output-dir <work>\nsight-traces`. The viewer env has `LL_VK_LABELS=1` (debug labels) and `--set FSGpuZoneTimers 1` (zone names); every other `LL_VK_*` variable of the shell is cleared first by the launcher.
5. The zone timers slow the main thread, so the GPU idles part of the window: whole-frame percentages come out low (SM 13.7%). Use the shares, not the absolute percentages.

## Where the data is

`<work>\nsight-traces\<exe>_<date>.ngfx-gputrace` (the full report, opens in the Nsight Graphics UI: per-pass limiters, shader profiler, Top-Down Calls) and the auto-export `<work>\nsight-traces\BASE_UNLOCKED\*.xls` (tab separated, OVERWRITTEN by the next capture; move it aside, `nsight-traces-old` holds the first one):
- `GPUTRACE_FRAME.xls`: whole-frame counters, three columns = three frames. Includes one row per shader: `GPUTrace.PCSampler.tpc__warps_active_shader_node_main_0x<hash>.avg.per_cycle_elapsed` (resident warps per cycle of that shader; each hash also has a `pct_of_peak` row, ignore it), and per stage `tpc__warps_active_shader_{vertex,pixel,compute,ngx}`.
- `D3DPERF_EVENTS.xls`: only the DLSS evaluate (`nv.ngx.dlss.Evaluate`, 1.26 ms).
- `GPUTRACE_REGIMES.xls`: per-event counters, again only the DLSS event (our Vulkan labels are not exported).
- NOT in the export: per-pass limiters/ms and source-level shader stalls (UI only). Per-pass GPU ms come from the `FSGpuZoneTimers` lap (log line "GPU time by render pass", see PLAN-shaders.md).

## Naming the hashes

`python <work>\nsight-shaders.py [export dir] [--top N]` ranks the shader nodes by their warps share and names each: the hash is the Aftermath SPIR-V hash, so `aftermath-decode\hashspirv.exe` (source `hashspirv.cpp`) scans the viewer's SPIR-V cache (`F:\Games\SL\vk_spirv_cache`, outside <work>: ask Adam before reading it) and gives stage, size, fragment outputs and the program's uniform list; the tool then matches the uniforms, weighted by rarity, to the `.glsl` sources in `app_settings/shaders`. The match is a HEURISTIC (shared uniforms such as proj_* confuse it; confirm by reading the source). Translations are kept in `<work>\nsight-hash-names.json` (hash in decimal -> stage, bytes, outputs, uniforms, source guesses), so each hash is read from the cache once. A shader edit changes its hash: old names stay valid for old traces only.

## Reading it right

- Warps share = where shader work sits, NOT ms. It does not line up with zone-timer ms (RT-off lap 2026-11-08: the top shader is 17.9% of warps, its pass 0.3 ms = 5%). Always confirm ms with a live key, an A/B lap or the zone timers.
- First results: PLAN-shaders.md sections 4 and after.
