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

Main-thread profile at the spot before this change (loop 14.5-15 ms): object list 3.9 (animated textures 1.7, avatar motion 1.2, avatar update 0.9), sun shadows 1.7, UI about 2.3, lighting 0.9, G-buffer pass 0.9, state sort 0.7, texture updates and fetch about 1.2. The G-buffer pass being only 0.9 ms is why the GPU-scene first slice was not the best target for this scene.

## Where the main thread stands (blues place, 43 dancers, loop about 16 ms)
display 11-12 ms (sun shadows 2.4-3.0, alpha pass about 1.6, stateSort 1.7, G-buffer 1.5, UI 1.2, lighting 0.5), idle 4.6 ms (object list 3.2: avatar update, animated textures 0.5-0.7).
Octree operations are about 1.2 ms of it. What is left is spread thin; the per-draw work of the alpha pass is about 0.7 us per draw.

## What a single end-to-end number needs
The same spot, the same settings, the official OpenGL viewer against `run-viewer-clean.bat`, read off the fps counter. Not done yet.
