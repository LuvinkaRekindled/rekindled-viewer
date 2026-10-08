# What the Vulkan / render-clean work gained

Numbers come from different scenes and days, taken with different tools, so they cannot be added up into one figure. Each line says where it was measured.
Hardware: RTX 5060 Ti, Windows 11. "Crowd" = a busy sim with 40-90 avatars.

## 1. OpenGL-side main-thread work (before Vulkan, 2026-10-03, branch vulkan-port)

| Change | Result |
|---|---|
| Parallel avatar animation (FSParallelAvatarMotion, now on) | avatar update about 3 ms/frame less |
| Far sun-shadow cascades reused every Nth frame (FSShadowFarCascadeInterval) | about 3 ms/frame |
| Texture update divisor 80 + a 3 s burst at 20 after a region change or camera move (FSTextureUpdateDivisor) | about 1.4 ms/frame |
| Crowd of about 50 avatars, plain OpenGL, all of the above | **28.8 -> 17.8 ms/frame** (35 -> 56 fps) |

Tried and dropped (no gain): shadow skin-once, shadow receiver cull, multi-draw-indirect for static casters, skin palette UBO, texture-priority threading.

## 2. The Vulkan renderer (GL calls mirrored, then owned, then in the main window; 2026-10-02 to 04)

| Step | Result |
|---|---|
| Finding: the viewer is main-thread CPU bound in crowds, not GPU bound | explains why GPU-side tricks gave nothing |
| Cheaper recording of GL calls into Vulkan | 25-29 ms -> 5 ms per iteration |
| Draw-path work (state/sampler/vertex-input caching, spin mutex) | crowded shadowed sim **about 65 -> about 100 fps**; 20.1 -> 15.4 ms/frame |
| Worker thread replays the queued GL calls (FSVulkanAsync) | heavy sim: OpenGL about 50 fps, inline Vulkan about 25, threaded Vulkan **60 fps standing, 78 fps moving** |
| Coalescing queue records (LL_VK_COALESCE) | records 154k -> 70k per frame, frame 16.4 -> 14.9 ms |
| Pool spin-before-sleep | about 14.5 ms |
| Post-teleport stall (about 55 s) | fixed (buffer rename copied from video memory) |
| Device-local buffers | shadow copies 551 MB -> about 230 MB |
| Persistent SPIR-V cache and Vulkan pipeline cache | shader compile on start-up gone; pipeline build 160 -> 13 ms per session |

## 3. DLSS / DLAA (2026-10-03 and 10-06)

| Change | Result |
|---|---|
| DLSS super resolution (Performance / Balanced / Quality) | crowd at Balanced **125+ fps steady**; "double fps in some places" (Adam) |
| DLAA mode, live mode switching, live texture mip bias | GPU-bound spot: Balanced vs DLAA about 50 fps apart; DLSS off about 13 fps faster than DLAA at the same size (DLAA costs that, for the cleanest edges) |

## 4. render-clean (2026-10-06 and 07)

| Change | Result |
|---|---|
| Texture option hoist | about +5 fps |
| Shadow packets: static, alpha-masked (bindless textures) and rigged casters as records (FSShadowPackets, on) | **about +10 fps** (same spot, on vs off); worker -2 to -3 ms; shadow GPU +0.5 ms, total GPU unchanged |
| Far-cascade interval 3 -> 4 | 0.5-0.7 ms/frame of shadow time |
| Deferred geometry packets (FSDeferredPackets, off) | worker -1.4 ms, **no frame-time gain** (main thread is the limit); kept as an option |
| Skeleton world matrices inside the parallel blend | about 2% of the main thread |
| Water: dedicated SSR trace (bands, grain and flicker gone) | visual fix; Adam saw more fps too (not measured) |
| Rigged attachment bounding-box override skipped when unchanged | 0.43 -> 0.22 ms/frame in a crowd of 43 |
| Rigged attachments of standing avatars updated every 8th frame (FSSkipStaticRiggedAttachmentUpdates, on) | about +3 fps (Adam, dancers) |
| Object-cache light bounds | correctness (lights exist before their source is in view), costs a little more light work |

## 5. native-vk (2026-10-07)

