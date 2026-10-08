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
Items 2-6 and 8-12 passed. Items 1 (snapshot reflections and DLSS) and 7 (black textures after minimize) failed and are fixed (see GAINS.md, "Regression-checklist findings fixed"). Full screen (5120x1440, started in the background so it began minimized, which is the normal reaction to losing focus): login screen and world draw, about 72 fps where windowed 2558x1360 gave about 2.5x that, alt-tab and return show no black flash and no black textures. Full screen is changed only at startup (Preferences > Graphics > Fullscreen Mode, restart); there is no shortcut.
