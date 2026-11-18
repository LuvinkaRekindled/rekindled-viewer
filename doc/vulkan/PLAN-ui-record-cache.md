# Plan: cached UI draw records, replayed on a schedule (2026-10-27)

Goal: take the widgets' 0.65-0.70 ms (harness layout) off the main thread on the frames where a part of the UI has not been touched, without a visible difference. Status: plan, not built. Measurements: MAINTHREAD-candidates.md, "UI: where the time is".

## What the data says (laps ui1- to ui3-, uc1-)
- The widgets draw 370-600 views a frame; at depth 4 there are 16 views and they hold 0.55-0.57 of the 0.57-0.65 ms: people list 0.19, toolbar 0.09, IM box 0.075, navigation bar 0.074, status bar 0.063, camera floater 0.053, favorites 0.032, location 0.031, move floater 0.028, menu bar 0.022, parcel info, balance, time. Another layout (minimap, radar): minimap 0.075, radar list 0.07, 67 buttons 0.057, toolbar 0.045.
- 90-95% of that time is in views whose output is byte-identical to the previous frame. Some depth-4 view changes in 56% of frames (FPS text, time, check boxes).
- 12-14k vertices, 340-380 KB of records and about 360 draws a frame come out of the UI.

## Design
**Unit.** A view that is drawn by `LLView::drawChildren` / `drawChild` at depth >= 3 and whose live draw costs at least 25 us (moving average, measured with `__rdtsc` around its own draw). Not nested: a unit inside a recording is drawn live as part of the outer one. Classes that must stay live are listed by name (menus, tool tips, popups, debug view, the HUD view, anything that draws into a render target).

**Live frame of a unit** (no valid cache, its turn, or it is being used): `segmentBoundary()`, `announceBound()`, a sub-stream records the view's `draw()` (the existing `RKVkAsync::SubStream`, on the main thread), the bytes are spliced into the queue at once (the stream is exactly what a live draw writes) and a copy is kept with the key.

**Replay frame**: the key is checked, `segmentBoundary()`, the bytes are appended to the queue (`memcpy` per record, about 25 KB for the biggest units), `draw()` is not called.

**Why a segment is self-contained.** The S6.2 pass boundary already does what is needed: after a pass record the render state block, the draw context's binding, the texture units, the uniform buffer bindings and constant attributes are sent again at their first use, and a program's uniform class blocks are announced at its bind (`passEpoch`, `RKVkState::forceEmit`, `mDraw.mDirty`, `RKVkBuf::reannounce`, `RKShaderProgram::vkAnnounce`). A segment boundary is the same set without the `CMD_PASS` record: bump `gPassEpoch`, `forceEmit`, mark the draw binding dirty, then `announceBound()` for the bound program. Done at the start and at the end of every segment (recorded or replayed): the start makes the recorded records not depend on what the worker holds; the end makes everything after resend what the producer believes, so the producer's beliefs and the worker's state agree again whichever of the two ran. One more belief is on the CPU side only: `LLTexUnit::mCurrTexture` (a bind of the texture a unit "already holds" writes nothing); it is reset for all units at the start of a segment so the recording carries its binds.

**Key** (a unit replays only if all are equal): the view pointer, its screen rect, the innermost clip rect (and whether there is one), the UI scale, `LLFontGL::sCurOrigin`, the modelview matrix at entry, a hash of the producer's render state block at entry, a UI epoch (window resize, UI scale, skin or colour table reload), and the list of texture ids the records name, each still the same texture (below).

**Refresh rate.** Target `FSUIRecordCacheHz` (default 30): a unit is live every `N = clamp(round(fps / Hz), 1, 4)` frames, staggered by its pointer so the live units are spread evenly over the frames (no spike every Nth frame). N = 1 is off.

**Input: per unit, no input hooks.** A unit is live every frame while the mouse is inside its rect, while the keyboard focus or the mouse capture is the view or one of its descendants, and for 15 frames after any of those ended (so a hover or a button release is drawn at once). Everything else is sliced. A floater that is dragged or resized changes its rect: the key differs. Anything that opens or closes (visibility, a new view) is a new key or a new unit.

