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

## State (the inventory script prints it; first count 2026-11-13: A 28, later in the day 13)

- A: stock programs left (13): Avatar Shader, Deferred Avatar Alpha Shader pull (stock files for the `rt` sessions), Deferred RT Reflection Shader, Deferred Sun Shader (stock vertex, the RT sun fragment), GLTF PBR Metallic
  Roughness, Impostor, No color alpha mask, Object Preview, Pathfinding x2, Preview Physics, Ray Scene Material Debug, Reflection Probe Display. Done today (step 2): CoF, DoFCombine, Deferred Post (DoF), Buffer
  Visualization, Avatar Velocity Debug, Vignette, Snapshot Frame, CAS x2 (cas.frag generated from the stock file by <work>\make_cas.py; the CPU half of that file is a C++ header now, `ffxcas.h`), Gamma x2,
  RLVa Sphere, DLSS-NR Rim Lift, RR Guides, RR Reflection Motion.
- A2: five stock file lines with computed names (llviewershadermgr.cpp: the sun program's fragment choice and the loop over the shader list at the end).
- A3: 17 manifest lines with `rt`: the lit alpha programs (Deferred Alpha Pull x2, Uber x4, PBR x2, Material Pull 1/5/9/13 x2) and the avatar alpha program keep a stock program in a session with ray queries. The RT track's
  port: the same as the water (lib/rt.glsl is the stock ray query code as a native include; a manifest define LL_RT_QUERY gives the program the extension and set 3). The sun lightmap's RT program and the RT Reflection
  Shader are the same kind.
- B: `pushFaceGeometry` 3, `renderIndexed` 7, `pushBufferVerts` 6, `postSort` 39 (lines).
- C: errorF / errorV (the old loader's fallback shaders: they go with the old loader path, step 6).

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
