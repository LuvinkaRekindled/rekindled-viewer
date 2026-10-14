# Plan: quality of the shading inside the traced glossy mirror

Written 2026-10-13 (end of the mirror flicker session). Repo `<work>\phoenix-firestorm`, branch `native-vk`. Status: NOT started. Decided with Adam: the correctness work of the traced mirror (dropouts, flicker) is done for now; this is the quality work, to be done later and probably offered as a **preference**: whoever wants the best mirror can pay for it, whatever it costs. Read `NEXT-glossy-mirror-fps-ab.md` first for the state of the traced mirror and the test tools.

## 1. What is wrong with the look

Everything seen inside the traced reflection (`glossyrt=1`: the avatar, the walls, the globe, the pictures) is shaded by `rtReflection()` in `indra/newview/app_settings/shaders/class1/deferred/shadowUtil.glsl` with

    color = (amblit_linear + N.L * sun_shadow * sunlit_linear) * albedo + emissive      (fullbright surfaces: albedo + emissive)

that is plain diffuse lighting, close to the albedo G-buffer view (RenderBufferVisualization 0). Next to the stock hero probe mirror, which shows the scene lit by the full PBR path of the main view, it looks flat. Missing:

1. specular: no highlights, no roughness or metallic response (metal and glossy surfaces look matte);
2. no normal maps (brick, fabric, wood lose their relief);
3. no point and spot lights (the lit circles on the floor, lamps, the projector circle Adam stands in);
4. one flat ambient colour (`amblit_linear`, the sky average) instead of the irradiance probes the main pass samples;
5. a hit far away fades to the stock probe colour (`fade = smoothstep(250, 1200, t)`), fine.

Not wrong, do not "fix": hits and misses (steady), geometry, shadows (the sun shadow ray of the hit is right: tested in the flicker hunt), the missing avatar body and the vanishing far objects (fixed, see the notes).

## 2. What the hit already gives the shader (no new data needed for steps A and B)

`rtShadeSurface(record, prim, bary, lod, ...)` decodes the hit: the geometry record (`LLRtGeom`: vertex, index, texcoord, colour and normal buffer addresses, flags) and the material row (`LLRtMaterial` / C++ `GpuMaterial` in `llvkrt.cpp`): `slots` (base colour textures, pin indices), `slots2` (**normal map, metallic-roughness or specular map, emissive map**), `color`, `emissive`, `params` (cutoff, **metallic, roughness**), `uv` transforms, `flags` (bit 0 fullbright, bit 1 glTF). It samples with `textureLod(ll_rt_tex[nonuniformEXT(slot)], uv, lod)` where `slot = ll_rt_slot(pin)`. It already computes the smooth normal when the record has normals (flag 1024). So the normal map slot, the roughness and metallic values and the specular map are in reach: they are read for the base colour and emissive only today.

## 3. Steps (each visible on its own, each checked against the stock mirror)

**A. Material response at the hit.** Read the normal map (needs a tangent: the record has no tangent buffer; derive it from the triangle's uv and positions as the viewer's `pbr` path does when there is none), roughness and metallic (glTF: metallic-roughness texture channels times the factors; legacy: the specular map and the shininess in `params`). Light the hit with the same BRDF as `pbrBaseLight` (`pbrPunctual.glsl`) for the sun: diffuse + GGX specular with the view direction = the reflection ray reversed. Cost: 1 to 2 more texture reads per hit and a BRDF evaluation. This is the biggest visual gain.

**B. Ambient at the hit.** Replace `amblit_linear` by the irradiance probe lookup at the hit's normal (the main pass does `sampleReflectionProbes` / `adjustIrradiance`; the hit is not at a screen pixel, so use the nearest probe's irradiance and radiance cubes sampled in the hit normal and reflected direction; a hit that is far from every probe keeps the sky average). Cost: one or two cube array reads.

