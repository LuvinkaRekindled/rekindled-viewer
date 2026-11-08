# PLAN-shaders: GPU pass research (started 2026-11-08)

Order set by Adam: clean core, then PLAN-hotspots (first pass done), then this shader pass. Research first. Nothing here changes the picture until Adam has seen the ranking and picked.

## 1. Where the GPU time goes, RT off, crowd spot (lap gz9-phasesoff1, 2026-11-08)

Method: `h6-fixed.ps1 -Mode phasesoff -Prefix gz9-` with `H6_EXTRA_ARGS=--set,FSGpuZoneTimers,1`; last-location login at the crowd spot, two still phases (60 s + 120 s), 25 one-per-6-s "GPU time by render pass" readings after settling. The zone timers perturb the main thread (frame 11 ms in this lap, the worker waited for work 3.3 ms a frame), so only the GPU ms per pass count here, never the fps. The log line lists the top ~45 names only, so the sum of the table is a little under the frame.

GPU frame, estimated from the DLSS share (1.29 ms = 20%): **about 6.4 ms**.

| GPU ms | share | what |
|---|---|---|
| 1.29 | 20% | DLSS (external, NVIDIA code) |
| 0.85 | 13% | shadow cascade passes (zone: shadow simple / alpha static / alpha rigged, all cascades) |
| 0.78 | 12% | alpha runs through the scene table: rigged 0.56, static 0.15, rigged fringe 0.07 |
| 0.60 | 9% | sun lighting 0.31 + soften shadow 0.29 |
| 0.59 | 9% | opaque G-buffer draws through the scene table (types 0, 10r, 17, 11r, 2, 7r, 0r, 2r, 5r) |
| 0.58 | 9% | sky and atmospherics: atmospherics 0.23, sky haze 0.17, clouds 0.12, haze 0.06 |
| 0.19 | 3% | avatar velocity |
| 0.13 | 2% | renderGeomPostDeferred |
| 0.11 | 2% | renderFinalize |
| 0.08 | 1% | water pass begin |
| 0.07 each | 1% | terrain pool, ui |
| 0.06 | 1% | hiz pyramid + box test |
| 0.05 | 1% | rt mipmap generation |

Not covered yet: RT on (the lap that matters for the RT lighting pipeline), a different crowd, the lower-res DLSS modes.

## 2. First reading (research, not measured as a cause)

- The cheap-looking big ones for a shader pass are the alpha runs (0.78), the sky/atmospherics stack (0.58) and soften shadow (0.29): each is a screen-space or overdraw-bound pass where a shader or resolution choice moves milliseconds. All are candidates, not decisions.
- The rigged alpha run is 0.56 ms alone: hair and clothing alpha on the crowd, overdraw-bound by the earlier Nsight read (pixel stage 5-6 warps of ~18, vertex ~9%).
- The shadow cascades are 13%: alpha-tested casters (hair, foliage) cost more than simple ones; the fragment shader's texture fetch is the suspect.
- DLSS at 1.29 ms is fixed cost for the quality mode and render resolution; that is Adam's quality call, not a shader.
- Sun rays under RT cost ~0.5 ms a ray (2026-10-29), not part of this RT-off table.

## 3. Next steps

1. Nsight GPU Trace of one frame at the same spot (Adam parked): per-pass limiter (SM, L2, VRAM, ROP) and, with the shader profiler, stall reasons for the alpha run, the cascade alpha casters, soften shadow and the sky pass.
2. An RT-on zone lap for the same ranking.
3. Ranked, picture-preserving candidates here, each with its expected ms and how it is checked.

## 4. Nsight GPU Trace, RT off, crowd spot (2026-11-08 10:11, rekindled-bin_2026_11_08_10_11_56.ngfx-gputrace)

Launcher `<work>\run-viewer-gputrace-shaders-rtoff.bat` (labels, FSGpuZoneTimers, RT off, real-time shader profiler, multi-pass metrics; F11 by hand after the settle). Three frames traced, 36 MB report in `<work>\nsight-traces`.

