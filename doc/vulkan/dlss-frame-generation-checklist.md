# DLSS Frame Generation (DLSS-FG / MFG) for the Vulkan mirror: checklist

Written 2026-10-03 from NVIDIA's own "DLSS Frame Generation SDK" programming guide (version 310.7.0, `<work>\DLSS\DLSS\doc\DLSS-FG Programming Guide.pdf`)
and the Vulkan headers in `<work>\DLSS\DLSS\include` (`nvsdk_ngx_helpers_dlssg_vk.h`, `nvsdk_ngx_params_dlssg.h`). Nothing here has been built or tried.
Status: parked idea, not started. What exists today is DLSS **super resolution** only (every displayed frame is a real, rendered frame).

## 0. Is it worth it? Read this first

- **Frame generation only helps when real fps is below what the display can show.** Smooth Motion earlier locked the real frame rate to half the refresh
  rate and sat at "60", which suggests a 120 Hz display (unconfirmed: check Windows display settings). At 125+ real fps with DLSS Balanced there is nothing
  left to generate. The useful range is a real 40-70 fps shown as 80-140. That is the heavy-sim case, not the fountain.
- It adds latency (the real frame is held back to make room for the generated one) and can show artifacts exactly where Second Life is hardest:
  dancing avatars, particles, alpha-blended things, water, animated textures, flexi hair. Quality depends mostly on how good the motion vectors are.
- NVIDIA recommends integrating through **Streamline** (it hooks the swapchain and does the pacing). The raw NGX route we would use means we write the
  pacing and the extra presents ourselves (section 6).
- NVIDIA Smooth Motion in the NVIDIA app already gives a driver-level version for free (worked with the Vulkan path; costs: locks real fps to half
  refresh, ghosting, no input from the viewer). Keep that as the zero-effort alternative.

## 1. Requirements (from the guide)

- [ ] GPU: RTX 40-series or newer for 2x frame generation; **multi-frame generation (3x, 4x) needs RTX 50-series**. Adam's card is a 50-series (5060), so MFG should
      be possible. Confirm at run time: capability `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` (3 means up to 4x; 0/1 means 2x only).