| Change | Result |
|---|---|
| No OpenGL at all (no context, no GL imports in the exe, dead GL code deleted) | runs at ~228 fps in an empty scene; GPU class 5; small clean log |
| Tracy GPU zones from Vulkan timestamps | context created; zone count logged (not yet seen in the Tracy UI) |
| N1b typed records: vertex array state, texture binds, draws (`LL_VK_TYPED=0` turns them off) | same spot, same 20 s: main-thread queue writing 0.68-0.70 -> 0.51 ms/frame (53k -> 38k calls/frame), frame 10.9-11.2 -> 10.7 ms. About 3% of the frame; not visible as fps |

### native-vk, continued (2026-10-07, evening)

| Change | Result |
|---|---|
| Draw records: texture binds as a producer-side slot table, changed slots written with the draw; vertex array state, shader bind, sampler params, render target binds as single records | records per frame 28,300 -> about 19,900; no fps change (CPU-bound spot); queue writing about 0.5-0.6 ms of the frame |
| Bindless textures for 169 of 200 programs (the rest: no samplers 16, geometry stage, 8 SMAA programs whose helper parameters collide with sampler names) | neutral at a CPU-bound spot (A/B: both about 75 fps; frames settle faster); the foundation for indirect draws. Worker time per draw is about 1.0 us either way |
| Worker draw path: flat texture/buffer lookups, cached render-target resolution, draw state translated on change only | render target and state phases now 0.24 ms/frame together; the rest of the worker draw time is spread (uniform block 1.2, textures 0.7, vertex input 1.0 ms/frame at 4,000 draws) |
| **Animated textures skipped for objects out of view (FSSkipHiddenTextureAnim) + face matrices in closed form** | **loop 14.5-15.0 -> 11.95-12.0 ms at Adam's spot (about 20%)**: animated textures 1.7 -> 0.47 ms, 1,953 -> 340 objects per frame, idle() 5.1 -> 3.3 ms, display() 9.4 -> 8.5 ms |
| **Other residents' avatars that are not in view get the full update every 2nd frame (4th beyond 50 m), FSReduceHiddenAvatarUpdates** | **loop 11.99 -> 11.15 ms**: full avatar updates 66 -> 28.5 per frame, object list 2.22 -> 1.73 ms, avatar motion batch 0.91 -> 0.64 ms. Together with the animated-texture change the loop went from 14.5-15.0 to 11.15 ms (about 23%) |
| **Bug fix: the no-GL start-up reported the device's descriptor limit (1,048,576) as the number of texture units; display() loops over every unit each frame** | **loop 12.64 -> 7.69 ms, display() 9.15 -> 5.06 ms, idle() 3.27 -> 2.41 ms; Adam measured +45 fps at the spot.** Found with a Tracy self-time export ("Texture Unbind" 7.3 ms self) and a CPU zone around the loop; now reports 32 per stage / 192 combined like a driver |

Since the start of the 2026-10-07 evening session the loop at Adam's spot went from 14.5-15.0 ms to 7.7 ms.

GPU time at Adam's spot after the fixes (LL_VK_PHASES + FSGpuZoneTimers, run-viewer-clean-gpuzones.bat; GPU 6.5-6.7 ms per frame, GPU busy about 80-90%): alpha pass (post water) 1.47 ms (22%), DLSS 1.17 (18%), water 0.82 (12%), G-buffer materials 0.46 (7%), sun lighting program 0.33, sky 0.31, atmospherics 0.23, soften shadow 0.23, alpha (pre water) 0.21, all sun shadow maps about 0.35, local lights 0.01. The CPU side is about 7.5 ms (display 5.1, idle 2.4). Both sides are now close to balanced.

| Alpha shader: sun shadow lookup after the alpha discards (identical output) | alpha pass 1.47 -> 1.40 ms GPU |
| Texture refresh slows again after 10 s of a steady view (FSTextureUpdateSteadyFactor) + object texture priority pass every other frame then | about 0.18 ms of main-thread time |
| **Water reflection ray: at most 30 uneven steps instead of up to 48 (FSWaterTraceAdaptive)** | **water 0.85 -> 0.63 ms GPU (-26%), total GPU 6.47 -> 6.22 ms; no visible difference (Adam)** |
| Tried and removed: skipping the lighting of transparent pixels under 2% alpha | 0.03 ms, not worth the code |

| Loose uniform writes pile up on the main thread and are written with the next draw (draw-record plan, uniform step) | records per frame 18,900-19,900 -> 13,550; queue writing unchanged within noise |
| Deleted the dead GL-dependent debug code (verification, snapshot comparison, render target dumps, GL stubs): llvkgl.cpp 6,120 -> 5,260 lines | no behaviour change expected; one live call (before_end_frame, the end-of-frame flush of pending device-buffer writes) was deleted by mistake and restored after it caused deformed avatars and a device loss |

