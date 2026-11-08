# Family 1: lighting resolve and sky (note written 2026-11-08, before any new shader)

Rule (Adam): the picture stays as it is; speed is the measure; only really cheap quality gains, shown to him first. Old shaders were READ for intent, fringe cases and settings; nothing is copied.

## 1. What the family is today (RT off, the baseline of the speed work), in draw order

| step | zone / ms (crowd lap gz9) | program | what it does |
|---|---|---|---|
| sky haze | 0.17 | skyV/F (WL sky dome) | draws the sky into the G-buffer: per-vertex haze colour + rainbow + halo22 (moisture/ice), `x2`, clamp 5, flag SKIP_ATMOS; HDRI sky (split screen) flag HAS_HDRI |
| sky clouds | 0.12 | cloudsV/F | the cloud layer into the G-buffer, same flag |
| (stars, sun disc, moon) | small | starsV/F, sunDiscV/F, moonV/F | |
| sun program | 0.31 | sunLightV/F (+ SSAO variant) | the lightmap: sun shadow from the 4 cascades (PCF with rotation), the 2 spot-projector shadows (only when such a light exists), SSAO, into RG (+B/A spot channels) of `deferredLight` |
| soften shadow | 0.29 | blurLightV/F, 2 passes | depth-aware separable blur of the lightmap (4 taps a side, `RenderShadowGaussian/BlurSize/BlurDistFactor`), skipped with DLSS + RT shadows |
| atmospherics (the resolve) | 0.23 | softenLightV/F (41 KB fragment) | per pixel: G-buffer + depth -> position; atmospheric scattering terms (`calcAtmosphericVarsLinear`); then ONE of: PBR (probe IBL + `pbrBaseLight`), HDRI sky (copy), WL sky (`srgb_to_linear * sky_hdr_scale`), legacy material (lambert ambient + sun x min(NdotL, shadow), specular lobe via the `lightFunc` lookup, gloss env + legacy env from probes, `classic_mode` tweaks); RT reflection branches (RT track, ignored here) |
| local lights | 0.03 | pointLight/spotLight V/F (+ pull variant) | additive light volumes (boxes), projector lights with texture + shadow, fullscreen lights when the camera is inside a volume; `RenderLocalLightCount` |
| haze (x2: sky-side and water-side) | 0.06 + | softenLightV + hazeF, waterHazeV/F | the remaining atmospheric haze over the post-deferred geometry (blend ONE / SRC_ALPHA), water patches, underwater tint, water exclusion mask |

Sum for this family in the crowd lap: about 1.2 ms of the 6.4 ms GPU frame (sky 0.29 + sun 0.31 + soften shadow 0.29 + resolve 0.23 + local lights/haze 0.1).

## 2. Keep list (must not change; each needs a test spot)

1. The plaza ground shine: sun and every local light show brightly in the specular lobe of shiny legacy and PBR surfaces (resolve: `lit * scol * sunlit * spec`, `applyGlossEnv`; lights: the `lightFunc` lookup in pointLightF/spotLightF). Test: the plaza at night and by day.
2. Sun / moon switch (`sun_up_factor`), the moon as the light at night, the sun below the horizon.
3. `classic_mode` (old Windlight sky look): sunlit x1.35, `pow(NdotL, 1.2)`, the sRGB round trips, final scale 1.1. Test: a classic sky preset.
4. Material branches: legacy (sRGB albedo and specular in the G-buffer, `envIntensity`, specular alpha = gloss), glTF PBR (ORM), HDRI sky, WL sky pixels; the `baseColor.a` override of the shadow (`scol = max(shadow, albedo.a)`) and the final `mix(color, albedo, albedo.a)` (the emissive-mask/fullbright alpha of legacy materials).
5. SSAO and the ambient occlusion on the irradiance (`adjustIrradiance`, `RenderSSAOIrradianceScale/Max`), and the soft look the blur gives the cascade shadows.
6. Cascade shadows: 4 cascades, PCF with the rotation noise, the spot-projector shadows, `RenderShadowDetail` 0..3.
7. Sky: gradient from the settings, rainbow and halo22 with moisture and ice levels, cloud layer (density, scattering, shadows on the sky), HDRI skies with the split screen, stars/sun/moon, `sky_hdr_scale`.
8. Atmospherics: distance haze and fog on everything but sky pixels, water plane haze above and below water, the water exclusion mask.
9. Reflection probes: IBL, parallax boxes, probe levels, legacy env, the probe CAPTURE mode (`gCubeSnapshot`, `light_scale`, local lights darkened when ambiance is above 1).
10. Local lights: point and spot volumes, projector textures and shadows, fullscreen case, the light falloff/size conventions (`getLightFalloff`, radius x1.5 in deferred).
11. Snapshots / impostors / HUD: HUD never goes through the resolve; the probe and snapshot variants of the sun program exist (`gDeferredSunProbeProgram`).