- [ ] Windows 10 20H1+, driver 520+ (any current driver is fine), Vulkan 1.1+ (we use 1.3).
- [ ] `nvngx_dlssg.dll` shipped next to the exe, like `nvngx_dlss.dll` now (`lib\Windows_x86_64\rel\`; use `dev\` while developing to get the debug hotkeys).
- [ ] Re-read the SDK licence before sharing a build with anyone (conditions on redistribution).

## 2. What we already have that carries over

- NGX init, extension discovery, device creation hook, thread-safe replay of NGX calls on the mirror thread (`rkvkdlss.cpp`, `LLVkMirror::upscale`, `Renderer::recordExternal`).
- DLSS-SR already needs render-size depth, motion vectors, jitter and a reset flag: **FG takes the same depth and motion vector resources** the guide says
  ("if an upscaler is in use, give FG the same depth and motion vectors that went to the upscaler").
- Unjittered view-projection for this frame and the last (`LLPipeline::mViewProjCur/Prev`), jitter, render and output sizes, the "texture needs storage usage" hook.
- Swapchain: FIFO, `minImageCount + 1` images, 3 frames in flight (`rkvkcontext.cpp`).

## 3. Work package A: real motion vectors for everything that moves (the big one)

FG is much less forgiving than SR about wrong or missing vectors: a generated frame built on camera-only vectors will smear and tear around every dancer.

- [ ] Rigid objects: previous-frame model matrix per drawable, passed to the object shaders; write screen-space motion into an extra G-buffer attachment.
- [ ] Avatars: previous-frame joint / skin matrices for rigged mesh and the base avatar (GLTF joint UBO and the legacy skinning path), plus attachments.
- [ ] Animesh, flexi prims, sculpts / morphs that change shape; terrain and water; particles; tree and grass sway (vertex animation in the shader).
- [ ] Things the guide says vectors cannot describe (shadows, particles, post effects, occlusion changes): accept some artifacts; decide what to mask.
- [ ] Sky and far scenery stay on the depth-based camera path we have now.
- [ ] Decide on the vector format (RG16F now; guide: pixels at the resolution of the vector texture, `mvecScale` fixes unit/axis differences, negative scale flips an axis).
- [ ] Optional: dilate the vectors (set `motionVectorsDilated`) so edges are covered.
- [ ] This work also improves DLSS-SR (the "light blur while moving" Adam noticed), so it is worth doing first even without FG.

## 4. Work package B: matrices and camera data (small)

- [ ] Camera matrices, each `float[4][4]` row-major, **no jitter in them** (the header says so): `clipToPrevClip`, `prevClipToClip`, `cameraViewToClip`, `clipToCameraView`.
- [ ] Camera position, up / right / forward vectors, near, far, FOV (radians), aspect ratio.
- [ ] `depthInverted = 0` (viewer depth: 0 near, 1 far), `colorBuffersHDR = 0` (SDR display), `cameraMotionIncluded` matching how our vectors are built, `mvecScale`.
- [ ] `reset` on teleport, camera cut, region change, window resize; `notRenderingGameFrames` while on the login screen or when no world is drawn.
- [ ] `multiFrameCount` and `multiFrameIndex` (1..count) per evaluate call; the number of evaluate calls per real frame is multiplier - 1.

## 5. Work package C: separate the UI from the 3D picture (large restructure)

- [ ] Required inputs: the final colour (with UI) as the backbuffer, depth, motion vectors.
- [ ] Strongly recommended: **HUDless** (the same picture without UI; must match the backbuffer exactly on every non-UI pixel, so tonemapping etc. must be in it) and **UI colour + alpha**
      (premultiplied, four full channels; not RGB10A2). Without them, UI text and panels get smeared across generated frames.
- [ ] Today the viewer draws UI straight into the backbuffer after `renderFinalize` (floaters, chat, name-tag overlays that are drawn in 2D, HUD attachments). That would need to render into an
      offscreen RGBA16F target and be composited, or at least be captured into a UI target by a pass that writes colour and alpha.
- [ ] In-scene 3D name tags and their boxes are part of the 3D picture and need no special handling.
- [ ] Menu detection (`menuDetectionEnabled`) can cover full-screen UI cases without the above, but is a fallback.

## 6. Work package D: presenting generated frames (the hard architectural part)

The guide is explicit: "The DLSS-FG feature itself does not handle timing or presentation; this is the responsibility of your application."

- [ ] Replace the single present per frame with: evaluate -> present the generated frame -> present the retained real frame, at equal intervals (2x: two presents per real frame; 4x: four).
- [ ] Keep the real frame alive until its turn: use the `OutputReal` resource (recommended in the guide) so it is copied for us.
- [ ] Present the generated frame as soon as its evaluate completes; for multi-frame generation present each as it is made, then the real frame last.
- [ ] Pacing runs asynchronously from the main render thread: a small pacer thread (or the mirror worker) that times presents against the measured real-frame interval.
- [ ] Swapchain needs enough images for the extra presents (probably `minImageCount + 2` or more) and a plan for the FIFO queue depth and vsync / VRR behaviour.
- [ ] Reflex-style latency management is recommended by NVIDIA (the held-back real frame adds latency). In raw Vulkan that is `VK_NV_low_latency2`; investigate whether it is needed.
- [ ] Interaction with the mirror: its backbuffer, the off-screen frames used for probes and snapshots, `holdFrame`, resize (FG must be recreated), and the async depth (`LL_VK_ASYNC_DEPTH`).
- [ ] Snapshots, previews and the login screen must not generate frames.
- [ ] Turn off Smooth Motion in the NVIDIA app while testing (two interpolators fighting each other would give nonsense).

## 7. Work package E: feature lifecycle and settings

- [ ] Create `NVSDK_NGX_Feature_FrameGeneration` with the output size, native backbuffer format and render size; recreate it on resize or render-scale change (same rule as the DLSS-SR feature; avoid recreating on tiny size flip-flops).
- [ ] Query support and `MultiFrameCountMax`; hide or grey out the options the card cannot do.
- [ ] Preferences (Vulkan tab): checkbox "Frame generation" + choice 2x / 3x / 4x limited by `MultiFrameCountMax`; applies after a restart like the other Vulkan options; only enabled when DLSS is.
- [ ] VRAM: guide estimates 218 MB at 1080p, 384 MB at 1440p, 598 MB at 4K (Adam's window is 2558x1341). `NGX_VK_ESTIMATE_VRAM_DLSSG` can check before creating.
- [ ] Cost from NVIDIA's table (2x): about 1.5-2 ms at 1440p on an RTX 5080 (4x about 2.5-3.6 ms); a 5060 is slower, not listed. The viewer's GPU time per frame must leave room for it.
- [ ] Optional: if the viewer applies motion blur anywhere, the guide says to halve it for 2x (divide by the multiplier for MFG).

## 8. Debugging aids (use the dev DLL)

- [ ] Use `lib\Windows_x86_64\dev\nvngx_dlssg.dll`; the registry files in the SDK `utils` folder switch on logging and the on-screen debug mode.
- [ ] Hotkeys (guide): ctrl+shift+v debug visualisation, then the test modes: **Motion Vector Resampling** (highlights where our vectors do not describe the motion), **Clip-to-Prev-Clip matrix**
      (static scenery must not light up, otherwise the matrices are wrong), **Depth and Motion Vector Alignment** (blue/yellow edges must outline the geometry), **UI Hints** (HUDless / UI alpha correct).
- [ ] Ctrl+shift+k shows the list of the other debug keys on screen (the guide's hotkey table).

## 9. Suggested order if this is ever picked up

0. Measure first: real fps in the heavy sims vs the display refresh rate (is there anything to gain?), `MultiFrameCountMax` on the 5060, DLSS-FG cost in ms on this GPU.
1. Package A (object and avatar motion vectors). Pays off for DLSS-SR even if FG never ships; test with the Motion Vector Resampling view.
2. Package B (matrices) and a stripped-down FG run: single evaluate, no UI hints, just to see the generated picture in the debug view.
3. Package C (UI split), then the real pacing and presentation (D).
4. Multi-frame generation, settings UI, resize/teleport edge cases, VRAM check (E).

## 10. Main risks

- The pacing / present rewrite touches the part of the mirror that took the most work to get stable (async, frame holding, black-frame and strobing history).
- UI split changes how the whole viewer draws its interface.
- Without good vectors for avatars the result may look worse than Smooth Motion in exactly the places Adam cares about (crowds, dancers).
- Extra latency; at 120 Hz with a real 60 fps base the benefit is real, at 125 real fps it is none.
