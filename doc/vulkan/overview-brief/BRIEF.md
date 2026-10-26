# Brief: a fresh-eyes overview of a Second Life viewer with a native Vulkan renderer: what looks inefficient, what could be updated

**Your task: read-only.** Do not build, run, change or measure anything, and do not edit any file except the one report named below. Read the code and the notes listed here, and write **`doc/vulkan/overview-brief/REPORT-fable.md`** (at most three pages) with:

1. **A ranked list of ideas** (at most 15). For each: what stands out and where (file and function, with line numbers), why it is inefficient or dated, what you would do instead, the kind of gain (main-thread ms per frame, GPU ms, VRAM, hitches, stability, maintainability), a rough size (small / medium / large work) and the main risk. Say how sure you are and what you read to be sure; mark guesses as guesses.
2. **What you looked at and found fine**, in one paragraph, so nobody re-reads it.
3. **Up to five rework proposals** (bigger than a patch: a whole area redesigned, or one design that unifies several areas and makes them easier to thread), each in a short paragraph: what it replaces, what it unifies, what runs in parallel afterwards, the first step, and how you would know it paid. Include ones you are unsure of, marked as such.

The author wants new ideas, not a re-check of what is already done: the list of "already done or decided" below is long on purpose. If something on it still looks wrong to you, say so and why, but do not spend your pages there.

## The system

- Firestorm (a Second Life viewer, C++, fork of the Linden Lab viewer) with a **native Vulkan renderer** replacing the OpenGL one (branch `native-vk`, repository `<work>\phoenix-firestorm`). The renderer is in `indra/llrender/` (`llvkcontext.cpp` device and memory arenas, `llvkworker.cpp` the render worker thread and scene tables, `llvkrt.cpp` ray tracing: acceleration structures, shadows, reflections, `llvkdlss*` upscaling) and `indra/newview/` (`pipeline.cpp` the frame, `lldrawpool*.cpp` the passes, `llspatialpartition.cpp` and `llvovolume.cpp` the scene and geometry, `llviewertexturelist.cpp` textures, `llappviewer.cpp` the main loop).
- Machine: Windows 11, AMD Ryzen 9 9950X (16 cores, 2 dies), 64 GB RAM, NVIDIA RTX 5060 Ti 16 GB. Typical load: a crowded plaza, 50k objects, 70-100 avatars. The viewer uses about 2.3 cores of 16: the main thread is one full core and the limit (loop about 15.9 ms at the crowded plaza after a turn), a render worker thread uses 0.6-0.7 cores and waits for work about 5.5 ms a frame, a fork-join pool (7 workers) does shadow cascade culling and some marking, a general work queue does texture decode, the rest is nearly idle.
- Threads: the main thread runs the viewer loop (`idle()` object and avatar update, UI update; `display()` culling, sorting, building the draw lists, UI draw). Draws are recorded by the render worker from retained "scene tables" (geometry table, texture handle table) and draw records the main thread prepares.
- Audio is FMOD Studio on the main thread (an audio thread was ruled out in `PLAN-audio-main-thread.md`; moving away from FMOD is possible some day).

## Scope: the whole viewer, with the main thread first

The author's main interest is **anything that would shorten the main thread's frame** (the limit today), then GPU cost, hitches and stability. Do not stop at the renderer. In scope, with where to start:

- **Main loop and object update**: `llappviewer.cpp` (`idle`, `display`), `llviewerobjectlist.cpp` (object update, moved list, apparent angles), `llspatialpartition.cpp`, `llvovolume.cpp`, `llvoavatar.cpp`.
- **Avatars and animation**: pose blends, attachments, rigged geometry, impostors (`llvoavatar.cpp`, `llcharacter`, `llkeyframemotion`), the per-avatar update at 70-100 avatars.
- **UI**: `llview`, floaters, the widget draw and update loops (2.2-2.5 ms a frame), name tags, the people and chat floaters.
- **Textures**: fetch, decode, priority and the texture list (`llviewertexturelist.cpp`, `llviewerfetchedtexture`, `lltexturefetch`), including memory that is held and never released.
- **Network and asset code on the main thread**: message handling, object and cache loading, media data client, anything that blocks.
- **Audio**: `indra/llaudio` (FMOD Studio on the main thread; the recent work is in `PLAN-audio-main-thread.md` and `llaudiostall.h`: a hitch of about 2.4 s was seen twice in 21 laps and is not explained, calls into FMOD such as `createSound` and `update` are synchronous). Ideas for a different audio design, including leaving FMOD, are welcome.
- **Startup, teleport, login and quit**: hitches and memory spikes (the quit dialog allocates about 1 GB of VRAM).
- **Stability**: bugs you notice while reading, in any of the above, even if not a performance issue.

