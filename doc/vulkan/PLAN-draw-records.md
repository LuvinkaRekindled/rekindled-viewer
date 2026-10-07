# Draw-centric records (replaces converting GL calls one by one)

Status: design, 2026-10-07. Written after the N1b/N4 conversions (typed vertex state, texture binds, shader binds, sampler parameters, render targets) showed
that the queue cost is per record (about 17 ns), not per GL-shaped call. Typed copies of GL calls cut calls but not records, and the frame did not move.

## What the main thread writes per frame today (Adam's spot, about 95 fps)
about 28,000 records, 0.6 ms: texture binds 7.4k, uniforms 5.7k (matrix 2.9k, scalar 1.6k, vector 1.3k), draws 4.1k, array-state 3.9k, sampler parameters
(now about 0.4k), blend 0.8k, scissor 0.2k, queries 0.3k, the rest small. Every draw is preceded by about 6 records that only say "what the draw uses".

## Target: one record per draw, written as a delta
`DrawRecord { kind, state-block id, vertex binding, texture slot delta, uniform delta, draw args }`.

- **Texture slots:** a texture is an index into the bindless table (it exists for the shadow packets: 8192 slots per frame). The main thread keeps a plain array
  `slot[16]` of table indices; a draw carries a bit mask of the slots that changed and the new indices. No active unit, no bind calls, no sampler state changes
  (samplers live in the table entry or in a small set of immutable samplers chosen by the filter/wrap pair).
- **Uniforms:** values are memcpy'd on the main thread into a per-frame uniform ring (one buffer, offsets); a draw carries the offset and size of what changed. The
  per-program uniform layout is the one `LLGLSLShader` already has (index into the program's uniform block). No per-uniform records, no compare on the worker.
- **State block:** blend, depth test/write/func, cull, polygon mode, scissor enable, colour mask, stencil, program and target format are hashed into a state block.
  A new block is announced once (a record), a draw only names its id. The pipeline is built from the block on the worker (cached), not from a replayed GL state machine.
- **Vertex binding:** buffer name, index buffer, attribute mask and offsets as now (already one record), folded into the draw record when it changed.

Expected: records per frame fall from about 28k to about 5k (draws plus state-block announcements and rare binds), main-thread queue cost from 0.6 ms to
about 0.1-0.15 ms, and the worker becomes a thin executor with no VAO / bound-texture / active-unit emulation, which is what lets `llvkgl*` be deleted.
The draw record is also the unit the GPU scene needs later (indirect draw arguments plus bindless ids).

## Order (each step is measurable by the VkOps record counts)
1. Texture ids: every `LLTexUnit` bind writes the main thread's slot array only; the draw carries the delta. (removes about 7k records, the active-unit state and the sampler calls)
2. Uniform ring: `LLGLSLShader::uniform*` write into the ring on the main thread; draws carry offsets. (removes about 5.7k records)
3. State blocks: `LLGLState`, `LLGLEnable`, blend, scissor, colour mask write the context state struct; the draw names the block id. (removes about 1.3k records, the GL enable/disable emulation)
4. Fold the vertex binding and the draw into the one record. (removes about 4k records)
5. Queries and readbacks as their own commands; `LLImageGL` uploads by name with the pixel data handed over without a copy where the source outlives the call.

Measure after each step with `LL_VK_OPSTATS` (records per frame, main-thread queue ms, worker ms) at the same spot, and keep the old path behind `LL_VK_TYPED=0` until step 4 is done.

## Things that need checking before step 1
- Are there GL calls that depend on the active unit besides binds (glTexImage*, glGenerateMipmap, glCopyTexSubImage2D on the bound texture)? They become named operations (texture id) in step 5; until then they keep the existing path.
- Does anything read a bound texture back (glGetIntegerv(TEXTURE_BINDING)) on the main thread? (`LLImageGL::...` line 190 does, in a debug path.)
- Per-draw uniform values that are set by `gGL.syncMatrices()` (matrices are 2.9k of the records): they must go into the same ring.