## 3. Settings that steer the family

`RenderDeferredSSAO`, `RenderShadowDetail`, `RenderDeferredAtmospheric`, `RenderLocalLightCount`, `RenderReflectionProbeLevel`, `RenderReflectionsEnabled`, `RenderScreenSpaceReflections` (post), `RenderShadowGaussian`, `RenderShadowBlurSize`, `RenderShadowBlurDistFactor`, `RenderSSAOIrradianceScale/Max`, `RenderShadowBlurWithDLSS`, the environment (sky settings: haze, density, moisture, ice, cloud, `classic_mode`, HDRI + `sky_hdr_scale`), the water plane. Test matrix per setting: on/off or each level where a lap spot shows it.

## 4. What the new shaders need as data (the per-frame block, built in slice 1)

camera (view, projection, inverse projection, jitter), sun and moon direction in view space + `sun_up_factor` + the clamped light norm, the sky/atmosphere terms (blue horizon/density, haze horizon/density, sunlight/moonlight/ambient colours, cloud/ice/moisture), the exposure scale, render size, frame index, the cascade matrices and splits, the spot shadow matrices, the handles of the shadow maps / lightmap / G-buffer / depth / probe cube arrays, the local light list.

## 5. Where the cost can go (candidates, not decisions; each is a small program or pass change with its own A/B)

1. **Sky pixels skip the G-buffer detour.** Today the sky and clouds are written into the G-buffer (albedo + flag, `x2` and clamps) and then re-read, converted (`srgb_to_linear * sky_hdr_scale`) and copied by the resolve. The sky is at the far plane: draw it straight into the lit target before the resolve, and let the resolve leave sky pixels alone (depth test / stencil, no read). Saves the G-buffer write bandwidth for the sky area and the resolve's fetches for those pixels. Same picture if the conversion is reproduced exactly.
2. **The lightmap blur (0.29 ms) as one compute pass** with shared memory (separable, depth-aware, same taps and weights) instead of two full-screen render passes, or folded into the resolve's lightmap read. Same weights = same picture; the render pass begin/end and the two target round trips go.
3. **The sun program (0.31 ms)**: the cascade selection, the PCF taps and the SSAO are one fragment shader with many branches; split the SSAO off at the resolution it needs (check against full resolution), keep the shadow lookups; separate small programs per `RenderShadowDetail` level instead of branches.
4. **The resolve (0.23 ms, 41 KB)**: split by material branch (PBR / legacy / sky) into small programs selected by the G-buffer flag through stencil or a classification pass, so each pixel runs only its branch and the program that runs is short. Remove the dead RT-hit code from the raster path (it belongs to the RT track).
5. **Haze (x2)**: fold the sky-side haze into the resolve for opaque pixels (the pass blends over the post-deferred geometry only); keep the water patches.
6. **Local lights**: already cheap (0.03 ms) in this lap; the clustered list is for the alpha pass (family 2) and for crowds of lights.

## 6. First native programs (slice 1 order)

(a) the per-frame data block and the pass-level compare; (b) the lightmap blur (self-contained, exact spec, 0.29 ms, A/B by name swap first: `Deferred Blur Light` keeps the old uniforms); (c) the sun lightmap program split per shadow level; (d) the sky into the lit target; (e) the resolve split. After each: lap (zones, old/new windows, the plaza), Adam's look at the keep-list spots.

## 7. Results log

**7.1 Lightmap blur (native `blur_light.frag`, 2026-11-08, lap nat5, RT off, crowd spot, three old/new window pairs):** zone "soften shadow" 0.293 ms old -> 0.250 ms new (-0.043 ms, -15%), every window pair the same (0.29/0.25). The new program is 517 + 2553 SPIR-V words, compiled in 51 ms (budget 20000 words / 150 ms). Changes against the old shader (same taps, weights, rules): sky pixels return at once (nothing reads the lightmap there: the lightmap is 1.0 and the resolve takes the sky from the G-buffer), the tap positions are rebuilt with the few multiplications the projection leaves instead of the full inverse-projection multiply and divide. Not changed: 13 taps, the plane-distance rule, the weights, the sampling nudge, the sun pass-through. Picture check: Adam's eye (pending), `imgdiff.py` is too noisy with moving avatars to judge by itself. The family key stays `native_blur=0` (old) until Adam accepts; to accept the manifest gets a default-on flag.
Honest size: the pass is fetch bound (3 + 12 depth + up to 12 lightmap fetches a pixel); the sky skip and the cheaper reconstruction took 15%. A bigger step needs fewer fetches per pixel (a compute pass that fetches a tile once into shared memory, same weights): that needs compute pipelines for native programs (not built yet; the post chain will want it too).
