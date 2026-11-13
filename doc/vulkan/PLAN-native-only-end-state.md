# Native only: the end state, and the list that gets us there

Written 2026-11-13 after Adam's note: "the plan was started to convert all shaders to native and get rid of old/dead shaders, yet it doesn't seem to be set up to do either unless I keep pushing".
The cause: every plan so far (twin removal, draw info removal, shader families) was scoped to one mechanism, was called done when that mechanism was done, and left the rest as "next" items that
needed a new instruction. This document is the one list. It is finished when `python <work>\native-inventory.py` exits 0, and each step below is worked down in order without asking.
The only things that stop the work are a look decision that is Adam's (a program whose picture would change) and a failed check.

## The end state (what `native-inventory.py` counts)

- A: no program of `llviewershadermgr.cpp` loads a stock file (`mShaderFiles.push_back`): every program is a line of `native/manifest.txt`, or is deleted because nothing binds it.
- A2: the loader attaches no library file by name (`shaders.push_back(make_pair("x.glsl", ...))`, 34 files on 2026-11-13).
- B: the buffer-attribute draw paths of the old pools are gone (`pushFaceGeometry`, `renderIndexed`, `pushBufferVerts`, the `postSort` lists): every draw is a record of table entries.
- C: no stock shader file under `app_settings/shaders/class1..3` is left that no program names (the stock directories themselves are deleted when empty).

## State on 2026-11-13 (before this list)

- A: 28 programs with stock files. Post and screen passes (CAS x2, CoF, DoFCombine, Gamma x2, Post, Visualize Buffers, Vignette, Snapshot Frame, RLVa Sphere, Rim Lift, Velocity Debug, RR Guides, RR Reflection Motion,
  Ray Scene Material Debug: 17), Avatar Shader, Avatar Alpha pull, Impostor, GLTF PBR Metallic Roughness, No color alpha mask, Deferred Sun Shader (stock vertex), RT Reflection, Pathfinding x2, Object Preview,
  Preview Physics, Reflection Probe Display.
- B: `pushFaceGeometry` 3, `renderIndexed` 7, `pushBufferVerts` 6, `postSort` 39 (lines).
- C: the water files of class1/class3 are dead since today (the stock water programs are gone).

## Order

1. Commit the water work (RT water in the native pull programs, the stock Water Shader / Underwater Shader / Water Haze Shader removed, the haze under water as a table entry, the water pool's dead walk).
2. The screen passes (17 programs, one family `post`, `fullscreen.vert`, one fragment file each, close translation of the stock text): each gets its manifest line, a picture check where it can be run (CAS, DoF, gamma, vignette,
   snapshot frame are in the views driver), and its stock files deleted in the same commit. Never-bound ones are deleted, not ported (the census says which).
3. Avatar Shader, Impostor, GLTF PBR Metallic Roughness, No color alpha mask, Deferred Sun Shader, RT Reflection, Avatar Alpha pull (its stock files: the loader still reads them for the `rt` sessions).
4. The interface programs: Pathfinding x2, Object Preview, Preview Physics, Reflection Probe Display (debug / preview views: converted, never dropped).
5. B: `renderIndexed` and `pushFaceGeometry` callers (wlsky, terrain, face), `pushBufferVerts` (the debug view), `postSort`; then draw info Stage 3 (the rename is Adam's decision, section 6.9 of DESIGN-draw-info).
6. A2 and C: the library files the loader attaches, the stock directories.

## Checks

After each step: `python <work>\native-inventory.py` (the numbers must go down), `native-check-all.py --matrix` (0 failures), a build with the result read, a lap (RT off and RT on) with the ground check, the census of
unbound programs. A step that fails one of these is fixed before the next starts. Each step is its own commit.

## Not on this list (separate plans)

GPU lifetimes, the repo move, the beta; the GPU particles; the OpenAL work.
