# Family 3: the G-buffer fill and velocity (note started 2026-11-08, before any new shader)

Adam's go, 2026-11-08: "G-buffer fill" is the next family (after the alpha family; the lighting resolve's leftovers, softenLight 0.23 ms and the haze passes, come after or alongside). Rules in force: speed first, the picture stays (PLAN-shaders-rewrite.md sections 11, 15, 16), programs small and not 1:1 with the old ones, anything visibly different is shown to Adam first.

## 1. What the family is today (read in the code, RT off, crowd lap gz9: 0.59 ms opaque scene table draws + 0.07 terrain + 0.19 avatar velocity of a 6.4 ms GPU frame)

**What a fill shader does.** Almost nothing. The G-buffer shaders are 1.6 to 4.6 KB of GLSL:

| program family | fragment source | what it writes |
|---|---|---|
| legacy diffuse (the "Deferred Diffuse Pull" family, the bulk of prims) | diffuseF.glsl, 1.6 KB | albedo = vertex colour * diffuse map, spec = vertex alpha in all four channels, normal target = normalize(normal) + env (vertex alpha) + flag HAS_ATMOS |
| legacy material (normal / specular maps, alpha mask, emissive) | class3 materialF.glsl (the class1 file is a stub), about 10 KB with its blend branch | albedo rgb + emissive in alpha, specular rgb + glossiness, normal (map applied) + env intensity + flag; the blend branch is the old forward alpha path (dead in the Vulkan packet path: the alpha family replaced it) |
| glTF opaque and mask | pbropaqueF.glsl, 4.6 KB | albedo (linear), occlusion / roughness / metal, TBN normal (flipped for back faces) + flag HAS_PBR, emissive only if the emissive buffer is on |
| terrain, avatar, eyes, tree, bump, fullbright, emissive, impostor, HUD copies | terrainF 2.3 KB, avatarF 1.6 KB, bumpF 2.1 KB, fullbrightF 2.8 KB, pbrterrainF 14.7 KB | the same targets |
| velocity | avatarVelocity* / objectVelocityPull / motionVectors | motion vectors for DLSS (0.19 ms) |

