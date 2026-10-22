# S6: every pass a closed description, and a render thread loop built on them (design, 2026-10-22)

Scope agreed with Adam: S6 right after S7 part 1 ("let's complete S6 so we have all that structure done"); then the rest of S7 (GPU LOD, OIT); then the plan to remove the multi-threading blockers. Facts come from S6-census.md (sections 1-5, 14), PLAN-render-threads.md, S7-design.md (9-11) and a read of llvkasync.cpp / llvkworker.cpp / llvkrenderer.h / llshaderprogram.cpp / llrendertarget.cpp. Pass counts are from the London midnight run s6p1 (`<work>\run-logs\s6p1.log`, ~99 avatars).

## 0. What this gives, plainly

- **Frame rate: no gain expected.** S6-census section 14: the main thread (14.2 ms) limits the frame, the worker has 4 ms of slack. Recording passes on several threads would not raise fps today, and S6 does not even do that; it prepares for it.
- **Cost: a little.** Making passes closed means state that used to persist from the previous pass is sent again at the first use in each pass. Expected +0.05 to +0.15 ms main thread, worker +0 to +0.1 ms (to be measured in S6.2; the gate below caps it).
- **Value is architecture:** after S6 a pass is data that names everything it reads (target, state, programs with their uniform values, texture units, frame inputs); the worker has an explicit pass/frame structure with its state in objects, not file-level globals; a debug mode (poison) proves a pass does not read anything it did not announce; dead translation code is deleted. The later threading plan (recorders per pass, single-owner tables, ray scene off the render thread) starts from a list of what is shared and who owns it, instead of finding out by crashing.
- It is not a performance slice; I would not present it as one.

## 1. Census: where passes are not yet self-contained

Passes today. The viewer opens a pass with `LLRenderTarget::bindTarget` (`passBegin` when it clears, else `targetState`; a nested bind and the restore after `flush` are `targetState` again); the default framebuffer is `targetState(0,...)`. Per frame (s6p1): **39 producer target binds** (3 inside another pass, depth of nesting 2), **89 worker render passes** (the renderer splits on target change 37, depth read-only flip 41, layout barrier 3, copy 5.5), average **~97 draws a pass**, ~7.1k records a frame (about 180 per producer pass). Binds come from ~35 sites (pipeline.cpp, reflection probes, water, dynamic textures).

State the worker takes from *before* the pass (all of it written by earlier records of the same stream):

