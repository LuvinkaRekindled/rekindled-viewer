# Family 2: the alpha pass (note written 2026-11-08, before any new shader)

Rules (Adam): the picture stays as it is (close output, the same feel), speed is the measure, cheap quality gains are shown to him first. ONE wanted quality change: transparent surfaces lit by the same lights as opaque ones (his hair and lamps, below). Old shaders are read for intent, fringe cases and settings; nothing is copied.

## 1. What the family is today (RT off, crowd lap gz9)

| part | GPU ms | what |
|---|---|---|
| alpha run rigged | 0.56 | the alpha blended draws of rigged meshes (hair, clothes, attachments) |
| alpha run (static) | 0.15 | alpha blended world geometry (glass, leaves cards, particles-as-prims, water edges) |
| alpha run rigged fringe | 0.07 | the fringe pass of the rigged alpha (edge pixels) |
| sum | 0.78 | about 12% of the 6.4 ms GPU frame |

Programs: `alphaPullV` (15 KB vertex, reads the rows by buffer address: positions, texcoords, colours, normals, tangents, weights/palette for rigged, the texture slots) + `alphaF` (19 KB fragment, `ALPHA_UBER`: plain, fullbright and material draws chosen per draw by a flat varying; `ALPHA_UBER_MAT`: the legacy material blend path with normal map and specular map read at run time) and `pbrAlphaPullV` + `pbralphaF` (glTF blend, small). Draw order is the blend order (back to front runs), depth test on, depth write off (avatar pool alpha writes no depth since 2026-11-01 unless RK_VK_AV_ALPHA_DEPTH=1).

Per fragment today: the texture fetch(es), `waterClip` + `mirrorClip`, the sun shadow lookup (`sampleDirectionalShadow` when shadows are on: the FULL cascade lookup, 5 taps per cascade), the atmospherics terms (`calcAtmosphericVarsLinear`), the reflection probe lookup (`sampleReflectionProbesLegacy`: ambient + gloss env + legacy env), the sun term (+ classic mode variants), the specular term, then a LOOP over the hardware light slots 1..7 (`light_position[8]`, `light_direction[8]`, `light_attenuation[8]`, `light_diffuse[8]`: slot 1 the avatar backlight, slots 2-7 the six nearest local lights), then `applySkyAndWaterFog`.

## 2. The lighting rule that causes Adam's hair / zoom problem (read in the code)

`LLPipeline::setupHWLights` fills the 8 slots: slot 0 sun or moon, slot 1 the avatar backlight, slots 2-7 the SIX lights of `mNearbyLights` that are nearest to the CAMERA (`calc_light_dist`: distance minus radius, selected lights first, moving lights a little closer), with a time fade when the set changes; spot lights take slots in the same list. The deferred opaque path lights each pixel by EVERY light whose volume touches it (RenderLocalLightCount, 256). So transparent surfaces see at most six lights chosen by distance to the camera, and zooming re-ranks them: a light drops out or enters, the hair's shade jumps while the body does not. The local light formula of the alpha shader reconstructs the radius through an "inverted attenuation work-around" (`inverted_la = ((12 fa) - 9) / la`, a leftover of reversing `setupHWLights`): a direct radius/falloff in the new shader removes that.

## 3. Keep list