Variants multiply them: alpha mask / no colour, skinned, indexed, HUD, impostor, 78 variants of the material fragment alone. Targets (pipeline.cpp `addDeferredAttachments`): albedo (the target's own first attachment), ORM / specular RGBA8, normal RGBA16 (RGB10A2 without HDR), emissive RGBA16F only with `RenderEnableEmissiveBuffer` (default off: glTF emissive then clamps at 1.0 in the 8 bit albedo). 16 to 20 bytes a pixel written, read once by the resolve.

**Where the time is NOT.** The research of 2026-11-08 (PLAN-shaders.md section 5) found the same for the cascade alpha casters: fragment shader work and texture fetches were noise; draws that do nothing (a draw command for every row, culled ones empty) cost 0.08 to 0.19 ms a pass; the PC sampler's warp share is not milliseconds. And the overshading count (PLAN-shaders-rewrite.md section 8): the opaque G-buffer shades about 3.2 fragments per screen pixel; the rigged crowd is vertex bound (234k triangles, 14k fragments in one type). So the shaders themselves are not where this family's milliseconds are; a native port of diffuseF / materialF / pbropaqueF would be the blur story again (same ms, expected, and worth doing only as the base for what follows: one small program per kind, specialization constants instead of 78 copies, own code).

## 2. Keep list (must not change; each needs a test spot)

1. Plaza ground shine and every reflection source: the albedo / spec / glossiness / env values written per pixel must stay bit-close (the resolve's shine comes from them). Test: plaza, night and day.
2. Legacy materials: emissive in the albedo alpha, glossiness from the normal map's alpha, env intensity = env * specular map alpha, vertex alpha doubling as the spec value of plain prims (the diffuse shader writes it into the spec target and the env slot). Test: a shiny prim and a normal-mapped prim.
3. Alpha mask edges (the 1/512 bias that stops acne at 8 bit alpha boundaries), glTF mask cutoff, back-face normal flip, mirror clip plane.
4. The flags in the normal target's w (HAS_ATMOS, HAS_PBR, SKIP_ATMOS, HDRI): the resolve, DLSS and the RT shaders read them.
5. Velocity: DLSS motion vectors for avatars, rigged objects and moving objects stay correct (hair and attachments were a glitch hunt before).
6. glTF emissive: stays as is (clamps at 1.0 with the emissive buffer off) unless Adam approves the HDR emissive field below.

## 3. Settings that steer the family

`RenderEnableEmissiveBuffer` (third target), `RenderHDREnabled` (normal format), `RenderDeferredSSAO`, classic mode (sets the material tweaks, not the fill), `FSTexAnimOnGPU` (texture animation in the vertex shaders), the draw packet path switches (live keys `native_*` do not touch the fill yet), mirrors (clip plane), impostor render.

## 4. Where the cost can go (candidates, each to be measured before any code; none is a decision)

1. **Measure the floor first.** Like the cascade casters: a live key that culls every row of the main G-buffer pass (zero sized draws) and one that clips every triangle after the vertex stage give the cost of the draws alone and the vertex part; what is left is the raster and pixel part. What the code says before any lap: the main pass commands are built on the CPU per listed fragment (`mRCmds` / `mRRuns` in llvkworker.cpp, the C4.5 visible set), so the culled objects are not in the list the way every row is in the cascade casters' resident cull; only draws the Hi-Z box test zeroes after the list is made would be empty ones. So the empty-draw lever of PLAN-shaders.md 5c is probably small here: the floor key tells, and the answer decides whether the compacted draws (append visible draws, `DrawIndirectCount`) apply to the main pass at all.
2. **Depth pre-pass experiment** (PLAN-shaders-rewrite.md section 8, option E): draw the opaque types depth only first, then the G-buffer at equal depth, and re-measure invocations (3.2 a pixel now) and ms. Cheap vertex work for the crowd is the risk (it doubles the vertex stage), so the pre-pass is for the static types; test per type.
3. **Fullbright and emissive-only surfaces straight to colour** (option C): skip the G-buffer write and the lighting read for them. Needs the resolve to leave those pixels alone (stencil or a flag test), so it pairs with the resolve work of family 1.
4. **Slim packing** (option A): normal RGB10A2 (4 bytes instead of 8), flags folded in: 12 to 16 bytes a pixel. Changes every reader (resolve, RT, DLSS inputs, SSR): measure the bandwidth gain with a throw-away format change first; expect tenths of a ms at most.
5. **One small native program per kind with specialization constants** (alpha mask, normal map, specular map, rigged, HUD) instead of the 78 variants: the compile and cache side gets simpler and the shader text becomes our own; the ms gain on its own is expected to be about zero (the blur lesson), so it is the base, not the prize.
6. **HDR emissive field** (R11G11B10F or RGB9E5): a visible improvement (glTF emissive above 1.0, glow that blooms) for Adam to approve; costs a little bandwidth when on.
7. **Visibility buffer** (option B): the largest rewrite for at best a few tenths of a ms; decided after the pre-pass experiment, not before.

## 5. First steps (proposed)

(a) the floor keys of candidate 1 in the main pass, one lap (needs a crowd spot or Adam's home: the plaza is not available; compare only inside a lap); (b) from the numbers, either the compacted main-pass draws or the depth pre-pass experiment; (c) the native fill programs as the base for the packing and emissive changes.

## 6. Decisions for Adam

1. Is an HDR emissive field wanted (visible: emissive above 1.0 stays above 1.0)?
2. Slim packing changes the G-buffer format that DLSS, the RT shaders and SSR read: go ahead once the throw-away test shows a gain worth it?
3. Where to run the floor lap: his home (small scene, 3.8 ms GPU, no crowd) or a crowd spot when he is there.

## 7. Results log

**7.1 The floor of the G-buffer pass (2026-11-08, lap gf1-phasesoff1, Adam's home, RT off, zone timers on, nine 18 s windows: key values 1 / 2 / 3 three times each).** The measuring key is `gfloor` in rt-debug.txt (llvkworker.cpp, `geo_draw`; 1 = normal, also when missing, 2 = every command of the table draws made empty, 3 = a zero scissor round the draws: the vertex stage runs, every triangle is thrown away before the rasteriser). `<work>\zones-by-window.py <lap>` averages the zones per key value. GPU frame 3.60 / 3.09 / 3.14 ms (the rest of the drop is the lighting passes and the lit surfaces that are gone from the picture, so only the G-buffer zones count here):

| zone (GPU ms) | normal | empty draws | clipped after vertex |
|---|---|---|---|
| pool: materials | 0.088 | 0.006 | 0.002 |
| pool: simple | 0.073 | 0.005 | 0.000 |
| pool: alpha mask | 0.009 | 0.000 | 0.000 |
| scene table type 10 rigged | 0.040 | 0.003 | 0.034 |
| scene table type 7 rigged | 0.039 | 0.003 | 0.026 |
| types 5, 11, 2 rigged | 0.011 / 0.010 / 0.010 | 0.001 / 0.001 / 0.000 | 0.009 / 0.009 / 0.008 |

Reading: (1) the draws themselves are free in the main pass (empty draws: 0.01 ms for everything), so the compacted-draw lever of the cascade casters does not apply here, as the code reading said. (2) The static opaque fill (pool materials + simple, 0.17 ms here) is all raster and pixel work: with every triangle clipped after the vertex stage it is 0.002 ms. The levers are on the pixel side: the overshading (depth pre-pass), the bytes written (packing), fullbright straight to colour. (3) The rigged types are the opposite: with every triangle clipped they keep about 80% of their time (type 10: 0.040 -> 0.034, type 7: 0.039 -> 0.026): the skinning vertex work is the cost, not the pixels, as the plaza census said. At this scene the whole fill is about 0.3 ms; the crowd spot has far more rigged work, so the rigged share there is the part to look at (needs Adam parked at the crowd). (4) Side effect seen in the same table: with the geometry gone the sky shaders run over the whole screen (haze 0.023 -> 0.16, clouds 0.018 -> 0.10), which confirms the late sky saves that cost in a normal frame.

Also in this lap: the old marcher savings patch (reflectionProbeF, screenSpaceReflUtil) compiled with no shader warning.
