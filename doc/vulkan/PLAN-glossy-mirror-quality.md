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