1. Hair, clothes, glass and leaf cards look as they do now in daylight and in a lit room, apart from the lights they were missing (the improvement below).
2. Blend order and the fringe pass behaviour; no depth write for the avatar pool alpha; alpha-to-coverage-like edge behaviour where it exists.
3. Fullbright alpha draws: texture x vertex colour, the fog, NO lighting.
4. Material alpha draws: normal map, specular map, glossiness, environment intensity, emissive (`mix(color, diffuse, emissive)`), the glare boost of the alpha (`max(diffuse.a, glare)`), classic mode scalings (sunlit x1.35, `pow(da, 1.2)`, final x1.1, local lights x0.9).
5. glTF PBR blend draws (pbralpha): separate small program, as today.
6. `waterClip` (a draw below the water plane is clipped unless it is avatar skin), `mirrorClip` (reflection views), `minimum_alpha` discard.
7. The sun shadow on transparent surfaces (cascades), the atmospherics and water fog, the probe ambient and gloss/legacy environment.
8. Local lights: the formula (distance attenuation with the light's radius and falloff, spot cone with exponent 2, `da` from the normal), spot (projector) lights lighting alpha items by the cosine falloff without the narrow cone clip (the shader has the projector clip commented out; keep as is), no specular from local lights in the plain path.
9. His home test (four lights and the hair) and the plaza.

## 4. Settings that steer it

RenderLocalLightCount (0 turns local lights off), RenderShadowDetail (HAS_SUN_SHADOW), classic mode (sky setting), RenderReflectionProbeLevel / RenderReflectionsEnabled, RenderDeferredAtmospheric, the water plane and RenderMirrors (clips), the avatar alpha depth switch, FSAlpha* debug views (the alpha highlight debug view).

## 5. Findings from reading the code (2026-11-08)

1. **The two formulas are the same.** Opaque (`pointLightF`): `dist_atten = 2 * (1 - clamp((d/size + falloff)/(1 + falloff), 0, 1))^2`, size = radius*1.5, falloff = `getLightFalloff(DEFERRED_LIGHT_FALLOFF)`. Alpha: `inverted_la = (12 fa - 9)/la` with la = 3(1+2f)/adjusted_radius and fa = deferred falloff + 1, which works out to adjusted_radius = size, and `clamp(1-(dist-(1-fa))/fa)` = `(dist+falloff)/(1+falloff)`. So the whole difference between hair and body is WHICH lights are used, not how a light is computed. The alpha path also has the extras the opaque path does not: the spot cone `max(dot(-ln, lv), is_pointlight)^2`, no specular, classic mode x0.9.
2. The 8 slots already carry size and falloff (`LIGHT_DEFERRED_ATTENUATION`, vec2 per slot: size, falloff), so a native shader can use the opaque form directly and drop the reconstruction.
3. The deferred local light loop (`renderDeferredLighting`, pipeline.cpp, "local lights") already walks every light that passes the frustum test and builds the exact data (centre, size = radius*1.5, linear colour, falloff, classic flag) for the opaque pass: `LightVolumeRow` for points outside the box, `SpotVolumeRow` for projectors, and `fullscreen_lights` for lights the camera is inside. Fade-in/out of lights is applied by `setupHWLights` (the six slots) but NOT by the deferred loop, which draws mNearbyLights as they are: the alpha list built from the same loop gets the same lights and the same brightness as the body (a light fading out is still drawn at full strength by the body until it leaves the set, so the alpha surfaces will do the same).
4. Alpha programs are drawn by the worker; a native program replaces an old one at the draw. A native override needs BOTH stages today (manifest names a vertex and a fragment file). `alphaPullV` is 15.6 KB with variants (HAS_SKIN rigged, fullbright, material, mask cutoff, indexed draw) and passes 12-20 varyings. So a native alpha fragment means either (a) a native vertex program too (rewrite of the pull vertex: rows, skinning, material rows) or (b) a loader feature "keep the old vertex SPIR-V, replace only the fragment" with the native fragment's inputs declared at the old vertex outputs' locations.
5. Somebody already cut the probe lookup of this shader down to the ambient part (0.7 of 1.1 ms in a 2026-11-04 lap), so the per-fragment cost left is the shadow lookup, atmospherics, the 7 slot loop (an unused slot has zero attenuation, so `inverted_la` is negative and it skips the math after one `length`; a used slot does the normalize and the spot term before the cheap tests: the early `dot(lv,lv) > size*size` reject of the new loop is cheaper) and the fog.

## 6. Design

**Light list.** Per frame, built on the main thread in the same place as the deferred local light loop (no second walk): one flat array of up to 64 lights in VIEW space of the pass: `vec4 (centre.xyz, size)`, `vec4 (colour.rgb, falloff)`, `vec4 (spot direction.xyz, point flag)`: 3 vec4 = 48 bytes a light, 3 KB in all. The deferred loop's rules decide which lights go in (frustum test, `light_scale`, colour floor, attachment rule, RenderLocalLightCount), points AND spots (a spot gets the cone term as before). Lights are ordered by distance to the camera so a cap above 64 drops the farthest, as the opaque pass never would, but only in scenes with more than 64 visible lights; the list length is the first word of the block. The list reaches the worker as a small record and is copied into the native alpha program's light array when the program is selected (the old program does not have the array, so it is not copied by name: it is a "pass data" feed, one per frame, like the matrices). A shader reads it with an early reject `dot(lv,lv) > size*size` before any sqrt.

**Why not tiles first.** A flat loop with the distance reject costs about 5 ALU a light that does not reach the pixel; 32 lights is 160 ALU against a 19 KB shader. Tile binning (compute, 16x16) is the step after, only if a lap with 100+ lights shows the loop on the alpha cost. Not needed to fix the hair.

**Programs.** Short mains plus the modules `lib/light.glsl` (the list loop, one function for the point and spot terms), `lib/fog.glsl` (atmospherics + water fog), `lib/shadow.glsl` (the cascade lookup, shared with the lightmap shader's cascade function), `lib/probe.glsl` (ambient). Cases chosen by define or spec constant, not by an uber branch per fragment: plain lit, fullbright, material, each with and without rigging when the vertex needs it.

## 7. Order of work

1. **Light list feed** (viewer + worker, no shader change): build the array in the deferred local light loop, send it, copy it into any native program that declares `alpha_lights`; a debug key that makes the old alpha shader show the count of lights reaching the pixel (screenshot recipe), to check the list against what is on screen at his home.
2. **Loader: keep the old vertex stage** (choice (b) above), so a native fragment can stand in without rewriting the 15 KB vertex program first; the old vertex outputs' locations are read from its reflection.
3. **Native plain alpha fragment** (diffuse + sun + ambient + list lights + fog; no materials) behind `native_alpha`, the old path as fallback for the other cases: A/B at his home with another avatar's hair (shade should no longer change with zoom) and a zone-timer comparison on the crowd lap.
4. Fullbright, material, rigged variants; the shadow and fog modules; then cost work on whatever Nsight shows.

## 8. Decision points for Adam

- The hair/lamp fix changes the picture where it was wrong (hair lit by the same lights as the body): shown to him at his home first, by his rule for visual changes.
- The flat list cap (64) and distance order. Alternative: tiles now.
- (b) first (smaller step, keeps one old vertex stage) or a native vertex shader now (bigger, cleaner end state).

## 9. Results

### 9.1 Light list for the transparent surfaces (2026-11-08, step 1, built, uncommitted)

Not the record route of the first sketch (kind 41 to the worker, dropped: it only reaches native programs, and the native alpha fragment needs the atmosphere, probe and shadow libraries first). The list is a plain uniform array fed through the light state, so the old alpha shader uses it now and a native program inherits it by name (`Program::adoptFrom`) later:

- pipeline.cpp, `renderDeferredLighting`: the deferred local light loop appends every light it draws (points and spots, nearest first, at most `LLRender::kAlphaLightMax` = 64) as 12 floats in view space (centre + size, colour + falloff, spot axis + point flag); `gRender.setAlphaLights(...)` hands it over before `renderGeomPostDeferred`, `setAlphaLights(nullptr, -1)` takes it back after (a view whose loop did not run: -1 = the hardware slots 2-7).
- llrender.cpp `syncLightState`: a program that declares `alpha_light_count` gets `alpha_light_count` and `alpha_lights[192]` when its stamp differs from the list's (a program that does not declare them is asked once and never again).
- alphaF.glsl: slot 1 (avatar backlight) as before; with a list, a loop over it replaces slots 2-7 (both the plain path and `matPointLight`); the old attenuation arguments are rebuilt from size and falloff (la = (12 fa - 9) / size), so the formula is the opaque pass's.
- Live key `alpha_list=0` (rt-debug.txt) = the old six slots; default on.

Adam, at his home with another avatar's hair (fullbright and materials off): "picks up the light but doesn't switch on walk or camera move like the stock issue". The zoom/walk shade jump is gone.

Open: main thread cost of the per-program uploads (not measured; a shared block is the fix if it shows); GPU ms of the loop vs six slots (not measured); `pbralphaF` (glTF blend) still on the six slots; a light fading out is drawn at full strength by the list (as by the opaque pass).

### 9.2 Cost of the list in the plaza (2026-11-08, GPU-bound, RT off, 440 nearby lights so the list is at its 64 cap)

Alternating windows of `alpha_list` 0 / 1 (keycycle, 25 s, three pairs, `<work>\alist-read.py`): GPU ms per viewer frame 5.82 -> 6.33 (pairs +0.45, +0.50, +0.64), main thread frame start 5.97 -> 6.44. Same again with the exact early reject in the loop (a light beyond its size skips the rest): 5.75 -> 6.41. So the loop's arithmetic is not the cost. Stepped lap by list length (`alpha_list_max` = -1 old slots, 0 empty list, 6, 16, 32, 64): results in 9.3.

### 9.3 Cost by list length (plaza, GPU ms per viewer frame, one 25 s window each; the -1 value has two windows that agree to 0.004)

| alpha_list_max | -1 (old slots) | 0 (empty list) | 6 | 16 | 32 | 64 |
|---|---|---|---|---|---|---|
| GPU ms | 6.37 | 6.29 | 6.57 | 6.60 | 6.64 | 6.84 |

No fixed cost (an empty list is not slower than the old slots); the cost is per light that reaches a pixel: +0.3 ms for the first six, about +0.27 more for the next 58. The lap's still rounds are 60 s + 120 s; twelve 25 s windows need `H6_STILL2_EXTRA=150` (seven windows ran, the rest had no viewer). Conclusion: the native program bins lights per tile (few lights a pixel, unrolled early reject) instead of looping over up to 64 uniforms; the loop in alphaF.glsl stays only until it is accepted.

### 9.4 Native fullbright alpha (2026-11-08, first piece of the native alpha program; `native_alpha=1`, needs `alphauber=0`, default off)

Files: `native/alpha.vert` (row pull: static, rigged with HAS_SKIN + HAS_INDEXED_DRAW, particles, texture animation), `alpha_fullbright.frag`, `alpha_pass.inc` (the loose blocks both stages share), `lib/rows.glsl`, `lib/color.glsl`, `lib/clip.glsl`, `lib/atmos.glsl`, `lib/fog.glsl`; manifest lines for "Deferred Fullbright Alpha Pull Shader" and its Skin variant. Worker: `NativeDrawSwap` swaps the program for a native stand-in at the two packet draw sites (`geo_alpha_draw`, the transient `on_geometry_packets`) after the record has written its uniforms; the loader marks a program that reads `rk_tbl*` with a slot from a varying as bindless.

Found on the way: (1) the translator appends `gl_Position.z = (z + w) * 0.5` to every viewer vertex shader and the scene table's matrices are GL clip space, so a native vertex shader that takes its matrix from the table must do the same (first test: a sign drawn over the tree in front of it); (2) `const` on a buffer reference is a compile error in GLSL.

Picture (still camera in the plaza, native p2 vs old b 15 s later; noise floor = old vs old): sign area mean error 0.52 (floor 1.29), store front 2.26 (floor 4.08); the whole image 3.27 vs floor 1.11 is moving avatars and wind-moved leaf shadows on the ground (ground regions 0.7-1.7 native laps, 0.3-0.6 old/old: three laps, not explained, not plausibly this program). Cost: GPU 5.737 ms native vs 5.747 ms old (six 12 s windows each), main thread the same.

### 9.5 Native lit alpha, plain diffuse, classic skies (2026-11-08; `native_alpha=1` with `alphauber=0`, default off)

Files: `native/alpha_lit.frag` (+ `alpha.vert` with ALPHA_LIT: the eye space normal), `lib/shadow.glsl` (the cascade lookup, now shared with `sun_lightmap.frag`), `lib/light.glsl` (one light with the opaque pass's falloff and the spot cone, the avatar backlight slot, the view's light list with the early reject, the hardware slots 2-7 when there is no list), `atmos_terms_lit` in `lib/atmos.glsl`; `alpha_pass.inc` grows the lit uniforms under ALPHA_LIT. Manifest: "Deferred Alpha Pull Shader" and its Skin variant, `require shadow_matrix` (the variant with the sun shadow), `when classic_mode=1` (new manifest condition: the old program's uniform value at the draw; the probe ambient of the other skies is not native yet, those draws keep the old program). Loader: `Program::uniformWord`, and `Program::adoptFrom` matches names once per source program (it looked 48 uniforms up by name at every packet record).

Compile: vertex 7467 / 6201 words, fragment 9280 words, 6-10 ms. Picture (still plaza, native p2 vs old b): whole image 2.07 (floor 1.11), ground 0.9 / 0.3, store front 1.09 (floor 4.08); the avatar's region reads 17.9 because the avatar changed pose between the shots (sit it on the ground for a picture lap: Ctrl+Alt+S, Force Ground Sit); the hair's colour and shadow look the same in the crop. Cost, two laps of six 12 s window pairs (the list is on in both): GPU 6.136 -> 6.089 ms and 6.079 -> 6.145 ms (noise: +-0.07), main thread frame interval +0.14 ms in both (6.63 -> 6.78, 6.53 -> 6.67), unexplained, not changed by the adoptFrom cache (windows are mixed: two of six pairs the other way). Not yet checked by Adam's eyes at his home (hair, four lights).

### 9.6 Adam's check of the native lit alpha at his home (2026-11-08, `native_alpha=1`, `alphauber=0`, classic sky, 25 nearby lights)

"That works great. I can move around and zoom. Walking picks up some of the colour from the different light sources and it doesn't blink solidly between them, just the hints of colour." Same behaviour as the old program with the light list, now from the native program (the log showed 88000 draws of the static and 58000 of the rigged lit program). The glTF material preview crash that had to be fixed first is `293447d952`.

### 9.7 Native uber: lit + fullbright in one program (2026-11-08; `native_alpha=1` with `alphamat=0`, default off)

`alpha.vert` + `alpha_lit.frag` with ALPHA_UBER: the row's fullbright flag (`vary_fullbright`, flags bit 5) picks the fullbright branch (`lib/shade.glsl: shade_fullbright`, shared with `alpha_fullbright.frag`) or the lit one, so the lit and fullbright draws of the blend order are one run, as in the old merged program. Manifest: "Deferred Alpha Uber Pull Shader" and its Skin variant (`when classic_mode=1`). The default old path is the uber WITH materials (alphamat on) and draws every alpha draw; the native uber stands in for the non-material uber, which is used when `alphamat=0` (the material draws then run in their own old programs), so the native alpha pass is only complete when the material draws are native too.

Smoke lap (compile + draw, not timing): the fragment shader 9775 words, vertex 6247 words (rigged), 6.9 ms; 400001 static and 66001 rigged draws, no warnings. Timings between the old default (A), the old split (B) and the native uber (C) are left for when all of it is native (Adam, 2026-11-08: "see how the cost is all together when we're done getting it all native"); `<work>lphacycle.ps1` cycles the states in one lap (states separated by slashes).

### 9.8 Native probe module and program variants (2026-11-08)

`lib/probe.glsl` (own code, the viewer's model: the same probe selection, weights, parallax correction): `probe_gather` (the probes that reach a point: the first one in the depth bucket, its neighbour list, the void probe 0), `probe_ambient` (the irradiance cube maps), `probe_reflection` (the radiance maps at a mip level), `probe_hero` (the hero probe for glossiness above 0.75). `alpha_pass.inc` (ALPHA_PROBES) declares the named block `ReflectionProbes` (set 0 binding 1: the renderer binds the named blocks for every draw, a native program only declares it), `env_mat`, `max_probe_lod`, `cube_snapshot` and the cube array slots (`rk_ti_cubearray_<name>`: new sampler naming in the loader). The lit program uses `probe_ambient` for the skies that are not classic (`classic_mode == 0`), as the old one does.

Loader: native programs are built with the defines the graphics settings give the viewer's shaders (SSR, REFMAP_LEVEL, REF_SAMPLE_COUNT, HERO_PROBES, SUN_SHADOW: pushed from `RKViewerShaderManager` through `RKVkBridge::nativeGlobalDefines` each time the shaders are set up; a native program is rebuilt when they change); several manifest lines may name one old program, the first whose `require` / `when` hold is the stand-in (`mNext` chain). The lit and uber programs now have two variants each: `when classic_mode=1` (no probes: fragment 9791 words) and `when classic_mode=0` (ALPHA_PROBES: 14056 words). The size budget of the loader is now 30000 words / 150 ms (the compile time is what hurt in the old shaders: these take 7-9 ms).

Smoke lap (classic sky): the classic variant is chosen, 402001 static and 134001 rigged uber draws, no warnings. The non-classic variant compiled (in the combined version) but has not run: it needs a sky that is not classic.

Screen space reflections on glossy alpha materials (old path: `sampleReflectionProbesLegacy` marches the scene map for a transparent surface, `RenderScreenSpaceReflections` is on in Adam's settings): to be decided with Adam before the material program is written (see below).

### 9.9 Native material program (2026-11-08; `native_alpha=1`, the default path with materials, static and rigged)

`lib/material.glsl` (the blend path of the old material shader: specular colour and glossiness, specular and normal maps, environment intensity, glow; the sun's specular lobe from the lightFunc table; the lights' specular; gloss and legacy environment from the probes, the screen march over them, the hero probe), `lib/ssr.glsl` (the old original marcher, exponential steps then a binary search), `alpha.vert` ALPHA_MAT outputs (flags bits 6-8, specular, parameters, tangent frame), `alpha_lit.frag` branches the material draws first. Manifest: "Deferred Alpha Uber Material Pull Shader" and its Skin variant. With these the native alpha pass covers fullbright, lit and material draws in all skies (classic: no probe code unless the draw is glossy; other skies: probe ambient).

Exact savings in the port (same picture): the screen march runs once for a transparent surface (the old code ran four identical rays: glossiness 1 makes the scatter zero) and not at all when the draw's glossiness is 0 (the old code traced and multiplied the answer by that 0); the probe list is only gathered when the sky, the glossiness or the environment intensity needs it. The same two savings are NOT in the old marcher yet (`<work>\old-marcher-savings.patch`, Adam: after the timing comparison). Kept as it is: the old fade of the reflection takes the surface's eye space x and y as if they were a screen position (a long-standing quirk of the old call); fixing it would change where reflections fade, to be shown to Adam.

THE HANG (three device losses): every smoke lap lost the GPU a few seconds after the material program's first draws. Aftermath decode (`<work>ftermath-decode\decode.exe <blob> 56`): device state "Hung", ~600 warps parked on three adjacent instructions of one fragment shader (the same SPIR-V hash in two crashes), not a page fault. Bisected with manifest defines: screen march off: clean; march with made up depth and colour (no texture reads): clean; real depth: hang; an iteration clamp did not help; the sampler list and unit bindings of the native program were identical to the old one's (RK_VK_NATIVE_LOG=1 prints them). Cause: `texture()` (implicit level, needs derivatives) inside the march's loops, which end at a different step for each pixel, so the neighbouring pixels of a quad are in different iterations: undefined, and on this GPU a hang. Fix: `textureLod(.., 0.0)` for the march's two textures and the lightFunc table (all single level). RULE for native shaders: never sample with an implicit level inside a branch or loop that can differ between neighbouring pixels; the cascade shadow lookup (`sampler2DShadow`) in sun_shadow still does inside its cascade branches and has never hung, to be reviewed. Diagnostic left in: `RK_VK_NATIVE_LOG=1` (mode `nativelog` of h6-fixed.ps1), `MAT_NO_SSR` / `MAT_NO_PROBES` / `MAT_SSR_NODEPTH` / `MAT_SSR_NOMAP` / `MAT_SSR_NOTEX` defines as bisect switches.

Smoke lap after the fix: 600001 static and 70001 rigged material draws, no device loss, no copy warnings; the compile is 13 ms (fragment 24461 words; the size budget is now 40000 words / 150 ms).

### 9.10 Timing: the old default alpha pass against the native one, at Adam's home (2026-11-08, 25 nearby lights, last-location login, 6 alternating pairs of 12 s windows, GPU ms per viewer frame)

old default (uber with materials, the light list loop in alphaF.glsl) 3.862, native (`native_alpha=1`, native fullbright + lit + material, probes, the screen march) 3.646: **-0.216 ms (-5.6%)**; main thread frame interval 3.905 -> 3.705 (-0.200). Every pair agrees (the native windows are 0.20-0.23 below their old neighbours; the old windows differ by 0.04 among themselves). No device loss. Not yet measured: the plaza (440 nearby lights, the list at its 64 cap, where the old loop cost +0.5 ms), a non-classic sky, rigged crowds. `<work>lphacycle.ps1 -StatesFile <work>lphacycle-states.txt` makes this lap (the first four tries ran every window as state A: see the memory note on the case-insensitive variable).

### 9.11 The native alpha pass is the default (2026-11-08)

Adam, on the home result (no noise in the windows; -0.216 ms GPU, and his fps in the same spot went from about 266 to about 274): make it the default. Every alpha line of the manifest is `default on`; `native_alpha=0` in rt-debug.txt switches back to the old programs live. Smoke lap with no keys and the installed manifest (15 native programs): the native material programs drew 602001 (static) and 70001 (rigged) times, no device loss, no copy warnings. Still the old programs: the glTF blend alpha (`pbralphaF`), the HUD alpha, impostor alpha and the depth only alpha passes (other program names), the non-material uber when `alphamat=0` is covered, and any draw the manifest conditions refuse. Open: the plaza crowd timing, a non-classic sky by eye, glossy transparent materials by eye (Adam's lap looked fine), pbralpha native, the old marcher savings patch (after the timings), retiring the old alphaF light loop and uniform feed once Adam accepts.

### 9.12 Native glTF (PBR) blend programs (2026-11-08, uncommitted, smoke lap only)

New: `alpha_pbr.vert` (the PBR material row, the four KHR texture transforms and the tangent frame from `lib/gltf.glsl`, static and rigged), `alpha_pbr.frag` (base colour, normal map, occlusion / roughness / metallic map, emissive map, sun and cascade shadow, probes with the screen space reflection and hero probe, light list), `alpha_pbr_hud.frag` (the HUD's unlit form), `lib/pbr.glsl` (GGX / Smith / Schlick, the split sum lookup, classic mode's sRGB mixing, local lights), three manifest lines ("Deferred PBR Alpha Pull Shader", "...Skin Shader", "HUD PBR Alpha Pull Shader"), `alpha_pass.inc` gains ALPHA_PBR (`rk_packet_pbr`, `rk_ti_brdfLut`). Same terms and constants as the old pbralphaF; the cost difference is that everything that depends on the view alone (view visibility term, roughness terms, fresnel colours) is made once per surface and not per light, and a light that cannot reach the surface is skipped first.

One visible change, the same one the lit programs got: the old pbralphaF used the six nearest hardware light slots; it now takes the view's light list too (the old shader got the list loop as well, because the renderer feeds the list through the old program's uniforms: `alpha_light_count`, `alpha_lights`), so a glTF blend surface in a room with many lights is lit by all of them, not the nearest six.

Smoke lap (`h6-fixed.ps1 -Mode nativelog`, last location, 180 s): the static program compiled (7688 + 19996 SPIR-V words, 12 ms), the sampler bindings include brdfLut, probes, shadows and the scene maps, it drew (pipeline built in 179 ms), no warnings, no device loss. Not exercised: the rigged and HUD variants (no such draw in view), the picture by eye (glossy glass at SSR level, classic and non-classic skies), the timing. Nothing in the program reads a texture with an implicit level inside a divergent loop or branch (the normal map tap sits in a branch on a flat value, the same for the whole primitive).

## 10. Decision (Adam, 2026-11-08): the alpha pass goes native, so the old uber shader and its loop do not stay mixed in with the native programs

What a native alpha program needs (all of it reused by later families: fullbright, water, the resolve):

| module | content | old source read |
|---|---|---|
| `lib/atmos.glsl` | sky terms (sunlit, amblit, additive, atten) from the pass uniforms; ambient lighting term; classic mode and linear/sRGB rules | windlight/atmosphericsFuncs.glsl |
| `lib/fog.glsl` | `applySkyAndWaterFog` (above/below water cases, `atmosFragLighting` soft clip) | environment/waterFogF.glsl, windlight/gammaF.glsl |
| `lib/clip.glsl` | `waterClip` (waterSign, waterPlane), `mirrorClip` (mirror_flag, clipPlane) | deferred/deferredUtil.glsl, globalF.glsl |
| `lib/shadow.glsl` | the cascade lookup for a transparent surface (shared with sun_lightmap.frag's cascade function) | deferred/shadowUtil.glsl `sampleDirectionalShadow` |
| `lib/probe.glsl` | probe list, `preProbeSample`, `sampleProbeAmbient` (irradiance taps, weights) | deferred/reflectionProbeF.glsl |
| `lib/light.glsl` | the light list loop (point and spot, early reject), the material point light with specular | alphaF.glsl, pointLightF.glsl |
| `alpha.vert` | row pull (positions, normals, colours, texture slots, skin: weights + palette, texture animation, particles) | alphaPullV.glsl (about 100 lines for the alpha cases) |
| `alpha.frag` | plain, fullbright and material cases by specialization constant, not by an uber branch | alphaF.glsl |

Order: clip + atmos + fog (checked on the fullbright case first: no lights, no shadow, no probe), then shadow, then the light list, then probes, then materials. The old loop in alphaF.glsl is deleted when the native program is accepted.
