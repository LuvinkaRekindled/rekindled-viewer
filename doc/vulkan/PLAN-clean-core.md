# A clean core: what we would build from an empty frame, and how to get there inside this project (design note, 2026-11-05, no code)

Adam asked: with a bare Firestorm feeding us from the server and Vulkan, how would we fill the space most efficiently, with multi-threading and load balancing, to reach a similar render? Then: why can't we own the object model in the current project? This note answers both. It is a direction, not a commitment; nothing here is started.

## 1. The rule

The server's data becomes **rows in tables**; every consumer reads rows; **nothing has two owners**. Most of what blocks the current tree comes from objects that several systems own at once: group rebuilds that cannot run in parallel (static scratch, refcounted pointers shared between groups), the raw pointer held by the moved list, the texture flash (shared rows against per-frame sets), thread-local cull state (sCull, sSortSink), global render-type masks, state-sorted draw pools, the CPU naming of about 1,600 ray scene casters a frame.

## 2. The layers, in build order

1. **Frame skeleton.** Job system (work stealing, fixed worker count). Frame graph: passes declare resources, barriers are derived, passes map to the graphics or compute queue (E2 showed the compute queue pays once there are idle windows to fill). Upload ring. Bindless tables (textures, geometry, materials) addressed by generation-checked handles, never pointers or counted references.
2. **Ingest (the one part of Firestorm kept).** Network, caches, asset fetch on their own threads. Object updates become scene deltas (entity id, transform, shape, material and texture handles, flags) on a queue. No render-side objects exist.
3. **Asset boundary.** Each asset is validated, converted and finished once, on a worker, and cached: textures to BCn with per-texture stats (alpha min and max at mip 2, which is what the sun ray's card walk needs); meshes parsed with bind-matrix and range checks (the writer-bug garbage would be caught here), then LOD chains and meshlets built with meshoptimizer.
4. **Scene store.** Struct-of-arrays: transforms, bounds, geometry ref, material ref, flags. The CPU writes changed rows only.
5. **GPU-driven drawing.** GPU cull (frustum, two-phase Hi-Z) for the main view, probes and mirrors; compute makes the indirect draws; alpha objects depth-binned on the GPU. Removes the CPU cull, the state sort, the render map, the draw-info rebuilds and the cascade prep (about 5-6 ms of main thread by the 8.35 inventory).
6. **Avatars.** One job per avatar for animation, palettes written straight into a GPU buffer, one compute skin pass whose output the G-buffer, the velocity pass and the BLAS refit all read (measured vertex work is small: this is for simplicity and the ray scene, not raw speed).
7. **Ray scene.** The TLAS instance array filled by compute from the same instance table; BLAS builds budgeted on the compute queue; ray traced shadows only (no cascades exist); the sun pass keeps the centre ray plus blocker-distance filter; cards grouped by the texture stats (opaque in effect / dead / binary cutout / soft / skinned).
8. **GPU for the rest.** Particles, texture animation, texture residency (sparse textures with GPU feedback replace updateImages and its face loops).
9. **UI.** Retained, on its own thread, composited as a layer in the last pass; llui widget logic kept with a new backend.

## 3. Balancing

A three stage pipeline: simulate N+1, record N, GPU on N-1; the camera latched late, just before submit (the Hi-Z verdict latency bug is what stale camera data looks like). No CPU-against-GPU balancer: each stage reports its waits and a few dials (build budget, refit period, texture upload rate) follow measured waits, which is where the current code converged.

## 4. Why we can own the object model here

We can. The measured surface (indra/newview, 2026-11-05):

| Class | Files | References | Role |
|---|---|---|---|
| LLViewerObject | 271 | 2,886 | the server's object: protocol, scripts' view, selection. **Stays.** |
| LLVOVolume | 59 | 613 | volume semantics and geometry generation. Stays as the source; its geometry output moves. |
| LLDrawable | 75 | 941 | render-side twin of an object: spatial entry, transform and bounds, face list owner |
| LLFace | 61 | 531 | one face's geometry, texture, material, per-frame state |
| LLSpatialGroup | 39 | 683 | octree node plus its draw infos and buffers |
| LLDrawInfo | 22 | 266 | one draw record of a group |
| LLSpatialPartition | 19 | 132 | the octrees |

The render-side duplicates (LLDrawable, LLFace, LLSpatialGroup, LLDrawInfo, the partitions) are about 100 files and 2,500 references. That is a tractable surface, not the whole viewer. What makes it feel blocked is that LLFace and LLDrawable also serve picking, selection highlights, the texture tool, media, mirrors, animesh and flexi, and that the geometry generation (LLVOVolume::getGeometry, 8,089 lines) writes faces straight into vertex buffers.

Upstream merges are not a reason: pipeline.cpp is 29,129 lines and the render files have already diverged beyond a mechanical merge.

## 5. The path inside the project (strangler, each step behind a switch and checked against the old result)

Keep LLDrawable and LLFace as thin handles for the legacy callers (identity, a pointer back to the object, accessors); move the **data** into tables they read through.

- **C1. Transform, bounds and flag rows.** LLDrawable's position, bounds, visibility state and flags live in a table; LLDrawable accessors read it. The view prep jobs (cull, sort) read rows, so the ledger shape of the view rework gets simpler.
- **C2. Material and texture handles on the CPU side** instead of counted pointers. The GPU side is DONE (PLAN-texture-handles.md H0-H6: stable texture ids, a handle map, descriptors written once, fence retirement; committed 2026-10-24/25). What is left is the render-side objects: LLFace / LLDrawInfo / LLSpatialGroup hold LLViewerTexture and LLMaterial pointers (now atomic-counted, levels 1+2 of plan 8.8, which removed the crash but not the shared writes); they would hold the texture id and a material handle instead, resolved where needed. CORRECTION (2026-11-05, after re-reading plan 8.8-8.9): the shared-refcount race is ALREADY gone (atomic counts, levels 1+2, measured no cost), and what stops a parallel group rebuild now is the order dependence of registerFace (it merges each face into the previous draw info of the pass), not the counts. So C2 on its own buys no measurable time. Its value is structural: ids instead of pointers are what a face row (C3) and a draw info that is only a table record (C5) are made of, and it takes the texture system's state out of the render objects.
- **C3. Geometry rows.** A face's vertices and indices live in an arena slice owned by the table; getGeometry writes to the slice (per-thread scratch is already done); LLFace becomes a view over rows for picking and the tools.
- **C4. GPU cull replaces the group walks** for drawing; the octree remains for CPU queries (picking, collisions, extents) and is rebuilt from the rows by a worker.
- **C5. Delete** LLDrawInfo, the draw pools and the state sort; the retained fragments and the scene table (already in the tree) become the only draw path.
- **C6. Ray scene from rows**: the TLAS instances written by compute; the CPU caster naming goes.

Already in the tree and pointing the same way: resident rows and the scene table, retained group fragments, Hi-Z two-phase occlusion, GPU particles, texture animation on the GPU, the ray scene with its skinning pass, the view/ledger machinery of the view prep rework (W1-W3), atomic asset refcounts (levels 1+2).

## 6. Rules for the migration

- One owner per datum at every step; a step that leaves two is not finished.
- Old and new path both run behind a switch and are compared (the cull split check is the model: lists in order, zero differences) until a soak; then the old one is deleted.
- Judge steps by structure, not by one step's ms ([[feedback-structure-over-single-gains]] in memory); report the honest number.
- UI-facing behaviour (picking, selection, highlights, texture tool, media, mirrors, animesh, flexi) is checked by hand by Adam per step.
- Not started. First candidates: C1 (smallest, helps W4/W5) and the C2 handle table.

## 7. C2 design: lifetime (2026-11-05)

**C2.1 done (uncommitted, built, lapped):** `llhandletable.h` (LLHandleTable<T>: 32-bit handle = generation << 18 | index, fixed capacity 262,144, lock-free resolve, mutex only in acquire/release), `LLViewerTexture::getRenderHandle()` / `fromRenderHandle()` / released in the destructor, and a self-check in `LLFace::setTexture` (each texture a face uses gets a handle; the handle must resolve back). Lap c2a-phasesoff1 (RT off, 16 cores, 2026-11-05): 300,000 face uses checked, 0 did not resolve; live handles 18,900 (peak 19,121, capacity 262,144); loop 7.6 / 11.5 (turn) / 8.5 ms, no regression (not an A/B).

**The open design point.** `resolve()` gives the object back while its handle is current; it does not keep it alive. Today a draw info owns a counted reference, and that is what protects it from three kinds of reader: the group rebuild (clears the draw map), the render-map lists and fragments the pool jobs read, and the packet builders / the render worker, which read a draw info up to a few frames after the main thread dropped it. Replacing the reference by a handle is safe only if a texture whose last owner lets go is destroyed AFTER those readers are done. Options:
1. **Retire epoch (recommended).** A texture whose last counted reference goes is not deleted at once; it is parked in a frame-stamped retire list and destroyed N frames later (N = the frames in flight plus the builders' lag, 4 to start, a debug check that nothing resolved a handle to a parked texture older than N). One mechanism for textures, materials, skin infos, vertex buffers; it is the standard way (epoch-based reclamation) and it is what the GPU side already does with fence retirement. Cost: textures live a few frames longer (memory), the destructor moves to a main-thread pump.
2. **Group-held references.** The group keeps one counted reference per distinct texture it uses and draw infos hold only handles. Fails for the readers that outlive the group's clear (builders, the worker), unless the group's clear is itself deferred, which is option 1 for groups.
3. **Keep counted references, make handles additional.** No lifetime change, no gain; rows could not be plain data.
Decision for Adam: option 1. Steps after it: C2.2 the retire epoch for textures (the destructor pump, the debug check), C2.3 LLDrawInfo's texture members (mTexture, mSpecularMap, mNormalMap, mTextureList) become handles behind accessors that resolve (a dead handle gives the default image), compared against the old lists, C2.4 materials and skin infos the same way, C2.5 LLFace's mTexture[ch].

**C2.2 done (2026-11-05, lap c22-phasesoff1, RT off, unrestricted affinity, pool of 7):** the retire list for textures. LLThreadSafeRefCount::unref() asks a virtual retireOnLastRef() before it deletes (default false); LLViewerTexture parks the texture in LLTexRetire (main thread only; any other thread deletes at once), onRetire() lets go of what a new texture of the same id would collide with at the moment of parking (LLViewerFetchedTexture: the fetch request and the texture lists), LLTexRetire::pump() in LLViewerTextureList::updateImages destroys what is 4 frames old (1.5 ms budget), a texture whose count rose again in the meantime is left alone, LLTexRetire::shutdown() at the start of the texture cleanup empties the list and stops parking. LL_VK_TEX_RETIRE=0 = delete at once, LL_VK_TEX_RETIRE_FRAMES = the age. LLViewerTexLayerSetBuffer inherits the hook through two bases and says explicitly that it follows the texture. Result: 13,677 parked in the lap (25,666 over the whole session), every one destroyed, 0 revived, at most 88 waiting; shutdown clean (parked 25,666, destroyed 25,666); handle self-check 250,000 uses, 0 mismatches; loop 6.8 / 12.1 (turn) / 7.9 ms (a lighter crowd than the earlier laps: 81 avatars, 7 visible; no regression visible, not a back-to-back comparison). "handle resolutions that met a parked texture" is 0 because nothing reads handles yet: C2.3 (LLDrawInfo textures as handles) is the first reader.

**C2.3 done (2026-11-05, lap c23-phasesoff1, RT off, unrestricted affinity, pool of 7):** LLDrawInfo's mTexture, mSpecularMap, mNormalMap and mTextureList are LLTexRef (llviewertexture.h): a 32-bit handle, no reference count, reads like the LLPointer it replaces (get(), ->, *, conversion to a pointer, isNull/notNull, comparison through the pointer). A handle whose texture is gone answers the default image and counts it. The compile pass found ONE site that needed a change (pipeline.cpp: a loop over mTextureList by LLPointer&). Creation sites checked for ownership: volumes and grass take the face's texture (the face owns it), particles take the class key's texture (the part group, rebuilt every frame), the glTF preview passes none. Result: handles of a gone texture 0, "met a parked texture" 0 (no draw info read a texture whose last owner had gone within 4 frames), 26,294 parked / destroyed at shutdown, 0 revived, handle self-check 250,000 uses 0 mismatches; loop 6.3 / 11.6 (turn) / 7.5 ms with 81 avatars (not a back-to-back comparison; c22 had 6.8 / 12.1 / 7.9). A draw info no longer touches a texture's reference count when it is made, copied or destroyed.

**Plan correction after C2.3 (2026-11-05): C2.5 is dropped; faces stay the owners.** The rule is one owner per datum. A face is where a texture is USED (it holds the counted reference per channel and registers with the texture); draw infos, rows and records only REFER (handles). So LLFace::mTexture[ch] stays an LLPointer (the owner), and what changes later is what is built from it: C3 face rows hold ids, C5 deletes the draw infos. The same reading for the rest of C2.4: a material is owned by the texture entry / the object (draw info refers), a skin info by the mesh repository and the face (draw info refers), an avatar by the object list (draw info refers). Their draw-info members (mMaterial 35 uses, mGLTFMaterial 45, mSkinInfo 81, mAvatar 106) could become handles the same way, but each needs its own retire path and the payoff stays structural until rows consume them: mMaterial is "likely for debugging only" by its own comment (UI users: 18 uses in 4 files), mGLTFMaterial is read by the packet builders, mSkinInfo has the LocalMesh use-after-free history, mAvatar keeps the avatar alive for async readers (a retire path through the object list). Proposed generic hook for them: LLThreadSafeRefCount::retireOnLastRef() default calls a hook function the viewer installs (dynamic_cast to the known types), and llhandletable.h moves to llcommon so llprimitive classes can hold a handle. Not started.