**Never replayed** (the unit is marked uncacheable for good, still drawn live): a recording that holds an op other than state, draw, draw scratch, texture slots, uniform writes, program bind, sampler unit, constant attribute (a pass, clear, `fboAttach`, `texImage`, buffer data, a query, a GPU zone, a sync): such a draw has effects that must happen every time (the minimap uploads its texture, previews draw into targets).

**Textures.** The ids a segment names are kept with it. `RKImageTex::deleteTextures` (the one path that frees a texture) checks the id against a small set of ids named by cached segments and drops every unit that names it. A texture that is replaced by another size or format keeps being named by the same id only if it is the same texture object; a re-created one gets a new name, so the key changes. Dynamic textures (a profile picture arriving, the minimap) show the new picture at the next live frame of the unit, at most N frames late.

**What a skipped draw does not do** (and why it is acceptable): animations that advance inside `draw()` (layout stack panels, floater fades, tab flashing) use the real frame time, so they catch up at the next live frame; `LLTexture::setActive` / bind statistics of UI textures are refreshed on live frames (their keep-alive margin is seconds); per-frame text reflow happens at the next live frame. Nothing in the world, in the simulation or on the worker reads the widgets' state from `draw()`.

## What it will and will not give
- Main thread, idle UI, harness layout: units cover about 0.55 of the 0.65 ms; at N = 3 the live share is a third: about 0.18 ms live, plus about 10 us per replayed unit (boundary, key, `memcpy`): about 0.07 ms; **about 0.3-0.4 ms saved** (N = 4: 0.4). While the mouse is over a unit, that unit is live (the people list adds 0.19 ms back). Zero gain on a frame where everything is being used; a layout with fewer or cheaper widgets gains less.
- Worker: **no change** (it plays the same records; about 360 draws and 350 KB a frame). A cached picture would also remove those, but it needs the redraw signal of its own and costs a full-screen composite; the unit key and the sliced redraw built here are what it would use too.
- Not covered: name tags (0.13), `updateUI` (0.20), the HUD.

## Risks, and how each is checked
1. **A belief that the boundary does not cover shows as a wrong texture, colour or clip on a replay frame.** `LL_VK_UI_CACHE_VERIFY=1`: on every replay frame the live draw is also recorded (not spliced) and compared with the cached bytes; the log counts mismatches per unit. In a static UI the mismatch rate must be 0 except for the views that really change (the status bar). Pictures: the same scripted spot with the cache on and off, `rt-shot-diff.py`, UI crops.
2. **Stale pictures** up to N frames (33 ms at 90 fps, N = 3). Your eyes decide N and the Hz.
3. **A widget that depends on being drawn every frame** (a timer in `draw()`, a blink, a drag preview). Found by use; `FSUIRecordCache` off per view class is a one-line list.
4. **Memory**: a cap of 8 MB of cached bytes, least recently live first dropped.
5. **Sub-stream on the main thread**: it exists and was tested on the cascades (`LL_VK_SUBSTREAM_TEST`); the UI's `LLRender::flush` goes through `drawScratch` -> `writeDraw` -> `mainReserve`, which the sub-stream buffer already intercepts (`tlsBuf`). Checked first, on a single unit, before any slicing.

## Steps (each measured or checked before the next; behind `FSUIRecordCache`, default off until you have looked)
1. `rkvkasync`: `segmentBoundary()`, `SegmentRecording` (begin / end returning the bytes and the texture ids, op scan), `replay(bytes)`. Single unit (the people floater), N = 1: records recorded and spliced at once, nothing replayed: must be byte-identical to the uncached stream (verify) and the picture identical.
2. `llrender`: the texture unit belief reset, `RKVkState::blockHash()`, texture id set in `deleteTextures`.
3. `llui`: the unit table, the key, eligibility, the schedule and the input rules; hooks in `drawChildren` / `drawChild`.
4. Counters (live, replayed, uncacheable, mismatches, bytes, cost saved per unit), the verify mode, a same-lap A/B (`ui-cache.txt`: off / 30 Hz / 20 Hz), a launcher for your own layout.
5. Laps (harness layout and yours), pictures, then you drive it: hover, type in chat, drag and resize floaters, open and close, scroll the people list, minimap, tooltips, menus.