| class | where the worker holds it | how the producer writes it | inherited across passes? |
|---|---|---|---|
| target | `sDrawFbo`, `sGL.mFbos` attachments (`CMD_FBO_ATTACH`) | `CMD_PASS` / `CMD_TARGET_STATE` at every pass | no (explicit); the attachments are properties of the target object |
| render state block (blend, depth, cull, masks, viewport, scissor, depth read-only) | `LLVkState::received()` | `CMD_STATE` delta, written before the first reader (`stateEmit`) | **yes**: only changed blocks are sent, viewport is the only field every pass sets |
| program | `Renderer::mProgram` | draw records name it; **packet records (kinds 20-34) use the renderer's program**, sent as `CMD_PROGRAM_BIND` by `pushData` when `mSent != mProgram` | draws: no. packets: **yes** (hidden dependency on an earlier bind record) |
| program uniforms | `Program::mShadow[class]` (5 classes), copied to the uniform ring when bytes differ | `uniformRange` (changed bytes only, from the producer's per-program copy `mVkShadow`) | **yes, the big one**: values persist across passes and frames; some are set per pass (matrices, pass constants), some only when the environment changes |
| texture units | `sUnitTex[32]`, per program sampler->unit map in `Program` | `slot_set` deltas in draw records / `CMD_TEX_SLOTS` | **yes** |
| uniform buffer bindings | `sUboBinding[4]` | `CMD_UNIFORM_BLOCK` | yes |
| vertex binding | draw context | draw record carries it when it changed; a program bind clears it | partly (same program re-bound in the next pass keeps nothing) |
| constant attributes | `sGL.mConstAttrib` | `CMD_CONST_ATTRIB` | yes |
| matrices, light state | producer-side stacks / `gRender.syncLightState` | arrive as uniforms (class above) or inside packet tables | via uniforms; the S5a "black alpha" bug was exactly this class |
| frame-scoped worker tables | kind 21 prepare output (draw commands by pass type), kind 27 palette list, matrix tables, Hi-Z serial, ray scene | named by the records of the frame | frame-scoped: legitimate, but the dependency is implicit (kind 22 needs the frame's kind 21) |
| resource tables | `sGL` textures/buffers, bindless table + pins, arena | texture/buffer/resource records interleaved in the stream | shared by design: single-owner on the worker; a pass sees every upload that precedes it in the stream |
| other threads' records | `gSide` queue (texture thread, sub-streams) | `sidePush`, spliced by queue position | ordering by position only |

What this says: closing a pass is mostly about **uniforms, state block, texture units, packet program and UBO bindings**. Targets and draw records are already explicit. Frame inputs need naming, not copying. Resource tables are the threading plan's job (single-owner), not S6's.

The worker's own state is in file-level statics (`sUnitTex`, `sDrawFbo`, `sBound`, `sUboBinding`, `sCurTargets`, the target cache, `sStats` ...) and in the `Renderer` singleton (`mProgram`, `mTextures`, raster/blend/depth state, vertex input, bound pipeline): there is no object that *is* a pass. That is the other half of the gap.

## 2. What a pass description carries

```
Pass { id, parent (nesting), name (viewer zone), kind (G-buffer, shadow, lighting, post, UI, probe...)
       target { fbo, colour count, attachments }, load ops (clear colour/depth), viewport, scissor
       state0 : the full LLVkState::Block at the start
       ops    : ordered records. Every draw/packet op names its program; the first use of a program
                in the pass is preceded by that program's whole uniform classes, the first use of
                a texture unit / UBO / const attribute by its value
       frame inputs : ids of the frame-scoped tables it reads (prepare, palette list, matrices, Hi-Z)
     }
```

Implementation rule, not a new data structure on the wire: **a pass epoch**. Every pass begin bumps a counter on the producer; each producer-side cache that is a delta (state block, per-program uniform copy, texture unit table, packet program, UBO, binding) stores the epoch it was last sent in, and the first use in a new epoch sends it in full. Lazy: nothing is sent for what the pass does not use. The worker needs no new logic for correctness (resending identical bytes is already cheap: `writeRange` copies to the ring only when bytes differ).

## 3. Stages

Order is fixed: 6.0 -> 6.1 -> 6.2 -> 6.3 -> 6.4 -> 6.5 (6.2 needs 6.1's epoch, 6.4 needs 6.2's poison mode to prove the state objects are complete). Each stage is behind a switch with a verify mode until it is kept; the end state has no switch and no old path.

**S6.0 Census and pass identity (diagnostic only; LL_VK_PASSCENSUS=1).** Worker side, per pass (named by the viewer's GPU zone / target size / depth identity): records, draws, worker microseconds, and for each state class the *stamp* of the pass that last wrote it, so every read at a draw or packet record is classed as written-this-pass / written-earlier-this-frame / written-in-an-earlier-frame. Uniforms by (program, class): bytes read, bytes written this pass, programs per pass. Producer side: bytes a full class re-send would cost per pass. Output: one table of passes per frame, a summary every 600 frames. *Gate:* the table is in this doc; no behaviour change (record counts equal); switch off costs nothing. *Gain:* none (it decides S6.2's cost estimate and gives the threading plan the per-pass worker cost, which nobody has).

**S6.1 Explicit pass records.** `CMD_PASS` / `CMD_TARGET_STATE` become `PASS_BEGIN{id, parent, name, target, load ops}` and `PASS_END`; the producer keeps the pass stack (`sTargetStack` already exists); a nested bind and the restore are a child begin / end of the outer pass. The worker gets a `PassScope` it opens and closes in dispatch (per-pass stats move there). Frame-level records outside any pass belong to the frame prologue/epilogue. *Gate:* records a frame within +-1 per pass, draws and pictures equal, passes in the census match the viewer's bind sites, nested restore still correct (reflection probes, snapshots, impostors). *Gain:* none.

**S6.2 Closed passes (LL_VK_PASSCLOSE=1 on, =2 verify, =3 poison).** The epoch rule of section 2. Verify: the worker compares each re-announced value with the one it already held and counts differences by class and program; a difference means the pass would have read a value another pass left behind (a latent hazard, or a worker-written uniform that has to be excluded). Poison: at pass begin the worker forgets the inherited classes (no program, units zero, default block, UBO bindings cleared) so anything not announced shows as a missing draw / wrong picture / refused draw count. *Gate:* poison mode pictures equal to adjacent baseline runs (several scenes: London day/night, a snapshot, an impostor-heavy view), 0 `draws refused (no program / no target)` more than baseline, re-announced bytes a frame and the cost measured; cap **+0.15 ms main thread, +0.1 ms worker**, otherwise reduce what is re-sent (programs whose uniforms the pass cannot change are frame constants and are announced once per frame instead). *Expected gain:* none, slight cost.

**S6.3 Frame inputs named.** Packet records that read a frame-scoped table carry the frame/prepare id (kind 22 -> kind 21, 25/26/27 -> palette list, 34 -> Hi-Z serial already); the worker counts references to a table of another frame. Packet records name their program in the header (the `CMD_PROGRAM_BIND`-before-packets in `pushData` goes). *Gate:* 0 references to a stale table over a long run; poison mode (S6.2) still equal. *Gain:* none.

**S6.4 Render thread loop on state objects.** The worker's `execute` loop becomes frame -> pass -> record: `FrameState` (the frame-scoped tables), `PassState` (target, unit table, UBO bindings, state block, const attributes, program uniform stamps), `ResourceState` (textures, buffers, bindless, pins: single-owner, the existing `sGL` and `Renderer` tables), each with its owner documented. The file-level statics move into those objects; the `Renderer` singleton's draw state (`mProgram`, `mTextures`, raster/blend/depth, vertex input) is listed with its owner but moved only as far as it is cheap (the bulk of it is the recorder the threading plan will split). *Gate:* worker time per frame +-0.05 ms, ops and draws equal, pictures equal, `grep` shows the remaining file-level mutable statics with a reason or an owner each. *Gain:* none; this is the seam.

**S6.5 Delete translation for what moved.** From the record-kind census (which `CMD_` kinds occur in a long London + login + teleport run, which readers exist): remove record kinds and code paths with no producer left (loose per-uniform records outside sub-streams, `!gEnabled` inline-mode branches that only startup used, `CMD_TARGET_STATE` after 6.1, state getters the typed records replaced), only after a grep of every reader (feedback: grep all readers when removing a record kind). *Gate:* build clean, record kinds a frame unchanged for the kept ones, pictures equal, lines removed counted. *Gain:* none, less to maintain.

## 4. What S6 does not do (so nobody expects it)

- No pass is recorded on another thread; no secondary command buffers. That needs the `Renderer` split and the resource tables made safe to read from several threads: the multi-threading plan.
- No change to how the viewer produces passes (the bind sites stay), no GPU change.
- Not the rest of S7 (GPU LOD, OIT), not the open bugs (gray crowd clothing, outfit flicker, absent foliage) unless the S6.0 counters show them.

## 5. Risks

- Re-announce correctness hangs on the producer knowing the *worker's* uniform writes: programs where the worker itself writes uniforms (`packetUniforms`, light state, table pointers). Those are per-record writes, self-contained, but verify mode must list them by name so they are not mistaken for stale reads.
- Sub-streams (pool jobs recording into private buffers, spliced in order) start from the main producer's state: they must take the epoch with the seed; only the main producer owns the class block copies.
- Nested passes: the outer pass is a new epoch after the restore (the nested pass changed the state): its first use re-announces. Costs a few records, only in 3 places a frame.
- `Renderer` splits a producer pass into several render passes (89 vs 39): epoch is per producer pass, the worker passes inside it share its announcements.

## 6. Results

### 6.0 Census (LL_VK_PASSCENSUS=1 target binds, =2 Vulkan render passes, =3 GPU zone phases; runs pc0a / pc0b, London midnight, ~99 avatars)

What it changed in this design:

- **The viewer's render-target binds are coarse.** The main scene is ONE producer pass (llviewerdisplay.cpp:1203: 615 records, 285 draws, 52 packet records, 43 programs a frame), the post-water alpha runs another (pipeline.cpp:17148: 3,338 records, 811 packet records). The real units inside them are Vulkan render passes (79 a frame, 97 draws each) and GPU-zone phases. A pass description at bind level is right for the stream (what the viewer announces); the unit a recorder thread would take is the render pass the worker begins, and turning it into immutable resolved draw ops is the resolve/record split of the threading plan, not S6.
- **The stream is already almost closed at bind level.** The render state block is rewritten inside the pass in nearly every pass that draws (every pass sets its viewport); it is inherited only in the restored passes (3 a frame). Texture units 2.2 own / 0.8 earlier in the frame / 0.3 older per pass, UBO bindings mostly set once and inherited, the vertex binding is carried by the first draw. The packet program is inherited by no pass (it is bound inside each).
- **Uniform class blocks are the inheritance.** First reads of a (program, class) per frame: 404 blocks, 205 KB over 104 programs; of that 176 KB was written by the stream in the same pass, 9 KB in an earlier frame, 19 KB never (initial shader values or written by the worker). So announcing every block at its first use in a pass sends at most 205 KB and truly re-sends about 28 KB.
- Kind 22 / 29 records read the frame's prepare output: 426k reads in 600 frames, 0 from another frame.

### 6.1-6.3 built (commit with this note; switch LL_VK_PASSCLOSE, default off until the next stage flips it)

- **6.1** One pass record (CMD_PASS: target, colour count, viewport, load op, kind NEW / RESUME / SCREEN, pass id, parent id, bind site). The producer keeps the open-pass stack (LLRenderTarget::bindImpl / flush call passBegin / passEnd); CMD_TARGET_STATE is only the draw-buffer count now (`targetDrawBuffers`); the worker has `currentPass()`. The pass site (file:line) travels in the record and names the pass in the logs.
- **6.2** LL_VK_PASSCLOSE=1: every pass record bumps an epoch; the render state block is re-emitted, the draw context's binding and the texture units are re-sent with the next draw, the uniform buffer bindings and constant attributes are re-sent at once (llvkasync.cpp `LLVkBuf::reannounce`), and a program announces all its uniform class blocks (`uniformRangeAnnounce`, marked in the record) at its first bind in a pass (`LLShaderProgram::bind`, `announceBound` for the program that is bound across a pass begin). =2 compares every announced block with what the worker held; =3 poison: at every pass the worker forgets all of it (`LLVkWorker::pass_poison`; LL_VK_PASSPOISON is a mask of what: 1 uniform blocks, 2 units, 4 UBO bindings, 8 constant attributes, 16 vertex binding, 32 render state, 64 program).
- **6.3** Packet records name their program in the header (`mProgram`, written by pushData from the producer's draw context; the CMD_PROGRAM_BIND-before-packets and `DrawCtx::mSent` are gone; the worker validates the pointer with `Program::isLive` when it differs) and carry the producer's frame (`mFrame`); kinds 22 / 29 are checked against the frame of the last kind 21 (warn if different).

Verification, London midnight (runs s61a baseline, s61b verify, s61c / d / h full poison, s61e closed, s61f / g poison of the uniform blocks only):

- Verify: 604 class blocks announced a frame (290 KB); the ones that differ from what the worker held are the worker's own writes (the packet table / model matrix addresses of the pull programs, written by each record before it draws, so overwriting them is harmless): once a frame per program.
- Poison: 0 draws refused (no program / target / vertex input) in every pass; pictures right in 4 of 5 full-poison runs (store front, neon sign, shadows, avatars). One run (s61d) showed objects missing, with 1,367 fragments listed a frame against 1,600-1,690 in the others: the intermittent "things not loading" (repeat full poison runs s61h: normal; mask 1 only s61g: normal). "Non Finite mOrigin" viewer warnings (911 to 1,036 in s61c / d / f) did not come back in s61g / s61h with the same flags: scene dependent, not caused by poison (the stack hook that was added to find the caller never fired).
- Closed (s61e): records 4,579 a frame (baseline 4,594), main thread writing the queue 0.35 ms (0.35), worker draws 844 (846), frame interval 12.3 ms (baseline 13.9, other crowd: noise), pictures equal, no warnings. Announced volume 290 KB a frame costs nothing visible.