Main-thread profile at the spot before this change (loop 14.5-15 ms): object list 3.9 (animated textures 1.7, avatar motion 1.2, avatar update 0.9), sun shadows 1.7, UI about 2.3, lighting 0.9, G-buffer pass 0.9, state sort 0.7, texture updates and fetch about 1.2. The G-buffer pass being only 0.9 ms is why the GPU-scene first slice was not the best target for this scene.

## Where the main thread stands (blues place, 43 dancers, loop about 16 ms)
display 11-12 ms (sun shadows 2.4-3.0, alpha pass about 1.6, stateSort 1.7, G-buffer 1.5, UI 1.2, lighting 0.5), idle 4.6 ms (object list 3.2: avatar update, animated textures 0.5-0.7).
Octree operations are about 1.2 ms of it. What is left is spread thin; the per-draw work of the alpha pass is about 0.7 us per draw.

## End-to-end comparison (2026-10-07, same spot in a crowded club sim, graphics preset "5060", window about 2558x1360, measured by Adam from the fps counter)

| Build | Settings | fps | GPU busy |
|---|---|---|---|
| Stock Firestorm 7.2.4 x64 release (OpenGL; one minor version behind the branch base 7.2.5) | full resolution, no DLSS (not available) | **66** | about 50% (CPU bound) |
| native-vk (this branch) | full resolution, DLSS off, same window size | **about 112** (log: 8.6-8.8 ms per frame, no upscale step) | 96+% |
| native-vk | DLSS Quality (67%: renders 1713x898, upscaled to the window) | **135-145** (a repeat read about 150) | 90-96% |
| Frozen reference (render-clean with the Vulkan mirror, before the native work) | DLSS Quality (67%), same settings as the native runs | **about 115** (two runs) | 70-75% |

Like for like (full resolution, no upscaling): **about 112 fps against 66, 70% faster than the stock release**. With DLSS Quality the same branch reaches 135-150 fps, which stock cannot do. Against the frozen pre-native reference at the same settings: 17-26% faster, with the GPU busier (balanced use).
Crowd size drifts between runs (two native runs read 135 and 145 minutes apart), so differences under about 10% are inside the noise. The stock viewer is CPU bound at this spot (GPU about 50%), as it was at the start of the project; the native branch is the one that uses the GPU (90-96%). Not measured: a second stock run.
Open item: the DLSS checkbox may not apply live when unchecked (the tooltip says it should); one run on 2026-10-07 kept DLSS on after the box was meant to be off (the log showed the upscale step in every frame). Not reproduced on purpose; run-viewer-clean-dlssoff.bat starts with it off.

## What a single end-to-end number needs
The same spot, the same settings, the official OpenGL viewer against `run-viewer-clean.bat`, read off the fps counter. Not done yet.

## Regression-checklist findings fixed (2026-10-08)
| Finding | Cause | Fix |
|---|---|---|
| Saved snapshots had no water reflections | the first render after the live view has no previous-frame scene copy at the snapshot's size (world view without UI differs from the live one) | FSSnapshotWarmupFrames extra renders before the kept one |
| Saved snapshots had no DLSS anti-aliasing | each render of a snapshot shares one frame number, so the upscale reset its history every time | renders of one snapshot continue each other's history, 18 warm-up frames (one jitter cycle), 3 for thumbnails; jitter advances per render |
| Hair and other textures black after minimize | texture downscale needs a frame to draw and copy into; a minimized window has none, so the new storage stayed empty | the downscale queue waits while the window is minimized or hidden |

## Alpha pass (post water): looked at, closed (2026-10-08)
Done before: the sun shadow lookup after the alpha discards (1.47 -> 1.40 ms), skipping faint pixels (0.03 ms, removed). In a light sim at about 170 fps the post-water alpha passes are about 1.2 ms (static 0.86 + rigged 0.34, 735 draws). Reading alphaF.glsl again: what is left per pixel (atmospherics, reflection probe sampling, seven local light calls that return at once when unused) is the same work the opaque path does, and the cost scales with the covered pixels (overdraw), not with the shader. Nothing image-neutral and measurable is left; it only goes down with fewer or smaller transparent surfaces. Do not raise it again unless a scene shows an alpha pass far above this share.