## Status 2026-10-27: built, off by default (`FSUIRecordCache` 0); measured; waiting for Adam's hands-on check
Built as planned (rkvkasync: `segmentBoundary`, `scanSegment`, `replaySegment`, `spliceSideEffects`, texture and buffer name registries with a cache epoch; `LLRender::segmentBoundary`; `LLView::drawChildren/drawChild` -> `ui_draw_view`). Switches: setting `FSUIRecordCache` (0 / 1) and `FSUIRecordCacheHz` (30); `LL_VK_UI_CACHE=<mode>,<hz>` (0 off, 1 record only the units of `LL_VK_UI_CACHE_UNITS`, 2 sliced replay) and, with `LL_VK_FARTIER_FILE=1`, `<work>/ui-cache.txt` for same-lap A/B; `LL_VK_UI_CACHE_VERIFY=1`. Launcher `<work>\run-viewer-uicache.bat`. Log tag `UiCache` every 600 frames: per unit live / replay cost, live and replayed frames, forced by input, key misses, records, bytes, recordings and the records that made some not cacheable, verify mismatches.

**What the first laps found (all fixed):**
1. *The pending immediate-mode batch.* `LLRender` batches vertices until the next state change, so the last widget of a recorded unit was still pending when the recording ended and was flushed afterwards, outside the segment, with whatever texture the main producer believed: a white square for the "?", a neighbour's icon on hover, a missing "Developer" label (laps ucv1- / uca1-, Adam saw it as flashing). `segmentBoundary()` flushes first and the recording flushes before it ends.
2. *Verify discarded uploads.* The live draw recorded for the comparison was thrown away, with the glyph and icon uploads it did. Now the records that are not drawing are spliced (`spliceSideEffects`).
3. *Noise in the compare.* The draw record header was written with uninitialised padding (byte 44 of every draw scratch record differed): zeroed.
4. *A recording that cannot be played again* (a buffer recycled or written, a texture uploaded) first marked the unit uncacheable for good; now it is only not saved, retried at once, then backed off (1 / 20 / 120 frames); only a draw that leaves the render state changed is permanent. A segment also names the vertex / index buffers its draws read; a buffer recycled, written or destroyed bumps the cache epoch like a freed texture (`uiCacheBufferTouched`, hooks in bufferData / bufferSubData / bufferRecycle).

**Numbers (quiet region, 6 people listed, 200 fps, harness layout; same lap, `uca2-`):** widgets 0.31-0.33 ms with the cache off, 0.19-0.20 ms on (the frame interval is 5 ms: N = 4, the UI is redrawn at 50 Hz). 8 units, 40 with a saved segment, about 440-520 KB; per frame 2 live draws (0.13 ms, recording overhead 0.007) and 6 replays (0.009 ms, 1-3 us each); the units' own estimate 0.16-0.37 ms a frame, the lap shows 0.12 ms because the UI is small here. Crowd layout (people list 0.2 ms, 30 rows): estimate 0.3-0.4 ms. Verify (`ucv3-`): 444-448 replay frames checked per unit, 0 mismatches for the bottom toolbar, IM box and camera floater; mismatches only where the content changes (status bar 38 of 445: clock and FPS text; people list 6 of 444). Records and pictures: same UI, no difference seen in `ucf1-` and `ucm2-` (cache on, record only).

**Not exercised by the harness (Adam, with `FSUIRecordCache` 1 or the launcher):** hover and click (a hovered unit is drawn every frame, plus 15 frames after), typing in chat, dragging and resizing a floater (the rect is in the key), scrolling the people list, opening and closing floaters and menus, tool tips, the minimap, the radar, an inventory window, a very full people list; what looks stale and for how long (the target is `FSUIRecordCacheHz`).

## Status 2026-10-31: Adam's hands-on check done ("the ui seems fine"); `FSUIRecordCache` is on by default (1) from the next build. Off with the setting (0) or `LL_VK_UI_CACHE=0`.

