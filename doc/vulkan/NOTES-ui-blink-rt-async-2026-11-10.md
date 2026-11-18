# UI blink with ray tracing and materials: findings so far (2026-11-10)

Status: **cause narrowed to the async compute split (E2) with ray scene materials on; the mechanism is not found.** One dedicated diagnostic run (22:35-22:43) did not support the one mechanism with log evidence. Everything here was read from the code and from the viewer's logs; the only viewer runs were Adam's. Nothing in this note is committed code except what is said under "State of the tree".

## The symptom (Adam)

- The UI, the name tags and "everything in the UI group" blink together, once in a while, for about a frame: they look bold, perhaps one pixel off, "like drawn twice over its already translucent self". Not one element at a time: all of them at once, in a cluster.
- Only with ray tracing on (RT mode 1, `LL_VK_RT_SHADOWS=1`) **and ray scene materials on** (`LL_VK_RT_MATERIALS=1`, the launcher `run-viewer-clean-rtmaterials.bat`). Not with RT off. Seen once earlier when materials were first tested, not in a while with RT on and materials off.
- Setup of the runs: DLSS 67%, ray reconstruction off, build of 2026-11-10 21:09 (steps 3a and 3b of the shadow walk removal, nothing that touches the UI), the 21:45 and later runs also had the RT penumbra filter shader change.

## Confirmed

