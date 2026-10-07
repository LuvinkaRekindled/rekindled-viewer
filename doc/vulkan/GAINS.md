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

## Where the main thread stands (blues place, 43 dancers, loop about 16 ms)
display 11-12 ms (sun shadows 2.4-3.0, alpha pass about 1.6, stateSort 1.7, G-buffer 1.5, UI 1.2, lighting 0.5), idle 4.6 ms (object list 3.2: avatar update, animated textures 0.5-0.7).
Octree operations are about 1.2 ms of it. What is left is spread thin; the per-draw work of the alpha pass is about 0.7 us per draw.

## What a single end-to-end number needs
The same spot, the same settings, the official OpenGL viewer against `run-viewer-clean.bat`, read off the fps counter. Not done yet.
