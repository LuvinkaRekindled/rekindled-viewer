# Regression checklist for the native-vk build (no OpenGL)

Run `<work>\run-viewer-clean.bat`, work through this in one session (about 15 minutes), then close the viewer and give the log to be read (`Firestorm.log`).
These features depend on readbacks, render-to-texture or window handling that the benchmark runs never exercise. Tick what works, note what looks wrong.

1. **Snapshot** (camera button or Ctrl+Shift+S): take a photo at the window size, save it to disk, open it. Then once at a size larger than the window (for example 4096 wide). Not black, not cropped, not stretched.
2. **Picking**: hover over objects (the hover tip names them), left-click an object, right-click for the pie menu, select a prim in edit mode (Ctrl+3), drag-select, click an avatar.
3. **Avatar preview**: Appearance / Outfits, the avatar editor, outfit thumbnails.
4. **Upload previews**: Build > Upload > Image (the preview), and a mesh upload preview if you have a file handy.
5. **Media**: the built-in web browser floater (or a media prim): page shows, scrolls, no black rectangle.
6. **Maps**: minimap, world map, a region change by teleport (loading, then the new region draws).
7. **Window**: resize the window, maximise and restore, minimise and restore (alt-tab), full-screen toggle if you use it.
8. **DLSS**: toggle the DLSS checkbox and change the mode in Preferences > Graphics while in world. Note whether unchecking it applies at once.
9. **Graphics preferences**: change the quality slider, turn shadows off and on, change the reflection setting, go underwater and come back up.
10. **Sky and time**: change the time of day (World > Environment) through dawn, noon, dusk and night; moon and stars draw.
11. **Camera**: mouselook (M), a sit camera, the flycam or zoom (Ctrl+0 / Ctrl+8).
12. **Chat bubbles, name tags, particles, a glowing object**: look right.

For each failing item, write what you did and what you saw; the log lines near the time of the problem usually name the cause.

## Result of the first run (2026-10-08)
Items 2-6 and 8-12 passed. Items 1 (snapshot reflections and DLSS) and 7 (black textures after minimize) failed and are fixed (see GAINS.md, "Regression-checklist findings fixed"). Full screen (5120x1440, started in the background so it began minimized, which is the normal reaction to losing focus): login screen and world draw, about 72 fps at that spot, about half the windowed fps for twice the pixels, alt-tab and return show no black flash and no black textures. Full screen is changed only at startup (Preferences > Graphics > Fullscreen Mode, restart); there is no shortcut.

## Result of the F1.2 run (2026-10-10, build f91c13da, items 1, 3, 4, 5, driven with <work>k-session.ps1)
- **1 Snapshot: pass.** Photo to disk at 1280x1024 (preview and saved file: not black, not cropped, not stretched) and at 4097x2178 (saved as 4100x2178, 15.6 MB, complete, correct framing). Files in `<work>\checklist_snaps\Snapshot_2026-10-10_160341_001.png` and `..._160422_001.png`.
- **3 Avatar preview: pass for what exists here.** Appearance > Outfits list (30 outfits) and the Outfit Gallery open (all hanger placeholder icons: these outfits have no thumbnails, nothing to compare); Edit Outfit Parts shows the avatar with correct baked textures. No outfit thumbnail was created (that would upload a texture).
- **4 Upload preview: pass for the image preview.** Build > Upload > Image with `burst_0.png` shows the image correctly (1024x512 uploaded size). Cancelled, nothing uploaded. Separate observation, not part of this item: the same dialog's "Preview image as" list offers Hair / Female Head / Female Upper Body / ... / Sculpted Prim; choosing Female Upper Body left the preview pane empty (no error in the log). Whether that worked before F1 is unknown (no baseline run); Adam's view is that this dialog has no 3D/body preview, so it was not pursued. A mesh upload preview was not tested (no file).
- **5 Media: pass.** Content > Search > Web loads search.secondlife.com in the floater (page draws, no black rectangle) and scrolls with the wheel.
- Side notes: the viewer throttles to about 8 fps while it is not the foreground window (frame pacing p50 122 ms, back to 38 ms when focused): not a regression. An unrelated avatar IM arrived during the run and was left unanswered.