## Read these first (in this order)

1. `doc/vulkan/MAINTHREAD-candidates.md`: the main thread sorted by cost, with the status after the first pass.
2. `doc/vulkan/GAINS.md`, `doc/vulkan/PLAN-native-renderer.md`, `doc/vulkan/PLAN-render-threads.md`: what the renderer is and what was gained.
3. `doc/vulkan/PLAN-texture-handles.md` sections 17.20 to 17.26 (the last days: lasting cost after a turn, host-block races, the expiry states Cold and Released). Long; skim the headings and the conclusions.
4. `doc/vulkan/S7-design.md`, `doc/vulkan/ATTACHMENTS-design.md`, `doc/vulkan/EXPIRY-design.md` as you need them.
5. Then the code. Do not stop at the notes: the point is what you see in the code.

## Already done or decided (do not spend pages on these)

- Draw records, retained scene tables, bindless textures with a handle table, render worker thread, packets for shadow, geometry, rigged and alpha passes (the alpha pass is already one record per run), the host-block race fixes (geometry-table row copies and one-rename-per-frame; see `crash-brief/ANALYSIS.md` Update 2), acceleration structure block pool and its census (compaction parked), shadow cascade prep as pool jobs in chunks, texture priority stamps (`LL_VK_TEXSTAMP`), Cold and Released scene states (built, off by default).
- Measured and rejected or parked: thread pinning (the gain is not reproducible and must never ship), a pool trim for acceleration structures (no benefit), the world-camera cull as a pool job (the cull does not split: one partition dominates), pose blends overlap (hides 0.34 ms).
- Deferred on purpose: **B** in `MAINTHREAD-candidates.md` (overlapping the cascade prep with the world cull: about 0.5-0.7 ms, needs `LLPipeline::sUseOcclusion` and `sShadowRender` thread-local and `generateSunShadow` at `pipeline.cpp` about line 21305 split into launch / join / render). The author thinks it may be worth a deeper rework for a bigger gain than the patch: **please look at this one with fresh eyes** (what would a frame look like if the cull, sort and record building were structured as a job graph from the start?).
- Open and unexplained: the frame cost that scales with the scene after an avatar turn (objects 24k to 49k, draw infos 17k to 39k, main thread +1.8-2.8 ms, about 1.2-1.5 ms of it unexplained); the UI at 2.2-2.5 ms a frame (retained widgets drawn every frame; `render_ui` 1.19 ms, `updateUI` 0.78 ms); the quit dialog spike of about 1 GB VRAM (final snapshot creates a new DLSS feature and render targets at the snapshot size); the sun shader hang (ray-query loops, one device loss population, cause unknown).

## What to look for (suggestions, not a checklist)

- Work done every frame for things that rarely change; polling that could be event-driven; full-list loops over all objects, drawables, textures, avatars, or faces; allocations in hot paths; locks and waits between the main thread and the worker; data layouts that the CPU has to walk with cache misses (pointer-heavy octree, per-drawable virtual calls).
- GL-era leftovers: structures, states, or passes designed around OpenGL's rules that a Vulkan renderer does not need (for example state sorting that exists to avoid GL state changes, per-draw uniform uploads, texture unit slots, readbacks, sync points).
- GPU side: passes that could be merged, resolution or frequency that could drop without a visible change, ray tracing cost (acceleration structure updates and rebuild policy, shadow ray count), memory that is allocated and never used, formats larger than needed.
- Memory and lifetime: things that grow after a turn or a teleport and never shrink.
- Anything that looks like a bug rather than an inefficiency: say so.

## Limits

**Full reworks of any area above are welcome** (object and avatar update, the UI, textures, audio, the scene and cull structures, how the frame is organised), especially where one redesign would unify several things and make them easier to run on worker threads: a job graph for the frame, data-oriented replacements for pointer-heavy structures, a message or snapshot boundary between the main thread and workers, one asset pipeline instead of several. For a rework say what it replaces, what it unifies, what it lets run in parallel, and a safe order of steps (the author builds and measures step by step, so each step should stand alone and be measurable). The author will not throw the viewer away; a rework has to arrive in stages on the running system. Nothing here can be profiled by you; the numbers in the notes are the only numbers. Do not ask the author for their opinions: an independent reading is the point.