What the auto-export holds: GPUTRACE_FRAME.xls has the whole-frame counters AND one row per shader (`tpc__warps_active_shader_node_main_0x<hash>`, the PC sampler's resident warps per cycle for each shader, three frames); D3DPERF_EVENTS has only the DLSS evaluate (1.26 ms, SM ~98% busy, dense compute, nothing to tune). The Vulkan labels and per-pass limiters are not in the export (they are in the .ngfx-gputrace UI). Per-shader share is the way in: rank by hash, name the hashes with `aftermath-decode/hashspirv.exe` against the viewer's SPIR-V cache.

Whole-frame reading (average of the three frames; the zone timers slow the main thread, so the GPU idles part of the window and the percentages are diluted):
- SM throughput 13.7%, L1TEX 13.2%, L2 12.4%, ScreenPipe 7.9%, WorldPipe 2.2%, CROP 5.8%, ZROP 3.2%, PCIe 6%: nothing near a hardware limit when averaged over the window.
- Resident warps per cycle by stage: pixel 5.8, compute 2.1, vertex/geometry 1.6 (the vertex stage is about 17% of the shader work, pixel about 61%).
- So the picture is the same as 2026-10-29: pixel-shader work dominates, vertex is small. It does not say which passes.

### Shader ranking by resident warps (112 shader nodes, 6.14 warps per cycle summed; DLSS/NGX 2.2 not included)

Top 8 are 67.5% of the shader work, the top 5 55%. Hashes (Aftermath, hex) with share of the node sum: 6eb53984711cda70 17.9%, 661f10a0a17ca17d 12.8%, a423d33ec6f653ff 8.8%, 3e0319e686017e6a 8.2%, 37d3170d533e8d3f 7.7%, ca1e82ce15884f4e 4.6%, f5a78a15f4bfe74d 3.8%, 821d9fe8ed7e5540 3.7% (37d3170d and ca1e82ce were also in the 2026-10-27 top ten). Decimal list for hashspirv in `<work>
sight-top-hashes-rtoff.txt`. Naming them (which pass, which program) is the next step.

### Names of the top shaders (hashspirv over the SPIR-V cache, matched to sources by uniforms; RT off, crowd spot)

Warp share is the PC sampler's resident-warps share, a proxy for GPU time, NOT ms. It does not line up with the zone-timer ms (the sun lighting is 18% of warps but 0.31 ms = 5% of the frame in the zone lap), so treat it as "where shader work sits", and check the ms with a live key before any change.

| warp share | stage | shader (source) | note |
|---|---|---|---|
| 17.9% | fragment, 33.5 KB | sunLightF / sunLightSSAOF (class2 deferred: screen_res, spot_shadow_mask, proj_*) | the sun lighting pass |
| 12.8% | fragment, 7.8 KB | shadowAlphaPull (alpha-tested cascade casters) | pair of the next row |
| 8.8% | vertex, 8.5 KB | a ll_packet_table / ll_view_proj pull vertex (no texmats), 14 variants | most likely shadow simple pull vertex |
| 8.2% | vertex, 27 KB | shadowAlphaPullV (shadow_alpha_indexed) | the alpha caster's vertex shader: 27 KB, includes the skinning |
| 7.7% | fragment, 10.4 KB | materialF family (frag_data, sun/haze uniforms, 78 variants) | the main G-buffer material pixel shader |
| 4.6% | fragment, 14.4 KB | postDeferredNoDoFF (depth_jitter) | |
| 3.8% | fragment, 118 KB | softenLightF (blur_fidelity, ssao_irradiance) | 4 variants |
| 3.7% | fragment, 1.8 KB | tiny depth + diffuse sampler shader (ll_ti_depthMap), 78 variants | not identified by source yet |
| 2.7% | vertex, 12.8 KB | avatarVelocityPullV | |
| 2.5% | fragment | terrain (object_plane_s) | |
| 1.9% | fragment, 79 KB | CASF (cas_param) | final sharpen/tonemap |
| 2.2%, 2.1%, 1.8% | | alphaPullV vertex, point/spot light fragment, hazeF | |

Reading: the two shadow-alpha rows (21%) plus the simple-shadow vertex (8.8%) put about 30% of the shader work in the cascade passes, a lot of it in vertex stages (the alpha caster's vertex shader alone is 8.2%). The sun lighting fragment is the single biggest shader. These are where a shader pass would look first; each needs its own ms check before a change.

## 5. Shadow alpha casters taken apart (2026-11-08, lap gz11-phasesoff1, RT off, crowd spot)

Temporary live key `shadowalphadbg` (rt-debug.txt; C++ in llvkworker.cpp next to `shadow_alpha_indexed`, shaders shadowAlphaPullV/F): 1 = no texture fetch and no discards, 2 = every triangle clipped after the vertex work. Windows of 25 s, 0,1,2 twice, zone timers on ("zone: shadow alpha" summed over all cascades, static + rigged); `<work>\keycycle.ps1` drives the key.

| mode | shadow alpha ms | shadow simple ms |
|---|---|---|
| 0 normal | 0.481 | 0.285 |
| 1 no fetch, no discard | 0.470 | 0.294 |
| 2 clipped after the vertex work | 0.314 | 0.286 |

- The texture fetch and the discards cost nothing measurable (0.011 ms, inside the noise): the fragment shader is NOT the lever, whatever the warp share said.
- Rasterising and shading the surviving fragments (mode 1 minus mode 2) is 0.156 ms of the 0.481.
- The other 0.314 ms is the vertex stage plus the pass's fixed cost; modes 3 (packet read only) and 4 (no skinning) split that next.
- Lesson for the tool: the PC sampler's warp share for a shader is not its ms (fragment 12.8% of warps, ~0 ms here). Rank by A/B, not by share.
- Pitfall: the debug key reader falls back to 1 when the line is missing; rt-debug.txt got `shadowalphadbg=0` appended so the key stays off until the switch is removed.

### 5b. Where the rest of the alpha caster time goes (lap gz12-phasesoff1; mode 0 reads 0.52 here against 0.48 in gz11: the lap-to-lap noise is about 0.04)

| mode | shadow alpha ms | static | rigged |
|---|---|---|---|
| 0 normal | 0.523 | 0.370 | 0.153 |
| 3 packet read only, everything clipped (the floor: draws + pass) | 0.153 | 0.092 | 0.060 |
| 4 no skinning | 0.456 | 0.361 | 0.095 |

- The floor, with the vertex work and the fragments gone, is 0.153 ms = 30% of the alpha casters. The resident alpha cull (`kCullShader`) writes one draw command for EVERY row (4787 rows, culled ones with an index count of 0) and the pass runs all of them through one multi-draw indirect: each cascade pass processes 4787 draws, most of them empty.
- Skinning (the 4-joint palette fetch) is 0.058 ms: the whole rigged part drops 0.153 -> 0.095.
- So 0.52 ms = floor 0.15 (draws) + vertex work ~0.16 (of which skinning 0.06) + raster and fragments ~0.16 + the rest.
- The same shape sits in the opaque resident cascade casters (second `memcpy(params.mPtr, blob, 96)` site, `kCullShader` too) and probably the GPU-built scene table draws (`sGeo.mRCommands`). Candidate fix, same picture: compact the visible commands in the cull shader (atomic append) and draw with `vkCmdDrawIndexedIndirectCount`, so culled rows cost nothing. Keys 5 (alpha casters) and 6 (opaque casters) cull every row to measure what the empty draws alone cost; that is the size of the prize.

### 5c. Empty draws measured (lap gz13-phasesoff1) and the fix built (compacted draws)

Key 5 culls every alpha caster row, key 6 every opaque caster row (so every draw of the pass is zero sized): the cost of the draws alone.

| mode | shadow alpha ms | shadow simple ms |
|---|---|---|
| 0 normal | 0.464 | 0.286 |
| 5 all alpha rows culled | 0.334 (static 0.189) | 0.282 |
| 6 all opaque rows culled | 0.456 | 0.079 |

So the empty draws alone cost 0.079 ms of the opaque cascade casters and 0.19 ms of the static alpha casters (rows: 4975 alpha, one draw each in every cascade pass). The prize of dropping the culled rows' draws is the share of those that are culled (most, in a cascade).
Fix built (J2 shader pass, not yet measured): both resident cull shaders append the visible draws (atomicAdd on a 16 byte header word) and the pass draws with `vkCmdDrawIndirectCount` / `vkCmdDrawIndexedIndirectCount` (new `Capabilities::mDrawIndirectCount`, new count-buffer arguments of `drawPackets` / `drawPacketsIndexed`). Temporary switch `residentcompact` (rt-debug.txt, default 1; 0 = the old every-row draw). The draw order inside a pass changes (depth-only casters: no visible effect).