- **The async compute split causes it.** In one session (21:53-22:02, log times local = UTC-6; the log's own timestamps are UTC): default (`rtearly=3`, ray scene built on the compute queue) blinked; live `rtearly=1` in `<work>\rt-debug.txt` at 21:55:28 (frame 9962): no blink for 2.4 minutes; `rtearly=3` again at 21:57:52 (frame 23377): the blink came back. An earlier run with `LL_VK_RT_EARLY=1` set at start (21:34-21:44, no compute queue at all) had no blink in about 10 minutes.
- It also happens with DLSS off (the sync validation run, below), so the upscaler is not involved.
- `LL_VK_RT_EARLY=1` hides the blink (it costs about 0.35 ms of GPU a frame, the A/B figure from 2026-11-05 recorded in the source and in the async compute memory note); it is a test switch, not a fix: Adam does not want the issue masked and the default stays at 3.

## Ruled out (with the evidence)

| Idea | Evidence against |
|---|---|
| The UI record cache (`FSUIRecordCache`) | still blinks with it at 0; mode 0 makes `ui_draw_view` call `viewp->draw()` directly (`llview.cpp:1598`). Its refresh interval N did flip between 3 and 4 (frame time near 9.5 ms) but it is not the cause |
| GPU hitches | about 97% of presented frames under 12 ms in the logs (VkPhases histograms), the rest mostly 12-25 ms |
| Camera jitter on the UI | the UI and the tags are drawn after the upscale inside `RKUnjitteredScope` (`llviewerdisplay.cpp:1868`), straight into the window |
| The dirty rectangle UI buffer (`RenderUIBuffer`) | off by default and in no settings file of Adam's; Adam had assumed the UI was drawn in rectangles: it is not |
| A skipped present pass in `renderFinalize` or `display()` | no early return in `renderFinalize`; no RT-dependent skip in `display()` |
| A missing barrier the validation layer can see | sync validation run, below: 0 SYNC-HAZARD lines |
| The sky-weighted penumbra fix, the fleck work | unrelated (the flecks were the RT filter, solved, see the fleck note) |

Adam's own reasoning that mattered: the sections of the UI are drawn at different times, so one section drawing twice could not blink all of them at once; the cause is below the sections (what they draw over) or after them.

## What the code does in the async path (read, no error found)

Files: `rkvkcontext.cpp` (`createDevice`, `splitFrame`, `submitCompute`, `endFrame`, `flushFrameAndWait`), `rkvkrenderer.cpp` (`Renderer::splitFrame`, `recordExternal`, `handleSet`, `bindlessFlush`), `rkvkworker.cpp` (~11925, the scene call phases), `rkvkrt.cpp` (`buildScene`, `sceneReadyBarrier`), `rkvkbridge.cpp` (the mirror frame), `llappviewer.cpp:1942`.

- E2 is on whenever RT is on, the device has a compute-only family and timeline semaphores, and `LL_VK_RT_EARLY` is unset or 3 (`rkvkcontext.cpp:946`). The frame is split into up to three command buffers: part 1 is submitted at the scene call (phase 1) with a timeline signal; the compute queue waits for that value, builds the ray scene (skin dispatch, BLAS builds, TLAS) in its own command buffer (`setComputeRedirect`), and signals; the join (phase 2) submits part 2 and sets the next submit to wait for the build's value; the last part (the lighting, the post chain, the UI) waits for it and also waits for the acquire semaphore; the frame's fence is on that last submit.
- Every buffer is created with concurrent sharing between the two queue families when async is on (`createBuffer`, `rkvkcontext.cpp:2507` and `:2802`); images are exclusive but the build does not use images.
- The build starts with a full barrier inside the compute command buffer; it orders the compute queue only, the cross-queue order comes from the timeline wait on the first split's signal (which covers the earlier graphics submissions).
- The frame draws into a persistent offscreen backbuffer (`sMirror.mBackbuffer`) and blits it to the swapchain image at the end (`endFrameImpl`); the present pass and the UI both draw into that backbuffer. One mirror frame is held open across `display()`, the reflection probe update and the snapshot floaters (`holdFrame`, `llappviewer.cpp:1942`).
- `Renderer::handleSet` does not write a texture handle's entry into the frame's handle map in a split frame (`rkvkrenderer.cpp:3229`): the first part is already on the GPU and reads that map, and a write that reached it before the descriptor was written was a device lost on 2026-11-09. The comment there says the later parts of the frame show the handle's old content until the next frame (a handle first shown shows the placeholder).

## The sync validation run (22:12-22:17, `run-viewer-clean-rtmaterials-syncval.bat`)

- Validation layer with synchronization validation, DLSS off, materials on, async on. Adam saw a few UI blinks in it.
- **0 SYNC-HAZARD lines.** Only three old messages, all noted as real defects on 2026-11-03 and still open: "Deferred Avatar Alpha Shader pull" has a fragment input at location 6 with no vertex output; "Deferred Shadow Pull Shader" and "Deferred Shadow Shader pull" write `frag_color` with no colour attachment (unused).
- The layer cannot see memory accessed through buffer device addresses (the viewer's render packets), the inside of acceleration structure builds, or the compute skinning, so it clears the barriers it can see, not this bug. The log is `<work>\run-logs\rtmaterials-syncval-20261110-221710.log`.

## The handle skip diagnostic run (22:35-22:43, Adam's last dedicated run)

- A temporary log in `handleSet` for entries a split frame's map does not get, with the part of the frame (1 = after the first split, 2 = after the join: lighting, post, UI) and whether the handle was first shown or changed; and a live key `mark=N` in `rt-debug.txt` that logs a marker with the frame number. Log: `<work>\run-logs\rtmaterials-handlelog-20261110-224353.log`.
- Counts: async runs skip 27,000 (syncval run) to 39,000 (21:30 run) entries; the no-async run skips 0. Most are part 1 (the shadow and G-buffer window), a few thousand around loading; part 2 is 0-8 entries per 1,200 frames in steady state, **all first shown, none changed**, mostly `512x512 fmt16 mip1` (probably glyph atlas pages), also a few small `fmt37` and BC7 (`fmt145`) textures.
- Adam marked **one cluster: mark 1 at frame 34662 (22:38:34 local)**. There are no part 2 entries in the 5,000 frames before it; the next one is a `512x512 fmt16` page at frame 35064, after the mark. So the handle map does **not** explain that cluster. (Adam: "at least that I caught".)
- Near the cluster the log has two dedicated 32 MB acceleration structure memory blocks created at 22:38:29 (the count of such blocks moves between 56 and 63 all run: they are created and freed all the time) and a burst of HTTP request timeouts at 22:38:21. Only a time coincidence.

## Hypotheses left, none shown

1. A hazard on memory the validation layer cannot see, shared by the compute queue and the graphics parts: buffer-address accessed data (render packets, the ray records, the material table when materials are on), the skinning output, scratch memory, the acceleration structure arena blocks (created and freed during the run).
2. Timing of something the async path moves earlier: the scene is built at phase 1 and read at the lighting, so any viewer or worker state that changes between them (bindless slots named by the ray records, the material table rows, deferred frees) is read stale; materials add the most of such state.
3. The handle map defect above (real, rare after the join, not this cluster).
4. A second UI draw in the held mirror frame (a snapshot preview with the UI): nothing seen, no floater was open.

## A second reading (another session, later the same night): nothing found, three shapes to tell apart

Read, no error found:

- Saved settings first: Adam's user settings have no override of `FSUIRecordCache`, `FSUIRecordCacheHz` or `RenderUIBuffer`.
- The full-screen draw that puts the finished picture into the backbuffer (`renderFinalize`, `pipeline.cpp` ~24182-24206) is unconditional and runs with blending off (`BlendScope blend(false)` at the top of the function), so it cannot let the old backbuffer through by its blend state.
- No pipeline was built in the background in the 22:02 and 22:43 runs (0 "pipeline built in the background" lines), so that draw was not dropped for a pipeline that was still compiling.
- `Context::splitFrame`, `submitCompute`, `endFrame`, `flushFrameAndWait`, `Renderer::splitFrame`, `on_rt_scene`: the parts are submitted in order, the last part (lighting, post, present pass, UI, the blit) is one command buffer, and a flush in a split frame records into the current part again. Nothing replays a part.
- The frame structure: `display()` opens a present frame, `swap()` presents it and opens an off-screen frame whose target is also the backbuffer (probe update, snapshots), the main loop closes that one. A draw that lands in the off-screen frame goes into the backbuffer and is covered by the next frame's full-screen draw.

"Bold for a frame" can be three different things, and the code read so far does not say which:

1. the UI drawn on a backbuffer the full-screen draw did not refresh (last frame's UI is still in it);
2. the UI drawn twice in one frame;
3. the UI drawn once with the wrong state (blend factors, filter, a sub-pixel offset).

**Not a path to take again (Adam, 2026-11-10 23:40).** Counters for those three shapes and a clear of the backbuffer at the start of a presented frame were written, built twice and removed the same night without a run: he had not asked for more diagnostics, and the clear only changes what a faulty frame would look like (black or magenta under the UI instead of the UI twice). Nothing of either is left in the tree. He wants the cause found in the code.

**Every way the scene draw could be dropped, read 2026-11-10 23:55 (the present draw of `renderFinalize`: `draw_arrays` -> `prepare_draw` -> `Renderer::draw`, `rkvkworker.cpp` 3046 and 1741, `rkvkrenderer.cpp` 4464):**

| Where it is dropped | Leaves a log line | In the six saved runs |
|---|---|---|
| the pipeline is being built in the background or failed (`applyState`, `rkvkrenderer.cpp` ~3833) | "pipeline built in the background" / "pipeline creation failed" | 0 lines |
| the frame's uniform memory is used up (`allocUniform`, 128 MB a frame) | "uniform ring exhausted" (once) | 0 lines |
| the vertex scratch and its overflow chunks are used up (`allocVertexScratch`, only for an attribute sent as a constant) | "vertex scratch ring ... exhausted" (once) | 0 lines |
| no frame is open (`prepare_draw`: the mirror could not begin one) | no | then nothing of the frame is drawn, the UI included: no doubling |
| no program, an invalid program, no backbuffer image, no target | no | fixed conditions, not something that comes and goes |

So in those runs the scene draw was not dropped. If the UI did stand on the last frame's picture, the draw was issued and had no effect (viewport, scissor, depth or colour mask at that draw), which is state, shape 3's ground.

**Adam, 2026-11-11 00:10: what he notices is the text, in the UI and in the name tags; he does not think the crosshairs do it.** Doubling is set aside. The glyph path, read end to end, no error found:

- A font keeps its glyphs in 512x512 two-channel pages (`llfontbitmapcache.cpp`). A page is made with point filtering (`setFilteringOption(TFO_POINT)`, applied at once when the texture exists, `rkimagetex.cpp` 1808). Changed sampler options are otherwise sent once a frame (`flushDirtyTexOptions`, `llviewerdisplay.cpp` 1066) or by the next bind (`llrender.cpp` 302).
- Every new glyph re-uploads its whole page (`llfontfreetype.cpp` 691: `setSubImage` of the full size goes to `setImage`). On the render side the same size and format keep the image (`ensure_image`, `rkvkworker.cpp` 704) and the data is copied in place, in order, in the frame's own command buffer (`uploadTextureDirect` -> `withCommandBuffer`, `rkvkrenderer.cpp` 4927: the render pass is ended, the copy recorded, the next draw resumes the pass). No handle changes, so the split frame's handle map skip does not touch it.
- A brand-new page shown for the first time after the join of a split frame has no entry in that frame's map: its glyphs draw from the placeholder for one frame. That is the `512x512 fmt16` entries of the handle skip log. It would show as blocks or missing characters for the few glyphs on the new page, not as all text bolder.
- Not read yet: the colours of the text draws (a shadow copy drawn in the text's colour is "bold and one pixel off", but only for text with a shadow), and whether the UI's projection or the origin text is snapped to can be a fraction of a pixel off for a frame (glyphs are point sampled, so that changes which texels land on which pixels).

**The two text paths left, read 2026-11-11 00:40. No cause found; one event that fits the timing.**

- Shadow and bold copies (`LLFontGL::drawGlyph`, `llfontgl.cpp`): bold style is the glyph twice, one pixel apart, in the text colour; a drop shadow is one copy at (+1, -1) in `sShadowColor`, a soft shadow five copies around the glyph, with the alpha scaled by the text's luminance; then the glyph itself. The colours are per vertex. Uncached text sends them in the draw record (`LLRender::flush` -> `RKVkAsync::drawScratch`, colour included when the bound shader reads it); cached text has them in a vertex buffer. Nothing found that could give the shadow copies the text's colour for a frame.
- Placement: the origin is floored and every glyph is rounded to a whole pixel (`llfontgl.cpp` 195, 371). The replay of a cached text (`LLFontVertexBuffer::renderBuffers`) sets the same matrices as the first render. Nothing found that moves text by part of a pixel.
- **The event that fits:** a text widget keeps its vertex buffers (`LLFontVertexBuffer`: buttons, line editors, scroll list cells, folder view items, badges, hover text; not found in `llhudnametag`) and rebuilds them when the font's cache generation changes (`llfontvertexbuffer.cpp` 151), which is every time a new glyph is rasterised (`llfontbitmapcache.cpp` 144). The widgets drawn after the new glyph rebuild in that frame, the ones drawn before it in the next: all cached text of the font, over one or two frames, once in a while. The same event re-uploads the font's whole page in the middle of the UI pass. Nothing else read touches all text of a font at once.
- Not shown: that a rebuild frame draws a different picture, and why only with the split frame. The rebuild's buffers on the render side were read (`write_buffer`, `rkvkworker.cpp` 2105): a host-written buffer still read by a frame in flight is renamed before the write, a device-local one is written by a copy in command order; a draw marks its vertex buffers in `build_vertex_input`. No hole found there.
- If this is taken further by reading: what a rebuild frame does that a replay frame does not, in order: several hundred new vertex buffers written and drawn in the same frame; recycled buffer names from the vertex buffer pool (`mBufferList.clear()` frees a text's buffers front to back, the rebuild takes them back last-freed first); the page upload that ends and resumes the backbuffer pass (`uploadTextureDirect` -> `withCommandBuffer`).

**Cached text no longer rebuilds for a glyph that is only added (Adam, 2026-11-11 00:25; built, not run, uncommitted).** `LLFontBitmapCache` has a second counter, `mLayoutGeneration`, bumped by `reset()` and by `noteLayoutChange()` (a wider digit in a font with tabular digits, `llfontfreetype.cpp` ~618); `LLFontVertexBuffer` compares that one in `render()` and saves it in `genBuffers()`. `LLFontWidthBuffer` still compares the old counter (widths only, nothing is drawn). Why: several hundred vertex buffers rebuilt for each new character is wasted work, and it removes the only event found that touches all text of a font at once. It is not a shown fix for the blink: stock rebuilds the same way and does not blink (Adam), so if the blink was a rebuild frame meeting the split frame it should stop; if it stays, the rebuild was not involved.

## How to resume

- **No stop-gap.** Setting `LL_VK_RT_EARLY=1` when materials are on was suggested and Adam refused it (2026-11-10): it masks the issue, and he did not ask for it. The default stays at 3 and the ordering problem is to be found. `rtearly=1` stays a test switch only.
- To confirm a blink live: start the normal materials launcher, add `rtearly=1` to `rt-debug.txt` when it blinks, remove it again.
- The next code reading, if the cause is still wanted: what the compute build reads and writes through addresses, and what the graphics parts between the split and the join write to the same buffers, material table and record buffers first; the arena block free path against the in-flight compute work.
- Launchers kept: `run-viewer-clean-rtmaterials.bat`, `-noasync.bat` (`LL_VK_RT_EARLY=1`), `-syncval.bat`, `-handlelog.bat` (only meaningful with the diagnostic build).

## State of the tree

- **The diagnostic code (handle skip block, `Context::frameCmdIndex()`, the `mark` key, tag `VkHandleSkip`) is removed again (2026-11-11);** `run-viewer-clean-rtmaterials-handlelog.bat` no longer logs anything useful.
- Committed this session and unrelated to the blink: shadow walk removal steps 1, 2, 3a, 3b (`pipeline.cpp`), the RT penumbra filter fleck fix (`db5d23a010`).
- Related notes: [[project-ui-blink-rt-async-2026-11-10]] (memory), [[project-async-compute-e1]] (the E1/E2 history), `NOTES-avatar-shadow-blink-2026-11-10.md`, `PLAN-async-compute.md`.

## Outcome (2026-11-11, after the handle map change)

- **Found while reading the handle skip log, not through the cluster:** in a split frame `Renderer::handleSet` wrote no map entry at all after the first split. A handle that the frame's map showed as the placeholder (a texture or glyph page shown for the first time, or one whose image had been cleared) therefore drew the white placeholder for the rest of that frame. Steady scene: new glyph pages alone 1 to 6 times in 1200 frames; during a load about 23,000 first sets after the split in one run. The ray shadow alpha test reads the leaf and flower textures through the same map (`LL_HM(slot)` in `shadowUtil.glsl`), so foliage shadows were affected too.
- **Change (uncommitted, `rkvkrenderer.cpp`, built 2026-11-11 01:13):** a handle whose entry in this frame's map is 0 gets its descriptor written first (`flushPendingWrites()`), then the map entry, also in a split frame. A handle that shows something keeps the old rule (old content until the next frame). Safety argument: the descriptor index comes from the free list, which only gives indices back after the frame that gave them up has finished, so no running frame's map names it; a running earlier part can reach the new entry only through a stale row that still carries a recycled handle number, and then finds a written descriptor, as the non-split path has it inside one command buffer.
- **Adam's first run with it:** the load came in like RT off, nothing flashed after it, bushes and flowers looked more shadowed and detailed, white textures filled back in after a turn away and `kill` clearing, and the UI did not blink at all in that session. Not yet confirmed over a long session.
- **Not explained:** the handle log of the marked cluster (frame 34662) showed no handle set after the split in the frames before the mark. So the log does not show that cluster being caused by this; the change and the end of the blink coincide. If the blink returns, `rtearly=1` live in the same session is still the first test, and the diagnostic (`VkHandleSkip`) was removed with the commit; it is in the history of this note (`9eda5e2ef6`, `2c4aa94772`) as a description only, the code was never committed.