## Added after Foundation F (2026-10-20): the paths F3.4-F5 touched
Run `<work>\run-viewer-guardcheck.bat` (the guard checks and `RK_VK_FORMAT_VERIFY=1` on), then grep `Firestorm.log` for `WARNING` lines of `PassCheck`, `StateGuard`, `MatrixGuard`, `DepthRO` and for `FormatVerify` lines with `store=None` on `image2D` / `setManualImage` (a dropped upload). Each item names what to look at.
13. **Uploads and previews**: upload an image and a texture with an alpha channel (preview floater: colours, not swapped), an animation or mesh upload preview, the object texture picker (thumbnails).
14. **Snapshots**: colour, depth, depth 24 bit (if enabled), a 360 / cube snapshot, an HUD-less snapshot, a snapshot larger than the window.
15. **Appearance bakes**: change clothes, wait for the bake (the avatar's own baked textures: alpha masks, tattoos), the morph slider preview (`LLTexLayer` readback).
16. **Media**: a web media prim and the login page (BGRA frames, sub rectangle updates), a video if one is available.
17. **Probes and mirrors**: walk around a reflective scene, a mirror, the hero probe (R11G11B10F), water reflections; look for a cube face that does not update.
18. **glTF**: a glTF material / mesh with a sampler that is not repeat / linear (wrap and filter come from the file).
19. **Minimap and fonts**: object layer dots, parcel layer, a long chat line in a rarely used glyph set (font bitmap pages update with sub rectangles).
20. **Terrain paint and dynamic textures**: paint terrain (framebuffer sub image copy), the avatar impostor, a texture preview of a baked texture.
21. **Texture compression** (Preferences > Graphics > Advanced > Compress textures, restart): one session with it on (the generic compressed storage path).
22. **Sim change**: teleport between a busy sim and a quiet one twice; note the frame time after each (open: the cumulative CPU cost, see `POST-F-REVIEW.md`).

## Lifetime stress lap (2026-11-13, GPU-LIFETIMES.md step 6)
Not always on: after a change to a channel the CPU hands to the GPU (scratch rings, palettes, arenas, handle tables, buffer writes, a new record kind, the ray scene's inputs). One lap in RT mode, launcher `h6-fixed.ps1 -Mode rt`, the knobs in `H6_EXTRA_ENV` (the launcher strips `RK_VK_*` from the shell):

`RK_VK_FRAMES=6;RK_VK_ARENA_POISON=1;RK_VK_STANDIN_MAGENTA=1;RK_VK_ARENA_BLOCK_MB=16;RK_VK_RT_EARLY=1;RK_VK_RT_MATERIALS=1`, `H6_EXTRA_ARGS='--set,FSVkMirrorQuality,3'`.

- **Pass:** no device lost, no flicker or magenta on screen (avatar shadows, textures), and in the log: `written in place` 0, `view release guard` 0, `retire list ... count rose again` 0, `PassCheck` / `StateGuard` warnings none new.
- **`RK_VK_RT_EARLY=1` is required with poison.** Poison fills a given-up range in graphics queue command order. With the ray scene on the compute queue (`RK_VK_RT_EARLY 3`, the default) nothing orders a structure build against that fill: the poison-only lap of 2026-11-13 lost the device inside an acceleration structure build (fault address in no arena block) and the six-slot + poison + magenta lap showed avatar shadow flicker; neither came back with `RK_VK_RT_EARLY=1`, and six slots alone (no poison) were clean. The viewer logs a warning when poison runs with the compute queue on. A real late read shows with `RK_VK_RT_EARLY=1` too.
- **Also run once without poison but with the default `RK_VK_RT_EARLY 3`** and `RK_VK_FRAMES=6`, and once with `RK_VK_FRAMES=2`: the frame count is the lever (a symptom that changes with it is a lifetime bug; one that does not is something else).
- **Thread rules (step 4):** every `thread phase check totals: N (healthy = 0)` line of the log reads 0 (a non-zero total names the writer: a shared queue written from another thread or beside a fork-join batch). After a change to `rkforkjoinpool.h` run one lap with `RK_VK_POOL_SELFTEST=10` in `H6_EXTRA_ENV`: `self test: ... 0 wrong, 0 still running`, and the totals line shows the 4 deliberate calls (3 from other threads, 1 during a batch).
- A device lost: decode `gpu-fault-frame<N>.bin` (`<work>\aftermath-decode\decode.exe <blob> 56`) and read the `is in arena block` line of the log first.