**C. Local lights.** The main pass loops over the lights near the pixel (`LIGHT_LOOP`). At a hit position the same list is not available per pixel; options: (1) pass the few strongest lights near the mirror (the lights in the lighting pass uniforms, top N by intensity at the hit's cell) and loop over them; (2) use the light list of the probe update; (3) skip lights for hits farther than a distance (cheap, most of the visible benefit is near the mirror). Shadows of local lights are not traced (soft shadows from the sun only). Cost: N light evaluations per hit: this is the expensive one and the reason for a quality level.

**D. (later, not part of this plan)** reflection of reflections (second bounce), transparency in the reflected scene, ray reconstruction: see `PLAN-ray-tracing.md` (DLSS Ray Reconstruction is a different NGX feature with its own guide buffers: not an easy add).

## 4. The preference

Offer the mirror as quality levels (setting name suggestion `FSVkMirrorQuality`, shown next to the other ray tracing options in the Vulkan preferences tab, `patch-pref-tab.py` shows how the tab is built):

| Level | Mirror | Cost |
|---|---|---|
| 0 | stock hero probe mirror (`glossyrt=0`) | none |
| 1 | traced, diffuse hit shading (today's `glossyrt=1`) | measured +0.3 to 0.4 ms GPU at 15 % of the screen, fps A/B pending |
| 2 | traced + material response and probe ambient (steps A, B) | to measure |
| 3 | traced + local lights (step C) | to measure, highest |

In the code the level is one integer uniform of the lighting pass (the same place `rt_glossy_reflections` is set in `pipeline.cpp`, next to `rt_hit_shadow_variant`), read by `rtReflection` to choose how much of the hit shading runs. The live switch `glossyrt` stays for tests. Adam's wish: the option is for people who want it "regardless of the cost": do not make level 2 or 3 the default.

## 5. How to test it (tools exist, see `NEXT-glossy-mirror-fps-ab.md` and memory `project-glossy-mirror-flicker-hunt`)

- Compare the same view in the stock mirror and the traced mirror: `rt-phases.ps1` switches `glossyrt` live in a running viewer and beeps the phase number; screenshots with `vk-burst.ps1` (PrintWindow, timestamped) and difference images with PIL (see `strobe-glossy` for examples). The stock mirror is the reference for how bright and how detailed the room should look.
- Locked 'Afternoon' sky for daylight, the Larsson mirror room, avatar standing in front of the mirror; also a night sky (the moon) and the projector circle for the local lights.
- fps cost per level: `hunt.ps1 -Lean -Configs "glossyrt=1|..." -Hold 45 -Settle 60` and `python <work>\ab-fps-report.py <name> --skip 1` (per-setting medians); GPU ms per pass needs the diagnostic config (`LL_VK_PHASES=1`, drop `-Lean`).
- Do not touch the viewer window during a session (an unfocused viewer drops to single-digit fps).

## 6. Files

`shadowUtil.glsl` (`rtReflection`, `rtShadeSurface`, `rtSunRay`), `softenLightF.glsl` (class3/deferred: where `rtReflection` is called and the result mixed into `radiance`), `pipeline.cpp` (~line 12545: the uniforms of the soften pass), `llvkrt.cpp` (`GpuMaterial`, `LLRtMaterial` declaration is in `llvkshader.cpp` ~line 1025 if a field is added: keep both in step, the row is 96 bytes), `pbrPunctual.glsl` / `pbrBaseLight` (the BRDF to reuse), `llvkmirror.cpp` (`rtGlossyReflections()`).

## Status 2026-10-13 (night): steps A and B built and seen

Built as the live switch `hitq` (rt-debug.txt, default 0; 1 = lit hits). `rtTraceHit` / `rtShadeSurfaceEx` (shadowUtil.glsl) trace and decode the hit (normal map by a tangent frame from the triangle's uv derivatives, glTF metallic-roughness map times the factors, legacy specular map); `rtReflection` is a flat-shading wrapper of it, so the water is unchanged. `rtReflectionLit` (softenLightF.glsl) lights a glTF hit with the main pass's `pbrBaseLight` and a legacy hit with the blinn-phong lines of main(), both with the reflection probes sampled AT THE HIT (`preProbeSample`, `sampleProbeAmbient`, `sampleProbes`).

Seen in the Larsson mirror room (afternoon sky locked, pose locked, `rt-hitq-ab.ps1`: live phases, beeps, PrintWindow pictures in run-logs/hitq-ab3): mean brightness of the mirror region against the stock hero probe mirror (stock / flat / lit): floor 46.5 / 83.9 / 38.7, wall pictures 214 / 198 / 198, globe 89 / 76 / 43, view outside the window 127 / 110 / 84. The flat shading was far too bright (a sky-average ambient in a dark room); the lit one has the stock mirror's darkness and crisp detail. Adam: what is missing in the darker scene is the projector he stands in (step C, local lights). The outside and the globe are also darker than stock: not yet explained (candidates: local lights, the sun term on masked foliage, glTF metallic globe sampling dim probe radiance).

## Step C design notes (read 2026-10-13, nothing built)

The main pass draws local lights AFTER the soften pass (pipeline.cpp ~12580: boxes for point lights with `deferredLightF`, spot lights/projectors with `spotLightF` and a projected texture, `multiLight` for lights that surround the camera), each shading the G-buffer pixel. A traced hit has no G-buffer pixel, so the lights have to be sent to the soften pass as data: an array uniform of the strongest nearby lights (eye space centre, radius, falloff, colour; for spot lights the projector matrix, near plane, ambiance and the projected texture). Point light = `calcLegacyDistanceAttenuation(dist, falloff)` times colour (legacy: `dlit * nl * dist_atten * diffuse`; PBR: `intensity * 3.25 * ...` through `pbrPunctual`). The projector circle Adam stands in is a spot light with a projected texture: needs the texture and the matrix in the soften pass, shadows of local lights are not traced (spot shadow maps exist but are per screen pixel).

## Status 2026-10-13 (late): step C built and seen (hitq=2)

`hitq=2` adds the local lights to a traced hit: up to 8 point lights (arrays `rt_hit_light_pos/col`) and ONE projector (the soften pass gets the projector's uniforms and texture through `setupSpotLight`, so `getProjectedLightDiffuseColor` / `getProjectedLightAmbiance` of deferredUtil.glsl run on the hit), each with a traced shadow ray (`rtShadowRay`, `rtLightMargin`). Seen in the Larsson room: the projector circle appears on the floor of the mirror, the avatar in the mirror is lit by it and casts its shadow (floor mean 29.5 against 32.9 stock, avatar 46.9 against 50.7 stock; `hitq=1` had 26.4 and 31.5).

Three things found on the way (each cost a build): (1) a light sits inside a prim (lamp, projector box), so a shadow ray that stops 3 cm before its centre is blocked by the light's own prim: rays now stop `max(0.3 m, 10 %)` short; (2) `mNearbyLights` is sorted by distance MINUS radius, so every light whose radius covers the camera has distance 0 and their order is arbitrary: the 8 lights and the projector are now ranked by colour x the viewer's distance attenuation AT THE CAMERA (hits of a mirror are mostly close to the camera); (3) the log line `traced hit projector` shows the chosen projector's texture channel, size, colour and distance.

Debug views of the soften pass (`hitq` in rt-debug.txt): 0 flat, 1 lit (steps A+B), 2 lit + local lights, 3 only the lights (red point lights, green projector, blue = how far the projector's test got: 0.3 range, 0.6 cone, 1 facing), 4 the same without shadow rays, 5 lit + lights without shadow rays, 6 projector internals (red |light colour|, green proj_lod, blue projected colour). Not done: more than one projector (one sampler), lights chosen per hit instead of per camera (a mirror far from the camera sees other lights), local light cost not measured, the FSVkMirrorQuality preference, outside the window / the globe are dimmer than stock (68 against 82).

## Fix 2026-10-13 (night): the projector vanished from the side

The room has THREE projectors (over the avatar, and 7 m to each side, discs near the bed); the ranking that picked the one projector for the hits looked only at distance, so seen from one side a side projector won and its cone did not cover the mirror's floor (debug view 3: blue 0.3 = inside range, cone test failed). Now each candidate counts `attenuation x cone` at the camera AND at the avatar (a projector counts a tenth where its cone, axis = -z of the prim rotated, field of view = spot parameter 0, does not reach). Log line `traced hit light candidates` prints the best four with positions. Measured from the left side (`hitq=2` against the stock mirror): lit disc floor 48.4 / 47.5, far floor 21.2 / 19.9, avatar 53.9 / 51.5. Still one projector at a time (one sampler): a mirror that sees two discs lights only one.

## Legacy specular like the G-buffer pass (2026-10-13, late)

The traced hits used the specular map's ALPHA as glossiness (it is the environment intensity mask) and left out the material's specular colour: clothes looked like plastic. The G-buffer pass (class3/deferred/materialF.glsl) stores spec.rgb = specular map x specular colour and glossiness = specular exponent/255 x normal map alpha. Now the legacy rows carry the specular colour in the emissive slots and the exponent in `params.w` (llvkrt.cpp updateMaterials), rtShadeSurfaceEx reads both and the normal map's alpha. Seen: the pink top of the test avatar is satin like in the stock mirror.

Still different from the stock mirror on the test avatar: the hair is brown and dark where stock shows platinum blonde (a lit hair card with a flat triangle normal facing sideways gets almost no light from a projector overhead; stock lights it with smooth normals and its forward alpha lighting), the face is darker. Suspected cause: avatars have FLAT normals in the ray scene (the skinning pass writes positions only, `PLAN-rt-materials.md` "smooth normals" open item). Next: skin the normals too.

## Where the avatar in the traced mirror stands (2026-10-14, night session end)

Measured in the Larsson room (afternoon sky, which is CLASSIC MODE: legacy sky, `classic_mode > 0` in the soften pass: ambient = the sky's ambient colour, no probe irradiance; the legacy formula goes through srgb_to_linear; lights are added with 0.9): chest of the avatar in the mirror, traced `hitq=2` 66 against stock 75 (the real chest in the main view is 106, the stock mirror shows it at about 0.7). So the brightness is within about 12 %. Still different: the HAIR (brown against blond: blended hair is lit by the forward alpha pass, which lights it with the 8 hardware lights `calcPointLightOrSpotLight` and final_scale 1.1 in classic mode, not by the soften lighting my hit shading copies) and harder shadows on the face.

Tests made and what they proved: `hitq=10` (the eye ray shaded by the hit shading) matched the main view on walls exactly (140,141,151 both) and on skin within the projector double count (the main pass adds the lights on top of the replaced pixel), so the hit shading is the main shading; `view=12/13/14/15/16` (material debug views): the geometric normal of avatar hits matches the G-buffer (flat triangle normals are fine); the normal-map comparison is meaningless for blended parts (not in the G-buffer). Debug views in the soften pass (`hitq` in rt-debug.txt): 3 / 4 lights only (4 without shadow rays), 5 lit without light shadows, 6 projector internals, 7 material parameters, 8 irradiance, 9 sun term, 10 eye-ray shading, 11 hit albedo, 12 ambient x albedo, 13 flags (red glTF, green emissive mask, blue glossiness).

Next idea: light blended parts (hit materials flagged blend, RTM_BLEND) like the forward alpha pass: the hardware-light loop and the alpha pass scale. Also to try: a non-classic sky (probe ambient path) to see whether the classic mode is the special case.

## Hair of the avatar in the traced mirror: where it stands (2026-10-14, after midnight)

Symptom (Adam): the hair in the traced mirror is brown and dark, the stock mirror shows it blond; hair wisps over the face are missing; the chest is darker. Checked and ruled out: the hit albedo is blond (view `hitq=11`), ambient x albedo is bright (`hitq=12`), the hair is a plain legacy material (flags view `hitq=13`: black = not glTF, no emissive mask, no gloss, NOT blended), the geometric normals of avatar hits match the G-buffer, shadow rays of the lights do not matter (`hitq=5`), the eye-ray test (`hitq=10`) matches the main shading on opaque things (walls exactly). The shared environment of the Larsson sim is also CLASSIC MODE (log line `traced hit lights ... classic mode yes`).

Found: blended rigged draws (PASS_ALPHA_RIGGED) are in the rigged alpha GROUPS' draw maps (`group->mDrawMap[PASS_ALPHA_RIGGED]`, walked by `LLDrawPoolAlpha::renderAlpha`), not in the render maps `rt_name_rigged_main_view` read, so the blond hair layer was never named into the ray scene. Added (uncommitted before this note, untested): `add_entry` lambda + a walk over `gPipeline.beginRiggedAlphaGroups()` with kind KIND_ALPHA (hint 2 = blend), and `rtHitForwardLights` (forward alpha lighting: spot lights as plain cones, 0.9 / 1.1 classic scales; up to 8 lights now in `rt_hit_light_pos/col/dir`, spots flagged by dir.w). Result of the test run: hair still brown, flags view still black on the hair, so either no blended rigged draw reached the scene (rejected by `shadow_alpha_packet_ok`, `rt_textures_ready`, `!params->mAvatar`, `mGLTFMaterial`, or the groups list was empty at that time) or the blond layer is something else. NEXT: log, once per 600 frames, how many draws the rigged alpha groups hold for the own avatar and which condition rejects them; check `PASS_ALPHA_RIGGED` vs `LLDrawPoolAlpha` use of `mRiggedAlpha`; then look at where the blond mesh really is (Develop > Render Metadata > avatar, or hide the avatar's hair attachment and compare).

## 2026-10-14 (day): hair layer, hatching, smooth avatar normals

**Hair layer / sock tops missing (fixed, commit 8c9e0f0c).** Cause: the main cull keeps the avatars' alpha-BLENDED rigged draws in the render map `PASS_ALPHA_RIGGED` (pass numbers start at `NUM_POOL_TYPES` 23, so it is 77); `rt_name_rigged_main_view` read only `PASS_ALPHA`. Those draws were named by the shadow passes only, and the notes of those land one frame slot (2 tags) before the tag the scene call computes (`av_tag = frame + 1`; the frame number steps by 2 per viewer frame), so in `llvkrt.cpp` they were never "fresh" and never skinned (only fresh draws are skinned): "left out" in the avatar's structure. A snapshot named them from the main view once and they stayed in the structure with a stale pose (what looked like "stuck"). Now: the main view names `PASS_ALPHA_RIGGED` and both alpha group lists, and a draw named within the last 2 tags counts as fresh (its buffers are checked first: epoch, address, size). `avlog=1` in rt-debug.txt writes `<work>\rt-avatar-draws.log` (every draw of the own avatar: in the structure or left out, noted age); expect no `left out` kind 1 rows. The traced hit trace now also composites a blended layer over the opaque hit (`rtTraceHitEx`, `rtLitHitColor` in softenLightF.glsl, debug views `hitq=14..16`).

**"Hatching" / lower-poly look of the mirror avatar (fixed).** Not the mesh LOD (the ray scene uses the same draws, the body is the same 99k triangles), not shadow rays and not the normal map. At native resolution the lit avatar showed blocks and stair steps that followed triangles; they were there with the shadow rays off (`hitq=5`) and gone with the local lights off (`hitq=1`): in the dark Larsson room only the projector's N.L shows the normals. The avatar's records carried NO vertex normals (the skinning pass wrote positions only), so every hit used the FLAT normal of its triangle, and a grazing projector turns the small differences between neighbouring facets into blocks. Fix: the skinning compute pass (`kSkinShader`, `SkinJob` now 128 bytes) also skins the normals (the same joint matrices, world rotation, normalised) into the second half of the draw's output allocation (positions first, normals after, `skin_normals_ok`), and the avatar's record gets `mNormals` + `kRecNormals`.

**Soft shadows of local lights at hits (kept).** `rtShadowRaySoft` (no first-hit stop; each masked or blended candidate lets through what its blurred alpha leaves, `rtAlphaCover`, mip + 2, ramp around the cutoff), `rtShadowRayArea` (4 rays per light to a disc of `RT_LIGHT_SIZE` 0.5 m round the light, rotated per pixel by interleaved gradient noise that moves with `rt_noise_frame`), used for local lights and the sun at hits; the sun lightmap pass keeps the binary `rtShadowRay`. `glossyrt=28` (variant 9) = one ray per light.

**Cost found:** every shader change makes the first minutes after login slow: the lighting program's GPU pipelines build in the background and the draws are dropped, so the mirror (all modes, the stock probe too) stays BLACK until they are ready (about 2 to 4 minutes after a change, then cached in the pipeline cache). Keep ray query call sites few (each `rayQuery` loop is inlined per call site).

**Live variants (`glossyrt=20+N` in rt-debug.txt, `rt_hit_shadow_variant`):** 1 sun always lit, 2 shadow ray start 0.15 m, 3 up to 1024 candidates, 4 texture mip 0 at hits, 9 one shadow ray per light. Debug views `hitq` 3..18 (3/4 lights only, 5 lit without light shadows, 6 projector internals, 7 materials, 8 irradiance, 9 sun term, 10 eye-ray test, 11 albedo, 12 ambient x albedo, 13 flags, 14 blended layer marks, 15 layer albedo, 16 layer lit, 17 normals toward the viewer, 18 hit normal).

**Crash notes:** a debug loop over `beginRenderMap(type)` for type up to 120 crashed the viewer (the array has `NUM_RENDER_TYPES` entries); a second crash (CPU, in `buildScene`, a lookup in a Blas' `mAlpha` map with a null bucket array) came with the first version of the looser "fresh" rule and has not come back since the buffer check was added; `cdb.exe -z <dump> -y <Release dir> -c ".lines -e; .ecxr; kn 12; q"` reads dumps (`<user-profile>\AppData\Local\CrashDumps`).
