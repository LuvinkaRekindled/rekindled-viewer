# Family 2: the alpha pass (note written 2026-11-08, before any new shader)

Rules (Adam): the picture stays as it is (close output, the same feel), speed is the measure, cheap quality gains are shown to him first. ONE wanted quality change: transparent surfaces lit by the same lights as opaque ones (his hair and lamps, below). Old shaders are read for intent, fringe cases and settings; nothing is copied.

## 1. What the family is today (RT off, crowd lap gz9)

| part | GPU ms | what |
|---|---|---|
| alpha run rigged | 0.56 | the alpha blended draws of rigged meshes (hair, clothes, attachments) |
| alpha run (static) | 0.15 | alpha blended world geometry (glass, leaves cards, particles-as-prims, water edges) |
| alpha run rigged fringe | 0.07 | the fringe pass of the rigged alpha (edge pixels) |
| sum | 0.78 | about 12% of the 6.4 ms GPU frame |

Programs: `alphaPullV` (15 KB vertex, reads the rows by buffer address: positions, texcoords, colours, normals, tangents, weights/palette for rigged, the texture slots) + `alphaF` (19 KB fragment, `ALPHA_UBER`: plain, fullbright and material draws chosen per draw by a flat varying; `ALPHA_UBER_MAT`: the legacy material blend path with normal map and specular map read at run time) and `pbrAlphaPullV` + `pbralphaF` (glTF blend, small). Draw order is the blend order (back to front runs), depth test on, depth write off (avatar pool alpha writes no depth since 2026-11-01 unless LL_VK_AV_ALPHA_DEPTH=1).

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
